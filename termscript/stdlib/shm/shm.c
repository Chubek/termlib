#define _POSIX_C_SOURCE 200809L
#include "shm/shm.h"
#include "common/ipc_name.h"
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum { SHM_LIMIT = 16 * 1024 * 1024 };
typedef struct { unsigned char *data; size_t size; int fd; } shared_t;
static const char tag;

static TS_Status
release (shared_t *s, TS_Error *e)
{
  int saved = 0;
  if (s->data && munmap (s->data, s->size)) saved = errno;
  s->data = NULL;
  s->size = 0;
  /* Never retry close: after EINTR the descriptor may already be reusable. */
  if (s->fd >= 0 && close (s->fd) && !saved) saved = errno;
  s->fd = -1;
  if (saved) { errno = saved; return ts_ipc_error (e, "shm close"); }
  return TS_OK;
}
static void destroy (void *p) { release (p, NULL); free (p); }

static TS_Status
open_shared (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  bool create = ud != NULL;
  const char *name;
  int64_t size = 0;
  if (ts_std_argc (vm, n, create ? 2 : 1, create ? 2 : 1, e, "shm.open")
      || ts_ipc_name (a, &name, e)
      || (create && ts_std_int (a + 1, &size, e, "shm size"))) return TS_ERR_INVAL;
  if (create && (size < 1 || size > SHM_LIMIT))
    { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "shared memory size must be 1..16 MiB"); return TS_ERR_LIMIT; }
  shared_t *s = calloc (1, sizeof *s);
  if (!s) return ts_std_oom (e);
  do s->fd = shm_open (name, O_RDWR | (create ? O_CREAT | O_EXCL : 0), 0600);
  while (s->fd < 0 && errno == EINTR);
  if (s->fd < 0)
    { TS_Status st = ts_ipc_error (e, "shm_open"); free (s); return st; }
  TS_Status st;
  if (create)
    {
      int rc;
      do rc = ftruncate (s->fd, (off_t)size); while (rc < 0 && errno == EINTR);
      if (rc < 0) { st = ts_ipc_error (e, "shm ftruncate"); goto fail; }
    }
  struct stat info;
  if (fstat (s->fd, &info)) { st = ts_ipc_error (e, "shm fstat"); goto fail; }
  if (info.st_size < 1 || info.st_size > SHM_LIMIT)
    { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "shared memory size must be 1..16 MiB"); st = TS_ERR_LIMIT; goto fail; }
  s->size = (size_t)info.st_size;
  void *mapping = mmap (NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
  if (mapping == MAP_FAILED) { st = ts_ipc_error (e, "shm mmap"); goto fail; }
  s->data = mapping;
  st = ts_value_make_handle (r, s, destroy, "shared memory", &tag);
  if (st) { ts_std_oom (e); goto fail; }
  return TS_OK;
fail:
  destroy (s);
  if (create) shm_unlink (name);
  return st;
}

static TS_Status
unlink_shared (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  (void)ud; (void)r;
  const char *name;
  if (ts_std_argc (vm, n, 1, 1, e, "shm.unlink") || ts_ipc_name (a, &name, e)) return TS_ERR_INVAL;
  int rc;
  do rc = shm_unlink (name); while (rc < 0 && errno == EINTR);
  return rc ? ts_ipc_error (e, "shm_unlink") : TS_OK;
}

static TS_Status
operation (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  intptr_t op = (intptr_t)ud;
  size_t count = op < 3 ? 1 : op == 3 ? 2 : 3;
  if (ts_std_argc (vm, n, count, count, e, "shm")) return TS_ERR_INVAL;
  shared_t *s = ts_std_handle (a, &tag, e, "shm");
  if (!s) return TS_ERR_INVAL;
  if (!op) return release (s, e);
  if (s->fd < 0) return ts_std_inval (e, "shared memory is closed");
  if (op == 1) { ts_std_ret_int (r, (int64_t)s->size); return TS_OK; }
  if (op == 2) { ts_std_ret_int (r, s->fd); return TS_OK; }
  int64_t index, byte;
  if (ts_std_int (a + 1, &index, e, "shm index") || index < 0
      || (uint64_t)index >= s->size) return ts_std_inval (e, "shared memory index out of bounds");
  if (op == 3) { ts_std_ret_int (r, s->data[index]); return TS_OK; }
  if (ts_std_int (a + 2, &byte, e, "shm byte") || byte < 0 || byte > 255)
    return ts_std_inval (e, "shared memory byte must be 0..255");
  s->data[index] = (unsigned char)byte;
  return TS_OK;
}
static const TS_FuncDef functions[] = {
  { "create", open_shared, (void *)1 }, { "open", open_shared, NULL },
  { "unlink", unlink_shared, NULL }, { "close", operation, (void *)0 },
  { "len", operation, (void *)1 }, { "fd", operation, (void *)2 },
  { "get", operation, (void *)3 }, { "set", operation, (void *)4 }
};
const TS_Module ts_std_shm_module = TS_MODULE_INIT ("std.shm", functions);
