/** @file StoreOfLoadElimination.cpp @brief Remove unchanged stack writebacks. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_StoreOfLoadElimination(Function& fn, PassResult&) {
    PassResult escape; run_EscapeAnalysis(fn, escape);
    std::unordered_set<Instruction*> dead;
    for (auto& block : fn.blocks()) {
        std::unordered_map<int32_t, Value> reads;
        for (auto* inst : block->instructions()) {
            if (inst->opcode() == Opcode::Call) { reads.clear(); continue; }
            if (inst->opcode() == Opcode::Load && inst->operands().size() == 1)
                reads[resolve_copy(fn, inst->operand(0)).raw()] = Value::vreg(inst->id());
            if (inst->opcode() != Opcode::Store || inst->operands().size() != 2) continue;
            auto pointer = resolve_copy(fn, inst->operand(0)); auto* allocation = definition(fn, pointer);
            auto read = reads.find(pointer.raw());
            if (allocation && allocation->opcode() == Opcode::Alloca && !escape.escaping_allocas.count(allocation->id()) &&
                read != reads.end() && resolve_copy(fn, inst->operand(1)) == read->second) dead.insert(inst);
            else reads.clear();
        }
    }
    erase(fn, dead);
}
}
JITERATI_DEFINE_PASS(StoreOfLoadElimination, "store-of-load-elimination", false)
