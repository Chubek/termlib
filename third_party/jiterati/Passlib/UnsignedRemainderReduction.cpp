/** @file UnsignedRemainderReduction.cpp @brief Unsigned power-of-two modulo. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_UnsignedRemainderReduction(Function& fn, PassResult&) {
    for (auto* inst : snapshot(fn)) {
        if (inst->opcode() != Opcode::URem || !inst->type().is_int() || inst->operands().size() != 2) continue;
        auto divisor = integer(fn, inst->operand(1));
        if (divisor && *divisor && !(*divisor & (*divisor-1)))
            rewrite(*inst, Opcode::And, {inst->operand(0), literal(fn, inst->type(), *divisor-1)});
    }
}
}
JITERATI_DEFINE_PASS(UnsignedRemainderReduction, "unsigned-remainder-reduction", false)
