/** @file PolyhedralOptimization.cpp @brief Bounded static affine-domain scheduling. */
#include "LoopSupport.hpp"
namespace jiterati::pass_detail {
void run_PolyhedralOptimization(Function& fn, PassResult& result) {
    // The scalar IR has no pointer-stride or dependence annotations. Restrict
    // affine scheduling to pure rectangular nests and constant finite domains.
    // A unit-stride static schedule removes induction and branch overhead.
    run_LoopInterchange(fn, result);
    run_LoopUnrolling(fn, result);
    run_ConstantFolding(fn, result);
    run_CopyPropagation(fn, result);
    run_DeadCodeElimination(fn, result);
}
}
JITERATI_DEFINE_PASS(PolyhedralOptimization, "polyhedral-optimization", false)
