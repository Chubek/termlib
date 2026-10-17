# Native Unicode text CRDT

This is an original C11 replicated growable array (RGA) implementation, not a
binding to the vendored Kotlin `c-crdtlib`. `rga.c`/`rga.h` have no VM, platform,
network, Unicode-library or third-party dependencies. `crdt.c` exposes the same
engine as the registered `std.crdt` Termscript module.

## Build

The normal repository build includes `libtermscript_crdt.a`, its standalone
tests, and the native module. The core can also be built without Termlib,
Termscript, C++, NovoParse or the standard-library dependencies:

```sh
cmake -S termscript/stdlib/crdt -B build-crdt
cmake --build build-crdt
ctest --test-dir build-crdt --output-on-failure
```

CMake consumers in the repository can link `termscript::crdt`. Installed C
consumers include `<termscript/stdlib/crdt/rga.h>` and link
`-ltermscript_crdt`. The full stdlib archive embeds the same core objects, so
generated Termscript programs do not require an extra link flag. Use the core
archive alone for a C-only embedding; it does not require the stdlib archive.
The module's `Termakfile` also builds just this independent core archive.

## Model and merge rules

* A character is a Unicode scalar, not a byte or an extended grapheme cluster.
  UTF-8 is decoded strictly; invalid sequences, surrogates and out-of-range
  scalars are rejected. Combining marks and ZWNJ are individual elements.
* Each inserted character has an immutable `(site, clock)` ID, a predecessor ID
  and a scalar value. The all-zero predecessor is the root. Both parts of a
  non-root ID must be nonzero unsigned 64-bit integers.
* Each local insertion uses a Lamport clock strictly above every observed ID's
  clock. Characters in a multi-character insertion form a predecessor chain.
  Parent clocks must be strictly smaller than child clocks, excluding cycles
  even in disconnected input. Clock overflow is an explicit limit failure.
* Materialization is a depth-first traversal. Sibling chains are ordered by
  **descending `(clock, site)`**, preserving local insertion at an index and
  giving concurrent insertions a deterministic tie break. This specifies one
  RGA variant, not compatibility with every other library named RGA.
* Inserts form a grow-only set of immutable definitions; deletions form a
  grow-only set of tombstones. Merge is their set union. Valid histories with
  unique writer identities merge commutatively, associatively and idempotently.
  If an ID is redefined with a different scalar/predecessor, merge returns a
  conflict instead of arbitrarily choosing one definition.
* Children received before parents remain stored but invisible. Unknown-ID
  deletions are retained, so delete-before-insert cannot resurrect text. Deleted
  characters remain as anchors, and their live descendants remain visible.
* No tombstones are garbage-collected and no deleted ID is revived. These would
  require a separate causal-stability/membership or undo design. Reinserting
  text creates new IDs. Local edits, apply and merge are failure-atomic.

The engine uses sorted arrays, binary lookup and iterative traversal, without
recursive descent through the document. A batch apply/materialization is
`O(n log n)` with `O(n)` scratch space. Individual edits do not have rope/index
performance; this is a bounded correctness-oriented engine, not a claim that a
large editor workload is optimized. Limits include tombstones and unresolved
definitions: default 8192 records, configurable from 1 to 65536; batches are at
most 65536 records. Snapshot parsing is also bounded. Exhausting the configured
limit fails without eviction, truncation or partial mutation.

## C API and ownership

`ts_crdt_create` owns a new opaque document, freed by NULL-safe `ts_crdt_free`.
`ts_crdt_version` returns the immutable library version string and
`ts_crdt_features` reports the implemented Unicode/delta/snapshot/out-of-order
capabilities without creating a document.
Input buffers, operation arrays and source documents are borrowed only for the
call. `ts_crdt_insert`/`ts_crdt_delete` optionally return owned operation batches;
release them with NULL-safe, repeatable `ts_crdt_batch_free`. Callers must supply
empty output slots, not allocations they still own. All text/snapshot buffers
are independent owned allocations freed with `free`. Text has an explicit byte
length plus a trailing NUL, and the C API supports embedded U+0000.

`ts_crdt_clone` makes an independent copy. The C API permits a same-site copy
for staging transactions, **not for independent writers**. New writers need
unique site IDs. There is no implicit randomness or identity service. The C
API permits the entire nonzero `uint64_t` site space; Termscript's `new`, `fork`
and `restore` accept sites 1 through `INT64_MAX`.

IDs returned by `ts_crdt_id_at` are copied values. `ts_crdt_index_of` reports
whether an ID is currently visible; unknown/deleted/unresolved IDs are not
silently converted into insertion positions. All indexes/counts are scalar
positions. None of the APIs transfers descriptors or starts background work.

Documents are not concurrently accessible, even for reads: externally serialize
all access and destruction. Different documents can be used by different
threads; there is no shared mutable global state. Operations perform no blocking
I/O and have no asynchronous cancellation API. Statuses distinguish invalid
arguments/UTF-8, allocation failure, limits, identity conflicts and malformed
snapshots. No API stores pointers to caller error or input buffers.

## Snapshot and delta format, version 1

Snapshots include **all** records, including deleted and pending records, but
not the receiving writer's site or node-limit configuration. Restoring computes
the next Lamport clock from the stored IDs. Receiving-site identity is an
explicit argument. Deltas are encoded in exactly the same format; a delta may
contain a disconnected fragment or deletion-only records.

