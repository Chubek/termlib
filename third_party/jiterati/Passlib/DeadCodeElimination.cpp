/** @file DeadCodeElimination.cpp @brief Mark/sweep dead SSA expression graphs. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_DeadCodeElimination(Function& fn, PassResult&) {
    std::unordered_set<Instruction*> live;
    std::function<void(Instruction*)> mark = [&](Instruction* inst) {
        if (!inst || !live.insert(inst).second) return;
        for_uses(*inst, [&](Value& value) { mark(definition(fn, value)); });
    };
    for (auto& inst : fn.instr_storage()) if (!nontrapping(fn, inst) || inst.type().is_void()) mark(&inst);
    std::unordered_set<Instruction*> dead;
    for (auto& inst : fn.instr_storage()) if (!live.count(&inst)) dead.insert(&inst);
    erase(fn, dead);
}
}
JITERATI_DEFINE_PASS(DeadCodeElimination, "dead-code-elimination", false)
