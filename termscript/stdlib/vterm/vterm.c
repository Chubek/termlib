/* std.vterm -- incremental VT/UTF-8 parsing over styled Unicode cells. */
#include "vterm/vterm.h"
#include "common/grid.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { VT_GROUND, VT_ESC, VT_CSI, VT_STRING, VT_STRING_ESC, VT_CHARSET };
typedef struct {
  ts_grid_t grid, alternate;
  size_t row, col, saved_row, saved_col, top, bottom;
  ts_cell_style_t style, saved_style;
  bool wrap, pending_wrap, cursor_visible, origin, alt, private_csi, discard;
  int state, params[32];
  size_t nparams;
  unsigned char utf8[4];
  size_t utf8_len, utf8_need;
  int32_t previous_cp, grapheme_state;
  size_t last_row, last_col;
  bool have_last, osc;
} vterm_t;
static const char vterm_tag;

static void
vt_free (void *p)
{
  vterm_t *v = p;
  if (!v) return;
  ts_grid_free (&v->grid);
  ts_grid_free (&v->alternate);
  free (v);
}

static vterm_t *
unwrap (const TS_Value *a, TS_Error *e)
{
  return ts_std_handle (a, &vterm_tag, e, "vterm");
}

static void
break_cluster (vterm_t *v)
{
  v->have_last = false;
  v->grapheme_state = 0;
}

static void
reset (vterm_t *v)
{
  if (v->alt)
    { ts_grid_t tmp = v->grid; v->grid = v->alternate; v->alternate = tmp; v->alt = false; }
  v->row = v->col = v->saved_row = v->saved_col = v->top = 0;
  v->bottom = v->grid.rows - 1;
  v->style = v->saved_style = ts_style_default ();
  v->wrap = v->cursor_visible = true;
  v->pending_wrap = v->origin = false;
  v->state = VT_GROUND;
  v->utf8_len = v->utf8_need = 0;
  break_cluster (v);
  ts_grid_clear (&v->grid, v->style);
}

static void
scroll_region (vterm_t *v, size_t top, size_t bottom, size_t count, bool down)
{
  ts_grid_t *g = &v->grid;
  size_t height = bottom - top + 1, row;
  if (count > height) count = height;
  if (down)
    memmove (g->cells + (top + count) * g->cols, g->cells + top * g->cols,
             (height - count) * g->cols * sizeof *g->cells);
  else
    memmove (g->cells + top * g->cols, g->cells + (top + count) * g->cols,
             (height - count) * g->cols * sizeof *g->cells);
  for (row = 0; row < count; row++)
    ts_grid_erase (g, down ? top + row : bottom - row, 0, g->cols, v->style);
  break_cluster (v);
}

static void
index_line (vterm_t *v, bool reverse)
{
  v->pending_wrap = false;
  if (reverse)
    {
      if (v->row == v->top) scroll_region (v, v->top, v->bottom, 1, true);
      else if (v->row) v->row--;
    }
  else
    {
      if (v->row == v->bottom) scroll_region (v, v->top, v->bottom, 1, false);
      else if (v->row + 1 < v->grid.rows) v->row++;
    }
  break_cluster (v);
}

