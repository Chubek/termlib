/* DRM discovery snapshots. Device ownership is independent of any host fd. */
#include "drm/drm.h"
#include "common/ts_std_common.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifdef TS_STD_HAVE_DRM
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

typedef struct { int fd; } drm_device;
static const char drm_tag;
static void d_free (void *p) { drm_device *d = p; if (d && d->fd >= 0) close (d->fd); free (d); }
static drm_device *
d_get (const TS_Value *v, TS_Error *e)
{
  drm_device *d = ts_std_handle (v, &drm_tag, e, "drm");
  if (d && d->fd < 0) { ts_std_inval (e, "drm: closed device"); return NULL; }
  return d;
}

static TS_Status
d_open (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  drm_device *d;
  drmVersionPtr version;
  int fd;
  if (ts_std_argc (vm, n, 1, 1, e, "drm.open")) return TS_ERR_INVAL;
  if (ud)
    {
      int64_t input;
      if (ts_std_int (a, &input, e, "drm.from_fd") || input < 0 || input > INT_MAX)
        return ts_std_inval (e, "drm.from_fd: invalid descriptor");
      do { fd = fcntl ((int) input, F_DUPFD_CLOEXEC, 0); } while (fd < 0 && errno == EINTR);
    }
  else
    {
      const char *path;
      if (ts_std_str (a, &path, e, "drm.open")) return TS_ERR_INVAL;
      do { fd = open (path, O_RDWR | O_CLOEXEC | O_NONBLOCK | O_NOCTTY); } while (fd < 0 && errno == EINTR);
    }
  if (fd < 0) return ts_std_system (e, "drm.open");
  version = drmGetVersion (fd);
  if (!version) { TS_Status st = ts_std_system (e, "drmGetVersion"); close (fd); return st; }
  drmFreeVersion (version);
  d = malloc (sizeof *d);
  if (!d) { close (fd); return ts_std_oom (e); }
  d->fd = fd;
  if (ts_value_make_handle (r, d, d_free, "<drm-device>", &drm_tag))
    { d_free (d); return ts_std_oom (e); }
  return TS_OK;
}

static TS_Status
d_state (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  drm_device *d;
  intptr_t op = (intptr_t) ud;
  if (ts_std_argc (vm, n, 1, 1, e, "drm")) return TS_ERR_INVAL;
  if (!op)
    {
      d = ts_std_handle (a, &drm_tag, e, "drm.close"); if (!d) return TS_ERR_INVAL;
      int fd = d->fd; d->fd = -1;
      if (fd >= 0 && close (fd)) return ts_std_system (e, "drm.close");
      ts_std_ret_nil (r); return TS_OK;
    }
  d = d_get (a, e); if (!d) return TS_ERR_INVAL;
  if (op == 1) { ts_std_ret_int (r, d->fd); return TS_OK; }
  {
    drmVersionPtr version = drmGetVersion (d->fd);
    TS_Status st;
    if (!version) return ts_std_system (e, "drmGetVersion");
    st = ts_std_ret_strn (r, version->name ? version->name : "", version->name_len, e);
    drmFreeVersion (version); return st;
  }
}

static TS_Status
d_cap (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  drm_device *d;
  const char *name;
  uint64_t cap, value;
  (void) ud;
  if (ts_std_argc (vm, n, 2, 2, e, "drm.cap") || ts_std_str (a + 1, &name, e, "cap")) return TS_ERR_INVAL;
  if (!strcmp (name, "dumb_buffer")) cap = DRM_CAP_DUMB_BUFFER;
  else if (!strcmp (name, "prime")) cap = DRM_CAP_PRIME;
  else if (!strcmp (name, "timestamp_monotonic")) cap = DRM_CAP_TIMESTAMP_MONOTONIC;
  else if (!strcmp (name, "async_page_flip")) cap = DRM_CAP_ASYNC_PAGE_FLIP;
  else return ts_std_inval (e, "drm.cap: unknown capability");
  d = d_get (a, e); if (!d) return TS_ERR_INVAL;
  if (drmGetCap (d->fd, cap, &value))
    {
      if (errno == EINVAL || errno == EOPNOTSUPP) { ts_std_ret_nil (r); return TS_OK; }
      return ts_std_system (e, "drmGetCap");
    }
  ts_std_ret_int (r, (int64_t) value); return TS_OK;
}

