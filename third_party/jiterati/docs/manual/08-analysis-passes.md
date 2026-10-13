# Analysis Passes

Analysis passes compute facts without semantic mutation.

## Pass API
`include/Jiterati-Pass.hpp` defines:
```cpp
class Pass { virtual std::string name() const = 0; };
class FunctionPass : public virtual Pass { virtual void run(Function&) = 0; };
class ModulePass : public virtual Pass { virtual void run(Module&) = 0; };
class AnalysisPass : public virtual Pass {};
class TransformPass : public virtual Pass {};
```

`PassManager` owns passes and runs module passes over a module.

## Bundled Analyses
`Passlib/` contains:
- `CFGAnalysis.cpp`
- `DominatorAnalysis.cpp`
- `LivenessAnalysis.cpp`
- `EscapeAnalysis.cpp`
- `RegisterAllocation.cpp`

`available_passes()` exposes the built-in catalog; `create_pass(name)` returns an
executable pass. `Pass::result()` exposes the most recently computed
`PassResult`, including postorder, dominators, edge-sensitive live sets,
escaping allocations, or target-independent interference colors.

## Analysis Discipline
- Do not mutate IR semantics.
- Keep cached results scoped to the analyzed function/module.
- Define invalidation conditions.
- Prefer deterministic traversal order.
- Avoid target-specific facts in generic analyses.

## JPL Pipeline Parser
```cpp
PassManager parse_jpl_pipeline(std::string_view source);
```

The parser resolves whitespace/semicolon-separated names through the built-in
registry and rejects unknown names. `PassManager::run` dispatches each function
pass over every function in the module. Results require recomputation after
mutation. See [the Passlib catalog](../passes.md) for contracts and limits.
