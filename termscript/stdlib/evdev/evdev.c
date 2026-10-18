#include "evdev/evdev.h"
#include "common/device.h"
#ifdef __linux__
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { int fd; struct input_event event; size_t filled; bool eof; } device_t;
static const char tag;
static TS_Status release (device_t *d, TS_Error *e)
{
  int fd = d->fd; d->fd = -1; d->filled = 0;
  return fd >= 0 && close (fd) ? ts_device_error (e, "evdev close") : TS_OK;
}
static void destroy (void *p) { release (p, NULL); free (p); }
static TS_Status
open_device (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  if (ts_std_argc (vm, n, 1, 1, e, "evdev.open")) return TS_ERR_INVAL;
  device_t *d = calloc (1, sizeof *d);
  if (!d) return ts_std_oom (e);
  d->fd = -1;
  TS_Status status;
  if (u)
    {
      int64_t fd;
      if (ts_std_int (a, &fd, e, "evdev.from_fd") || fd < 0 || fd > INT_MAX)
        { free (d); return ts_std_inval (e, "invalid input descriptor"); }
      int flags = fcntl ((int)fd, F_GETFL);
      if (flags < 0) { status = ts_device_error (e, "evdev descriptor flags"); goto fail; }
      if (!(flags & O_NONBLOCK) || (flags & O_ACCMODE) == O_WRONLY)
        { status = ts_std_inval (e, "from_fd requires an already nonblocking readable stream"); goto fail; }
      do d->fd = fcntl ((int)fd, F_DUPFD_CLOEXEC, 0); while (d->fd < 0 && errno == EINTR);
    }
  else
    {
      const char *path;
      if (ts_std_str (a, &path, e, "evdev.open")) { free (d); return TS_ERR_INVAL; }
      do d->fd = open (path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
      while (d->fd < 0 && errno == EINTR);
    }
  if (d->fd < 0) { status = ts_device_error (e, "evdev open"); goto fail; }
  if (!u)
    {
      struct stat info; int version, rc;
      if (fstat (d->fd, &info)) { status = ts_device_error (e, "evdev fstat"); goto fail; }
      if (!S_ISCHR (info.st_mode)) { status = ts_std_inval (e, "evdev.open requires a character event device"); goto fail; }
      do rc = ioctl (d->fd, EVIOCGVERSION, &version); while (rc < 0 && errno == EINTR);
      if (rc < 0) { status = ts_device_error (e, "EVIOCGVERSION"); goto fail; }
    }
  status = ts_value_make_handle (r, d, destroy, "evdev", &tag);
  if (status) { ts_std_oom (e); goto fail; }
  return TS_OK;
fail:
  destroy (d); return status;
}
static TS_Status
operation (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  intptr_t op = (intptr_t)u;
  if (ts_std_argc (vm, n, op >= 4 ? 2 : 1, op >= 4 ? 2 : 1, e, "evdev")) return TS_ERR_INVAL;
  device_t *d = ts_std_handle (a, &tag, e, "evdev");
  if (!d) return TS_ERR_INVAL;
  if (!op) return release (d, e);
  if (d->fd < 0) return ts_std_inval (e, "evdev is closed");
  if (op == 1) { ts_std_ret_int (r, d->fd); return TS_OK; }
  if (op == 3)
    {
      char name[256] = {0}; int rc;
      do rc = ioctl (d->fd, EVIOCGNAME (sizeof name - 1), name); while (rc < 0 && errno == EINTR);
      if (rc < 0) return ts_device_error (e, "EVIOCGNAME");
      name[sizeof name - 1] = 0; return ts_std_ret_str (r, name, e);
    }
  if (op >= 4)
    {
      int64_t key;
      if (ts_std_int (a + 1, &key, e, "key code") || key < 0 || key > KEY_MAX)
        return ts_std_inval (e, "key code outside Linux KEY_MAX");
      unsigned char bits[(KEY_MAX + 8) / 8] = {0}; int rc;
      unsigned long request = op == 4 ? EVIOCGBIT (EV_KEY, sizeof bits) : EVIOCGKEY (sizeof bits);
      do rc = ioctl (d->fd, request, bits); while (rc < 0 && errno == EINTR);
      if (rc < 0) return ts_device_error (e, op == 4 ? "EVIOCGBIT" : "EVIOCGKEY");
      ts_std_ret_bool (r, (bits[key / 8] & (1u << (key % 8))) != 0); return TS_OK;
    }
  while (d->filled < sizeof d->event && !d->eof)
    {
      ssize_t got = read (d->fd, (char *)&d->event + d->filled, sizeof d->event - d->filled);
      if (got < 0)
        { if (errno == EINTR) continue; return ts_device_error (e, "evdev read"); }
      if (!got) { d->eof = true; break; }
      d->filled += (size_t)got;
    }
  if (d->filled != sizeof d->event)
    {
      TS_Status status = d->filled ? TS_ERR_PARSE : TS_ERR_EOF;
      ts_error_set (e, status, 0, d->filled, d->filled ? "truncated input_event record" : "event stream EOF");
      return status;
    }
  if ((uint64_t)d->event.input_event_usec > 999999)
    { d->filled = 0; ts_error_set (e, TS_ERR_PARSE, 0, 0, "invalid input_event microseconds"); return TS_ERR_PARSE; }
  ts_dom_t *event = ts_dom_new_obj ();
  if (!event || ts_dom_obj_set (event, "type", ts_dom_new_int (d->event.type))
      || ts_dom_obj_set (event, "code", ts_dom_new_int (d->event.code))
      || ts_dom_obj_set (event, "value", ts_dom_new_int (d->event.value))
      || ts_dom_obj_set (event, "seconds", ts_dom_new_int ((int64_t)d->event.input_event_sec))
      || ts_dom_obj_set (event, "microseconds", ts_dom_new_int ((int64_t)d->event.input_event_usec)))
    { ts_dom_free (event); return ts_std_oom (e); }
  TS_Status status = ts_dom_wrap (r, event, e);
  if (!status) d->filled = 0; // retain a complete record on allocation failure
  return status;
}
#else
#define open_device ts_linux_input_unavailable
#define operation ts_linux_input_unavailable
#endif
static const TS_FuncDef functions[] = {
  { "available", ts_linux_input_available, NULL },
  { "open", open_device, NULL }, { "from_fd", open_device, (void *)1 },
  { "close", operation, (void *)0 }, { "fd", operation, (void *)1 },
  { "read", operation, (void *)2 }, { "name", operation, (void *)3 },
  { "has_key", operation, (void *)4 }, { "pressed", operation, (void *)5 }
};
const TS_Module ts_std_evdev_module = TS_MODULE_INIT ("std.evdev", functions);
