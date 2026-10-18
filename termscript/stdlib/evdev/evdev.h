/* Linux event devices; handles own descriptors, fd() borrows. from_fd
 * duplicates a borrowed, already-nonblocking native input_event stream without
 * changing its file status flags. Serialize aliases/close and reads of the
 * same stream.
 */
#ifndef TS_STD_EVDEV_H
#define TS_STD_EVDEV_H
#include <termscript/termscript.h>
#ifdef __cplusplus
extern "C"
{
#endif
  extern const TS_Module ts_std_evdev_module;
#ifdef __cplusplus
}
#endif
#endif
