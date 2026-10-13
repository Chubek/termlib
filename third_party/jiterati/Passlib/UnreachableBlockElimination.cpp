/** @file UnreachableBlockElimination.cpp @brief Prune unreachable regions and phi edges. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_UnreachableBlockElimination(Function& fn, PassResult&) {
    auto reached = reachable(fn);
    std::unordered_set<Block*> live(reached.begin(), reached.end()), dead;
    for (auto& block : fn.blocks()) if (!live.count(block.get())) dead.insert(block.get());
    remove_blocks(fn, dead);
}
}
JITERATI_DEFINE_PASS(UnreachableBlockElimination, "unreachable-block-elimination", false)
