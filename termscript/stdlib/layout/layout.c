#include "layout/layout.h"
#include "common/text.h"
#include <stdlib.h>
#include <string.h>

typedef struct { int64_t row, col, rows, cols; } rect_t;
static const char rect_tag;
static TS_Status
wrap_rect (TS_Value *ret, rect_t rect, TS_Error *e)
{
  rect_t *p = malloc (sizeof *p); if (!p) return ts_std_oom (e); *p = rect;
  if (ts_value_make_handle (ret, p, free, "<rectangle>", &rect_tag)) { free (p); return ts_std_oom (e); }
  return TS_OK;
}

static TS_Status
l_rect (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  rect_t r;
  (void) ud;
  if (ts_std_argc (vm, n, 4, 4, e, "rect") || ts_std_int (a, &r.row, e, "rect") ||
      ts_std_int (a + 1, &r.col, e, "rect") || ts_std_int (a + 2, &r.rows, e, "rect") ||
      ts_std_int (a + 3, &r.cols, e, "rect")) return TS_ERR_INVAL;
  if (r.row < 0 || r.col < 0 || r.rows < 0 || r.cols < 0 || r.row > 10000 || r.col > 10000 || r.rows > 10000 || r.cols > 10000)
    return ts_std_inval (e, "rect: coordinates/dimensions must be 0..10000");
  return wrap_rect (ret, r, e);
}

static TS_Status
l_get (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  rect_t *r;
  const char *property;
  int64_t row, col;
  if (ts_std_argc (vm, n, ud ? 3 : 2, ud ? 3 : 2, e, "rect")) return TS_ERR_INVAL;
  r = ts_std_handle (a, &rect_tag, e, "rect"); if (!r) return TS_ERR_INVAL;
  if (ud)
    {
      if (ts_std_int (a + 1, &row, e, "contains") || ts_std_int (a + 2, &col, e, "contains")) return TS_ERR_INVAL;
      ts_std_ret_bool (ret, row >= r->row && col >= r->col && row - r->row < r->rows && col - r->col < r->cols); return TS_OK;
    }
  if (ts_std_str (a + 1, &property, e, "get")) return TS_ERR_INVAL;
  if (!strcmp (property, "row")) row = r->row;
  else if (!strcmp (property, "col")) row = r->col;
  else if (!strcmp (property, "rows")) row = r->rows;
  else if (!strcmp (property, "cols")) row = r->cols;
  else { ts_std_ret_nil (ret); return TS_OK; }
  ts_std_ret_int (ret, row); return TS_OK;
}

static TS_Status
l_inset (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  rect_t *r, next;
  int64_t pad;
  (void) ud;
  if (ts_std_argc (vm, n, 2, 2, e, "inset") || ts_std_int (a + 1, &pad, e, "inset")) return TS_ERR_INVAL;
  r = ts_std_handle (a, &rect_tag, e, "inset"); if (!r) return TS_ERR_INVAL;
  if (pad < 0 || pad > r->rows / 2 || pad > r->cols / 2) return ts_std_inval (e, "inset: padding exceeds rectangle");
  next = *r; next.row += pad; next.col += pad; next.rows -= pad * 2; next.cols -= pad * 2;
  return wrap_rect (ret, next, e);
}

static TS_Status
l_split (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  rect_t *r, next;
  const char *direction;
  int64_t count, gap = 0, available, offset = 0, i;
  bool horizontal;
  ts_std_list_t *l;
  TS_Value part = {0};
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 3, 4, e, "split") || ts_std_str (a + 1, &direction, e, "split") ||
      ts_std_int (a + 2, &count, e, "split") || (n == 4 && ts_std_int (a + 3, &gap, e, "split"))) return TS_ERR_INVAL;
  r = ts_std_handle (a, &rect_tag, e, "split"); if (!r) return TS_ERR_INVAL;
  horizontal = !strcmp (direction, "horizontal");
  if ((!horizontal && strcmp (direction, "vertical")) || count < 1 || count > 500 || gap < 0 || gap > 10000)
    return ts_std_inval (e, "split: invalid direction/count/gap");
  available = (horizontal ? r->cols : r->rows) - (count - 1) * gap;
  if (available < count) return ts_std_inval (e, "split: insufficient space");
  l = ts_std_list_new (); if (!l) return ts_std_oom (e);
  for (i = 0; i < count; i++)
    {
      int64_t size = available / count + (i < available % count ? 1 : 0);
      next = *r;
      if (horizontal) { next.col += offset; next.cols = size; } else { next.row += offset; next.rows = size; }
      st = wrap_rect (&part, next, e); if (!st) st = ts_std_list_push (l, &part); ts_value_free (&part);
      if (st) { ts_std_list_free (l); return st; }
      offset += size + gap;
    }
  return ts_std_list_wrap (ret, l, e);
}

static TS_Status
l_align (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  const char *s, *align = "left";
  int64_t width;
  ts_sbuf_t clipped, b;
  size_t len, visible, left = 0, i;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 2, 3, e, "align") || ts_std_str (a, &s, e, "align") ||
      ts_std_int (a + 1, &width, e, "align") || (n == 3 && ts_std_str (a + 2, &align, e, "align"))) return TS_ERR_INVAL;
  if (width < 0 || width > 10000 || (strcmp (align, "left") && strcmp (align, "right") && strcmp (align, "center"))) return ts_std_inval (e, "align: invalid width/alignment");
  len = strlen (s); st = ts_utf8_validate (s, len, e); if (st) return st;
  ts_sbuf_init (&clipped); ts_sbuf_init (&b);
  if (ts_text_clip (&clipped, s, len, (size_t) width)) goto oom;
  visible = ts_text_width (clipped.data ? clipped.data : "", clipped.len);
  if (!strcmp (align, "right")) left = (size_t) width - visible;
  else if (!strcmp (align, "center")) left = ((size_t) width - visible) / 2;
  for (i = 0; i < left; i++) if (ts_sbuf_ch (&b, ' ')) goto oom;
  if (ts_sbuf_put (&b, clipped.data, clipped.len)) goto oom;
  if (memchr (s, 27, len) && ts_sbuf_str (&b, "\033[0m")) goto oom;
  for (i = left + visible; i < (size_t) width; i++) if (ts_sbuf_ch (&b, ' ')) goto oom;
  ts_sbuf_free (&clipped); return ts_std_ret_sbuf (ret, &b, e);
oom: ts_sbuf_free (&clipped); ts_sbuf_free (&b); return ts_std_oom (e);
}

static const TS_FuncDef funcs[] = {
  {"rect", l_rect, NULL}, {"get", l_get, NULL}, {"contains", l_get, (void *) 1},
  {"inset", l_inset, NULL}, {"split", l_split, NULL}, {"align", l_align, NULL}, {"fit", l_align, NULL},
};
const TS_Module ts_std_layout_module = TS_MODULE_INIT ("std.layout", funcs);
