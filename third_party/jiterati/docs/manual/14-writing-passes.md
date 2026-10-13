# Writing Passes

Passes implement analyses or transformations over `Module` or `Function`.

## Minimal Function Pass
```cpp
class MyPass : public TransformPass, public FunctionPass {
public:
    std::string name() const override { return "my-pass"; }
    void run(Function& function) override;
};
```

## Minimal Module Pass
```cpp
class MyModulePass : public ModulePass {
public:
    std::string name() const override { return "my-module-pass"; }
    void run(Module& module) override;
};
```

## Pass Manager
```cpp
PassManager manager;
manager.add(std::make_unique<MyModulePass>());
manager.run(module);
```

`PassManager::run(Module&)` executes module passes once and function passes over
every module function, preserving insertion order. Built-in factories and JPL
tokens are declared by `Passlib/Passes.def`; each implementation defines its own
runner/factory and is listed in `Passlib/CMakeLists.txt`.

`Passlib/PassSupport.hpp` supplies ownership-preserving rewrites, semantic SSA
use iteration, exact-width literals and CFG maintenance. Branch/Phi labels and
call symbols must not be treated as data uses. New passes require differential
semantics and legality tests. See [the Passlib catalog](../passes.md).

## Pass Metadata
`specs/passes-specs.yaml` defines documentation fields:
- name;
- category;
- required analyses;
- invalidated analyses;
- preserved analyses;
- inputs/outputs;
- ordering constraints;
- parallel safety;
- determinism.

## Authoring Rules
- State mutation scope precisely.
- Keep analysis and transformation passes separate.
- Preserve deterministic traversal.
- Revalidate after structural mutation.
- Do not encode backend legality in generic passes.
