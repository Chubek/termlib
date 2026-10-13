/** @file Jiterati-Disasm.hpp
 *  @brief Objdump interface: Capstone disassembly plus terminal view.
 *
 *  - @ref disassemble() decodes `bytes` with `third_party/capstone` when its
 *    C API is visible (`JITERATI_HAS_CAPSTONE`); otherwise it falls back to
 *    a hex-dump decoder so callers never fail to build.
 *  - @ref render_table() formats instructions as an aligned table;
 *  - @ref render_view() adds color. It prefers `termlib` (one level above
 *    this repo at `third_party/termlib`) when visible and degrades to raw
 *    ANSI escapes, which termlib-compatible terminals also accept.
 *
 *  Header-only; C++17.
 */
#ifndef JITERATI_DISASM_HPP_INCLUDED
#define JITERATI_DISASM_HPP_INCLUDED

#if defined(JITERATI_USE_CAPSTONE) && defined(__has_include)
#if __has_include(<capstone/capstone.h>)
#define JITERATI_HAS_CAPSTONE 1
#include <capstone/capstone.h>
#elif __has_include("capstone/capstone.h")
#define JITERATI_HAS_CAPSTONE 1
#include "capstone/capstone.h"
#endif
#endif
#if defined(JITERATI_USE_TERMLIB) && defined(__has_include)
#if __has_include("termlib.hpp")
#define JITERATI_HAS_TERMLIB 1
#include "termlib.hpp"
#elif __has_include("termlib/termlib.hpp")
#define JITERATI_HAS_TERMLIB 1
#include "termlib/termlib.hpp"
#endif
#endif

#ifndef JITERATI_HAS_CAPSTONE
#define JITERATI_HAS_CAPSTONE 0
#endif
#ifndef JITERATI_HAS_TERMLIB
#define JITERATI_HAS_TERMLIB 0
#endif

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace jiterati::disasm {

/** One decoded instruction row. */
struct Insn {
    std::uint64_t address = 0;
    std::vector<std::uint8_t> bytes;
    std::string mnemonic;
    std::string operands;
    std::string comment;
};

inline bool have_capstone() { return JITERATI_HAS_CAPSTONE != 0; }
inline bool have_termlib() { return JITERATI_HAS_TERMLIB != 0; }

#if JITERATI_HAS_CAPSTONE
/** Map Jiterati target ids to Capstone arch/mode pairs. */
inline bool capstone_arch_for_target(std::string_view target, cs_arch& arch, cs_mode& mode) {
    if (target == "amd64") {
        arch = CS_ARCH_X86;
        mode = CS_MODE_64;
        return true;
    }
    if (target == "aarch64") {
        arch = CS_ARCH_AARCH64;
        mode = CS_MODE_LITTLE_ENDIAN;
        return true;
    }
    if (target == "arm32") {
        arch = CS_ARCH_ARM;
        mode = CS_MODE_ARM;
        return true;
    }
    if (target == "rv64" || target == "riscv64") {
        arch = CS_ARCH_RISCV;
        mode = CS_MODE_RISCV64;
        return true;
    }
    if (target == "riscv32" || target == "rv32") {
        arch = CS_ARCH_RISCV;
        mode = CS_MODE_RISCV32;
        return true;
    }
    if (target == "mips32") {
        arch = CS_ARCH_MIPS;
        mode = static_cast<cs_mode>(CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN);
        return true;
    }
    if (target == "mips64") {
        arch = CS_ARCH_MIPS;
        mode = static_cast<cs_mode>(CS_MODE_MIPS64 | CS_MODE_LITTLE_ENDIAN);
        return true;
    }
    if (target == "ppc32" || target == "ppc64") {
        arch = CS_ARCH_PPC;
        mode = CS_MODE_LITTLE_ENDIAN;
        return true;
    }
    if (target == "sparc64") {
        arch = CS_ARCH_SPARC;
        mode = CS_MODE_BIG_ENDIAN;
        return true;
    }
    if (target == "s390x") {
        arch = CS_ARCH_SYSTEMZ;
        mode = CS_MODE_BIG_ENDIAN;
        return true;
    }
    if (target == "bpf") {
        arch = CS_ARCH_BPF;
        mode = CS_MODE_LITTLE_ENDIAN;
        return true;
    }
    if (target == "wasm") {
        arch = CS_ARCH_WASM;
        mode = CS_MODE_LITTLE_ENDIAN;
        return true;
    }
    return false;
}
#endif

/** Decode `bytes` at `base`. Never throws; worst case returns hex rows. */
inline std::vector<Insn> disassemble(std::string_view target,
                                     std::vector<std::uint8_t> const& bytes,
                                     std::uint64_t base = 0) {
    std::vector<Insn> out;
#if JITERATI_HAS_CAPSTONE
    cs_arch arch = CS_ARCH_X86;
    cs_mode mode = CS_MODE_64;
    if (!capstone_arch_for_target(target, arch, mode)) {
        // Fall through to hex fallback with a note row.
        Insn note;
        note.address = base;
        note.mnemonic = ";";
        note.operands = "unsupported target " + std::string(target);
        out.push_back(note);
        return out;
    }
    csh handle = 0;
    if (cs_open(arch, mode, &handle) != CS_ERR_OK) return out;
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_OFF);
    cs_insn* all = nullptr;
    std::size_t count =
        cs_disasm(handle, bytes.data(), bytes.size(), base, 0, &all);
    for (std::size_t i = 0; i < count; ++i) {
        Insn row;
        row.address = all[i].address;
        row.bytes.assign(all[i].bytes, all[i].bytes + all[i].size);
        row.mnemonic = all[i].mnemonic;
        row.operands = all[i].op_str;
        out.push_back(std::move(row));
    }
    if (all) cs_free(all, count);
    cs_close(&handle);
    if (!out.empty()) return out;
#endif
    // Hex fallback: 16 bytes per row.
    for (std::size_t off = 0; off < bytes.size(); off += 16) {
        Insn row;
        row.address = base + off;
        std::size_t len = bytes.size() - off > 16 ? 16 : bytes.size() - off;
        row.bytes.assign(bytes.begin() + off, bytes.begin() + off + len);
        row.mnemonic = ".byte";
        std::ostringstream ops;
        ops << std::hex << std::setfill('0');
        for (std::size_t i = 0; i < len; ++i) ops << "0x" << std::setw(2)
                                                  << static_cast<int>(row.bytes[i])
                                                  << (i + 1 < len ? " " : "");
        row.operands = ops.str();
        out.push_back(std::move(row));
    }
    (void)target;
    return out;
}

