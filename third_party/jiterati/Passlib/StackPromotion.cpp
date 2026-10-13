/** @file StackPromotion.cpp @brief Promote initialized block-local stack slots. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_StackPromotion(Function& fn, PassResult&) {
    PassResult escape; run_EscapeAnalysis(fn, escape);
    std::unordered_set<Instruction*> dead;
    for (auto* allocation : snapshot(fn)) {
        if (allocation->opcode() != Opcode::Alloca || !allocation->parent() || escape.escaping_allocas.count(allocation->id())) continue;
        auto pointer = Value::vreg(allocation->id()); auto* owner = allocation->parent();
        bool legal = true, initialized = false;
        Type stored_type;
        for (auto& block : fn.blocks()) for (auto* inst : block->instructions()) {
            if ((inst->opcode() != Opcode::Load && inst->opcode() != Opcode::Store) || inst->operands().empty() ||
                resolve_copy(fn, inst->operand(0)) != pointer) continue;
            if (block.get() != owner) { legal = false; continue; }
            if (inst->opcode() == Opcode::Store && inst->operands().size() == 2) {
                auto type = fn.value_type(inst->operand(1));
                if (initialized && type != stored_type) legal = false;
                stored_type = type; initialized = true;
            } else if (!initialized || inst->type() != stored_type) legal = false;
        }
        if (!legal || !initialized) continue;
        Value current;
        for (auto* inst : owner->instructions()) {
            if ((inst->opcode() != Opcode::Load && inst->opcode() != Opcode::Store) || inst->operands().empty() ||
                resolve_copy(fn, inst->operand(0)) != pointer) continue;
            if (inst->opcode() == Opcode::Store) { current = inst->operand(1); dead.insert(inst); }
            else copy(*inst, current);
        }
        // Copy aliases are retained until ordinary DCE removes them.
    }
    erase(fn, dead);
}
}
JITERATI_DEFINE_PASS(StackPromotion, "stack-promotion", false)
