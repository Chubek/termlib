/** @file ConstantPropagation.cpp @brief Fixed-point integer constant propagation. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_ConstantPropagation(Function& fn, PassResult& result) {
    // Each productive iteration converts at least one operation or operand.
    for (;;) {
        std::vector<std::pair<Opcode, std::vector<Value>>> before;
        for (auto* inst : snapshot(fn)) before.emplace_back(inst->opcode(), inst->operands());
        auto instructions = snapshot(fn);
        for (auto* inst : instructions) for_uses(*inst, [&](Value& value) {
            auto resolved = resolve_copy(fn, value);
            if (resolved.is_const()) value = resolved;
        });
        run_ConstantFolding(fn, result);
        run_SelectSimplification(fn, result);
        run_PhiSimplification(fn, result);
        bool changed = false;
        for (std::size_t i = 0; i < instructions.size(); ++i)
            if (before[i].first != instructions[i]->opcode() || before[i].second != instructions[i]->operands()) changed = true;
        if (!changed) break;
    }
}
}
JITERATI_DEFINE_PASS(ConstantPropagation, "constant-propagation", false)
