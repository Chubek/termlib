/** @file CommonSubexpressionElimination.cpp @brief Dominating expression reuse. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_CommonSubexpressionElimination(Function& fn, PassResult&) {
    auto dom = dominators(fn);
    std::map<ExprKey, std::vector<Instruction*>> available;
    for (auto* block : reachable(fn)) for (auto* inst : block->instructions()) {
        if (!expression(fn, *inst) || inst->opcode() == Opcode::Copy) continue;
        auto& candidates = available[key(fn, *inst)];
        Instruction* previous = nullptr;
        for (auto* candidate : candidates)
            if (candidate->parent() == block || dom[block].count(candidate->parent())) { previous = candidate; break; }
        if (previous) {
            auto replacement = Value::vreg(previous->id());
            replace_uses(fn, Value::vreg(inst->id()), replacement); copy(*inst, replacement);
        } else candidates.push_back(inst);
    }
}
}
JITERATI_DEFINE_PASS(CommonSubexpressionElimination, "common-subexpression-elimination", false)
