# Native input and Wayland connections

These modules expose low-level, owned resources through the existing native ABI.
All arguments are borrowed for the call; handles share aliases; returned records
and lists own copies. `fd(handle)` borrows a descriptor and never transfers it.
Do not close or perform competing I/O through a borrowed descriptor. Serialize
shared-handle operations, including close. No import or `available()` call opens
a device, injects input, connects to a compositor or starts a thread.

## `std.evdev`

Linux-only native `struct input_event` streams. Not a terminal keymap/escape
decoder, recording wire format, or portable serialized event representation.

| Operation | Contract |
|---|---|
| `available()` | Build/platform capability, not device existence or permissions |
| `open(path)` | Open read-only, nonblocking, close-on-exec; require a character device answering `EVIOCGVERSION` |
| `from_fd(fd)` | Duplicate a borrowed, already-nonblocking readable native event stream |
| `fd(handle)` | Borrow the owned duplicate/device descriptor for readiness integration |
| `read(handle)` | Next copied DOM record: `type`, `code`, `value`, `seconds`, `microseconds` |
| `name(handle)` | Copied kernel device name, bounded to 255 bytes |
| `has_key(handle,code)` | Whether `EVIOCGBIT(EV_KEY)` advertises a key capability |
| `pressed(handle,code)` | Current `EVIOCGKEY` pressed-state snapshot for a key |
| `close(handle)` | Idempotently close descriptor; all aliases become closed |

`open` rejects a final symlink and regular files; it does not silently treat
arbitrary file contents as an event device. Ancestor paths are trusted caller
configuration. `from_fd` deliberately accepts a **trusted native event stream**,
including pipes for fixtures; it does not claim that the source is a kernel
device. Device queries on such streams can fail with preserved `ENOTTY`.
The caller retains the original descriptor. Duplication shares the open-file
description, so do not clear `O_NONBLOCK`, close/reuse descriptors concurrently,
or have another reader consume the same stream.

Reads retry EINTR and retain partial records across `TS_ERR_WOULD_BLOCK`
(`EAGAIN`/`EWOULDBLOCK`). Clean EOF is `TS_ERR_EOF`; EOF within a record is
`TS_ERR_PARSE`, with the partial byte count as error offset. Invalid microseconds
are consumed with `TS_ERR_PARSE`; a complete valid record is retained if result
allocation fails, permitting retry without losing that event. Input timestamps
are the native device/stream's timestamps, not a new wall-clock or monotonic
promise. No clock-selection ioctl is performed. Native integer constants are
Linux UAPI values, not a new portable enumeration. All event types, including
`SYN_DROPPED`, are forwarded. Callers handle lost-event resynchronization (ignore
through the next `SYN_REPORT`, then query state as needed). There is no automatic
hotplug watcher, keyboard layout mapping, exclusive grab, repeat scheduler or
application-example `Device` type. Normal OS errors preserve errno; missing paths
use `TS_ERR_NOTFOUND`. No blocking wait, timeout or cross-thread cancellation is
hidden in the reader. Close is never retried after EINTR.

## `std.uvdev`

The previously empty directory is implemented as an explicit **uinput virtual
keyboard adapter**, using the Linux UAPI version-5 setup interface. It does not
claim to implement libudev device discovery or the richer illustrative applications.

| Operation | Contract |
|---|---|
| `available()` | Linux platform capability; not permission to create devices |
| `create(name,key_codes,path?="/dev/uinput")` | Create one keyboard with an explicit `std.list` of enabled key codes |
| `fd(handle)` | Borrow its nonblocking, close-on-exec descriptor |
| `key(handle,code,value)` | Emit one enabled key: 0 release, 1 press, 2 repeat |
| `sync(handle)` | Emit `EV_SYN/SYN_REPORT`, committing the caller's event frame |
| `close(handle)` | Destroy the virtual device and close the fd, idempotently |

Device names are 1–79 bytes; keys must be 1–`KEY_MAX`, with a bounded nonempty
list; duplicate keys are harmless. Creation uses `BUS_VIRTUAL`, vendor/product/
version 1, and `UI_DEV_SETUP`/`UI_DEV_CREATE`; there is no legacy-setup fallback.
It enables only the declared keys, not absolute/relative axes or force feedback.
The optional path is trusted configuration (and lets tests use an isolated
ioctl fixture); it is opened without following a final symlink. Unsupported
kernels/permissions return real errors. A failed setup closes all acquired
resources. Final destruction also destroys a successfully created device.

Writes retry EINTR; backpressure returns `TS_ERR_WOULD_BLOCK` without pretending
the key was sent. A short native-record write is a system error with `EIO` and
closes the device because blindly retrying can duplicate/corrupt event framing.
`key` does not inject an implicit sync, sleep for device discovery, or wait for
desktop acceptance. The caller supplies framing, readiness, deadlines and any
policy about pressed keys. Device close removes the virtual keyboard; it does
not synthesize release events before destruction. A failed destroy ioctl still
closes the fd, preserving the original error. Close itself is not retried.

