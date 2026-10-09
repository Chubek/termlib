# Emulator front-end modules

These modules complement `std.pty`, `std.vterm`, `std.screen`, `std.font`,
`std.utf8`, and `std.key` when emulator policy lives in Termscript and its
renderer or platform integration lives in C.

`G:import "x11"`, `G:import "drm"`, and `G:import "poll"` bind `x11_m`,
`drm_m`, and `poll_m`. Alternatively use `G:load "std.x11"`, etc.
All three modules work in the VM, generated C, and the QBE native backend.

## Building

* `TERMSCRIPT_X11=ON` (default) detects Xlib through pkg-config `x11`.
  Without Xlib, the module remains registered but `available()` is false.
* `TERMSCRIPT_DRM=ON` defaults on Linux and compiles core sources from
  `third_party/libdrm` into the stdlib archive, without system libdrm.
  Other platforms default to the disabled backend.
* `std.poll` uses POSIX poll and the monotonic clock.

Use `-DTERMSCRIPT_X11=OFF -DTERMSCRIPT_DRM=OFF` for a headless build.
Disabled backend operations return `TS_ERR_UNSUPPORTED`. `available()` reports
build support, not whether a display/device can currently be opened. Driver
link flags and installed pkg-config metadata include the Xlib dependency.

## Ownership, errors, threading

Arguments are borrowed for the call. New/open functions return owned,
reference-counted handles. Copies alias the same object; closing an alias
closes every alias. Close is idempotent on a correctly typed handle; nil and
wrong types are argument errors. Last-reference release also frees resources.
Returned strings, lists, and document handles own their data and survive later
calls and closing their source.

`fd` returns a **borrowed** descriptor. Do not close it, change its flags, or
read an Xlib connection directly. Remove watches before closing their owners.
`drm.from_fd` duplicates its input, leaving ownership of the original with the
caller. Its duplicate shares the original open-file description and flags;
the module does not modify those flags.

Each object and its aliases require external serialization. Distinct poll sets
and DRM handles may be used on different threads. By default keep **all** Xlib
activity on one thread. A multithreaded embedding host must call `XInitThreads`
before any Xlib operation and still serialize each Termscript object. This
module installs no global Xlib handlers: the host's existing protocol/error
handlers apply, including Xlib's default fatal connection-loss behavior.

Failures return `TS_Status` with `TS_Error` diagnostics and the relevant errno
for OS failures. The Termlib adapter maps these to `DT_Status`/`DT_Error`.
Normal timeout/empty-event conditions use the results described below.

## `std.poll`

| Call | Result |
|---|---|
| `new` | Empty readiness-set handle |
| `watch set fd mask` | Insert/update borrowed descriptor; true |
| `remove set fd` | True if removed, false if absent |
| `wait set timeout_ms` | Owned `std.list`: flat pairs `[fd, mask, fd, mask, ...]` |
| `close set` | Clear watches and mark closed; nil; closes no descriptors |

Input masks: **1** readable, **2** writable, **3** both. Output additionally
uses **4** hangup, **8** error, **16** invalid descriptor. Bits can combine:
drain readable data before treating a simultaneous hangup as EOF. Readiness
is a hint; use nonblocking transport I/O and handle short transfers/would-block.

Timeout is -1 for indefinite wait, 0 for a probe, or milliseconds up to
`INT_MAX`. Timeout returns an empty list. `EINTR` retries preserve the monotonic
deadline. Maximum 1024 descriptors (`TS_ERR_LIMIT` beyond that). Descriptor
values must fit a nonnegative C int. Order is not stable after removal.

For cancellation, watch a host-owned pipe/eventfd and write to it from the
cancelling thread; do not concurrently mutate the poll handle. An empty set
can be used as a timed sleep; an indefinite empty wait has no descriptor wakeup.

## `std.x11`

Each handle owns a display connection, mapped top-level window, and GC.
The default visual must be TrueColor. Connections are close-on-exec.

