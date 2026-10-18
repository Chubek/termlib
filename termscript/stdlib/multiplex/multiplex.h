/* POSIX poll-based readiness sets. Fds are borrowed and never closed.
 * Serialize operations; cancellation uses a separately signalled borrowed fd.
 */
#ifndef TS_STD_MULTIPLEX_H
#define TS_STD_MULTIPLEX_H
#include <termscript/termscript.h>
#ifdef __cplusplus
extern "C"
{
#endif
  extern const TS_Module ts_std_multiplex_module;
#ifdef __cplusplus
}
#endif
#endif
