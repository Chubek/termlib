#include "css/css.h"
#include "common/html.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char css_tag;
static void
css_free (void *p)
{
  ts_css_t *c = p;
  if (!c) return;
  if (c->sheet) lxb_css_stylesheet_destroy (c->sheet, true);
  if (c->parser) lxb_css_parser_destroy (c->parser, true);
  free (c);
}

static TS_Status
c_parse (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  const char *source;
  ts_css_t *c;
  TS_Status st;
  lxb_status_t status;
  (void) ud;
  if (ts_std_argc (vm, n, 1, 1, e, "css.parse") || ts_std_str (a, &source, e, "css.parse")) return TS_ERR_INVAL;
  st = ts_utf8_validate (source, strlen (source), e); if (st) return st;
  c = calloc (1, sizeof *c); if (!c) return ts_std_oom (e);
  c->parser = lxb_css_parser_create (); c->sheet = lxb_css_stylesheet_create (NULL);
  if (!c->parser || !c->sheet) { css_free (c); return ts_std_oom (e); }
  status = lxb_css_parser_init (c->parser, NULL);
  if (!status) status = lxb_css_stylesheet_parse (c->sheet, c->parser, (const lxb_char_t *) source, strlen (source));
  if (status) { css_free (c); return ts_lexbor_error (e, status, "parse CSS"); }
  if (ts_value_make_handle (ret, c, css_free, "<stylesheet>", &css_tag)) { css_free (c); return ts_std_oom (e); }
  return TS_OK;
}

TS_Status
ts_css_attach (const TS_Value *sheet, const TS_Value *html, TS_Error *e)
{
  ts_css_t *c = ts_std_handle (sheet, &css_tag, e, "stylesheet");
  ts_html_t *h;
  lxb_status_t status;
  size_t i;
  TS_Status st;
  if (!c) return TS_ERR_INVAL;
  h = ts_html_owner (html, e); if (!h) return TS_ERR_INVAL;
  for (i = 0; i < ts_std_list_len (h->sheets); i++)
    if (ts_value_handle (ts_std_list_get (h->sheets, i), &css_tag) == c) return TS_OK;
  if (ts_std_list_len (h->sheets) >= 64) { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "document stylesheet limit is 64"); return TS_ERR_LIMIT; }
  st = ts_std_list_push (h->sheets, sheet); if (st) return ts_std_oom (e);
  status = lxb_html_document_stylesheet_attach (h->document, c->sheet);
  /* Retain even on failure: a partial attachment may reference the sheet. */
  return status ? ts_lexbor_error (e, status, "attach CSS") : TS_OK;
}

static TS_Status
c_apply (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 2, 2, e, "apply")) return TS_ERR_INVAL;
  st = ts_css_attach (a, a + 1, e);
  if (!st) ts_std_ret_bool (ret, true);
  return st;
}

static bool
inherited (const char *name)
{
  return !strcmp (name, "color") || !strncmp (name, "font-", 5) || !strcmp (name, "direction") ||
         !strcmp (name, "text-align") || !strcmp (name, "white-space") || !strcmp (name, "visibility") || !strncmp (name, "--", 2);
}

const lxb_css_rule_declaration_t *
ts_css_property (lxb_dom_node_t *node, const char *name, bool inherit)
{
  while (node)
    {
      if (node->type == LXB_DOM_NODE_TYPE_ELEMENT)
        {
          const lxb_css_rule_declaration_t *d = lxb_dom_element_style_by_name (lxb_dom_interface_element (node), (const lxb_char_t *) name, strlen (name));
          if (d)
            {
              ts_sbuf_t b; bool parent;
              ts_sbuf_init (&b);
              if (lxb_css_property_serialize (d->u.user, d->type, ts_lexbor_write, &b)) { ts_sbuf_free (&b); return d; }
              parent = b.data && (!strcmp (b.data, "inherit") || (!strcmp (b.data, "unset") && inherited (name)));
              if (b.data && (!strcmp (b.data, "initial") || (!strcmp (b.data, "unset") && !inherited (name))))
                { ts_sbuf_free (&b); return NULL; }
              ts_sbuf_free (&b);
              if (!parent) return d;
              inherit = true;
            }
          else if (!inherit || !inherited (name)) return NULL;
        }
      node = node->parent;
    }
  return NULL;
}

