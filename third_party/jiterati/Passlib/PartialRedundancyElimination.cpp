/** @file PartialRedundancyElimination.cpp @brief Edge insertion and phi-based PRE. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_PartialRedundancyElimination(Function& fn, PassResult&) {
    auto dom = dominators(fn);
    for (auto* inst : snapshot(fn)) {
        auto* block = inst->parent();
        if (!block || block->predecessors().size() < 2 || !expression(fn, *inst) || inst->opcode() == Opcode::Copy) continue;
        bool legal = true, found = false;
        std::vector<std::pair<Block*, Instruction*>> sources;
        for (auto* pred : block->predecessors()) {
            auto* term = terminator(pred);
            if (!term || !control_terminator(term->opcode())) { legal = false; break; }
            Instruction* available = nullptr;
            for (auto* candidate : pred->instructions())
                if (expression(fn, *candidate) && key(fn, *candidate) == key(fn, *inst)) available = candidate;
            if (available) found = true;
            else for_uses(*inst, [&](Value& value) {
                auto resolved = resolve_copy(fn, value); auto* def = definition(fn, resolved);
                if (def && def->parent() && !dom[pred].count(def->parent())) legal = false;
            });
            sources.emplace_back(pred, available);
        }
        if (!legal || !found) continue;
        std::vector<Value> operands; std::vector<Block*> incoming;
        for (auto [pred, available] : sources) {
            if (!available) {
                available = fn.make_instruction(inst->opcode(), inst->type(), inst->operands());
                for_uses(*available, [&](Value& value) { value = resolve_copy(fn, value); });
                available->set_parent(pred);
                auto& list = pred->instructions(); list.insert(list.end()-1, available);
            }
            incoming.push_back(pred); operands.push_back(Value::none()); operands.push_back(Value::vreg(available->id()));
        }
        auto* phi = fn.make_instruction(Opcode::Phi, inst->type(), std::move(operands));
        phi->incoming_blocks() = std::move(incoming); phi->set_parent(block);
        block->instructions().insert(block->instructions().begin(), phi);
        copy(*inst, Value::vreg(phi->id()));
    }
}
}
JITERATI_DEFINE_PASS(PartialRedundancyElimination, "partial-redundancy-elimination", false)
