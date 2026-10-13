/** @file DeadStoreElimination.cpp @brief Remove exact-address overwritten stores. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_DeadStoreElimination(Function& fn, PassResult&) {
    std::unordered_set<Instruction*> dead;
    for (auto& block : fn.blocks()) {
        Instruction* pending = nullptr;
        for (auto* inst : block->instructions()) {
            if (inst->opcode() == Opcode::Store && inst->operands().size() == 2) {
                if (pending && resolve_copy(fn, pending->operand(0)) == resolve_copy(fn, inst->operand(0)) &&
                    fn.value_type(pending->operand(1)) == fn.value_type(inst->operand(1))) dead.insert(pending);
                pending = inst;
            } else if (!nontrapping(fn, *inst)) pending = nullptr;
        }
    }
    erase(fn, dead);
}
}
JITERATI_DEFINE_PASS(DeadStoreElimination, "dead-store-elimination", false)
