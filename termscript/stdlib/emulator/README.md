# `std.emulator`: composited terminal sessions

The emulator combines `std.compositor`, `std.pty`, `std.vterm`, `std.font`, and
`std.poll`. Each instance owns a VT screen, a font, one compositor surface, and
optionally a nonblocking PTY child. It can also render host-fed terminal output
without spawning a process.

**Illustrative:** requires an X11 display and an interactive shell session.

```termscript
G:import("compositor");
G:import("emulator");
const output = compositor_m:new(800, 480, "Termscript terminal");
const terminal = emulator_m:new(output, 30, 100);
emulator_m:spawn(terminal, "exec /bin/sh");
# Call emulator_m:step(terminal, timeout_ms) in the application event loop.
```

See [`terminal.tsc`](terminal.tsc) for a complete windowed example and
[`headless.tsc`](headless.tsc) for a script that captures a finite child into
a VT screen. Both can run under the VM or be compiled with the C/QBE backends.

## API

| Call | Result / behavior |
|---|---|
| `new(compositor, rows, cols, font?, cell_width?, cell_height?)` | Owned emulator; default monospace, 8×16 pixels |
| `surface(emulator)` | Retained alias to the emulator's compositor surface |
| `spawn(emulator, command)` | Spawn `/bin/sh -c command` with initial PTY geometry; nil |
| `feed(emulator, text)` | Feed terminal output directly into VT parser; byte count |
| `input(emulator, text)` | Copy UTF-8/input bytes into the PTY input queue; byte count accepted |
| `pump(emulator, timeout_ms?)` | Service nonblocking PTY input/output; count of output bytes read |
| `render(emulator)` | Rasterize changed VT screen and commit the surface; nil |
| `resize(emulator, rows, cols)` | Update VT/surface geometry and PTY size; nil |
| `handle(emulator, event)` | Translate a routed key press for this surface; true if queued |
| `step(emulator, timeout_ms)` | Single-terminal event/pump/render/present convenience; event or nil |
| `text(emulator)` | Owned plain-text screen snapshot |
| `fd(emulator)` | Borrowed PTY master fd, -1 before spawn |
| `status(emulator)` | `idle`, `running`, or `eof`; independent of child exit status |
| `wait(emulator)` | Owned child-state document, or nil if no update/child |
| `pending(emulator)` | Number of queued unsent input bytes |
| `close(emulator)` | Destroy surface, terminate/reap live child, release resources; idempotent |

Font is a Fontconfig pattern. Cell dimensions are 4..256 pixels. Total surface
pixels cannot exceed 8,388,608, and pixel width/height cannot exceed 32767.
New emulators receive keyboard focus. Use the surface alias to position or
raise a terminal within a shared scene.

Spawn may be called once per instance. It follows `std.pty` process-group and
close semantics. This change also extends `std.pty.spawn` with optional
`rows cols` arguments, set on the slave **before fork/exec**, so early child
size queries see correct geometry. Later resize uses `TIOCSWINSZ`, allowing
the kernel to notify the foreground process group with SIGWINCH. Resizing
preserves overlapping VT cells and parser state. Allocations are prepared
before publishing the new VT state; a failed resize does not commit the staged
surface buffer.

## Event loops

`step` is for one terminal filling an output. It paints an initial frame,
obtains one compositor event, handles keys for its surface, maps output resize
to rows/columns, pumps the PTY without further waiting, renders changes, and
presents. Close/destroy events are returned immediately for the application to
handle. EOF forces subsequent steps to probe rather than block indefinitely;
inspect status/wait to decide when to exit. Step does not automatically close
the emulator or shared compositor.

For multiple terminals, control the loop explicitly:

1. `C:event(compositor, timeout)` obtains an output/input/readiness event.
2. Dispatch `E:handle(terminal, event)` to each terminal; only the focused target
   accepts key presses. Manage layout and call `E:resize` yourself.
