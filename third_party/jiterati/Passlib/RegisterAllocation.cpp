/** @file RegisterAllocation.cpp @brief Deterministic target-independent interference coloring. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_RegisterAllocation(Function& fn, PassResult& result) {
    run_LivenessAnalysis(fn, result);
    std::map<uint32_t, std::set<uint32_t>> graph;
    auto connect = [&](std::unordered_set<uint32_t> const& live) {
        for (auto a : live) for (auto b : live) if (a != b) { graph[a].insert(b); graph[b].insert(a); }
    };
    for (auto& block : fn.blocks()) {
        auto live = result.live_out[block.get()]; connect(live);
        for (auto it = block->instructions().rbegin(); it != block->instructions().rend(); ++it) {
            auto* inst = *it;
            if (!inst->type().is_void()) {
                graph[inst->id()];
                for (auto value : live) if (value != inst->id()) { graph[value].insert(inst->id()); graph[inst->id()].insert(value); }
                live.erase(inst->id());
            }
            if (inst->opcode() != Opcode::Phi) for_uses(*inst, [&](Value& value) { if (value.is_vreg()) live.insert(value.vreg_index()); });
            connect(live);
        }
        connect(result.live_in[block.get()]);
    }
    for (uint32_t i = 0; i < fn.param_count(); ++i) graph[i];
    for (auto& inst : fn.instr_storage()) if (!inst.type().is_void()) graph[inst.id()];
    std::vector<uint32_t> order;
    for (auto const& entry : graph) order.push_back(entry.first);
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return graph[a].size() > graph[b].size(); });
    for (auto value : order) {
        std::set<int> occupied;
        for (auto neighbor : graph[value]) {
            auto it = result.registers.find(neighbor);
            if (it != result.registers.end() && it->second >= 0) occupied.insert(it->second);
        }
        int color = 0; while (color < 8 && occupied.count(color)) ++color;
        result.registers[value] = color < 8 ? color : -1;
    }
}
}
JITERATI_DEFINE_PASS(RegisterAllocation, "register-allocation", true)
