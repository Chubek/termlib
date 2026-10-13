/** @file CFGAnalysis.cpp @brief Deterministic reachable CFG postorder. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_CFGAnalysis(Function& fn, PassResult& result) {
    std::unordered_set<Block*> seen;
    std::function<void(Block*)> visit = [&](Block* block) {
        if (!block || !seen.insert(block).second) return;
        for (auto* successor : block->successors()) visit(successor);
        result.postorder.push_back(block);
    };
    visit(fn.entry_block());
}
}
JITERATI_DEFINE_PASS(CFGAnalysis, "cfg-analysis", true)
