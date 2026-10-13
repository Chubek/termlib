/** @file AlgebraicSimplification.cpp @brief Integer identity elimination. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_AlgebraicSimplification(Function& fn, PassResult&) {
    for (auto* inst : snapshot(fn)) {
        if (!inst->type().is_int() || inst->operands().size() != 2) continue;
        auto a = inst->operand(0), b = inst->operand(1);
        bool az = equals(fn, a, 0), bz = equals(fn, b, 0);
        switch (inst->opcode()) {
            case Opcode::Add: case Opcode::Or: case Opcode::Xor:
                if (bz) copy(*inst, a); else if (az) copy(*inst, b); break;
            case Opcode::Sub:
                if (a == b) copy(*inst, literal(fn, inst->type(), 0));
                else if (bz) copy(*inst, a); break;
            case Opcode::Mul:
                if (az || bz) copy(*inst, literal(fn, inst->type(), 0));
                else if (equals(fn, b, 1)) copy(*inst, a);
                else if (equals(fn, a, 1)) copy(*inst, b); break;
            case Opcode::And:
                if (az || bz) copy(*inst, literal(fn, inst->type(), 0));
                else if (equals(fn, b, mask(inst->type()))) copy(*inst, a);
                else if (equals(fn, a, mask(inst->type()))) copy(*inst, b); break;
            case Opcode::UDiv:
                if (equals(fn, b, 1)) copy(*inst, a); break;
            case Opcode::SDiv:
                // In i1 the bit pattern 1 represents signed -1, not +1.
                if (bits(inst->type()) > 1 && equals(fn, b, 1)) copy(*inst, a); break;
            default: break;
        }
    }
}
}
JITERATI_DEFINE_PASS(AlgebraicSimplification, "algebraic-simplification", false)
