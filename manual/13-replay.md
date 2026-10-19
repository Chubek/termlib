# Chapter 13: Replay

Step mode is useful for deterministic tests. Release an event early when its borrowed payload is no longer needed.

## Source export

`dt_replayer_compile` generates a replay program from the current recording
position to EOF, regardless of pacing mode. Call `dt_replayer_rewind` first to
export the whole recording. The returned text is borrowed until the next
**successful** compilation or replayer destruction. A failed compilation returns
`NULL`, preserves the previous program, and may advance the stream; rewind before
retrying. Invalid target values are rejected without consuming input.

Exports are bounded to 65,536 events, 16 MiB of aggregate payload and 128 MiB of
generated source (including the template). Allocation failure never returns
silently truncated code. The optional template substitutes its first
`{{events}}` placeholder with the **entire generated program**, or is prepended
if no placeholder exists. This is source composition, not HTML sanitization.

### Termscript

The generated program loads `std.stdio` and writes only OUTPUT events through
`write_hex`, then flushes. It is unpaced and binary-exact: recorded input is not
echoed, NUL bytes survive, and no newlines are added. Resize/metadata/input events
are not applied. The standard library is required at execution/link time.

**Runnable binary-output example** (emits NUL, `a`, byte 255 and a newline):

```termscript
const S = G:load("std.stdio");
S:write_hex("0061Ff0a");
S:flush();
```

### JavaScript / Node.js

The source exports `{events, replay}` through CommonJS, or
`globalThis.TermlibReplay` in a browser. Each event retains an exact BigInt
nanosecond timestamp, numeric type, rows/columns and a byte-valued `data` string.
`replay({onEvent, speed, now, sleep})` calls `onEvent(event, Uint8Array)` in record
order. `speed` must be finite and positive. Defaults use `performance.now()` and
`setTimeout`; timers are chunked to their supported range. Timing is relative to
the first event, callback time counts toward the deadline, and backwards
timestamps never reorder events. Executing the file with Node writes only OUTPUT
bytes to stdout. Importing it does not automatically replay anything.

### WebAssembly

The WASM target emits **WAT text**, not a binary module. Assemble with `wasm-as`
or `wat2wasm`, then instantiate with an imported function:

```text
termlib.event(type:i32, timestamp:i64, offset:i32, size:i32,
              rows:i32, columns:i32)
```

Calling exported `replay()` invokes that callback once per event, in record
order, including input, metadata, heartbeat and resize events. Bytes are in the
exported `memory`; offsets are contiguous and memory covers the entire payload,
including records larger than 64 KiB. Timestamps preserve their unsigned 64-bit
bit pattern; JavaScript hosts use `BigInt.asUintN(64, timestamp)`. The host supplies
pacing and any terminal behavior. Replaying again repeats the same events.

### WebGPU

The generated source exports `{events, replay}` like JavaScript, but its API is
`replay(device, onBuffer, options)`. For each OUTPUT event it allocates a
four-byte-aligned `COPY_DST | STORAGE` buffer, uploads exact output bytes with
zero padding, and awaits `onBuffer(buffer, event, byteLength)`. The callback may
submit rendering/compute work using that buffer. After queue completion the
buffer is destroyed, including callback/write failure paths; **do not retain it**.
Device size limits are checked before allocation. `options` supports the same
clock/speed settings and an optional `onEvent` observer for all event types.

This is a functional upload/consumer adapter, **not a supplied terminal renderer**.
It requires a real WebGPU device, `GPUBufferUsage`, and a consumer callback. It
never executes arbitrary shaders or renders to a canvas implicitly.

### Manim and Processing

MANIM produces a Python `Replay(Scene)` class. PROCESSING produces p5.js global
`setup`/`draw` functions, not Java Processing source. Both pace output text using
recorded timestamps, decode UTF-8 incrementally with replacement for invalid
bytes, display control bytes as replacement characters, and retain only the last
65,536 decoded characters. p5.js applies resize events to a canvas bounded to
4096 pixels per dimension. Manim preserves resize metadata but does not resize
the scene. These are text previews, not ANSI cursor/color emulators. Manim and
p5.js must be provided by the host.

## Validation and security

`tests/test_replay_export.c` exercises all targets, empty recordings, templates,
limits and deterministic allocation failures. `tests/replay_exports.py` and
`tests/replay_exports.js` execute generated JavaScript and assembled WASM, compare
binary Termscript output through VM/C/QBE/Jiterati, and use isolated GPU/p5/Manim
fixtures to check timing, data and cleanup. Real GPU/display rendering is **not**
verified by those mocks. The runtime test is registered when Python, Node and a
WAT assembler are available; core generation tests do not require them.

Exporting never executes recorded data. Generated source encodes payloads instead
of inserting raw bytes into code. Running a byte-exact replay can still send
untrusted terminal control sequences to a real terminal. Do not execute generated
programs or display untrusted recordings implicitly. Compilation is not a sandbox.

## Scope

This chapter documents the stable Termlib contract and the decisions applications must make when using it.

## Recommended pattern

Initialize option structures with their `*_options_init` function, check every returned status, and release each owned handle with its matching cleanup function. Keep borrowed pointers and file descriptors valid for the lifetime promised by the API.

## Reference

See `termlib.h` for exact signatures and `termlib.hpp` for the header-only C++ facade.
