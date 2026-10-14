#include "cell/cell.h"
#include "common/grid.h"
#include <stdlib.h>
#include <string.h>

static const char cell_tag;

const ts_cell_t *
ts_cell_unwrap (const TS_Value *v, TS_Error *e)
{
  return ts_std_handle (v, &cell_tag, e, "cell");
}

TS_Status
ts_cell_wrap (TS_Value *ret, const ts_cell_t *c, TS_Error *e)
{
  ts_cell_t *copy = malloc (sizeof *copy);
  if (!copy) return ts_std_oom (e);
  *copy = *c;
  if (ts_value_make_handle (ret, copy, free, "<cell>", &cell_tag))
    { free (copy); return ts_std_oom (e); }
  return TS_OK;
}

static TS_Status
c_new (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  const char *s = " ";
  ts_cell_t c;
  ts_cell_style_t style = ts_style_default ();
  int64_t attrs = 0;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 0, 4, e, "cell.new") ||
      (n && ts_std_str (a, &s, e, "cell.new"))) return TS_ERR_INVAL;
  if (n > 1 && ts_style_color (a + 1, &style.fg, e)) return TS_ERR_INVAL;
  if (n > 2 && ts_style_color (a + 2, &style.bg, e)) return TS_ERR_INVAL;
  if (n > 3 && ts_std_int (a + 3, &attrs, e, "cell.new")) return TS_ERR_INVAL;
  if (attrs < 0 || attrs > 511) return ts_std_inval (e, "cell: invalid attribute mask");
  style.attrs = (uint32_t) attrs;
  st = ts_cell_set (&c, s, strlen (s), style, e);
  return st ? st : ts_cell_wrap (ret, &c, e);
}

static TS_Status
c_get (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  const ts_cell_t *c;
  ts_sbuf_t b;
  if (ts_std_argc (vm, n, 1, 1, e, "cell")) return TS_ERR_INVAL;
  c = ts_cell_unwrap (a, e);
  if (!c) return TS_ERR_INVAL;
  switch ((intptr_t) ud)
    {
    case 0: return ts_std_ret_str (ret, c->text, e);
    case 1: ts_std_ret_int (ret, c->width); break;
    case 2: ts_std_ret_int (ret, c->style.fg); break;
    case 3: ts_std_ret_int (ret, c->style.bg); break;
    case 4: ts_std_ret_int (ret, c->style.attrs); break;
    case 5:
      ts_sbuf_init (&b);
      if (ts_style_ansi (&b, c->style) || ts_sbuf_str (&b, c->text) || ts_sbuf_str (&b, "\033[0m"))
        { ts_sbuf_free (&b); return ts_std_oom (e); }
      return ts_std_ret_sbuf (ret, &b, e);
    default: return ts_cell_wrap (ret, c, e);
    }
  return TS_OK;
}

static TS_Status
c_style (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  const ts_cell_t *c;
  ts_cell_t next;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 2, 2, e, "with_style")) return TS_ERR_INVAL;
  c = ts_cell_unwrap (a, e);
  if (!c) return TS_ERR_INVAL;
  next = *c;
  st = ts_style_parse (&next.style, a + 1, e);
  return st ? st : ts_cell_wrap (ret, &next, e);
}

static const TS_FuncDef funcs[] = {
  {"new", c_new, NULL}, {"text", c_get, NULL}, {"width", c_get, (void *) 1},
  {"foreground", c_get, (void *) 2}, {"background", c_get, (void *) 3},
  {"attributes", c_get, (void *) 4}, {"ansi", c_get, (void *) 5},
  {"clone", c_get, (void *) 6}, {"with_style", c_style, NULL},
};
const TS_Module ts_std_cell_module = TS_MODULE_INIT ("std.cell", funcs);
