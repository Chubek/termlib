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

The registry currently contains 91 native modules. Additional implemented
modules are documented in `stdlib/new-modules.md`, `stdlib/parser-modules.md`
and `stdlib/device-modules.md`, with infrastructure modules documented in
`stdlib/infrastructure-modules.md`; the preexisting modules keep
their documented smaller APIs, not the richer APIs imagined by the application
examples. `stdlib/MANIFEST.yaml` records the shipped module inventory.
Every shipped module now has a functional Termakfile; unrelated empty
`tools/*.c` programs remain unfinished rather than successful no-ops.

The de-stubbing pass added bounded `procfs`/`sysfs` views, fixed the empty
`env` companion, and added configure-time checks rejecting empty/missing source,
header and companion files for shipped modules (including alias companions).
Fresh installations include the tested C++ convenience and protocol framing
headers. The previously empty index/multiplex/protocol modules are registered
with real bounded implementations, not successful no-ops.

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

## Implementation hardening pass

`include/Termscript.hpp` now supplies C++17 `termscript::value`, move-only
`termscript::vm`, `termscript::error`, and `check_syntax`. Values copy strings and
retain native handle aliases. Moves/adoption/release transfer ownership without
calling the C API's releasing constructors on the transferred payload. The VM
wrapper copies borrowed compiler output, frees owned run output even on errors,
rejects embedded NUL input, and diagnoses moved-from access. It does not silently
register the stdlib. Native module tables/userdata still outlive registration.

Runnable embedding example (link the same runtime dependencies as the C API):

```cpp
#include <termscript/include/Termscript.hpp>
#include <iostream>
int main() {
  termscript::vm vm;
  std::cout << vm.run("G:puts(42);");
}
```

Four previously empty modules now have actual implementations and companions:
`std.input` incrementally frames terminal events; `std.font_shape` reuses the
existing HarfBuzz implementation/handles; `std.shm` maps bounded POSIX named
shared memory; `std.mqueue` performs bounded, nonblocking POSIX message operations.
Their ownership, error, persistence, thread-safety and security contracts are
specified in `stdlib/new-modules.md`. SGR mouse decimal fields are now parsed
with checked bounds, rather than overflowing `sscanf` conversions.

Termak now validates all selected sources/install inputs before any build hook,
rejects escaping source symlinks and DESTDIR prefix traversal, atomically
publishes archives as well as objects/executables, and exposes explicitly pinned
package retrieval through `--index FILE --cache DIR fetch NAME VERSION`.
It is not a package dependency/version solver and does not extract archives.

The previously empty `evdev`, `uvdev`, and `wayland` modules now implement the
low-level APIs documented in `stdlib/device-modules.md`. The previously empty
`TSC-Protocol.h`, `index`, `multiplex`, and `protocols` scaffolds now implement
the bounded APIs documented in `stdlib/infrastructure-modules.md`; they do not
claim language-level collection/interface protocols or secure transport.

Validation of this pass: the native build succeeded and the full CTest suite
passed 36/36 tests. The new C++ ownership, input/font-shaping and POSIX IPC tests
also passed under ASan/UBSan (3/3). Input and shaping fixtures passed VM, compiled
C, QBE and Jiterati conformance; shaping uses the vendored Roboto font, not host
font discovery. Termak's 14 offline tests passed. Strict C11/C++17 warning checks,
the no-message-queue compilation path, selected GNU formatting checks and
`git diff --check` passed. A staged install, installed C++ header/runtime consumer
and installed Termak archive build succeeded. Clang's analyzer found no lifetime
or bounds defects in the four new adapters; it did emit two generic Annex K
recommendations for the explicitly bounded input memcpy/memmove operations.
Non-Linux runtime portability and online package downloads were not tested.
The full companion sweep took 118 seconds, still close to its 120-second bound.

## Parser module implementation

`std.grammar` now builds native NovoParse descriptors through declared symbols
and productions, uses the existing scannerless libglr runtime, rejects ambiguity
unless `first` was explicitly requested, and copies selected syntax trees into
owned DOM values. This is not a run-time `.ng` compiler, a packed-forest API, or
the examples' language grammar registry. It exposes the bundled runtime's lack
of hard internal resource budgets rather than pretending the VM budget limits it.

`std.peg` implements actual PEG ordered choice, possessive repetition, predicates,
byte classes and named captures. It has bounded compilation, rule/expression
depth, input, evaluation steps, capture count and output text; it rejects left
recursion and potentially empty repetition. Parse state is local, captures are
rolled back on failed alternatives/lookahead, and returned DOM trees outlive
grammar closure. It is bounded backtracking, not a packrat/linear-time claim.
See `stdlib/parser-modules.md` for the exact APIs and unsupported features.

Regression coverage includes the PEG grammar extracted from illustrative
`tsh.tsc` (not the application), strict trailing-input behavior, malformed input,
ambiguous GLR grammars, empty productions, source ownership, alias/double-close,
recovery after failures, capture/step/nesting limits and a deterministic
malformed-grammar corpus. The ELF allocation harness injects each failure along
PEG compilation/result construction, GLR result copying and grammar-handle construction and checks
complete cleanup; it does not intercept NovoParse shared-library allocations.

