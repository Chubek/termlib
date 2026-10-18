# Index, multiplexing and framing

These are implemented low-level APIs, not the richer illustrative application
APIs or language-level indexing/interface protocols. No new third-party library
is required: index/framing use C11, and multiplex uses POSIX poll/monotonic clocks.
All handle operations, including close, require external serialization. Distinct
objects can be used independently. Imports acquire no external resources. Native
callback arguments are borrowed; returned strings/lists/records own their copies.
Close is idempotent and invalidates all aliases. Final handle release frees owned
storage. Wrong types, arity, or use after close return `TS_ERR_INVAL`.

## `std.index`

A sorted in-memory mapping from **NUL-free byte strings to int64 values**.
This is not a socket module, database, Unicode collation or full-text engine.
The previously empty `index/socket.*` files were replaced by `index/index.*`.

| Call | Result |
|---|---|
| `new()` | Owned empty index |
| `set(index,key,value)` | True if inserted; false if replaced |
| `get(index,key)` | Value; `TS_ERR_NOTFOUND` if absent |
| `has(index,key)` | Boolean |
| `remove(index,key)` | True if removed |
| `prefix(index,prefix)` | Owned key list in unsigned-byte lexicographic order |
| `len(index)` | Number of keys |
| `clear(index)` | Release entries; keep index reusable |
| `close(index)` | Release storage and invalidate aliases |

Empty keys/prefixes are valid. Keys are copied, values are integers (not retained
handles), so the index cannot create handle-reference cycles. Limits: 65536 keys,
4096 bytes per key, 4 MiB combined key storage including NUL terminators. Storage
limits return `TS_ERR_LIMIT` before mutation. Failed allocation leaves entries
unchanged. Lookup uses binary search, insertion/removal a sorted-vector move.
Prefix snapshots remain valid after index changes/close. There is no persistence,
implicit filesystem access, blocking operation, or cross-thread cancellation.

## `std.multiplex`

A POSIX readiness set with the same event-mask vocabulary as `std.poll`, but
explicit timeout statuses and an optional **borrowed cancellation descriptor**.
It owns no watched descriptors, never consumes bytes and does not change flags.

| Call | Result |
|---|---|
| `new()` | Empty owned set |
| `watch(set,fd,mask)` | Add/update descriptor; true |
| `remove(set,fd)` | Whether an entry existed |
| `len(set)` | Number of watches |
| `wait(set,timeout_ms,cancel_fd?)` | Owned flat `[fd,mask,fd,mask,...]` list |
| `close(set)` | Invalidate set; leave all descriptors open |

At most 1024 watches, one per fd; updates preserve registration order. Requested
masks: read=1, write=2, both=3. Returned masks additionally include hangup=4,
error=8, invalid-fd=16. Readiness is not a guarantee that subsequent I/O cannot
block: use nonblocking descriptors and handle partial I/O/would-block/EOF yourself.
The caller retains every fd and must not close/reuse watched fds concurrently.
Invalid watched fds produce events rather than silent removal.

Timeouts: -1 waits indefinitely; 0 probes; positive values are milliseconds up to
INT_MAX. No ready events gives `TS_ERR_TIMEOUT`, including a zero-timeout probe.
EINTR is retried using a monotonic deadline, not a fresh timeout. Other syscall
failures return `TS_ERR_SYSTEM` with preserved errno. Timing is subject to OS
scheduling and poll resolution. Empty sets can wait for timeout/cancellation.

To cancel, pass the read end of a pipe/socket or another pollable signal fd; a
producer may signal it while the waiting thread exclusively owns the set. Any
cancel-fd readiness/hangup/error yields `TS_ERR_CANCELLED`, taking priority over
ordinary events. Invalid cancel fd gives `TS_ERR_SYSTEM`/EBADF. The module never
drains or closes it: the caller acknowledges/drains it before reusing the wait.
The cancel fd may also be watched, but cancellation takes priority. Mutating or
closing the set concurrently is **not** the cancellation mechanism. There is no
thread, callback loop, descriptor duplication, epoll/kqueue backend, or message
channel multiplexing hidden behind this API.

## `TSC-Protocol.h` and `std.protocols`

The public header defines a binary framing codec linked from
`libtermscript_stdlib`, independent of the VM but using `TS_Status`/`TS_Error`.
It is included by the public umbrella and installed by CMake/Termak. It is **not**
a language interface/plugin protocol or a complete connection/session layer.

Version-1 wire header (16 bytes, integers in big-endian/network byte order):

