/* Explicit Linux uinput keyboard creation/injection. Requires OS permissions;
 * never creates a device on import. Handles own fd/device and destroy both on
 * close/final release. Serialize shared aliases and close. Not a sandbox.
 */
#ifndef TS_STD_UVDEV_H
#define TS_STD_UVDEV_H
#include <termscript/termscript.h>
#ifdef __cplusplus
extern "C"
{
#endif
  extern const TS_Module ts_std_uvdev_module;
#ifdef __cplusplus
}
#endif
#endif