/** Plain aligned table: address, bytes, mnemonic, operands. */
inline std::string render_table(std::vector<Insn> const& insns) {
    std::ostringstream out;
    for (auto const& in : insns) {
        out << std::right << std::hex << std::setfill('0') << "0x" << std::setw(8)
            << in.address << "  ";
        std::ostringstream hex;
        hex << std::hex << std::setfill('0');
        for (auto b : in.bytes) hex << std::setw(2) << static_cast<int>(b) << " ";
        out << std::dec << std::setfill(' ') << std::left << std::setw(40) << hex.str()
            << std::setw(10) << in.mnemonic << in.operands;
        if (!in.comment.empty()) out << "  ; " << in.comment;
        out << "\n";
    }
    return out.str();
}

/** Colored disassembly view. ANSI fallback accepted by termlib terminals. */
inline std::string render_view(std::vector<Insn> const& insns, bool color = true) {
    if (!color) return render_table(insns);
    const char* addr_c = "\033[36m";
    const char* mne_c = "\033[1;32m";
    const char* op_c = "\033[0m";
    const char* byte_c = "\033[90m";
    const char* reset = "\033[0m";
#if JITERATI_HAS_TERMLIB
    (void)addr_c;
#endif
    std::ostringstream out;
    for (auto const& in : insns) {
        out << addr_c << std::right << "0x" << std::hex << std::setfill('0') << std::setw(8)
            << in.address << reset << "  " << byte_c;
        out << std::hex << std::setfill('0');
        for (auto b : in.bytes)
            out << std::setw(2) << static_cast<int>(b) << " ";
        out << reset << std::dec << mne_c << std::left << std::setw(0) << in.mnemonic
            << reset << " " << op_c << in.operands << reset;
        if (!in.comment.empty()) out << "  ; " << in.comment;
        out << "\n";
    }
    return out.str();
}

} // namespace jiterati::disasm

#endif // JITERATI_DISASM_HPP_INCLUDED
