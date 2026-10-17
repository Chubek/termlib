#include "crdt/crdt.h"
#include "common/ts_std_common.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  TS_CRDT *doc;
} document;
static const char tag;
static void
destroy (void *ptr)
{
  document *d = ptr;
  ts_crdt_free (d->doc);
  free (d);
}
static TS_Status
report (TS_CRDTStatus status, TS_Error *error, size_t offset)
{
  if (!status)
    return TS_OK;
  TS_Status result = status == TS_CRDT_NOMEM    ? TS_ERR_NOMEM
                     : status == TS_CRDT_LIMIT  ? TS_ERR_LIMIT
                     : status == TS_CRDT_FORMAT ? TS_ERR_PARSE
                                                : TS_ERR_INVAL;
  ts_error_set (error, result, 0, offset, "%s",
                ts_crdt_status_string (status));
  return result;
}
static document *
unwrap (const TS_Value *value, TS_Error *error)
{
  document *d = ts_std_handle (value, &tag, error, "CRDT document");
  if (d && !d->doc)
    {
      ts_std_inval (error, "CRDT document is closed");
      return NULL;
    }
  return d;
}
/* Takes ownership even if wrapping fails. */
static TS_Status
wrap (TS_CRDT *doc, TS_Value *ret, TS_Error *error)
{
  document *d = malloc (sizeof *d);
  if (!d)
    {
      ts_crdt_free (doc);
      return ts_std_oom (error);
    }
  d->doc = doc;
  if (ts_value_make_handle (ret, d, destroy, "CRDT RGA", &tag))
    {
      destroy (d);
      return ts_std_oom (error);
    }
  return TS_OK;
}
static TS_Status
integer (const TS_Value *value, int64_t *out, TS_Error *error)
{
  if (ts_std_int (value, out, error, "CRDT nonnegative integer") || *out < 0)
    return ts_std_inval (error, "CRDT integer must be nonnegative");
  return TS_OK;
}
/* Binary snapshots are hex-wrapped for the current NUL-terminated string ABI.
 * All state and deltas use the same portable, checked format. */
