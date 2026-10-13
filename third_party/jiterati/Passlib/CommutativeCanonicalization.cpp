/** @file CommutativeCanonicalization.cpp @brief Stable commutative operand order. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_CommutativeCanonicalization(Function& fn, PassResult&) {
    for (auto* inst : snapshot(fn)) {
        if (!inst->is_commutative() || inst->operands().size() != 2 || !fn.value_type(inst->operand(0)).is_int()) continue;
        bool a = integer(fn, inst->operand(0)).has_value(), b = integer(fn, inst->operand(1)).has_value();
        if ((a && !b) || (a == b && inst->operand(0).raw() > inst->operand(1).raw()))
            std::swap(inst->operands()[0], inst->operands()[1]);
    }
}
}
JITERATI_DEFINE_PASS(CommutativeCanonicalization, "commutative-canonicalization", false)