## Device modules and Termak recipes

`std.evdev` owns nonblocking Linux device/duplicated native-stream descriptors,
retains split records across would-block, distinguishes EOF/truncation, and
exposes bounded device-name/key capability/state queries. `std.uvdev` is an
explicit Linux uinput-v5 virtual keyboard adapter, not an inferred libudev API;
import never creates a device. `std.wayland` conditionally uses the platform
Wayland client >= 1.23 for bounded registry discovery and prepared-read/poll
integration, without pretending to implement windows or a compositor. It never
silently consumes WAYLAND_SOCKET, and closes only its own descriptors/proxies.
Tests use pipes, mocked uinput setup/writes, and a local Wayland server/socketpair;
real hardware/desktop injection and non-Linux runtime behavior remain unverified.

Termak now provides explicit trusted `cmake_project` integration, with declared
artifacts, validated cache definitions, component/full installation and consistent
prefix/DESTDIR handling. Its native builder retains its own atomic output and
install-path policy; external CMake builds/installers follow CMake's semantics,
not a promised sandbox or automatic dependency solver.

All 91 native modules (including aliases) have real per-adapter CMake archive
targets and Termakfiles; `common` adds a helper archive, and `gen`/`q` install real
headers/generator inputs without fake binaries. The full stdlib manifest installs
the runtime/dependency closure. Individual component installs require that base
package; adapter archives are not self-contained plugins. CRDT's independent core
recipe remains available alongside its VM adapter. Public headers and Termak itself
also have functional manifests. The original 89 module/common archives were compiled; the
Termak PEG recipe and a complete staged stdlib/runtime install were verified.

Device/Termak-stage validation (before the infrastructure additions below):

- Native build succeeded; `ctest --test-dir /tmp/opencode/termlib-native
  --output-on-failure --parallel 4` passed 43/43 tests, including backend checks.
- Nine focused C++/input/IPC/parser/device tests passed under ASan/UBSan.
- `ruby tests/test_termak.rb` passed all 19 tests, including CMake artifact,
  component installation, C++ dependent linking and invalid-path checks.
- New adapter sources passed strict C11 warning checks; device/parser sources
  passed the selected clang diagnostic/analyzer checks. Public parser/device
  headers passed GNU clang-format checks; `git diff --check` passed.
- Staged full and PEG-component installs succeeded. An installed pkg-config
  consumer linked the PEG adapter archive and ran successfully. The installed
  Termak launcher built a fixture archive; header/generator manifests installed
  their declared files; the protocol header was still unimplemented at that stage.

These results do not verify real input hardware, desktop injection, non-Linux
runtime behavior, hostile native plugins, or hard internal GLR resource budgets.

## Index, multiplexing and protocol framing

The remaining three scaffolds are now shipped modules, with their C sources,
public module headers, companions and Termakfiles under `stdlib/index`,
`stdlib/multiplex` and `stdlib/protocols`. Full contracts and a runnable backend
fixture are documented in `stdlib/infrastructure-modules.md`.

- `std.index` is a bounded sorted NUL-free byte-key/int64 index with binary
  lookup, transactional allocation failure and owned ordered prefix snapshots.
  Empty misnamed `index/socket.*` scaffolds were replaced by `index/index.*`.
- `std.multiplex` is a POSIX readiness set with borrowed fds, monotonic finite
  deadlines, EINTR retry and optional pollable-fd cancellation. It preserves
  std.poll's mask vocabulary but returns explicit TIMEOUT/CANCELLED statuses.
- `TSC-Protocol.h` exposes an opaque incremental binary frame decoder and
  allocation-free encoder from libtermscript_stdlib. The versioned big-endian
  format bounds lengths and checks CRC over header/payload. Unknown nonzero
  types are preserved; unsupported versions/flags and corrupt frames fail
  explicitly. `std.protocols` uses strict hex strings to avoid NUL truncation at
  the script boundary. Decoder failures require explicit reset where documented.

Validation:

- Native build and all 46 CTest tests passed, including VM/C/QBE/Jiterati
  conformance, readiness/cancellation, every frame split/truncation, corruption,
  limits, snapshot lifetime and deterministic allocation-failure cleanup.
- The three infrastructure tests also passed under ASan/UBSan. The initial
  sanitizer AOT link lacked sanitizer runtime flags; rerunning with an explicit
  `CC` wrapper adding `-fsanitize=address,undefined` passed. No unrelated driver
  configuration behavior was changed to hide that limitation.
- Strict C11 warnings-as-errors and the selected clang diagnostic/analyzer checks
  passed for new modules/tests. Public headers passed GNU clang-format checks.
- All 19 Termak tests passed. Each new module's Termak component build and staged
  install succeeded. A C++17 installed-header/archive/pkg-config consumer compiled
  with warnings-as-errors and ran a binary-payload protocol roundtrip.

These APIs do not implement language-level collection/interface protocols,
persistent/full-text indexing, channel routing, or a secure session transport.
CRC is not authentication; untrusted networks still require authenticated,
encrypted transport plus application authorization and payload validation.
Multiplexing requires POSIX; non-Linux runtime behavior remains unverified.
