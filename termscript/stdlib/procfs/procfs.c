#include "procfs/procfs.h"
#include "common/fs_view.h"
static const TS_FuncDef functions[]
    = { { "open", ts_fs_open, (void *)"/proc" },
        { "read", ts_fs_read, NULL },
        { "list", ts_fs_list, NULL },
        { "close", ts_fs_close, NULL } };
const TS_Module ts_std_procfs_module
    = TS_MODULE_INIT ("std.procfs", functions);
