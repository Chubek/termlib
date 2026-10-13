/** @file LivenessAnalysis.cpp @brief Edge-sensitive SSA liveness fixed point. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_LivenessAnalysis(Function& fn, PassResult& result) {
    for (auto& block : fn.blocks()) { result.live_in[block.get()]; result.live_out[block.get()]; }
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = fn.blocks().rbegin(); it != fn.blocks().rend(); ++it) {
            auto* block = it->get(); std::unordered_set<uint32_t> out;
            for (auto* succ : block->successors()) {
                auto edge = result.live_in[succ];
                for (auto* phi : succ->instructions()) {
                    if (phi->opcode() != Opcode::Phi) continue;
                    edge.erase(phi->id());
                    bool precise = phi->incoming_blocks().size() * 2 == phi->operands().size();
                    for (std::size_t i = 1; i < phi->operands().size(); i += 2) {
                        if (precise && phi->incoming_blocks()[i/2] != block) continue;
                        auto value = phi->operand(i); if (value.is_vreg()) edge.insert(value.vreg_index());
                    }
                }
                out.insert(edge.begin(), edge.end());
            }
            auto in = out;
            for (auto inst = block->instructions().rbegin(); inst != block->instructions().rend(); ++inst) {
                if (!(*inst)->type().is_void()) in.erase((*inst)->id());
                if ((*inst)->opcode() != Opcode::Phi)
                    for_uses(**inst, [&](Value& v) { if (v.is_vreg()) in.insert(v.vreg_index()); });
            }
            if (in != result.live_in[block] || out != result.live_out[block]) {
                result.live_in[block] = std::move(in); result.live_out[block] = std::move(out); changed = true;
            }
        }
    }
}
}
JITERATI_DEFINE_PASS(LivenessAnalysis, "liveness-analysis", true)
