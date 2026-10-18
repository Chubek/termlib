#include "uvdev/uvdev.h"
#include "common/device.h"
#ifdef __linux__
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct { int fd; bool created; unsigned char keys[KEY_MAX + 1]; } virtual_t;
static const char tag;
static TS_Status release (virtual_t *v, TS_Error *e)
{
  int saved = 0, fd = v->fd; v->fd = -1;
  if (v->created)
    {
      int rc;
      do rc = ioctl (fd, UI_DEV_DESTROY, 0UL); while (rc < 0 && errno == EINTR);
      if (rc < 0) saved = errno;
      v->created = false;
    }
  if (fd >= 0 && close (fd) && !saved) saved = errno;
  if (saved) { errno = saved; return ts_device_error (e, "uinput close"); }
  return TS_OK;
}
static void destroy (void *p) { release (p, NULL); free (p); }
static TS_Status
create (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  (void)u;
  const char *name, *path = "/dev/uinput";
  if (ts_std_argc (vm, n, 2, 3, e, "uvdev.create")
      || ts_std_str (a, &name, e, "device name")
      || (n == 3 && ts_std_str (a + 2, &path, e, "uinput path"))) return TS_ERR_INVAL;
  if (!*name || strlen (name) >= UINPUT_MAX_NAME_SIZE)
    return ts_std_inval (e, "uinput name must be 1..79 bytes");
  ts_std_list_t *keys = ts_std_list_unwrap (a + 1, e, "enabled key codes");
  if (!keys) return TS_ERR_INVAL;
  size_t count = ts_std_list_len (keys);
  if (!count || count > KEY_MAX) return ts_std_inval (e, "uinput needs 1..KEY_MAX key codes");
  virtual_t *v = calloc (1, sizeof *v);
  if (!v) return ts_std_oom (e);
  v->fd = -1;
  TS_Status status;
  for (size_t i = 0; i < count; i++)
    {
      int64_t key;
      if (ts_std_int (ts_std_list_get (keys, i), &key, e, "key code") || key < 1 || key > KEY_MAX)
        { free (v); return ts_std_inval (e, "uinput key code must be 1..KEY_MAX"); }
      v->keys[key] = 1;
    }
  do v->fd = open (path, O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  while (v->fd < 0 && errno == EINTR);
  if (v->fd < 0) { status = ts_device_error (e, "uinput open"); goto fail; }
  int rc;
  do rc = ioctl (v->fd, UI_SET_EVBIT, (unsigned long)EV_KEY); while (rc < 0 && errno == EINTR);
  if (rc < 0) { status = ts_device_error (e, "UI_SET_EVBIT"); goto fail; }
  for (unsigned key = 1; key <= KEY_MAX; key++)
    if (v->keys[key])
      {
        do rc = ioctl (v->fd, UI_SET_KEYBIT, (unsigned long)key); while (rc < 0 && errno == EINTR);
        if (rc < 0) { status = ts_device_error (e, "UI_SET_KEYBIT"); goto fail; }
      }
  struct uinput_setup setup = {0};
  setup.id.bustype = BUS_VIRTUAL; setup.id.vendor = 1; setup.id.product = 1; setup.id.version = 1;
  memcpy (setup.name, name, strlen (name) + 1);
  do rc = ioctl (v->fd, UI_DEV_SETUP, (void *)&setup); while (rc < 0 && errno == EINTR);
  if (rc < 0) { status = ts_device_error (e, "UI_DEV_SETUP (requires uinput v5)"); goto fail; }
  do rc = ioctl (v->fd, UI_DEV_CREATE, 0UL); while (rc < 0 && errno == EINTR);
  if (rc < 0) { status = ts_device_error (e, "UI_DEV_CREATE"); goto fail; }
  v->created = true;
  status = ts_value_make_handle (r, v, destroy, "uinput keyboard", &tag);
  if (status) { ts_std_oom (e); goto fail; }
  return TS_OK;
fail:
  destroy (v); return status;
}
static TS_Status
operation (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  intptr_t op = (intptr_t)u;
  if (ts_std_argc (vm, n, op == 2 ? 3 : 1, op == 2 ? 3 : 1, e, "uvdev")) return TS_ERR_INVAL;
  virtual_t *v = ts_std_handle (a, &tag, e, "uvdev");
  if (!v) return TS_ERR_INVAL;
  if (!op) return release (v, e);
  if (v->fd < 0) return ts_std_inval (e, "uinput keyboard is closed");
  if (op == 1) { ts_std_ret_int (r, v->fd); return TS_OK; }
  struct input_event event = {0};
  if (op == 2)
    {
      int64_t key, value;
      if (ts_std_int (a + 1, &key, e, "key code") || ts_std_int (a + 2, &value, e, "key value")
          || key < 1 || key > KEY_MAX || !v->keys[key] || value < 0 || value > 2)
        return ts_std_inval (e, "key must be enabled; value is 0 (release), 1 (press), or 2 (repeat)");
      event.type = EV_KEY; event.code = (unsigned short)key; event.value = (int)value;
    }
  else { event.type = EV_SYN; event.code = SYN_REPORT; }
  ssize_t sent;
  do sent = write (v->fd, &event, sizeof event); while (sent < 0 && errno == EINTR);
  if (sent < 0) return ts_device_error (e, "uinput write");
  if ((size_t)sent != sizeof event)
    {
      release (v, NULL); // cannot safely retry a partially accepted native record
      ts_error_set (e, TS_ERR_SYSTEM, EIO, (size_t)sent, "short uinput event write; device closed");
      return TS_ERR_SYSTEM;
    }
  return TS_OK;
}
#else
#define create ts_linux_input_unavailable
#define operation ts_linux_input_unavailable
#endif
static const TS_FuncDef functions[] = {
  { "available", ts_linux_input_available, NULL }, { "create", create, NULL },
  { "close", operation, (void *)0 }, { "fd", operation, (void *)1 },
  { "key", operation, (void *)2 }, { "sync", operation, (void *)3 }
};
const TS_Module ts_std_uvdev_module = TS_MODULE_INIT ("std.uvdev", functions);
