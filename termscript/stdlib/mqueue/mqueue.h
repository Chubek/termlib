/* POSIX named nonblocking message queues. Handles own mq descriptors, names
 * persist until explicit unlink. Receive copies payload/priority into a list.
 * Serialize shared handle access/close. Queue access is NOT authentication.
 */
#ifndef TS_STD_MQUEUE_H
#define TS_STD_MQUEUE_H
#include <termscript/termscript.h>
#ifdef __cplusplus
extern "C" {
#endif
extern const TS_Module ts_std_mqueue_module;
#ifdef __cplusplus
}
#endif
#endif
