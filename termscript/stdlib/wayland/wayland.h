/* Optional Wayland registry discovery and external-loop connection management.
 * Handles own display/registry; fd() borrows, connect_fd() duplicates. No
 * native pointers, windows, compositor or scripting callbacks are exposed.
 * Serialize all operations on one handle, including prepared-read sequences
 * and close.
 */
#ifndef TS_STD_WAYLAND_H
#define TS_STD_WAYLAND_H
#include <termscript/termscript.h>
#ifdef __cplusplus
extern "C"
{
#endif
  extern const TS_Module ts_std_wayland_module;
#ifdef __cplusplus
}
#endif
#endif
