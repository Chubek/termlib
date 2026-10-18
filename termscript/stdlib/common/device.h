#ifndef TS_STD_DEVICE_H
#define TS_STD_DEVICE_H
#include "common/ts_std_common.h"
#include <errno.h>
#include <string.h>
static inline TS_Status
ts_device_error (TS_Error *e, const char *operation)
{
  int saved = errno ? errno : EIO;
  TS_Status status = saved == EAGAIN || saved == EWOULDBLOCK ? TS_ERR_WOULD_BLOCK
                     : saved == ENOENT ? TS_ERR_NOTFOUND : TS_ERR_SYSTEM;
  ts_error_set (e, status, saved, 0, "%s: %s", operation, strerror (saved));
  return status;
}
static inline TS_Status
ts_linux_input_available (TS_VM *vm, void *u, const TS_Value *a, size_t n,
                          TS_Value *r, TS_Error *e)
{
  (void)u; (void)a;
  if (ts_std_argc (vm, n, 0, 0, e, "input.available")) return TS_ERR_INVAL;
#ifdef __linux__
  ts_std_ret_bool (r, true);
#else
  ts_std_ret_bool (r, false);
#endif
  return TS_OK;
}
#ifndef __linux__
static inline TS_Status
ts_linux_input_unavailable (TS_VM *vm, void *u, const TS_Value *a, size_t n,
                            TS_Value *r, TS_Error *e)
{
  (void)vm; (void)u; (void)a; (void)n; (void)r;
  ts_error_set (e, TS_ERR_UNSUPPORTED, 0, 0, "Linux input APIs are unavailable on this platform");
  return TS_ERR_UNSUPPORTED;
}
#endif
#endif
