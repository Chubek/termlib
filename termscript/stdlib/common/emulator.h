#ifndef TS_STD_EMULATOR_INTERNAL_H
#define TS_STD_EMULATOR_INTERNAL_H
#include "common/grid.h"
/* Borrowed views, valid only while the corresponding handle is unchanged. */
const ts_grid_t *ts_vterm_grid (const TS_Value *, size_t *row, size_t *col,
                                bool *cursor, TS_Error *);
TS_Status ts_vterm_bytes (const TS_Value *, const char *, size_t, TS_Error *);
TS_Status ts_vterm_resized (const TS_Value *, size_t, size_t, TS_Value *, TS_Error *);
TS_Status ts_font_grid (const TS_Value *, const ts_grid_t *, unsigned, unsigned,
                        uint32_t *, TS_Error *);
#endif
