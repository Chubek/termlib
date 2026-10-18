#include "wayland/wayland.h"
#include "common/device.h"
#ifdef TS_STD_HAVE_WAYLAND
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>

enum { GLOBAL_LIMIT = 4096, INTERFACE_LIMIT = 255 };
typedef struct { uint32_t name, version; char *interface; } global_t;
typedef struct
{
  struct wl_display *display;
  struct wl_registry *registry;
  global_t globals[GLOBAL_LIMIT];
  size_t count;
  bool reading;
  TS_Status fault;
  int fault_errno;
} display_t;
static const char tag;
static void release (display_t *d)
{
  if (d->reading) { wl_display_cancel_read (d->display); d->reading = false; }
  if (d->registry) { wl_registry_destroy (d->registry); d->registry = NULL; }
  if (d->display) { wl_display_disconnect (d->display); d->display = NULL; }
  for (size_t i = 0; i < d->count; i++) free (d->globals[i].interface);
  d->count = 0;
}
static void destroy (void *p) { release (p); free (p); }
static void added (void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version)
{
  (void)registry;
  display_t *d = data;
  if (d->fault) return;
  if (!name || !version || !interface || !*interface || strlen (interface) > INTERFACE_LIMIT)
    { d->fault = TS_ERR_SYSTEM; d->fault_errno = EPROTO; return; }
  for (size_t i = 0; i < d->count; i++)
    if (d->globals[i].name == name) { d->fault = TS_ERR_SYSTEM; d->fault_errno = EPROTO; return; }
  if (d->count == GLOBAL_LIMIT) { d->fault = TS_ERR_LIMIT; return; }
  char *copy = strdup (interface);
  if (!copy) { d->fault = TS_ERR_NOMEM; d->fault_errno = ENOMEM; return; }
  d->globals[d->count++] = (global_t){ name, version, copy };
}
static void removed (void *data, struct wl_registry *registry, uint32_t name)
{
  (void)registry;
  display_t *d = data;
  for (size_t i = 0; i < d->count; i++)
    if (d->globals[i].name == name)
      {
        free (d->globals[i].interface);
        memmove (d->globals + i, d->globals + i + 1, (d->count - i - 1) * sizeof *d->globals);
        d->count--; break;
      }
}
static const struct wl_registry_listener listener = { added, removed };
static TS_Status
connect_display (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  if (ts_std_argc (vm, n, u ? 1 : 0, 1, e, "wayland.connect")) return TS_ERR_INVAL;
  display_t *d = calloc (1, sizeof *d);
  if (!d) return ts_std_oom (e);
  TS_Status status;
  if (u)
    {
      int64_t fd;
      if (ts_std_int (a, &fd, e, "wayland.connect_fd") || fd < 0 || fd > INT_MAX)
        { free (d); return ts_std_inval (e, "invalid Wayland descriptor"); }
      int flags = fcntl ((int)fd, F_GETFL);
      if (flags < 0) { status = ts_device_error (e, "Wayland descriptor flags"); goto fail; }
      if (!(flags & O_NONBLOCK))
        { status = ts_std_inval (e, "connect_fd requires an already nonblocking Unix stream"); goto fail; }
      struct sockaddr_un peer = {0}; socklen_t length = sizeof peer;
      int type = 0; socklen_t type_length = sizeof type;
      if (getpeername ((int)fd, (struct sockaddr *)&peer, &length)
          || getsockopt ((int)fd, SOL_SOCKET, SO_TYPE, &type, &type_length))
        { status = ts_device_error (e, "Wayland socket validation"); goto fail; }
      if (length < sizeof peer.sun_family || type_length != sizeof type
          || peer.sun_family != AF_UNIX || type != SOCK_STREAM)
        { status = ts_std_inval (e, "Wayland descriptor must be a connected Unix stream"); goto fail; }
      int copy;
      do copy = fcntl ((int)fd, F_DUPFD_CLOEXEC, 0); while (copy < 0 && errno == EINTR);
      if (copy < 0) { status = ts_device_error (e, "Wayland descriptor duplication"); goto fail; }
      // Wayland adopts the duplicate on both success and failure.
      d->display = wl_display_connect_to_fd (copy);
    }
  else
    {
      const char *name = NULL;
      if (n && ts_std_str (a, &name, e, "display name")) { free (d); return TS_ERR_INVAL; }
      // libwayland consumes WAYLAND_SOCKET even with an explicit name. Never
      // silently adopt a descriptor from the embedding process's environment.
      if (getenv ("WAYLAND_SOCKET"))
        { status = ts_std_inval (e, "WAYLAND_SOCKET is set; use explicit connect_fd to borrow it"); goto fail; }
      d->display = wl_display_connect (name);
    }
  if (!d->display)
    { status = errno == ENOMEM ? ts_std_oom (e) : ts_device_error (e, "Wayland connect"); goto fail; }
  wl_display_set_max_buffer_size (d->display, 1048576);
  d->registry = wl_display_get_registry (d->display);
  if (!d->registry) { status = ts_std_oom (e); goto fail; }
  if (wl_registry_add_listener (d->registry, &listener, d))
    { status = ts_std_inval (e, "cannot install Wayland registry listener"); goto fail; }
  status = ts_value_make_handle (r, d, destroy, "Wayland connection", &tag);
  if (status) { ts_std_oom (e); goto fail; }
  return TS_OK;
fail:
  destroy (d); return status;
}
static TS_Status snapshot (display_t *d, TS_Value *r, TS_Error *e)
{
  ts_std_list_t *list = ts_std_list_new ();
  if (!list) return ts_std_oom (e);
  for (size_t i = 0; i < d->count; i++)
    {
      ts_dom_t *item = ts_dom_new_obj ();
      if (!item || ts_dom_obj_set (item, "name", ts_dom_new_int (d->globals[i].name))
          || ts_dom_obj_set (item, "version", ts_dom_new_int (d->globals[i].version))
          || ts_dom_obj_set (item, "interface", ts_dom_new_str (d->globals[i].interface)))
        { ts_dom_free (item); ts_std_list_free (list); return ts_std_oom (e); }
      TS_Value value = {0};
      TS_Status status = ts_dom_wrap (&value, item, e);
      if (!status) status = ts_std_list_push (list, &value);
      ts_value_free (&value);
      if (status) { ts_std_list_free (list); return ts_std_oom (e); }
    }
  return ts_std_list_wrap (r, list, e);
}
static TS_Status registry_failure (display_t *d, TS_Error *e)
{
  const char *message = d->fault == TS_ERR_NOMEM ? "Wayland registry allocation failed"
                        : d->fault == TS_ERR_LIMIT ? "Wayland registry exceeds 4096 globals"
                        : "invalid or duplicate Wayland registry metadata";
  ts_error_set (e, d->fault, d->fault_errno, 0, "%s", message);
  return d->fault;
}
static TS_Status
operation (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  intptr_t op = (intptr_t)u;
  if (ts_std_argc (vm, n, 1, 1, e, "wayland")) return TS_ERR_INVAL;
  display_t *d = ts_std_handle (a, &tag, e, "Wayland connection");
  if (!d) return TS_ERR_INVAL;
  if (!op) { release (d); return TS_OK; }
  if (!d->display) return ts_std_inval (e, "Wayland connection is closed");
  if (op == 1) { ts_std_ret_int (r, wl_display_get_fd (d->display)); return TS_OK; }
  if (op == 6)
    { if (d->reading) wl_display_cancel_read (d->display); d->reading = false; return TS_OK; }
  if (d->fault) return registry_failure (d, e);
  if (op == 7) return snapshot (d, r, e);
  if (op == 2)
    {
      int rc;
      do rc = wl_display_flush (d->display); while (rc < 0 && errno == EINTR);
      if (rc < 0) return ts_device_error (e, "Wayland flush");
      ts_std_ret_int (r, rc); return TS_OK;
    }
  if (op == 5)
    {
      if (!d->reading) return ts_std_inval (e, "Wayland read_events requires prepare_read");
      d->reading = false;
      // The read intention is consumed even on EINTR: the caller must prepare
      // again. Retrying read_events here would violate the library contract.
      if (wl_display_read_events (d->display) < 0)
        {
          if (errno == EPIPE)
            { ts_error_set (e, TS_ERR_EOF, EPIPE, 0, "Wayland peer disconnected"); return TS_ERR_EOF; }
          return ts_device_error (e, "Wayland read events");
        }
      return TS_OK;
    }
  if (d->reading) return ts_std_inval (e, "Wayland read is already prepared; read or cancel it first");
  if (op == 4)
    {
      if (wl_display_prepare_read (d->display) < 0) return ts_device_error (e, "Wayland prepare read");
      d->reading = true; return TS_OK;
    }
  int rc = wl_display_dispatch_pending (d->display);
  if (rc < 0) return ts_device_error (e, "Wayland dispatch pending");
  if (d->fault) return registry_failure (d, e);
  ts_std_ret_int (r, rc); return TS_OK;
}
#else
static TS_Status unavailable (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  (void)vm; (void)u; (void)a; (void)n; (void)r;
  ts_error_set (e, TS_ERR_UNSUPPORTED, 0, 0, "Wayland client support is unavailable in this build");
  return TS_ERR_UNSUPPORTED;
}
#define connect_display unavailable
#define operation unavailable
#endif
static TS_Status available (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  (void)u; (void)a;
  if (ts_std_argc (vm, n, 0, 0, e, "wayland.available")) return TS_ERR_INVAL;
#ifdef TS_STD_HAVE_WAYLAND
  ts_std_ret_bool (r, true);
#else
  ts_std_ret_bool (r, false);
#endif
  return TS_OK;
}
static const TS_FuncDef functions[] = {
  { "available", available, NULL }, { "connect", connect_display, NULL },
  { "connect_fd", connect_display, (void *)1 }, { "close", operation, (void *)0 },
  { "fd", operation, (void *)1 }, { "flush", operation, (void *)2 },
  { "dispatch_pending", operation, (void *)3 }, { "prepare_read", operation, (void *)4 },
  { "read_events", operation, (void *)5 }, { "cancel_read", operation, (void *)6 },
  { "globals", operation, (void *)7 }
};
const TS_Module ts_std_wayland_module = TS_MODULE_INIT ("std.wayland", functions);
