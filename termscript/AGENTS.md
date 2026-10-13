# AGENTS.md — Termscript Engineering Guide

## 1. Project mission

Termscript is a systems-oriented scripting language and a subset of Termlib. It is designed to wire together libraries, manage resources, configure runtimes, and orchestrate terminal-oriented programs.

The project targets three backends:

- **C** — native compilation through C code generation.
- **VM** — execution through a virtual machine.
- **QBE** — compilation through QBE intermediate representation.
- **Jiterati** - using Jiterati for a JIT-based backend.

All the libraries are in `third_party`.

The repository is the source of truth for which features and backends are currently implemented. Do not describe planned functionality as working functionality.

The goals of every change are:

1. Preserve the language's concise, section-oriented design.
2. Keep language semantics independent of individual backends.
3. Make library integration explicit and testable.
4. Provide useful diagnostics and predictable cleanup.
5. Keep the compiler, runtime, and standard library maintainable.

## 2. Mandatory workflow

Before making changes:

1. Inspect the repository structure and current Git state.
2. Read the grammar, language specification, and existing examples.
3. Identify the implementation of the relevant parser, IR, runtime, or backend.
4. Run the existing tests and record any failures that predate the change.
5. Determine which dependencies are available and which versions are pinned.

Before completing changes:

1. Run formatting, compilation, and relevant tests.
2. Add regression tests for changed behavior.
3. Verify cleanup on both successful and failed execution.
4. Update examples and documentation.
5. Report changed files, test results, limitations, and unresolved assumptions.

Never claim a feature compiles or works unless it has been tested.

## 3. Language conventions

Existing Termscript examples use:

- `@alias` for namespace aliases.
- `@define` for symbolic substitutions or macro-like definitions.
- `.interface` to declare sections.
- `.section Name` to introduce a section.
- `.end` to terminate a section.
- `lifetime` for managed resources.
- `config` for configuration values.
- `G:Import(...)` for library imports.
- `~>`, `->`, and `=>` for error handling and result flow.

These conventions must be checked against the actual implementation before changing their semantics.

In particular, do not assume the exact meaning of `~>`, `->`, and `=>` from examples alone. Confirm their grammar, precedence, evaluation order, scoping rules, and control-flow behavior.

### Syntax compatibility

- Preserve existing valid syntax unless a breaking change is explicitly approved.
- Fix typographical errors in new examples.
- Do not silently change the meaning of existing operators.
- Add grammar tests whenever syntax changes.
- Document breaking changes and migration paths.
- Keep example programs consistent with the parser actually shipped in the repository.

## 4. Compiler architecture

Keep these responsibilities separate where possible:

1. Source loading and preprocessing.
2. Parsing.
3. Semantic analysis and type checking.
4. Intermediate representation construction.
5. Backend lowering and code generation.
6. Runtime execution and resource management.
7. Diagnostics and debugging support.

Do not embed language-wide semantics in a single backend.

### Front end

The front end should:

- Preserve source locations.
- Diagnose malformed syntax without crashing.
- Report undefined symbols and invalid section references.
- Distinguish parse errors from semantic errors.
- Handle imports and aliases consistently.
- Avoid unbounded processing of malformed input.

### Intermediate representation

Represent sections, imports, symbols, resource lifetimes, configuration, error paths, and control flow explicitly.

Prefer a backend-independent IR so that a language feature does not need three separate semantic implementations.

### Diagnostics

A diagnostic should contain, where available:

- A stable error category or code.
- Source file, line, and column.
- A concise explanation.
- The relevant symbol or section.
- A suggested correction or contextual note.

Do not replace useful underlying library errors with generic messages.

## 5. Backend requirements

### C backend

- Generate valid C with correct ownership and cleanup.
- Check return values from foreign-function interfaces.
- Avoid undefined behavior and unsafe implicit conversions.
- Test generated programs with compiler warnings enabled.
- Ensure failure branches release acquired resources.

### VM backend

- Define instruction semantics independently of the C backend.
- Validate stack effects and control-flow targets.
- Check resource lifetime and exception behavior.
- Provide instruction tracing and useful runtime diagnostics.
- Test invalid bytecode or IR rejection.

### QBE backend

- Lower the common IR into valid QBE IR.
- Validate types, control flow, and calling conventions.
- Keep target-specific details in the QBE lowering layer.
- Test generated IR and final program behavior.
- Do not assume that successful IR emission proves the program is correct.

### Backend conformance

Maintain a shared set of programs that exercise:

- Imports and symbol resolution.
- Configuration values.
- Branches and loops.
- Error propagation.
- Resource initialization and cleanup.
- Foreign-function calls.
- Parsing and plugin operations.

Run each test on every backend that claims to support the relevant feature.

## 6. Resource lifetimes and error handling

Termscript's `lifetime` construct must have documented ownership semantics.

For each managed resource, establish:

- Who creates it.
- Who owns it.
- Whether ownership can be transferred.
- When it is destroyed.
- What happens if initialization fails.
- What happens on early return or process exit.
- Whether destruction can fail.

