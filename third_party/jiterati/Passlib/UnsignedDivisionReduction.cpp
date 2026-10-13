/** @file UnsignedDivisionReduction.cpp @brief Unsigned power-of-two division. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_UnsignedDivisionReduction(Function& fn, PassResult&) {
    for (auto* inst : snapshot(fn)) {
        if (inst->opcode() != Opcode::UDiv || !inst->type().is_int() || inst->operands().size() != 2) continue;
        auto divisor = integer(fn, inst->operand(1));
        if (!divisor || !*divisor || (*divisor & (*divisor-1))) continue;
        unsigned shift = 0;
        for (auto n = *divisor; n > 1; n >>= 1) ++shift;
        if (!shift) copy(*inst, inst->operand(0));
        else rewrite(*inst, Opcode::LShr, {inst->operand(0), literal(fn, inst->type(), shift)});
    }
}
}
JITERATI_DEFINE_PASS(UnsignedDivisionReduction, "unsigned-division-reduction", false)
