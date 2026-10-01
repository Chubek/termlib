/* Shared styled Unicode cell grid. Private; snapshots never borrow the grid. */
#ifndef TS_STD_GRID_H
#define TS_STD_GRID_H
#include "common/text.h"

#define TS_CELL_BYTES 128
#define TS_COLOR_RGB 0x1000000
enum {
  TS_ATTR_BOLD = 1, TS_ATTR_DIM = 2, TS_ATTR_ITALIC = 4,
  TS_ATTR_UNDERLINE = 8, TS_ATTR_BLINK = 16, TS_ATTR_REVERSE = 32,
  TS_ATTR_HIDDEN = 64, TS_ATTR_STRIKE = 128, TS_ATTR_OVERLINE = 256
};
typedef struct { int32_t fg, bg; uint32_t attrs; } ts_cell_style_t;
typedef struct {
  ts_cell_style_t style;
  unsigned char width; /* 0: continuation, 1/2: leading cell. */
  char text[TS_CELL_BYTES];
} ts_cell_t;
typedef struct { size_t rows, cols; ts_cell_t *cells; } ts_grid_t;

ts_cell_style_t ts_style_default (void);
bool ts_style_equal (ts_cell_style_t a, ts_cell_style_t b);
void ts_style_sgr (ts_cell_style_t *s, const int *params, size_t n);
int ts_style_ansi (ts_sbuf_t *b, ts_cell_style_t s);
TS_Status ts_style_color (const TS_Value *v, int32_t *out, TS_Error *e);
TS_Status ts_style_parse (ts_cell_style_t *out, const TS_Value *v, TS_Error *e);
TS_Status ts_cell_set (ts_cell_t *c, const char *s, size_t n,
                       ts_cell_style_t style, TS_Error *e);
TS_Status ts_cell_wrap (TS_Value *ret, const ts_cell_t *c, TS_Error *e);
const ts_cell_t *ts_cell_unwrap (const TS_Value *v, TS_Error *e);
void ts_cell_blank (ts_cell_t *c, ts_cell_style_t s);
TS_Status ts_grid_init (ts_grid_t *g, int64_t rows, int64_t cols, TS_Error *e);
void ts_grid_free (ts_grid_t *g);
TS_Status ts_grid_resize (ts_grid_t *g, int64_t rows, int64_t cols, TS_Error *e);
void ts_grid_clear (ts_grid_t *g, ts_cell_style_t s);
/* Erases both halves of any wide character touched. */
void ts_grid_erase (ts_grid_t *g, size_t row, size_t col, size_t count,
                    ts_cell_style_t s);
void ts_grid_put_cell (ts_grid_t *g, size_t row, size_t col, const ts_cell_t *c);
TS_Status ts_grid_write (ts_grid_t *g, size_t row, size_t col, const char *text,
                         ts_cell_style_t style, TS_Error *e);
int ts_grid_line (ts_sbuf_t *b, const ts_grid_t *g, size_t row,
                  bool ansi, bool trim);
TS_Status ts_grid_render (TS_Value *ret, const ts_grid_t *g, bool ansi,
                          TS_Error *e);
/* Borrowed only for this C call. */
ts_grid_t *ts_screen_unwrap (const TS_Value *v, TS_Error *e);
#endif
