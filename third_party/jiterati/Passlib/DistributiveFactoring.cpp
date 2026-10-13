/** @file DistributiveFactoring.cpp @brief Factor common integer multiplicands. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_DistributiveFactoring(Function& fn, PassResult&) {
    std::unordered_map<uint32_t, unsigned> uses;
    for (auto& inst : fn.instr_storage()) for_uses(inst, [&](Value& v) { if (v.is_vreg()) ++uses[v.vreg_index()]; });
    for (auto* inst : snapshot(fn)) {
        if (inst->opcode() != Opcode::Add || !inst->type().is_int() || inst->operands().size() != 2 || !inst->parent()) continue;
        auto* a = definition(fn, inst->operand(0)); auto* b = definition(fn, inst->operand(1));
        if (!a || !b || a->opcode() != Opcode::Mul || b->opcode() != Opcode::Mul || a->type() != inst->type() ||
            b->type() != inst->type() || a->operands().size() != 2 || b->operands().size() != 2 || uses[a->id()] != 1 || uses[b->id()] != 1) continue;
        bool found = false;
        for (unsigned i = 0; i < 2 && !found; ++i) for (unsigned j = 0; j < 2 && !found; ++j) {
            if (a->operand(i) != b->operand(j)) continue;
            auto* sum = fn.make_instruction(Opcode::Add, inst->type(), {a->operand(1-i), b->operand(1-j)});
            auto& list = inst->parent()->instructions();
            list.insert(std::find(list.begin(), list.end(), inst), sum); sum->set_parent(inst->parent());
            rewrite(*inst, Opcode::Mul, {a->operand(i), Value::vreg(sum->id())}); found = true;
        }
    }
}
}
JITERATI_DEFINE_PASS(DistributiveFactoring, "distributive-factoring", false)
