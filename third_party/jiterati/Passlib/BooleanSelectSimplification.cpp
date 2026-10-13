/** @file BooleanSelectSimplification.cpp @brief Boolean select truth-table reduction. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_BooleanSelectSimplification(Function& fn, PassResult&) {
    for (auto& inst : fn.instr_storage()) {
        if (inst.opcode() != Opcode::Select || inst.type() != Type::i1() || inst.operands().size() != 3 ||
            fn.value_type(inst.operand(0)) != Type::i1()) continue;
        auto a = integer(fn, inst.operand(1)), b = integer(fn, inst.operand(2));
        if (!a || !b || *a == *b) continue;
        if (*a) copy(inst, inst.operand(0)); else rewrite(inst, Opcode::Not, {inst.operand(0)});
    }
}
}
JITERATI_DEFINE_PASS(BooleanSelectSimplification, "boolean-select-simplification", false)
