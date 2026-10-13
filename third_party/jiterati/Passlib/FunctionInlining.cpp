/** @file FunctionInlining.cpp @brief Bounded single-block nonrecursive inlining. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_FunctionInlining(Function& fn, PassResult&) {
    if (!fn.module()) return;
    std::unordered_set<Instruction*> dead;
    for (auto* call : snapshot(fn)) {
        if (call->opcode() != Opcode::Call || !call->parent()) continue;
        auto* callee = fn.module()->find_function(call->name());
        if (!callee || callee == &fn || callee->blocks().size() != 1 || callee->instr_storage().size() > 32 ||
            call->operands().size() != callee->param_count() + 1 || callee->ret_type() != call->type()) continue;
        auto* body = callee->entry_block(); auto* ret = terminator(body);
        if (!ret || ret->opcode() != Opcode::Ret || ret->operands().size() != (call->type().is_void() ? 0u : 1u)) continue;
        bool legal = true;
        for (auto* inst : body->instructions()) if (inst != ret &&
            (control_terminator(inst->opcode()) || inst->opcode() == Opcode::Call || inst->opcode() == Opcode::Phi)) legal = false;
        for (std::size_t i = 0; i < callee->param_count(); ++i)
            if (fn.value_type(call->operand(i+1)) != callee->param_types()[i]) legal = false;
        if (!legal) continue;
        std::unordered_map<int32_t, Value> values;
        for (std::size_t i = 0; i < callee->param_count(); ++i) values[callee->param_value(i).raw()] = call->operand(i+1);
        for (uint32_t i = 0; i < callee->const_count(); ++i)
            values[Value::const_idx(i).raw()] = fn.make_constant(callee->const_value(i), callee->const_type(i))->operand(0);
        for (auto& inst : callee->instr_storage()) if (!inst.parent() && inst.opcode() == Opcode::Copy && inst.operands().size() == 1) {
            auto it = values.find(inst.operand(0).raw());
            if (it != values.end()) values[Value::vreg(inst.id()).raw()] = it->second;
        }
        auto* owner = call->parent(); std::vector<Instruction*> expansion;
        for (auto* inst : body->instructions()) if (inst != ret) {
            auto* cloned = clone(fn, *inst, values);
            if (inst->opcode() == Opcode::Alloca)
                for (std::size_t i = 0; i < inst->operands().size(); ++i) {
                    auto it = values.find(inst->operand(i).raw());
                    if (it != values.end()) cloned->set_operand(i,it->second);
                }
            cloned->set_parent(owner); expansion.push_back(cloned);
        }
        if (!call->type().is_void()) {
            auto it = values.find(ret->operand(0).raw());
            Value returned = it == values.end() ? ret->operand(0) : it->second;
            replace_uses(fn, Value::vreg(call->id()), returned);
        }
        auto& list = owner->instructions(); list.insert(std::find(list.begin(), list.end(), call), expansion.begin(), expansion.end());
        dead.insert(call);
    }
    erase(fn, dead);
}
}
JITERATI_DEFINE_PASS(FunctionInlining, "function-inlining", false)
