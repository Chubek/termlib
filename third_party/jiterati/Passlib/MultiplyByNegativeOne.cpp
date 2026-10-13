/** @file MultiplyByNegativeOne.cpp @brief Modular multiply by minus one. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_MultiplyByNegativeOne(Function& fn, PassResult&) {
    for (auto& inst : fn.instr_storage()) {
        if (inst.opcode() != Opcode::Mul || !inst.type().is_int() || inst.operands().size() != 2) continue;
        if (equals(fn, inst.operand(1), mask(inst.type()))) rewrite(inst, Opcode::Neg, {inst.operand(0)});
        else if (equals(fn, inst.operand(0), mask(inst.type()))) rewrite(inst, Opcode::Neg, {inst.operand(1)});
    }
}
}
JITERATI_DEFINE_PASS(MultiplyByNegativeOne, "multiply-by-negative-one", false)
