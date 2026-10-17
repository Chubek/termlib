#include "rga.h"
#include <stdlib.h>
#include <string.h>

struct TS_CRDT
{
  uint64_t site, clock;
  size_t count, limit;
  TS_CRDTOp *nodes; /* Sorted by (site, clock). */
};
const char *
ts_crdt_version (void)
{
  return "1.0.0";
}
uint32_t
ts_crdt_features (void)
{
  return TS_CRDT_FEATURE_UNICODE | TS_CRDT_FEATURE_DELTAS
         | TS_CRDT_FEATURE_SNAPSHOTS | TS_CRDT_FEATURE_OUT_OF_ORDER;
}
static int
id_compare (TS_CRDTId a, TS_CRDTId b)
{
  if (a.site != b.site)
    return a.site < b.site ? -1 : 1;
  return a.clock < b.clock ? -1 : a.clock != b.clock;
}
static int
op_compare (const void *a, const void *b)
{
  return id_compare (((const TS_CRDTOp *)a)->id, ((const TS_CRDTOp *)b)->id);
}
static int
root (TS_CRDTId id)
{
  return !id.site && !id.clock;
}
static int
valid_id (TS_CRDTId id)
{
  return id.site && id.clock;
}
static int
scalar_valid (uint32_t c)
{
  return c <= 0x10ffff && (c < 0xd800 || c > 0xdfff);
}
static int
op_valid (const TS_CRDTOp *op)
{
  if (!valid_id (op->id) || !op->flags || (op->flags & ~3u))
    return 0;
  if (!(op->flags & TS_CRDT_INSERT))
    return root (op->parent) && !op->scalar;
  return scalar_valid (op->scalar)
         && (root (op->parent)
             || (valid_id (op->parent) && op->parent.clock < op->id.clock));
}
const char *
ts_crdt_status_string (TS_CRDTStatus status)
{
  switch (status)
    {
    case TS_CRDT_OK:
      return "success";
    case TS_CRDT_INVALID:
      return "invalid CRDT argument, operation or UTF-8";
    case TS_CRDT_NOMEM:
      return "CRDT allocation failed";
    case TS_CRDT_LIMIT:
      return "CRDT node, operation or logical-clock limit exceeded";
    case TS_CRDT_CONFLICT:
      return "conflicting CRDT character ID (replica identity reused)";
    case TS_CRDT_FORMAT:
      return "invalid, corrupt or unsupported CRDT snapshot";
    default:
      return "unknown CRDT status";
    }
}
TS_CRDTStatus
ts_crdt_create (uint64_t site, size_t limit, TS_CRDT **out)
{
  if (!out)
    return TS_CRDT_INVALID;
  *out = NULL;
  if (!site || !limit || limit > TS_CRDT_MAX_NODES)
    return TS_CRDT_INVALID;
  TS_CRDT *doc = calloc (1, sizeof *doc);
  if (!doc)
    return TS_CRDT_NOMEM;
  doc->site = site;
  doc->limit = limit;
  *out = doc;
  return TS_CRDT_OK;
}
void
ts_crdt_free (TS_CRDT *doc)
{
  if (doc)
    {
      free (doc->nodes);
      free (doc);
    }
}
void
ts_crdt_batch_free (TS_CRDTBatch *batch)
{
  if (batch)
    {
      free (batch->ops);
      batch->ops = NULL;
      batch->count = 0;
    }
}
uint64_t
ts_crdt_site (const TS_CRDT *doc)
{
  return doc ? doc->site : 0;
}
size_t
ts_crdt_size (const TS_CRDT *doc)
{
  return doc ? doc->count : 0;
}
size_t
ts_crdt_limit (const TS_CRDT *doc)
{
  return doc ? doc->limit : 0;
}
TS_CRDTStatus
ts_crdt_clone (const TS_CRDT *doc, uint64_t site, TS_CRDT **out)
{
  if (!doc)
    {
      if (out)
        *out = NULL;
      return TS_CRDT_INVALID;
    }
  TS_CRDTStatus status = ts_crdt_create (site, doc->limit, out);
  if (status)
    return status;
  TS_CRDT *copy = *out;
  if (doc->count)
    {
      copy->nodes = malloc (doc->count * sizeof *copy->nodes);
      if (!copy->nodes)
        {
          ts_crdt_free (copy);
          *out = NULL;
          return TS_CRDT_NOMEM;
        }
      memcpy (copy->nodes, doc->nodes, doc->count * sizeof *copy->nodes);
    }
  copy->count = doc->count;
  copy->clock = doc->clock;
  return TS_CRDT_OK;
}

