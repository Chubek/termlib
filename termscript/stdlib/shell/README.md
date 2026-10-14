# std.shell

`std.shell` embeds the bundled LibShell C++20 runtime, text parser and POSIX
executor. Import the companion with `G:import("shell")` (binding `shell_m`), or
load the native module with `G:load("std.shell")`.

**Runnable:**

```termscript
G:import("shell");
const SH = shell_m;
const sh = SH:new();
SH:set(sh, "LABEL", "hello world");
const label = SH:word("\"$LABEL\"");
const print = SH:command("printf", "%s\n", label);
const output = SH:capture(sh, print);
const status = SH:status(sh);
G:puts(output);
G:puts(status);
SH:close(sh);
```

## Execution and graphs

| Function | Result / behavior |
|---|---|
| `new(inherit_environment? = true)` | Owning shell handle; snapshot the host environment, or start with only `PWD` and `IFS` |
| `close(shell)` | Release shell state; idempotent; all aliases become closed |
| `command(argument, ...)` | Immutable simple-command program; alternatively accept one `std.list` of arguments |
| `word(text)` | Parse exactly one shell word, retaining quotes and expansion provenance |
| `expand(shell, word_or_text)` | Expand one word into a fresh `std.list` of strings; text is parsed as a shell word |
| `parse(script)` | Parse and validate shell text into an immutable program |
| `pipeline(command, ...)` | Immutable pipeline of simple commands; alternatively accept one `std.list` of command handles |
| `sequence(left, right, connective? = "sequence")` | Combine programs; connectives are `sequence`, `and`, `or` |
| `redirect(program, stream, path, mode?)` | Return a new command/pipeline with file redirection; streams are `stdin`, `stdout`, `stderr`; modes are `read`, `truncate`, `append` |
| `run(shell, program_or_script, input?)` | Execute synchronously, inheriting standard streams; return the exit code |
| `capture(shell, program_or_script, input?)` | Execute synchronously, capturing stdout and stderr; return stdout verbatim, including trailing newlines |
| `status(shell)` | Exit code from the most recent execution; initially zero |
| `stdout(shell)`, `stderr(shell)` | Copies of the most recently captured streams; empty after `run` |
| `result(shell)` | Fresh document handle, readable with `std.json:get`; independent of later executions/close |

Ordinary string arguments to `command` are **literal argv elements**, including
empty arguments, whitespace, dollar signs and metacharacters. Only `word`
handles request expansion. Builders do not execute. Words and programs retain
their own data and remain usable after their original strings, lists, component
handles or defining shell are released.

Passing shell text to `parse`, `run` or `capture` enables LibShell's shell
grammar, including assignments, expansions, command substitution, pipelines,
boolean lists, redirections, conditionals, loops and functions. These are
LibShell semantics rather than a delegation to `/bin/sh`. Native builtins use
the same execution path as external commands. Explicit command redirections
override capture/input defaults. For `redirect`, stdin targets a pipeline's
first stage; stdout/stderr target its last stage. The default mode is `read`
for stdin, `truncate` for outputs. Paths are literal and relative to the shell
cwd. Output capture also covers lists, branches, loops and functions.

Descriptor numbers in parsed fd redirections are borrowed. The caller must
keep those descriptors valid until execution completes; programs do not retain
them. The external executor owns temporary duplicates, not caller descriptors.

`input` is an optional string, supplied without an added newline. Without it,
stdin is inherited even during capture. External-only pipelines use OS pipes;
LibShell buffers hand-offs in pipelines containing in-process builtins.

## Shell state

| Function | Result / behavior |
|---|---|
| `get(shell, name, fallback?)` | Copy of a variable; nil or a copy of `fallback` when absent |
| `set(shell, name, value, exported?)` | Assign a string, respecting readonly variables; preserve the existing export attribute by default; new variables are unexported |
| `unset(shell, name)` | Remove a variable; missing names are harmless, readonly names fail |
| `export(shell, name, enabled? = true)` | Change its export attribute; an absent variable is created with an empty value |
| `cwd(shell)` | Copy of the shell's current working directory |
| `cd(shell, path)` | Validate/change the shell cwd and update `PWD`/`OLDPWD` |
| `args(shell, list?)` | Set positional parameters when a list is supplied; return a fresh list of their current values |
| `pipefail(shell, enabled?)` | Set/query pipeline failure policy; false selects the last stage, true selects the first failed stage, matching LibShell's `any_failed` policy |
| `timeout(shell, milliseconds?)` | Set/query external command/pipeline deadline; zero disables; range `0..INT32_MAX` |
| `output_limit(shell, bytes?)` | Set/query each captured stream's byte limit; initially 1 MiB; range `1..64 MiB` |

The host process's cwd and environment are not changed. Exported variables are
copied into external children's environments. Shell functions and variables
persist across calls. `$0` is `termscript`; positional parameters and `$?`
are maintained by LibShell.

## Status and errors

The result document contains:

- `code`, `ok`, `signaled`, `signal` (integer or null), `timed_out`, `cancelled`;
- `pid` (integer or null), `stdout`, `stderr`;
- `pipeline`: an array of the same status fields for each pipeline stage;
- `diagnostics`: an array of LibShell `code`, `message`, `path` objects.

Nonzero child exits are ordinary results. Signal termination is `128 + signal`
and sets `signaled`/`signal`. Missing/nonexecutable commands produce 127/126;
external command deadlines produce 124 and set `timed_out`. Child processes
are reaped before execution returns. The report describes the last executed
command/pipeline in a compound program.

Arity/type/closed-handle/graph errors return `TS_ERR_INVAL`; malformed shell
text returns `TS_ERR_PARSE`; allocation failures return `TS_ERR_NOMEM`;
runtime system failures return `TS_ERR_SYSTEM` with the available errno.
Diagnostics are copied into `TS_Error`, and C++ exceptions are contained at
the native ABI boundary. Parsing currently supplies offset zero because
LibShell's parser diagnostics do not expose a source byte offset.

Limits apply to strings (1 MiB), argv/stage/positional counts (4096), and command
graphs (128 levels, 8192 node occurrences). Parser preflight also limits each
source to 8192 tokens, 128 compound-command introducers, 128 list connectives,
and 128 `$(` occurrences (including literal occurrences), bounding the vendored
parser/lexer recursion and cleanup depth. Capture overflow returns
`TS_ERR_LIMIT`; captured embedded NUL bytes return `TS_ERR_UNSUPPORTED`, since
Termscript strings are NUL-terminated. After a capture failure, already
captured text remains available. Captures continue draining external streams
before reporting an overflow; the output limit bounds retained bytes, while
`timeout` bounds external execution. Timeouts apply to individual external
commands/pipelines, not an entire script or in-process loop.

Shell operations are synchronous and require serialized access to each shell
handle. There is no script-side asynchronous cancellation operation. Independent
shells on independent VMs can execute concurrently. Synchronize host environment
mutation with shell creation. Destructors accept the internal null/partially
initialized state; `close` accepts a shell handle, not nil. Graph and word
handles need only ordinary Termscript value cleanup.

## Building and embedding

The CMake build compiles LibShell's parser into `libtermscript_stdlib` and uses
its header-only runtime/ POSIX executor. Public headers remain C-compatible.
The standard library requires a C++20 compiler and C++ runtime; CMake targets,
the Termscript AOT driver, and `termscript_stdlib.pc` include that dependency.
Generated Termscript source is still C. LibShell's QaMRpp adapter, CLI main,
packaged kernels and scripting-language extensions are separate components.
