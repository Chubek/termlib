# AGENTS.md

# The Jiterati Project

Most of Jiterati has already been implemented. The remaining work is primarily documentation, specifications, packaging infrastructure, CLI polish, and project integration.

The purpose of this document is to define the remaining work in sufficient detail that an autonomous coding agent can complete it with minimal human intervention.

---

# Tightening of the Backends

The backends located in the `BE/` directory are expected to closely match the architectural specifications contained in the datasets under:

```
.agents/datasets/
```

These datasets are the canonical machine-readable description of each supported architecture. Whenever backend code and dataset disagree, the dataset should be considered the source of truth unless there is a documented reason otherwise.

## Purpose

The purpose of tightening a backend is to improve its correctness, completeness, consistency, and maintainability by aligning its implementation with the corresponding architecture dataset.

This includes, but is not limited to:

- instruction definitions
- operand constraints
- register classes
- register aliases
- calling conventions
- ABI rules
- instruction encodings
- addressing modes
- relocations
- feature flags
- architecture extensions
- assembler syntax
- target-specific metadata

## Responsibilities

Whenever modifying a backend, the agent should consult the matching dataset:

| Backend | Dataset |
|---------|---------|
| AMD64 | `.agents/datasets/amd64.lua` |
| AArch64 | `.agents/datasets/aarch64.lua` |
| RV64 | `.agents/datasets/rv64.lua` |
| WebAssembly | `.agents/datasets/wasm.lua` |

The dataset should be used to:

- fill in missing instructions
- correct inaccurate metadata
- normalize naming conventions
- remove duplicate definitions
- improve validation logic
- improve instruction selection tables
- improve operand verification
- improve assembler and disassembler support
- improve code generation consistency

## Acceptable Improvements

Agents are encouraged to make changes such as:

- adding missing instruction definitions
- correcting operand types
- correcting register classes
- improving feature detection
- improving instruction encoding tables
- improving parser tables
- improving pretty-printers
- reducing duplicated backend logic
- replacing hard-coded values with generated tables derived from the dataset

## Generated Code

Whenever practical, backend tables should be generated from the corresponding Lua dataset instead of being manually maintained.

Generated artifacts should be deterministic and reproducible.

Hand-written code should be reserved for algorithms and target-specific behavior that cannot reasonably be generated.

## Validation

After tightening a backend, verify that:

- every instruction referenced by the backend exists in the dataset
- every register is represented
- register aliases are consistent
- operand validation agrees with the dataset
- architecture feature flags are synchronized
- assembler and parser tables remain internally consistent
- existing tests continue to pass
- no behavior regresses

## Goal

The long-term goal is for each backend in `BE/` to become a faithful implementation of its corresponding architecture dataset, minimizing duplicated information and ensuring that architectural knowledge is maintained in exactly one canonical location.

---

````markdown
## Implementation of `include/Jiterati-Macro.hpp`

The `Jiterati-Macro.hpp` interface provides the bridge between the C++ runtime and the Lua-powered macro system built on top of `sol2`.

The implementation relies heavily on the `sol2` Lua API wrapper library, located in:

```text
dependencies/sol2/
```

Internally, the runtime exposes the `ljiterati` Lua module, implemented in:

```text
Lua/ljiterati.cpp
```

This module exposes instruction builders, macro construction APIs, runtime metadata, and utility helpers to Lua scripts.

---

## Macro Overview

Macros are Lua scripts capable of dynamically generating instructions, transformations, analysis passes, or arbitrary runtime behavior.

A macro is represented as a Lua object created through:

```lua
ljiterati.macro.new(name)
```

Each macro typically defines:

- `main(...)`
  - Primary entry point
- `init()`
  - Optional initialization hook
- `cleanup()`
  - Optional teardown hook
- helper functions
  - Reusable utility logic

Macros may emit instructions through the `ljiterati.insn` API.

---

## Basic Example

```lua
M = ljiterati.macro.new "QuickDivide"

M.main = function(num, denum)
    ljiterati.insn.load(num)
    ljiterati.insn.load(denum)
    ljiterati.insn.div()
    ljiterati.insn.ret()
end
```

---

## Example: Integer Addition Macro

```lua
M = ljiterati.macro.new "QuickAdd"

M.main = function(a, b)
    ljiterati.insn.load(a)
    ljiterati.insn.load(b)
    ljiterati.insn.add()
    ljiterati.insn.ret()
end
```

---

## Example: Multiplication With Constant Folding

