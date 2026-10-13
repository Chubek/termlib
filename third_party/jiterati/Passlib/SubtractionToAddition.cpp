/** @file SubtractionToAddition.cpp @brief Modular subtraction canonicalization. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_SubtractionToAddition(Function& fn, PassResult&) {
    for (auto* inst : snapshot(fn)) {
        if (inst->opcode() != Opcode::Sub || !inst->type().is_int() || inst->operands().size() != 2) continue;
        auto constant = integer(fn, inst->operand(1));
        if (constant) rewrite(*inst, Opcode::Add, {inst->operand(0), literal(fn, inst->type(), uint64_t(0) - *constant)});
    }
}
}
JITERATI_DEFINE_PASS(SubtractionToAddition, "subtraction-to-addition", false)
