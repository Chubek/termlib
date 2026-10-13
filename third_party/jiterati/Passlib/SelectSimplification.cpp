/** @file SelectSimplification.cpp @brief Constant and equal-arm selects. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_SelectSimplification(Function& fn, PassResult&) {
    for (auto* inst : snapshot(fn)) {
        if (inst->opcode() != Opcode::Select || inst->operands().size() != 3) continue;
        auto condition = integer(fn, inst->operand(0));
        if (condition) copy(*inst, inst->operand(*condition ? 1 : 2));
        else if (resolve_copy(fn, inst->operand(1)) == resolve_copy(fn, inst->operand(2))) copy(*inst, inst->operand(1));
    }
}
}
JITERATI_DEFINE_PASS(SelectSimplification, "select-simplification", false)
