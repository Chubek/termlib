# Termscript implementation status

This is a **partial implementation of the new application examples**, not a
claim that `coedit`, `flashbench`, `kmsterm`, `tmx`, or `tsh` run yet.

## Build

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
build/bin/termscript termscript/examples/core.tsc
build/bin/termscript --backend qbe -o build/core termscript/examples/core.tsc
build/bin/core
```

The existing C11 runtime, native ABI, section lowering and AOT emitters were
reused from repository revision `ac270cfa`, preserving the stdlib's existing C
implementations. The original checkout could not configure because its runtime,
driver, parser adapter and native build file were missing. Historical companion
files and the canonical `emulator/` implementation were recovered; the existing
empty `emulatotor/` directory is not the emulator implementation.

## Architecture and ownership

- `Termscript.ng` is compiled by the bundled NovoParse generator. There is no
  second hand-written syntax parser. `source/parser.c` converts NovoParse trees
  into a common owned AST; abandoned parse allocations are reclaimed.
- `source/runtime.c` implements C values, module dispatch, lexical function
  frames, the evaluator and shared compiled-code helpers. Section lowering is
  in `source/sections_front.inc`; lifetime/flow semantics are shared helpers.
- `include/Termscript.h` is the public umbrella. `TSC-API.h` defines embedding
  and compiled-code support, `TSC-Module.h` exposes the counted native ABI,
  and `TSC-Backend.h` defines immutable backend descriptors. The historical
  `termscript/termscript.h` include remains compatible.
- `backends/C.cpp` and `QBE.cpp` expose the backend contract. The C emitter
  currently shares its translation unit with runtime internals; QBE lowering is
  in `backends/QBE.inc`. Both emit real control flow and native callbacks, not
  embedded source to parse later. A future internal-IR header can separate these
  translation units without making AST internals public.
- `backends/Jiterati.cpp` uses the bundled Jiterati API for checked i32
  arithmetic/comparisons. Other operations and wider results use the common
  evaluator. It is a hybrid execution backend, not a whole-program JIT.
- QBE runs as a separate compiler process. `backends/native.c` assembles its
  AMD64 output with bundled Keystone and writes the relocatable ELF object.
  Its offset-zero entry trampoline permits private functions before the unit.

VMs are single-threaded; use separate VMs or external synchronization. Native
descriptors and callbacks are borrowed for their entire registration lifetime.
Value strings are owned copies; handles retain aliases and release their payload
once. Output strings are caller-freed; compiled text is borrowed until the next
compile or VM destruction. Cleanup accepts NULL where specified in the API.
Execution has a shared step budget, nesting and recursion limits; there is no
cross-thread cancellation API. Parser failures carry byte offsets; existing
runtime diagnostics use source lines in the offset slot.

## Tested example-style core

- Newline/semicolon statement separators and a final statement before `}`/EOF.
  This intentionally makes formerly unterminated final expressions valid.
- `use a, b` binds registered `std.a`/`std.b` modules; missing modules fail.
- `let`/`const` immutable bindings and mutable `var`; optional simple type
  annotations are parsed as documentation, **not statically enforced**.
- Module dot calls, ordinary function calls, nested expressions and module
  colon calls (including historical whitespace calls).
- Decimal/hexadecimal int64 literals; checked arithmetic and string `+`;
  comparisons; bitwise operations; logical `and`/`or`; unary `!`, `not`, `-`.
- Precedence, highest first: unary, `* / %`, `+ -`, shifts, bitwise AND/XOR/OR,
  comparisons, `and`, `or`, then existing flow operators. All binary levels
  associate left. Boolean conjunction/disjunction short-circuit and return an
  operand. Operands and call arguments evaluate left-to-right once.
- Shifts require counts 0–63. Left shift wraps its uint64 bit pattern; right
  shift is logical. Ordinary signed arithmetic reports overflow.
- Brace `if`/`else` and `while`; existing `do`/`then`/`end` forms remain valid.
- Top-level named recursive functions, positional parameters and early returns.
  Functions see their own locals and top-level globals, not caller locals.
  Assignments inside a function create/update locals, not global bindings.
  Branches/loops share their enclosing function's frame. No closures.
- `main()` or `main(args)` is invoked after top-level initialization. `args`
  uses the registered list module and excludes the executable/script name.
  Its nil/int result becomes exit code 0 or 0–255; execution failures remain
  separate statuses. CLI options precede the script; following tokens are args.

Function frames release locals on success, error, recursion-limit failure and
early return. Returned handle aliases remain live. `lifetime` keeps the existing
reverse-acquisition ownership contract; it releases its reference, not aliases
retained elsewhere. Flow operators retain the existing observer semantics:
`~>` observes failure, `->` follows that observer, `=>` observes success. Error
observers do not suppress the original failure. These semantics were not
redefined to guess the meaning of the new examples' `catch`/`?` constructs.

## Remaining work

The applications still require structs/enums/impl, match/patterns, ranges/for,
closures, generics, floats, interpolation/raw strings, destructuring, collection
indexing/method protocols, optional/result propagation, defer, scheduling and
substantial additional library APIs. Those are **not implemented**. Do not
describe parsing a simple type annotation as a type checker.

The registry currently contains 79 native modules. Ten newly implemented
modules are documented in `stdlib/new-modules.md`; the preexisting modules keep
their documented smaller APIs, not the richer APIs imagined by the application
examples. `stdlib/MANIFEST.yaml` lists unimplemented module directories.
Empty per-module Termakfiles and the other empty `tools/*.c` programs remain
unfinished rather than being replaced with successful no-ops. C++ convenience
wrappers in `include/Termscript.hpp` remain unfinished.

The de-stubbing pass added bounded `procfs`/`sysfs` views, fixed the empty
`env` companion, and added configure-time checks rejecting empty/missing source,
header and companion files for shipped modules (including alias companions).
Fresh installations omit the unfinished empty C++ convenience header. The nine
remaining module directories in the manifest are not registered as fake native
modules; their empty files remain an explicit backlog, not working APIs.

`stdlib/crdt/rga.[ch]` now provides an original, independent C11 Unicode RGA
library (`libtermscript_crdt.a`, also embedded in the stdlib archive). Its
`std.crdt` adapter implements concurrent scalar-indexed text editing, stable
character IDs, duplicate/out-of-order delta delivery, state merge and checked
version-1 snapshots. See `stdlib/crdt/README.md` for the exact public API,
ownership, ordering and wire format. It has no dependency on the vendored
Kotlin `c-crdtlib`, which is not a C library. Tombstone GC, revive/invert undo,
effect streams, vector-clock acknowledgments, authenticated transport and the
full illustrative `coedit.tsc` method/type API remain unimplemented.

Dependencies use the repository's vendored snapshots; no external library API
was guessed and no dependencies were downloaded. Existing platform packages are
still required for the full stdlib. New integrations use the existing buffer
and utf8proc helpers, standard C I/O and POSIX IPv4 sockets. UDP is unauthenticated
and unencrypted and must not be treated as secure remote collaboration.

## Regression coverage

- `test_termscript_modern.c`: backend descriptors, owned function trees, local
  resource destruction on return/error, recursion failure and VM reuse.
- `termscript_modern.tsc`: common VM/C/QBE/Jiterati core conformance.
- `termscript_entry.cmake`: script arguments and nonzero main exit codes.
- `test_termscript_new_stdlib.c`: memory bounds/double-close, local UDP
  would-block/truncation/empty packets, invalid UTF-8 and stdin EOF.
- `test_termscript_fs_view.c`: isolated filesystem fixtures, traversal and
  symlink rejection, FIFO/binary rejection, exact/oversized reads, bounded and
  repeatable sorted listings, alias lifetime/double-close and descriptor counts
  after repeated error paths. `termscript_fs_view.tsc` exercises both adapters
  and the repaired env companion across the execution/compilation backends.
- `test_termscript_crdt_core.c`: all 720 delivery permutations of a mixed
  history, randomized three-replica convergence, replay/reconnect, strict
  Unicode, deleted anchors, snapshots/corruption/truncation, conflicts,
  logical-clock overflow and a 65536-character chain. The independent
  `test_termscript_crdt_alloc.c` injects every allocation failure along core
  operation paths and verifies atomic state and complete cleanup.
  `test_termscript_crdt_threads.c` checks concurrent independent documents and
  externally serialized sharing on pthread platforms.
- `test_termscript_crdt.c` and `termscript_crdt.tsc`: adapter ownership/aliases,
  limits/error paths and shared VM/C/QBE/Jiterati native module behavior.
- `termscript_new_stdlib.tsc`: new module behavior across backends.
- `test_termak.rb`: trusted plugins/hooks, compile/link, quoted paths,
  dependency validation, staged install guards and verified offline packages;
  failed compiler/linker artifact preservation and transitive C++ linking.

Run the full CTest suite for legacy ABI, sections, companions, rendering and
shell regressions. Hardware/display examples remain illustrative.

## Validation record

Linux native configuration (`TERMSCRIPT_NATIVE=ON`, Jiterati enabled) builds
successfully. `ctest --test-dir /tmp/opencode/termlib-native --output-on-failure
--parallel 4` passed all 26 tests after the de-stubbing pass. The companion sweep took about 115 seconds:
deeply nested legacy examples still expose a serious parser-performance
limitation, close to that test's 120-second bound.

AddressSanitizer/UndefinedBehaviorSanitizer builds also passed the function-frame
and new-native-module tests, plus the compiled section success/failure cleanup
fixtures. Generated modern C passes `cc -std=c11 -Wall -Wextra -Werror
-fsyntax-only`. Termak's nine offline tests and Ruby syntax checks pass.
Root and `cmake -S termscript` entry-point builds were tested, including the
no-stdlib/no-JIT configuration. Staged installation and installed driver/Termak
smoke tests succeeded; full relocation of the separately invoked compiler's
dependency paths was not tested. Vendored Keystone/libdrm produce existing
configuration/deprecation warnings.

The new filesystem fixture also passes ASan/UBSan. It uses only local POSIX
files/directories/FIFOs/symlinks, not real process/device data or unverified
third-party APIs. Both views share one private implementation; root paths are
trusted configuration, and these read-only adapters are not filesystem sandboxes.
After the final root-symlink/trailing-slash regression, the native rebuild,
25-test rerun excluding the slow companion sweep, and filesystem ASan/UBSan
test all passed again.

The CRDT addition built successfully in the full project and independently
with `cmake -S termscript/stdlib/crdt -B /tmp/opencode/termlib-crdt` and
`-Wall -Wextra -Werror -Wpedantic`. The standalone core/allocation/thread tests
passed (3/3); the adapter/core/allocation/thread ASan/UBSan tests passed (4/4).
The final full-project run passed 31/31 tests, including shared CRDT backend
conformance and the added threading test. Generated CRDT C passed `-std=c11 -Wall -Wextra -Werror
-fsyntax-only`; clang-tidy's diagnostic/static-analyzer checks reported no
diagnostics. The CRDT Termakfile build, standalone staged installation,
installed archive/header consumer, and runnable CRDT example were verified.
