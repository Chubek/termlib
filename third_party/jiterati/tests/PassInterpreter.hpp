/** @file PassInterpreter.hpp @brief Independent scalar interpreter and SSA verifier for pass tests. */
#ifndef JITERATI_TEST_PASS_INTERPRETER_HPP
#define JITERATI_TEST_PASS_INTERPRETER_HPP
#include "Jiterati-Pass.hpp"
#include <map>
#include <set>
#include <limits>
#include <cstdio>
#include <cstdlib>

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s at %s:%d\n", #cond, __FILE__, __LINE__); std::exit(1); \
} } while (0)

namespace pass_test {
using namespace jiterati;
inline unsigned width(Type type) { return type == Type::i1() ? 1 : static_cast<unsigned>(type.size()*8); }
inline uint64_t limit(Type type) { return width(type) == 64 ? UINT64_MAX : (uint64_t(1) << width(type))-1; }
inline int64_t signed_value(uint64_t v, Type type) {
    v &= limit(type);
    return (v & (uint64_t(1) << (width(type)-1))) ? -1-static_cast<int64_t>((~v)&limit(type)) : static_cast<int64_t>(v);
}
inline Value constant(Function& fn, Type type, uint64_t v) {
    v &= limit(type);
    Constant c;
    switch (type.prim()) {
        case PrimType::I1: c = bool(v); break;
        case PrimType::I8: c = static_cast<uint8_t>(v); break;
        case PrimType::I16: c = static_cast<uint16_t>(v); break;
        case PrimType::I32: c = static_cast<uint32_t>(v); break;
        default: c = v; break;
    }
    return fn.make_constant(c, type)->operand(0);
}
struct Execution {
    uint64_t returned = 0;
    bool trapped = false;
    std::map<uint64_t, uint64_t> memory;
    bool operator==(Execution const& other) const {
        return returned == other.returned && trapped == other.trapped && memory == other.memory;
    }
};

inline uint64_t interpret(Function& fn, std::vector<uint64_t> const& args,
                          std::map<uint64_t, uint64_t>& memory, unsigned depth = 0) {
    if (depth > 16) throw std::runtime_error("recursion limit");
    std::unordered_map<uint32_t, uint64_t> values;
    for (uint32_t i = 0; i < args.size(); ++i) values[i] = args[i] & limit(fn.param_types()[i]);
    std::function<uint64_t(Value)> read = [&](Value value) -> uint64_t {
        if (value.is_const()) {
            uint64_t result = 0;
            std::visit([&](auto v) { if constexpr (std::is_integral_v<decltype(v)>) result = static_cast<uint64_t>(v); }, fn.const_value(value.const_index()));
            return result & limit(fn.const_type(value.const_index()));
        }
        if (!value.is_vreg()) throw std::runtime_error("invalid value");
        auto it = values.find(value.vreg_index()); if (it != values.end()) return it->second;
        auto* def = fn.instruction_by_id(value.vreg_index());
        if (def && !def->parent() && def->opcode() == Opcode::Copy && def->operands().size() == 1) return read(def->operand(0));
        throw std::runtime_error("undefined SSA value");
    };
    Block* block = fn.entry_block(); Block* previous = nullptr;
    for (unsigned ticks = 0; block && ticks < 10000; ++ticks) {
        std::vector<std::pair<uint32_t, uint64_t>> phis;
        for (auto* inst : block->instructions()) if (inst->opcode() == Opcode::Phi) {
            bool found = false;
            for (std::size_t i = 0; i < inst->incoming_blocks().size(); ++i) if (inst->incoming_blocks()[i] == previous) {
                phis.emplace_back(inst->id(), read(inst->operand(2*i+1))); found = true; break;
            }
            if (!found) throw std::runtime_error("missing phi edge");
        }
        for (auto [id, value] : phis) values[id] = value;
        Block* next = nullptr;
        for (auto* inst : block->instructions()) {
            auto op = inst->opcode();
            if (op == Opcode::Phi) continue;
            if (op == Opcode::Ret) return inst->operands().empty() ? 0 : read(inst->operand(0));
            if (op == Opcode::Br) { next = block->successors().at(0); break; }
            if (op == Opcode::CondBr) { next = block->successors().at(read(inst->operand(0)) ? 0 : 1); break; }
            if (op == Opcode::Unreachable) throw std::runtime_error("unreachable");
            uint64_t result = 0;
            if (op == Opcode::Alloca) { result = 0x100000 + inst->id()*16 + depth*0x10000; values[inst->id()] = result; continue; }
            if (op == Opcode::Call) {
                auto* callee = fn.module()->find_function(inst->name());
                if (!callee) throw std::runtime_error("unknown call");
                std::vector<uint64_t> actual;
                for (std::size_t i = 1; i < inst->operands().size(); ++i) actual.push_back(read(inst->operand(i)));
                result = interpret(*callee, actual, memory, depth+1);
                if (!inst->type().is_void()) values[inst->id()] = result & limit(inst->type());
                continue;
            }
            auto a = inst->operands().empty() ? 0 : read(inst->operand(0));
            auto b = inst->operands().size() < 2 ? 0 : read(inst->operand(1));
            auto source = inst->operands().empty() ? inst->type() : fn.value_type(inst->operand(0));
            auto sa = signed_value(a, source), sb = signed_value(b, source);
            switch (op) {
                case Opcode::Copy: result = a; break;
                case Opcode::Add: result = a+b; break;
                case Opcode::Sub: result = a-b; break;
                case Opcode::Mul: result = a*b; break;
                case Opcode::Neg: result = uint64_t(0)-a; break;
                case Opcode::Not: result = ~a; break;
                case Opcode::And: result = a&b; break;
                case Opcode::Or: result = a|b; break;
                case Opcode::Xor: result = a^b; break;
                case Opcode::Shl: case Opcode::LShr: case Opcode::AShr:
                    if (b >= width(source)) throw std::runtime_error("shift trap");
                    if (op == Opcode::Shl) result = a<<b;
                    else if (op == Opcode::LShr) result = a>>b;
                    else {
                        // Floor division by 2^b without signed shifts or overflow.
                        result = sa >= 0 ? a>>b : ~(static_cast<uint64_t>(-(sa+1)) >> b);
                    }
                    break;
                case Opcode::SDiv: case Opcode::SRem:
                    if (!b || (sa == signed_value(uint64_t(1) << (width(source)-1), source) && sb == -1))
                        throw std::runtime_error("division trap");
                    result = static_cast<uint64_t>(op == Opcode::SDiv ? sa/sb : sa%sb); break;
                case Opcode::UDiv: case Opcode::URem:
                    if (!b) throw std::runtime_error("division trap");
                    result = op == Opcode::UDiv ? a/b : a%b; break;
                case Opcode::ICmpEq: result = a==b; break;
                case Opcode::ICmpNe: result = a!=b; break;
                case Opcode::ICmpSLt: result = sa<sb; break;
                case Opcode::ICmpSLe: result = sa<=sb; break;
                case Opcode::ICmpSGt: result = sa>sb; break;
                case Opcode::ICmpSGe: result = sa>=sb; break;
                case Opcode::ICmpULt: result = a<b; break;
                case Opcode::ICmpULe: result = a<=b; break;
                case Opcode::ICmpUGt: result = a>b; break;
                case Opcode::ICmpUGe: result = a>=b; break;
                case Opcode::Trunc: case Opcode::ZExt: case Opcode::IntToPtr: case Opcode::PtrToInt: result = a; break;
                case Opcode::SExt: result = static_cast<uint64_t>(sa); break;
                case Opcode::Select: result = a ? b : read(inst->operand(2)); break;
                case Opcode::Load:
                    if (!memory.count(a)) throw std::runtime_error("uninitialized load");
                    result = memory[a]; break;
                case Opcode::Store: memory[a] = b; continue;
                default: throw std::runtime_error("unsupported opcode");
            }
            values[inst->id()] = result & limit(inst->type());
        }
        previous = block; block = next;
    }
    throw std::runtime_error("execution limit");
}
inline Execution execute(Function& fn, std::vector<uint64_t> const& args) {
    Execution result; result.memory = {{0x1000, 17}, {0x2000, 23}};
    try { result.returned = interpret(fn, args, result.memory); }
    catch (std::runtime_error const&) { result.trapped = true; }
    // Stack slots are unobservable after return.
    for (auto it = result.memory.begin(); it != result.memory.end();)
        if (it->first >= 0x100000) it = result.memory.erase(it); else ++it;
    return result;
}

inline void verify(Function& fn) {
    std::unordered_set<Instruction*> storage, linked;
    for (auto& inst : fn.instr_storage()) storage.insert(&inst);
    std::unordered_set<Block*> blocks;
    for (auto& block : fn.blocks()) blocks.insert(block.get());
    std::unordered_map<Instruction*, std::size_t> position;
    for (auto& block : fn.blocks()) {
        for (std::size_t i = 0; i < block->instructions().size(); ++i) {
            auto* inst = block->instructions()[i]; CHECK(storage.count(inst)); CHECK(inst->parent() == block.get());
            CHECK(linked.insert(inst).second); position[inst] = i;
        }
        for (auto* succ : block->successors()) {
            CHECK(blocks.count(succ)); CHECK(std::count(succ->predecessors().begin(), succ->predecessors().end(), block.get()) == 1);
        }
        for (auto* pred : block->predecessors()) {
            CHECK(blocks.count(pred)); CHECK(std::find(pred->successors().begin(), pred->successors().end(), block.get()) != pred->successors().end());
        }
        CHECK(!block->instructions().empty()); auto* term = block->instructions().back();
        if (term->opcode() == Opcode::Br) CHECK(block->successors().size() == 1);
        else if (term->opcode() == Opcode::CondBr) CHECK(block->successors().size() == 2);
        else { CHECK(term->opcode() == Opcode::Ret || term->opcode() == Opcode::Unreachable); CHECK(block->successors().empty()); }
    }
    auto analysis = create_pass("dominator-analysis"); dynamic_cast<FunctionPass&>(*analysis).run(fn);
    auto const& dom = analysis->result()->dominators;
    for (auto& inst : fn.instr_storage()) {
        if (inst.parent()) CHECK(linked.count(&inst));
        else CHECK(inst.opcode() == Opcode::Copy && inst.operands().size() == 1 && inst.operand(0).is_const());
        if (inst.opcode() == Opcode::Phi) {
            CHECK(inst.incoming_blocks().size()*2 == inst.operands().size());
            CHECK(inst.incoming_blocks().size() == inst.parent()->predecessors().size());
            for (auto* pred : inst.incoming_blocks()) CHECK(std::find(inst.parent()->predecessors().begin(), inst.parent()->predecessors().end(), pred) != inst.parent()->predecessors().end());
        }
        for (std::size_t i = 0; i < inst.operands().size(); ++i) {
            if (inst.opcode() == Opcode::Br || inst.opcode() == Opcode::Alloca || inst.opcode() == Opcode::GetParam ||
                (inst.opcode() == Opcode::Call && i == 0) || (inst.opcode() == Opcode::Phi && i%2 == 0)) continue;
            auto value = inst.operand(i); CHECK(value.valid());
            if (value.is_const()) { CHECK(value.const_index() < fn.const_count()); continue; }
            if (value.vreg_index() < fn.param_count()) continue;
            auto* def = fn.instruction_by_id(value.vreg_index()); CHECK(def != nullptr);
            if (!def->parent() || !inst.parent()) continue;
            auto* use_block = inst.opcode() == Opcode::Phi ? inst.incoming_blocks()[i/2] : inst.parent();
            auto found = dom.find(use_block); if (found == dom.end()) continue;
            CHECK(found->second.count(def->parent()));
            if (use_block == def->parent() && inst.opcode() != Opcode::Phi) CHECK(position[def] < position[&inst]);
        }
        if (inst.opcode() == Opcode::Copy) CHECK(fn.value_type(inst.operand(0)) == inst.type());
    }
}
inline void run(Function& fn, std::string_view name) {
    auto pass = create_pass(name); dynamic_cast<FunctionPass&>(*pass).run(fn); verify(fn);
}
}
#endif
