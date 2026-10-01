#include "common/grid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

ts_cell_style_t
ts_style_default (void)
{
  ts_cell_style_t s = {-1, -1, 0};
  return s;
}

bool
ts_style_equal (ts_cell_style_t a, ts_cell_style_t b)
{
  return a.fg == b.fg && a.bg == b.bg && a.attrs == b.attrs;
}

void
ts_cell_blank (ts_cell_t *c, ts_cell_style_t s)
{
  memset (c, 0, sizeof *c);
  c->style = s;
  c->width = 1;
  strcpy (c->text, " ");
}

void
ts_style_sgr (ts_cell_style_t *s, const int *p, size_t n)
{
  size_t i;
  if (!n) *s = ts_style_default ();
  for (i = 0; i < n; i++)
    {
      int c = p[i] < 0 ? 0 : p[i];
      switch (c)
        {
        case 0: *s = ts_style_default (); break;
        case 1: s->attrs |= TS_ATTR_BOLD; break;
        case 2: s->attrs |= TS_ATTR_DIM; break;
        case 3: s->attrs |= TS_ATTR_ITALIC; break;
        case 4: s->attrs |= TS_ATTR_UNDERLINE; break;
        case 5: case 6: s->attrs |= TS_ATTR_BLINK; break;
        case 7: s->attrs |= TS_ATTR_REVERSE; break;
        case 8: s->attrs |= TS_ATTR_HIDDEN; break;
        case 9: s->attrs |= TS_ATTR_STRIKE; break;
        case 22: s->attrs &= ~(TS_ATTR_BOLD | TS_ATTR_DIM); break;
        case 23: s->attrs &= ~TS_ATTR_ITALIC; break;
        case 24: s->attrs &= ~TS_ATTR_UNDERLINE; break;
        case 25: s->attrs &= ~TS_ATTR_BLINK; break;
        case 27: s->attrs &= ~TS_ATTR_REVERSE; break;
        case 28: s->attrs &= ~TS_ATTR_HIDDEN; break;
        case 29: s->attrs &= ~TS_ATTR_STRIKE; break;
        case 39: s->fg = -1; break;
        case 49: s->bg = -1; break;
        case 53: s->attrs |= TS_ATTR_OVERLINE; break;
        case 55: s->attrs &= ~TS_ATTR_OVERLINE; break;
        case 38: case 48:
          {
            int32_t color = -1;
            bool valid = false;
            if (i + 2 < n && p[i + 1] == 5)
              {
                color = p[i + 2];
                valid = color >= 0 && color <= 255;
                i += 2;
              }
            else if (i + 4 < n && p[i + 1] == 2)
              {
                size_t base = i + 2;
                /* ISO colon form may include an empty color-space field. */
                if (p[base] < 0 && i + 5 < n) base++;
                if (base + 2 < n)
                  {
                    int r = p[base], g = p[base + 1], b = p[base + 2];
                    valid = r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255;
                    if (valid) color = TS_COLOR_RGB | (r << 16) | (g << 8) | b;
                    i = base + 2;
                  }
              }
            else if (i + 1 < n) i = n - 1;
            if (valid) { if (c == 38) s->fg = color; else s->bg = color; }
            break;
          }
        default:
          if (c >= 30 && c <= 37) s->fg = c - 30;
          else if (c >= 40 && c <= 47) s->bg = c - 40;
          else if (c >= 90 && c <= 97) s->fg = c - 90 + 8;
          else if (c >= 100 && c <= 107) s->bg = c - 100 + 8;
          break;
        }
    }
}

static int
ansi_color (ts_sbuf_t *b, int32_t color, bool bg)
{
  if (color < 0) return 0;
  if (color & TS_COLOR_RGB)
    return ts_sbuf_printf (b, ";%d;2;%d;%d;%d", bg ? 48 : 38,
                           (color >> 16) & 255, (color >> 8) & 255, color & 255);
  if (color < 16)
    return ts_sbuf_printf (b, ";%d", (bg ? 40 : 30) + (color & 7) + (color >= 8 ? 60 : 0));
  return ts_sbuf_printf (b, ";%d;5;%d", bg ? 48 : 38, color);
}

int
ts_style_ansi (ts_sbuf_t *b, ts_cell_style_t s)
{
  static const int codes[] = {1, 2, 3, 4, 5, 7, 8, 9, 53};
  size_t i;
  if (ts_sbuf_str (b, "\033[0")) return -1;
  for (i = 0; i < sizeof codes / sizeof codes[0]; i++)
    if ((s.attrs & (1u << i)) && ts_sbuf_printf (b, ";%d", codes[i])) return -1;
  if (ansi_color (b, s.fg, false) || ansi_color (b, s.bg, true)) return -1;
  return ts_sbuf_ch (b, 'm');
}

