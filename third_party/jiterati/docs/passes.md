# Passlib: executable optimization catalog

`Passlib` contains 55 registered passes: 5 analyses and 50 transformations.
Each pass has a separate `.cpp` translation unit. `Passes.def` is the factory
catalog; `Passlib/CMakeLists.txt` explicitly includes every implementation.

## IR boundary

- Inputs are `jiterati::Function`/`jiterati::Module` from `include/Jiterati.hpp`.
- `jiterati::ir::TACFunction` is a separate representation; its existing
  `ir::peephole` pipeline is independent of Passlib.
- Instruction IDs remain stable. Rewrites retain names and parent pointers.
- Deletion removes instructions from both block lists and function storage.
- CFG edges are ordered: conditional successor 0 is true, successor 1 is false.
  Equal targets still occupy both successor positions.
- `Instruction::incoming_blocks()` records Phi predecessor order; operands
  remain `(label-placeholder, value)` pairs for API compatibility.
- Branch labels, Phi label placeholders, call symbols and allocation metadata
  are excluded from SSA use rewriting and liveness.

## API and pipelines

```cpp
#include "Jiterati-Pass.hpp"

auto pipeline = jiterati::parse_jpl_pipeline(
    "commutative-canonicalization; constant-propagation; "
    "algebraic-simplification; strength-reduction; "
    "common-subexpression-elimination; control-flow-optimization; dce");
pipeline.run(module);
```

Whitespace and semicolons delimit names. Tokens are case-sensitive. Factories
accept canonical kebab-case names and class names such as `ConstantFolding`.
Aliases: `cfg`, `dom`, `liveness`, `dce`, `auto-vectorize`.
Unknown names throw `std::invalid_argument`; parsing never substitutes no-ops.

```cpp
auto pass = jiterati::create_pass("liveness-analysis");
dynamic_cast<jiterati::FunctionPass&>(*pass).run(function);
auto const& live_out = pass->result()->live_out;

for (auto const& info : jiterati::available_passes()) {
    // info.name, info.class_name, info.description, info.analysis
}
```

`PassManager::run` dispatches module passes once and function passes once per
function, in insertion order. Analysis outputs describe the most recent
function processed by that pass instance. Results are invalid after modifying
the corresponding input. Pass instances contain mutable result state and must
not be concurrently reused; independent instances/functions are parallel-safe.
There is no implicit analysis cache: transforms compute needed facts locally.

CLI discovery uses the same registry:

```sh
Jiterati-CLI pass --list
```

## Integer semantics and legality

- Integer add/sub/mul/negation and bitwise operations use fixed-width modular
  arithmetic for `i1`, `i8`, `i16`, `i32`, and `i64`.
- `i1` has one bit, independently of `Type::size()`'s byte-sized storage.
- Folding preserves exact result types, including boolean comparisons.
- Unsigned host arithmetic avoids signed-overflow UB in the optimizer.
- Signed division/remainder by zero or `MIN / -1` is retained.
- Negative or out-of-width shifts are retained. Constant arithmetic right
  shifts use explicit sign fill rather than implementation-defined host shifts.
- Floating-point reassociation, identities and constant evaluation are excluded.
- DCE preserves loads, stores, calls, allocations, divisions/remainders, dynamic
  shifts and other potentially trapping operations, even with unused results.
- SIMD widths and tail-call eligibility are explicit instruction metadata;
  these planning passes do not replace scalar operations with machine code.

## Existing passes completed and integrated

| Pass | Supported implementation |
|---|---|
| CFGAnalysis | Reachable deterministic DFS postorder. |
| DominatorAnalysis | Reachability-aware iterative dominator sets. |
| LivenessAnalysis | Backward fixed point with predecessor-specific Phi uses. |
| EscapeAnalysis | Conservative stack escape classification through copy aliases. |
| RegisterAllocation | SSA interference coloring with eight virtual colors; `-1` spills. |
| ConstantFolding | Width-aware integer arithmetic, comparisons and conversions. |
| ConstantPropagation | Constant-copy resolution, folding, select/Phi simplification to fixed point. |
| CopyPropagation | Transitive type-preserving copy resolution with cycle detection. |
| DeadCodeRemoval | Iterative removal of unused nontrapping expressions. |
| DeadCodeElimination | Root-based mark/sweep, including dead Phi cycles. |
| StrengthReduction | Integer multiplication by power-of-two constants. |
| PeepholeOptimization | Algebraic/select/Phi cleanup and copy propagation. |
| CommonSubexpressionElimination | Local and dominating-block expression reuse. |
| ControlFlowOptimization | Constant/equal-target branches, unreachable regions, linear block merging. |
| FunctionInlining | Single-block, nonrecursive, call-free callees of at most 32 stored instructions. |
| InstructionScheduling | Dependency-ready critical-height scheduling within nontrapping runs. |
| LoopInvariantCodeMotion | Natural-loop hoisting into unique branch-only preheaders. |
| LoopUnrolling | Full expansion of statically simulated small counted loops. |
| LoopPeeling | First-iteration expansion and induction entry update. |
| LoopFusion | Zipped static schedule of independent adjacent equal-trip-count loops. |
| LoopInterchange | Interchanged static schedule of pure rectangular two-level nests. |
| PolyhedralOptimization | Bounded static-domain scheduling and cleanup. |
| PartialRedundancyElimination | Predecessor availability, safe missing-edge computation, merge Phi. |
| TailCallOptimization | Mark adjacent call/return with matching signatures and no local allocations. |
| AutoVectorize | Group independent same-opcode/same-type scalar integer operations by host SIMD width. |

