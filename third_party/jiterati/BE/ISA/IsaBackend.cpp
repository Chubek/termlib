/** @file BE/ISA/IsaBackend.cpp
 *  @brief Implementation of the ISA-driven generic backend.
 */
#include "IsaBackend.hpp"

#include "../../include/Jiterati-Asm.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace jiterati::be::isa {
namespace {

namespace fs = std::filesystem;

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return std::string(text.substr(begin, end - begin));
}

std::string strip_comment(std::string_view line) {
    bool in_string = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') in_string = !in_string;
        if (line[i] == '#' && !in_string) return std::string(line.substr(0, i));
    }
    return std::string(line);
}

std::string unquote(std::string value) {
    value = trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        return value.substr(1, value.size() - 2);
    return value;
}

void parse_arch_header(IsaDatabase& db, std::string_view line) {
    // arch NAME { k = v; k = v; }
    auto open = line.find('{');
    auto close = line.rfind('}');
    std::string head = open == std::string_view::npos
                           ? std::string(line)
                           : std::string(line.substr(0, open));
    std::istringstream words(head);
    std::string keyword, name;
    words >> keyword >> name;
    db.arch = trim(name);
    if (open == std::string_view::npos || close == std::string_view::npos ||
        close <= open)
        return;
    std::string body(line.substr(open + 1, close - open - 1));
    std::istringstream fields(body);
    std::string field;
    while (std::getline(fields, field, ';')) {
        auto eq = field.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(field.substr(0, eq));
        std::string value = trim(field.substr(eq + 1));
        if (key == "word_size") db.word_size = static_cast<unsigned>(std::stoul(value));
        else if (key == "addr_size") db.addr_size = static_cast<unsigned>(std::stoul(value));
        else if (key == "endian") db.endian = value;
        else if (key == "align") db.align = static_cast<unsigned>(std::stoul(value));
    }
}

void parse_reg_entry(IsaDatabase& db, std::string const& regclass, std::string entry) {
    // name(width)=number  (trailing comma tolerated)
    entry = trim(entry);
    if (!entry.empty() && entry.back() == ',') entry.pop_back();
    auto paren = entry.find('(');
    auto eq = entry.find('=');
    if (paren == std::string::npos || eq == std::string::npos) return;
    IsaRegister reg;
    reg.name = trim(entry.substr(0, paren));
    reg.regclass = regclass;
    auto close = entry.find(')', paren);
    reg.width = static_cast<unsigned>(std::stoul(trim(entry.substr(paren + 1, close - paren - 1))));
    reg.number = static_cast<unsigned>(std::stoul(trim(entry.substr(eq + 1))));
    if (!reg.name.empty()) db.registers.push_back(std::move(reg));
}

void parse_op_body(IsaOperation& op, std::string const& body) {
    // Split on ';' outside double-quoted strings (PTX syntax embeds ';').
    std::string field;
    bool in_string = false;
    auto flush = [&] {
        auto eq = field.find('=');
        if (eq != std::string::npos) {
            std::string key = trim(field.substr(0, eq));
            std::string value = unquote(trim(field.substr(eq + 1)));
            if (key == "class") op.category = value;
            else if (key == "encoding") op.encoding = value;
            else if (key == "syntax") op.syntax = value;
            else if (key == "operands") op.operands = value;
        }
        field.clear();
    };
    for (char c : body) {
        if (c == '"') in_string = !in_string;
        if (c == ';' && !in_string) flush();
        else field += c;
    }
    flush();
}

