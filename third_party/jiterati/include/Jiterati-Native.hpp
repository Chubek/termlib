/** @file Jiterati-Native.hpp
 *  @brief Native ahead-of-time facilities: assemble, link, run.
 *
 *  Three complementary routes from Jiterati IR to native files:
 *
 *  1. JIT bytes: `compile_to_bytes(fn, backend)` copies machine code out of
 *     a `CompiledFunction` (exact length via `code_size()`), ready for
 *     `write_object()` / `write_executable()` through the ELF linker in
 *     Jiterati-Asm.hpp.
 *  2. Textual assembly: `assemble_text()` prefers Keystone when enabled
 *     (`JITERATI_USE_KEYSTONE`), otherwise translates Jiterati Intel-syntax
 *     assembly (`global`, bare registers) to GAS (`/dev/null`-checked `cc`)
 *     for host targets.
 *  3. Execution: `run_file()` spawns a produced executable and captures its
 *     output and exit status.
 *
 *  Header-only API surface; process spawning lives in src/Native.cpp.
 *  Every entry point degrades to a descriptive error when no toolchain is
 *  present, so callers never fail to build.
 */
#ifndef JITERATI_NATIVE_HPP_INCLUDED
#define JITERATI_NATIVE_HPP_INCLUDED

#include "Jiterati.hpp"
#include "Jiterati-Asm.hpp"
#include "Jiterati-BE.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace jiterati::native {

/** True when a `cc`-compatible driver answers on PATH. */
bool have_system_toolchain();

/** Host target id (`amd64`, `aarch64`, `rv64`, or `wasm` fallback). */
inline std::string host_target() {
#if defined(__x86_64__) || defined(_M_X64)
    return "amd64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__riscv) && __riscv_xlen == 64
    return "rv64";
#else
    return "wasm";
#endif
}

/** Translate Jiterati Intel-syntax assembly to GAS for `cc -c`. */
inline std::string to_gas(std::string_view assembly, std::string_view target) {
    std::string out;
    if (target == "amd64" || target == "x86_64") out += ".intel_syntax noprefix\n";
    out += ".text\n";
    std::string line;
    for (char c : assembly) {
        if (c == '\n') {
            std::string stripped = line;
            while (!stripped.empty() &&
                   (stripped.front() == ' ' || stripped.front() == '\t'))
                stripped.erase(stripped.begin());
            if (stripped.rfind("global ", 0) == 0)
                out += ".globl " + stripped.substr(7) + "\n";
            else if (stripped.rfind("extern ", 0) == 0)
                out += ".extern " + stripped.substr(7) + "\n";
            else if (!stripped.empty() && stripped.front() == ';')
                out += "#" + stripped.substr(1) + "\n";
            else
                out += line + "\n";
            line.clear();
        } else {
            line += c;
        }
    }
    if (!line.empty()) out += line + "\n";
    return out;
}

struct AssembleReport {
    bool ok = false;
    std::vector<std::uint8_t> object_bytes;
    std::string log;
};

/** Assemble `assembly` for `target` (implemented in src/Native.cpp). */
AssembleReport assemble_text(std::string_view target, std::string_view assembly);

/** Link object bytes into an executable image (ELF stub or system cc). */
std::vector<std::uint8_t> link_executable(std::string_view target,
                                          std::vector<std::uint8_t> const& object_or_text,
                                          bool object_is_elf = false);

/** Copy JIT machine code out of a compiled function. */
inline std::vector<std::uint8_t> compile_to_bytes(Function& fn, Backend& backend) {
    std::unique_ptr<CompiledFunction> compiled =
        JIT::create_default().compile(fn, backend);
    if (!compiled) return {};
    return compiled->code_bytes();
}

struct RunReport {
    int exit_code = -1;
    std::string output;
    std::string error;
};

/** Execute `path` with `args`, capturing combined output. */
RunReport run_file(std::string const& path, std::vector<std::string> const& args = {});

/** Write `bytes` to `path`, adding owner-execute permission when asked. */
void write_file(std::string const& path, std::vector<std::uint8_t> const& bytes,
                bool executable = false);

/** Extract the `.text` section from an ELF64 relocatable object. */
std::vector<std::uint8_t> extract_text_section(std::vector<std::uint8_t> const& elf);

} // namespace jiterati::native

#endif // JITERATI_NATIVE_HPP_INCLUDED