All integers are unsigned fixed-width **little-endian**, with no native struct
padding. No host-endian data or native pointers are serialized.

| Header offset | Width | Value |
|---|---|---|
| 0 | 8 | bytes `54 53 52 47 41 00 01 00` (`TSRGA`, NUL, version 1, NUL) |
| 8 | 4 | record count |
| 12 | 4 | record width, exactly 40 |
| 16 | 4 | IEEE CRC32 of payload (polynomial `0xedb88320`, initial/final XOR `0xffffffff`) |
| 20 | 4 | reserved, must be zero |

Each record contains `site:u64`, `clock:u64`, `parent_site:u64`,
`parent_clock:u64`, `scalar:u32`, `flags:u32`. Records are strictly sorted by
ascending `(site, clock)`, with no duplicate IDs. Flag 1 is INSERT, flag 2 is
DELETE, flag 3 is a deleted insertion. Other flags are rejected. A deletion-only
record must have zero parent and scalar fields. Scalar and parent-clock rules
are validated for every insertion. Unknown format versions, trailing bytes,
truncation, malformed IDs, duplicates, invalid fields and CRC mismatch are
rejected with a byte offset; oversized inputs return a limit status. Canonical
encoding of converged replicas is byte-identical, independent of local site.

Callers handle durable storage: write snapshots to a temporary file, flush as
needed, then atomically rename. The library performs no filesystem operations.
Recovery from a corrupt snapshot is an error, not an empty-document fallback.

## Termscript API

| Function | Result |
|---|---|
| `new(site, limit?)` | owned empty document, default limit 8192 |
| `fork(doc, new_site)` | independent replica, rejects the source site |
| `insert(doc, index, text)` | applies local edit and returns an owned delta string |
| `delete(doc, index, count)` | applies deletion and returns an owned delta string |
| `apply(doc, delta_or_snapshot)` | integrates a serialized state fragment, returns nil |
| `merge(doc, other_doc)` | integrates the other document, returns nil |
| `text(doc)` / `len(doc)` | owned UTF-8 text / visible scalar count |
| `size(doc)` | retained record count, including deleted and pending records |
| `id_at(doc, index)` | owned `16hex:16hex` stable character ID |
| `index_of(doc, id)` | visible index or nil |
| `snapshot(doc)` | owned canonical snapshot string |
| `restore(site, snapshot, limit?)` | owned restored document |
| `close(doc)` | idempotent, affects all aliases; subsequent operations fail |

Serialized strings are `rga1:` plus hexadecimal pairs for the binary format.
The maximum encoding is 5,242,933 text bytes at 65536 records; it is **not** a
single-UDP-packet protocol. Snapshot parsing returns `TS_ERR_PARSE` and an
offset into this string; limits use `TS_ERR_LIMIT`, conflicts/invalid arguments
use `TS_ERR_INVAL`, and allocation failures use `TS_ERR_NOMEM`. A C snapshot
containing U+0000 can be restored, but Termscript `text` returns
`TS_ERR_UNSUPPORTED` rather than silently truncating the current string ABI.
Module edits stage both the state change and returned delta before committing,
so a failed return allocation cannot leave an unreported edit.

**Runnable:** `termscript/examples/crdt.tsc` demonstrates concurrent editing,
out-of-order delivery and recovery. `tests/termscript_crdt.tsc` exercises this
API on VM, C, QBE and Jiterati backends.

The independent core tests cover all 720 permutations of a mixed history,
randomized three-replica convergence and shuffled replay, strict UTF-8,
pending/deleted anchors, a 65536-element chain, golden-format bytes, corruption,
truncation, identity conflicts and clock overflow. Allocation fault injection
checks every allocation point along edit/merge/materialization/codec paths for
unchanged state and leak-free unwinding. On pthread platforms a separate test
uses independent documents concurrently and a shared document under an external
mutex. These tests also run under AddressSanitizer/UndefinedBehaviorSanitizer.

## Identity, transport and security

Assign a unique site to every independently writing replica and persist that
assignment. Do not restore the same site in two concurrent writers, or restore
an old backup as an independent writer under its old site: use a new site or
recover the complete state while ensuring the old writer cannot continue.
The engine detects conflicting definitions, not all possible identity misuse.

This library is **not an authenticated collaboration protocol**. It does not
provide encryption, signatures, authorization, peer discovery, delivery,
rate-limiting or anti-entropy scheduling. CRC32 detects accidental corruption,
not adversarial tampering. A malicious peer can delete known or future IDs,
consume pending-record capacity or inject a maximal clock to prevent further
local insertion. Authenticate and authorize messages **before** applying them,
and enforce document identity, size/rate limits and session membership outside
the engine. State from different documents must never be merged accidentally.

Delta exchange alone cannot repair lost messages. Exchange complete snapshots
for anti-entropy/reconnection, or build a separate acknowledged protocol. A
maximum clock per site is **not** a sufficient version vector when gaps are
permitted; this API does not pretend otherwise.

Not implemented: generic-value CRDTs, RGA revive/invert undo, automatic cursor
affinity, effect streams, vector-clock/delta-ack protocols, tombstone GC or the
full illustrative `coedit.tsc` object/method API. Its application still requires
unimplemented language/UI/network work; this module does not make it runnable.