/// Canonical operation aliases: the first form present in the database wins.
/// Covers targets whose .isa sources spell arithmetic with width/variant
/// suffixes (loongarch `add_w`, nvptx `add_u32`, amdgpu `s_add_u32`, ...).
std::string resolve_op(IsaDatabase const& db, std::string const& want) {
    if (db.find_operation(want)) return want;
    static const std::pair<const char*, const char*> candidates[] = {
        {"add", "add_w"}, {"add", "add_u32"}, {"add", "s_add_u32"},
        {"add", "s_add_i32"}, {"add", "iadd"},
        {"sub", "sub_w"}, {"sub", "sub_u32"}, {"sub", "s_sub_u32"},
        {"sub", "s_sub_i32"}, {"sub", "isub"},
        {"mul", "mul_w"}, {"mul", "mul_lo_u32"}, {"mul", "imul"},
        {"div", "div_w"}, {"div", "div_u32"}, {"div", "sdiv"},
        {"and", "and_b32"}, {"or", "or_b32"},
        {"mov", "li"},
        {"load", "ld"}, {"store", "st"},
    };
    for (auto const& [from, to] : candidates) {
        if (want == from && db.find_operation(to)) return to;
    }
    return want;
}

/// Map core IR opcodes to conventional isa operation names.
std::string core_op_name(Opcode op) {
    switch (op) {
        case Opcode::Add: return "add";
        case Opcode::Sub: return "sub";
        case Opcode::Mul: return "mul";
        case Opcode::SDiv: return "div";
        case Opcode::UDiv: return "divu";
        case Opcode::SRem: return "rem";
        case Opcode::URem: return "remu";
        case Opcode::Neg: return "neg";
        case Opcode::And: return "and";
        case Opcode::Or: return "or";
        case Opcode::Xor: return "xor";
        case Opcode::Shl: return "sll";
        case Opcode::LShr: return "srl";
        case Opcode::AShr: return "sra";
        case Opcode::Not: return "not";
        case Opcode::ICmpEq: return "seq";
        case Opcode::ICmpNe: return "sne";
        case Opcode::ICmpSLt: return "slt";
        case Opcode::ICmpSLe: return "sle";
        case Opcode::ICmpSGt: return "sgt";
        case Opcode::ICmpSGe: return "sge";
        case Opcode::Ret: return "ret";
        case Opcode::Br: return "j";
        case Opcode::CondBr: return "br";
        case Opcode::Call: return "call";
        case Opcode::Load: return "load";
        case Opcode::Store: return "store";
        case Opcode::Copy: return "mov";
        case Opcode::Select: return "select";
        default: return opcode_name(op);
    }
}

/// Map TAC opcodes to conventional isa operation names.
std::string tac_op_name(ir::TACOpcode op) {
    using TOC = ir::TACOpcode;
    switch (op) {
        case TOC::Add: return "add";
        case TOC::Sub: return "sub";
        case TOC::Mul: return "mul";
        case TOC::SDiv: return "div";
        case TOC::UDiv: return "divu";
        case TOC::SRem: return "rem";
        case TOC::URem: return "remu";
        case TOC::Neg: return "neg";
        case TOC::And: return "and";
        case TOC::Or: return "or";
        case TOC::Xor: return "xor";
        case TOC::Shl: return "sll";
        case TOC::LShr: return "srl";
        case TOC::AShr: return "sra";
        case TOC::Not: return "not";
        case TOC::Eq: return "seq";
        case TOC::Ne: return "sne";
        case TOC::SLt: return "slt";
        case TOC::SLe: return "sle";
        case TOC::SGt: return "sgt";
        case TOC::SGe: return "sge";
        case TOC::ULt: return "sltu";
        case TOC::ULe: return "sleu";
        case TOC::UGt: return "sgtu";
        case TOC::UGe: return "sgeu";
        case TOC::Select: return "select";
        case TOC::Call: return "call";
        case TOC::Return: return "ret";
        case TOC::Branch: return "j";
        case TOC::CondBranch: return "br";
        case TOC::Move: return "mov";
        case TOC::Const: return "li";
        default: return ir::tac_opcode_str(op);
    }
}

class ByteBufferAssembler final : public AssemblerContext {
public:
    void emit_bytes(void const* data, std::size_t len) override {
        auto const* bytes = static_cast<unsigned char const*>(data);
        buffer_.insert(buffer_.end(), bytes, bytes + len);
    }
    std::size_t offset() const override { return buffer_.size(); }
    void bind_label(uint32_t) override {}
    void reloc_label(uint32_t, std::ptrdiff_t) override {}
    void reloc_function(std::string const&, std::ptrdiff_t) override {}

private:
    std::vector<unsigned char> buffer_;
};

