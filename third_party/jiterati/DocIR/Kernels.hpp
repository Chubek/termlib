/** @file DocIR/Kernels.hpp
 *  @brief Document creation kernels: build DocIR from live Jiterati metadata.
 *
 *  Each kernel is a pure function returning a @ref jiterati::docir::Document:
 *
 *  - @ref kernel_module_reference() — function/block/instruction reference;
 *  - @ref kernel_backend_catalog() — every backend incl. ISA targets;
 *  - @ref kernel_pass_catalog() — pass pipeline reference;
 *  - @ref kernel_cli_reference() — CLI command reference;
 *  - @ref kernel_article() — free-form article wrapper.
 */
#ifndef JITERATI_DOCIR_KERNELS_HPP_INCLUDED
#define JITERATI_DOCIR_KERNELS_HPP_INCLUDED

#include "../include/Jiterati-DocIR.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace jiterati::docir::kernels {

/** Minimal function summary consumed by kernel_module_reference(). */
struct FunctionSummary {
    std::string name;
    std::string signature;
    std::size_t blocks = 0;
    std::size_t instructions = 0;
};

Document kernel_module_reference(std::string_view module_name,
                                 std::vector<FunctionSummary> const& functions);

Document kernel_backend_catalog();
Document kernel_pass_catalog();
Document kernel_cli_reference();

Document kernel_article(std::string_view title,
                        std::vector<std::string> const& paragraphs);

/** Serialize `doc` in the requested markup projection. */
std::string serialize(Document const& doc, std::string_view format);

} // namespace jiterati::docir::kernels

#endif // JITERATI_DOCIR_KERNELS_HPP_INCLUDED
