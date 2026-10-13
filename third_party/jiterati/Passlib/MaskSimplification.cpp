/** @file MaskSimplification.cpp @brief Combine nested constant AND/OR masks. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_MaskSimplification(Function& fn, PassResult&) {
    for (auto* inst : snapshot(fn)) {
        if (!inst->type().is_int() || inst->operands().size() != 2 ||
            (inst->opcode() != Opcode::And && inst->opcode() != Opcode::Or)) continue;
        for (unsigned i = 0; i < 2; ++i) {
            auto c = integer(fn, inst->operand(i)); auto* inner = definition(fn, inst->operand(1-i));
            if (!c || !inner || inner->opcode() != inst->opcode() || inner->type() != inst->type() || inner->operands().size() != 2) continue;
            for (unsigned j = 0; j < 2; ++j) {
                auto d = integer(fn, inner->operand(j)); if (!d) continue;
                auto combined = inst->opcode() == Opcode::And ? *c & *d : *c | *d;
                rewrite(*inst, inst->opcode(), {inner->operand(1-j), literal(fn, inst->type(), combined)}); break;
            }
            break;
        }
    }
}
}
JITERATI_DEFINE_PASS(MaskSimplification, "mask-simplification", false)