Use a consistent cleanup strategy for partially initialized resources.

If a plugin is loaded and then fails initialization, close it safely. If an SSH connection succeeds but workspace initialization fails, disconnect. If a parser allocates a parse forest and semantic processing fails, release it.

Cleanup should be deterministic and should not mask the original error.

## 7. Library integration policy

Use the actual dependency headers, documentation, pinned versions, and existing wrappers as the source of truth.

For every new integration, document:

- The library's purpose.
- The supported version.
- Its API boundary.
- Memory ownership.
- Threading and concurrency requirements.
- Error handling.
- Security implications.
- Tests and known limitations.

Do not invent external library APIs and then present them as verified interfaces.

### `libglr`

Use for GLR parsing and scannerless grammar processing if supported by the actual version in the repository.

Tasks:

- Verify the grammar representation and parsing API.
- Define whitespace and comment handling.
- Preserve ambiguity when required.
- Distinguish parse forests from ASTs.
- Preserve source locations.
- Test ambiguous, malformed, empty, and deeply nested inputs.

Do not introduce a conventional lexer if the intended grammar is scannerless unless the language design explicitly requires preprocessing or a separate lexical mode.

### `dynalo`

Use as the dynamic-library loading layer.

Tasks:

- Verify load, symbol lookup, error reporting, and unload APIs.
- Define a stable Termscript plugin ABI.
- Check ABI compatibility before invoking plugin functions.
- Validate required symbols.
- Define plugin initialization and shutdown.
- Prevent unloading while plugin code is still executing.

Dynamic loading is not a sandbox. Untrusted native plugins must not be treated as safe merely because they are loaded through a library wrapper.

### `dyncall`

Use only when runtime-determined native function signatures are required.

- Define argument and return types explicitly.
- Validate calling conventions.
- Keep function signatures separate from symbol names.
- Avoid unsafe calls with guessed or mismatched signatures.

### `c-crdtlib`

Use for collaborative shared state when its actual API and CRDT model meet project requirements.

- Document the CRDT data model.
- Define replica identity and synchronization behavior.
- Test concurrent updates and merge convergence.
- Handle disconnects and resynchronization.
- Define persistence and recovery.
- Keep authorization separate from merge semantics.

### `libssh`

Use for SSH transport and remote sessions.

- Verify host keys.
- Use supported authentication mechanisms.
- Apply connection and operation timeouts.
- Handle disconnects and cleanup.
- Keep credentials out of logs.
- Never disable host verification as a default.
- Do not assume that SSH authentication grants application-level permissions.

### `isocline`

Use for interactive command-line editing, prompts, completion, and history where appropriate.

- Preserve non-interactive execution.
- Detect unavailable terminal interaction.
- Avoid requiring a TTY for ordinary compiler or batch operations.
- Ensure terminal state is restored on errors.

### `cpp-httplib`

Use for HTTP functionality when required.

- Verify the actual version and API.
- Isolate C++ interfaces from C-oriented components where needed.
- Define threading and request-lifetime behavior.
- Configure timeouts and response-size limits.
- Validate untrusted network input.

### `tomlc99`

Use for TOML configuration support.

Define configuration precedence explicitly. A possible policy is:

1. Built-in defaults.
2. Configuration file.
3. Environment overrides.
4. Command-line overrides.

Validate configuration types, required keys, and value ranges. Do not silently ignore malformed configuration.

### `libchomsky3` and `novoparse`

Evaluate these as parsing alternatives or complementary components.

Before adding another parser framework, compare:

- Grammar expressiveness.
- Error reporting.
- Integration effort.
- Performance.
- Maintenance status.
- Compatibility with the existing IR.

Avoid maintaining multiple parser abstractions without a clear architectural reason.

### `gumbo-parser` and `lexbor`

Use for HTML parsing and document processing.

- Confirm each library's supported use cases.
- Document parser ownership and cleanup.
- Test malformed documents.
- Do not confuse HTML parsing with sanitization or security validation.

### `fontconfig`, `freetype`, `fribidi`, `harfbuzz`, and `libdrm`

Treat these as a possible terminal rendering or display subsystem.

Keep platform-specific display code separate from language semantics. Define ownership for fonts, faces, buffers, rendering resources, and display connections.

Test Unicode, bidirectional text, font fallback, and headless execution where relevant.

### `keystone`

Use for assembly generation when explicitly requested.

- Validate architecture and mode.
- Check assembly errors.
- Document generated-code ownership.
- Never execute generated machine code implicitly.
- Separate assembly generation from code execution.

### `lmdbxx`

Use for persistent storage when required.

- Define environment and transaction lifetimes.
- Document transaction ownership.
- Handle database errors explicitly.
- Test transaction rollback and recovery.
- Avoid keeping invalid references after transaction closure.

### `libshell`

Inspect the exact library and its API before integration.

Avoid shell-string concatenation with untrusted input. Prefer structured arguments and explicit process execution.

### `openaipp`

Verify the dependency's identity, source, version, license, API, and data-handling behavior before designing an integration. Do not infer capabilities from its name.

