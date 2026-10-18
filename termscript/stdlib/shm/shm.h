/* POSIX named shared memory: handles own mapping/fd, names persist until unlink.
 * Externally synchronize aliases and memory shared between processes. Peers must
 * not truncate mapped objects (POSIX can deliver SIGBUS); this is not a sandbox.
 */
#ifndef TS_STD_SHM_H
#define TS_STD_SHM_H
#include <termscript/termscript.h>
#ifdef __cplusplus
extern "C" {
#endif
extern const TS_Module ts_std_shm_module;
#ifdef __cplusplus
}
#endif
#endif
