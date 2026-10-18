/* Incremental terminal-event decoder. Handles own bounded pending text; feeds
 * copy their input. Events are ordinary owned DOM values, not buffer views.
 * No descriptor/TTY state is acquired or changed. Externally synchronize aliases.
 */
#ifndef TS_STD_INPUT_H
#define TS_STD_INPUT_H
#include <termscript/termscript.h>
#ifdef __cplusplus
extern "C" {
#endif
extern const TS_Module ts_std_input_module;
#ifdef __cplusplus
}
#endif
#endif