static TS_Status
put_cp (vterm_t *v, int32_t cp, TS_Error *e)
{
  unsigned char encoded[5] = {0};
  size_t len = (size_t) utf8proc_encode_char (cp, encoded);
  int width = utf8proc_charwidth (cp);
  ts_cell_t c;
  if (v->have_last && !utf8proc_grapheme_break_stateful (v->previous_cp, cp, &v->grapheme_state))
    {
      ts_cell_t *last = &v->grid.cells[v->last_row * v->grid.cols + v->last_col];
      size_t old = strlen (last->text), bytes;
      int combined;
      if (old + len >= TS_CELL_BYTES)
        { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "cell grapheme exceeds 127 bytes"); return TS_ERR_LIMIT; }
      c = *last;
      memcpy (c.text + old, encoded, len + 1);
      ts_text_cluster (c.text, old + len, &bytes, &combined);
      if (combined > c.width && v->last_col + (size_t) combined <= v->grid.cols)
        {
          c.width = (unsigned char) combined;
          v->col = v->last_col + c.width;
          v->pending_wrap = v->col >= v->grid.cols;
          if (v->pending_wrap) v->col = v->grid.cols - 1;
        }
      ts_grid_put_cell (&v->grid, v->last_row, v->last_col, &c);
      v->previous_cp = cp;
      return TS_OK;
    }
  break_cluster (v);
  if (width <= 0) return TS_OK;
  if (v->pending_wrap)
    {
      if (v->wrap) { v->col = 0; index_line (v, false); }
      v->pending_wrap = false;
    }
  if ((size_t) width > v->grid.cols) return TS_OK;
  if (v->col + (size_t) width > v->grid.cols)
    {
      if (!v->wrap) return TS_OK;
      v->col = 0;
      index_line (v, false);
    }
  memset (&c, 0, sizeof c);
  memcpy (c.text, encoded, len + 1);
  c.style = v->style;
  c.width = (unsigned char) width;
  ts_grid_put_cell (&v->grid, v->row, v->col, &c);
  v->last_row = v->row;
  v->last_col = v->col;
  v->previous_cp = cp;
  v->have_last = true;
  v->col += (size_t) width;
  if (v->col >= v->grid.cols) { v->col = v->grid.cols - 1; v->pending_wrap = true; }
  return TS_OK;
}

static size_t
param (vterm_t *v, size_t i, size_t fallback)
{
  return i >= v->nparams || v->params[i] <= 0 ? fallback : (size_t) v->params[i];
}

static size_t
clamp (size_t n, size_t max)
{
  return n < max ? n : max;
}

static void
save_cursor (vterm_t *v)
{
  v->saved_row = v->row; v->saved_col = v->col; v->saved_style = v->style;
}

static void
restore_cursor (vterm_t *v)
{
  v->row = clamp (v->saved_row, v->grid.rows - 1);
  v->col = clamp (v->saved_col, v->grid.cols - 1);
  v->style = v->saved_style;
}