Register allocation is target-independent analysis, preserving SSA IDs. Actual
physical register classes, ABI constraints and machine spills remain backend
responsibilities. Tail-call metadata similarly requires backend consumption.

## Thirty additional optimizations

Each row maps directly to `Passlib/<ClassName>.cpp` and a factory/JPL token.

| Class / file stem | JPL token | Rewrite |
|---|---|---|
| AlgebraicSimplification | algebraic-simplification | Integer identities/annihilators and `x-x`. |
| Reassociation | reassociation | Combine constants in associative integer chains. |
| CommutativeCanonicalization | commutative-canonicalization | Stable ordering, with constants on the right. |
| ComparisonCanonicalization | comparison-canonicalization | Swap greater-than predicates into less-than forms. |
| SelectSimplification | select-simplification | Constant conditions and identical arms. |
| PhiSimplification | phi-simplification | Identical inputs, excluding self-references. |
| RedundantCastElimination | redundant-cast-elimination | Identity casts and truncate-after-extension round trips. |
| DoubleNegationElimination | double-negation-elimination | `neg(neg(x)) -> x`. |
| DoubleNotElimination | double-not-elimination | `not(not(x)) -> x`. |
| BitwiseIdempotence | bitwise-idempotence | `x&x`, `x|x`, `x^x`. |
| BitwiseAbsorption | bitwise-absorption | `x&(x|y)` and `x|(x&y)`. |
| BitwiseComplementFolding | bitwise-complement-folding | Bitwise operations on `x` and `not(x)`. |
| ShiftSimplification | shift-simplification | Zero shifts and in-range shifts of zero. |
| ShiftCombination | shift-combination | Same-direction shifts whose combined count is in range. |
| UnsignedDivisionReduction | unsigned-division-reduction | Power-of-two unsigned division to logical shift. |
| UnsignedRemainderReduction | unsigned-remainder-reduction | Power-of-two unsigned remainder to mask. |
| MultiplyByNegativeOne | multiply-by-negative-one | Modular multiplication by minus one to negation. |
| SubtractionToAddition | subtraction-to-addition | Subtract constant to add its modular negative. |
| NegationPropagation | negation-propagation | Add/subtract negated operands. |
| BooleanComparisonSimplification | boolean-comparison-simplification | `i1` equality/inequality with constants. |
| BooleanSelectSimplification | boolean-select-simplification | Boolean truth-table selects to condition/complement. |
| DistributiveFactoring | distributive-factoring | Factor common multiplicands when both products are single-use. |
| MaskSimplification | mask-simplification | Nested commuted constant AND/OR masks. |
| LoadForwarding | load-forwarding | Exact-address, exact-type store-to-load forwarding. |
| RedundantLoadElimination | redundant-load-elimination | Reuse matching loads between memory clobbers. |
| DeadStoreElimination | dead-store-elimination | Overwritten matching stores across nontrapping expressions. |
| StoreOfLoadElimination | store-of-load-elimination | Unchanged nonescaping stack writeback. |
| StackPromotion | stack-promotion | Initialized, uniform-type, nonescaping block-local slots. |
| JumpThreading | jump-threading | Branch-only trampoline bypass with Phi-free destination. |
| UnreachableBlockElimination | unreachable-block-elimination | Reachability pruning and obsolete Phi inputs. |

## Memory conservatism

The scalar IR has no volatile/atomic qualifiers or alias-region annotations.
Memory passes operate on ordinary scalar accesses. Pointer equality is exact
SSA identity after copy resolution; distinct pointers are never assumed disjoint.
Every unknown store/call clears load availability. Load forwarding remembers
only the most recent store. Dead-store elimination stops at reads, calls and
potentially trapping instructions. Store-of-load elimination requires a
nonescaping allocation, avoiding writeability assumptions about external memory.
Stack promotion rejects uninitialized reads, cross-block accesses and mixed types.

## Loop transformation limits

- Counted domains require one integer induction Phi with explicit incoming
  blocks, a constant initial value, add/subtract constant step, and an integer
  comparison against a constant bound in the conditional latch.
- Recognized loops are post-tested. Exact modular simulation proves termination
  within 16 iterations; expansion is capped at 128 source-instruction visits.
- Full unrolling preserves scalar memory order. Calls, allocations, extra Phis
  and additional control terminators reject the simple-loop fast path.
- Fusion requires equal iteration counts, no cross-loop SSA dependencies and
  nontrapping bodies. It emits the statically fused schedule in the preheader.
- Interchange requires a two-instruction outer header, a three-instruction outer
  latch and a self-latched inner loop with constant rectangular bounds. Memory
  operations and potential traps prohibit reordering.
- Static-domain scheduling is a bounded scalar optimization, not a general
  affine dependence solver. Unsupported domains retain their original CFG.
- LICM can handle larger natural loops but only speculates proven nontrapping
  expressions with operands available in the unique preheader.

## Validation

`tests/test_passes.cpp` checks every factory, arithmetic boundary cases,
differential execution across widths, predecessor-specific liveness, alias
barriers, PRE insertion, inlining, scheduling and explicit planning metadata.
`tests/test_pass_loops.cpp` checks live-outs, exit Phis, memory ordering, static
loop scheduling and rejected domains. `tests/PassInterpreter.hpp` provides an
independent interpreter and ownership/CFG/SSA verifier.

```sh
cmake -S . -B build -DJITERATI_BUILD_TESTS=ON -DJITERATI_BUILD_LUA=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Analysis invalidation and per-pass legality are specified in
[`specs/passes-specs.yaml`](../specs/passes-specs.yaml).