TS_Status
ts_style_color (const TS_Value *v, int32_t *out, TS_Error *e)
{
  static const char *names[] = {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};
  size_t i;
  const char *s;
  bool bright = false;
  if (v->type == TS_NIL) { *out = -1; return TS_OK; }
  if (v->type == TS_INT)
    {
      int64_t n = v->as.integer;
      if (n == -1 || (n >= 0 && n <= 255) || (n >= TS_COLOR_RGB && n <= 0x1ffffff))
        { *out = (int32_t) n; return TS_OK; }
      return ts_std_inval (e, "color: expected -1, palette 0..255, or tagged RGB");
    }
  if (ts_std_str (v, &s, e, "color")) return TS_ERR_INVAL;
  if (!strcasecmp (s, "default")) { *out = -1; return TS_OK; }
  if (s[0] == '#' && (strlen (s) == 7 || strlen (s) == 4))
    {
      unsigned value = 0;
      for (i = 1; s[i]; i++)
        {
          char c = s[i];
          int digit = c >= '0' && c <= '9' ? c - '0' :
                      c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                      c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
          if (digit < 0) return ts_std_inval (e, "color: invalid hex color");
          value = value * 16 + (unsigned) digit;
        }
      if (strlen (s) == 4)
        value = ((value & 0xf00) << 12) | ((value & 0xf00) << 8) |
                ((value & 0xf0) << 8) | ((value & 0xf0) << 4) |
                ((value & 0xf) << 4) | (value & 0xf);
      *out = TS_COLOR_RGB | value;
      return TS_OK;
    }
  if (!strncasecmp (s, "bright-", 7)) { bright = true; s += 7; }
  for (i = 0; i < 8; i++)
    if (!strcasecmp (s, names[i])) { *out = (int32_t) i + (bright ? 8 : 0); return TS_OK; }
  return ts_std_inval (e, "color: unknown color");
}

TS_Status
ts_style_parse (ts_cell_style_t *out, const TS_Value *v, TS_Error *e)
{
  const char *s;
  if (v->type == TS_HANDLE)
    {
      const ts_cell_t *c = ts_cell_unwrap (v, e);
      if (!c) return TS_ERR_INVAL;
      *out = c->style;
      return TS_OK;
    }
  if (ts_std_str (v, &s, e, "style")) return TS_ERR_INVAL;
  while (*s)
    {
      int p[32], n = 0, value = -1;
      if (s[0] != '\033' || s[1] != '[') return ts_std_inval (e, "style: SGR string or cell required");
      s += 2;
      while (*s)
        {
          if (*s >= '0' && *s <= '9')
            {
              if (value > 1000000) return ts_std_inval (e, "style: SGR parameter too large");
              value = (value < 0 ? 0 : value) * 10 + (*s - '0');
            }
          else if (*s == ';' || *s == ':' || *s == 'm')
            {
              if (n == 32) return ts_std_inval (e, "style: too many SGR parameters");
              p[n++] = value;
              value = -1;
              if (*s == 'm') break;
            }
          else return ts_std_inval (e, "style: malformed SGR");
          s++;
        }
      if (*s != 'm') return ts_std_inval (e, "style: incomplete SGR");
      ts_style_sgr (out, p, (size_t) n);
      s++;
    }
  return TS_OK;
}

TS_Status
ts_cell_set (ts_cell_t *c, const char *s, size_t n, ts_cell_style_t style, TS_Error *e)
{
  size_t bytes;
  int width;
  TS_Status st = ts_utf8_validate (s, n, e);
  if (st) return st;
  if (n >= TS_CELL_BYTES)
    { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "cell grapheme exceeds 127 bytes"); return TS_ERR_LIMIT; }
  if (!n || ts_text_cluster (s, n, &bytes, &width) != 1 || bytes != n || width < 1 || width > 2)
    return ts_std_inval (e, "cell: one printable grapheme required");
  memset (c, 0, sizeof *c);
  memcpy (c->text, s, n);
  c->width = (unsigned char) width;
  c->style = style;
  return TS_OK;
}

TS_Status
ts_grid_init (ts_grid_t *g, int64_t rows, int64_t cols, TS_Error *e)
{
  if (rows < 1 || cols < 1 || rows > 500 || cols > 500)
    return ts_std_inval (e, "grid: dimensions must be 1..500");
  g->cells = calloc ((size_t) rows * (size_t) cols, sizeof *g->cells);
  if (!g->cells) return ts_std_oom (e);
  g->rows = (size_t) rows;
  g->cols = (size_t) cols;
  ts_grid_clear (g, ts_style_default ());
  return TS_OK;
}

void
ts_grid_free (ts_grid_t *g)
{
  if (!g) return;
  free (g->cells);
  memset (g, 0, sizeof *g);
}

void
ts_grid_clear (ts_grid_t *g, ts_cell_style_t s)
{
  size_t i;
  for (i = 0; i < g->rows * g->cols; i++) ts_cell_blank (&g->cells[i], s);
}

void
ts_grid_erase (ts_grid_t *g, size_t row, size_t col, size_t count, ts_cell_style_t s)
{
  size_t end;
  if (row >= g->rows || col >= g->cols || !count) return;
  end = count > g->cols - col ? g->cols : col + count;
  if (!g->cells[row * g->cols + col].width && col) col--;
  if (end < g->cols && !g->cells[row * g->cols + end].width) end++;
  while (col < end) ts_cell_blank (&g->cells[row * g->cols + col++], s);
}

