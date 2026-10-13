/** @file LoopFusion.cpp @brief Static fusion of independent adjacent counted loops. */
#include "LoopSupport.hpp"
namespace jiterati::pass_detail {
void run_LoopFusion(Function& fn, PassResult&) {
    for (;;) {
        std::optional<CountedDomain> first, second;
        for (auto& block : fn.blocks()) {
            auto a = simple_loop(fn, block.get(), true); if (!a) continue;
            auto b = counted_domain(fn, a->exit, a->exit);
            if (!b || b->preheader != a->header || b->iterations.size() != a->iterations.size()) continue;
            bool independent = true;
            for (auto* inst : b->header->instructions()) {
                if (inst != b->phi && inst != terminator(b->header) && !nontrapping(fn, *inst)) independent = false;
                for_uses(*inst, [&](Value& value) {
                    auto* def = definition(fn, resolve_copy(fn, value));
                    if (def && def->parent() == a->header) independent = false;
                });
            }
            if (!independent || (a->header->instructions().size() + b->header->instructions().size()) * a->iterations.size() > 128) continue;
            first = a; second = b; break;
        }
        if (!first) break;
        std::unordered_map<int32_t, Value> values;
        for (std::size_t i = 0; i < first->iterations.size(); ++i) {
            append_before_branch(first->preheader, emit_iteration(fn, *first, first->iterations[i], values));
            append_before_branch(first->preheader, emit_iteration(fn, *second, second->iterations[i], values));
        }
        finish_expansion(fn, first->preheader, second->exit, {first->header, second->header}, second->header, values);
    }
}
}
JITERATI_DEFINE_PASS(LoopFusion, "loop-fusion", false)