| Call | Result |
|---|---|
| `available` | Boolean build support |
| `open title width height [display]` | Window; empty/omitted display uses `$DISPLAY` |
| `fd window` | Borrowed connection fd |
| `pending window` | Queued event count after checking the connection |
| `event window` | Copied event document, or nil if none pending |
| `fill window x y width height rgb` | Queue solid rectangle; nil |
| `image window x y width height pixels` | Queue RGB image from `std.list`; nil |
| `flush window` | Flush X requests; nil |
| `close window` | Release GC/window/connection; nil |

RGB integers are `0..16777215` (`0xRRGGBB` in C). Image pixels are row-major
and copied during the call; list length must equal width × height. Images are
bounded to 1,048,576 pixels. Dimensions are `1..32767`, coordinates
`-32768..32767`, titles at most 4096 bytes. A host renderer can shape/rasterize
glyph runs, compose RGB pixels, and upload them with `image`.

Events are `std.json`-compatible documents. Use `J:get event "type"`, etc.
Every event contains `type` (string) and `code` (X event number):

| Type | Additional fields |
|---|---|
| `expose` | `x`, `y`, `width`, `height`, `count` |
| `resize` | `width`, `height` in pixels |
| `key_press`, `key_release` | `keycode`, `keysym`, `state`, `text` |
| `button_press`, `button_release` | `button`, `state`, `x`, `y` |
| `motion` | `state`, `x`, `y` |
| `focus_in`, `focus_out` | none |
| `close` | WM_DELETE_WINDOW request; caller decides when to close |
| `destroy` | Externally destroyed window; only close remains valid |
| `other` | Unclassified X event |

State and keysym retain X11 values. Key text uses `XLookupString` (legacy
single-byte input); Unicode composition/IME belongs in the host input layer.
No script callbacks are invoked.

**Drain Xlib's queue before polling its fd**: buffered events need not leave
the socket readable. Event retrieval never waits for a new event. Connection
setup and request flushing can block on transport backpressure; they have no
Termscript timeout/cancellation facility.

See [`x11/window.tsc`](x11/window.tsc). Add a PTY fd to the poll set, feed its
output into `std.vterm`, redraw exposed/changed cells, and propagate pixel
resizes to the PTY after converting to rows/columns.

## `std.drm`

Device/display discovery supports a host-owned KMS renderer. Modesetting,
master acquisition, buffer allocation, and page flipping are host operations.

| Call | Result |
|---|---|
| `available` | Boolean build support |
| `open path` | Validated DRM handle: read/write, nonblocking, close-on-exec |
| `from_fd fd` | Validated handle owning a close-on-exec duplicate |
| `fd device` | Borrowed fd for host integration |
| `driver device` | Copied kernel driver name |
| `cap device name` | Integer value; nil if kernel query unsupported |
| `connectors device` | Copied array document with connector/mode snapshots |
| `close device` | Close owned fd; nil |

Capabilities: `dumb_buffer`, `prime`, `timestamp_monotonic`, `async_page_flip`.
Unknown names are argument errors; prime returns the kernel capability bitmask.

Connectors contain `id`, numeric DRM `type`, `type_id`, `encoder`, `connected`,
`width_mm`, `height_mm`, and `modes`. Modes have `name`, `width`, `height`,
`refresh_hz`, and `preferred`. Navigate with e.g.
`J:get snapshot "0.modes.0.width"`. Limits: 256 connectors, 4096 modes each.
Re-query after hotplug. Disappearance during a query reports an error rather
than a partial snapshot. KMS discovery generally needs a primary `cardN` node;
render nodes may reject it.

Open/duplication retry EINTR; libdrm retries interrupted ioctls internally.
Discovery ioctls are synchronous without Termscript cancellation. Close does
not retry, avoiding descriptor-reuse hazards. See [`drm/discover.tsc`](drm/discover.tsc)
and adjust its node for the local machine.

## Tests

Regular tests use pipes and invalid/unavailable backends, requiring neither
a display nor a DRM device. They cover readiness, timeout/EINTR, hangup, invalid
fds, limits, aliases, cleanup, and VM/C/QBE behavior. With `xvfb-run` installed,
CTest adds window/image/event/cleanup coverage. Manual integration checks:

```sh
build/test_termscript_emulator --x11
build/test_termscript_emulator --drm /dev/dri/card0
```
