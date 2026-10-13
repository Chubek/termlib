#ifndef JITERATI_BE_AMD64_BACKEND_HPP_INCLUDED
#define JITERATI_BE_AMD64_BACKEND_HPP_INCLUDED

#include "../../include/Jiterati-BE.hpp"

#include <cstddef>
#include <string>

namespace jiterati::be {

/// Concrete AMD64 (System V / x86-64) backend.
///
/// This backend owns the AMD64 executable-compilation surface and returns a
/// `CompiledFunction` backed by executable memory for the IR shapes implemented
/// in `BE/AMD64/Backend.cpp`.  It is the path exercised by the JIT entry point
/// `JIT::compile(fn, backend)`.
///
/// The textual/debug emitter can describe a practical subset of the IR intended
/// for integer arithmetic, comparisons, control flow, memory access and
/// intra-module calls:
///
///   - integer add / sub / mul / sdiv / udiv / srem / urem
///   - bitwise and / or / xor / shl / lshr / ashr / neg / not
///   - icmp (all signed/unsigned predicates) producing i1
///   - copy, const materialisation, select (cmov)
///   - load / store / alloca (stack-slot based)
///   - br / condbr (block labels)
///   - ret / call (intra-module, resolved when the callee is co-compiled)
///
/// The executable `compile_function` path currently recognizes selected i32
/// binary operations on two parameters: add/sub/mul/sdiv/srem and signed integer
/// comparisons.  Broader lowering must extend `BE/AMD64/Backend.cpp` before
/// callers may claim general native execution support.
class AMD64Backend : public Backend {
public:
    AMD64Backend() = default;

    std::string target_id() const override { return "amd64"; }

    std::unique_ptr<CompiledFunction> compile_function(Function& fn) override;

    std::string emit_assembly(Function& fn) override;

    std::unique_ptr<AssemblerContext> create_assembler_context() override;
    ABI& abi() override;
    RegAllocator& allocator() override;

private:
    // Internal textual-assembly emitter (used by emit_assembly() and as a
    // debugging aid).  Defined in Backend.cpp.
    std::string disassemble(Function& fn);
};

} // namespace jiterati::be

#endif // JITERATI_BE_AMD64_BACKEND_HPP_INCLUDED
