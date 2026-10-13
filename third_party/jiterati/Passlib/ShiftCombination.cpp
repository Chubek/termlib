/** @file ShiftCombination.cpp @brief Combine legal same-direction shifts. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_ShiftCombination(Function& fn, PassResult&) {
    for (auto* inst : snapshot(fn)) {
        auto op = inst->opcode();
        if (!inst->type().is_int() || inst->operands().size() != 2 ||
            (op != Opcode::Shl && op != Opcode::LShr && op != Opcode::AShr)) continue;
        auto* inner = definition(fn, inst->operand(0));
        auto a = integer(fn, inst->operand(1));
        if (!a || !inner || inner->opcode() != op || inner->type() != inst->type() || inner->operands().size() != 2) continue;
        auto b = integer(fn, inner->operand(1));
        if (b && *a < bits(inst->type()) && *b < bits(inst->type()) && *a + *b < bits(inst->type()))
            rewrite(*inst, op, {inner->operand(0), literal(fn, fn.value_type(inst->operand(1)), *a + *b)});
    }
}
}
JITERATI_DEFINE_PASS(ShiftCombination, "shift-combination", false)
