/** @file InstructionScheduling.cpp @brief Critical-height scheduling of pure runs. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_InstructionScheduling(Function& fn, PassResult&) {
    for (auto& block : fn.blocks()) {
        auto& list = block->instructions();
        for (std::size_t begin = 0; begin < list.size();) {
            if (!expression(fn, *list[begin])) { ++begin; continue; }
            std::size_t end = begin;
            while (end < list.size() && expression(fn, *list[end])) ++end;
            std::unordered_map<uint32_t, unsigned> height;
            for (std::size_t i = end; i-- > begin;) {
                unsigned own = height[list[i]->id()] + 1;
                for_uses(*list[i], [&](Value& v) { if (v.is_vreg()) height[v.vreg_index()] = std::max(height[v.vreg_index()], own); });
            }
            std::vector<Instruction*> remaining(list.begin()+begin, list.begin()+end), scheduled;
            std::unordered_set<uint32_t> pending;
            for (auto* inst : remaining) pending.insert(inst->id());
            while (!remaining.empty()) {
                auto best = remaining.end();
                for (auto it = remaining.begin(); it != remaining.end(); ++it) {
                    bool ready = true;
                    for_uses(**it, [&](Value& v) { if (v.is_vreg() && pending.count(v.vreg_index())) ready = false; });
                    if (ready && (best == remaining.end() || height[(*it)->id()] > height[(*best)->id()])) best = it;
                }
                if (best == remaining.end()) { scheduled.insert(scheduled.end(), remaining.begin(), remaining.end()); break; }
                pending.erase((*best)->id()); scheduled.push_back(*best); remaining.erase(best);
            }
            std::copy(scheduled.begin(), scheduled.end(), list.begin()+begin); begin = end;
        }
    }
}
}
JITERATI_DEFINE_PASS(InstructionScheduling, "instruction-scheduling", false)