```lua
M = ljiterati.macro.new "FoldMultiply"

M.main = function(a, b)
    if type(a) == "number" and type(b) == "number" then
        ljiterati.insn.const(a * b)
    else
        ljiterati.insn.load(a)
        ljiterati.insn.load(b)
        ljiterati.insn.mul()
    end

    ljiterati.insn.ret()
end
```

---

## Example: Conditional Branch Emission

```lua
M = ljiterati.macro.new "CompareAndJump"

M.main = function(lhs, rhs, label)
    ljiterati.insn.load(lhs)
    ljiterati.insn.load(rhs)

    ljiterati.insn.cmp_eq()
    ljiterati.insn.jump_if(label)
end
```

---

## Example: Loop Construction

```lua
M = ljiterati.macro.new "CountdownLoop"

M.main = function(count)
    local begin_label = ljiterati.label.new("loop_begin")
    local end_label = ljiterati.label.new("loop_end")

    ljiterati.insn.label(begin_label)

    ljiterati.insn.load(count)
    ljiterati.insn.const(0)
    ljiterati.insn.cmp_eq()

    ljiterati.insn.jump_if(end_label)

    ljiterati.insn.dec(count)
    ljiterati.insn.jump(begin_label)

    ljiterati.insn.label(end_label)
    ljiterati.insn.ret()
end
```

---

## Example: Macro Initialization Hook

```lua
M = ljiterati.macro.new "ConfiguredMacro"

M.init = function()
    print("Initializing macro...")
end

M.main = function()
    ljiterati.insn.nop()
    ljiterati.insn.ret()
end

M.cleanup = function()
    print("Cleaning up macro...")
end
```

---

## Example: Runtime Introspection

```lua
M = ljiterati.macro.new "InspectRuntime"

M.main = function()
    local arch = ljiterati.runtime.arch()
    local os = ljiterati.runtime.os()

    print("Architecture:", arch)
    print("Operating System:", os)

    ljiterati.insn.ret()
end
```

---

## sol2 Integration

`Jiterati-Macro.hpp` uses `sol::state` and `sol::table` extensively to:

- expose the `ljiterati` runtime
- register instruction emitters
- bind macro objects
- transfer runtime values
- manage Lua execution contexts

Typical initialization flow:

```cpp
sol::state lua;

lua.open_libraries(
    sol::lib::base,
    sol::lib::package,
    sol::lib::string,
    sol::lib::table,
    sol::lib::math
);

RegisterLJiterati(lua);
```

Macros may then be loaded through:

```cpp
lua.script_file("macro.lua");
```

or:

```cpp
lua.script(R"(
    M = ljiterati.macro.new "Example"
)");
```

Below is a **practical set of macro types** worth implementing under the `Macro` directory. They’re grouped by purpose so your runtime stays organized and extensible.

## Core Macros

These are basic building blocks most scripts will use.

- `QuickAdd`  
  Emits integer or floating-point addition.

- `QuickSub`  
  Emits subtraction.

- `QuickMul`  
  Emits multiplication.

- `QuickDivide`  
  Emits division.

- `Modulo`  
  Emits remainder operation.

- `Negate`  
  Unary negation.

- `Increment`  
  Adds `1` to a value.

- `Decrement`  
  Subtracts `1` from a value.

---

### Constant & Value Macros

Useful for IR construction.

- `Const`
  Emit a constant literal.

- `LoadValue`
  Load a runtime value.

- `StoreValue`
  Store value into variable slot.

- `Move`
  Move value between registers/slots.

- `Swap`
  Swap two stack/register values.

---

### Control Flow Macros

Very important for building real programs.

- `If`
  Basic conditional execution.

- `IfElse`
  Conditional branching.

- `WhileLoop`
  Loop while condition holds.

- `ForRange`
  Numeric loop.

- `Break`
  Exit loop.

- `Continue`
  Skip iteration.

- `Jump`
  Unconditional jump.

- `JumpIf`
  Conditional jump.

- `Return`
  Return from macro/function.

---

### Comparison Macros

Used by branching logic.

- `CompareEqual`
- `CompareNotEqual`
- `CompareGreater`
- `CompareLess`
- `CompareGreaterEqual`
- `CompareLessEqual`

---

### Logical Macros

Boolean logic helpers.

- `LogicalAnd`
- `LogicalOr`
- `LogicalNot`

---

### Stack / Register Macros

If your IR uses a stack-like execution model.

- `Push`
- `Pop`
- `Dup`
- `Rotate`
- `Peek`

---

### Memory Macros

For interacting with memory regions.

- `LoadMemory`
- `StoreMemory`
- `Allocate`
- `Free`
- `Memcpy`
- `Memset`

