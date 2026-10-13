/** @file BitwiseIdempotence.cpp @brief Fold identical bitwise operands. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_BitwiseIdempotence(Function& fn, PassResult&) {
    for (auto* inst : snapshot(fn)) {
        if (!inst->type().is_int() || inst->operands().size() != 2 || inst->operand(0) != inst->operand(1)) continue;
        if (inst->opcode() == Opcode::And || inst->opcode() == Opcode::Or) copy(*inst, inst->operand(0));
        else if (inst->opcode() == Opcode::Xor) copy(*inst, literal(fn, inst->type(), 0));
    }
}
}
JITERATI_DEFINE_PASS(BitwiseIdempotence, "bitwise-idempotence", false)
