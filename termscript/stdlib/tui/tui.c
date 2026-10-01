#include "tui/tui.h"
#include "screen/screen.h"
#include "bidi/bidi.h"
#include "common/grid.h"
#include "common/terminal.h"
#include <stdlib.h>
#include <string.h>

typedef struct { TS_Value screen, terminal; ts_grid_t previous; bool valid, active; } tui_t;
static const char tui_tag;
static const char enter[] = "\033[?1049h\033[?25l\033[?2004h\033[0m";
static const char leave[] = "\033[0m\033[?2004l\033[?1000l\033[?1006l\033[?25h\033[?1049l";

static TS_Status
end_tui (tui_t *t, TS_Error *e)
{
  TS_Status st = TS_OK, restored;
  if (t->active)
    {
      ts_terminal_t *s = ts_terminal_unwrap (&t->terminal, e);
      if (s)
        {
          ts_terminal_t cleanup = *s;
          cleanup.cancel[0] = -1; /* Cleanup remains possible after cancellation. */
          st = ts_terminal_write_all (&cleanup, leave, sizeof leave - 1, 100, e);
          restored = ts_tty_restore (&s->tty, st ? NULL : e);
          if (!st) st = restored;
        }
      else st = TS_ERR_INVAL;
      t->active = false;
      ts_value_free (&t->terminal);
    }
  return st;
}

static void
tui_free (void *p)
{
  tui_t *t = p;
  if (!t) return;
  (void) end_tui (t, NULL);
  ts_grid_free (&t->previous); ts_value_free (&t->screen); free (t);
}

static TS_Status
screen_call (TS_VM *vm, const char *name, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  const TS_FuncDef *fn = ts_std_screen_module.funcs;
  while (fn->name && strcmp (fn->name, name)) fn++;
  return fn->func (vm, fn->userdata, a, n, ret, e);
}

static TS_Status
t_new (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  tui_t *t = calloc (1, sizeof *t);
  ts_grid_t *g;
  TS_Status st;
  (void) ud;
  if (!t) return ts_std_oom (e);
  st = screen_call (vm, "new", a, n, &t->screen, e);
  if (st) { tui_free (t); return st; }
  g = ts_screen_unwrap (&t->screen, e);
  st = ts_grid_init (&t->previous, (int64_t) g->rows, (int64_t) g->cols, e);
  if (st) { tui_free (t); return st; }
  if (ts_value_make_handle (ret, t, tui_free, "<tui>", &tui_tag)) { tui_free (t); return ts_std_oom (e); }
  return TS_OK;
}

static TS_Status
t_put (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  tui_t *t;
  TS_Value args[5], visual = {0};
  TS_Status st;
  if (ts_std_argc (vm, n, 4, 5, e, "put")) return TS_ERR_INVAL;
  t = ts_std_handle (a, &tui_tag, e, "put"); if (!t) return TS_ERR_INVAL;
  memcpy (args, a, n * sizeof *a); args[0] = t->screen;
  if (ud)
    {
      st = ts_std_bidi_module.funcs[0].func (vm, NULL, a + 3, 1, &visual, e);
      if (st) return st;
      args[3] = visual;
    }
  st = screen_call (vm, "put", args, n, ret, e); ts_value_free (&visual); return st;
}

static TS_Status
diff (tui_t *t, TS_Value *ret, TS_Error *e)
{
  ts_grid_t *g = ts_screen_unwrap (&t->screen, e);
  ts_sbuf_t b;
  size_t r;
  if (g->rows != t->previous.rows || g->cols != t->previous.cols)
    {
      TS_Status st = ts_grid_resize (&t->previous, (int64_t) g->rows, (int64_t) g->cols, e);
      if (st) return st;
      t->valid = false;
    }
  ts_sbuf_init (&b);
  for (r = 0; r < g->rows; r++)
    if (!t->valid || memcmp (g->cells + r * g->cols, t->previous.cells + r * g->cols, g->cols * sizeof *g->cells))
      {
        /* Full changed rows preserve background cells and clear old wide text. */
        if (ts_sbuf_printf (&b, "\033[%zu;1H", r + 1) || ts_grid_line (&b, g, r, true, false))
          { ts_sbuf_free (&b); return ts_std_oom (e); }
      }
  return ts_std_ret_sbuf (ret, &b, e);
}

static void
commit (tui_t *t)
{
  ts_grid_t *g = ts_screen_unwrap (&t->screen, NULL);
  memcpy (t->previous.cells, g->cells, g->rows * g->cols * sizeof *g->cells);
  t->valid = true;
}

static TS_Status
t_action (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  tui_t *t;
  intptr_t op = (intptr_t) ud;
  TS_Status st;
  TS_Value output = {0};
  if (ts_std_argc (vm, n, 1, op == 4 ? 2 : 1, e, "tui")) return TS_ERR_INVAL;
  t = ts_std_handle (a, &tui_tag, e, "tui"); if (!t) return TS_ERR_INVAL;
  if (op == 0) return ts_value_copy (ret, &t->screen);
  if (op == 1) return screen_call (vm, "clear", &t->screen, 1, ret, e);
  if (op == 2) { t->valid = false; ts_std_ret_nil (ret); return TS_OK; }
  if (op == 5) { st = end_tui (t, e); ts_std_ret_nil (ret); return st; }
  if (op == 3)
    { st = diff (t, ret, e); if (!st) commit (t); return st; }
  ts_terminal_t *s = ts_terminal_unwrap (n == 2 ? a + 1 : &t->terminal, e);
  if (!s) return TS_ERR_INVAL;
  st = diff (t, &output, e);
  if (!st) st = ts_terminal_write_all (s, output.as.string, strlen (output.as.string), 1000, e);
  if (!st) { commit (t); ts_std_ret_bool (ret, true); }
  else t->valid = false; /* The terminal may have received a prefix. */
  ts_value_free (&output); return st;
}

