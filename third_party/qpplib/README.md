# QLib modules

Q files generate header-only C libraries. Compile a module with the bundled
compiler, then include the generated header:

```sh
python -m q bignum.q PREFIX=big -o big.h
python -m q messaging.q T=int CAP=128 -o messages.h
python -m q visitor.q 'T=struct node *' EQ=node_equal -o walk.h
```

Every module accepts `PREFIX` (default: the module name). Functions, types,
enum constants, and imported helpers use that prefix. Declare custom element
or operator types and equality functions before including the generated
header. Equality functions return nonzero for equal arguments. Different
instantiations in one translation unit need different prefixes.

The [Q language reference](AGENTS.md) describes compilation and the original
container, text, SIMD, and I/O modules. The modules below extend that library.
Their `.q` sources contain the complete function signatures.

## Build and ownership conventions

Use C11. The POSIX modules (`process`, `dlopen`, `messaging`, `daemon`,
`evloop`, `server`, `perf`) target Unix-like systems. Define
`_POSIX_C_SOURCE=200809L` before any system headers, or pass it to the compiler.
Link `messaging` with `-pthread` and `dlopen` with `-ldl` where required:

```sh
cc -std=c11 -D_POSIX_C_SOURCE=200809L app.c -pthread -ldl -o app
```

Initialize objects before their first operation. Do not reinitialize live
resource owners; release their resources first. Unless explicitly described
otherwise, pointer arguments must refer to valid objects and buffers. Objects
with owned memory, handles, locks, or descriptors must not be shallow-copied.
Only `messaging` synchronizes concurrent operations. Callback contexts and
payload pointers remain caller-owned.

Readers return `1` for success and `0` for rejected input, preserving output
arguments on failure. System-call wrappers use the exceptions documented
below. Binary views borrow **immutable** memory that must outlive the view;
validated reader structures must not be modified by callers. Parsers do not
load, execute, relocate, or authenticate executable files.

## Module APIs

| Module | Parameters besides `PREFIX` | API and scope |
|---|---|---|
| `process` | — | `init`, `spawn`, `wait`, `signal`, `exit_code`: POSIX argv execution with an explicit environment |
| `binfile` | — | `init`, `range`, `slice`, `read`, `uint`: checked byte ranges and unaligned endian integer reads |
| `elf` | — | `init`, `segment_at`, `section_at`: ELF32/64 metadata, both byte orders |
| `pe` | — | `init`, `section_at`, `rva_offset`: PE32/PE32+ metadata and file-backed RVA resolution |
| `macho` | — | `init`, `command_at`: thin Mach-O32/64 and load-command views, both byte orders |
| `dlopen` | — | `init`, `open`, `symbol`, `error`, `close`: POSIX shared-object handles |
| `visitor` | required `T`, `EQ`; `CAP=256` | `walk`: iterative graph DFS, enter/leave callbacks, cycle suppression |
| `parsec` | — | `init`, `literal`, `byte`, `eof`, `uint`, `choice`, `sequence`, `many`: byte-oriented parser primitives and combinators |
| `bignum` | — | `init`, `free`, `set_u64`, `copy`, `compare`, `add`, `sub`, `mul`, `div_u32`, `from_decimal`, `to_decimal`: unsigned arbitrary-precision arithmetic |
| `schema` | — | `validate`: typed flat-record validation with structured error codes and indices |
| `messaging` | required `T`; `CAP=64` | `init`, `send`, `recv`, `close`, `destroy`: bounded, synchronized FIFO mailbox |
| `daemon` | — | `detach`, `pidfile_lock`, `pidfile_unlock`: POSIX daemon lifecycle helpers |
| `evloop` | `CAP=64` | `init`, `add`, `remove`, `once`, `run`, `stop`: fixed-capacity `poll` reactor |
| `manpage` | — | `header`, `section`, `paragraph`, `text`, `argument`: escaped roff output |
| `server` | — | `init`, `listen`, `accept`, `nonblocking`, `cloexec`, `close`: TCP listener helpers |
| `perf` | — | `init`, `start`, `elapsed`, `stats_init`, `record`, `variance`: monotonic timing and online statistics |
| `egraph` | required `EQ`; `OP=int`, `CAP=256`, `ARITY=4` | `init`, `add`, `find`, `merge`, `rebuild`, `equal`, `extract`: bounded term e-graph with congruence closure |

Capacities and `ARITY` must be positive integer constants. `visitor`, `evloop`,
and `egraph` use capacity-sized automatic arrays; keep capacities appropriate
for the thread's stack budget.

### Process and dynamic libraries

`process_spawn(&p, file, argv, envp)` uses `posix_spawnp`, with no implicit
shell evaluation. `argv` and `envp` are NULL-terminated arrays. Pass an array
containing just NULL for an empty environment, or the application's environment
array to inherit variables. Spawn returns `0` on success, `-1` with `errno` on
failure, including an executable that cannot be started.