class GenericABI final : public ABI {
public:
    int num_arg_regs() const override { return 8; }
    int arg_reg(std::size_t i) const override { return i < 8 ? static_cast<int>(i) : -1; }
    int ret_reg() const override { return 0; }
    std::set<int> callee_saved_regs() const override { return {}; }
    std::set<int> caller_saved_regs() const override { return {0, 1, 2, 3, 4, 5, 6, 7}; }
    std::size_t stack_alignment() const override { return 16; }
};

class GenericAllocator final : public RegAllocator {
public:
    std::vector<int> allocate(Function&, int num_physical_regs,
                              std::vector<int>* out_spill_slots) override {
        std::vector<int> result(64, -1);
        for (std::size_t i = 0; i < result.size() && i < static_cast<std::size_t>(num_physical_regs); ++i)
            result[i] = static_cast<int>(i);
        if (out_spill_slots) out_spill_slots->assign(result.size(), -1);
        return result;
    }
};

} // namespace

std::vector<IsaTarget> const& all_isa_targets() {
    static const std::vector<IsaTarget> targets = {
        {"amd64", "x86.isa", "amd64", "System V AMD64", "ELF64", "ELF64 executable",
         "GPR/FPR, legacy+REX forms", "AVX/AVX-512 forms via xsimd only", 144, false},
        {"aarch64", "aarch64.isa", "aarch64", "AAPCS64", "ELF64", "ELF64 executable",
         "GPR/FPR/SIMD, LSE", "SVE streaming forms partial", 315, false},
        {"arm32", "arm32.isa", "arm32", "AAPCS32/EABI", "ELF32", "ELF32 executable",
         "ARMv7-A GPR, Thumb interwork", "MVE/NEON partial", 114, false},
        {"rv64", "riscv64.isa", "riscv64", "LP64D", "ELF64", "ELF64 executable",
         "RV64GC+Zba/Zbb/Zbs,Zicond", "sign/zero-extend pseudos expand to pairs", 252, false},
        {"riscv32", "riscv32.isa", "riscv32", "ILP32D", "ELF32", "ELF32 executable",
         "RV32GC", "privileged forms minimal", 199, false},
        {"mips32", "mips32.isa", "mips32", "O32", "ELF32", "ELF32 executable",
         "MIPS32r2 GPR", "BAL omitted (dual-field encoding)", 62, false},
        {"mips64", "mips64.isa", "mips64", "N64", "ELF64", "ELF64 executable",
         "MIPS64r2 GPR", "BAL omitted (dual-field encoding)", 84, false},
        {"ppc32", "ppc32.isa", "ppc32", "SysV PPC32", "ELF32", "ELF32 executable",
         "GPR/FPR, branches", "Altivec partial", 64, false},
        {"ppc64", "ppc64.isa", "ppc64", "ELFv2", "ELF64", "ELF64 executable",
         "GPR/FPR/VSX subset", "HTM omitted", 90, false},
        {"s390x", "s390x.isa", "s390x", "s390x ELF", "ELF64", "ELF64 executable",
         "GPR/AR/FPR subset", "vector facility partial", 70, false},
        {"sparc64", "sparc64.isa", "sparc64", "SysV SPARC64", "ELF64", "ELF64 executable",
         "GPR/FPR, windowed ABI", "VIS partial", 107, false},
        {"loongarch64", "loongarch64.isa", "loongarch64", "LP64D", "ELF64", "ELF64 executable",
         "GPR/FPR/LSX subset", "LASX partial", 148, false},
        {"loongarch32", "loongarch32.isa", "loongarch32", "ILP32D", "ELF32", "ELF32 executable",
         "GPR base", "FPU subset", 91, false},
        {"m68k", "m68k.isa", "m68k", "m68k ELF", "ELF32", "ELF32 executable",
         "D/A registers", "FPU/MMU partial", 104, false},
        {"avr", "avr.isa", "avr", "avr-gcc", "ELF32-avr", "ELF32-avr executable",
         "8-bit GPR pairs", "XMEGA/SPM partial", 68, false},
        {"hexagon", "hexagon.isa", "hexagon", "QuRT", "ELF32-hexagon", "ELF32 executable",
         "single-instruction V60 packets", "multi-packet bundles not encoded", 26, false},
        {"bpf", "bpf.isa", "bpf", "BPF CO-RE", "BPF object", "BPF program",
         "ALU/JMP/MEM, XADD", "kfunc set minimal", 138, false},
        {"amdgpu", "amdgpu.isa", "amdgpu", "AMDGPU PAL", "ELF64-amdgpu", "HSACO",
         "GCN gfx900 subset", "SDWA/DPP modifiers partial", 139, false},
        {"nvptx", "nvptx.isa", "nvptx", "PTX calling", ".ptx text", ".cubin via ptxas",
         "integer/branches/predicates", "text-only: encode() n/a", 42, true},
        {"spirv", "spirv.isa", "spirv", "SPIR-V logical", ".spv binary", ".spv module",
         "core arithmetic/control", "module header built separately", 873, false},
        {"tricore", "tricore.isa", "tricore", "Tasking/HighTec", "ELF32-tricore", "ELF32 executable",
         "D/A registers, CSA", "text-only PCP forms", 31, true},
        {"wasm", "wasm.isa", "wasm", "WASM MVP", ".wasm binary", ".wasm module",
         "MVP+SIMD128", "module constructed separately", 200, false},
    };
    return targets;
}