**Security:** access to uinput can affect other applications and the desktop.
This is intentional privileged input injection, not a sandbox or authentication
mechanism. Nothing is injected implicitly. Real device creation and desktop
integration are not exercised by the ordinary tests.

On non-Linux platforms both input modules report `available() == false`; all
other operations return `TS_ERR_UNSUPPORTED`, never fake successful resources.

## `std.wayland`

Optional Wayland **client connection and registry discovery**, not a compositor,
window/xdg-shell implementation, renderer, keyboard client or DRM backend.
Builds use platform `wayland-client >= 1.23` when available; configure
`-DTERMSCRIPT_WAYLAND=OFF` to disable it. No dependency is downloaded. The tested
platform library was 1.26.0. Missing support gives `available() == false` and
`TS_ERR_UNSUPPORTED` from other operations.

| Operation | Contract |
|---|---|
| `available()` | Build-time library support, not an available compositor |
| `connect(name?)` | Connect by Wayland display name/path; create owned registry proxy |
| `connect_fd(fd)` | Duplicate a borrowed, connected, already-nonblocking Unix stream |
| `fd(connection)` | Borrow the display fd for poll integration |
| `flush(connection)` | Nonblocking flush; return bytes sent, or would-block |
| `dispatch_pending(connection)` | Dispatch already queued events only; return count |
| `prepare_read(connection)` | Begin a read intention; would-block if pending events need dispatch |
| `read_events(connection)` | Consume a prepared intention and read ready events |
| `cancel_read(connection)` | Cancel a prepared intention, idempotently |
| `globals(connection)` | Owned snapshot list of `{name,interface,version}` registry records |
| `close(connection)` | Cancel preparation, destroy registry, disconnect, invalidate aliases |

`connect` follows `WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR` and named/absolute socket
rules of libwayland; initial connection follows the library's connection behavior
and is not a cancellable nonblocking-connect API. **If `WAYLAND_SOCKET` is set,
`connect` rejects it** rather than silently adopting an embedding process's fd.
Use `connect_fd` to explicitly borrow/duplicate that descriptor instead. The
library adopts our duplicate on success **and failure**; the caller's original
remains owned by the caller. It shares file status flags and the connection, so
do not alter flags or perform competing socket I/O. No raw display/proxy pointer
escapes this adapter.

Correct external-loop sequence:

1. Call `dispatch_pending` and then `prepare_read`. If preparation would block,
   dispatch pending events and retry it.
2. Flush requests. If flushing would block, include writable readiness in your
   wait and retry flushing as needed. A successful preparation remains active.
3. Poll the borrowed fd using your own deadline/cancellation policy.
4. If readable, call `read_events`; otherwise (timeout/cancellation/error), call
   `cancel_read`. Every successful preparation must be balanced.
5. Dispatch pending events and take a fresh registry snapshot.

`read_events` must only be called after readiness; the module does not add an
unbounded dispatch/roundtrip wait. It consumes the read intention even on EINTR:
the caller must prepare again, so the adapter does not incorrectly retry it
internally. Flush retries EINTR. Dispatch/repeated preparation while prepared is
rejected to prevent deadlock. Close balances an outstanding preparation. All
calls on one connection must be externally serialized; no other native client
thread or untracked queue may share the display. EOF from the library's EPIPE
read result is `TS_ERR_EOF`; other errors preserve errno. A failed display is
terminal; close it rather than trying to resurrect it.

Connection buffers are capped at 1 MiB through the verified native API. Registry
snapshots are bounded to 4096 globals and 255 interface-name bytes each. Duplicate/
invalid metadata, capacity exhaustion or callback OOM becomes a sticky failure
reported by subsequent operations; close/cancel remain available for cleanup.
Snapshot records own data and survive removal, later dispatch and connection
closure. Global removal preserves the remaining announcement order. A snapshot
may be partial until the server's initial announcements have been processed;
there is no implicit blocking synchronization or claim that it is complete.

The compositor/socket path is a trust boundary: Unix permissions and the chosen
server control access. This adapter does not authenticate a compositor's identity,
encrypt a transport, bind arbitrary advertised interfaces, or make untrusted
protocol peers safe. libwayland performs wire validation; adapter limits are not
a whole-process denial-of-service sandbox. See the verified
[Wayland client API contracts](https://wayland.freedesktop.org/docs/html/apb.html).

## Test isolation

Input tests use nonblocking pipes containing native event records and a regular
temporary file with mocked uinput ioctls/writes. They exercise every event split,
EOF/truncation, descriptor ownership, setup failure unwinding, backpressure,
EINTR, fatal short writes and double-close; they never open a keyboard or create
a real virtual input device. Wayland tests use a socketpair and an in-process
`wayland-server` registry fixture, not a running desktop compositor. They cover
global add/remove, owned snapshots, prepared-read rules, disconnect, close while
prepared, borrowed fds, environment-fd protection and the compiled unavailable
path. Capability probing is compared across the execution/compilation backends.
Real hardware, hotplug, rendering, desktop injection and non-Linux runtime
behavior remain unverified.