/* Set union of immutable insert definitions and grow-only tombstones. Build
 * the entire candidate before publishing; duplicate messages do not consume
 * additional node capacity. Strictly increasing parent clocks exclude cycles
 * even when ancestors have not arrived yet. */
TS_CRDTStatus
ts_crdt_apply (TS_CRDT *doc, const TS_CRDTOp *ops, size_t count)
{
  if (!doc || (count && !ops))
    return TS_CRDT_INVALID;
  if (count > TS_CRDT_MAX_NODES)
    return TS_CRDT_LIMIT;
  if (!count)
    return TS_CRDT_OK;
  for (size_t i = 0; i < count; i++)
    if (!op_valid (ops + i))
      return TS_CRDT_INVALID;
  size_t total = doc->count + count;
  TS_CRDTOp *nodes = malloc (total * sizeof *nodes);
  if (!nodes)
    return TS_CRDT_NOMEM;
  if (doc->count)
    memcpy (nodes, doc->nodes, doc->count * sizeof *nodes);
  memcpy (nodes + doc->count, ops, count * sizeof *nodes);
  qsort (nodes, total, sizeof *nodes, op_compare);
  size_t used = 0;
  uint64_t clock = doc->clock;
  for (size_t i = 0; i < total; i++)
    {
      TS_CRDTOp op = nodes[i];
      if (op.id.clock > clock)
        clock = op.id.clock;
      if (used && !id_compare (nodes[used - 1].id, op.id))
        {
          TS_CRDTOp *old = nodes + used - 1;
          if ((old->flags & op.flags & TS_CRDT_INSERT)
              && (id_compare (old->parent, op.parent)
                  || old->scalar != op.scalar))
            {
              free (nodes);
              return TS_CRDT_CONFLICT;
            }
          uint32_t flags = old->flags | op.flags;
          if (op.flags & TS_CRDT_INSERT)
            *old = op;
          old->flags = flags;
        }
      else
        nodes[used++] = op;
    }
  if (used > doc->limit)
    {
      free (nodes);
      return TS_CRDT_LIMIT;
    }
  free (doc->nodes);
  doc->nodes = nodes;
  doc->count = used;
  doc->clock = clock;
  return TS_CRDT_OK;
}
TS_CRDTStatus
ts_crdt_merge (TS_CRDT *doc, const TS_CRDT *other)
{
  if (!doc || !other)
    return TS_CRDT_INVALID;
  if (doc == other)
    return TS_CRDT_OK;
  return ts_crdt_apply (doc, other->nodes, other->count);
}
static size_t
find_id (const TS_CRDT *doc, TS_CRDTId id)
{
  size_t lo = 0, hi = doc->count;
  while (lo < hi)
    {
      size_t mid = lo + (hi - lo) / 2;
      int cmp = id_compare (doc->nodes[mid].id, id);
      if (cmp < 0)
        lo = mid + 1;
      else
        hi = mid;
    }
  return lo < doc->count && !id_compare (doc->nodes[lo].id, id) ? lo
                                                                : SIZE_MAX;
}
typedef struct
{
  size_t parent, node;
  TS_CRDTId id;
} edge;
static int
edge_compare (const void *a, const void *b)
{
  const edge *x = a, *y = b;
  if (x->parent != y->parent)
    return x->parent < y->parent ? -1 : 1;
  /* Newer children first; site breaks ties deterministically. */
  if (x->id.clock != y->id.clock)
    return x->id.clock > y->id.clock ? -1 : 1;
  return x->id.site > y->id.site ? -1 : x->id.site != y->id.site;
}
/* Materialization is O(n log n), bounded and iterative: no recursion, even
 * for 65536-deep runs. Unresolved subtrees remain invisible until their
 * ancestors arrive. Tombstoned definitions still anchor their descendants. */