static TS_Status
csi (vterm_t *v, unsigned char final, TS_Error *e)
{
  ts_grid_t *g = &v->grid;
  size_t a = param (v, 0, 1), b = param (v, 1, 1), r;
  size_t low = v->origin ? v->top : 0, high = v->origin ? v->bottom : g->rows - 1;
  if (v->discard) return TS_OK;
  if (final == 'm' && !v->private_csi)
    { ts_style_sgr (&v->style, v->params, v->nparams); return TS_OK; }
  v->pending_wrap = false;
  break_cluster (v);
  if (v->private_csi)
    {
      size_t i;
      if (final != 'h' && final != 'l') return TS_OK;
      for (i = 0; i < v->nparams; i++)
        {
          bool on = final == 'h';
          switch (v->params[i])
            {
            case 6: v->origin = on; v->row = on ? v->top : 0; v->col = 0; break;
            case 7: v->wrap = on; break;
            case 25: v->cursor_visible = on; break;
            case 1049:
              if (on != v->alt)
                {
                  ts_grid_t temp;
                  if (on)
                    {
                      TS_Status st;
                      if (!v->alternate.cells)
                        { st = ts_grid_init (&v->alternate, (int64_t) g->rows, (int64_t) g->cols, e); if (st) return st; }
                      save_cursor (v);
                    }
                  temp = v->grid; v->grid = v->alternate; v->alternate = temp;
                  v->alt = on;
                  if (on) { ts_grid_clear (&v->grid, ts_style_default ()); v->row = v->col = 0; }
                  else restore_cursor (v);
                  v->top = 0; v->bottom = v->grid.rows - 1;
                }
              break;
            default: break;
            }
        }
      return TS_OK;
    }
  switch (final)
    {
    case 'A': v->row = a > v->row - low ? low : v->row - a; break;
    case 'B': case 'e': v->row = clamp (v->row + a, high); break;
    case 'C': case 'a': v->col = clamp (v->col + a, g->cols - 1); break;
    case 'D': v->col = a > v->col ? 0 : v->col - a; break;
    case 'E': v->row = clamp (v->row + a, high); v->col = 0; break;
    case 'F': v->row = a > v->row - low ? low : v->row - a; v->col = 0; break;
    case 'G': case '`': v->col = clamp (a - 1, g->cols - 1); break;
    case 'd': v->row = clamp (low + a - 1, high); break;
    case 'H': case 'f': v->row = clamp (low + a - 1, high); v->col = clamp (b - 1, g->cols - 1); break;
    case 'J':
      a = param (v, 0, 0);
      if (a == 2 || a == 3) ts_grid_clear (g, v->style);
      else if (a == 0)
        {
          ts_grid_erase (g, v->row, v->col, g->cols - v->col, v->style);
          for (r = v->row + 1; r < g->rows; r++) ts_grid_erase (g, r, 0, g->cols, v->style);
        }
      else if (a == 1)
        {
          for (r = 0; r < v->row; r++) ts_grid_erase (g, r, 0, g->cols, v->style);
          ts_grid_erase (g, v->row, 0, v->col + 1, v->style);
        }
      break;
    case 'K':
      a = param (v, 0, 0);
      if (a == 0) ts_grid_erase (g, v->row, v->col, g->cols - v->col, v->style);
      else if (a == 1) ts_grid_erase (g, v->row, 0, v->col + 1, v->style);
      else if (a == 2) ts_grid_erase (g, v->row, 0, g->cols, v->style);
      break;
    case 'X': ts_grid_erase (g, v->row, v->col, a, v->style); break;
    case 'L': case 'M':
      if (v->row >= v->top && v->row <= v->bottom)
        scroll_region (v, v->row, v->bottom, a, final == 'L');
      break;
    case 'S': case 'T': scroll_region (v, v->top, v->bottom, a, final == 'T'); break;
    case '@': case 'P':
      {
        size_t count = clamp (a, g->cols - v->col), c;
        ts_cell_t *line = g->cells + v->row * g->cols;
        /* Remove a character intersecting the insertion/deletion boundary. */
        if (!line[v->col].width) ts_grid_erase (g, v->row, v->col, 1, v->style);
        if (final == '@')
          {
            memmove (line + v->col + count, line + v->col, (g->cols - v->col - count) * sizeof *line);
            for (c = v->col; c < v->col + count; c++) ts_cell_blank (line + c, v->style);
          }
        else
          {
            ts_grid_erase (g, v->row, v->col, count, v->style);
            memmove (line + v->col, line + v->col + count, (g->cols - v->col - count) * sizeof *line);
            for (c = g->cols - count; c < g->cols; c++) ts_cell_blank (line + c, v->style);
          }
        for (c = 0; c < g->cols; c++)
          if ((!line[c].width && (!c || line[c - 1].width != 2)) ||
              (line[c].width == 2 && (c + 1 == g->cols || line[c + 1].width))) ts_cell_blank (line + c, v->style);
        break;
      }
    case 'r':
      a = param (v, 0, 1); b = param (v, 1, g->rows);
      if (a < b && b <= g->rows)
        { v->top = a - 1; v->bottom = b - 1; v->row = v->origin ? v->top : 0; v->col = 0; }
      break;
    case 's': save_cursor (v); break;
    case 'u': restore_cursor (v); break;
    default: break;
    }
  return TS_OK;
}

