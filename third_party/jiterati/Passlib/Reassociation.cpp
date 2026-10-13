/** @file Reassociation.cpp @brief Constant regrouping under modular arithmetic. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_Reassociation(Function& fn, PassResult&) {
    for (auto* inst : snapshot(fn)) {
        auto op = inst->opcode();
        if (!inst->type().is_int() || inst->operands().size() != 2 ||
            (op != Opcode::Add && op != Opcode::Mul && op != Opcode::And && op != Opcode::Or && op != Opcode::Xor)) continue;
        auto c = integer(fn, inst->operand(1));
        auto* inner = definition(fn, inst->operand(0));
        if (!c || !inner || inner->opcode() != op || inner->type() != inst->type() || inner->operands().size() != 2) continue;
        auto d = integer(fn, inner->operand(1));
        if (!d) continue;
        uint64_t combined = 0;
        switch (op) {
            case Opcode::Add: combined = *d + *c; break;
            case Opcode::Mul: combined = *d * *c; break;
            case Opcode::And: combined = *d & *c; break;
            case Opcode::Or: combined = *d | *c; break;
            case Opcode::Xor: combined = *d ^ *c; break;
            default: break;
        }
        rewrite(*inst, op, {inner->operand(0), literal(fn, inst->type(), combined)});
    }
}
}
JITERATI_DEFINE_PASS(Reassociation, "reassociation", false)
