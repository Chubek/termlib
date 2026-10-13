/** @file RedundantCastElimination.cpp @brief Identity and lossless cast cancellation. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_RedundantCastElimination(Function& fn, PassResult&) {
    for (auto& inst : fn.instr_storage()) {
        if (inst.operands().size() != 1) continue;
        auto op = inst.opcode();
        if (op != Opcode::Trunc && op != Opcode::ZExt && op != Opcode::SExt) continue;
        auto value = inst.operand(0);
        if (inst.type() == fn.value_type(value)) { copy(inst, value); continue; }
        auto* inner = definition(fn, value);
        if (!inner || inner->operands().size() != 1) continue;
        if (op == Opcode::Trunc && (inner->opcode() == Opcode::ZExt || inner->opcode() == Opcode::SExt) &&
            inst.type() == fn.value_type(inner->operand(0)) && bits(inner->type()) > bits(inst.type()))
            copy(inst, inner->operand(0));
    }
}
}
JITERATI_DEFINE_PASS(RedundantCastElimination, "redundant-cast-elimination", false)
