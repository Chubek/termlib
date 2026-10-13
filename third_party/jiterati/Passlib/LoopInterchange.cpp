/** @file LoopInterchange.cpp @brief Static schedule interchange of rectangular pure nests. */
#include "LoopSupport.hpp"
namespace jiterati::pass_detail {
void run_LoopInterchange(Function& fn, PassResult&) {
    for (;;) {
        std::optional<CountedDomain> outer, inner;
        for (auto& owner : fn.blocks()) {
            auto* header = owner.get();
            if (header->successors().size() != 1 || !terminator(header) || terminator(header)->opcode() != Opcode::Br) continue;
            auto b = counted_domain(fn, header->successors()[0], header->successors()[0]);
            if (!b || b->preheader != header) continue;
            auto a = counted_domain(fn, header, b->exit);
            if (!a || a->preheader->successors() != std::vector<Block*>{header} ||
                !terminator(a->preheader) || terminator(a->preheader)->opcode() != Opcode::Br ||
                a->latch->predecessors() != std::vector<Block*>{b->header}) continue;
            bool legal = header->instructions().size() == 2 && a->latch->instructions().size() == 3;
            for (auto* inst : b->header->instructions())
                if (inst != b->phi && inst != terminator(b->header) && !nontrapping(fn, *inst)) legal = false;
            if (!legal || a->iterations.size() * b->iterations.size() * b->header->instructions().size() > 128) continue;
            outer = a; inner = b; break;
        }
        if (!outer) break;
        std::unordered_map<int32_t, Value> values;
        // Only independent, nontrapping SSA arithmetic is reordered. The last
        // lattice point remains (outer.last, inner.last), preserving live-outs.
        for (auto j : inner->iterations) for (auto i : outer->iterations) {
            values[Value::vreg(outer->phi->id()).raw()] = literal(fn, outer->phi->type(), i);
            append_before_branch(outer->preheader, emit_iteration(fn, *inner, j, values));
        }
        std::vector<Instruction*> final_latch;
        for (auto* inst : outer->latch->instructions()) if (!control_terminator(inst->opcode())) final_latch.push_back(clone(fn, *inst, values));
        append_before_branch(outer->preheader, final_latch);
        finish_expansion(fn, outer->preheader, outer->exit, {outer->header, inner->header, outer->latch}, outer->latch, values);
    }
}
}
JITERATI_DEFINE_PASS(LoopInterchange, "loop-interchange", false)
