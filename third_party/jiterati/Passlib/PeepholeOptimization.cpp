/** @file PeepholeOptimization.cpp @brief Local simplification pipeline. */
#include "PassSupport.hpp"
namespace jiterati::pass_detail {
void run_PeepholeOptimization(Function& fn, PassResult& result) {
    run_AlgebraicSimplification(fn, result);
    run_SelectSimplification(fn, result);
    run_PhiSimplification(fn, result);
    run_CopyPropagation(fn, result);
}
}
JITERATI_DEFINE_PASS(PeepholeOptimization, "peephole-optimization", false)
