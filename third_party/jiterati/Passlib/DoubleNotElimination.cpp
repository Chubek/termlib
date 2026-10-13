/** @file DoubleNotElimination.cpp @brief Cancel paired bitwise complements. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_DoubleNotElimination(Function& fn, PassResult&) {
    for (auto& inst : fn.instr_storage()) {
        if (inst.opcode() != Opcode::Not || !inst.type().is_int() || inst.operands().size() != 1) continue;
        auto* inner = definition(fn, inst.operand(0));
        if (inner && inner->opcode() == Opcode::Not && inner->type() == inst.type() && inner->operands().size() == 1)
            copy(inst, inner->operand(0));
    }
}
}
JITERATI_DEFINE_PASS(DoubleNotElimination, "double-not-elimination", false)
