# Additional native modules

All arguments borrow values. Returned strings/lists are owned values; returned
resource handles own their allocations/descriptors, shared by aliases. Modules
use VM serialization (no concurrent mutation of an object). No callbacks or
background threads are started.

| Module | Operations |
|---|---|
| `std.gapbuffer` | `new(text?)`, `empty()`, `from(text)`, `insert`, `delete`, `replace`, `slice`, `line`, `len`, `contents`, `to_string` |
| `std.piecetable` | Same surface, backed by the existing append-only piece-table storage |
| `std.memory` | `alloc(size)`/`new(size)`, `len`, `get(handle,index)`, `set(handle,index,byte)`, `fill(handle,byte)`, `close` |
| `std.persian_script` | `normalize(text)`, `search_fold(text)` |
| `std.set` | `new`, `add`, `has`, `remove`, `len`, `clear`, `values` |
| `std.stdio` | `write(text)`, `eprint(text)`, `flush()`, `flush_error()`, `read_line()` |
| `std.datagram` | `bind(port,address?)`, `fd`, `port`, `send_to(handle,address,port,text)`, `recv(handle,limit?)`, `close` |
| `std.procfs` | `open(root?="/proc")`, `read(view,path,byte_limit?)`, `list(view,path?,entry_limit?)`, `close(view)` |
| `std.sysfs` | Same read-only view API, default root `/sys` |
| `std.crdt` | `new`, `fork`, `insert`, `delete`, `apply`, `merge`, `text`, `len`, `size`, `id_at`, `index_of`, `snapshot`, `restore`, `close` ([contracts](crdt/README.md)) |

## Contracts and limits

Gap/piece offsets and lengths count bytes, not graphemes. Editing operations use
the existing `std.buffer` contracts; text is NUL-terminated. These adapters share
its handle type and may be used through its API. `empty`/`new`/`from` are aliases
accepting zero or one text argument. `line` returns nil for a missing line.

Memory allocations are zero-filled, limited to 16 MiB each. Byte values must be
0–255 and indices must be in range. Closing frees the allocation immediately;
repeated close succeeds, but later reads/writes/length queries fail. Destruction
also closes it. Raw pointers and native memory addresses are not exposed.

Persian normalization validates UTF-8, maps Arabic yeh/alef maksura to Persian
yeh, and Arabic kaf to Persian keheh. Search folding additionally drops ZWNJ,
kashida and U+064B–U+065F marks and folds ASCII capitals. This is a specified
small transformation, not full Unicode normalization or case folding.

Sets contain strings only, preserve insertion order, use O(n) lookup, and cap
at 65536 members. `add` returns whether a new member was added; `remove` returns
whether one existed. `values` returns a new shared-list value.

Stdio borrows process stdin/stdout/stderr and never closes them. Writes are
immediate native stream I/O, separate from captured `print`/`G:puts` output.
`read_line` strips newline, returns nil on clean EOF with no data, retries EINTR,
and caps a line at 1 MiB. NUL input is rejected. Errors consume bytes already
read; nonblocking stdin is not a resumable line-reader API. Use the existing
readiness/descriptor APIs for incremental nonblocking streams.

UDP binds numeric IPv4 loopback by default; binding other addresses must be
explicit. Sockets are owned, close-on-exec and nonblocking; `fd` returns a
borrowed descriptor for poll integration (do not close it separately). EINTR is
retried, EAGAIN/EWOULDBLOCK returns `TS_ERR_WOULD_BLOCK`. There are no blocking
waits or read timeouts. Callers implement deadlines/cancellation around poll.
Payloads are text, limited to 65507 bytes. Receive returns a fresh list
`[text, peer_address, peer_port]`; empty UDP packets are data, not EOF. A packet
larger than the receive bound is consumed with `TS_ERR_LIMIT`; a packet containing
NUL is consumed with `TS_ERR_UNSUPPORTED`. Close is idempotent. There is no DNS,
multicast helper, reliability, authentication, encryption or replay protection.
Never use this transport alone for trusted collaborative editing.

## Read-only procfs/sysfs views

These are actual descriptor-based filesystem adapters, not simulated process or
device records. They can also open a fixture directory; ordinary tests do not
depend on a host procfs/sysfs installation. They do **not** yet provide the
application examples' typed process enumeration or device-management APIs.

`open` owns a close-on-exec directory descriptor. Every `read`/`list` opens a
fresh descriptor relative to that root. Reads return owned text; listings return
owned lists of copied names sorted in byte order. Missing paths return
`TS_ERR_NOTFOUND` with errno; permission/OS errors preserve errno. EOF produces
the text accumulated so far, including an empty string for an empty file.

Paths must be relative, at most 4096 bytes, with no empty, `.` or `..`
components. Symlinks are rejected at **every** path component; directory entries
that happen to be symlinks may appear in listings but cannot be followed.
Consequently procfs magic links (`self`, `fd/*`) and sysfs class/device links
are intentionally not dereferenced. Use real numeric PID/physical device paths
where appropriate. The explicitly chosen root path is trusted configuration;
this adapter is not a sandbox against hostile mounts or privileged filesystem
changes, and intermediate components of the supplied root path are resolved by
the OS. The final root component itself may not be a symlink.

Text reads accept only regular/regular-like virtual files. FIFOs/devices are
rejected without blocking on their contents. Default read bound is 65536 bytes,
maximum 1 MiB. Exceeding it returns `TS_ERR_LIMIT`, never silent truncation;
embedded NUL returns `TS_ERR_UNSUPPORTED`. Directory bounds default to 4096
entries, maximum 65536 entries and 1 MiB of names; excess returns no partial
listing. Open/read interruption is retried, and would-block has its own status.
Kernel virtual-file reads may still block despite O_NONBLOCK: there is no
deadline or cross-thread cancellation guarantee.

Close is idempotent and affects aliases; destruction closes a remaining root.
Views use a common handle type, so either module can operate on an explicitly
opened view. Serialize operations and closing on a shared view. No files are
written and no background threads or subprocesses are started.