static TS_Status
feed_byte (vterm_t *v, unsigned char c, TS_Error *e)
{
  if (v->state == VT_STRING || v->state == VT_STRING_ESC)
    {
      if ((v->state == VT_STRING_ESC && c == '\\') || (v->osc && c == 7)) v->state = VT_GROUND;
      else v->state = c == 27 ? VT_STRING_ESC : VT_STRING;
      return TS_OK;
    }
  if (c == 0x18 || c == 0x1a) { v->state = VT_GROUND; v->utf8_len = 0; break_cluster (v); return TS_OK; }
  if (v->state == VT_GROUND && v->utf8_len)
    {
      if ((c & 0xc0) == 0x80)
        {
          v->utf8[v->utf8_len++] = c;
          if (v->utf8_len == v->utf8_need)
            {
              int32_t cp;
              int r = ts_utf8_decode ((char *) v->utf8, v->utf8_len, &cp);
              v->utf8_len = 0;
              return put_cp (v, r < 0 ? 0xfffd : cp, e);
            }
          return TS_OK;
        }
      else
        {
          TS_Status st;
          v->utf8_len = 0;
          st = put_cp (v, 0xfffd, e);
          if (st) return st;
        }
    }
  if (c == 27) { v->state = VT_ESC; return TS_OK; }
  if (c < 32)
    {
      if (c == '\r') { v->col = 0; v->pending_wrap = false; break_cluster (v); }
      else if (c == '\n' || c == '\v' || c == '\f') index_line (v, false);
      else if (c == '\b') { if (v->col) v->col--; v->pending_wrap = false; break_cluster (v); }
      else if (c == '\t') { v->col = clamp ((v->col + 8) & ~(size_t) 7, v->grid.cols - 1); v->pending_wrap = false; break_cluster (v); }
      return TS_OK;
    }
  switch (v->state)
    {
    case VT_ESC:
      v->state = VT_GROUND;
      if (c == '[')
        { v->state = VT_CSI; v->nparams = 1; v->params[0] = -1; v->private_csi = v->discard = false; }
      else if (c == ']' || c == 'P' || c == '^' || c == '_') { v->state = VT_STRING; v->osc = c == ']'; }
      else if (c == '(' || c == ')' || c == '*' || c == '+') v->state = VT_CHARSET;
      else if (c == 'c') reset (v);
      else if (c == '7') save_cursor (v);
      else if (c == '8') { restore_cursor (v); v->pending_wrap = false; break_cluster (v); }
      else if (c == 'M') index_line (v, true);
      else if (c == 'D' || c == 'E') { if (c == 'E') v->col = 0; index_line (v, false); }
      break;
    case VT_CHARSET: v->state = VT_GROUND; break;
    case VT_CSI:
      if (c == '?' && v->nparams == 1 && v->params[0] == -1) v->private_csi = true;
      else if (c >= '0' && c <= '9')
        {
          int *p = &v->params[v->nparams - 1];
          if (*p < 0) *p = 0;
          if (*p > (INT_MAX - (c - '0')) / 10) *p = INT_MAX;
          else *p = *p * 10 + c - '0';
        }
      else if (c == ';' || c == ':')
        {
          if (v->nparams == 32) v->discard = true;
          else v->params[v->nparams++] = -1;
        }
      else if (c >= '@' && c <= '~') { v->state = VT_GROUND; return csi (v, c, e); }
      else v->discard = true;
      break;
    default:
      if (c >= 0xc2 && c <= 0xf4)
        { v->utf8[0] = c; v->utf8_len = 1; v->utf8_need = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4; }
      else if (c >= 0x80) return put_cp (v, 0xfffd, e);
      else if (c != 0x7f) return put_cp (v, c, e);
      break;
    }
  return TS_OK;
}

static TS_Status
vt_new (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  int64_t rows = 24, cols = 80;
  vterm_t *v;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 0, 2, e, "vterm.new") ||
      (n && ts_std_int (a, &rows, e, "vterm.new")) ||
      (n > 1 && ts_std_int (a + 1, &cols, e, "vterm.new"))) return TS_ERR_INVAL;
  v = calloc (1, sizeof *v);
  if (!v) return ts_std_oom (e);
  st = ts_grid_init (&v->grid, rows, cols, e);
  if (st) { vt_free (v); return st; }
  reset (v);
  if (ts_value_make_handle (ret, v, vt_free, "<vterm>", &vterm_tag)) { vt_free (v); return ts_std_oom (e); }
  return TS_OK;
}