3. Call `E:pump(terminal, 0)` and `E:render(terminal)` for each terminal.
4. Call `C:present(compositor)` once.

PTY fds are automatically registered with the compositor's poll set at spawn
and removed at EOF/close. Rendering a terminal commits its own surface but does
not present the shared scene. The `step` convenience assumes full-output layout;
use the explicit loop for multiple windows/surfaces.

## I/O and process semantics

Input is copied into a bounded 65,536-byte queue. Acceptance means queued, not
necessarily written. `pump` handles partial writes and retains unsent bytes
on EAGAIN/EWOULDBLOCK; oversized input returns `TS_ERR_LIMIT` without accepting
a prefix. Empty/closed PTY input is rejected. EOF discards input that can no
longer be delivered. Queue readiness includes writability only when needed.

Pump timeout defaults to 0; -1 is indefinite, otherwise milliseconds up to
`INT_MAX`. Waiting uses `std.poll` monotonic deadlines and retries EINTR.
Reads/writes retry EINTR and stop on would-block. At most 256 KiB of PTY output
is processed per pump to bound event-loop work. The raw-byte VT path handles
embedded NULs and UTF-8/escape sequences split across reads. Direct script
`feed`/`input` retain Termscript's NUL-terminated string representation; a
single direct feed is limited to 1 MiB.

PTY EOF (including Linux PTY EIO) and child termination are distinct. Drain
output before closing. `wait` follows `std.pty.wait_event`: documents have
`state` (`exited`, `signalled`, `stopped`, `continued`), `code`, and `signal`.
Final status remains inspectable while the emulator is open. System failures
preserve errno. Cleanup releases partially initialized objects and kills/reaps
the live child according to the PTY ownership contract.

Use the compositor's watched cancellation fd for cancellable application loops.
Do not concurrently close a handle to interrupt a blocking operation. X11
transport operations retain the underlying Xlib blocking/error semantics.

## Rendering and input scope

The VT behavior is the existing `std.vterm` subset, including its styled cell
grid and alternate-screen behavior. FreeType rasterizes HarfBuzz-shaped cell
grapheme clusters. Wide cells are clipped to their two-cell span, combining
characters stay in their leading cell, and missing glyphs use the selected
font's missing-glyph rendering. Colors include ANSI/256-color/RGB, reverse,
dim, hidden, underline, and strike attributes. A steady underline cursor marks
the VT cursor. Bold/italic font selection, blinking, cross-cell ligatures,
paragraph bidi layout, scrollback, and additional VT replies can be supplied
by higher-level renderer/parser extensions; this is not a full xterm clone.

The default key translator handles printable X11 Latin-1 text (converted to
UTF-8), Enter/Tab/Backspace/Escape, arrows, Home/End, Insert/Delete, PageUp/Down,
Alt prefixes, and Ctrl-Space/Ctrl-@ NUL. Navigation uses normal cursor-mode
sequences. Use `input` for IME UTF-8 commits, richer key maps, application-mode
sequences, bracketed paste, or mouse-reporting policies. Other events remain
available to the embedding host.

## Ownership and concurrency

Arguments are borrowed; all returned strings/documents are owned snapshots.
Surface aliases are retained, share state, and are explicitly invalidated by
emulator close. Do not independently resize/destroy an emulator's surface while
it is rendering; use emulator resize/close. The emulator retains its compositor
but never closes a shared compositor. Explicit compositor close invalidates
its surfaces; close associated emulators to release their children as well.
Borrowed fds must not be closed, have flags changed, or be read/written directly
while the emulator services its I/O.

Handle aliases share state. Close is idempotent for a correctly typed handle;
other operations on closed emulators fail. Last-reference destruction closes
the emulator. Operations are single-threaded or externally serialized, and
inherit the font, PTY, VT, and Xlib contracts. No background thread or hidden
global session state is introduced. Termscript's existing step budget still
applies to scripts running long-lived event loops.