static TS_CRDTStatus
visible (const TS_CRDT *doc, size_t **out, size_t *length)
{
  if (!doc || !out || !length)
    return TS_CRDT_INVALID;
  *out = NULL;
  *length = 0;
  size_t n = doc->count;
  if (!n)
    return TS_CRDT_OK;
  edge *edges = malloc (n * sizeof *edges);
  size_t *first = malloc ((n + 1) * sizeof *first);
  size_t *next = malloc (n * sizeof *next);
  size_t *stack = malloc ((n + 1) * sizeof *stack);
  size_t *order = malloc (n * sizeof *order);
  if (!edges || !first || !next || !stack || !order)
    {
      free (edges);
      free (first);
      free (next);
      free (stack);
      free (order);
      return TS_CRDT_NOMEM;
    }
  for (size_t i = 0; i <= n; i++)
    first[i] = SIZE_MAX;
  size_t count = 0;
  for (size_t i = 0; i < n; i++)
    {
      if (!(doc->nodes[i].flags & TS_CRDT_INSERT))
        continue;
      size_t p = root (doc->nodes[i].parent)
                     ? n
                     : find_id (doc, doc->nodes[i].parent);
      if (p == SIZE_MAX || (p < n && !(doc->nodes[p].flags & TS_CRDT_INSERT)))
        continue;
      edges[count++] = (edge){ p, i, doc->nodes[i].id };
    }
  qsort (edges, count, sizeof *edges, edge_compare);
  for (size_t i = count; i > 0; i--)
    {
      edge e = edges[i - 1];
      next[e.node] = first[e.parent];
      first[e.parent] = e.node;
    }
  size_t top = 1, used = 0;
  stack[0] = first[n];
  while (top)
    {
      size_t i = stack[top - 1];
      if (i == SIZE_MAX)
        {
          top--;
          continue;
        }
      stack[top - 1] = next[i];
      if (!(doc->nodes[i].flags & TS_CRDT_DELETE))
        order[used++] = i;
      if (first[i] != SIZE_MAX)
        stack[top++] = first[i];
    }
  free (edges);
  free (first);
  free (next);
  free (stack);
  *out = order;
  *length = used;
  return TS_CRDT_OK;
}
/* Decode a single Unicode scalar, rejecting overlongs and surrogate values. */
static size_t
decode_utf8 (const unsigned char *s, size_t n, uint32_t *scalar)
{
  if (!n)
    return 0;
  uint32_t c = s[0];
  size_t size;
  if (c < 0x80)
    size = 1;
  else if (c >= 0xc2 && c <= 0xdf)
    {
      c &= 31;
      size = 2;
    }
  else if (c >= 0xe0 && c <= 0xef)
    {
      c &= 15;
      size = 3;
    }
  else if (c >= 0xf0 && c <= 0xf4)
    {
      c &= 7;
      size = 4;
    }
  else
    return 0;
  if (size > n)
    return 0;
  for (size_t i = 1; i < size; i++)
    {
      if ((s[i] & 0xc0) != 0x80)
        return 0;
      c = (c << 6) | (s[i] & 63);
    }
  if ((size == 2 && c < 0x80) || (size == 3 && c < 0x800)
      || (size == 4 && c < 0x10000) || !scalar_valid (c))
    return 0;
  *scalar = c;
  return size;
}
static size_t
encode_utf8 (uint32_t c, char *s)
{
  if (c < 0x80)
    {
      s[0] = (char)c;
      return 1;
    }
  if (c < 0x800)
    {
      s[0] = (char)(0xc0 | (c >> 6));
      s[1] = (char)(0x80 | (c & 63));
      return 2;
    }
  if (c < 0x10000)
    {
      s[0] = (char)(0xe0 | (c >> 12));
      s[1] = (char)(0x80 | ((c >> 6) & 63));
      s[2] = (char)(0x80 | (c & 63));
      return 3;
    }
  s[0] = (char)(0xf0 | (c >> 18));
  s[1] = (char)(0x80 | ((c >> 12) & 63));
  s[2] = (char)(0x80 | ((c >> 6) & 63));
  s[3] = (char)(0x80 | (c & 63));
  return 4;
}
TS_CRDTStatus
ts_crdt_insert (TS_CRDT *doc, size_t index, const char *text, size_t bytes,
                TS_CRDTBatch *delta)
{
  if (delta)
    *delta = (TS_CRDTBatch){ 0 };
  if (!doc || (bytes && !text))
    return TS_CRDT_INVALID;
  if (bytes > 4u * TS_CRDT_MAX_NODES)
    return TS_CRDT_LIMIT;
  size_t chars = 0, at = 0;
  uint32_t c;
  while (at < bytes)
    {
      size_t step
          = decode_utf8 ((const unsigned char *)text + at, bytes - at, &c);
      if (!step)
        return TS_CRDT_INVALID;
      at += step;
      chars++;
    }
  if (chars > doc->limit - doc->count || chars > UINT64_MAX - doc->clock)
    return TS_CRDT_LIMIT;
  size_t *order = NULL, length;
  TS_CRDTStatus status = visible (doc, &order, &length);
  if (status)
    return status;
  if (index > length)
    {
      free (order);
      return TS_CRDT_INVALID;
    }
  TS_CRDTId parent
      = index ? doc->nodes[order[index - 1]].id : (TS_CRDTId){ 0 };
  free (order);
  if (!chars)
    return TS_CRDT_OK;
  TS_CRDTOp *ops = calloc (chars, sizeof *ops);
  if (!ops)
    return TS_CRDT_NOMEM;
  at = 0;
  for (size_t i = 0; i < chars; i++)
    {
      at += decode_utf8 ((const unsigned char *)text + at, bytes - at, &c);
      ops[i] = (TS_CRDTOp){
        { doc->site, doc->clock + i + 1 }, parent, c, TS_CRDT_INSERT
      };
      parent = ops[i].id;
    }
  status = ts_crdt_apply (doc, ops, chars);
  if (!status && delta)
    {
      delta->ops = ops;
      delta->count = chars;
    }
  else
    free (ops);
  return status;
}
TS_CRDTStatus
ts_crdt_delete (TS_CRDT *doc, size_t index, size_t count, TS_CRDTBatch *delta)
{
  if (delta)
    *delta = (TS_CRDTBatch){ 0 };
  size_t *order = NULL, length;
  TS_CRDTStatus status = visible (doc, &order, &length);
  if (status)
    return status;
  if (index > length || count > length - index)
    {
      free (order);
      return TS_CRDT_INVALID;
    }
  if (!count)
    {
      free (order);
      return TS_CRDT_OK;
    }
  TS_CRDTOp *ops = calloc (count, sizeof *ops);
  if (!ops)
    {
      free (order);
      return TS_CRDT_NOMEM;
    }
  for (size_t i = 0; i < count; i++)
    {
      ops[i].id = doc->nodes[order[index + i]].id;
      ops[i].flags = TS_CRDT_DELETE;
    }
  free (order);
  status = ts_crdt_apply (doc, ops, count);
  if (!status && delta)
    {
      delta->ops = ops;
      delta->count = count;
    }
  else
    free (ops);
  return status;
}
TS_CRDTStatus
ts_crdt_text (const TS_CRDT *doc, char **out, size_t *bytes)
{
  if (!out || !bytes)
    return TS_CRDT_INVALID;
  *out = NULL;
  *bytes = 0;
  size_t *order = NULL, length;
  TS_CRDTStatus status = visible (doc, &order, &length);
  if (status)
    return status;
  char *text = malloc (length * 4 + 1);
  if (!text)
    {
      free (order);
      return TS_CRDT_NOMEM;
    }
  size_t used = 0;
  for (size_t i = 0; i < length; i++)
    used += encode_utf8 (doc->nodes[order[i]].scalar, text + used);
  text[used] = 0;
  free (order);
  *out = text;
  *bytes = used;
  return TS_CRDT_OK;
}
TS_CRDTStatus
ts_crdt_length (const TS_CRDT *doc, size_t *length)
{
  size_t *order = NULL;
  TS_CRDTStatus status = visible (doc, &order, length);
  free (order);
  return status;
}
TS_CRDTStatus
ts_crdt_id_at (const TS_CRDT *doc, size_t index, TS_CRDTId *out)
{
  if (!out)
    return TS_CRDT_INVALID;
  *out = (TS_CRDTId){ 0 };
  size_t *order = NULL, length;
  TS_CRDTStatus status = visible (doc, &order, &length);
  if (!status)
    {
      if (index >= length)
        status = TS_CRDT_INVALID;
      else
        *out = doc->nodes[order[index]].id;
    }
  free (order);
  return status;
}
TS_CRDTStatus
ts_crdt_index_of (const TS_CRDT *doc, TS_CRDTId id, size_t *index, int *found)
{
  if (!index || !found)
    return TS_CRDT_INVALID;
  *index = 0;
  *found = 0;
  size_t *order = NULL, length;
  TS_CRDTStatus status = visible (doc, &order, &length);
  if (!status)
    for (size_t i = 0; i < length; i++)
      if (!id_compare (doc->nodes[order[i]].id, id))
        {
          *index = i;
          *found = 1;
          break;
        }
  free (order);
  return status;
}
static void
put32 (unsigned char *p, uint32_t n)
{
  for (unsigned i = 0; i < 4; i++)
    p[i] = (unsigned char)(n >> (i * 8));
}
static void
put64 (unsigned char *p, uint64_t n)
{
  for (unsigned i = 0; i < 8; i++)
    p[i] = (unsigned char)(n >> (i * 8));
}
static uint32_t
get32 (const unsigned char *p)
{
  uint32_t n = 0;
  for (unsigned i = 0; i < 4; i++)
    n |= (uint32_t)p[i] << (i * 8);
  return n;
}
static uint64_t
get64 (const unsigned char *p)
{
  uint64_t n = 0;
  for (unsigned i = 0; i < 8; i++)
    n |= (uint64_t)p[i] << (i * 8);
  return n;
}
static uint32_t
crc32 (const unsigned char *p, size_t n)
{
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < n; i++)
    {
      crc ^= p[i];
      for (unsigned bit = 0; bit < 8; bit++)
        crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
  return ~crc;
}
static const unsigned char magic[8] = { 'T', 'S', 'R', 'G', 'A', 0, 1, 0 };
TS_CRDTStatus
ts_crdt_encode (const TS_CRDT *doc, unsigned char **out, size_t *bytes)
{
  if (!out || !bytes)
    return TS_CRDT_INVALID;
  *out = NULL;
  *bytes = 0;
  if (!doc)
    return TS_CRDT_INVALID;
  size_t size = TS_CRDT_SNAPSHOT_HEADER + doc->count * TS_CRDT_SNAPSHOT_RECORD;
  unsigned char *buffer = calloc (size, 1);
  if (!buffer)
    return TS_CRDT_NOMEM;
  memcpy (buffer, magic, 8);
  put32 (buffer + 8, (uint32_t)doc->count);
  put32 (buffer + 12, TS_CRDT_SNAPSHOT_RECORD);
  for (size_t i = 0; i < doc->count; i++)
    {
      unsigned char *p
          = buffer + TS_CRDT_SNAPSHOT_HEADER + i * TS_CRDT_SNAPSHOT_RECORD;
      const TS_CRDTOp *op = doc->nodes + i;
      put64 (p, op->id.site);
      put64 (p + 8, op->id.clock);
      put64 (p + 16, op->parent.site);
      put64 (p + 24, op->parent.clock);
      put32 (p + 32, op->scalar);
      put32 (p + 36, op->flags);
    }
  put32 (buffer + 16, crc32 (buffer + TS_CRDT_SNAPSHOT_HEADER,
                             size - TS_CRDT_SNAPSHOT_HEADER));
  *out = buffer;
  *bytes = size;
  return TS_CRDT_OK;
}
TS_CRDTStatus
ts_crdt_encode_ops (const TS_CRDTOp *ops, size_t count, unsigned char **out,
                    size_t *bytes)
{
  if (!out || !bytes)
    return TS_CRDT_INVALID;
  *out = NULL;
  *bytes = 0;
  TS_CRDT *doc = NULL;
  TS_CRDTStatus status = ts_crdt_create (1, TS_CRDT_MAX_NODES, &doc);
  if (!status)
    status = ts_crdt_apply (doc, ops, count);
  if (!status)
    status = ts_crdt_encode (doc, out, bytes);
  ts_crdt_free (doc);
  return status;
}
TS_CRDTStatus
ts_crdt_decode (uint64_t site, size_t limit, const void *data, size_t bytes,
                TS_CRDT **out, size_t *offset)
{
  if (offset)
    *offset = 0;
  if (!out)
    return TS_CRDT_INVALID;
  *out = NULL;
  if (!site || !limit || limit > TS_CRDT_MAX_NODES || (!data && bytes))
    return TS_CRDT_INVALID;
  if (bytes
      > TS_CRDT_SNAPSHOT_HEADER + TS_CRDT_MAX_NODES * TS_CRDT_SNAPSHOT_RECORD)
    return TS_CRDT_LIMIT;
  const unsigned char *p = data;
  if (bytes < TS_CRDT_SNAPSHOT_HEADER)
    {
      if (offset)
        *offset = bytes;
      return TS_CRDT_FORMAT;
    }
  for (size_t i = 0; i < 8; i++)
    if (p[i] != magic[i])
      {
        if (offset)
          *offset = i;
        return TS_CRDT_FORMAT;
      }
  uint32_t count = get32 (p + 8);
  if (count > limit)
    {
      if (offset)
        *offset = 8;
      return TS_CRDT_LIMIT;
    }
  if (get32 (p + 12) != TS_CRDT_SNAPSHOT_RECORD || get32 (p + 20))
    {
      if (offset)
        *offset = get32 (p + 20) ? 20 : 12;
      return TS_CRDT_FORMAT;
    }
  size_t expected
      = TS_CRDT_SNAPSHOT_HEADER + (size_t)count * TS_CRDT_SNAPSHOT_RECORD;
  if (bytes != expected)
    {
      if (offset)
        *offset = bytes < expected ? bytes : expected;
      return TS_CRDT_FORMAT;
    }
  if (get32 (p + 16)
      != crc32 (p + TS_CRDT_SNAPSHOT_HEADER, bytes - TS_CRDT_SNAPSHOT_HEADER))
    {
      if (offset)
        *offset = 16;
      return TS_CRDT_FORMAT;
    }
  TS_CRDT *doc = NULL;
  TS_CRDTStatus status = ts_crdt_create (site, limit, &doc);
  if (status)
    return status;
  if (count)
    {
      doc->nodes = calloc (count, sizeof *doc->nodes);
      if (!doc->nodes)
        {
          ts_crdt_free (doc);
          return TS_CRDT_NOMEM;
        }
    }
  for (size_t i = 0; i < count; i++)
    {
      size_t at = TS_CRDT_SNAPSHOT_HEADER + i * TS_CRDT_SNAPSHOT_RECORD;
      const unsigned char *s = p + at;
      TS_CRDTOp *op = doc->nodes + i;
      *op = (TS_CRDTOp){ { get64 (s), get64 (s + 8) },
                         { get64 (s + 16), get64 (s + 24) },
                         get32 (s + 32),
                         get32 (s + 36) };
      if (!op_valid (op)
          || (i && id_compare (doc->nodes[i - 1].id, op->id) >= 0))
        {
          if (offset)
            *offset = at;
          ts_crdt_free (doc);
          return TS_CRDT_FORMAT;
        }
      if (op->id.clock > doc->clock)
        doc->clock = op->id.clock;
    }
  doc->count = count;
  *out = doc;
  return TS_CRDT_OK;
}
