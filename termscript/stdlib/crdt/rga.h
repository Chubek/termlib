/* Standalone C11 replicated growable Unicode sequence; no Termscript runtime
 * dependency. See README.md for ordering, snapshot and security contracts. */
#ifndef TS_CRDT_RGA_H
#define TS_CRDT_RGA_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif

#define TS_CRDT_MAX_NODES 65536u
#define TS_CRDT_VERSION_MAJOR 1
#define TS_CRDT_VERSION_MINOR 0
#define TS_CRDT_VERSION_PATCH 0
#define TS_CRDT_DEFAULT_LIMIT 8192u
#define TS_CRDT_SNAPSHOT_HEADER 24u
#define TS_CRDT_SNAPSHOT_RECORD 40u

  typedef enum
  {
    TS_CRDT_OK = 0,
    TS_CRDT_INVALID,
    TS_CRDT_NOMEM,
    TS_CRDT_LIMIT,
    TS_CRDT_CONFLICT,
    TS_CRDT_FORMAT
  } TS_CRDTStatus;
  typedef struct
  {
    uint64_t site, clock;
  } TS_CRDTId;
  enum
  {
    TS_CRDT_INSERT = 1,
    TS_CRDT_DELETE = 2
  };
  /* INSERT defines an immutable character and its predecessor. DELETE alone
   * carries only id (parent/scalar must be zero). INSERT|DELETE is a
   * tombstoned definition. The all-zero id is the root, never an operation
   * target. */
  typedef struct
  {
    TS_CRDTId id, parent;
    uint32_t scalar, flags;
  } TS_CRDTOp;
  typedef struct
  {
    TS_CRDTOp *ops;
    size_t count;
  } TS_CRDTBatch;
  typedef struct TS_CRDT TS_CRDT;

  enum
  {
    TS_CRDT_FEATURE_UNICODE = 1u,
    TS_CRDT_FEATURE_DELTAS = 2u,
    TS_CRDT_FEATURE_SNAPSHOTS = 4u,
    TS_CRDT_FEATURE_OUT_OF_ORDER = 8u
  };
  const char *
  ts_crdt_version (void); /* Borrowed immutable string, never NULL. */
  uint32_t ts_crdt_features (void);

  /* All mutations are failure-atomic. Inputs are borrowed, outputs are owned.
   * Free documents/batches with their matching NULL-safe functions, byte/text
   * outputs with free(). Output pointers must not own a previous allocation.
   * Operations require externally serialized access, including reads/close.
   * No background work, blocking I/O, hidden global state or callbacks. */
  TS_CRDTStatus ts_crdt_create (uint64_t site, size_t limit, TS_CRDT **out);
  void ts_crdt_free (TS_CRDT *doc);
  void ts_crdt_batch_free (TS_CRDTBatch *batch);
  /* A transactional copy may reuse site; independently writing replicas MUST
   * use different nonzero sites, including after restoring a backup. */
  TS_CRDTStatus ts_crdt_clone (const TS_CRDT *doc, uint64_t site,
                               TS_CRDT **out);
  uint64_t ts_crdt_site (const TS_CRDT *doc);
  size_t
  ts_crdt_size (const TS_CRDT *doc); /* Definitions + pending tombstones. */
  size_t ts_crdt_limit (const TS_CRDT *doc);
  TS_CRDTStatus ts_crdt_apply (TS_CRDT *doc, const TS_CRDTOp *ops,
                               size_t count);
  TS_CRDTStatus ts_crdt_merge (TS_CRDT *doc, const TS_CRDT *other);
  /* Index/count units are Unicode scalars, not bytes or grapheme clusters.
   * delta may be NULL; otherwise caller owns it after success. Empty edits are
   * valid. UTF-8 is strict, length-delimited, and may contain U+0000 in the C
   * API. */
  TS_CRDTStatus ts_crdt_insert (TS_CRDT *, size_t index, const char *utf8,
                                size_t bytes, TS_CRDTBatch *delta);
  TS_CRDTStatus ts_crdt_delete (TS_CRDT *, size_t index, size_t count,
                                TS_CRDTBatch *delta);
  TS_CRDTStatus ts_crdt_text (const TS_CRDT *, char **out, size_t *bytes);
  TS_CRDTStatus ts_crdt_length (const TS_CRDT *, size_t *length);
  TS_CRDTStatus ts_crdt_id_at (const TS_CRDT *, size_t index, TS_CRDTId *out);
  /* found=0 for unknown, deleted or unresolved IDs. */
  TS_CRDTStatus ts_crdt_index_of (const TS_CRDT *, TS_CRDTId, size_t *index,
                                  int *found);
  /* Canonical little-endian version-1 format with payload CRC32. Decoding
   * validates lengths, counts, flags, scalar values, IDs and causal ordering.
   * Unknown versions/corruption/truncation are FORMAT; excess sizes are LIMIT.
   * offset is optional, is a byte offset, and is zero on success. */
  TS_CRDTStatus ts_crdt_encode (const TS_CRDT *, unsigned char **out,
                                size_t *bytes);
  TS_CRDTStatus ts_crdt_encode_ops (const TS_CRDTOp *, size_t count,
                                    unsigned char **out, size_t *bytes);
  TS_CRDTStatus ts_crdt_decode (uint64_t site, size_t limit, const void *data,
                                size_t bytes, TS_CRDT **out, size_t *offset);
  const char *ts_crdt_status_string (TS_CRDTStatus);
#ifdef __cplusplus
}
#endif
#endif
