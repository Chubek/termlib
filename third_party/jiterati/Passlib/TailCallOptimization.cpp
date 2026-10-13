/** @file TailCallOptimization.cpp @brief ABI-compatible tail-position eligibility. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_TailCallOptimization(Function& fn, PassResult&) {
    for (auto& inst : fn.instr_storage()) if (inst.opcode() == Opcode::Call) inst.set_tail_call(false);
    if (!fn.module()) return;
    for (auto& block : fn.blocks()) {
        auto const& list = block->instructions();
        if (list.size() < 2) continue;
        auto* ret = list.back(); auto* call = list[list.size()-2];
        if (ret->opcode() != Opcode::Ret || call->opcode() != Opcode::Call) continue;
        auto* callee = fn.module()->find_function(call->name());
        if (!callee || callee->ret_type() != fn.ret_type() || callee->param_types() != fn.param_types()) continue;
        if (fn.ret_type().is_void() ? !ret->operands().empty() :
            (ret->operands().size() != 1 || resolve_copy(fn, ret->operand(0)) != Value::vreg(call->id()))) continue;
        bool stack = false;
        for (auto& inst : fn.instr_storage()) if (inst.opcode() == Opcode::Alloca) stack = true;
        if (!stack) call->set_tail_call(true);
    }
}
}
JITERATI_DEFINE_PASS(TailCallOptimization, "tail-call-optimization", false)