static TS_Status
vt_feed (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  vterm_t *v;
  const char *s;
  size_t i, len;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 2, 2, e, "feed") || ts_std_str (a + 1, &s, e, "feed")) return TS_ERR_INVAL;
  v = unwrap (a, e);
  if (!v) return TS_ERR_INVAL;
  len = strlen (s);
  if (len > TS_TEXT_LIMIT) { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "feed exceeds 1 MiB"); return TS_ERR_LIMIT; }
  for (i = 0; i < len; i++) { st = feed_byte (v, (unsigned char) s[i], e); if (st) return st; }
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
vt_get (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  vterm_t *v;
  intptr_t op = (intptr_t) ud;
  int64_t row, col;
  ts_sbuf_t b;
  if (ts_std_argc (vm, n, op == 3 ? 3 : 1, op == 3 ? 3 : 1, e, "vterm")) return TS_ERR_INVAL;
  v = unwrap (a, e);
  if (!v) return TS_ERR_INVAL;
  if (op == 0 || op == 1) return ts_grid_render (ret, &v->grid, op == 1, e);
  if (op == 2)
    {
      ts_sbuf_init (&b);
      if (ts_sbuf_printf (&b, "%zu,%zu", v->row, v->col)) { ts_sbuf_free (&b); return ts_std_oom (e); }
      return ts_std_ret_sbuf (ret, &b, e);
    }
  if (op == 3)
    {
      if (ts_std_int (a + 1, &row, e, "cell") || ts_std_int (a + 2, &col, e, "cell")) return TS_ERR_INVAL;
      if (row < 0 || col < 0 || (uint64_t) row >= v->grid.rows || (uint64_t) col >= v->grid.cols)
        { ts_std_ret_nil (ret); return TS_OK; }
      return ts_cell_wrap (ret, &v->grid.cells[(size_t) row * v->grid.cols + (size_t) col], e);
    }
  if (op == 4) { reset (v); ts_std_ret_nil (ret); }
  else if (op == 5) ts_std_ret_bool (ret, v->cursor_visible);
  else ts_std_ret_int (ret, (int64_t) (op == 6 ? v->grid.rows : v->grid.cols));
  return TS_OK;
}

static TS_Status
vt_resize (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  vterm_t *v;
  int64_t rows, cols;
  ts_grid_t next = {0}, alt = {0};
  size_t r, c;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 3, 3, e, "resize") || ts_std_int (a + 1, &rows, e, "resize") ||
      ts_std_int (a + 2, &cols, e, "resize")) return TS_ERR_INVAL;
  v = unwrap (a, e);
  if (!v) return TS_ERR_INVAL;
  st = ts_grid_init (&next, rows, cols, e);
  if (!st && v->alternate.cells) st = ts_grid_init (&alt, rows, cols, e);
  if (st) { ts_grid_free (&next); return st; }
  for (r = 0; r < next.rows && r < v->grid.rows; r++)
    for (c = 0; c < next.cols && c < v->grid.cols; c++)
      {
        ts_grid_put_cell (&next, r, c, &v->grid.cells[r * v->grid.cols + c]);
        if (alt.cells) ts_grid_put_cell (&alt, r, c, &v->alternate.cells[r * v->alternate.cols + c]);
      }
  ts_grid_free (&v->grid); ts_grid_free (&v->alternate);
  v->grid = next; v->alternate = alt;
  v->row = clamp (v->row, next.rows - 1); v->col = clamp (v->col, next.cols - 1);
  v->top = 0; v->bottom = next.rows - 1;
  v->pending_wrap = false; break_cluster (v);
  ts_std_ret_nil (ret);
  return TS_OK;
}

static const TS_FuncDef funcs[] = {
  {"new", vt_new, NULL}, {"feed", vt_feed, NULL}, {"text", vt_get, NULL},
  {"render", vt_get, (void *) 1}, {"ansi", vt_get, (void *) 1},
  {"cursor", vt_get, (void *) 2}, {"cell", vt_get, (void *) 3},
  {"reset", vt_get, (void *) 4}, {"cursor_visible", vt_get, (void *) 5},
  {"rows", vt_get, (void *) 6}, {"cols", vt_get, (void *) 7},
  {"resize", vt_resize, NULL}, {NULL, NULL, NULL}
};
const TS_Module ts_std_vterm_module = {"std.vterm", funcs};
