/** @file CopyPropagation.cpp @brief Cycle-safe transitive SSA copy propagation. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_CopyPropagation(Function& fn, PassResult&) {
    for (auto& inst : fn.instr_storage()) for_uses(inst, [&](Value& value) { value = resolve_copy(fn, value); });
}
}
JITERATI_DEFINE_PASS(CopyPropagation, "copy-propagation", false)
