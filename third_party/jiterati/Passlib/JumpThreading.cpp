/** @file JumpThreading.cpp @brief Bypass phi-free branch-only trampolines. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_JumpThreading(Function& fn, PassResult&) {
    for (auto& block : fn.blocks()) {
        auto edges = block->successors(); bool changed = false;
        for (auto*& edge : edges) {
            std::unordered_set<Block*> seen{block.get()};
            while (seen.insert(edge).second && edge->instructions().size() == 1 &&
                   edge->instructions()[0]->opcode() == Opcode::Br && edge->successors().size() == 1 &&
                   !has_phi(edge->successors()[0])) {
                edge = edge->successors()[0]; changed = true;
            }
        }
        if (changed) block->set_successors(std::move(edges));
    }
}
}
JITERATI_DEFINE_PASS(JumpThreading, "jump-threading", false)
