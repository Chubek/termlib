/** @file ShiftSimplification.cpp @brief Proven in-range shift identities. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_ShiftSimplification(Function& fn, PassResult&) {
    for (auto* inst : snapshot(fn)) {
        auto op = inst->opcode();
        if (!inst->type().is_int() || inst->operands().size() != 2 ||
            (op != Opcode::Shl && op != Opcode::LShr && op != Opcode::AShr)) continue;
        auto amount = integer(fn, inst->operand(1));
        if (!amount || *amount >= bits(inst->type())) continue;
        if (!*amount) copy(*inst, inst->operand(0));
        else if (equals(fn, inst->operand(0), 0)) copy(*inst, literal(fn, inst->type(), 0));
    }
}
}
JITERATI_DEFINE_PASS(ShiftSimplification, "shift-simplification", false)
