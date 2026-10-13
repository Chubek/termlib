/** @file DeadCodeRemoval.cpp @brief Iterative unused-expression deletion. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_DeadCodeRemoval(Function& fn, PassResult&) {
    for (;;) {
        std::unordered_set<uint32_t> used;
        for (auto& inst : fn.instr_storage()) for_uses(inst, [&](Value& value) { if (value.is_vreg()) used.insert(value.vreg_index()); });
        std::unordered_set<Instruction*> dead;
        for (auto& inst : fn.instr_storage())
            if (nontrapping(fn, inst) && !inst.type().is_void() && !used.count(inst.id())) dead.insert(&inst);
        if (dead.empty()) break;
        erase(fn, dead);
    }
}
}
JITERATI_DEFINE_PASS(DeadCodeRemoval, "dead-code-removal", false)
