/** @file LoopInvariantCodeMotion.cpp @brief Speculation-safe natural-loop invariant hoisting. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_LoopInvariantCodeMotion(Function& fn, PassResult&) {
    auto dom = dominators(fn);
    for (auto* latch : reachable(fn)) for (auto* header : latch->successors()) {
        if (!dom[latch].count(header)) continue;
        std::unordered_set<Block*> loop{header};
        std::vector<Block*> work;
        if (latch != header) { loop.insert(latch); work.push_back(latch); }
        while (!work.empty()) {
            auto* block = work.back(); work.pop_back();
            for (auto* pred : block->predecessors()) if (loop.insert(pred).second && pred != header) work.push_back(pred);
        }
        Block* preheader = nullptr; bool multiple = false;
        for (auto* pred : header->predecessors()) if (!loop.count(pred)) {
            if (preheader) multiple = true; preheader = pred;
        }
        if (!preheader || multiple || preheader->successors() != std::vector<Block*>{header} ||
            !terminator(preheader) || terminator(preheader)->opcode() != Opcode::Br) continue;
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto* inst : snapshot(fn)) {
                if (!loop.count(inst->parent()) || !expression(fn, *inst)) continue;
                bool invariant = true;
                for_uses(*inst, [&](Value& value) {
                    auto* def = definition(fn, resolve_copy(fn, value));
                    if (def && def->parent() && (loop.count(def->parent()) || !dom[preheader].count(def->parent()))) invariant = false;
                });
                if (!invariant) continue;
                for_uses(*inst, [&](Value& value) { value = resolve_copy(fn, value); });
                auto& list = inst->parent()->instructions(); list.erase(std::find(list.begin(), list.end(), inst));
                auto& destination = preheader->instructions(); destination.insert(destination.end()-1, inst); inst->set_parent(preheader);
                changed = true;
            }
        }
    }
}
}
JITERATI_DEFINE_PASS(LoopInvariantCodeMotion, "loop-invariant-code-motion", false)
