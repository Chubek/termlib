#ifndef TS_STD_COMPOSITOR_INTERNAL_H
#define TS_STD_COMPOSITOR_INTERNAL_H
#include "common/module_call.h"
/* Copies pixels into a pending surface buffer. Failure leaves prior pending
 * state intact. Borrowed surface and pixels, valid only for this call. */
TS_Status ts_surface_pixels (const TS_Value *surface, size_t width, size_t height,
                             const uint32_t *argb, TS_Error *e);
#endif
