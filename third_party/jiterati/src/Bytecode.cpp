/** @file src/Bytecode.cpp
 *  @brief `.jbc` file IO: save/load over the vendored zstd helper.
 */
#include "Jiterati-Bytecode.hpp"
#include "zstd/zstd.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace jiterati::bytecode {
namespace {

namespace fs = std::filesystem;

std::string read_file(std::string const& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("bytecode: cannot read " + path);
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

void write_file(std::string const& path, std::string const& data) {
    fs::path file(path);
    if (file.has_parent_path()) fs::create_directories(file.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("bytecode: cannot write " + path);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string json_field(std::string const& json, std::string const& key) {
    std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    auto end = json.find('"', pos);
    return end == std::string::npos ? std::string{} : json.substr(pos, end - pos);
}

} // namespace

void save(BytecodeModule const& image, std::string const& path) {
    std::string header = image.header_json();
    std::string payload = encode_records(image.functions);
    std::string raw = header + "\n" + payload;
    if (third_party::zstd::available()) {
        fs::path tmp = fs::temp_directory_path() / "jiterati-jbc-write.tmp";
        fs::path tmp_z = fs::temp_directory_path() / "jiterati-jbc-write.tmp.zst";
        write_file(tmp.string(), raw);
        third_party::zstd::compress(tmp, tmp_z);
        std::string compressed = read_file(tmp_z.string());
        fs::remove(tmp);
        fs::remove(tmp_z);
        write_file(path, std::string(kMagic) + compressed);
    } else {
        write_file(path, std::string(kMagic) + raw);
    }
}

BytecodeModule load(std::string const& path) {
    std::string blob = read_file(path);
    // Transparent bundle support: skip the shell runtime stub.
    std::string marker(kBundleMarker);
    auto mark = blob.find(marker);
    if (mark != std::string::npos) blob = blob.substr(mark + marker.size());
    if (blob.rfind(kMagic, 0) != 0)
        throw std::runtime_error("bytecode: bad magic in " + path);
    std::string body = blob.substr(sizeof(kMagic) - 1);
    if (third_party::zstd::available()) {
        try {
            fs::path tmp_z = fs::temp_directory_path() / "jiterati-jbc-read.tmp.zst";
            fs::path tmp = fs::temp_directory_path() / "jiterati-jbc-read.tmp";
            write_file(tmp_z.string(), body);
            third_party::zstd::decompress(tmp_z, tmp);
            body = read_file(tmp.string());
            fs::remove(tmp_z);
            fs::remove(tmp);
        } catch (...) {
            // Fall back to treating the body as raw records.
        }
    }
    auto newline = body.find('\n');
    if (newline == std::string::npos)
        throw std::runtime_error("bytecode: missing header in " + path);
    BytecodeModule image;
    std::string header = body.substr(0, newline);
    image.producer = json_field(header, "producer");
    image.version = json_field(header, "version");
    image.target = json_field(header, "target");
    image.entry = json_field(header, "entry");
    image.functions = decode_records(body.substr(newline + 1));
    return image;
}

std::string describe(BytecodeModule const& image) {
    std::ostringstream out;
    out << "bytecode " << image.producer << " " << image.version
        << " target=" << image.target << " entry=" << image.entry
        << " functions=" << image.functions.size() << "\n";
    for (auto const& fn : image.functions)
        out << "  " << fn.name << " ir_bytes=" << fn.ir_text.size()
            << " code_bytes=" << fn.code.size() << "\n";
    return out.str();
}

void bundle_file(std::string const& jbc_path, std::string const& out_path) {
    std::string payload = read_file(jbc_path);
    std::string marker(kBundleMarker);
    auto mark = payload.find(marker);
    std::string image_bytes =
        mark == std::string::npos ? payload : payload.substr(mark + marker.size());
    // Re-load to recover entry/target for the stub header.
    BytecodeModule image = load(jbc_path);
    std::string stub = "#!/bin/sh\n# jiterati bundle entry=" + image.entry +
                       " target=" + image.target + "\n"
                       "# runtime stub: executes via `jiterati-cli run \"$0\"`\n"
                       "if command -v jiterati-cli >/dev/null 2>&1; then\n"
                       "  exec jiterati-cli run \"$0\" \"$@\"\n"
                       "else\n"
                       "  exec Jiterati-CLI run \"$0\" \"$@\"\n"
                       "fi\n";
    write_file(out_path, stub + marker.substr(1) + image_bytes);
#if !defined(_WIN32)
    ::chmod(out_path.c_str(), 0755);
#endif
}

void bundle(BytecodeModule const& image, std::string const& path) {
    fs::path tmp = fs::temp_directory_path() / "jiterati-bundle-tmp.jbc";
    save(image, tmp.string());
    bundle_file(tmp.string(), path);
    std::error_code ec;
    fs::remove(tmp, ec);
}

bool is_bundle(std::string const& path) {
    std::string blob = read_file(path);
    if (blob.rfind("#!", 0) == 0) return true;
    return blob.find(kBundleMarker) != std::string::npos;
}

std::int64_t execute(BytecodeModule const& image, std::string const& entry,
                     std::vector<std::int64_t> args) {
    BytecodeFunction const* record = nullptr;
    for (auto const& fn : image.functions) {
        if (fn.name == entry) {
            record = &fn;
            break;
        }
    }
    if (!record) throw std::runtime_error("bytecode: unknown entry " + entry);
    if (record->code.empty())
        throw std::runtime_error("bytecode: entry '" + entry +
                                 "' carries no executable payload");
    if (args.size() != 0 && args.size() != 2)
        throw std::runtime_error("bytecode: execute supports 0 or 2 i32 args");
    std::size_t size = record->code.size();
#if defined(_WIN32)
    void* memory = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE,
                                PAGE_EXECUTE_READWRITE);
    if (!memory) throw std::runtime_error("bytecode: VirtualAlloc failed");
    std::memcpy(memory, record->code.data(), size);
    std::int64_t result = 0;
    if (args.empty()) {
        using Fn = int (*)();
        result = reinterpret_cast<Fn>(memory)();
    } else {
        using Fn = int (*)(int, int);
        result = reinterpret_cast<Fn>(memory)(static_cast<int>(args[0]),
                                              static_cast<int>(args[1]));
    }
    VirtualFree(memory, 0, MEM_RELEASE);
    return result;
#else
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    std::size_t alloc = ((size + static_cast<std::size_t>(page) - 1) /
                         static_cast<std::size_t>(page)) *
                        static_cast<std::size_t>(page);
    void* memory = mmap(nullptr, alloc, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) throw std::runtime_error("bytecode: mmap failed");
    std::memcpy(memory, record->code.data(), size);
    std::int64_t result = 0;
    if (args.empty()) {
        using Fn = int (*)();
        result = reinterpret_cast<Fn>(memory)();
    } else {
        using Fn = int (*)(int, int);
        result = reinterpret_cast<Fn>(memory)(static_cast<int>(args[0]),
                                              static_cast<int>(args[1]));
    }
    munmap(memory, alloc);
    return result;
#endif
}

} // namespace jiterati::bytecode
