/** @file ControlFlowOptimization.cpp @brief Branch folding and linear CFG merging. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_ControlFlowOptimization(Function& fn, PassResult& result) {
    for (auto& owner : fn.blocks()) {
        auto* block = owner.get(); auto* term = terminator(block);
        if (!term || term->opcode() != Opcode::CondBr || term->operands().size() != 1 || block->successors().size() != 2) continue;
        auto condition = integer(fn, term->operand(0));
        if (!condition && block->successors()[0] != block->successors()[1]) continue;
        auto* chosen = block->successors()[condition && !*condition ? 1 : 0];
        for (auto* successor : block->successors()) if (successor != chosen) remove_phi_edge(successor, block);
        rewrite(*term, Opcode::Br, {}); block->set_successors({chosen});
    }
    run_UnreachableBlockElimination(fn, result);
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& owner : fn.blocks()) {
            auto* block = owner.get(); auto* term = terminator(block);
            if (!term || term->opcode() != Opcode::Br || block->successors().size() != 1) continue;
            auto* next = block->successors()[0];
            if (next == block || next == fn.entry_block() || next->predecessors().size() != 1) continue;
            bool legal = true;
            for (auto* inst : next->instructions()) if (inst->opcode() == Opcode::Phi &&
                (inst->operands().size() != 2 || inst->incoming_blocks() != std::vector<Block*>{block})) legal = false;
            if (!legal) continue;
            for (auto* inst : next->instructions()) if (inst->opcode() == Opcode::Phi) copy(*inst, inst->operand(1));
            erase(fn, {term});
            auto edges = next->successors();
            for (auto* target : edges) rename_phi_edge(target, next, block);
            next->set_successors({}); block->set_successors(std::move(edges));
            for (auto* inst : next->instructions()) block->append(inst);
            next->instructions().clear(); remove_blocks(fn, {next});
            changed = true; break;
        }
    }
}
}
JITERATI_DEFINE_PASS(ControlFlowOptimization, "control-flow-optimization", false)
