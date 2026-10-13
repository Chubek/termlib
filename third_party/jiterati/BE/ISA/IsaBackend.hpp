/** @file BE/ISA/IsaBackend.hpp
 *  @brief Generic `third_party/isa`-driven backends for every ISA target.
 *
 *  Each `.isa` source in `third_party/isa` describes registers, encodings and
 *  operation forms for one architecture. @ref IsaDatabase parses that text
 *  (C++17, no dependency on the C++20 `simple_isa.hpp`) and @ref IsaBackend
 *  lowers Jiterati IR to assembly rendered from the database's syntax
 *  templates. Targets with binary encodings additionally feed
 *  Jiterati-Asm.hpp (Keystone assemble + ELF link).
 *
 *  New targets are added by extending @ref all_isa_targets(); no per-target
 *  C++ is required.
 */
#ifndef JITERATI_BE_ISA_BACKEND_HPP_INCLUDED
#define JITERATI_BE_ISA_BACKEND_HPP_INCLUDED

#include "../../include/Jiterati-BE.hpp"
#include "../../IR/IR.hpp"
#include "../../IR/ICompiler.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jiterati::be::isa {

/** Static descriptor for one ISA target. */
struct IsaTarget {
    std::string_view id;            ///< CLI/backend id, e.g. "arm32".
    std::string_view isa_file;      ///< File under third_party/isa.
    std::string_view arch;          ///< Arch name inside the .isa source.
    std::string_view calling_convention;
    std::string_view object_format;
    std::string_view exec_format;
    std::string_view features;      ///< Supported extensions, comma separated.
    std::string_view limitations;   ///< Known gaps.
    std::size_t isa_operations = 0; ///< Operation forms in the .isa source.
    bool text_only = false;         ///< No target-independent binary opcode.
};

/** All ISA-driven targets (existing 4 aliases + 16 new). */
std::vector<IsaTarget> const& all_isa_targets();
/** Find a descriptor by backend id; nullptr when unknown. */
IsaTarget const* find_isa_target(std::string_view id);

/** One parsed register. */
struct IsaRegister {
    std::string name;
    std::string regclass;
    unsigned width = 0;
    unsigned number = 0;
};

/** One parsed operation form. */
struct IsaOperation {
    std::string name;
    std::string category;
    std::string encoding;
    std::string syntax;
    std::string operands;
};

/** Parsed `.isa` database with lightweight query API. */
class IsaDatabase {
public:
    std::string arch;
    unsigned word_size = 0;
    unsigned addr_size = 0;
    std::string endian;
    unsigned align = 1;
    std::vector<IsaRegister> registers;
    std::vector<IsaOperation> operations;
    std::map<std::string, std::string> encodings;

    /** Load and parse `path`; throws IsaError on malformed input. */
    static IsaDatabase load_file(std::string const& path);

    /** Load the database backing `target_id` (searches known roots). */
    static IsaDatabase for_target(std::string_view target_id);

    /** Locate the .isa source file for a target; empty when absent. */
    static std::string resolve_source(std::string_view target_id);

    IsaOperation const* find_operation(std::string_view name) const;
    std::vector<IsaRegister> registers_of_class(std::string_view cls) const;

    /** Render `op` with `{placeholder}` substitution from `operands`. */
    std::string render(std::string_view op,
                       std::map<std::string, std::string> const& operands) const;
};

class IsaError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Generic backend rendering Jiterati IR through an IsaDatabase. */
class IsaBackend final : public Backend {
public:
    explicit IsaBackend(IsaTarget target);

    std::string target_id() const override { return std::string(target_.id); }
    IsaTarget const& descriptor() const { return target_; }
    IsaDatabase const& database() const { return database_; }

    std::unique_ptr<CompiledFunction> compile_function(Function& fn) override;
    std::string emit_assembly(Function& fn) override;

    std::unique_ptr<AssemblerContext> create_assembler_context() override;
    ABI& abi() override;
    RegAllocator& allocator() override;

private:
    IsaTarget target_;
    IsaDatabase database_;
};

/** Create a backend for any ISA target id (nullptr when unknown). */
std::unique_ptr<Backend> create_isa_backend(std::string_view target_id);

/** Create a TAC-level IR compiler emitting this target's assembly. */
std::unique_ptr<ir::ICompiler> create_isa_ir_compiler(std::string_view target_id);

/** Render a TAC module to assembly text for `target_id`. */
std::string emit_tac_assembly(ir::TACModule const& module, std::string_view target_id);

} // namespace jiterati::be::isa

#endif // JITERATI_BE_ISA_BACKEND_HPP_INCLUDED
