/* Private read-only filesystem views. Callbacks borrow arguments and return
 * owned values. Views own their root descriptors; no raw descriptor escapes.
 */
#ifndef TS_FS_VIEW_H
#define TS_FS_VIEW_H
#include "common/ts_std_common.h"
TS_Status ts_fs_open (TS_VM *, void *, const TS_Value *, size_t, TS_Value *,
                      TS_Error *);
TS_Status ts_fs_read (TS_VM *, void *, const TS_Value *, size_t, TS_Value *,
                      TS_Error *);
TS_Status ts_fs_list (TS_VM *, void *, const TS_Value *, size_t, TS_Value *,
                      TS_Error *);
TS_Status ts_fs_close (TS_VM *, void *, const TS_Value *, size_t, TS_Value *,
                       TS_Error *);
#endif
