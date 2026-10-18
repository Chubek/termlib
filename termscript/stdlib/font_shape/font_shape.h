/* Shares std.font's opaque font handles and HarfBuzz shaping implementation.
 * Input values are borrowed, results are owned. Serialize access to a shared
 * font handle (including calls through std.font/std.font_discovery).
 */
#ifndef TS_STD_FONT_SHAPE_H
#define TS_STD_FONT_SHAPE_H
#include <termscript/termscript.h>
#ifdef __cplusplus
extern "C" {
#endif
extern const TS_Module ts_std_font_shape_module;
#ifdef __cplusplus
}
#endif
#endif
