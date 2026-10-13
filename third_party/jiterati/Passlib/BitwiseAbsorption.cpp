/** @file BitwiseAbsorption.cpp @brief Integer lattice absorption laws. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_BitwiseAbsorption(Function& fn, PassResult&) {
    for (auto& inst : fn.instr_storage()) {
        if (!inst.type().is_int() || inst.operands().size() != 2) continue;
        if (inst.opcode() != Opcode::And && inst.opcode() != Opcode::Or) continue;
        auto opposite = inst.opcode() == Opcode::And ? Opcode::Or : Opcode::And;
        for (unsigned i = 0; i < 2; ++i) {
            auto value = inst.operand(i);
            auto* inner = definition(fn, inst.operand(1-i));
            if (inner && inner->opcode() == opposite && inner->type() == inst.type() && inner->operands().size() == 2 &&
                (inner->operand(0) == value || inner->operand(1) == value)) { copy(inst, value); break; }
        }
    }
}
}
JITERATI_DEFINE_PASS(BitwiseAbsorption, "bitwise-absorption", false)