static int
channel (lxb_css_value_number_percentage_t v)
{
  double n = v.type == LXB_CSS_VALUE__PERCENTAGE ? v.u.percentage.num * 2.55 : v.u.number.num;
  return (int) lround (n < 0 ? 0 : n > 255 ? 255 : n);
}

static bool
css_color (const lxb_css_value_color_t *c, int32_t *out)
{
  static const struct { const char *name; int rgb; } names[] = {
#include "css_named_colors.h"
  };
  int r, g, b;
  size_t i;
  if (c->type == LXB_CSS_VALUE_TRANSPARENT) { *out = -1; return true; }
  if (c->type == LXB_CSS_VALUE_HEX)
    {
      if (!c->u.hex.rgba.a) { *out = -1; return true; }
      if (c->u.hex.rgba.a != 255) return false;
      r = c->u.hex.rgba.r; g = c->u.hex.rgba.g; b = c->u.hex.rgba.b;
    }
  else if (c->type == LXB_CSS_VALUE_RGB || c->type == LXB_CSS_VALUE_RGBA)
    {
      const lxb_css_value_number_percentage_t *alpha = &c->u.rgb.a;
      if (alpha->type == LXB_CSS_VALUE__NUMBER && alpha->u.number.num != 1) return false;
      if (alpha->type == LXB_CSS_VALUE__PERCENTAGE && alpha->u.percentage.num != 100) return false;
      r = channel (c->u.rgb.r); g = channel (c->u.rgb.g); b = channel (c->u.rgb.b);
    }
  else if (c->type == LXB_CSS_VALUE_HSL || c->type == LXB_CSS_VALUE_HSLA)
    {
      double hue, sat = c->u.hsl.s.percentage.num / 100, light = c->u.hsl.l.percentage.num / 100;
      double chroma, x, m, rr = 0, gg = 0, bb = 0;
      const lxb_css_value_number_percentage_t *alpha = &c->u.hsl.a;
      if ((alpha->type == LXB_CSS_VALUE__NUMBER && alpha->u.number.num != 1) ||
          (alpha->type == LXB_CSS_VALUE__PERCENTAGE && alpha->u.percentage.num != 100)) return false;
      hue = c->u.hsl.h.type == LXB_CSS_VALUE__ANGLE ? c->u.hsl.h.u.angle.num : c->u.hsl.h.u.number.num;
      if (c->u.hsl.h.type == LXB_CSS_VALUE__ANGLE)
        {
          if (c->u.hsl.h.u.angle.unit == LXB_CSS_UNIT_RAD) hue *= 180 / 3.141592653589793;
          else if (c->u.hsl.h.u.angle.unit == LXB_CSS_UNIT_TURN) hue *= 360;
          else if (c->u.hsl.h.u.angle.unit == LXB_CSS_UNIT_GRAD) hue *= 0.9;
        }
      hue = fmod (hue, 360); if (hue < 0) hue += 360;
      sat = fmax (0, fmin (1, sat)); light = fmax (0, fmin (1, light));
      chroma = (1 - fabs (2 * light - 1)) * sat; x = chroma * (1 - fabs (fmod (hue / 60, 2) - 1)); m = light - chroma / 2;
      if (hue < 60) { rr = chroma; gg = x; } else if (hue < 120) { rr = x; gg = chroma; }
      else if (hue < 180) { gg = chroma; bb = x; } else if (hue < 240) { gg = x; bb = chroma; }
      else if (hue < 300) { rr = x; bb = chroma; } else { rr = chroma; bb = x; }
      r = (int) lround ((rr + m) * 255); g = (int) lround ((gg + m) * 255); b = (int) lround ((bb + m) * 255);
    }
  else
    {
      const lxb_css_data_t *data = lxb_css_value_by_id (c->type);
      if (!data) return false;
      for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strlen (names[i].name) == data->length && !memcmp (names[i].name, data->name, data->length))
          { *out = TS_COLOR_RGB | names[i].rgb; return true; }
      return false;
    }
  *out = TS_COLOR_RGB | (r << 16) | (g << 8) | b;
  return true;
}

