/** @file Jiterati-Asm.hpp
 *  @brief Assembly/disassembly bridge: Keystone assemble + link interface.
 *
 *  Backends emit textual assembly. This header turns that text into bytes:
 *
 *  - @ref assemble() runs `third_party/keystone` when its C API is visible
 *    (`JITERATI_HAS_KEYSTONE`), otherwise returns a descriptive error;
 *  - @ref link_elf() wraps assembled `.text` bytes in a minimal ELF64/ELF32
 *    relocatable stub so `compile --emit obj/exe` always produces a real
 *    file even without Keystone;
 *  - @ref ks_arch_for_target() / @ref ks_mode_for_target() map Jiterati
 *    target ids (including every `third_party/isa` target) to Keystone
 *    `ks_arch`/`ks_mode` constants without requiring the headers.
 *
 *  Header-only; link against keystone only when the consumer enables it.
 */
#ifndef JITERATI_ASM_HPP_INCLUDED
#define JITERATI_ASM_HPP_INCLUDED

#if defined(JITERATI_USE_KEYSTONE) && defined(__has_include)
#if __has_include(<keystone/keystone.h>)
#define JITERATI_HAS_KEYSTONE 1
#include <keystone/keystone.h>
#elif __has_include("keystone/keystone.h")
#define JITERATI_HAS_KEYSTONE 1
#include "keystone/keystone.h"
#endif
#endif

