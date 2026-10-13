/** @file ComparisonCanonicalization.cpp @brief Canonical less-than predicates. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_ComparisonCanonicalization(Function& fn, PassResult&) {
    for (auto& inst : fn.instr_storage()) {
        if (inst.operands().size() != 2 || !fn.value_type(inst.operand(0)).is_int()) continue;
        Opcode op;
        switch (inst.opcode()) {
            case Opcode::ICmpSGt: op = Opcode::ICmpSLt; break;
            case Opcode::ICmpSGe: op = Opcode::ICmpSLe; break;
            case Opcode::ICmpUGt: op = Opcode::ICmpULt; break;
            case Opcode::ICmpUGe: op = Opcode::ICmpULe; break;
            default: continue;
        }
        rewrite(inst, op, {inst.operand(1), inst.operand(0)});
    }
}
}
JITERATI_DEFINE_PASS(ComparisonCanonicalization, "comparison-canonicalization", false)
