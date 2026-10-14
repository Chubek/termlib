/* std.screen -- styled Unicode grids, clipping and snapshot cell access. */
#include "screen/screen.h"
#include "common/grid.h"
#include <stdlib.h>
#include <string.h>

static const char screen_tag;

static void
screen_free (void *p)
{
  ts_grid_free (p);
  free (p);
}

ts_grid_t *
ts_screen_unwrap (const TS_Value *v, TS_Error *e)
{
  return ts_std_handle (v, &screen_tag, e, "screen");
}

static TS_Status
s_new (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  int64_t rows = 24, cols = 80;
  ts_grid_t *g;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 0, 2, e, "screen.new") ||
      (n && ts_std_int (a, &rows, e, "screen.new")) ||
      (n > 1 && ts_std_int (a + 1, &cols, e, "screen.new"))) return TS_ERR_INVAL;
  g = calloc (1, sizeof *g);
  if (!g) return ts_std_oom (e);
  st = ts_grid_init (g, rows, cols, e);
  if (st) { free (g); return st; }
  if (ts_value_make_handle (ret, g, screen_free, "<screen>", &screen_tag))
    { screen_free (g); return ts_std_oom (e); }
  return TS_OK;
}

static TS_Status
s_put (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  ts_grid_t *g;
  int64_t row, col;
  const char *s;
  ts_cell_style_t style = ts_style_default ();
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 4, 5, e, "put") || ts_std_int (a + 1, &row, e, "put") ||
      ts_std_int (a + 2, &col, e, "put")) return TS_ERR_INVAL;
  g = ts_screen_unwrap (a, e);
  if (!g) return TS_ERR_INVAL;
  if (n == 5) { st = ts_style_parse (&style, a + 4, e); if (st) return st; }
  if (row < 0 || col < 0 || (uint64_t) row >= g->rows || (uint64_t) col >= g->cols)
    { ts_std_ret_bool (ret, false); return TS_OK; }
  if (a[3].type == TS_HANDLE)
    {
      const ts_cell_t *c = ts_cell_unwrap (a + 3, e);
      ts_cell_t next;
      if (!c) return TS_ERR_INVAL;
      next = *c;
      if (n == 5) next.style = style;
      ts_grid_put_cell (g, (size_t) row, (size_t) col, &next);
    }
  else
    {
      if (ts_std_str (a + 3, &s, e, "put")) return TS_ERR_INVAL;
      st = ts_grid_write (g, (size_t) row, (size_t) col, s, style, e);
      if (st) return st;
    }
  ts_std_ret_bool (ret, true);
  return TS_OK;
}

static TS_Status
s_fill (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  ts_grid_t *g;
  ts_cell_t c;
  const char *s;
  size_t row, col;
  TS_Status st;
  if (ts_std_argc (vm, n, ud ? 1 : 2, ud ? 1 : 2, e, "fill")) return TS_ERR_INVAL;
  g = ts_screen_unwrap (a, e);
  if (!g) return TS_ERR_INVAL;
  if (ud) ts_grid_clear (g, ts_style_default ());
  else
    {
      if (a[1].type == TS_HANDLE)
        { const ts_cell_t *v = ts_cell_unwrap (a + 1, e); if (!v) return TS_ERR_INVAL; c = *v; }
      else
        {
          if (ts_std_str (a + 1, &s, e, "fill")) return TS_ERR_INVAL;
          st = ts_cell_set (&c, s, strlen (s), ts_style_default (), e);
          if (st) return st;
        }
      if (c.width < 1 || c.width > 2) return ts_std_inval (e, "fill: leading cell required");
      ts_grid_clear (g, c.style);
      for (row = 0; row < g->rows; row++)
        for (col = 0; col + c.width <= g->cols; col += c.width) ts_grid_put_cell (g, row, col, &c);
    }
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
s_get (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  ts_grid_t *g;
  int64_t row, col;
  ts_sbuf_t b;
  intptr_t op = (intptr_t) ud;
  if (ts_std_argc (vm, n, op == 4 ? 2 : op == 5 ? 3 : 1,
                  op == 4 ? 2 : op == 5 ? 3 : 1, e, "screen")) return TS_ERR_INVAL;
  g = ts_screen_unwrap (a, e);
  if (!g) return TS_ERR_INVAL;
  if (op == 0 || op == 1) return ts_grid_render (ret, g, op == 1, e);
  if (op == 2 || op == 3)
    { ts_std_ret_int (ret, (int64_t) (op == 2 ? g->rows : g->cols)); return TS_OK; }
  if (ts_std_int (a + 1, &row, e, "screen")) return TS_ERR_INVAL;
  if (op == 5 && ts_std_int (a + 2, &col, e, "screen")) return TS_ERR_INVAL;
  if (row < 0 || (uint64_t) row >= g->rows || (op == 5 && (col < 0 || (uint64_t) col >= g->cols)))
    { ts_std_ret_nil (ret); return TS_OK; }
  if (op == 5) return ts_cell_wrap (ret, &g->cells[(size_t) row * g->cols + (size_t) col], e);
  ts_sbuf_init (&b);
  if (ts_grid_line (&b, g, (size_t) row, false, true)) { ts_sbuf_free (&b); return ts_std_oom (e); }
  return ts_std_ret_sbuf (ret, &b, e);
}

static TS_Status
s_resize (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  ts_grid_t *g;
  int64_t rows, cols;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 3, 3, e, "resize") || ts_std_int (a + 1, &rows, e, "resize") ||
      ts_std_int (a + 2, &cols, e, "resize")) return TS_ERR_INVAL;
  g = ts_screen_unwrap (a, e);
  if (!g) return TS_ERR_INVAL;
  st = ts_grid_resize (g, rows, cols, e);
  ts_std_ret_nil (ret);
  return st;
}

static const TS_FuncDef funcs[] = {
  {"new", s_new, NULL}, {"put", s_put, NULL}, {"clear", s_fill, (void *) 1},
  {"fill", s_fill, NULL}, {"text", s_get, NULL}, {"render", s_get, (void *) 1},
  {"ansi", s_get, (void *) 1}, {"rows", s_get, (void *) 2}, {"cols", s_get, (void *) 3},
  {"line", s_get, (void *) 4}, {"cell", s_get, (void *) 5}, {"resize", s_resize, NULL},
};
const TS_Module ts_std_screen_module = TS_MODULE_INIT ("std.screen", funcs);
