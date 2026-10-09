# `std.compositor`: an in-process surface compositor

This is a Wayland-style **surface/commit/input model** for embedded Termscript
clients. It is an in-process API, not a Wayland wire-protocol server: existing
Wayland clients cannot connect to it. Rendering is software ARGB composition
into a headless framebuffer or a nested X11 window. `std.drm` supplies discovery
for an embedding host's KMS output integration; this module does not take DRM
master or perform modesetting.

```termscript
G:import "compositor";
const C = compositor_m;
const output = C:new 640 480 "My compositor";
const surface = C:surface output 100 50;
C:fill surface 4294901760; # opaque red, ARGB 0xffff0000
C:position surface 20 30;
C:commit surface;
const serial = C:present output;
```

Omit the title for headless operation. The module uses `std.x11` for the nested
window and event source, `std.poll` for readiness/deadlines, and `std.drm` for
connector snapshots. No additional external dependency is introduced.

## API

| Call | Result / behavior |
|---|---|
| `new width height [title]` | Owned compositor handle; optional title selects X11 |
| `surface compositor width height` | Owned, initially unmapped surface handle |
| `id surface` | Stable positive ID, unique within its compositor; never reused |
| `attach surface pixels` | Copy a row-major `std.list` of straight-alpha ARGB integers |
| `fill surface argb` | Stage a uniform ARGB buffer |
| `position surface x y` | Stage a surface position |
| `resize surface width height` | Stage new dimensions and a transparent buffer |
| `commit surface` | Publish pending pixels, dimensions, and position together |
| `raise surface` | Immediately move surface to the top of the stack |
| `destroy surface` | Unmap/remove surface and release its buffers; idempotent |
| `focus compositor surface` | Set keyboard focus; nil clears focus |
| `present compositor` | Compose committed state and flush output; return frame serial |
| `frame compositor` | Owned RGB pixel-list snapshot of the most recent composition |
| `damage compositor` | Request a repaint without changing surface state |
| `output_size compositor width height` | Resize logical framebuffer; does not resize the X window |
| `info compositor` | Owned document: width, height, surfaces, serial, focus, backend |
| `event compositor timeout_ms` | One owned routed event document, or nil on timeout |
| `inject compositor event` | Copy an event document into the bounded input queue |
| `watch compositor fd mask` | Borrow an external fd and register read/write interest |
| `unwatch compositor fd` | Remove external watch; return whether it existed |
| `outputs drm_device` | Connector/mode snapshot through `std.drm.connectors` |
| `close compositor` | Invalidate surfaces, release output, watches, buffers; idempotent |

Mutating operations otherwise return nil (`watch` returns true). ARGB is
`0xAARRGGBB` in C, represented as a nonnegative integer up to 4294967295 in
Termscript. Alpha is straight, not premultiplied. The output background is
opaque black; frame snapshots contain RGB integers in `0..16777215`.

Attach/fill/resize replace pending buffer state only. Commit applies buffer and
position state atomically from the compositor's perspective; it never draws
on its own. Pixels are copied, so the caller can immediately reuse its list.
Committed buffers remain unchanged when later pending operations fail.
New surfaces are unmapped until the first buffer commit. Pending geometry
changes alone do not map a surface. A new committed surface appears above
existing surfaces. Composition clips surfaces to the output and blends bottom
to top. Raising a surface does not implicitly change keyboard focus.

Damage currently invalidates the whole output. A successful repaint increments
the serial; presenting a clean scene returns the previous serial. This is a
synchronous software submission serial, not a GPU completion fence or vsync
timestamp. X11 upload uses bounded horizontal bands and follows Xlib's flushing
semantics; multiple uploads are not a tear-free swapchain.

## Events and event-loop integration

Drain events and update surfaces before presenting a frame. `event` drains the
injection queue, then Xlib's buffered queue before waiting on descriptors. It
uses the timeout/EINTR semantics of `std.poll`: -1 waits indefinitely, 0 probes,
positive values are milliseconds up to `INT_MAX`. Timeout returns nil.

X11 event documents retain the fields described in
[`../emulator-backends.md`](../emulator-backends.md), plus `surface` (ID or 0).
Pointer events hit-test mapped surface rectangles from top to bottom and add
`local_x`/`local_y`; transparent pixels still participate in rectangular hit
testing. Button presses focus their target. Key events target keyboard focus.
There is no pointer grab or cross-process seat protocol. Resize/close/expose
events apply to the output and have surface 0. Expose marks the scene dirty.
The application decides its surface layout after resize and when to close.

External readiness produces `{"type":"ready","fds":[fd,mask,...]}` using
the `std.poll` masks. Watches borrow descriptors; remove them before closing
their owners. A ready descriptor may become ready again until serviced.
For cancellation, register a host-owned pipe/eventfd and signal it from another
thread; do not mutate this object concurrently. Headless injection is useful
for deterministic tests but does not wake a concurrent blocking wait.

`inject` requires an object with a string `type`. It copies the document;
routing uses the scene/focus at delivery time. This is a trusted in-process
input facility, not an authenticated remote input protocol.

## Ownership, lifetime, and limits

Handles are reference-counted. Surface handles retain their compositor, while
the compositor keeps weak scene references to avoid cycles. Dropping the last
surface reference destroys that surface. Dropping a compositor handle alone
does not close outputs still retained by surfaces. Explicit compositor close
invalidates all of its surfaces; their later destruction/close remains safe.
Handle aliases share state. Closing accepts only the correct handle type, not
nil. No operation transfers ownership of a caller's descriptor or argument.
Returned pixel lists and documents are independent owned snapshots.

Limits per compositor:

* 64 live surfaces, 64 queued injected events, 16 KiB serialized per event.
* Output and each surface: up to 8,388,608 pixels and dimensions 1..32767.
* Combined pending and committed surface buffers: 33,554,432 pixels (128 MiB).
* Positions: -32768..32767 on each axis.

The output framebuffer, returned snapshots, and temporary upload/copy storage
are additional bounded allocations. Large snapshots can consume substantially
more memory than packed pixel buffers because lists hold full Termscript values.
Resource exhaustion returns `TS_ERR_LIMIT` or `TS_ERR_NOMEM`; malformed handles,
dimensions, foreign focus targets, and closed objects return `TS_ERR_INVAL`.
Backend errors preserve their original status/diagnostic.

Objects and aliases are single-threaded or externally serialized. The Xlib
threading and fatal connection-loss policies of `std.x11` apply to nested
outputs. Headless compositors on distinct VMs can operate independently. The
library owns no worker threads or hidden global display state.
