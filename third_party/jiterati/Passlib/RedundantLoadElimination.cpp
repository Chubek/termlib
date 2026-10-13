/** @file RedundantLoadElimination.cpp @brief Reuse loads between memory clobbers. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_RedundantLoadElimination(Function& fn, PassResult&) {
    for (auto& block : fn.blocks()) {
        std::map<std::pair<int32_t, PrimType>, Value> loads;
        for (auto* inst : block->instructions()) {
            if (inst->opcode() == Opcode::Store || inst->opcode() == Opcode::Call) { loads.clear(); continue; }
            if (inst->opcode() != Opcode::Load || inst->operands().size() != 1) continue;
            auto key = std::make_pair(resolve_copy(fn, inst->operand(0)).raw(), inst->type().prim());
            auto previous = loads.find(key);
            if (previous != loads.end()) copy(*inst, previous->second);
            else loads[key] = Value::vreg(inst->id());
        }
    }
}
}
JITERATI_DEFINE_PASS(RedundantLoadElimination, "redundant-load-elimination", false)
