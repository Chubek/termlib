# Termscript Manual

**Restructuring note:** the example-style core, current backend boundary and
remaining unimplemented application features are documented in
[`termscript/IMPLEMENTATION.md`](../termscript/IMPLEMENTATION.md). The older
language reference below remains useful for section syntax and native modules;
its statements that functions, infix expressions or script arguments are absent
have been superseded by that status document.

This manual describes the shipped Termscript 0.2 language, command-line driver,
standard library, and native module ABI version 2. The implementation references
are [`Termscript.ng`](../termscript/Termscript.ng),
[`termscript.h`](../termscript/termscript.h), and the native modules under
[`termscript/stdlib/`](../termscript/stdlib/).

**Runnable** examples use the current parser and runtime. **Illustrative**
fragments describe integration points or require a display, device, or host setup
that is not supplied by the example. Parenthesized calls are used throughout;
historical whitespace calls remain accepted.

## Contents

1. [Build and first program](#1-build-and-first-program)
2. [Command-line reference](#2-command-line-reference)
3. [Language reference](#3-language-reference)
4. [The global module](#4-the-global-module)
5. [Modules and imports](#5-modules-and-imports)
6. [Standard library](#6-standard-library)
7. [Resources and errors](#7-resources-and-errors)
8. [Compilation backends](#8-compilation-backends)
9. [Embedding and native extensions](#9-embedding-and-native-extensions)
10. [Diagnostics and troubleshooting](#10-diagnostics-and-troubleshooting)
11. [Implementation boundaries and references](#11-implementation-boundaries-and-references)

## 1. Build and first program

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The driver is `build/bin/termscript`. After installation, invoke `termscript`
through `PATH`. The `.tsc` extension is conventional, not required by the driver.

Build requirements include CMake, a C11 compiler, POSIX threads, and the vendored
NovoParse generator/runtime. The full stdlib also requires a C++20 compiler,
vendored parsing/shell libraries, and the pkg-config packages `libutf8proc`,
`fribidi`, `fontconfig`, `freetype2`, and `harfbuzz`. Xlib is detected optionally.
The project compiles the bundled libdrm core on enabled Linux builds.

The original native text CRDT is also available independently as
`libtermscript_crdt.a`, without these dependencies. Its C API, Termscript
`std.crdt` bindings, replica-identity rules, versioned snapshot format and
security limitations are documented in
[`stdlib/crdt/README.md`](../termscript/stdlib/crdt/README.md).

| CMake option | Purpose |
|---|---|
| `DOMWEAVE_BUILD_TERMSCRIPT_STDLIB` | Build native stdlib modules and install companions; defaults ON |
| `TERMSCRIPT_X11` | Detect and enable Xlib window support; defaults ON |
| `TERMSCRIPT_DRM` | Build bundled DRM discovery support; defaults ON on Linux |
| `TERMSCRIPT_NATIVE` | Build the QBE/Keystone executable toolchain; defaults ON on Linux x86-64 |
| `TERMSCRIPT_JITERATI` | Build the experimental AMD64 JIT adapter; defaults ON on x86-64 |

Disabling native compilation still permits QBE IL emission. Disabling the stdlib
retains the standalone language and global `G` module. See
[build and installation](../INSTALL.md) and [native compilation](23-native-compilation.md).

Save this **Runnable** program as `hello.tsc`:

```termscript
const message = "hello from Termscript";
const answer = G:add(40, 2);
G:puts(message);
G:puts(answer);
```

Run it with `build/bin/termscript hello.tsc`. Output:

```text
hello from Termscript
42
```

## 2. Command-line reference

```text
termscript [--help] [--backend c|qbe|jiterati] [-c] [-o OUT]
           [--static] [--cc CC] SCRIPT
```

| Invocation | Operation |
|---|---|
| `termscript hello.tsc` | Execute through the tree-walking VM |
| `termscript - < hello.tsc` | Read the complete program from standard input, then execute |
| `termscript -c hello.tsc` | Emit generated C to standard output |
| `termscript -c -o hello.c hello.tsc` | Emit generated C to a file |
| `termscript -o hello hello.tsc` | Compile and link a C-backend executable |
| `termscript --backend c -o hello hello.tsc` | Explicit C-backend selection |
| `termscript --backend qbe -c -o hello.ssa hello.tsc` | Emit QBE IL |
| `termscript --backend qbe -o hello hello.tsc` | Compile/link with QBE and Keystone on supported builds |
| `termscript --backend jiterati hello.tsc` | Execute through the experimental JIT adapter |

`-c` means **emit backend text**, not compile a C object. With `-c`, `-o` names
the emitted file; otherwise `-o` names the executable. `--cc CC` selects the
compiler used for executable links; otherwise the driver uses `$CC`, then `cc`.
`--static` requests a fully static link and requires static dependency libraries.

Jiterati rejects `-c` and `-o`. QBE requires `-c` or `-o`; it is not a VM
execution mode. `--backend c` without either option still executes the VM.

Exit codes are 0 for success, 1 for execution/compilation failure, and 2 for
invalid command-line usage or rejected mode combinations. The driver accepts
one input program and does not expose script arguments or an interactive REPL.

`G:puts` output is buffered by the VM. The driver prints it after successful
execution; it does not print the accumulated buffer when execution fails.
Native module I/O and other side effects are independent of this buffer.

## 3. Language reference

### 3.1 Source text and literals

- Identifiers match `[A-Za-z_][A-Za-z0-9_]*` and are case-sensitive.
- Spaces, tabs, and newlines are layout. A `#` outside a string begins a comment
  extending to the end of its line.
- Declarations, assignments, and expression statements end with `;`.
- Block statements end with `end` and need no trailing semicolon.
- Integers are signed 64-bit decimal values; a leading minus belongs to the
  literal. Floating-point and hexadecimal numeric literals are not implemented.
- Strings use double quotes. There is no interpolation.
- `true`, `false`, and `nil` are literal values.

Language keywords include `const`, `let`, `var`, `fn`, `return`, `use`, `if`,
`else`, `while`, `do`, `then`, `end`, `and`, `or`, `not`, `config` and `lifetime`.
Use other identifiers for bindings and call names.

Supported string escapes are `\n`, `\t`, `\r`, `\"`, `\\`, `\e`, `\a`, `\b`,
`\f`, `\v`, and nonzero `\xHH` bytes. Strings are NUL-terminated byte strings;
embedded NULs are not supported. Literal Unicode text may be written as UTF-8.
`G:len` counts bytes; Unicode character/width operations belong to `std.utf8`.

| Value kind | Meaning | False when |
|---|---|---|
| nil | Missing/empty result | Always |
| bool | Boolean | `false` |
| int | Signed 64-bit integer | 0 |
| float | Finite IEEE-754 binary64 | Positive or negative zero |
| string | Owned byte string | Empty |
| handle | Reference-counted native object or module | Never |

Closing a resource does not turn its handle into nil. Inspect a module's
status API or explicitly replace the binding when appropriate.

### 3.2 Bindings and scope

`name = expression;` creates or updates a mutable binding.
`const name = expression;` creates a binding that cannot be reassigned.
Constants prevent rebinding; a native object held by a constant can still be
mutated through its module's methods.

Top-level bindings share VM scope, including imported files. Named functions
have their own locals and can read top-level globals, not caller locals.
Branches/loops share their enclosing function's frame; there is no block-local
scope or closure capture. Reassigning an existing
constant fails with `TS_ERR_INVAL`. Reading an unbound name fails with
`TS_ERR_NOTFOUND`.

Successive embedding calls on the same VM retain bindings, modules, and search
paths. Each top-level run resets captured output and the step counter. A failed
run does not roll back previously executed assignments or native side effects.

### 3.3 Calls and references

Calls have the form `module_binding:function(arguments)`. The receiver must be
a module handle, usually returned by `G:load`. Function names and receiver names
are identifiers. A native module name such as `"std.json"` is a string passed
to `G:load`, not a dotted receiver expression.

Parenthesized arguments may be nested expressions, including integers, floats,
strings, literal booleans/nil, calls, operators, collection literals and `&name`.
Arguments are separated by commas; a trailing comma is accepted.
Use `M:f()` for a zero-argument call.

`name` and `&name` currently read the same stored value. `&` does not expose a
C address or permit a callback to rebind the variable. Strings are copied when
values are copied; native handles retain aliases to the same object.

Nested calls, parenthesized general expressions and `or` inside an argument
list are supported. Intermediate bindings remain useful for readability:

**Runnable:**

```termscript
const sum = G:add(20, 22);
G:puts(&sum);
```

The equivalent `G:puts(G:add(20, 22));` is also runnable.
Arithmetic and comparisons are module functions, not infix operators.

### 3.4 Fallback expressions

`left or right` evaluates left first. If left is truthy, its value is returned
without evaluating right; otherwise right is evaluated. Chained fallbacks are
left-associative. Calls bind more tightly than `or`.

**Runnable:**

```termscript
const value = nil or false or 0 or "default";
const available = true or G:die("unreachable");
G:puts(value);
G:puts(available);
```

Output is `default` followed by `true`, each on its own line.

`or` handles falsey **successful results**, not exceptions or native error
statuses. A division-by-zero error in the left operand stops execution before
the fallback. Use `G:load("module") or G:die("module missing")` because a
missing registered module is a successful nil result.

### 3.5 Control flow

```text
if expression do
  statements
else
  statements
end

while expression do
  statements
end
```

`else` is optional. Conditions use the truthiness table above. A while condition
is reevaluated before each iteration. There are no `break`, `continue`, `for`,
user-defined functions, or return statements in the shipped grammar.

**Runnable:**

```termscript
counter = 0;
while G:lt(counter, 3) do
  counter = G:add(counter, 1);
  if G:eq(counter, 2) do
    G:puts("two");
  else
    G:puts(counter);
  end
end
```

Output is `1`, `two`, and `3`, on separate lines.

## 4. The global module

Every VM starts with the constant module binding `G`. These functions require
no standard-library registration:

| Function | Contract |
|---|---|
| `G:load(name)` | Registered module handle, or nil if absent; name must be a string |
| `G:import(name)` | Execute a companion in the shared scope; return nil |
| `G:import_path(directory)` | Append a copied search directory to this VM; return nil |
| `G:puts(value)` | Append the value's spelling and a newline to captured output; return nil |
| `G:die(value)` | Stop execution with `TS_ERR_CANCELLED` and the value's spelling as the diagnostic |
| `G:add(a, b)` | Numeric addition, or concatenation of two strings |
| `G:sub(a, b)` | Numeric subtraction |
| `G:mul(a, b)` | Numeric multiplication |
| `G:div(a, b)` | Integer division for two ints; otherwise binary64 division |
| `G:mod(a, b)` | Integer remainder or floating `fmod`; sign follows dividend |
| `G:eq(a, b)` | Numeric int/float equality; other different types compare false |
| `G:lt(a, b)`, `G:gt(a, b)` | Strict numeric comparisons |
| `G:le(a, b)`, `G:ge(a, b)` | Non-strict numeric comparisons |
| `G:float(value)` | Explicit binary64 conversion from number or decimal string |
| `G:int(value)` | Truncate an in-range numeric value toward zero |
| `G:not(value)` | Boolean negation of truthiness |
| `G:len(text)` | Byte length of a string |
| `G:concat(a, b)` | Concatenate the printable spellings of two values |
| `G:type(value)` | `"nil"`, `"bool"`, `"int"`, `"float"`, `"string"`, or `"handle"` |

Numeric arithmetic requires numeric operands. Integer-only operations keep
checked int64 behavior; a floating operand promotes arithmetic to binary64.
Mixed comparisons do not lose int64 precision. Overflow, division by zero, and remainder by
zero fail with `TS_ERR_PARSE`. Arity/type violations fail with `TS_ERR_INVAL`.
`G:eq` compares strings by contents, nil with nil as equal, and handles by
handle identity. Separate loads of the same module create separate handles.

Printable spellings are `nil`, `true`/`false`, decimal integers, round-trippable
17-significant-digit floats (retaining `.0` where needed), strings verbatim,
and native handle descriptions. `G:puts` is not a general binary-output API.
See [migration to 0.4 / ABI v2](../termscript/MIGRATING-0.4.md) for finite-number,
locale, native-module rebuild and JSON/TOML/YAML changes.

## 5. Modules and imports

### 5.1 Native modules versus companions

`G:load("std.json")` looks up a native module already registered on the VM.
It does not search the filesystem or load a shared object.

`G:import("json")` resolves and executes `json.tsc`, which binds `json_m` to
that native module and may initialize additional values. A missing companion
is `TS_ERR_NOTFOUND`. A successful import itself returns nil, so placing
`or G:die(...)` after `G:import(...)` would run that fallback even on success.

**Runnable with the stdlib:**

```termscript
G:import("json");
const J = json_m;
const doc = J:parse("{\"items\":[10,20]}") or G:die("invalid JSON");
const second = J:get(doc, "items.1");
const missing = J:get(doc, "absent") or "not found";
G:puts(second);
G:puts(missing);
```

Output is `20` followed by `not found`.

### 5.2 Resolution and execution

Search order is the VM's directories in insertion order, followed by nonempty
entries in the colon-separated `TERMSCRIPT_PATH` environment variable. In each
directory `DIR`, probing tries:

1. `DIR/NAME.tsc`
2. `DIR/NAME/NAME.tsc`

The first readable match wins. Import names are at most 128 bytes, using
alphanumerics, `_`, `-`, and `.`; separators and the names `.` and `..` are
rejected. The source program's directory is not automatically added.

The full driver and `DT_TermVM` install the stdlib search directories. Standalone
embedders call `ts_stdlib_search_path`, `ts_vm_add_search_path`, or configure
`TERMSCRIPT_PATH`. `G:import_path("directory")` adds an application directory.

Imported code shares variables, output, and the current step budget with its
caller. Import cycles are rejected. Imports are not cached: importing a companion
again executes it again, and its constant declarations may fail on rebinding.
Import once during program initialization.

## 6. Standard library

The bundled registry exposes 69 native modules. Companion names generally match
the suffix of `std.NAME` and bind `NAME_m`. The standalone terminal-capability
module is the exception: `G:import("terminfo")` binds `tinfo_m` to `std.tinfo`.
The Termlib adapter additionally registers `std.terminfo` for full database
profiles; database operations depend on an available terminal entry.

| Area | Companion names |
|---|---|
| Containers and editor storage | `array`, `list`, `map`, `stream`, `buffer`, `edit_history` |
| Structured data and codecs | `codec`, `csv`, `json`, `toml`, `yaml` |
| Unicode and text matching | `utf8`, `bidi`, `cell`, `text_search`, `word_motion`, `regex`, `glob`, `autocomp` |
| Terminal and system I/O | `baudrate`, `termios`, `terminal`, `pty`, `poll`, `signal`, `io`, `env`, `ipc`, `pipe`, `socket`, `key`, `termcap`, `terminfo` |
| Rendering and emulation | `screen`, `vterm`, `font`, `font_discovery`, `drm`, `x11`, `compositor`, `emulator`, `layout`, `tui` |
| Presentation and documents | `color`, `style`, `stylesheet`, `css`, `html`, `syntax`, `verbatim`, `draw`, `rules`, `table`, `widget`, `theme`, `panel`, `menu`, `notify` |
| Interactive input | `readline`, `prompt`, `prompt_string`, `expect` |
| Execution and utilities | `exec`, `shell`, `sched`, `log`, `fsm`, `test`, `http` |

The [stdlib reference](../termscript/stdlib/README.md) describes each module's
operations and ownership. Detailed subsystem references cover
[shell execution](../termscript/stdlib/shell/README.md),
[compositors](../termscript/stdlib/compositor/README.md),
[emulators](../termscript/stdlib/emulator/README.md), and
[poll/X11/DRM](../termscript/stdlib/emulator-backends.md).

### 6.1 Native handle aliases

**Runnable:**

```termscript
const A = G:load("std.array") or G:die("array unavailable");
values = A:new();
alias = values;
A:push(alias, 10);
A:push(values, 20);
const total = A:sum(values);
G:puts(total);
alias = nil;
values = nil;
```

Output is `30`. Both bindings reference the same vector. Replacing the final
alias releases it; no explicit array close operation is required.

### 6.2 File I/O

**Runnable:** creates and removes `message.txt` in the current directory.

```termscript
G:import("io");
const IO = io_m;
const written = IO:write("message.txt", "saved text") or G:die("write failed");
const text = IO:read("message.txt") or G:die("read failed or empty");
G:puts(text);
IO:remove("message.txt");
```

Output is `saved text`. Since empty strings are falsey, this fallback deliberately
treats an empty file as failure. Use a separate result/type check if empty
contents are valid for the application.

### 6.3 Structured process execution

**Runnable on a POSIX host with `printf`:**

```termscript
G:import("shell");
const SH = shell_m;
const sh = SH:new();
const command = SH:command("printf", "%s", "hello from a child");
const output = SH:capture(sh, command);
const status = SH:status(sh);
G:puts(output);
G:puts(status);
SH:close(sh);
```

Output is `hello from a child` followed by `0`. Ordinary strings passed to
`command` are literal argument elements. Explicit `word`, `parse`, and script
execution APIs enable LibShell expansion/parsing semantics.

## 7. Resources and errors

Native resources are carried by reference-counted handles. Copying a handle
retains its underlying object; it does not clone the native resource. The last
reference runs the handle destructor. Mutable assignment releases the old value,
and VM destruction releases all remaining bindings.

Use a module's explicit `close` when timely release is needed. For modules whose
close invalidates the shared object, every alias becomes closed. A borrowed fd
returned by a module remains owned by that resource; follow the module's
descriptor/lifetime contract when watching or passing it to another library.

The execution budget is 1,048,576 charged steps per top-level run; statement and
expression evaluation both charge steps. It is not an iteration count or a
wall-clock timeout. Imports share the caller's budget. Native calls may block
according to their own timeout/cancellation rules. File-based script loading
uses a 1 MiB buffer ceiling, including terminator space. These limits are not
configurable through the public VM API.

Two failure mechanisms must be distinguished:

- A successful native call may return nil, false, or an empty string. Test the
  result or use `or`, subject to the module's contract.
- A non-OK native status stops execution and fills the diagnostic. The runtime
  releases a payload returned by the failing callback before propagating that
  status. There is no script-level catch/exception construct.

Cleanup on VM destruction is deterministic, but native process/terminal modules
define their own shutdown behavior. A failed execution preserves earlier external
side effects. Consult the module reference before assuming that closing a PTY
leaves its child running or that a terminal setting is restored automatically.

## 8. Compilation backends

All backends consume the same backend-independent AST produced from
`Termscript.ng`. Parsing, value types, calls, and control flow are shared.

| Backend | Execution model | Current boundary |
|---|---|---|
| VM | Tree-walking execution | Default CLI mode |
| C | Ordinary C control flow over `ts_rt_*`/`dt_rt_*` | Generated source must link with the runtime and used native libraries |
| QBE | QBE IL plus compiled-code runtime calls | Driver-managed native executables target Linux x86-64, System V ABI/ELF64 |
| Jiterati | Runtime glue with JIT-compiled arithmetic/comparisons | Experimental AMD64 adapter; machine arithmetic is a checked i32 subset |

The C and QBE backends compile the entry program rather than embedding source
for startup parsing. Eligible constant-string `G:import("name")` sites are
resolved during compilation and their companions are inlined. Dynamic imports
and imports that cannot be inlined retain runtime resolution and may parse
imported source then. Native modules are linked and resolved through the VM
registry; the emitters do not embed their implementations.

The driver supplies the native dependency closure when building an executable.
`--static` requests full static linking, rather than removing runtime
dependencies. `-c` emits source/IL only and does not validate a subsequent link.

Jiterati currently JITs `G:add`, `sub`, `mul`, `div`, `mod`, `eq`, `lt`, `gt`,
`le`, and `ge` for the supported i32 machine-code path. Control flow, values,
imports, strings, and other native calls use runtime glue. Unsupported lowering
fails explicitly; do not assume the VM's full int64 arithmetic range is available
through the JIT.

See [the C backend](19-c-backend.md) and
[QBE/Jiterati compilation](23-native-compilation.md) for linkage and entry points.

## 9. Embedding and native extensions

### 9.1 Embedding lifecycle

The standalone header is `termscript/termscript.h`; it does not require
`termlib.h`. Link the CMake target `termscript_standalone` for the VM and parser.
Its CMake dependency closure includes NovoParse and, when enabled, the JIT's C++
runtime. Link `termscript_stdlib` when using the bundled native modules.

1. Initialize `TS_Error` with `ts_error_clear`.
2. Create a VM with `ts_vm_create`.
3. Register custom modules with `ts_vm_register_module`.
4. For the stdlib, call `ts_stdlib_register_all` and `ts_stdlib_search_path`.
5. Validate with `ts_check_syntax`, or execute with `ts_vm_run_string`/`ts_vm_run_file`.
6. Inspect status/diagnostics, free returned output with `free`, and free the VM.

Syntax checking parses without executing, loading modules, or proving symbol
availability. A syntactically valid call can still fail during execution.

The Termlib `DT_TermVM` API wraps the standalone VM and automatically installs
the enabled stdlib plus Termlib's terminal-profile module. Its public interface
is in [`termlib.h`](../termlib.h).

### 9.2 Complete native module example

**Runnable C:** register a module, execute a script, capture `42`, and clean up.

```c
#include "termscript/termscript.h"
#include <stdio.h>
#include <stdlib.h>

static TS_Status
increment(TS_VM *vm, void *userdata, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  (void)userdata;
  TS_Status status = ts_check_argc(vm, argv, argc, 1, error);
  if (status != TS_OK)
    return status;
  if (argv[0].type != TS_INT) {
    ts_error_set(error, TS_ERR_INVAL, 0, 0, "increment requires an integer");
    return TS_ERR_INVAL;
  }
  if (argv[0].as.integer == INT64_MAX) {
    ts_error_set(error, TS_ERR_PARSE, 0, 0, "increment overflow");
    return TS_ERR_PARSE;
  }
  ts_value_make_int(ret, argv[0].as.integer + 1);
  return TS_OK;
}

static const TS_FuncDef functions[] = {
  { "increment", increment, NULL }
};
static const TS_Module module = TS_MODULE_INIT("example", functions);

int main(void)
{
  TS_Error error;
  char *output = NULL;
  ts_error_clear(&error);
  TS_VM *vm = ts_vm_create(&error);
  if (!vm) {
    fprintf(stderr, "%s\n", error.message);
    return 1;
  }
  TS_Status status = ts_vm_register_module(vm, &module, &error);
  if (status == TS_OK)
    status = ts_vm_run_string(vm,
      "const M = G:load(\"example\"); "
      "const answer = M:increment(41); G:puts(answer);",
      &output, &error);
  if (status == TS_OK)
    fputs(output, stdout);
  else
    fprintf(stderr, "%s: %s\n", ts_status_string(status), error.message);
  free(output);
  ts_vm_free(vm);
  return status == TS_OK ? 0 : 1;
}
```

### 9.3 ABI and ownership contracts

ABI version 2 retains v1's size-tagged, counted module layout and adds finite
binary64 values (`TS_FLOAT`, `as.real`). ABI-v1 modules must be rebuilt and must
handle or reject floats explicitly; changing only the version field of an old
binary is unsafe. The original v1 migration replaced the unversioned
sentinel-terminated layout.
Rebuild native extensions against the current header. `TS_Func` callback
signatures and `TS_FuncDef.userdata` remain unchanged.

- Static descriptors use `TS_MODULE_INIT(name, actual_function_array)`.
  Do not pass a pointer or append a NULL sentinel.
- Dynamic descriptors use `ts_module_init(&module, name, funcs, count)`.
  The initializer borrows storage and does not validate it.
- Descriptors carry `struct_size`, `abi_version`, `func_count`, and zero reserved
  fields. An empty module may use NULL functions with count zero.
- `ts_module_validate` checks ABI metadata, count limits, names, callbacks, and
  duplicate function names. Registration validates before publishing. Each
  module supports at most `TS_MAX_MODULE_FUNCS` (4096) entries; the VM registry
  holds at most `TS_MAX_MODULES` (128) modules, including builtins and the stdlib.
- `ts_native_abi_version` queries the runtime ABI.
- `ts_module_find_func` returns a borrowed entry from a validated immutable
  descriptor, or NULL on a miss. It scans only `func_count` entries.

Descriptors, names, and function tables must remain immutable and alive while
registered or referenced by a module handle. Callback userdata is borrowed and
must remain alive; its contents may be mutable. There is no module-unload API.

Callback arguments are borrowed for the duration of the call. Copy values that
must outlive it. `ret` starts as nil, and its payload transfers to the runtime;
the runtime releases it on failure. A direct C caller of a function-table
callback instead owns its return value even on failure. Callbacks release their
intermediate allocations and set useful diagnostics; `error` may be NULL.

Initialize local values as `TS_Value value = {0}`. Constructors copy string
payloads or wrap handles with a destructor and identity tag. Release values with
`ts_value_free`, which accepts NULL. Free an owned destination before calling
`ts_value_copy`; string copies are deep and handle copies retain references.
`ts_value_to_string` returns a heap allocation that the caller frees.

One VM requires serialized use. Distinct VMs may run on different threads;
parsing is internally serialized. Shared callback userdata and module resources
require their own synchronization. Native code executes with host process access;
module ABI validation checks descriptors, not a sandbox boundary.

### 9.4 Compilation APIs

Use `ts_compile_options_init` before `ts_vm_compile_backend`. The default is C;
QBE emits IL. `ts_vm_compile_to_c` and `ts_vm_compile_to_qbe` are direct helpers.
Returned text is borrowed until the next compile on that VM or VM destruction.
Copy it if it must outlive that interval.

`ts_vm_run_backend` selects Jiterati execution. Passing C or QBE to that API
returns `TS_ERR_UNSUPPORTED`; use the text compiler or driver for AOT builds.
`ts_backend_available` reports adapter availability, not whether an external
native executable link will succeed. QBE IL exposes `ts_qbe_run(vm, error)`;
the CLI supplies the executable entry wrapper.

## 10. Diagnostics and troubleshooting

`TS_Error` carries a status, preserved system errno, offset, and a NUL-terminated
message. Syntax diagnostics include the byte position and line/column in their
message. Runtime diagnostics may carry a source line in the offset field;
native modules may use it for subsystem-specific positions. Interpret the
message/category rather than assuming every offset is a source byte index.

| Status | Typical cause |
|---|---|
| `TS_ERR_PARSE` | Malformed source, invalid escape, arithmetic overflow or division by zero |
| `TS_ERR_NOTFOUND` | Unbound variable, missing function/companion, or receiver that is not a module |
| `TS_ERR_INVAL` | Wrong arity/type, constant reassignment, invalid descriptor, or import cycle |
| `TS_ERR_SYSTEM` | OS/native operation failure; inspect `system_errno` |
| `TS_ERR_NOMEM` | Allocation failure |
| `TS_ERR_LIMIT` | Step budget, script/table size, or subsystem resource limit |
| `TS_ERR_UNSUPPORTED` | Unsupported backend, native ABI, or disabled subsystem |
| `TS_ERR_CANCELLED` | `G:die` or a subsystem cancellation |
| `TS_ERR_EOF`, `TS_ERR_TIMEOUT`, `TS_ERR_WOULD_BLOCK` | Module-specific I/O outcomes |

Clear the error before an embedding operation; successful operations need not
clear an earlier diagnostic. Check the returned status before inspecting errors.

- **Nested call syntax error:** bind the inner result, then pass the variable.
- **Missing native module:** verify registration; `TERMSCRIPT_PATH` resolves
  companions and does not register native libraries.
- **Missing companion:** inspect VM paths and `TERMSCRIPT_PATH`; use the two
  supported filename layouts.
- **Constant reassignment after import:** initialize once; imports share scope
  and are not cached.
- **Unexpected fallback:** zero, false, nil, and empty strings are all falsey.
- **No output after failure:** `G:puts` is buffered; the driver prints the buffer
  only on successful execution.
- **AOT link failure:** use the driver-managed link or supply the complete runtime
  and native dependency closure, including the C++ runtime when required.
- **JIT arithmetic rejected:** check build availability and the i32 lowering range.
- **Long-running native I/O:** the VM step budget does not time out a native call;
  use the module's timeout/cancellation facilities.

## 11. Implementation boundaries and references

The section-oriented files under `termscript/examples/` are **Illustrative design
inputs**. `@alias`, `@define`, `.interface`, `.section`, `lifetime`, `config`,
`~>`, `->`, `=>`, `$` variables, object field syntax, and collection literals
are not implemented executable language constructs. Lists, maps, documents,
and resources are manipulated through native module handles.

References:

- [Language chapter](17-termscript-language.md)
- [VM and C API chapter](18-termscript-vm.md)
- [C backend](19-c-backend.md)
- [Standard library chapter](20-standard-library.md)
- [QBE/native compilation and Jiterati](23-native-compilation.md)
- [Full stdlib documentation](../termscript/stdlib/README.md)
- [Public standalone API](../termscript/termscript.h)
- [Termlib adapter API](../termlib.h)

The manual's runnable examples were checked with the current VM, C backend, and
Jiterati adapter. QBE IL emission was checked; QBE executable linking and
interactive X11/DRM execution require the corresponding enabled toolchain or
local resources.
