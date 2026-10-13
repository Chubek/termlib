/** @file BitwiseComplementFolding.cpp @brief Fold bitwise complements. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_BitwiseComplementFolding(Function& fn, PassResult&) {
    for (auto* inst : snapshot(fn)) {
        if (!inst->type().is_int() || inst->operands().size() != 2) continue;
        auto op = inst->opcode();
        if (op != Opcode::And && op != Opcode::Or && op != Opcode::Xor) continue;
        for (unsigned i = 0; i < 2; ++i) {
            auto* inner = definition(fn, inst->operand(i));
            if (inner && inner->opcode() == Opcode::Not && inner->type() == inst->type() && inner->operands().size() == 1 &&
                inner->operand(0) == inst->operand(1-i)) {
                copy(*inst, literal(fn, inst->type(), op == Opcode::And ? 0 : mask(inst->type()))); break;
            }
        }
    }
}
}
JITERATI_DEFINE_PASS(BitwiseComplementFolding, "bitwise-complement-folding", false)
