/** @file NegationPropagation.cpp @brief Add/sub of integer negated values. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_NegationPropagation(Function& fn, PassResult&) {
    for (auto& inst : fn.instr_storage()) {
        if (!inst.type().is_int() || inst.operands().size() != 2 ||
            (inst.opcode() != Opcode::Add && inst.opcode() != Opcode::Sub)) continue;
        auto* inner = definition(fn, inst.operand(1));
        if (inner && inner->opcode() == Opcode::Neg && inner->type() == inst.type() && inner->operands().size() == 1)
            rewrite(inst, inst.opcode() == Opcode::Add ? Opcode::Sub : Opcode::Add, {inst.operand(0), inner->operand(0)});
    }
}
}
JITERATI_DEFINE_PASS(NegationPropagation, "negation-propagation", false)