IsaTarget const* find_isa_target(std::string_view id) {
    for (auto const& target : all_isa_targets()) {
        if (target.id == id) return &target;
        // Aliases for the historic backend ids.
        if (id == "riscv64" && target.id == "rv64") return &target;
        if (id == "rv32" && target.id == "riscv32") return &target;
        if (id == "x86_64" && target.id == "amd64") return &target;
        if (id == "x86" && target.id == "amd64") return &target;
        if (id == "arm64" && target.id == "aarch64") return &target;
    }
    return nullptr;
}

std::string IsaDatabase::resolve_source(std::string_view target_id) {
    IsaTarget const* target = find_isa_target(target_id);
    if (!target) return {};
    std::string file(target->isa_file);
    if (char const* env = std::getenv("JITERATI_ISA_DIR")) {
        fs::path candidate = fs::path(env) / file;
        if (fs::exists(candidate)) return candidate.string();
    }
    static const char* roots[] = {
        "third_party/isa",
        "../third_party/isa",
        "jiterati/third_party/isa",
        "/mnt/lambda/libstone/jiterati/third_party/isa",
        "/usr/share/jiterati/isa",
    };
    for (char const* root : roots) {
        fs::path candidate = fs::path(root) / file;
        if (fs::exists(candidate)) return candidate.string();
    }
    return {};
}

