/** @file BooleanComparisonSimplification.cpp @brief Boolean equality reduction. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_BooleanComparisonSimplification(Function& fn, PassResult&) {
    for (auto& inst : fn.instr_storage()) {
        if ((inst.opcode() != Opcode::ICmpEq && inst.opcode() != Opcode::ICmpNe) || inst.operands().size() != 2) continue;
        for (unsigned i = 0; i < 2; ++i) {
            auto value = inst.operand(i);
            auto constant = integer(fn, inst.operand(1-i));
            if (fn.value_type(value) != Type::i1() || !constant || fn.value_type(inst.operand(1-i)) != Type::i1()) continue;
            bool identity = (inst.opcode() == Opcode::ICmpEq) == (*constant != 0);
            if (identity) copy(inst, value); else rewrite(inst, Opcode::Not, {value});
            break;
        }
    }
}
}
JITERATI_DEFINE_PASS(BooleanComparisonSimplification, "boolean-comparison-simplification", false)