| Offset | Width | Meaning |
|---|---|---|
| 0 | 4 | ASCII `TSPF` magic |
| 4 | 1 | Wire version, exactly 1 |
| 5 | 1 | Flags, exactly 0 |
| 6 | 2 | Application message type, 1..65535 |
| 8 | 4 | Payload byte length |
| 12 | 4 | CRC-32/ISO-HDLC of header bytes 0..11 followed by payload |
| 16 | length | Arbitrary payload bytes, including NUL |

CRC uses reflected polynomial 0xedb88320, initial 0xffffffff, final xor
0xffffffff. Unknown nonzero message types are preserved; the application must
explicitly handle/reject them. Type 0 is malformed. Unknown versions/flags return
`TS_ERR_UNSUPPORTED`. Bad magic/checksum/truncation returns `TS_ERR_PARSE`; the
error offset is frame-relative (checksum offset 12, length offset 8).

`ts_protocol_options_init` supplies versioned/size-checked options, default
65536-byte payload limit, four reserved zero fields. Limits can be 0..1 MiB.
Encode enforces a hard 1 MiB maximum and copies into a caller-owned buffer; no
heap allocation or descriptor is involved. Insufficient capacity returns LIMIT
and required size without writing. Payload/output overlap is supported. NULL
payload is valid only at length 0. See header for output-pointer requirements.

Create/free own a decoder; free/reset accept NULL. Feed borrows input only for
the call, copies at most one frame and always reports the consumed byte count.
Unused trailing bytes remain the caller's responsibility; feed them again to
decode further frames. `TS_ERR_WOULD_BLOCK` means incomplete input, not a socket
state. `TS_OK` supplies a borrowed frame payload valid until the next feed/reset/
free. Even an empty next feed invalidates the old payload; finish does not.
There is no automatic buffering of an unbounded stream of frames.

Header/payload corruption, unsupported versions/flags and excessive lengths are
sticky failures: reset explicitly before reuse. Oversized lengths are rejected
before payload allocation. Allocation failure during C feed is retryable: retain
the unconsumed input, not the already-consumed header. Finish explicitly marks
input ended: EOF at a frame boundary, PARSE for truncation; feed after EOF returns
EOF until reset. A failed decoder does not attempt unsafe wire resynchronization.
There is no blocking, background work or cross-thread cancellation; feed work is
bounded by one capped frame.

Termscript strings are NUL-terminated, so the module uses **explicit hexadecimal
strings**, not silently truncated binary strings:

| Call | Result |
|---|---|
| `new(max_payload?=65536)` | Owned incremental decoder |
| `encode_hex(type,payload_hex)` | Lowercase hex wire frame |
| `feed_hex(decoder,chunk_hex)` | Owned `{consumed,ready,type,payload_hex}` record |
| `reset(decoder)` | Drop partial frame/failure/EOF and reuse |
| `finish(decoder)` | EOF at boundary; PARSE for truncation |
| `close(decoder)` | Release buffers and invalidate aliases |

Input accepts upper/lower hex digits, no whitespace, whole byte pairs only; each
chunk is capped at 1 MiB + 16 bytes. `consumed` counts **decoded bytes**, not hex
characters; replay only the unused suffix. Incomplete input returns a successful
record with `ready=false,type=0,payload_hex=""`. Malformed hex is rejected before
touching decoder state. Allocation failure inside feed or while constructing a
result **after consumption** poisons the decoder with NOMEM; reset/restart at a known frame boundary instead
of guessing how much input was consumed. Native C callers can use the precise
consumed count for retryable allocation failures. Returned records survive reuse
and close. The codec does not perform sends/receives, timeouts, backpressure or
keepalives; applications compose it with their own transport and readiness loop.

**Security:** CRC detects accidental corruption, not malicious modification.
There is no authentication, encryption, peer verification, negotiation, replay
protection or secure credential storage. Use an authenticated encrypted transport
for untrusted networks. Do not present raw framed TCP as secure. Applications
must validate payloads, authorize operations, enforce rate/session budgets and
implement version negotiation separately.

## Runnable example and tests

`tests/termscript_infrastructure.tsc` is verified across configured VM/C/QBE/
Jiterati backends. The native fixture covers ordered/replaced/prefix index keys,
limits and snapshot ownership; pipe readiness/hangup/invalid-fd, EINTR and
cancellation; every frame split/truncation, header/payload corruption, unknown
types, options/size validation, zero-length/binary payloads, consecutive frames,
hex errors, shared-handle close and reset. No external service or real network
is used. Non-POSIX multiplex operation is not implemented; non-Linux POSIX
runtime behavior remains unverified.
