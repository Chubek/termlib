/** @file StrengthReduction.cpp @brief Width-aware power-of-two multiplication. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_StrengthReduction(Function& fn, PassResult& result) {
    run_AlgebraicSimplification(fn, result);
    for (auto* inst : snapshot(fn)) {
        if (inst->opcode() != Opcode::Mul || !inst->type().is_int() || inst->operands().size() != 2) continue;
        for (unsigned i = 0; i < 2; ++i) {
            auto value = integer(fn, inst->operand(i));
            if (!value || !*value || (*value & (*value-1))) continue;
            unsigned shift = 0;
            for (auto n = *value; n > 1; n >>= 1) ++shift;
            rewrite(*inst, Opcode::Shl, {inst->operand(1-i), literal(fn, inst->type(), shift)}); break;
        }
    }
}
}
JITERATI_DEFINE_PASS(StrengthReduction, "strength-reduction", false)
