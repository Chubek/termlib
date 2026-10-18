#define _POSIX_C_SOURCE 200809L
#include "mqueue/mqueue.h"
#include "common/ipc_name.h"
#ifdef TS_STD_HAVE_MQUEUE
#include <fcntl.h>
#include <limits.h>
#include <mqueue.h>
#include <stdlib.h>
#include <unistd.h>

enum { MESSAGE_LIMIT = 65536, QUEUE_LIMIT = 1024 };
typedef struct { mqd_t descriptor; size_t size; } queue_t;
static const char tag;
static TS_Status
release (queue_t *q, TS_Error *e)
{
  mqd_t descriptor = q->descriptor;
  q->descriptor = (mqd_t)-1;
  if (descriptor != (mqd_t)-1 && mq_close (descriptor)) return ts_ipc_error (e, "mq_close");
  return TS_OK;
}
static void destroy (void *p) { release (p, NULL); free (p); }

static TS_Status
open_queue (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  bool create = ud != NULL;
  const char *name;
  int64_t messages = 10, size = 8192;
  if (ts_std_argc (vm, n, 1, create ? 3 : 1, e, "mqueue.open")
      || ts_ipc_name (a, &name, e)
      || (n > 1 && ts_std_int (a + 1, &messages, e, "queue capacity"))
      || (n > 2 && ts_std_int (a + 2, &size, e, "message size"))) return TS_ERR_INVAL;
  if (messages < 1 || messages > QUEUE_LIMIT || size < 1 || size > MESSAGE_LIMIT)
    { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "queue capacity must be 1..1024, message size 1..65536"); return TS_ERR_LIMIT; }
  queue_t *q = malloc (sizeof *q);
  if (!q) return ts_std_oom (e);
  struct mq_attr attr = {0};
  attr.mq_maxmsg = (long)messages; attr.mq_msgsize = (long)size;
  int flags = O_RDWR | O_NONBLOCK | (create ? O_CREAT | O_EXCL : 0);
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  do q->descriptor = mq_open (name, flags, 0600, create ? &attr : NULL);
  while (q->descriptor == (mqd_t)-1 && errno == EINTR);
  if (q->descriptor == (mqd_t)-1)
    { TS_Status st = ts_ipc_error (e, "mq_open"); free (q); return st; }
  TS_Status st;
  if (mq_getattr (q->descriptor, &attr)) { st = ts_ipc_error (e, "mq_getattr"); goto fail; }
  if (attr.mq_msgsize < 1 || attr.mq_msgsize > MESSAGE_LIMIT || attr.mq_maxmsg < 1 || attr.mq_maxmsg > QUEUE_LIMIT)
    { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "existing queue exceeds library limits"); st = TS_ERR_LIMIT; goto fail; }
  q->size = (size_t)attr.mq_msgsize;
  st = ts_value_make_handle (r, q, destroy, "message queue", &tag);
  if (st) { ts_std_oom (e); goto fail; }
  return TS_OK;
fail:
  destroy (q);
  if (create) mq_unlink (name);
  return st;
}

static TS_Status
unlink_queue (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  (void)ud; (void)r;
  const char *name;
  if (ts_std_argc (vm, n, 1, 1, e, "mqueue.unlink") || ts_ipc_name (a, &name, e)) return TS_ERR_INVAL;
  int rc;
  do rc = mq_unlink (name); while (rc < 0 && errno == EINTR);
  return rc ? ts_ipc_error (e, "mq_unlink") : TS_OK;
}