TS_Status
ts_css_cell_style (lxb_dom_node_t *node, ts_cell_style_t *style, TS_Error *e)
{
  const lxb_css_rule_declaration_t *d;
  ts_sbuf_t b;
  size_t i;
  static const char *props[] = {"font-weight", "font-style", "text-decoration-line", "visibility"};
  *style = ts_style_default ();
  d = ts_css_property (node, "color", true);
  if (d && d->u.color->type != LXB_CSS_VALUE_CURRENTCOLOR && !css_color (d->u.color, &style->fg))
    { ts_error_set (e, TS_ERR_UNSUPPORTED, 0, 0, "terminal CSS: unsupported color space or alpha"); return TS_ERR_UNSUPPORTED; }
  d = ts_css_property (node, "background-color", false);
  if (d)
    {
      if (d->u.background_color->type == LXB_CSS_VALUE_CURRENTCOLOR) style->bg = style->fg;
      else if (!css_color (d->u.background_color, &style->bg))
        { ts_error_set (e, TS_ERR_UNSUPPORTED, 0, 0, "terminal CSS: unsupported background color space or alpha"); return TS_ERR_UNSUPPORTED; }
    }
  for (i = 0; i < sizeof props / sizeof props[0]; i++)
    {
      d = ts_css_property (node, props[i], true);
      if (!d) continue;
      ts_sbuf_init (&b);
      lxb_status_t status = lxb_css_property_serialize (d->u.user, d->type, ts_lexbor_write, &b);
      if (status) { ts_sbuf_free (&b); return ts_lexbor_error (e, status, "CSS property"); }
      if (i == 0 && (!strcmp (b.data, "bold") || !strcmp (b.data, "bolder") || strtol (b.data, NULL, 10) >= 600)) style->attrs |= TS_ATTR_BOLD;
      if (i == 1 && (!strcmp (b.data, "italic") || !strncmp (b.data, "oblique", 7))) style->attrs |= TS_ATTR_ITALIC;
      if (i == 2) { if (strstr (b.data, "underline")) style->attrs |= TS_ATTR_UNDERLINE; if (strstr (b.data, "line-through")) style->attrs |= TS_ATTR_STRIKE; if (strstr (b.data, "overline")) style->attrs |= TS_ATTR_OVERLINE; }
      if (i == 3 && (!strcmp (b.data, "hidden") || !strcmp (b.data, "collapse"))) style->attrs |= TS_ATTR_HIDDEN;
      ts_sbuf_free (&b);
    }
  return TS_OK;
}

static TS_Status
c_get (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  lxb_dom_node_t *node;
  const char *name;
  const lxb_css_rule_declaration_t *d;
  ts_sbuf_t b;
  ts_cell_style_t style;
  ts_cell_t cell;
  TS_Status st;
  lxb_status_t status;
  intptr_t op = (intptr_t) ud;
  if (ts_std_argc (vm, n, op ? 1 : 2, op ? 1 : 2, e, "css")) return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  if (op == 3)
    {
      ts_css_t *c = ts_std_handle (a, &css_tag, e, "serialize"); if (!c) return TS_ERR_INVAL;
      status = c->sheet->root ? lxb_css_rule_serialize (c->sheet->root, ts_lexbor_write, &b) : LXB_STATUS_OK;
    }
  else
    {
      node = ts_html_node (a, e); if (!node) return TS_ERR_INVAL;
      if (!op)
        {
          if (ts_std_str (a + 1, &name, e, "get")) return TS_ERR_INVAL;
          d = ts_css_property (node, name, true);
          if (!d) { ts_std_ret_nil (ret); return TS_OK; }
          status = lxb_css_property_serialize (d->u.user, d->type, ts_lexbor_write, &b);
        }
      else
        {
          st = ts_css_cell_style (node, &style, e); if (st) return st;
          if (op == 1) { ts_cell_blank (&cell, style); return ts_cell_wrap (ret, &cell, e); }
          if (ts_style_ansi (&b, style)) { ts_sbuf_free (&b); return ts_std_oom (e); }
          status = LXB_STATUS_OK;
        }
    }
  if (status) { ts_sbuf_free (&b); return ts_lexbor_error (e, status, "serialize CSS"); }
  return ts_std_ret_sbuf (ret, &b, e);
}

static const TS_FuncDef funcs[] = {
  {"parse", c_parse, NULL}, {"apply", c_apply, NULL}, {"get", c_get, NULL},
  {"style", c_get, (void *) 1}, {"ansi", c_get, (void *) 2}, {"serialize", c_get, (void *) 3},
  {NULL, NULL, NULL}
};
const TS_Module ts_std_css_module = {"std.css", funcs};
const TS_Module ts_std_stylesheet_module = {"std.stylesheet", funcs};