### `qbe`

Use for the QBE compilation backend.

- Validate generated IR.
- Check ABI and calling conventions.
- Keep code generation deterministic where practical.
- Test both compiler acceptance and executable behavior.

### `qpplib`

Inspect the actual headers and documentation before deciding which subsystem it should support. Avoid adding an abstraction until its role is clear.

## 8. Scannerless GLR implementation plan

Implement GLR integration as a distinct language or parsing feature.

Suggested stages:

1. Verify the real `libglr` interface.
2. Define grammar loading and validation.
3. Define source-input and parse-result ownership.
4. Implement parsing and diagnostics.
5. Represent ambiguous results explicitly.
6. Define conversion to Termscript's semantic AST or IR.
7. Add regression tests.
8. Integrate with the existing CLI and backends only after the parsing layer is stable.

Test at least:

- Empty input.
- Valid minimal input.
- Invalid characters or malformed syntax.
- Ambiguous grammar input.
- Nested expressions.
- Missing delimiters.
- Excessive nesting or resource consumption.
- Source-location accuracy.

Do not treat a parse forest as a single AST without an explicit disambiguation policy.

## 9. Plugin architecture plan

Define a small, stable plugin contract before implementing a public plugin API.

A plugin contract should specify:

- ABI version.
- Required exported symbols.
- Initialization arguments.
- Shutdown behavior.
- Ownership of returned objects.
- Error representation.
- Supported capabilities.
- Threading constraints.
- Unload requirements.

A recommended lifecycle is:

`discovered -> loaded -> validated -> initialized -> active -> stopping -> unloaded`

A failure can occur at every stage. The runtime must clean up all resources acquired before the failure.

Resolve and validate required symbols before publishing the plugin as active. Prevent use-after-unload by tracking callbacks, objects, and threads associated with the plugin.

Add tests for missing files, missing symbols, ABI mismatch, failed initialization, repeated loading, shutdown failure, and partial cleanup.

## 10. Remote collaboration and CRDTs

Keep the transport, synchronization, and user-interface layers independent.

- SSH or another authenticated transport carries messages.
- The CRDT subsystem defines merge and synchronization behavior.
- Workspace authorization defines who can edit or view content.
- `isocline` provides interactive input.
- Persistent storage, if used, manages recovery and history.

Do not assume SSH alone solves collaboration authorization. Validate permissions on the authoritative service or protocol endpoint.

Test simultaneous edits, reconnects, duplicate messages, invalid operations, and recovery from interrupted sessions.

Network integration tests should use local fixtures by default. Tests must not modify real firewall rules or connect to production infrastructure.

## 11. Debugging workflow

When a program fails:

1. Reproduce the failure with the smallest input.
2. Identify the phase: parsing, semantic analysis, IR lowering, code generation, loading, or execution.
3. Preserve the original diagnostic.
4. Inspect the relevant source span and symbol references.
5. Dump intermediate representations if supported.
6. Compare backend behavior using the same test.
7. Add a regression test before finalizing the fix.

Recommended debugging outputs:

- Parsed section structure.
- Symbol and import tables.
- Semantic diagnostics.
- Backend-independent IR.
- C output.
- QBE IR.
- VM instruction trace.
- Plugin loading and symbol-resolution diagnostics.
- Resource-lifetime tracing in debug builds.

Debug output should be deterministic where possible and must not disclose credentials, private keys, tokens, or sensitive configuration values.

## 12. Testing strategy

For every new feature, include:

1. A successful minimal example.
2. Invalid-input tests.
3. Missing-dependency tests.
4. Resource-cleanup tests.
5. Regression tests.
6. Backend conformance tests where applicable.

Keep unit tests independent of network access. Use mocks or local services for SSH and HTTP tests. Do not require a running remote server to test ordinary parsing or IR generation.

Examples in documentation must be labeled as either:

- **Runnable:** verified against the current implementation.
- **Illustrative:** demonstrates a proposed API or architecture that has not yet been implemented.

Do not blur these categories.

## 13. Suggested implementation order

1. Document current grammar and operator semantics.
2. Establish compiler and runtime regression tests.
3. Improve diagnostics and source-location tracking.
4. Formalize resource ownership and `lifetime` cleanup.
5. Establish a stable intermediate representation.
6. Verify and integrate `libglr`.
7. Add scannerless parsing examples and tests.
8. Define a versioned plugin ABI and integrate `dynalo`.
9. Add backend conformance coverage for plugins.
10. Prototype CRDT integration and transport separation.
11. Add SSH and interactive terminal adapters.
12. Expand remaining library integrations based on demonstrated project needs.

## 14. Completion report

Every agent completing a task must report:

- Summary of the change.
- Files modified.
- Architectural decisions.
- Commands and tests executed.
- Whether compilation succeeded.
- Any assumptions about unverified APIs.
- Known limitations and security implications.
- Remaining work.

A task is not complete merely because a code sample looks plausible. It is complete when the implementation, tests, documentation, and limitations agree.
