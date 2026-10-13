/** @file PassSupport.hpp @brief Internal SSA, CFG and integer rewrite utilities. */
#ifndef JITERATI_PASS_SUPPORT_HPP
#define JITERATI_PASS_SUPPORT_HPP
#include "Jiterati-Pass.hpp"
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace jiterati::pass_detail {
using Runner = void (*)(Function&, PassResult&);
class BuiltinPass final : public FunctionPass {
public:
    BuiltinPass(std::string name, bool analysis, Runner runner)
        : name_(std::move(name)), analysis_(analysis), runner_(runner) {}
    std::string name() const override { return name_; }
    bool is_analysis() const override { return analysis_; }
    bool is_transform() const override { return !analysis_; }
    PassResult const* result() const override { return analysis_ ? &result_ : nullptr; }
    void run(Function& fn) override { result_ = {}; runner_(fn, result_); }
private:
    std::string name_;
    bool analysis_;
    Runner runner_;
    PassResult result_;
};

inline unsigned bits(Type type) {
    return type.prim() == PrimType::I1 ? 1 : static_cast<unsigned>(type.size() * 8);
}
inline uint64_t mask(Type type) {
    unsigned width = bits(type);
    return width == 64 ? UINT64_MAX : (uint64_t(1) << width) - 1;
}
inline int64_t signed_bits(uint64_t value, Type type) {
    value &= mask(type);
    uint64_t sign = uint64_t(1) << (bits(type) - 1);
    if (!(value & sign)) return static_cast<int64_t>(value);
    return -1 - static_cast<int64_t>((~value) & mask(type));
}
inline Instruction* definition(Function& fn, Value value) {
    return value.is_vreg() ? fn.instruction_by_id(value.vreg_index()) : nullptr;
}
inline Value resolve_copy(Function& fn, Value value) {
    std::unordered_set<int32_t> seen;
    while (seen.insert(value.raw()).second) {
        auto* inst = definition(fn, value);
        if (!inst || inst->opcode() != Opcode::Copy || inst->operands().size() != 1 ||
            !inst->operand(0).valid() || fn.value_type(inst->operand(0)) != inst->type()) break;
        value = inst->operand(0);
    }
    return value;
}
inline std::optional<uint64_t> integer(Function& fn, Value value) {
    value = resolve_copy(fn, value);
    if (!value.is_const() || !fn.value_type(value).is_int()) return std::nullopt;
    std::optional<uint64_t> result;
    std::visit([&](auto v) {
        using T = decltype(v);
        if constexpr (std::is_integral_v<T>) result = static_cast<uint64_t>(v);
    }, fn.const_value(value.const_index()));
    if (result) *result &= mask(fn.value_type(value));
    return result;
}
inline bool equals(Function& fn, Value value, uint64_t expected) {
    auto number = integer(fn, value);
    return number && *number == (expected & mask(fn.value_type(value)));
}
inline Value literal(Function& fn, Type type, uint64_t value) {
    value &= mask(type);
    Constant constant;
    switch (type.prim()) {
        case PrimType::I1: constant = bool(value); break;
        case PrimType::I8: constant = static_cast<uint8_t>(value); break;
        case PrimType::I16: constant = static_cast<uint16_t>(value); break;
        case PrimType::I32: constant = static_cast<uint32_t>(value); break;
        case PrimType::I64: constant = value; break;
        default: throw std::invalid_argument("integer literal requires an integer type");
    }
    return fn.make_constant(constant, type)->operand(0);
}
inline void rewrite(Instruction& inst, Opcode opcode, std::vector<Value> operands) {
    inst.set_opcode(opcode);
    inst.operands() = std::move(operands);
    if (opcode != Opcode::Phi) inst.incoming_blocks().clear();
    inst.set_vector_width(0);
    inst.set_tail_call(false);
}
inline void copy(Instruction& inst, Value value) { rewrite(inst, Opcode::Copy, {value}); }
// Branch labels, call symbols, alloca sizes and phi labels are not SSA uses.
template<class F> void for_uses(Instruction& inst, F&& visit) {
    if (inst.opcode() == Opcode::Br || inst.opcode() == Opcode::Alloca ||
        inst.opcode() == Opcode::GetParam) return;
    std::size_t start = inst.opcode() == Opcode::Call || inst.opcode() == Opcode::Phi ? 1 : 0;
    std::size_t stride = inst.opcode() == Opcode::Phi ? 2 : 1;
    for (std::size_t i = start; i < inst.operands().size(); i += stride) visit(inst.operands()[i]);
}
inline void replace_uses(Function& fn, Value old, Value replacement) {
    if (old == replacement) return;
    for (auto& inst : fn.instr_storage())
        for_uses(inst, [&](Value& value) { if (value == old) value = replacement; });
}
inline bool pure(Instruction const& inst) {
    switch (inst.opcode()) {
        case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::Neg:
        case Opcode::And: case Opcode::Or: case Opcode::Xor: case Opcode::Not:
        case Opcode::ICmpEq: case Opcode::ICmpNe: case Opcode::ICmpSLt: case Opcode::ICmpSLe:
        case Opcode::ICmpSGt: case Opcode::ICmpSGe: case Opcode::ICmpULt: case Opcode::ICmpULe:
        case Opcode::ICmpUGt: case Opcode::ICmpUGe: case Opcode::Trunc: case Opcode::ZExt:
        case Opcode::SExt: case Opcode::IntToPtr: case Opcode::PtrToInt: case Opcode::Copy:
        case Opcode::Select: case Opcode::Phi: return true;
        // Shifts require a proven in-range count before they can be speculated.
        default: return false;
    }
}
inline bool nontrapping(Function& fn, Instruction const& inst) {
    if (pure(inst)) return true;
    if ((inst.opcode() == Opcode::Shl || inst.opcode() == Opcode::LShr || inst.opcode() == Opcode::AShr) &&
        inst.operands().size() == 2 && inst.type().is_int()) {
        auto amount = integer(fn, inst.operand(1));
        return amount && *amount < bits(inst.type());
    }
    return false;
}
inline bool expression(Function& fn, Instruction const& inst) {
    return nontrapping(fn, inst) && inst.opcode() != Opcode::Phi && !inst.type().is_void();
}
inline void erase(Function& fn, std::unordered_set<Instruction*> const& dead) {
    for (auto& block : fn.blocks()) {
        auto& list = block->instructions();
        list.erase(std::remove_if(list.begin(), list.end(), [&](auto* inst) { return dead.count(inst); }), list.end());
    }
    for (auto it = fn.instr_storage().begin(); it != fn.instr_storage().end();) {
        if (dead.count(&*it)) it = fn.instr_storage().erase(it); else ++it;
    }
}
inline std::vector<Instruction*> snapshot(Function& fn) {
    std::vector<Instruction*> result;
    for (auto& inst : fn.instr_storage()) result.push_back(&inst);
    return result;
}
inline Instruction* terminator(Block* block) {
    return block && !block->instructions().empty() ? block->instructions().back() : nullptr;
}
inline bool control_terminator(Opcode op) { return op != Opcode::Call && opcode_is_terminator(op); }
inline std::vector<Block*> reachable(Function& fn) {
    std::vector<Block*> result;
    std::unordered_set<Block*> seen;
    std::function<void(Block*)> visit = [&](Block* block) {
        if (!block || !seen.insert(block).second) return;
        result.push_back(block);
        for (auto* next : block->successors()) visit(next);
    };
    visit(fn.entry_block());
    return result;
}
inline auto dominators(Function& fn) {
    std::unordered_map<Block*, std::unordered_set<Block*>> dom;
    auto blocks = reachable(fn);
    if (blocks.empty()) return dom;
    std::unordered_set<Block*> all(blocks.begin(), blocks.end());
    for (auto* block : blocks) dom[block] = block == fn.entry_block() ? std::unordered_set<Block*>{block} : all;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto* block : blocks) {
            if (block == fn.entry_block()) continue;
            auto next = all;
            for (auto* pred : block->predecessors()) {
                if (!all.count(pred)) continue;
                for (auto it = next.begin(); it != next.end();) {
                    if (!dom[pred].count(*it)) it = next.erase(it); else ++it;
                }
            }
            next.insert(block);
            if (next != dom[block]) { dom[block] = std::move(next); changed = true; }
        }
    }
    return dom;
}
inline void remove_phi_edge(Block* target, Block* source) {
    for (auto* inst : target->instructions()) {
        if (inst->opcode() != Opcode::Phi) continue;
        auto& incoming = inst->incoming_blocks();
        if (incoming.size() * 2 != inst->operands().size()) continue;
        for (std::size_t i = incoming.size(); i-- > 0;) {
            if (incoming[i] != source) continue;
            incoming.erase(incoming.begin() + i);
            inst->operands().erase(inst->operands().begin() + 2*i, inst->operands().begin() + 2*i + 2);
        }
    }
}
inline void rename_phi_edge(Block* target, Block* old, Block* replacement) {
    for (auto* inst : target->instructions())
        for (auto*& block : inst->incoming_blocks()) if (block == old) block = replacement;
}
inline bool has_phi(Block* block) {
    return std::any_of(block->instructions().begin(), block->instructions().end(),
        [](auto* inst) { return inst->opcode() == Opcode::Phi; });
}
inline void remove_blocks(Function& fn, std::unordered_set<Block*> const& dead) {
    std::unordered_set<Instruction*> instructions;
    for (auto* block : dead) {
        for (auto* inst : block->instructions()) instructions.insert(inst);
        for (auto* succ : block->successors()) remove_phi_edge(succ, block);
    }
    for (auto& block : fn.blocks()) {
        if (dead.count(block.get())) { block->set_successors({}); continue; }
        auto edges = block->successors();
        edges.erase(std::remove_if(edges.begin(), edges.end(), [&](auto* edge) { return dead.count(edge); }), edges.end());
        block->set_successors(std::move(edges));
    }
    erase(fn, instructions);
    auto& blocks = fn.blocks();
    blocks.erase(std::remove_if(blocks.begin(), blocks.end(), [&](auto const& block) { return dead.count(block.get()); }), blocks.end());
}
using ExprKey = std::tuple<Opcode, PrimType, std::vector<int32_t>>;
inline ExprKey key(Function& fn, Instruction const& inst) {
    std::vector<int32_t> operands;
    for (auto operand : inst.operands()) operands.push_back(resolve_copy(fn, operand).raw());
    if (inst.is_commutative()) std::sort(operands.begin(), operands.end());
    return {inst.opcode(), inst.type().prim(), operands};
}
inline Instruction* clone(Function& fn, Instruction const& inst, std::unordered_map<int32_t, Value>& values) {
    auto* result = fn.make_instruction(inst.opcode(), inst.type(), inst.operands());
    for_uses(*result, [&](Value& value) { auto it = values.find(value.raw()); if (it != values.end()) value = it->second; });
    if (!inst.name().empty()) result->set_name(inst.name() + "." + std::to_string(result->id()));
    result->incoming_blocks() = inst.incoming_blocks();
    values[Value::vreg(inst.id()).raw()] = Value::vreg(result->id());
    return result;
}
// Shared analysis/rewrite entry points, implemented in their respective files.
void run_ConstantFolding(Function&, PassResult&);
void run_CopyPropagation(Function&, PassResult&);
void run_AlgebraicSimplification(Function&, PassResult&);
void run_SelectSimplification(Function&, PassResult&);
void run_PhiSimplification(Function&, PassResult&);
void run_UnreachableBlockElimination(Function&, PassResult&);
void run_DeadCodeElimination(Function&, PassResult&);
void run_ControlFlowOptimization(Function&, PassResult&);
void run_LoopUnrolling(Function&, PassResult&);
void run_LoopInterchange(Function&, PassResult&);
void run_LivenessAnalysis(Function&, PassResult&);
void run_EscapeAnalysis(Function&, PassResult&);
} // namespace jiterati::pass_detail

#define JITERATI_DEFINE_PASS(CLASS, NAME, ANALYSIS) \
namespace jiterati { std::unique_ptr<Pass> make_##CLASS() { \
    return std::make_unique<pass_detail::BuiltinPass>(NAME, ANALYSIS, pass_detail::run_##CLASS); \
} }
#endif