---

### Function / Call Macros

Support modular code.

- `Call`
- `TailCall`
- `DefineFunction`
- `Lambda`
- `InvokeMacro`

---

### Optimization Macros

These run as transformations.

- `ConstantFold`
- `DeadCodeEliminate`
- `InlineMacro`
- `StrengthReduce`
- `CommonSubexpression`

---

### JIT / Backend Macros

Specific to code generation.

- `EmitNative`
- `EmitCall`
- `EmitSyscall`
- `PatchJump`
- `RegisterAlloc`

---

### Debug / Introspection Macros

Helpful for development.

- `PrintValue`
- `Trace`
- `DumpIR`
- `Assert`
- `InspectStack`

---

### Meta Macros

Macros that manipulate macros.

- `DefineMacro`
- `MacroExpand`
- `MacroCompose`
- `MacroPipeline`

---

### Recommended Minimum Set

If you're starting small, implement these first:

- `Const`
- `LoadValue`
- `StoreValue`
- `QuickAdd`
- `QuickSub`
- `QuickMul`
- `QuickDivide`
- `CompareEqual`
- `Jump`
- `JumpIf`
- `Return`
- `Call`

That gives you a **complete minimal instruction language**.

---

## Design Notes

The macro system is intentionally dynamic.

Macros are capable of:

- generating instructions
- rewriting intermediate representations
- performing static analysis
- implementing optimization passes
- constructing control-flow graphs
- emitting architecture-specific operations
- interfacing with runtime metadata

Because macros execute as Lua code, they can combine:

- procedural logic
- metaprogramming
- runtime inspection
- dynamic instruction generation

This design enables rapid experimentation while keeping the core runtime implemented in native C++.
````
---

# General Requirements

Unless explicitly stated otherwise:

- Preserve the existing architecture.
- Do not introduce breaking API changes.
- Keep all code cross-platform.
- Follow the project's existing formatting and naming conventions.
- New functionality must integrate with the current CMake build.
- All public APIs must be documented with Doxygen.
- Every new CLI command must include:
  - help output
  - examples
  - error handling
  - exit codes
- All generated specifications should be human-readable.

Whenever uncertainty exists, infer behavior from the surrounding code instead of inventing entirely new abstractions.

---

# Tightening of the Backends

We have provided datasets for all the architectures under `BE`. These datasets are in `.agents/datasets`. I want you to 

# Documentation

Documentation is the highest priority.

The documentation should be written for compiler engineers rather than end users.

Assume readers understand:

- C++
- LLVM
- compiler terminology
- SSA
- parsing
- optimization

Do **not** assume familiarity with Jiterati itself.

---

# The Manual

A complete manual must be written.

Directory:

```
docs/manual/
```

Requirements:

- exactly 18 chapters
- one Markdown file per chapter
- minimum 150 lines per chapter
- practical examples
- diagrams where appropriate (Mermaid is acceptable)
- code snippets
- references to relevant APIs

Suggested chapter layout:

1. Introduction
2. Architecture
3. Core Concepts
4. The IR
5. Parsing
6. Serialization
7. Validation
8. Analysis Passes
9. Transformation Passes
10. Backend Interface
11. Writing a Backend
12. Plugin System
13. Writing Plugins
14. Writing Passes
15. Package System
16. CLI
17. Embedding Jiterati
18. Internal Design

Every chapter should conclude with:

- Summary
- Further Reading
- Related APIs

---

# Front Page

```
docs/frontpage.md
```

The front page should contain:

- project overview
- feature list
- architecture diagram
- quick example
- installation
- links to every manual chapter
- links to API documentation
- links to specifications

This file becomes the Doxygen front page.

---

# Doxygen Integration

`Doxyfile.in` must:

- use `docs/frontpage.md` as `USE_MDFILE_AS_MAINPAGE`
- recursively include `docs/manual`
- generate a documentation tree
- generate search indices
- generate call graphs if Graphviz is available

`docs/CMakeLists.txt` must:

- locate Doxygen
- configure `Doxyfile`
- expose

```
make docs
```

or

```
cmake --build . --target docs
```

The generated documentation should contain:

- API reference
- Manual
- Specifications
- Cross-links

---

# Specifications

Specifications should describe behavior independently of implementation.

They are intended to become the authoritative reference.

---

# IR Grammar

Write

```
specs/IR.ebnf
```

The grammar should fully describe the textual IR.

Include:

- lexical grammar
- identifiers
- literals
- whitespace
- comments
- modules
- declarations
- functions
- globals
- instructions
- operands
- metadata
- attributes
- directives

