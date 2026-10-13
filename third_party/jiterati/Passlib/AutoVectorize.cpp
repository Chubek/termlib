/** @file AutoVectorize.cpp @brief Plan independent same-type scalar SIMD groups. */
#include "PassSupport.hpp"
#include "Jiterati-SIMD.hpp"
namespace jiterati::pass_detail {
void run_AutoVectorize(Function& fn, PassResult&) {
    for (auto& inst : fn.instr_storage()) inst.set_vector_width(0);
    for (auto& block : fn.blocks()) {
        std::vector<Instruction*> group;
        std::unordered_set<uint32_t> definitions;
        auto flush = [&] {
            if (!group.empty()) {
                auto width = simd::policy_width(simd::VectorPolicy::Auto, group[0]->type().size());
                if (width > 1 && group.size() >= width)
                    for (std::size_t i = 0; i < group.size() / width * width; ++i) group[i]->set_vector_width(width);
            }
            group.clear(); definitions.clear();
        };
        for (auto* inst : block->instructions()) {
            bool eligible = inst->type().is_int() && (inst->opcode() == Opcode::Add || inst->opcode() == Opcode::Mul);
            bool dependent = false;
            for_uses(*inst, [&](Value& value) { if (value.is_vreg() && definitions.count(value.vreg_index())) dependent = true; });
            if (!eligible || dependent || (!group.empty() && (inst->type() != group[0]->type() || inst->opcode() != group[0]->opcode()))) flush();
            if (eligible) { group.push_back(inst); definitions.insert(inst->id()); }
        }
        flush();
    }
}
}
namespace jiterati {
std::string autovec_host_description() {
    return "simd: cpu=" + simd::describe_cpu() + " xsimd=" + (simd::have_xsimd() ? "yes" : "no") +
           " simdette=" + (simd::have_simdette() ? "yes" : "no");
}
}
JITERATI_DEFINE_PASS(AutoVectorize, "autovectorize", false)
