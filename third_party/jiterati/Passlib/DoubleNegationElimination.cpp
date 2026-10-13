/** @file DoubleNegationElimination.cpp @brief Cancel paired integer negations. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_DoubleNegationElimination(Function& fn, PassResult&) {
    for (auto& inst : fn.instr_storage()) {
        if (inst.opcode() != Opcode::Neg || !inst.type().is_int() || inst.operands().size() != 1) continue;
        auto* inner = definition(fn, inst.operand(0));
        if (inner && inner->opcode() == Opcode::Neg && inner->type() == inst.type() && inner->operands().size() == 1)
            copy(inst, inner->operand(0));
    }
}
}
JITERATI_DEFINE_PASS(DoubleNegationElimination, "double-negation-elimination", false)