static TS_Status
operation (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  intptr_t op = (intptr_t)ud;
  if (ts_std_argc (vm, n, op == 2 ? 2 : 1, op == 2 ? 3 : 1, e, "mqueue")) return TS_ERR_INVAL;
  queue_t *q = ts_std_handle (a, &tag, e, "mqueue");
  if (!q) return TS_ERR_INVAL;
  if (!op) return release (q, e);
  if (q->descriptor == (mqd_t)-1) return ts_std_inval (e, "message queue is closed");
  if (op == 1 || op == 4)
    {
      struct mq_attr attr;
      if (mq_getattr (q->descriptor, &attr)) return ts_ipc_error (e, "mq_getattr");
      ts_std_ret_int (r, op == 1 ? attr.mq_curmsgs : attr.mq_msgsize);
      return TS_OK;
    }
  if (op == 2)
    {
      const char *text;
      int64_t priority = 0;
      if (ts_std_str (a + 1, &text, e, "mqueue.send")
          || (n == 3 && ts_std_int (a + 2, &priority, e, "message priority"))) return TS_ERR_INVAL;
      long priority_limit = sysconf (_SC_MQ_PRIO_MAX);
      /* POSIX guarantees at least 32 priorities when sysconf is indeterminate. */
      if (priority_limit < 1) priority_limit = 32;
      if (priority < 0 || (uint64_t)priority >= (uint64_t)priority_limit || (uint64_t)priority > UINT_MAX)
        return ts_std_inval (e, "message priority out of range");
      size_t len = strlen (text);
      if (len > q->size)
        { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "message exceeds queue message size"); return TS_ERR_LIMIT; }
      int rc;
      do rc = mq_send (q->descriptor, text, len, (unsigned)priority);
      while (rc < 0 && errno == EINTR);
      if (rc) return ts_ipc_error (e, "mq_send");
      ts_std_ret_int (r, (int64_t)len);
      return TS_OK;
    }
  /* Preallocate the result's container/buffer before removing a message. Payload
   * conversion can still fail after receipt; consumed messages cannot be undone.
   */
  char *text = malloc (q->size + 1);
  ts_std_list_t *list = ts_std_list_new ();
  if (!text || !list) { free (text); ts_std_list_free (list); return ts_std_oom (e); }
  unsigned priority;
  ssize_t received;
  do received = mq_receive (q->descriptor, text, q->size, &priority);
  while (received < 0 && errno == EINTR);
  TS_Status st;
  if (received < 0) { st = ts_ipc_error (e, "mq_receive"); goto done; }
  if (memchr (text, 0, (size_t)received))
    { ts_error_set (e, TS_ERR_UNSUPPORTED, 0, 0, "NUL in received text message (message consumed)"); st = TS_ERR_UNSUPPORTED; goto done; }
  text[received] = 0;
  st = ts_std_list_push_str (list, text);
  if (!st) st = ts_std_list_push_int (list, priority);
  if (st) { ts_std_oom (e); goto done; }
  st = ts_std_list_wrap (r, list, e); list = NULL;
done:
  free (text); ts_std_list_free (list); return st;
}
static TS_Status available (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  (void)ud; (void)a;
  if (ts_std_argc (vm, n, 0, 0, e, "mqueue.available")) return TS_ERR_INVAL;
  ts_std_ret_bool (r, true); return TS_OK;
}
static const TS_FuncDef functions[] = {
  { "available", available, NULL },
  { "create", open_queue, (void *)1 }, { "open", open_queue, NULL },
  { "unlink", unlink_queue, NULL }, { "close", operation, (void *)0 },
  { "len", operation, (void *)1 }, { "send", operation, (void *)2 },
  { "receive", operation, (void *)3 }, { "message_size", operation, (void *)4 }
};
#else
static TS_Status
unavailable (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  (void)vm; (void)ud; (void)a; (void)n; (void)r;
  ts_error_set (e, TS_ERR_UNSUPPORTED, 0, 0, "POSIX message queues are unavailable in this build");
  return TS_ERR_UNSUPPORTED;
}
static TS_Status available (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  (void)ud; (void)a;
  if (ts_std_argc (vm, n, 0, 0, e, "mqueue.available")) return TS_ERR_INVAL;
  ts_std_ret_bool (r, false); return TS_OK;
}
static const TS_FuncDef functions[] = {
  { "available", available, NULL },
  { "create", unavailable, NULL }, { "open", unavailable, NULL },
  { "unlink", unavailable, NULL }, { "close", unavailable, NULL },
  { "len", unavailable, NULL }, { "send", unavailable, NULL },
  { "receive", unavailable, NULL }, { "message_size", unavailable, NULL }
};
#endif
const TS_Module ts_std_mqueue_module = TS_MODULE_INIT ("std.mqueue", functions);
