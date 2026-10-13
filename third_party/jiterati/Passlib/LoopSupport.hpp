/** @file LoopSupport.hpp @brief Static counted-domain recognition and SSA cloning. */
#ifndef JITERATI_LOOP_SUPPORT_HPP
#define JITERATI_LOOP_SUPPORT_HPP
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
struct CountedDomain {
    Block* header = nullptr;
    Block* latch = nullptr;
    Block* preheader = nullptr;
    Block* exit = nullptr;
    Instruction* phi = nullptr;
    Instruction* step = nullptr;
    Instruction* compare = nullptr;
    std::vector<uint64_t> iterations;
    uint64_t delta = 0;
};
inline bool predicate(Opcode op, uint64_t a, uint64_t b, Type type) {
    switch (op) {
        case Opcode::ICmpEq: return a == b;
        case Opcode::ICmpNe: return a != b;
        case Opcode::ICmpSLt: return signed_bits(a, type) < signed_bits(b, type);
        case Opcode::ICmpSLe: return signed_bits(a, type) <= signed_bits(b, type);
        case Opcode::ICmpSGt: return signed_bits(a, type) > signed_bits(b, type);
        case Opcode::ICmpSGe: return signed_bits(a, type) >= signed_bits(b, type);
        case Opcode::ICmpULt: return a < b;
        case Opcode::ICmpULe: return a <= b;
        case Opcode::ICmpUGt: return a > b;
        case Opcode::ICmpUGe: return a >= b;
        default: return false;
    }
}
inline std::optional<CountedDomain> counted_domain(Function& fn, Block* header, Block* latch) {
    CountedDomain domain; domain.header = header; domain.latch = latch;
    auto* term = terminator(latch);
    if (!header || !term || term->opcode() != Opcode::CondBr || term->operands().size() != 1 ||
        latch->successors().size() != 2 || header->predecessors().size() != 2) return std::nullopt;
    bool continue_true = latch->successors()[0] == header;
    if (!continue_true && latch->successors()[1] != header) return std::nullopt;
    domain.exit = latch->successors()[continue_true ? 1 : 0];
    if (domain.exit == header) return std::nullopt;
    for (auto* pred : header->predecessors()) if (pred != latch) domain.preheader = pred;
    if (!domain.preheader) return std::nullopt;
    for (auto* inst : header->instructions()) if (inst->opcode() == Opcode::Phi) {
        if (domain.phi) return std::nullopt;
        domain.phi = inst;
    }
    auto* phi = domain.phi;
    if (!phi || !phi->type().is_int() || phi->operands().size() != 4 || phi->incoming_blocks().size() != 2) return std::nullopt;
    std::size_t initial_index = phi->incoming_blocks()[0] == domain.preheader ? 0 : 1;
    if (phi->incoming_blocks()[initial_index] != domain.preheader || phi->incoming_blocks()[1-initial_index] != latch) return std::nullopt;
    auto initial = integer(fn, phi->operand(2*initial_index+1));
    auto* step = definition(fn, phi->operand(2*(1-initial_index)+1));
    if (!initial || !step || step->parent() != latch || step->type() != phi->type() || step->operands().size() != 2 ||
        (step->opcode() != Opcode::Add && step->opcode() != Opcode::Sub) || step->operand(0) != Value::vreg(phi->id())) return std::nullopt;
    auto increment = integer(fn, step->operand(1));
    if (!increment) return std::nullopt;
    domain.delta = step->opcode() == Opcode::Add ? *increment : uint64_t(0) - *increment;
    domain.delta &= mask(phi->type()); domain.step = step;
    auto* cmp = definition(fn, term->operand(0));
    if (!cmp || cmp->parent() != latch || cmp->operands().size() != 2 ||
        cmp->opcode() < Opcode::ICmpEq || cmp->opcode() > Opcode::ICmpUGe) return std::nullopt;
    if (cmp->operand(0) != Value::vreg(step->id()) && cmp->operand(0) != Value::vreg(phi->id())) return std::nullopt;
    auto bound = integer(fn, cmp->operand(1)); if (!bound) return std::nullopt;
    domain.compare = cmp;
    uint64_t current = *initial;
    for (unsigned i = 0; i < 16; ++i) {
        domain.iterations.push_back(current);
        uint64_t next = (current + domain.delta) & mask(phi->type());
        uint64_t tested = cmp->operand(0) == Value::vreg(step->id()) ? next : current;
        if (predicate(cmp->opcode(), tested, *bound, phi->type()) != continue_true) return domain;
        current = next;
    }
    return std::nullopt;
}
inline std::optional<CountedDomain> simple_loop(Function& fn, Block* header, bool reorder = false) {
    auto domain = counted_domain(fn, header, header);
    if (!domain || domain->preheader->successors() != std::vector<Block*>{header} ||
        !terminator(domain->preheader) || terminator(domain->preheader)->opcode() != Opcode::Br) return std::nullopt;
    for (auto* inst : header->instructions()) {
        if (inst == domain->phi || inst == terminator(header)) continue;
        if (control_terminator(inst->opcode()) || inst->opcode() == Opcode::Call || inst->opcode() == Opcode::Alloca ||
            inst->opcode() == Opcode::Phi || (reorder && !nontrapping(fn, *inst))) return std::nullopt;
    }
    if (header->instructions().size() * domain->iterations.size() > 128) return std::nullopt;
    return domain;
}
inline std::vector<Instruction*> emit_iteration(Function& fn, CountedDomain const& domain, uint64_t iteration,
                                               std::unordered_map<int32_t, Value>& values) {
    values[Value::vreg(domain.phi->id()).raw()] = literal(fn, domain.phi->type(), iteration);
    std::vector<Instruction*> result;
    for (auto* inst : domain.header->instructions()) {
        if (inst == domain.phi || control_terminator(inst->opcode())) continue;
        result.push_back(clone(fn, *inst, values));
    }
    return result;
}
inline void append_before_branch(Block* target, std::vector<Instruction*> const& instructions) {
    auto& list = target->instructions(); list.insert(list.end()-1, instructions.begin(), instructions.end());
    for (auto* inst : instructions) inst->set_parent(target);
}
inline void replace_region_uses(Function& fn, std::unordered_set<Block*> const& region,
                                std::unordered_map<int32_t, Value> const& values) {
    for (auto& inst : fn.instr_storage()) {
        if (region.count(inst.parent())) continue;
        for_uses(inst, [&](Value& value) { auto it = values.find(value.raw()); if (it != values.end()) value = it->second; });
    }
}
inline void finish_expansion(Function& fn, Block* preheader, Block* exit, std::unordered_set<Block*> const& region,
                             Block* exiting, std::unordered_map<int32_t, Value> const& values) {
    replace_region_uses(fn, region, values);
    rename_phi_edge(exit, exiting, preheader);
    for (auto* block : region) block->set_successors({});
    preheader->set_successors({exit});
    remove_blocks(fn, region);
}
}
#endif