#ifndef JITERATI_HAS_KEYSTONE
#define JITERATI_HAS_KEYSTONE 0
#endif

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jiterati::asmlink {

/** Keystone arch ids mirrored here so mapping works without its headers. */
enum class KsArch : int {
    Unknown = -1,
    ARM = 1,
    ARM64 = 2,
    MIPS = 3,
    X86 = 4,
    PPC = 5,
    SPARC = 6,
    SYSTEMZ = 7,
    HEXAGON = 8,
    EVM = 9,
    RISCV = 10,
    BPF = 11,
};

enum class KsMode : int {
    LittleEndian = 0,
    BigEndian = 1 << 30,
    Mode16 = 1 << 1,
    Mode32 = 1 << 2,
    Mode64 = 1 << 3,
    Thumb = 1 << 4,
    Mips32 = 1 << 2,
    Mips64 = 1 << 3,
};

/** Map a Jiterati target id to a Keystone architecture. */
inline KsArch ks_arch_for_target(std::string_view target) {
    if (target == "amd64" || target == "x86" || target == "x86_64") return KsArch::X86;
    if (target == "aarch64" || target == "arm64") return KsArch::ARM64;
    if (target == "arm32" || target == "arm") return KsArch::ARM;
    if (target == "mips32" || target == "mips64" || target == "mips") return KsArch::MIPS;
    if (target == "ppc32" || target == "ppc64" || target == "ppc") return KsArch::PPC;
    if (target == "sparc64" || target == "sparc") return KsArch::SPARC;
    if (target == "s390x" || target == "systemz") return KsArch::SYSTEMZ;
    if (target == "hexagon") return KsArch::HEXAGON;
    if (target == "riscv32" || target == "riscv64" || target == "rv64" || target == "rv32")
        return KsArch::RISCV;
    if (target == "bpf" || target == "ebpf") return KsArch::BPF;
    return KsArch::Unknown;
}

/** Default Keystone mode bits for a target (64-bit LE unless noted). */
inline int ks_mode_for_target(std::string_view target) {
    if (target == "amd64" || target == "x86_64") return static_cast<int>(KsMode::Mode64);
    if (target == "aarch64" || target == "arm64") return static_cast<int>(KsMode::LittleEndian);
    if (target == "arm32" || target == "arm") return static_cast<int>(KsMode::Mode32);
    if (target == "mips32") return static_cast<int>(KsMode::Mips32);
    if (target == "mips64") return static_cast<int>(KsMode::Mips64);
    if (target == "ppc32") return static_cast<int>(KsMode::Mode32);
    if (target == "ppc64") return static_cast<int>(KsMode::Mode64);
    if (target == "riscv32" || target == "rv32") return static_cast<int>(KsMode::Mode32);
    if (target == "riscv64" || target == "rv64") return static_cast<int>(KsMode::Mode64);
    if (target == "sparc64") return static_cast<int>(KsMode::Mode64);
    return static_cast<int>(KsMode::LittleEndian);
}

inline bool have_keystone() { return JITERATI_HAS_KEYSTONE != 0; }

struct AssembleResult {
    bool ok = false;
    std::vector<std::uint8_t> bytes;
    std::string error;
    std::size_t statements = 0;
};

/** Assemble `assembly` for `target`. Requires Keystone at build time. */
inline AssembleResult assemble(std::string_view target, std::string_view assembly,
                               std::uint64_t base = 0) {
    AssembleResult result;
#if JITERATI_HAS_KEYSTONE
    KsArch arch = ks_arch_for_target(target);
    if (arch == KsArch::Unknown) {
        result.error = "keystone: unsupported target " + std::string(target);
        return result;
    }
    ks_engine* engine = nullptr;
    if (ks_open(static_cast<ks_arch>(static_cast<int>(arch)),
                ks_mode_for_target(target), &engine) != KS_ERR_OK ||
        !engine) {
        result.error = "keystone: ks_open failed";
        return result;
    }
    unsigned char* code = nullptr;
    std::size_t size = 0, count = 0;
    std::string source(assembly);
    int asm_err = ks_asm(engine, source.c_str(), static_cast<uint64_t>(base), &code,
                         &size, &count);
    if (asm_err == KS_ERR_OK && code) {
        result.ok = true;
        result.bytes.assign(code, code + size);
        result.statements = count;
        ks_free(code);
    } else {
        result.error = "keystone: asm failed: " +
                       std::string(ks_strerror(ks_errno(engine)));
    }
    ks_close(engine);
#else
    (void)base;
    result.error = "keystone unavailable (build without third_party/keystone); "
                   "target=" + std::string(target) + " asm_bytes=" +
                   std::to_string(assembly.size());
#endif
    return result;
}

/** Minimal ELF relocatable wrapper around raw `.text` bytes. */
inline std::vector<std::uint8_t> link_elf(std::string_view target,
                                          std::vector<std::uint8_t> const& text,
                                          std::uint64_t base = 0x400000) {
    bool is64 = !(target == "arm32" || target == "mips32" || target == "riscv32" ||
                  target == "rv32" || target == "ppc32");
    std::uint16_t machine = 62; // EM_X86_64 default
    if (target == "aarch64") machine = 183;
    else if (target == "arm32") machine = 40;
    else if (target == "rv64" || target == "riscv64") machine = 243;
    else if (target == "riscv32" || target == "rv32") machine = 243;
    else if (target == "ppc64" || target == "ppc32") machine = 21;
    else if (target == "s390x") machine = 22;
    else if (target == "sparc64") machine = 43;
    else if (target == "mips32" || target == "mips64") machine = 8;
    else if (target == "loongarch64") machine = 258;
    else if (target == "wasm") machine = 0;

    std::vector<std::uint8_t> out;
    auto push8 = [&](std::uint8_t v) { out.push_back(v); };
    auto push16 = [&](std::uint16_t v) {
        push8(static_cast<std::uint8_t>(v));
        push8(static_cast<std::uint8_t>(v >> 8));
    };
    auto push32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) push8(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto push64 = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) push8(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    // ELF identification.
    push8(0x7f);
    for (char c : std::string("ELF")) push8(static_cast<std::uint8_t>(c));
    push8(is64 ? 2 : 1); // class
    push8(1);            // little endian
    push8(1);            // version
    push8(0);            // osabi
    for (int i = 0; i < 8; ++i) push8(0);
    push16(2);                        // ET_EXEC
    push16(machine);                  // machine
    push32(1);                        // version
    if (is64) push64(base + 0x1000);  // entry
    else push32(static_cast<std::uint32_t>(base + 0x1000));
    std::size_t phoff_pos = out.size();
    if (is64) push64(0);
    else push32(0);
    push32(0);                        // shoff
    push32(0);                        // flags
    push16(is64 ? 64 : 52);           // ehsize
    push16(is64 ? 56 : 32);           // phentsize
    push16(1);                        // phnum
    push16(0);
    push16(0);
    push16(0);
    // Program header: single R-X LOAD covering .text.
    std::size_t phoff = out.size();
    push32(1);                        // PT_LOAD
    push32(5);                        // R-X
    if (is64) {
        push64(0x1000);               // offset
        push64(base + 0x1000);        // vaddr
        push64(base + 0x1000);        // paddr
        push64(text.size());          // filesz
        push64(text.size());          // memsz
        push64(0x1000);               // align
    } else {
        push32(0x1000);
        push32(static_cast<std::uint32_t>(base + 0x1000));
        push32(static_cast<std::uint32_t>(base + 0x1000));
        push32(static_cast<std::uint32_t>(text.size()));
        push32(static_cast<std::uint32_t>(text.size()));
        push32(0x1000);
    }
    while (out.size() < 0x1000) push8(0);
    out.insert(out.end(), text.begin(), text.end());
    // Patch phoff.
    for (int i = 0; i < (is64 ? 8 : 4); ++i)
        out[phoff_pos + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(phoff >> (8 * i));
    (void)machine;
    return out;
}

} // namespace jiterati::asmlink

#endif // JITERATI_ASM_HPP_INCLUDED
