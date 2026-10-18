/* Private POSIX IPC name validation and syscall error mapping. */
#ifndef TS_STD_IPC_NAME_H
#define TS_STD_IPC_NAME_H
#include "common/ts_std_common.h"
#include <errno.h>
#include <string.h>

static inline TS_Status
ts_ipc_name (const TS_Value *v, const char **name, TS_Error *e)
{
  if (ts_std_str (v, name, e, "IPC name"))
    return TS_ERR_INVAL;
  size_t len = strlen (*name);
  if (len < 2 || len > 240 || (*name)[0] != '/' || strchr (*name + 1, '/'))
    return ts_std_inval (
        e, "IPC name must be /NAME (1..239 name bytes, no slashes)");
  return TS_OK;
}
static inline TS_Status
ts_ipc_error (TS_Error *e, const char *operation)
{
  int saved = errno;
  TS_Status status = saved == EAGAIN || saved == EWOULDBLOCK
                         ? TS_ERR_WOULD_BLOCK
                     : saved == ENOENT   ? TS_ERR_NOTFOUND
                     : saved == EMSGSIZE ? TS_ERR_LIMIT
                                         : TS_ERR_SYSTEM;
  ts_error_set (e, status, saved, 0, "%s: %s", operation, strerror (saved));
  return status;
}
#endif