void
ts_grid_put_cell (ts_grid_t *g, size_t row, size_t col, const ts_cell_t *c)
{
  ts_cell_t copy = *c;
  if (row >= g->rows || col >= g->cols || !copy.width || copy.width > g->cols - col) return;
  ts_grid_erase (g, row, col, copy.width, ts_style_default ());
  g->cells[row * g->cols + col] = copy;
  if (copy.width == 2)
    {
      ts_cell_t *tail = &g->cells[row * g->cols + col + 1];
      memset (tail, 0, sizeof *tail);
      tail->style = copy.style;
    }
}

TS_Status
ts_grid_resize (ts_grid_t *g, int64_t rows, int64_t cols, TS_Error *e)
{
  ts_grid_t next = {0};
  size_t r, c;
  TS_Status st = ts_grid_init (&next, rows, cols, e);
  if (st) return st;
  for (r = 0; r < g->rows && r < next.rows; r++)
    for (c = 0; c < g->cols && c < next.cols; c++)
      ts_grid_put_cell (&next, r, c, &g->cells[r * g->cols + c]);
  ts_grid_free (g);
  *g = next;
  return TS_OK;
}

TS_Status
ts_grid_write (ts_grid_t *g, size_t row, size_t col, const char *text,
               ts_cell_style_t style, TS_Error *e)
{
  size_t len = strlen (text), at = 0, bytes, initial = col;
  int width;
  TS_Status st = ts_utf8_validate (text, len, e);
  if (st) return st;
  /* Preflight the grapheme bound before modifying the grid. */
  while (at < len)
    {
      size_t esc = ts_text_escape (text + at, len - at);
      if (esc) { at += esc; continue; }
      ts_text_cluster (text + at, len - at, &bytes, &width);
      if (bytes >= TS_CELL_BYTES)
        { ts_error_set (e, TS_ERR_LIMIT, 0, at, "cell grapheme exceeds 127 bytes"); return TS_ERR_LIMIT; }
      at += bytes;
    }
  at = 0;
  while (at < len && row < g->rows)
    {
      size_t esc = ts_text_escape (text + at, len - at);
      if (esc)
        {
          if (esc >= 3 && text[at + 1] == '[' && text[at + esc - 1] == 'm')
            {
              TS_Value seq = {0};
              st = ts_std_ret_strn (&seq, text + at, esc, e);
              if (!st) st = ts_style_parse (&style, &seq, e);
              ts_value_free (&seq);
              if (st) return st;
            }
          at += esc;
          continue;
        }
      if (text[at] == '\n') { row++; col = initial; at++; continue; }
      if (text[at] == '\r') { col = initial; at++; continue; }
      if (text[at] == '\t') { col = (col + 8) & ~(size_t) 7; at++; continue; }
      ts_text_cluster (text + at, len - at, &bytes, &width);
      if (width > 0 && col < g->cols && (size_t) width <= g->cols - col)
        {
          ts_cell_t cell;
          st = ts_cell_set (&cell, text + at, bytes, style, e);
          if (st) return st;
          ts_grid_put_cell (g, row, col, &cell);
        }
      col += (size_t) width;
      at += bytes;
    }
  return TS_OK;
}

int
ts_grid_line (ts_sbuf_t *b, const ts_grid_t *g, size_t row, bool ansi, bool trim)
{
  size_t c, end = g->cols;
  ts_cell_style_t last = ts_style_default ();
  bool emitted = false;
  if (trim)
    while (end > 0)
      {
        const ts_cell_t *cell = &g->cells[row * g->cols + end - 1];
        if (cell->width != 1 || strcmp (cell->text, " ") ||
            (ansi && !ts_style_equal (cell->style, ts_style_default ()))) break;
        end--;
      }
  for (c = 0; c < end; c++)
    {
      const ts_cell_t *cell = &g->cells[row * g->cols + c];
      if (!cell->width) continue;
      if (ansi && (!emitted || !ts_style_equal (last, cell->style)))
        {
          if (ts_style_ansi (b, cell->style)) return -1;
          last = cell->style;
          emitted = true;
        }
      if (ts_sbuf_str (b, cell->text)) return -1;
    }
  return ansi && emitted ? ts_sbuf_str (b, "\033[0m") : 0;
}

TS_Status
ts_grid_render (TS_Value *ret, const ts_grid_t *g, bool ansi, TS_Error *e)
{
  ts_sbuf_t b;
  size_t row;
  ts_sbuf_init (&b);
  for (row = 0; row < g->rows; row++)
    if (ts_grid_line (&b, g, row, ansi, true) ||
        (ansi ? (row + 1 < g->rows && ts_sbuf_str (&b, "\r\n")) : ts_sbuf_ch (&b, '\n')))
      { ts_sbuf_free (&b); return ts_std_oom (e); }
  return ts_std_ret_sbuf (ret, &b, e);
}
