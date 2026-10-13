/** @file ConstantFolding.cpp @brief UB-free, width-aware integer folding. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_ConstantFolding(Function& fn, PassResult&) {
    for (auto* inst : snapshot(fn)) {
        if (!inst->type().is_int() || inst->operands().empty()) continue;
        auto a = integer(fn, inst->operand(0));
        auto b = inst->operands().size() > 1 ? integer(fn, inst->operand(1)) : std::nullopt;
        if (!a) continue;
        Type source = fn.value_type(inst->operand(0));
        if (!source.is_int()) continue;
        unsigned width = bits(source);
        int64_t sa = signed_bits(*a, source), sb = b ? signed_bits(*b, source) : 0;
        std::optional<uint64_t> result;
        bool division_overflow = b && sa == signed_bits(uint64_t(1) << (width-1), source) && sb == -1;
        switch (inst->opcode()) {
            case Opcode::Add: if (b) result = *a + *b; break;
            case Opcode::Sub: if (b) result = *a - *b; break;
            case Opcode::Mul: if (b) result = *a * *b; break;
            case Opcode::SDiv: if (b && *b && !division_overflow) result = static_cast<uint64_t>(sa / sb); break;
            case Opcode::SRem: if (b && *b && !division_overflow) result = static_cast<uint64_t>(sa % sb); break;
            case Opcode::UDiv: if (b && *b) result = *a / *b; break;
            case Opcode::URem: if (b && *b) result = *a % *b; break;
            case Opcode::And: if (b) result = *a & *b; break;
            case Opcode::Or: if (b) result = *a | *b; break;
            case Opcode::Xor: if (b) result = *a ^ *b; break;
            case Opcode::Neg: result = uint64_t(0) - *a; break;
            case Opcode::Not: result = ~*a; break;
            case Opcode::Shl: if (b && *b < width) result = *a << *b; break;
            case Opcode::LShr: if (b && *b < width) result = *a >> *b; break;
            case Opcode::AShr:
                if (b && *b < width) {
                    uint64_t shifted = *a >> *b;
                    if (sa < 0 && *b) shifted |= mask(source) ^ (mask(source) >> *b);
                    result = shifted;
                }
                break;
            case Opcode::ICmpEq: if (b) result = *a == *b; break;
            case Opcode::ICmpNe: if (b) result = *a != *b; break;
            case Opcode::ICmpSLt: if (b) result = sa < sb; break;
            case Opcode::ICmpSLe: if (b) result = sa <= sb; break;
            case Opcode::ICmpSGt: if (b) result = sa > sb; break;
            case Opcode::ICmpSGe: if (b) result = sa >= sb; break;
            case Opcode::ICmpULt: if (b) result = *a < *b; break;
            case Opcode::ICmpULe: if (b) result = *a <= *b; break;
            case Opcode::ICmpUGt: if (b) result = *a > *b; break;
            case Opcode::ICmpUGe: if (b) result = *a >= *b; break;
            case Opcode::Trunc: case Opcode::ZExt: result = *a; break;
            case Opcode::SExt: result = static_cast<uint64_t>(sa); break;
            default: break;
        }
        if (result) copy(*inst, literal(fn, inst->type(), *result));
    }
}
}
JITERATI_DEFINE_PASS(ConstantFolding, "constant-folding", false)
