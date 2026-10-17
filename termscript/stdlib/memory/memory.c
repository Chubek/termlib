/* Bounded byte allocations. No raw pointers or unchecked native addresses. */
#include "memory/memory.h"
#include "common/ts_std_common.h"
#include <stdlib.h>
#include <string.h>

enum
{
  MEMORY_LIMIT = 16 * 1024 * 1024
};
typedef struct
{
  unsigned char *bytes;
  size_t size;
  bool closed;
} memory;
static const char tag;
static void
destroy (void *ptr)
{
  memory *m = ptr;
  free (m->bytes);
  free (m);
}

static TS_Status
allocate (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r,
          TS_Error *e)
{
  (void)ud;
  int64_t size;
  if (ts_std_argc (vm, n, 1, 1, e, "alloc")
      || ts_std_int (a, &size, e, "alloc"))
    return TS_ERR_INVAL;
  if (size < 0 || size > MEMORY_LIMIT)
    {
      ts_error_set (e, TS_ERR_LIMIT, 0, 0,
                    "memory size must be in [0, 16 MiB]");
      return TS_ERR_LIMIT;
    }
  memory *m = calloc (1, sizeof *m);
  if (!m)
    return ts_std_oom (e);
  m->bytes = calloc (size ? (size_t)size : 1, 1);
  if (!m->bytes)
    {
      free (m);
      return ts_std_oom (e);
    }
  m->size = (size_t)size;
  TS_Status st = ts_value_make_handle (r, m, destroy, "memory", &tag);
  if (st != TS_OK)
    {
      destroy (m);
      return ts_std_oom (e);
    }
  return TS_OK;
}

static TS_Status
operation (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r,
           TS_Error *e)
{
  intptr_t op = (intptr_t)ud;
  size_t argc = op < 2 ? 1 : op == 2 || op == 4 ? 2 : 3;
  if (ts_std_argc (vm, n, argc, argc, e, "memory"))
    return TS_ERR_INVAL;
  memory *m = ts_std_handle (a, &tag, e, "memory");
  if (!m)
    return TS_ERR_INVAL;
  if (op == 0)
    {
      free (m->bytes);
      m->bytes = NULL;
      m->size = 0;
      m->closed = true;
      return TS_OK;
    }
  if (m->closed)
    return ts_std_inval (e, "memory allocation is closed");
  if (op == 1)
    {
      ts_std_ret_int (r, (int64_t)m->size);
      return TS_OK;
    }
  int64_t index, value;
  if (op == 4)
    {
      if (ts_std_int (a + 1, &value, e, "fill") || value < 0 || value > 255)
        return ts_std_inval (e, "byte must be in [0, 255]");
      memset (m->bytes, (int)value, m->size);
      return TS_OK;
    }
  if (ts_std_int (a + 1, &index, e, "index") || index < 0
      || (uint64_t)index >= m->size)
    return ts_std_inval (e, "memory index out of bounds");
  if (op == 2)
    {
      ts_std_ret_int (r, m->bytes[index]);
      return TS_OK;
    }
  if (ts_std_int (a + 2, &value, e, "byte") || value < 0 || value > 255)
    return ts_std_inval (e, "byte must be in [0, 255]");
  m->bytes[index] = (unsigned char)value;
  return TS_OK;
}
static const TS_FuncDef functions[]
    = { { "alloc", allocate, NULL },       { "new", allocate, NULL },
        { "close", operation, (void *)0 }, { "len", operation, (void *)1 },
        { "get", operation, (void *)2 },   { "set", operation, (void *)3 },
        { "fill", operation, (void *)4 } };
const TS_Module ts_std_memory_module
    = TS_MODULE_INIT ("std.memory", functions);
