/** @file LoadForwarding.cpp @brief Local exact-address store-to-load forwarding. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_LoadForwarding(Function& fn, PassResult&) {
    for (auto& block : fn.blocks()) {
        Value address, stored;
        for (auto* inst : block->instructions()) {
            if (inst->opcode() == Opcode::Call) { address = stored = Value::none(); continue; }
            if (inst->opcode() == Opcode::Store && inst->operands().size() == 2) {
                address = resolve_copy(fn, inst->operand(0)); stored = inst->operand(1);
            } else if (inst->opcode() == Opcode::Load && inst->operands().size() == 1 &&
                       address.valid() && resolve_copy(fn, inst->operand(0)) == address && fn.value_type(stored) == inst->type()) {
                copy(*inst, stored);
            }
        }
    }
}
}
JITERATI_DEFINE_PASS(LoadForwarding, "load-forwarding", false)
