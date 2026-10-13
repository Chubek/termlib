/** @file src/Native.cpp
 *  @brief Process-level native facilities: system assembler, runner, ELF reader.
 */
#include "Jiterati-Native.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace jiterati::native {
namespace {

namespace fs = std::filesystem;

std::string read_bytes(std::string const& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

void write_bytes(std::string const& path, std::string const& data) {
    fs::path file(path);
    if (file.has_parent_path()) fs::create_directories(file.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string shell_quote(std::string const& text) {
    std::string out = "'";
    for (char c : text) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

unsigned counter() {
    static unsigned n = 0;
    return ++n;
}

} // namespace

bool have_system_toolchain() {
#if defined(_WIN32)
    return std::system("where cc >NUL 2>&1") == 0;
#else
    return std::system("command -v cc >/dev/null 2>&1") == 0;
#endif
}

AssembleReport assemble_text(std::string_view target, std::string_view assembly) {
    AssembleReport report;
    // Route 1: Keystone when the build enabled it.
    if (asmlink::have_keystone()) {
        asmlink::AssembleResult ks = asmlink::assemble(target, assembly);
        report.ok = ks.ok;
        report.object_bytes = std::move(ks.bytes);
        report.log = ks.error;
        return report;
    }
    // Route 2: system cc for host targets.
    if (have_system_toolchain() && target == host_target() &&
        (target == "amd64" || target == "aarch64" || target == "rv64")) {
        std::string stem =
            (fs::temp_directory_path() / ("jiterati-asm-" + std::to_string(counter()))).string();
        write_bytes(stem + ".s", to_gas(assembly, target));
        std::string log_path = stem + ".log";
        std::string command = "cc -c -o " + shell_quote(stem + ".o") + " " +
                              shell_quote(stem + ".s") + " >" + shell_quote(log_path) +
                              " 2>&1";
        int rc = std::system(command.c_str());
        report.log = read_bytes(log_path);
        if (rc == 0) {
            std::string object = read_bytes(stem + ".o");
            if (!object.empty() &&
                object.size() > 4 && object[0] == '\x7f' && object[1] == 'E') {
                report.ok = true;
                report.object_bytes.assign(object.begin(), object.end());
            } else {
                report.log += "\nassembler produced no ELF object";
            }
        } else {
            report.log = "cc -c failed (exit " + std::to_string(rc) + "):\n" + report.log;
        }
        std::error_code ec;
        fs::remove(stem + ".s", ec);
        fs::remove(stem + ".o", ec);
        fs::remove(log_path, ec);
        return report;
    }
    report.log = "no assembler for target " + std::string(target) +
                 " (keystone disabled, no host cc route)";
    return report;
}

std::vector<std::uint8_t> link_executable(std::string_view target,
                                          std::vector<std::uint8_t> const& object_or_text,
                                          bool object_is_elf) {
    std::vector<std::uint8_t> text = object_or_text;
    if (object_is_elf) {
        auto section = extract_text_section(object_or_text);
        if (!section.empty()) text = std::move(section);
    }
    return asmlink::link_elf(target, text);
}

RunReport run_file(std::string const& path, std::vector<std::string> const& args) {
    RunReport report;
#if defined(_WIN32)
    std::string command = shell_quote(path);
    for (auto const& arg : args) command += " " + shell_quote(arg);
    command += " >NUL 2>&1";
    report.exit_code = std::system(command.c_str());
#else
    std::string out_path =
        (fs::temp_directory_path() / ("jiterati-run-" + std::to_string(counter()) + ".log")).string();
    std::string command = shell_quote(path);
    for (auto const& arg : args) command += " " + shell_quote(arg);
    command += " >" + shell_quote(out_path) + " 2>&1";
    int rc = std::system(command.c_str());
    if (WIFEXITED(rc)) report.exit_code = WEXITSTATUS(rc);
    else report.exit_code = 128;
    report.output = read_bytes(out_path);
    std::error_code ec;
    fs::remove(out_path, ec);
#endif
    return report;
}

void write_file(std::string const& path, std::vector<std::uint8_t> const& bytes,
                bool executable) {
    fs::path file(path);
    if (file.has_parent_path()) fs::create_directories(file.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("native: cannot write " + path);
    output.write(reinterpret_cast<char const*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
#if !defined(_WIN32)
    if (executable) {
        ::chmod(path.c_str(), 0755);
    }
#endif
}

std::vector<std::uint8_t> extract_text_section(std::vector<std::uint8_t> const& elf) {
    std::vector<std::uint8_t> empty;
    if (elf.size() < 64 || elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' ||
        elf[3] != 'F' || elf[4] != 2)
        return empty; // ELF64 only.
    auto u16 = [&](std::size_t off) -> std::uint16_t {
        if (off + 2 > elf.size()) return 0;
        return static_cast<std::uint16_t>(elf[off] | (elf[off + 1] << 8));
    };
    auto u32 = [&](std::size_t off) -> std::uint32_t {
        if (off + 4 > elf.size()) return 0;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(elf[off + i]) << (8 * i);
        return v;
    };
    auto u64 = [&](std::size_t off) -> std::uint64_t {
        if (off + 8 > elf.size()) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(elf[off + i]) << (8 * i);
        return v;
    };
    std::uint64_t shoff = u64(40);
    std::uint16_t shentsize = u16(58);
    std::uint16_t shnum = u16(60);
    std::uint16_t shstrndx = u16(62);
    if (shoff == 0 || shentsize < 64 || shnum == 0 || shnum > 256) return empty;
    std::uint64_t strtab = shoff + static_cast<std::uint64_t>(shstrndx) * shentsize;
    std::uint64_t str_off = u64(strtab + 24);
    std::uint64_t str_size = u64(strtab + 32);
    auto name = [&](std::uint32_t off) -> std::string {
        std::string out;
        while (off < str_size && str_off + off < elf.size() && elf[str_off + off] != 0) {
            out += static_cast<char>(elf[str_off + off]);
            ++off;
        }
        return out;
    };
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint64_t entry = shoff + static_cast<std::uint64_t>(i) * shentsize;
        if (name(u32(entry)) == ".text") {
            std::uint64_t off = u64(entry + 24);
            std::uint64_t size = u64(entry + 32);
            if (size == 0 || off + size > elf.size() || size > (1u << 26)) return empty;
            return std::vector<std::uint8_t>(elf.begin() + off, elf.begin() + off + size);
        }
    }
    return empty;
}

} // namespace jiterati::native