Where ambiguity exists, annotate the grammar.

The grammar should be sufficiently complete that another parser could be implemented solely from the specification.

---

# IR Specification

Additionally write

```
specs/IR.md
```

Describe:

- design philosophy
- object model
- instruction categories
- SSA rules
- symbol visibility
- naming rules
- typing rules
- validation rules
- serialization rules
- textual examples

---

# Plugin Specification

Create

```
specs/plugin-specs.yaml
```

Each plugin entry should include:

- name
- version
- description
- author
- ABI version
- exported symbols
- initialization
- shutdown
- dependencies
- configurable options
- capabilities
- extension points

Also document:

- plugin loading
- version negotiation
- compatibility rules

---

# Pass Specification

Create

```
specs/passes-specs.yaml
```

Every pass should document:

- name
- category
- description
- required analyses
- invalidated analyses
- preserved analyses
- inputs
- outputs
- configurable options
- execution order constraints
- parallel safety
- deterministic behavior

---

# Backend Specification

Create

```
specs/backend-specs.yaml
```

Describe every backend:

- supported targets
- architecture
- relocation model
- object formats
- executable formats
- code generation stages
- supported features
- limitations

---

# Package Format

Jiterati packages use Zstandard.

Implementation:

```
third_party/zstd
```

Package extension:

```
.jpkg
```

A package is an archive containing:

```
META-INF/
    manifest.json

BE/
Plugin/
Pass/
Docs/
Examples/
Resources/
```

The package may optionally include:

```
Tests/
Licenses/
Scripts/
```

---

# Manifest

The manifest describes package contents.

Example:

```json
{
  "name": "...",
  "version": "...",
  "description": "...",
  "author": "...",
  "license": "...",
  "homepage": "...",
  "dependencies": [],
  "plugins": [],
  "passes": [],
  "backends": [],
  "files": []
}
```

Every installed file should appear in the manifest.

---

# Package Scripts

Users create packages using a build script.

Example layout:

```
my-package/

META-INF/
    manifest.json

Plugin/
Pass/
BE/

package.sh
```

Packaging:

```bash
jiterati-cli package \
    --pack package.sh \
    --out Foo.jpkg
```

Extraction:

```bash
jiterati-cli package \
    --unpack Foo.jpkg
```

Validation:

```bash
jiterati-cli package \
    --validate Foo.jpkg
```

Manifest viewing:

```bash
jiterati-cli package \
    --view-manifest Foo.jpkg
```

Listing:

```bash
jiterati-cli package --list-plugins Foo.jpkg
jiterati-cli package --list-passes Foo.jpkg
jiterati-cli package --list-backends Foo.jpkg
```

---

# Installation

Packages install into

```
$JITERATI_HOME
```

Default:

```
~/.local/jiterati
```

Layout:

```
bin/
plugins/
passes/
backends/
packages/
cache/
logs/
docs/
examples/
```

CLI:

```bash
jiterati-cli package --install Foo.jpkg
```

Additional commands:

```bash
jiterati-cli package --remove Foo
jiterati-cli package --upgrade Foo.jpkg
jiterati-cli package --list-installed
jiterati-cli package --verify Foo
jiterati-cli package --repair
```

---

# CLI

Provide a comprehensive command-line interface.

Top-level commands:

```
compile
parse
validate
format
package
plugin
backend
pass
config
cache
doctor
version
help
```

Every command must support

```
--help
```

and meaningful error messages.

---

# IR Support

The CLI should understand textual IR.

Examples:

```bash
jiterati-cli parse foo.ir

jiterati-cli validate foo.ir

jiterati-cli format foo.ir

jiterati-cli compile \
    --target amd64 \
    foo.ir
```

Supported outputs should include:

- assembly
- object file
- executable (where supported)
- textual IR
- binary IR (if implemented)

---

# Diagnostics

Diagnostics should include:

- colored output
- source locations
- notes
- warnings
- errors
- suggestions

Error formatting should resemble Clang where practical.

---

# Testing

All newly implemented functionality should include tests.

Documentation examples should compile whenever practical.

CLI tests should cover:

- valid packages
- malformed packages
- manifest validation
- plugin discovery
- installation
- removal
- IR parsing
- compilation

---

# Completion Criteria

This task is complete when:

- all documentation exists
- specifications are complete
- package management functions correctly
- CLI commands are implemented
- documentation builds successfully
- Doxygen integrates manuals
- examples compile
- all tests pass
- no TODO or FIXME markers remain in the newly added code
