/* Sorted vector: O(log n) lookup, O(n) insert/remove, bounded owned keys. */
#include "index/index.h"
#include "common/ts_std_common.h"
#include <stdlib.h>
#include <string.h>

#define INDEX_COUNT 65536u
#define INDEX_KEY 4096u
#define INDEX_BYTES (4u * 1024u * 1024u)
typedef struct { char *key; int64_t value; } entry;
typedef struct
{
  entry *items;
  size_t count, capacity, bytes;
  bool closed;
} key_index;
static const char tag;

static void clear (key_index *s)
{
  for (size_t i = 0; i < s->count; i++) free (s->items[i].key);
  free (s->items);
  s->items = NULL;
  s->count = s->capacity = s->bytes = 0;
}
static void destroy (void *p)
{
  key_index *s = p;
  if (s) { clear (s); free (s); }
}
static size_t lower (key_index *s, const char *key)
{
  size_t lo = 0, hi = s->count;
  while (lo < hi)
    {
      size_t mid = lo + (hi - lo) / 2;
      if (strcmp (s->items[mid].key, key) < 0) lo = mid + 1;
      else hi = mid;
    }
  return lo;
}
static TS_Status create (TS_VM *vm, void *u, const TS_Value *a, size_t n,
                         TS_Value *r, TS_Error *e)
{
  (void)u; (void)a;
  if (ts_std_argc (vm, n, 0, 0, e, "index.new")) return TS_ERR_INVAL;
  key_index *s = calloc (1, sizeof *s);
  if (!s) return ts_std_oom (e);
  if (ts_value_make_handle (r, s, destroy, "index", &tag))
    { destroy (s); return ts_std_oom (e); }
  return TS_OK;
}
static TS_Status operation (TS_VM *vm, void *u, const TS_Value *a, size_t n,
                            TS_Value *r, TS_Error *e)
{
  intptr_t op = (intptr_t)u;
  size_t argc = op == 0 ? 3 : op < 5 ? 2 : 1;
  if (ts_std_argc (vm, n, argc, argc, e, "index")) return TS_ERR_INVAL;
  key_index *s = ts_std_handle (a, &tag, e, "index");
  if (!s) return TS_ERR_INVAL;
  if (op == 7) { clear (s); s->closed = true; ts_std_ret_nil (r); return TS_OK; }
  if (s->closed) return ts_std_inval (e, "index is closed");
  if (op == 5) { ts_std_ret_int (r, (int64_t)s->count); return TS_OK; }
  if (op == 6) { clear (s); ts_std_ret_nil (r); return TS_OK; }
  const char *key;
  if (ts_std_str (a + 1, &key, e, "index key")) return TS_ERR_INVAL;
  size_t len = strnlen (key, INDEX_KEY + 1);
  if (len > INDEX_KEY)
    { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "index key exceeds 4096 bytes"); return TS_ERR_LIMIT; }
  size_t at = lower (s, key);
  bool found = at < s->count && !strcmp (s->items[at].key, key);
  if (op == 4)
    {
      ts_std_list_t *list = ts_std_list_new ();
      if (!list) return ts_std_oom (e);
      for (size_t i = at; i < s->count && !strncmp (s->items[i].key, key, len); i++)
        if (ts_std_list_push_str (list, s->items[i].key))
          { ts_std_list_free (list); return ts_std_oom (e); }
      return ts_std_list_wrap (r, list, e);
    }
  if (op == 1)
    {
      if (!found)
        { ts_error_set (e, TS_ERR_NOTFOUND, 0, 0, "index key not found"); return TS_ERR_NOTFOUND; }
      ts_std_ret_int (r, s->items[at].value); return TS_OK;
    }
  if (op == 2) { ts_std_ret_bool (r, found); return TS_OK; }
  if (op == 3)
    {
      if (found)
        {
          s->bytes -= strlen (s->items[at].key) + 1;
          free (s->items[at].key);
          memmove (s->items + at, s->items + at + 1, (--s->count - at) * sizeof *s->items);
        }
      ts_std_ret_bool (r, found); return TS_OK;
    }
  int64_t value;
  if (ts_std_int (a + 2, &value, e, "index value")) return TS_ERR_INVAL;
  if (found) { s->items[at].value = value; ts_std_ret_bool (r, false); return TS_OK; }
  if (s->count == INDEX_COUNT || len + 1 > INDEX_BYTES - s->bytes)
    { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "index storage limit exceeded"); return TS_ERR_LIMIT; }
  char *copy = strdup (key);
  if (!copy) return ts_std_oom (e);
  if (s->count == s->capacity)
    {
      size_t cap = s->capacity ? s->capacity * 2 : 8;
      entry *items = realloc (s->items, cap * sizeof *items);
      if (!items) { free (copy); return ts_std_oom (e); }
      s->items = items; s->capacity = cap;
    }
  memmove (s->items + at + 1, s->items + at, (s->count - at) * sizeof *s->items);
  s->items[at] = (entry){ copy, value }; s->count++; s->bytes += len + 1;
  ts_std_ret_bool (r, true); return TS_OK;
}
static const TS_FuncDef functions[] = {
  { "new", create, NULL }, { "set", operation, (void *)0 },
  { "get", operation, (void *)1 }, { "has", operation, (void *)2 },
  { "remove", operation, (void *)3 }, { "prefix", operation, (void *)4 },
  { "len", operation, (void *)5 }, { "clear", operation, (void *)6 },
  { "close", operation, (void *)7 }
};
const TS_Module ts_std_index_module = TS_MODULE_INIT ("std.index", functions);