static TS_Status
t_begin (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  tui_t *t;
  ts_terminal_t *s;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 2, 2, e, "begin")) return TS_ERR_INVAL;
  t = ts_std_handle (a, &tui_tag, e, "begin"); if (!t) return TS_ERR_INVAL;
  if (t->active) return ts_std_inval (e, "begin: TUI already active");
  s = ts_terminal_unwrap (a + 1, e); if (!s) return TS_ERR_INVAL;
  st = ts_terminal_raw (s, e); if (st) return st;
  st = ts_value_copy (&t->terminal, a + 1);
  if (st) { ts_tty_restore (&s->tty, NULL); return ts_std_oom (e); }
  t->active = true; t->valid = false;
  st = ts_terminal_write_all (s, enter, sizeof enter - 1, 1000, e);
  if (st) { end_tui (t, NULL); return st; }
  ts_std_ret_bool (ret, true); return TS_OK;
}

static TS_Status
t_resize (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  tui_t *t;
  int64_t rows, cols;
  ts_grid_t previous = {0};
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 3, 3, e, "resize") || ts_std_int (a + 1, &rows, e, "resize") || ts_std_int (a + 2, &cols, e, "resize")) return TS_ERR_INVAL;
  t = ts_std_handle (a, &tui_tag, e, "resize"); if (!t) return TS_ERR_INVAL;
  st = ts_grid_init (&previous, rows, cols, e); if (st) return st;
  st = ts_grid_resize (ts_screen_unwrap (&t->screen, e), rows, cols, e);
  if (st) { ts_grid_free (&previous); return st; }
  ts_grid_free (&t->previous); t->previous = previous; t->valid = false;
  ts_std_ret_nil (ret); return TS_OK;
}

static TS_Status
t_box (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  tui_t *t;
  ts_grid_t *g;
  int64_t row, col, rows, cols;
  const char *title = "";
  ts_cell_style_t style = ts_style_default ();
  size_t r, c;
  ts_cell_t cell;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 5, 7, e, "box") || ts_std_int (a + 1, &row, e, "box") ||
      ts_std_int (a + 2, &col, e, "box") || ts_std_int (a + 3, &rows, e, "box") ||
      ts_std_int (a + 4, &cols, e, "box") || (n > 5 && ts_std_str (a + 5, &title, e, "box"))) return TS_ERR_INVAL;
  t = ts_std_handle (a, &tui_tag, e, "box"); if (!t) return TS_ERR_INVAL;
  g = ts_screen_unwrap (&t->screen, e);
  if (row < 0 || col < 0 || rows < 2 || cols < 2 || rows > 500 || cols > 500 ||
      (uint64_t) row >= g->rows || (uint64_t) col >= g->cols || (uint64_t) rows > g->rows - (size_t) row || (uint64_t) cols > g->cols - (size_t) col)
    return ts_std_inval (e, "box: rectangle outside canvas");
  if (n == 7) { st = ts_style_parse (&style, a + 6, e); if (st) return st; }
  for (r = 0; r < (size_t) rows; r++)
    for (c = 0; c < (size_t) cols; c++)
      if (!r || r + 1 == (size_t) rows || !c || c + 1 == (size_t) cols)
        {
          const char *ch = !r ? (!c ? "┌" : c + 1 == (size_t) cols ? "┐" : "─") :
                           r + 1 == (size_t) rows ? (!c ? "└" : c + 1 == (size_t) cols ? "┘" : "─") : "│";
          st = ts_cell_set (&cell, ch, strlen (ch), style, e); if (st) return st;
          ts_grid_put_cell (g, (size_t) row + r, (size_t) col + c, &cell);
        }
  if (*title && cols > 4)
    {
      ts_sbuf_t clipped; ts_sbuf_init (&clipped);
      if (ts_text_clip (&clipped, title, strlen (title), (size_t) cols - 4)) { ts_sbuf_free (&clipped); return ts_std_oom (e); }
      st = ts_grid_write (g, (size_t) row, (size_t) col + 2, clipped.data ? clipped.data : "", style, e);
      ts_sbuf_free (&clipped); if (st) return st;
    }
  ts_std_ret_nil (ret); return TS_OK;
}

static const TS_FuncDef funcs[] = {
  {"new", t_new, NULL}, {"screen", t_action, NULL}, {"put", t_put, NULL}, {"put_visual", t_put, (void *) 1},
  {"clear", t_action, (void *) 1}, {"invalidate", t_action, (void *) 2},
  {"render", t_action, (void *) 3}, {"present", t_action, (void *) 4},
  {"end", t_action, (void *) 5}, {"close", t_action, (void *) 5},
  {"begin", t_begin, NULL}, {"resize", t_resize, NULL}, {"box", t_box, NULL}, {NULL, NULL, NULL}
};
const TS_Module ts_std_tui_module = {"std.tui", funcs};