`process_wait(&p, &status, nohang)` returns `1` when reaped, `0` when still
running in nonblocking mode, or `-1` on error. It retries interrupted waits.
A successfully reaped process is no longer live. Always reap successfully
spawned children. `process_signal` returns the `kill` result and rejects
non-live handles. `exit_code` accepts raw wait status and returns `-1` for
signal termination. Do not reap managed children through another API.

`dlopen_open` accepts native `RTLD_*` flags and returns a boolean. A NULL path
opens the current program. `symbol` returns a boolean and writes a `void *`
only on success; a successful symbol address can itself be NULL. It clears
and consumes `dlerror` to distinguish that case. `error()` exposes any
**unconsumed** native loader error (for example, after a failed `open` or
`close`); it does not retain errors consumed by `symbol`. `close` returns a
boolean and clears the handle only after success. Symbol pointers remain
valid only while the object is loaded.

### Binary formats

`binfile_uint(view, offset, width, big, &value)` accepts widths 1–8; nonzero
`big` selects big-endian, zero selects little-endian. All ranges are checked
without addition overflow. Empty NULL-backed views and empty slices are valid.

`elf_init` validates the ELF identity, header version, header size, and
program/section table bounds. `segment_at` and `section_at` check payload
bounds when accessed; ELF `SHT_NOBITS` has no file payload. The header exposes
`bits`, `big`, `machine`, `type`, `entry`, and table counts. Extended numbering
is rejected. Section names and symbol tables are not decoded.

`pe_init` checks the DOS and PE signatures, COFF header, optional-header
variant and directory count, section table, and `SizeOfHeaders`. Section names
are copied into a nine-byte NUL-terminated buffer. Section payload bounds are
checked by `section_at`. `rva_offset` accepts header RVAs and bytes backed by a
section's raw data; virtual zero-fill tails fail. Imports, exports, relocations,
and COFF-only object files are outside this API.

`macho_init` checks the complete load-command region, command sizes, alignment,
and command count. `command_at` returns the command identifier and a bounded
view of its bytes, including its header. Command-specific payloads remain
opaque. Fat/universal containers are rejected.

### Visitors and parsers

`visitor_walk(root, count, child, visit, ctx)` uses callbacks with these shapes:

```c
size_t count(T node, void *ctx);
T child(T node, size_t index, void *ctx);
int visit(T node, int leaving, void *ctx);
```

`visit` returns positive to continue, zero to stop, or negative on entry to
prune children. A pruned node still receives a leave callback. `EQ` determines
identity; shared and cyclic nodes are visited once. Child lists must remain
stable during the walk. Return values are `1` complete, `0` stopped, `-1`
invalid callbacks or capacity exhausted. Stop/error does not unwind remaining
leave callbacks. `CAP` bounds both unique nodes and traversal depth.

`parsec_t` holds a byte view and cursor. Parser callbacks have signature
`int parser(parsec_t *, void *)`; return nonzero for success. They may only
advance the cursor within the input and must not replace the input view.
`choice` and `sequence` restore the cursor on failure. `many` accepts minimum
and maximum counts, restores the original cursor if the minimum is unmet,
and rejects successful parsers that consume no bytes. Context side effects
are **not** rolled back. `uint` recognizes decimal digits into `uint64_t`,
rejects overflow, and leaves the following non-digit for the next parser.
There is no implicit whitespace skipping, Unicode decoding, or parse tree.

### Bignums and schemas

A `bignum_t` owns little-endian 32-bit limbs; zero has no significant limbs.
All arithmetic permits the destination to alias an input. Mutating operations
return booleans and leave their destination unchanged on failure. Subtraction
rejects negative results. Division supports a nonzero `uint32_t` divisor,
with an optional remainder output; full bignum division and signed numbers
are not provided. Decimal input is nonempty digits only, with leading zeroes
accepted. Decimal output includes a NUL and fails without modifying the buffer
if capacity is insufficient. Initialize every output and call `free` on each
owned value. Output buffers and remainder pointers must not overlap limbs.

```c
#include "big.h"
#include <assert.h>

int main(void) {
    big_t n;
    char text[128];
    big_init(&n);
    assert(big_from_decimal(&n, "18446744073709551616"));
    assert(big_mul(&n, &n, &n));
    assert(big_to_decimal(&n, text, sizeof text));
    big_free(&n);
    return 0;
}
```

`schema_field` describes a name, exact type (`BOOL`, `INT`, `NUMBER`, `STRING`),
required flag, optional inclusive numeric bounds, and maximum string byte
length (zero means unlimited). `schema_value` is a named tagged union.
Validation checks schema definitions, duplicate input names, unknown names
when strict, required fields, exact types, finite numbers, and bounds.
There is no coercion or nested/JSON schema interpretation. Numeric bounds are
`double`; integers are compared using `long double`, so bounds requiring exact
integers beyond floating-point precision need application validation.
`schema_error` provides `code`, `field`, and `value` indices; `(size_t)-1`
means not applicable. Strings must be NUL-terminated.

### Mailboxes, daemons, reactors, and servers

