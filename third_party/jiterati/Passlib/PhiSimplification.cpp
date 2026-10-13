/** @file PhiSimplification.cpp @brief Collapse equal and self-referential phi inputs. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_PhiSimplification(Function& fn, PassResult&) {
    for (auto& inst : fn.instr_storage()) {
        if (inst.opcode() != Opcode::Phi || inst.operands().size() % 2) continue;
        Value common;
        bool different = false;
        for (std::size_t i = 1; i < inst.operands().size(); i += 2) {
            auto value = resolve_copy(fn, inst.operand(i));
            if (value == Value::vreg(inst.id())) continue;
            if (!common.valid()) common = value;
            else if (common != value) { different = true; break; }
        }
        if (common.valid() && !different) copy(inst, common);
    }
}
}
JITERATI_DEFINE_PASS(PhiSimplification, "phi-simplification", false)