IsaDatabase IsaDatabase::load_file(std::string const& path) {
    std::ifstream input(path);
    if (!input) throw IsaError("cannot open .isa source: " + path);
    IsaDatabase db;
    std::string raw, logical;
    std::string section; // "", "regclass:NAME", "encoding:NAME", "op:NAME"
    IsaOperation pending_op;
    auto flush_op = [&] {
        if (!pending_op.name.empty()) {
            db.operations.push_back(pending_op);
            pending_op = IsaOperation{};
        }
    };
    auto handle = [&](std::string line) {
        line = strip_comment(line);
        line = trim(line);
        if (line.empty()) return;
        if (line.rfind("arch ", 0) == 0) {
            flush_op();
            section.clear();
            parse_arch_header(db, line);
            return;
        }
        if (line.rfind("regclass ", 0) == 0) {
            flush_op();
            auto name_begin = line.size() > 9 ? 9 : line.size();
            auto brace = line.find('{');
            section = "regclass:" + trim(line.substr(name_begin, brace - name_begin));
            return;
        }
        if (line.rfind("encoding ", 0) == 0) {
            flush_op();
            auto brace = line.find('{');
            std::string name = trim(line.substr(9, brace == std::string::npos
                                                        ? std::string::npos
                                                        : brace - 9));
            section = "encoding:" + name;
            db.encodings[name] = line;
            return;
        }
        if (line.rfind("op ", 0) == 0) {
            flush_op();
            auto brace = line.find('{');
            pending_op.name = trim(line.substr(3, brace == std::string::npos
                                                        ? std::string::npos
                                                        : brace - 3));
            section = "op";
            if (brace != std::string::npos) {
                std::string rest = trim(line.substr(brace + 1));
                if (!rest.empty() && rest.back() == '}') {
                    rest.pop_back();
                    parse_op_body(pending_op, rest);
                    flush_op();
                    section.clear();
                } else if (!rest.empty()) {
                    parse_op_body(pending_op, rest);
                }
            }
            return;
        }
        if (line == "}" || line == "},") {
            flush_op();
            section.clear();
            return;
        }
        if (section.rfind("regclass:", 0) == 0) {
            std::istringstream entries(line);
            std::string entry;
            while (std::getline(entries, entry, ',')) {
                if (!trim(entry).empty())
                    parse_reg_entry(db, section.substr(9), entry + ",");
            }
            return;
        }
        if (section == "op") {
            std::string body = line;
            bool closes = false;
            if (!body.empty() && body.back() == '}') {
                body.pop_back();
                closes = true;
            }
            parse_op_body(pending_op, body);
            if (closes) {
                flush_op();
                section.clear();
            }
            return;
        }
    };
    while (std::getline(input, raw)) {
        // Accumulate until braces balance for arch one-liners; otherwise per line.
        logical += raw;
        handle(logical);
        logical.clear();
    }
    flush_op();
    return db;
}

IsaDatabase IsaDatabase::for_target(std::string_view target_id) {
    std::string source = resolve_source(target_id);
    if (!source.empty()) {
        try {
            return load_file(source);
        } catch (IsaError const&) {
            // Fall through to the static fallback below.
        }
    }
    IsaDatabase db;
    IsaTarget const* target = find_isa_target(target_id);
    db.arch = target ? std::string(target->arch) : std::string(target_id);
    db.endian = "little";
    return db;
}

IsaOperation const* IsaDatabase::find_operation(std::string_view name) const {
    for (auto const& op : operations) {
        if (op.name == name) return &op;
    }
    return nullptr;
}

std::vector<IsaRegister> IsaDatabase::registers_of_class(std::string_view cls) const {
    std::vector<IsaRegister> out;
    for (auto const& reg : registers) {
        if (reg.regclass == cls) out.push_back(reg);
    }
    return out;
}

std::string IsaDatabase::render(std::string_view op,
                                std::map<std::string, std::string> const& operands) const {
    IsaOperation const* form = find_operation(op);
    std::string syntax = form ? form->syntax : std::string(op);
    if (syntax.empty()) syntax = std::string(op);
    // Accept both `{rd}` and PTX-style `%{rd}` placeholders.
    for (std::size_t pos = 0; (pos = syntax.find("%{", pos)) != std::string::npos;)
        syntax.replace(pos, 2, "{");
    std::string out;
    for (std::size_t i = 0; i < syntax.size(); ++i) {
        if (syntax[i] == '{') {
            auto close = syntax.find('}', i);
            if (close == std::string::npos) {
                out += syntax.substr(i);
                break;
            }
            std::string key = syntax.substr(i + 1, close - i - 1);
            auto it = operands.find(key);
            out += it == operands.end() ? ("{" + key + "}") : it->second;
            i = close;
        } else {
            out += syntax[i];
        }
    }
    return out;
}

IsaBackend::IsaBackend(IsaTarget target) : target_(target), database_(IsaDatabase::for_target(target.id)) {}

std::unique_ptr<CompiledFunction> IsaBackend::compile_function(Function& fn) {
    (void)fn;
    throw std::runtime_error("isa backend '" + std::string(target_.id) +
                             "': executable JIT lowering is host-only; use "
                             "emit_assembly() + Keystone, or the bytecode archive");
}

