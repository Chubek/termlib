/* Readiness masks match std.poll, with explicit timeout and cancellation. */
#include "multiplex/multiplex.h"
#include "common/ts_std_common.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Reserve one extra slot for cancellation in the same atomic readiness wait. */
#define WATCH_LIMIT 1024
typedef struct
{
  struct pollfd fds[WATCH_LIMIT + 1];
  size_t count;
  bool closed;
} multiplexer;
static const char tag;
static void destroy (void *p) { free (p); }
static TS_Status create (TS_VM *vm, void *u, const TS_Value *a, size_t n,
                         TS_Value *r, TS_Error *e)
{
  (void)u; (void)a;
  if (ts_std_argc (vm, n, 0, 0, e, "multiplex.new")) return TS_ERR_INVAL;
  multiplexer *s = calloc (1, sizeof *s);
  if (!s) return ts_std_oom (e);
  if (ts_value_make_handle (r, s, destroy, "multiplex", &tag))
    { free (s); return ts_std_oom (e); }
  return TS_OK;
}
static TS_Status edit (TS_VM *vm, void *u, const TS_Value *a, size_t n,
                       TS_Value *r, TS_Error *e)
{
  intptr_t op = (intptr_t)u;
  size_t argc = op == 0 ? 3 : op == 1 ? 2 : 1;
  if (ts_std_argc (vm, n, argc, argc, e, "multiplex")) return TS_ERR_INVAL;
  multiplexer *s = ts_std_handle (a, &tag, e, "multiplex");
  if (!s) return TS_ERR_INVAL;
  if (op == 2) { s->closed = true; s->count = 0; ts_std_ret_nil (r); return TS_OK; }
  if (s->closed) return ts_std_inval (e, "multiplex is closed");
  if (op == 3) { ts_std_ret_int (r, (int64_t)s->count); return TS_OK; }
  int64_t fd, mask;
  if (ts_std_int (a + 1, &fd, e, "multiplex fd") || fd < 0 || fd > INT_MAX)
    return ts_std_inval (e, "multiplex: invalid descriptor");
  size_t at = 0;
  while (at < s->count && s->fds[at].fd != fd) at++;
  if (op == 1)
    {
      bool found = at < s->count;
      if (found)
        memmove (s->fds + at, s->fds + at + 1, (--s->count - at) * sizeof *s->fds);
      ts_std_ret_bool (r, found); return TS_OK;
    }
  if (ts_std_int (a + 2, &mask, e, "multiplex mask") || mask < 1 || mask > 3)
    return ts_std_inval (e, "multiplex mask must be read=1, write=2, or both=3");
  if (at == WATCH_LIMIT)
    { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "multiplex watch limit exceeded"); return TS_ERR_LIMIT; }
  s->fds[at] = (struct pollfd){ (int)fd, (short)(((mask & 1) ? POLLIN : 0) | ((mask & 2) ? POLLOUT : 0)), 0 };
  if (at == s->count) s->count++;
  ts_std_ret_bool (r, true); return TS_OK;
}
static int64_t milliseconds (void)
{
  struct timespec t;
  if (clock_gettime (CLOCK_MONOTONIC, &t)) return -1;
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static TS_Status wait_ready (TS_VM *vm, void *u, const TS_Value *a, size_t n,
                             TS_Value *r, TS_Error *e)
{
  (void)u;
  if (ts_std_argc (vm, n, 2, 3, e, "multiplex.wait")) return TS_ERR_INVAL;
  multiplexer *s = ts_std_handle (a, &tag, e, "multiplex.wait");
  if (!s) return TS_ERR_INVAL;
  if (s->closed) return ts_std_inval (e, "multiplex is closed");
  int64_t timeout, cancel = -1;
  if (ts_std_int (a + 1, &timeout, e, "multiplex timeout") || timeout < -1 || timeout > INT_MAX)
    return ts_std_inval (e, "multiplex timeout must be -1 or 0..INT_MAX milliseconds");
  if (n == 3 && (ts_std_int (a + 2, &cancel, e, "multiplex cancel fd") || cancel < 0 || cancel > INT_MAX))
    return ts_std_inval (e, "multiplex: invalid cancellation descriptor");
  size_t count = s->count;
  if (cancel >= 0) s->fds[count++] = (struct pollfd){ (int)cancel, POLLIN, 0 };
  int left = (int)timeout, ready;
  int64_t deadline = 0;
  if (timeout >= 0)
    {
      int64_t now = milliseconds ();
      if (now < 0) return ts_std_system (e, "clock_gettime");
      deadline = now + timeout;
    }
  for (;;)
    {
      ready = poll (s->fds, count, left);
      if (ready >= 0) break;
      if (errno != EINTR) return ts_std_system (e, "poll");
      if (timeout >= 0)
        {
          int64_t now = milliseconds ();
          if (now < 0) return ts_std_system (e, "clock_gettime");
          left = now >= deadline ? 0 : (int)(deadline - now);
        }
    }
  if (cancel >= 0 && s->fds[s->count].revents)
    {
      if (s->fds[s->count].revents & POLLNVAL)
        { ts_error_set (e, TS_ERR_SYSTEM, EBADF, 0, "invalid cancellation descriptor"); return TS_ERR_SYSTEM; }
      ts_error_set (e, TS_ERR_CANCELLED, 0, 0, "multiplex wait cancelled"); return TS_ERR_CANCELLED;
    }
  if (!ready)
    { ts_error_set (e, TS_ERR_TIMEOUT, 0, 0, "multiplex wait timed out"); return TS_ERR_TIMEOUT; }
  ts_std_list_t *list = ts_std_list_new ();
  if (!list) return ts_std_oom (e);
  for (size_t i = 0; i < s->count; i++)
    {
      short re = s->fds[i].revents;
      int mask = ((re & POLLIN) ? 1 : 0) | ((re & POLLOUT) ? 2 : 0)
                 | ((re & POLLHUP) ? 4 : 0) | ((re & POLLERR) ? 8 : 0) | ((re & POLLNVAL) ? 16 : 0);
      if (mask && (ts_std_list_push_int (list, s->fds[i].fd) || ts_std_list_push_int (list, mask)))
        { ts_std_list_free (list); return ts_std_oom (e); }
    }
  return ts_std_list_wrap (r, list, e);
}
static const TS_FuncDef functions[] = {
  { "new", create, NULL }, { "watch", edit, (void *)0 },
  { "remove", edit, (void *)1 }, { "close", edit, (void *)2 },
  { "len", edit, (void *)3 }, { "wait", wait_ready, NULL }
};
const TS_Module ts_std_multiplex_module = TS_MODULE_INIT ("std.multiplex", functions);
