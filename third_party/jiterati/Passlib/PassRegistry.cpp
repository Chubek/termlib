#include "PassSupport.hpp"

namespace jiterati {
#define JITERATI_PASS(CLASS, NAME, ANALYSIS, DESCRIPTION) std::unique_ptr<Pass> make_##CLASS();
#include "Passes.def"
#undef JITERATI_PASS

std::vector<PassInfo> const& available_passes() {
    static const std::vector<PassInfo> catalog = {
#define JITERATI_PASS(CLASS, NAME, ANALYSIS, DESCRIPTION) {NAME, #CLASS, DESCRIPTION, ANALYSIS},
#include "Passes.def"
#undef JITERATI_PASS
    };
    return catalog;
}

std::unique_ptr<Pass> create_pass(std::string_view name) {
    if (name == "cfg") name = "cfg-analysis";
    if (name == "dce") name = "dead-code-elimination";
    if (name == "dom") name = "dominator-analysis";
    if (name == "liveness") name = "liveness-analysis";
    if (name == "auto-vectorize") name = "autovectorize";
#define JITERATI_PASS(CLASS, NAME, ANALYSIS, DESCRIPTION) \
    if (name == NAME || name == #CLASS) return make_##CLASS();
#include "Passes.def"
#undef JITERATI_PASS
    throw std::invalid_argument("unknown optimization pass: " + std::string(name));
}
} // namespace jiterati
