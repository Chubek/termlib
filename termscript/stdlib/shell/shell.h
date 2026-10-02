/* shell.h -- std.shell: LibShell's POSIX runtime and command graphs.
 *
 * Shell, word and program handles own their payloads. Arguments are borrowed
 * during a call; builders copy strings and retain immutable graph nodes.
 * close(shell) is idempotent and invalidates every alias of that shell.
 * Releasing the last handle closes it automatically. No host cwd/environment
 * mutation is performed. run/capture are synchronous and reap their children.
 *
 * Shell handles require serialized access. Independent shells may be used on
 * independent VMs concurrently; the host must serialize process-environment
 * mutation with new(), which snapshots environ. See shell/README.md for the
 * Termscript API, execution limits and status/error semantics.
 */
#ifndef TS_STD_SHELL_H
#define TS_STD_SHELL_H
#include <termscript/termscript.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const TS_Module ts_std_shell_module;

#ifdef __cplusplus
}
#endif
#endif /* TS_STD_SHELL_H */