std::string IsaBackend::emit_assembly(Function& fn) {
    std::ostringstream out;
    out << "; jiterati isa backend: target=" << target_.id
        << " arch=" << database_.arch << " source=" << target_.isa_file << "\n";
    for (auto const& block : fn.blocks()) {
        out << block->name() << ":\n";
        for (auto const* inst : block->instructions()) {
            std::string want = resolve_op(database_, core_op_name(inst->opcode()));
            std::map<std::string, std::string> operands;
            if (!inst->operands().empty()) {
                operands["rd"] = inst->operands()[0].to_string();
                if (inst->operands().size() > 1)
                    operands["rs1"] = inst->operands()[1].to_string();
                if (inst->operands().size() > 2)
                    operands["rs2"] = inst->operands()[2].to_string();
                operands["rs"] = inst->operands()[0].to_string();
            }
            if (database_.find_operation(want))
                out << "  " << database_.render(want, operands) << "\n";
            else
                out << "  ; " << opcode_name(inst->opcode()) << " (no '" << want
                    << "' form in " << target_.isa_file << ")\n";
        }
    }
    return out.str();
}

std::unique_ptr<AssemblerContext> IsaBackend::create_assembler_context() {
    return std::make_unique<ByteBufferAssembler>();
}

ABI& IsaBackend::abi() {
    static GenericABI instance;
    return instance;
}

RegAllocator& IsaBackend::allocator() {
    static GenericAllocator instance;
    return instance;
}

std::unique_ptr<Backend> create_isa_backend(std::string_view target_id) {
    IsaTarget const* target = find_isa_target(target_id);
    if (!target) return nullptr;
    return std::make_unique<IsaBackend>(*target);
}

std::string emit_tac_assembly(ir::TACModule const& module, std::string_view target_id) {
    IsaTarget const* target = find_isa_target(target_id);
    IsaDatabase db = IsaDatabase::for_target(target_id);
    std::ostringstream out;
    out << "; module " << module.name << " target=" << target_id;
    if (target) out << " source=" << target->isa_file;
    out << "\n";
    for (auto const& fn : module.functions) {
        out << "\n" << fn.name << ":\n";
        for (auto const& block : fn.blocks) {
            out << "." << block.label() << ":\n";
            for (auto const& inst : block.instructions) {
                std::string want = resolve_op(db, tac_op_name(inst.opcode));
                std::map<std::string, std::string> operands;
                for (std::size_t i = 0; i < inst.operands.size() && i < 3; ++i) {
                    operands[i == 0 ? "rd" : (i == 1 ? "rs1" : "rs2")] =
                        inst.operands[i].str();
                }
                if (db.find_operation(want))
                    out << "  " << db.render(want, operands) << "\n";
                else
                    out << "  ; " << inst.str() << "\n";
            }
        }
    }
    return out.str();
}

namespace {
// TAC-level compiler adapter routing through the ISA database.
class IsaIrCompiler final : public ir::ICompiler {
public:
    explicit IsaIrCompiler(IsaTarget target) : target_(target) {}
    std::string target() const override { return std::string(target_.id); }
    ir::ArtifactFormat format() const override { return ir::ArtifactFormat::Assembly; }
    ir::CompilationArtifact compile(ir::TACModule const& module,
                                    ir::CompileOptions options) override {
        ir::TACModule work = module;
        if (options.optimize_tac) ir::peephole(work);
        ir::CompilationArtifact artifact{target(), format()};
        if (options.verify_input) artifact.diagnostics = ir::validate(work);
        if (!artifact.ok()) return artifact;
        artifact.text = emit_tac_assembly(work, target_.id);
        artifact.bytes.assign(artifact.text.begin(), artifact.text.end());
        return artifact;
    }

private:
    IsaTarget target_;
};
} // namespace

std::unique_ptr<ir::ICompiler> create_isa_ir_compiler(std::string_view target_id) {
    IsaTarget const* target = find_isa_target(target_id);
    if (!target) return nullptr;
    return std::make_unique<IsaIrCompiler>(*target);
}

} // namespace jiterati::be::isa
