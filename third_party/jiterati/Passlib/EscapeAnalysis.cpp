/** @file EscapeAnalysis.cpp @brief Conservative allocation escape classification. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_EscapeAnalysis(Function& fn, PassResult& result) {
    for (auto* allocation : snapshot(fn)) {
        if (allocation->opcode() != Opcode::Alloca) continue;
        auto pointer = Value::vreg(allocation->id());
        for (auto& inst : fn.instr_storage()) {
            if (inst.opcode() == Opcode::Copy) continue;
            std::size_t use = 0;
            for_uses(inst, [&](Value& value) {
                bool address = (inst.opcode() == Opcode::Load || inst.opcode() == Opcode::Store) && use == 0;
                if (resolve_copy(fn, value) == pointer && !address) result.escaping_allocas.insert(allocation->id());
                ++use;
            });
        }
    }
}
}
JITERATI_DEFINE_PASS(EscapeAnalysis, "escape-analysis", true)
