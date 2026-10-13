/** @file DominatorAnalysis.cpp @brief Reachability-aware dominator fixed point. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_DominatorAnalysis(Function& fn, PassResult& result) { result.dominators = dominators(fn); }
}
JITERATI_DEFINE_PASS(DominatorAnalysis, "dominator-analysis", true)
