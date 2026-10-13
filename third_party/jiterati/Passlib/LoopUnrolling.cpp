/** @file LoopUnrolling.cpp @brief Bounded complete static counted-loop expansion. */
#include "LoopSupport.hpp"
namespace jiterati::pass_detail {
void run_LoopUnrolling(Function& fn, PassResult&) {
    for (;;) {
        std::optional<CountedDomain> chosen;
        for (auto& block : fn.blocks()) if ((chosen = simple_loop(fn, block.get()))) break;
        if (!chosen) break;
        auto domain = *chosen; std::unordered_map<int32_t, Value> values;
        for (auto iteration : domain.iterations) append_before_branch(domain.preheader, emit_iteration(fn, domain, iteration, values));
        finish_expansion(fn, domain.preheader, domain.exit, {domain.header}, domain.header, values);
    }
}
}
JITERATI_DEFINE_PASS(LoopUnrolling, "loop-unrolling", false)