`messaging_init` returns a pthread error code (zero succeeds). `send(q, value,
wait)` and `recv(q, &value, wait)` return `1` on transfer, `0` on closure, and
`-1` when a nonblocking operation would block. Closing rejects new sends,
wakes all waiters, and lets receivers drain existing values. Close and join
all users before `destroy`. Values are copied without deep ownership transfer.
Thread cancellation while blocked is not supported. The queue cannot be
shared across processes.

`daemon_detach(change_dir, redirect_stdio)` must run before creating threads.
It returns `1` in the original parent (the application should exit there),
`0` in the detached process, or `-1` on error. The intermediate child exits
internally. Parent success does not report daemon readiness; applications
must implement their own handshake and reap the intermediate child if the
original parent remains alive. Options change directory to `/` and redirect
stdin/stdout/stderr to `/dev/null`. Other descriptors and the umask are retained.

`pidfile_lock(path)` opens the file, acquires an advisory process lock, writes
the current PID, and returns the fd, or `-1` with `errno`. Keep the fd open
until `pidfile_unlock`. Acquire after detaching. The file is deliberately not
unlinked on release. POSIX record locks are per-process: use one owner per
pidfile, and do not independently open/close the same file while holding it.
A second lock attempt from the same process is not an ownership test.

`evloop_add` registers or replaces one fd's events/callback and returns a
boolean. `remove` unregisters without closing the fd. `once(timeout_ms)`
returns the number of callbacks dispatched, zero on timeout, or `-1` with
`errno` on poll failure (including interruption). Negative timeouts wait
indefinitely. Ready callbacks may add/remove registrations; stale entries in
that dispatch are skipped. Nested `once`/`run` calls fail with `EBUSY`.
`run` clears the stop flag and dispatches until a callback calls `stop`;
it retries interruptions. `stop` does not wake a blocked poll from another
thread and is not signal-safe. Use a registered pipe to deliver external
notifications. No timer queue or descriptor ownership is included.

`server_listen(&s, host, port, backlog)` returns `0` or `-1` with `errno`.
NULL host binds a wildcard address; zero port requests an ephemeral port.
It tries resolver addresses until one binds. `accept` returns a new owned
close-on-exec fd or `-1`. Both the listener and accepted sockets initially
use native blocking behavior; configure nonblocking mode explicitly.
`nonblocking`, `cloexec`, and `close` return syscall-style results.
Closing the server does not close accepted peers. This is a transport helper,
without HTTP, TLS, framing, or an application service loop.

### Manpages and performance

`manpage_header(stream, title, section, date)`, `section`, and `paragraph`
emit roff macros; `text` emits escaped body text and `argument` a quoted macro
argument. Backslashes are escaped, leading body `.`/`'` are neutralized, and
argument quotes are encoded. Newlines are allowed only in body text.
All return booleans. Invalid input or I/O failure can leave partial output;
check the return and discard the output if necessary. The caller owns and
flushes/closes the stream. No pager or formatter process is launched.

`perf_start` and `elapsed` return booleans, using monotonic time and seconds.
Initialize a timer before querying it. `stats_init`, `record`, and `variance`
provide Welford online statistics. Samples must be finite and nonnegative;
count overflow and nonfinite accumulated statistics are rejected without
changing the state. `variance` is sample variance, zero before two samples.
No hardware counters or process-wide profiler are included.

### E-graphs

An operator value plus an ordered child-class list defines an e-node.
`EQ` compares operators. `add` returns a canonical class ID, deduplicates
congruent terms, and returns `SIZE_MAX` for capacity or argument errors.
`CAP` bounds stored e-nodes, and `ARITY` bounds children per node. Merging
does not reclaim node slots. Class IDs remain valid after merging;
`find` canonicalizes them. `merge` returns `1` for a new union, `0` if already
equal, and `-1` for invalid IDs. It automatically rebuilds congruence closure.
`union` is an internal low-level helper: use `merge` for public operations.

`extract(graph, root, cost, ctx, best_node, &total)` computes a finite minimum
additive-cost term, returning a boolean. Supply `best_node` with at least
`graph.size` entries. The callback `size_t cost(OP, void *)` must be stable
and positive; zero and `SIZE_MAX` disallow an operator. Costs that overflow
are excluded. On success each class ID maps to its selected node index, or
`SIZE_MAX` when that class has no finite extraction. Cyclic equivalences are
supported when they contain a finite term. Positive costs keep selected terms
acyclic. Outputs are preserved when the requested root cannot be extracted.

This implementation uses linear term lookup and repeated pairwise congruence
checks. It is intended for small bounded graphs. Rewrite rules, saturation
scheduling, and expression rendering are application responsibilities.

## Validation

Run the local extension suite:

```sh
python -m pytest tests/test_extended_modules.py -q
QLIB_SANITIZE=1 python -m pytest tests/test_extended_modules.py -q
```

The suite compiles generated headers as strict C11 with warnings as errors,
checks independent and combined instantiation, and runs behavioral and malformed
input cases. Sanitizer mode enables undefined-behavior checks. POSIX tests need
process creation, dynamic loading, threads, and loopback socket permissions.
The existing library suite lives at `../../tests/domqlib-test-suite`.