static TS_Status
encode (const TS_CRDT *doc, const TS_CRDTBatch *batch, TS_Value *ret,
        TS_Error *error)
{
  unsigned char *bytes = NULL;
  size_t size = 0;
  TS_CRDTStatus status
      = batch ? ts_crdt_encode_ops (batch->ops, batch->count, &bytes, &size)
              : ts_crdt_encode (doc, &bytes, &size);
  if (status)
    return report (status, error, 0);
  char *text = malloc (6 + size * 2);
  if (!text)
    {
      free (bytes);
      return ts_std_oom (error);
    }
  static const char hex[] = "0123456789abcdef";
  memcpy (text, "rga1:", 5);
  for (size_t i = 0; i < size; i++)
    {
      text[5 + i * 2] = hex[bytes[i] >> 4];
      text[6 + i * 2] = hex[bytes[i] & 15];
    }
  text[5 + size * 2] = 0;
  free (bytes);
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = text;
  return TS_OK;
}
static int
unhex (char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
static TS_Status
decode (const char *text, uint64_t site, size_t limit, TS_CRDT **out,
        TS_Error *error)
{
  *out = NULL;
  size_t size = strlen (text);
  if (size > 5
                 + 2
                       * (TS_CRDT_SNAPSHOT_HEADER
                          + TS_CRDT_MAX_NODES * TS_CRDT_SNAPSHOT_RECORD))
    return report (TS_CRDT_LIMIT, error, 0);
  if (size < 5 || memcmp (text, "rga1:", 5) || (size - 5) % 2)
    {
      ts_error_set (
          error, TS_ERR_PARSE, 0, 0,
          "CRDT snapshot requires rga1: followed by hexadecimal pairs");
      return TS_ERR_PARSE;
    }
  size_t bytes = (size - 5) / 2;
  unsigned char *buffer = malloc (bytes ? bytes : 1);
  if (!buffer)
    return ts_std_oom (error);
  for (size_t i = 0; i < bytes; i++)
    {
      int hi = unhex (text[5 + i * 2]), lo = unhex (text[6 + i * 2]);
      if (hi < 0 || lo < 0)
        {
          free (buffer);
          ts_error_set (error, TS_ERR_PARSE, 0, 5 + i * 2 + (hi >= 0),
                        "invalid CRDT snapshot hex digit");
          return TS_ERR_PARSE;
        }
      buffer[i] = (unsigned char)((hi << 4) | lo);
    }
  size_t offset = 0;
  TS_CRDTStatus status
      = ts_crdt_decode (site, limit, buffer, bytes, out, &offset);
  free (buffer);
  return report (status, error, status ? 5 + offset * 2 : 0);
}
static TS_Status
create (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret,
        TS_Error *error)
{
  (void)ud;
  int64_t site, limit = TS_CRDT_DEFAULT_LIMIT;
  if (ts_std_argc (vm, n, 1, 2, error, "crdt.new") || integer (a, &site, error)
      || (n == 2 && integer (a + 1, &limit, error)))
    return TS_ERR_INVAL;
  TS_CRDT *doc = NULL;
  if (!site || !limit || limit > TS_CRDT_MAX_NODES)
    return ts_std_inval (error, "invalid CRDT site or node limit");
  TS_CRDTStatus status = ts_crdt_create ((uint64_t)site, (size_t)limit, &doc);
  return status ? report (status, error, 0) : wrap (doc, ret, error);
}
static TS_Status
restore (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret,
         TS_Error *error)
{
  (void)ud;
  int64_t site, limit = TS_CRDT_DEFAULT_LIMIT;
  const char *text;
  if (ts_std_argc (vm, n, 2, 3, error, "crdt.restore")
      || integer (a, &site, error)
      || ts_std_str (a + 1, &text, error, "snapshot")
      || (n == 3 && integer (a + 2, &limit, error)))
    return TS_ERR_INVAL;
  if (!site || !limit || limit > TS_CRDT_MAX_NODES)
    return ts_std_inval (error, "invalid CRDT site or node limit");
  TS_CRDT *doc = NULL;
  TS_Status status = decode (text, (uint64_t)site, (size_t)limit, &doc, error);
  return status ? status : wrap (doc, ret, error);
}
static TS_Status
edit (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret,
      TS_Error *error)
{
  int64_t index, count = 0;
  const char *text = NULL;
  if (ts_std_argc (vm, n, 3, 3, error, "crdt edit")
      || integer (a + 1, &index, error))
    return TS_ERR_INVAL;
  if (ud ? integer (a + 2, &count, error)
         : ts_std_str (a + 2, &text, error, "text"))
    return TS_ERR_INVAL;
  if (index > TS_CRDT_MAX_NODES || count > TS_CRDT_MAX_NODES)
    return ts_std_inval (
        error, "CRDT edit index/count exceeds maximum sequence size");
  document *d = unwrap (a, error);
  if (!d)
    return TS_ERR_INVAL;
  TS_CRDT *copy = NULL;
  TS_CRDTBatch batch = { 0 };
  TS_CRDTStatus code = ts_crdt_clone (d->doc, ts_crdt_site (d->doc), &copy);
  if (!code)
    code = ud ? ts_crdt_delete (copy, (size_t)index, (size_t)count, &batch)
              : ts_crdt_insert (copy, (size_t)index, text, strlen (text),
                                &batch);
  TS_Status status = report (code, error, 0);
  if (!status)
    status = encode (NULL, &batch, ret, error);
  if (!status)
    {
      TS_CRDT *old = d->doc;
      d->doc = copy;
      copy = old;
    }
  ts_crdt_free (copy);
  ts_crdt_batch_free (&batch);
  return status;
}
static TS_Status
combine (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret,
         TS_Error *error)
{
  (void)ret;
  if (ts_std_argc (vm, n, 2, 2, error, "crdt merge/apply"))
    return TS_ERR_INVAL;
  document *d = unwrap (a, error);
  if (!d)
    return TS_ERR_INVAL;
  TS_CRDT *other = NULL;
  if (ud)
    {
      const char *text;
      if (ts_std_str (a + 1, &text, error, "CRDT delta/snapshot"))
        return TS_ERR_INVAL;
      TS_Status status = decode (text, ts_crdt_site (d->doc),
                                 ts_crdt_limit (d->doc), &other, error);
      if (status)
        return status;
    }
  else
    {
      document *source = unwrap (a + 1, error);
      if (!source)
        return TS_ERR_INVAL;
      return report (ts_crdt_merge (d->doc, source->doc), error, 0);
    }
  TS_CRDTStatus code = ts_crdt_merge (d->doc, other);
  ts_crdt_free (other);
  return report (code, error, 0);
}
static TS_Status
fork_doc (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret,
          TS_Error *error)
{
  (void)ud;
  int64_t site;
  if (ts_std_argc (vm, n, 2, 2, error, "crdt.fork")
      || integer (a + 1, &site, error))
    return TS_ERR_INVAL;
  document *d = unwrap (a, error);
  if (!d)
    return TS_ERR_INVAL;
  if ((uint64_t)site == ts_crdt_site (d->doc))
    return ts_std_inval (error, "fork requires a different replica site");
  TS_CRDT *copy = NULL;
  TS_CRDTStatus status = ts_crdt_clone (d->doc, (uint64_t)site, &copy);
  return status ? report (status, error, 0) : wrap (copy, ret, error);
}
static TS_Status
query (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret,
       TS_Error *error)
{
  intptr_t op = (intptr_t)ud;
  if (ts_std_argc (vm, n, 1, 1, error, "crdt query"))
    return TS_ERR_INVAL;
  if (op == 0)
    {
      document *d = ts_std_handle (a, &tag, error, "CRDT document");
      if (!d)
        return TS_ERR_INVAL;
      ts_crdt_free (d->doc);
      d->doc = NULL;
      return TS_OK;
    }
  document *d = unwrap (a, error);
  if (!d)
    return TS_ERR_INVAL;
  if (op == 1)
    {
      char *text = NULL;
      size_t bytes;
      TS_CRDTStatus code = ts_crdt_text (d->doc, &text, &bytes);
      if (code)
        return report (code, error, 0);
      if (memchr (text, 0, bytes))
        {
          free (text);
          ts_error_set (
              error, TS_ERR_UNSUPPORTED, 0, 0,
              "CRDT text contains NUL; use the length-delimited C API");
          return TS_ERR_UNSUPPORTED;
        }
      ts_value_free (ret);
      ret->type = TS_STRING;
      ret->as.string = text;
      return TS_OK;
    }
  if (op == 2)
    return encode (d->doc, NULL, ret, error);
  size_t length = 0;
  if (op == 3)
    {
      TS_CRDTStatus code = ts_crdt_length (d->doc, &length);
      if (code)
        return report (code, error, 0);
    }
  else
    length = ts_crdt_size (d->doc);
  ts_std_ret_int (ret, (int64_t)length);
  return TS_OK;
}
static TS_Status
id_query (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret,
          TS_Error *error)
{
  if (ts_std_argc (vm, n, 2, 2, error, "crdt ID query"))
    return TS_ERR_INVAL;
  document *d = unwrap (a, error);
  if (!d)
    return TS_ERR_INVAL;
  if (!ud)
    {
      int64_t index;
      TS_CRDTId id;
      if (integer (a + 1, &index, error))
        return TS_ERR_INVAL;
      if (index >= TS_CRDT_MAX_NODES)
        return ts_std_inval (error, "CRDT character index out of bounds");
      TS_CRDTStatus code = ts_crdt_id_at (d->doc, (size_t)index, &id);
      if (code)
        return report (code, error, 0);
      char text[34];
      snprintf (text, sizeof text, "%016" PRIx64 ":%016" PRIx64, id.site,
                id.clock);
      return ts_std_ret_str (ret, text, error);
    }
  const char *text;
  TS_CRDTId id = { 0 };
  if (ts_std_str (a + 1, &text, error, "CRDT ID"))
    return TS_ERR_INVAL;
  if (strlen (text) != 33 || text[16] != ':')
    return ts_std_inval (error, "CRDT ID requires 16hex:16hex");
  for (size_t i = 0; i < 33; i++)
    {
      if (i == 16)
        continue;
      int digit = unhex (text[i]);
      if (digit < 0)
        return ts_std_inval (error, "invalid CRDT ID hex digit");
      uint64_t *part = i < 16 ? &id.site : &id.clock;
      *part = (*part << 4) | (unsigned)digit;
    }
  size_t index;
  int found;
  TS_CRDTStatus code = ts_crdt_index_of (d->doc, id, &index, &found);
  if (code)
    return report (code, error, 0);
  if (found)
    ts_std_ret_int (ret, (int64_t)index);
  else
    ts_std_ret_nil (ret);
  return TS_OK;
}
static const TS_FuncDef functions[]
    = { { "new", create, NULL },         { "restore", restore, NULL },
        { "fork", fork_doc, NULL },      { "insert", edit, NULL },
        { "delete", edit, (void *)1 },   { "merge", combine, NULL },
        { "apply", combine, (void *)1 }, { "close", query, (void *)0 },
        { "text", query, (void *)1 },    { "snapshot", query, (void *)2 },
        { "len", query, (void *)3 },     { "size", query, (void *)4 },
        { "id_at", id_query, NULL },     { "index_of", id_query, (void *)1 } };
const TS_Module ts_std_crdt_module = TS_MODULE_INIT ("std.crdt", functions);
