/** @file DocIR/Kernels.cpp
 *  @brief Document creation kernel implementations.
 */
#include "Kernels.hpp"
#include "Jiterati-Pass.hpp"

#include "../BE/ISA/IsaBackend.hpp"

#include <stdexcept>

namespace jiterati::docir::kernels {
namespace {

void add_section(Node& parent, std::string heading, std::string body) {
    auto section = Node::element("section");
    auto title = Node::element("heading");
    title->add_text(std::move(heading));
    auto para = Node::element("para");
    para->add_text(std::move(body));
    section->add(std::move(title));
    section->add(std::move(para));
    parent.add(std::move(section));
}

} // namespace

Document kernel_module_reference(std::string_view module_name,
                                 std::vector<FunctionSummary> const& functions) {
    Document doc("reference");
    doc.title("Module " + std::string(module_name)).lang("en");
    doc.root().attr("kind", "module-reference").attr("module", std::string(module_name));
    for (auto const& fn : functions) {
        auto item = Node::element("function");
        item->attr("name", fn.name).attr("signature", fn.signature);
        item->attr("blocks", std::to_string(fn.blocks));
        item->attr("instructions", std::to_string(fn.instructions));
        auto sig = Node::element("signature");
        sig->add_text(fn.signature + " @" + fn.name);
        item->add(std::move(sig));
        doc.root().add(std::move(item));
    }
    return doc;
}

Document kernel_backend_catalog() {
    Document doc("catalog");
    doc.title("Jiterati backend catalog").lang("en");
    doc.root().attr("kind", "backend-catalog");
    for (auto const& target : be::isa::all_isa_targets()) {
        auto entry = Node::element("backend");
        entry->attr("target", std::string(target.id));
        entry->attr("source", std::string(target.isa_file));
        entry->attr("arch", std::string(target.arch));
        entry->attr("operations", std::to_string(target.isa_operations));
        add_section(*entry, "Calling convention", std::string(target.calling_convention));
        add_section(*entry, "Object format", std::string(target.object_format));
        add_section(*entry, "Executable format", std::string(target.exec_format));
        add_section(*entry, "Features", std::string(target.features));
        add_section(*entry, "Limitations", std::string(target.limitations));
        doc.root().add(std::move(entry));
    }
    return doc;
}

Document kernel_pass_catalog() {
    Document doc("catalog");
    doc.title("Jiterati pass catalog").lang("en");
    doc.root().attr("kind", "pass-catalog");
    for (auto const& pass : available_passes()) {
        auto item = Node::element("pass");
        item->attr("name", std::string(pass.class_name));
        item->attr("pipeline", std::string(pass.name));
        item->attr("category", pass.analysis ? "analysis" : "transform");
        item->add_text(std::string(pass.description));
        doc.root().add(std::move(item));
    }
    return doc;
}

Document kernel_cli_reference() {
    Document doc("reference");
    doc.title("Jiterati CLI reference").lang("en");
    doc.root().attr("kind", "cli-reference");
    static const char* commands[] = {
        "compile|Validate IR and emit assembly, objects, executables, or bytecode",
        "parse|Parse textual IR and report a normalized summary",
        "validate|Validate textual IR",
        "format|Format textual IR",
        "package|Pack, unpack, validate, inspect, and install .jpkg archives",
        "plugin|List plugin metadata known to this build",
        "backend|List backend targets known to this build",
        "pass|List pass metadata known to this build",
        "disasm|Disassemble bytes via Capstone with a terminal view",
        "bytecode|Archive and run portable .jbc bytecode",
        "doc|Render DocIR document creation kernels",
        "config|Read and write simple user configuration",
        "cache|Inspect or clear the Jiterati cache directory",
        "doctor|Check local tool and home-directory configuration",
        "version|Print version information",
    };
    for (char const* entry : commands) {
        std::string text(entry);
        auto bar = text.find('|');
        auto item = Node::element("command");
        item->attr("name", text.substr(0, bar));
        item->add_text(text.substr(bar + 1));
        doc.root().add(std::move(item));
    }
    return doc;
}

Document kernel_article(std::string_view title,
                        std::vector<std::string> const& paragraphs) {
    Document doc("article");
    doc.title(std::string(title)).lang("en");
    doc.root().attr("kind", "article");
    auto heading = Node::element("heading");
    heading->add_text(std::string(title));
    doc.root().add(std::move(heading));
    for (auto const& paragraph : paragraphs) {
        auto para = Node::element("para");
        para->add_text(paragraph);
        doc.root().add(std::move(para));
    }
    return doc;
}

std::string serialize(Document const& doc, std::string_view format) {
    if (format == "xtas" || format == "xml") return doc.to_xtas();
    if (format == "html") return doc.to_html();
    throw std::runtime_error("unknown DocIR format: " + std::string(format));
}

} // namespace jiterati::docir::kernels