static TS_Status
d_connectors (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  drm_device *d;
  drmModeRes *res;
  drmModeConnector *conn = NULL;
  ts_dom_t *all = NULL, *obj = NULL, *modes = NULL, *mode = NULL;
  TS_Status st = TS_OK;
  (void) ud;
  if (ts_std_argc (vm, n, 1, 1, e, "drm.connectors")) return TS_ERR_INVAL;
  d = d_get (a, e); if (!d) return TS_ERR_INVAL;
  res = drmModeGetResources (d->fd);
  if (!res) return ts_std_system (e, "drmModeGetResources");
  if (res->count_connectors < 0 || res->count_connectors > 256)
    { st = TS_ERR_LIMIT; goto done; }
  all = ts_dom_new_arr (); if (!all) goto oom;
  for (int i = 0; i < res->count_connectors; i++)
    {
      conn = drmModeGetConnector (d->fd, res->connectors[i]);
      if (!conn) { st = ts_std_system (e, "drmModeGetConnector"); goto done; }
      if (conn->count_modes < 0 || conn->count_modes > 4096) { st = TS_ERR_LIMIT; goto done; }
      obj = ts_dom_new_obj (); modes = ts_dom_new_arr ();
      if (!obj || !modes) goto oom;
#define SET(o, k, v) do { if (ts_dom_obj_set ((o), (k), (v))) goto oom; } while (0)
      SET (obj, "id", ts_dom_new_int (conn->connector_id));
      SET (obj, "type", ts_dom_new_int (conn->connector_type));
      SET (obj, "type_id", ts_dom_new_int (conn->connector_type_id));
      SET (obj, "encoder", ts_dom_new_int (conn->encoder_id));
      SET (obj, "connected", ts_dom_new_bool (conn->connection == DRM_MODE_CONNECTED));
      SET (obj, "width_mm", ts_dom_new_int (conn->mmWidth));
      SET (obj, "height_mm", ts_dom_new_int (conn->mmHeight));
      for (int j = 0; j < conn->count_modes; j++)
        {
          drmModeModeInfo *m = &conn->modes[j];
          char name[DRM_DISPLAY_MODE_LEN + 1];
          memcpy (name, m->name, DRM_DISPLAY_MODE_LEN); name[DRM_DISPLAY_MODE_LEN] = 0;
          mode = ts_dom_new_obj (); if (!mode) goto oom;
          SET (mode, "name", ts_dom_new_str (name));
          SET (mode, "width", ts_dom_new_int (m->hdisplay));
          SET (mode, "height", ts_dom_new_int (m->vdisplay));
          SET (mode, "refresh_hz", ts_dom_new_int (m->vrefresh));
          SET (mode, "preferred", ts_dom_new_bool ((m->type & DRM_MODE_TYPE_PREFERRED) != 0));
          st = ts_dom_arr_push (modes, mode); mode = NULL; if (st) goto oom;
        }
      st = ts_dom_obj_set (obj, "modes", modes); modes = NULL; if (st) goto oom;
      st = ts_dom_arr_push (all, obj); obj = NULL; if (st) goto oom;
      drmModeFreeConnector (conn); conn = NULL;
    }
#undef SET
  drmModeFreeResources (res);
  return ts_dom_wrap (r, all, e);
oom:
  st = ts_std_oom (e);
done:
  if (st == TS_ERR_LIMIT) ts_error_set (e, st, 0, 0, "drm: discovery limit exceeded");
  ts_dom_free (mode); ts_dom_free (modes); ts_dom_free (obj); ts_dom_free (all);
  if (conn) drmModeFreeConnector (conn);
  drmModeFreeResources (res); return st;
}
#else
static TS_Status
d_unsupported (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  (void) vm; (void) ud; (void) a; (void) n; (void) r;
  ts_error_set (e, TS_ERR_UNSUPPORTED, 0, 0, "std.drm was built without libdrm"); return TS_ERR_UNSUPPORTED;
}
#define d_open d_unsupported
#define d_state d_unsupported
#define d_cap d_unsupported
#define d_connectors d_unsupported
#endif
static TS_Status
d_available (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  (void) ud; (void) a;
  if (ts_std_argc (vm, n, 0, 0, e, "drm.available")) return TS_ERR_INVAL;
#ifdef TS_STD_HAVE_DRM
  ts_std_ret_bool (r, true);
#else
  ts_std_ret_bool (r, false);
#endif
  return TS_OK;
}
static const TS_FuncDef funcs[] = {
  {"available", d_available, NULL}, {"open", d_open, NULL}, {"from_fd", d_open, (void *) 1},
  {"close", d_state, (void *) 0}, {"fd", d_state, (void *) 1}, {"driver", d_state, (void *) 2},
  {"cap", d_cap, NULL}, {"connectors", d_connectors, NULL}, {NULL, NULL, NULL}
};
const TS_Module ts_std_drm_module = {"std.drm", funcs};
