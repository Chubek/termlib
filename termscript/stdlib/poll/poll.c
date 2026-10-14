/* A bounded readiness set. Descriptors are borrowed, never closed here. */
#include "poll/poll.h"
#include "common/ts_std_common.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <time.h>

#define MAX_WATCHES 1024
typedef struct { struct pollfd fds[MAX_WATCHES]; size_t count; bool closed; } watch_set;
static const char poll_tag;
static void destroy (void *p) { free (p); }

static TS_Status
p_new (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  watch_set *s;
  (void) ud; (void) a;
  if (ts_std_argc (vm, n, 0, 0, e, "poll.new")) return TS_ERR_INVAL;
  s = calloc (1, sizeof *s);
  if (!s) return ts_std_oom (e);
  if (ts_value_make_handle (r, s, destroy, "<poll>", &poll_tag))
    { free (s); return ts_std_oom (e); }
  return TS_OK;
}

static TS_Status
p_edit (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  intptr_t op = (intptr_t) ud;
  int64_t fd, mask;
  size_t i;
  watch_set *s;
  size_t want = op == 0 ? 3 : op == 1 ? 2 : 1;
  if (ts_std_argc (vm, n, want, want, e, "poll")) return TS_ERR_INVAL;
  s = ts_std_handle (a, &poll_tag, e, "poll");
  if (!s) return TS_ERR_INVAL;
  if (op == 2) { s->count = 0; s->closed = true; ts_std_ret_nil (r); return TS_OK; }
  if (s->closed) return ts_std_inval (e, "poll: closed set");
  if (ts_std_int (a + 1, &fd, e, "poll") || fd < 0 || fd > INT_MAX)
    return ts_std_inval (e, "poll: invalid descriptor");
  for (i = 0; i < s->count && s->fds[i].fd != fd; i++) ;
  if (op == 1)
    {
      ts_std_ret_bool (r, i < s->count);
      if (i < s->count) s->fds[i] = s->fds[--s->count];
      return TS_OK;
    }
  if (ts_std_int (a + 2, &mask, e, "poll.watch") || mask < 1 || mask > 3)
    return ts_std_inval (e, "poll.watch: mask must be 1 (read), 2 (write), or 3");
  if (i == MAX_WATCHES)
    { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "poll: watch limit reached"); return TS_ERR_LIMIT; }
  s->fds[i] = (struct pollfd) { (int) fd, (short) (((mask & 1) ? POLLIN : 0) | ((mask & 2) ? POLLOUT : 0)), 0 };
  if (i == s->count) s->count++;
  ts_std_ret_bool (r, true);
  return TS_OK;
}

static int64_t
milliseconds (void)
{
  struct timespec t;
  if (clock_gettime (CLOCK_MONOTONIC, &t)) return -1;
  return (int64_t) t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static TS_Status
p_wait (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  watch_set *s;
  int64_t timeout, deadline = 0, now;
  int left, ready;
  ts_std_list_t *events;
  (void) ud;
  if (ts_std_argc (vm, n, 2, 2, e, "poll.wait") || ts_std_int (a + 1, &timeout, e, "poll.wait")) return TS_ERR_INVAL;
  if (timeout < -1 || timeout > INT_MAX) return ts_std_inval (e, "poll.wait: invalid timeout");
  s = ts_std_handle (a, &poll_tag, e, "poll.wait");
  if (!s) return TS_ERR_INVAL;
  if (s->closed) return ts_std_inval (e, "poll: closed set");
  if (timeout >= 0)
    { now = milliseconds (); if (now < 0) return ts_std_system (e, "clock_gettime"); deadline = now + timeout; }
  left = (int) timeout;
  for (;;)
    {
      ready = poll (s->fds, s->count, left);
      if (ready >= 0) break;
      if (errno != EINTR) return ts_std_system (e, "poll");
      if (timeout >= 0)
        {
          now = milliseconds (); if (now < 0) return ts_std_system (e, "clock_gettime");
          left = now >= deadline ? 0 : (int) (deadline - now);
        }
    }
  events = ts_std_list_new ();
  if (!events) return ts_std_oom (e);
  for (size_t i = 0; i < s->count; i++)
    {
      short re = s->fds[i].revents;
      int mask = ((re & POLLIN) ? 1 : 0) | ((re & POLLOUT) ? 2 : 0) |
                 ((re & POLLHUP) ? 4 : 0) | ((re & POLLERR) ? 8 : 0) | ((re & POLLNVAL) ? 16 : 0);
      if (mask && (ts_std_list_push_int (events, s->fds[i].fd) || ts_std_list_push_int (events, mask)))
        { ts_std_list_free (events); return ts_std_oom (e); }
    }
  return ts_std_list_wrap (r, events, e);
}

static const TS_FuncDef funcs[] = {
  {"new", p_new, NULL}, {"watch", p_edit, (void *) 0},
  {"remove", p_edit, (void *) 1}, {"close", p_edit, (void *) 2},
  {"wait", p_wait, NULL}
};
const TS_Module ts_std_poll_module = TS_MODULE_INIT ("std.poll", funcs);
