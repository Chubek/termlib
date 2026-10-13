/** @file LoopPeeling.cpp @brief First-iteration peeling of static counted loops. */
#include "LoopSupport.hpp"
namespace jiterati::pass_detail {
void run_LoopPeeling(Function& fn, PassResult& result) {
    std::vector<Block*> blocks;
    for (auto& block : fn.blocks()) blocks.push_back(block.get());
    for (auto* block : blocks) {
        auto domain = simple_loop(fn, block); if (!domain) continue;
        if (domain->iterations.size() == 1) { run_LoopUnrolling(fn, result); return; }
        std::unordered_map<int32_t, Value> values;
        append_before_branch(domain->preheader, emit_iteration(fn, *domain, domain->iterations.front(), values));
        for (std::size_t i = 0; i < domain->phi->incoming_blocks().size(); ++i)
            if (domain->phi->incoming_blocks()[i] == domain->preheader)
                domain->phi->set_operand(2*i+1, literal(fn, domain->phi->type(), domain->iterations[1]));
    }
}
}
JITERATI_DEFINE_PASS(LoopPeeling, "loop-peeling", false)
