#include "html/html.h"
#include "common/html.h"
#include <stdlib.h>
#include <string.h>

static TS_Status
h_parse (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  const char *s;
  (void) ud;
  if (ts_std_argc (vm, n, 1, 1, e, "html.parse") || ts_std_str (a, &s, e, "html.parse")) return TS_ERR_INVAL;
  return ts_html_parse (ret, s, e);
}

static TS_Status
h_query (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  const char *selector;
  if (ts_std_argc (vm, n, 2, 2, e, "query") || ts_std_str (a + 1, &selector, e, "query")) return TS_ERR_INVAL;
  return ts_html_select (a, selector, ud != NULL, ret, e);
}

static TS_Status
h_get (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  lxb_dom_node_t *node;
  intptr_t op = (intptr_t) ud;
  size_t len = 0;
  lxb_char_t *text;
  const lxb_char_t *name;
  TS_Status st;
  ts_sbuf_t b;
  if (ts_std_argc (vm, n, 1, 1, e, "html")) return TS_ERR_INVAL;
  node = ts_html_node (a, e); if (!node) return TS_ERR_INVAL;
  if (op == 0)
    {
      text = lxb_dom_node_text_content (node, &len);
      if (!text) return ts_std_ret_str (ret, "", e);
      st = ts_std_ret_strn (ret, (char *) text, len, e);
      lxb_dom_document_destroy_text (node->owner_document, text);
      return st;
    }
  if (op == 1)
    {
      lxb_status_t status;
      ts_sbuf_init (&b);
      status = node->type == LXB_DOM_NODE_TYPE_DOCUMENT ?
        lxb_html_serialize_tree_cb (node, ts_lexbor_write, &b) : lxb_html_serialize_deep_cb (node, ts_lexbor_write, &b);
      if (status) { ts_sbuf_free (&b); return ts_lexbor_error (e, status, "serialize HTML"); }
      return ts_std_ret_sbuf (ret, &b, e);
    }
  if (op == 2)
    {
      if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return ts_std_ret_str (ret, "", e);
      name = lxb_dom_element_local_name (lxb_dom_interface_element (node), &len);
      return ts_std_ret_strn (ret, (const char *) name, len, e);
    }
  if (op == 3) return ts_html_wrap_node (ret, a, node->parent, e);
  if (op == 4)
    {
      ts_std_list_t *l = ts_std_list_new ();
      lxb_dom_node_t *child;
      TS_Value v = {0};
      if (!l) return ts_std_oom (e);
      for (child = node->first_child; child; child = child->next)
        {
          st = ts_html_wrap_node (&v, a, child, e);
          if (!st) st = ts_std_list_push (l, &v);
          ts_value_free (&v);
          if (st) { ts_std_list_free (l); return st; }
        }
      return ts_std_list_wrap (ret, l, e);
    }
  ts_html_t *h = ts_html_owner (a, e);
  return ts_html_wrap_node (ret, a, lxb_dom_interface_node (lxb_html_document_body_element (h->document)), e);
}

static TS_Status
h_attr (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  lxb_dom_node_t *node;
  const char *name, *value;
  const lxb_char_t *text;
  size_t len;
  if (ts_std_argc (vm, n, ud ? 3 : 2, ud ? 3 : 2, e, "attr") || ts_std_str (a + 1, &name, e, "attr")) return TS_ERR_INVAL;
  node = ts_html_node (a, e); if (!node) return TS_ERR_INVAL;
  if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return ts_std_inval (e, "attr: HTML element required");
  if (ud)
    {
      if (ts_std_str (a + 2, &value, e, "set_attr")) return TS_ERR_INVAL;
      if (!lxb_dom_element_set_attribute (lxb_dom_interface_element (node), (const lxb_char_t *) name, strlen (name), (const lxb_char_t *) value, strlen (value))) return ts_std_oom (e);
    }
  text = lxb_dom_element_get_attribute (lxb_dom_interface_element (node), (const lxb_char_t *) name, strlen (name), &len);
  if (!text) { ts_std_ret_nil (ret); return TS_OK; }
  return ts_std_ret_strn (ret, (const char *) text, len, e);
}

static TS_Status
h_escape (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  const char *s;
  ts_sbuf_t b;
  (void) ud;
  if (ts_std_argc (vm, n, 1, 1, e, "escape") || ts_std_str (a, &s, e, "escape")) return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (; *s; s++)
    {
      const char *escaped = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : *s == '\'' ? "&#39;" : NULL;
      if (escaped ? ts_sbuf_str (&b, escaped) : ts_sbuf_ch (&b, *s)) { ts_sbuf_free (&b); return ts_std_oom (e); }
    }
  return ts_std_ret_sbuf (ret, &b, e);
}

typedef struct { ts_sbuf_t b; size_t columns, col; bool ansi, pending_space; ts_cell_style_t style; TS_Error *error; } render_ctx;
static TS_Status
newline (render_ctx *ctx, bool force)
{
  ctx->pending_space = false;
  if (!force && !ctx->col) return TS_OK;
  if (ctx->ansi && ts_sbuf_str (&ctx->b, "\033[0m")) return ts_std_oom (ctx->error);
  if (ts_sbuf_ch (&ctx->b, '\n')) return ts_std_oom (ctx->error);
  ctx->col = 0; ctx->style = ts_style_default ();
  return TS_OK;
}

static TS_Status
render_text (render_ctx *ctx, const char *s, size_t len, ts_cell_style_t style, bool pre)
{
  size_t at = 0, bytes;
  int width;
  TS_Status st;
  while (at < len)
    {
      unsigned char c = (unsigned char) s[at];
      if (c == '\n' || c == '\r' || c == '\t' || c == ' ')
        {
          if (!pre) { ctx->pending_space = ctx->col != 0; at++; continue; }
          if (c == '\n') { st = newline (ctx, true); if (st) return st; at++; continue; }
          if (c == '\r') { at++; continue; }
          if (c == '\t')
            {
              size_t spaces = 8 - ctx->col % 8;
              while (spaces--) { st = render_text (ctx, " ", 1, style, true); if (st) return st; }
              at++; continue;
            }
        }
      if (c < 32 || c == 127) { at++; continue; } /* Terminal control hygiene. */
      if (ts_text_cluster (s + at, len - at, &bytes, &width) < 0)
        { ts_error_set (ctx->error, TS_ERR_PARSE, 0, at, "invalid UTF-8 in HTML text"); return TS_ERR_PARSE; }
      if (ctx->pending_space && ctx->col && ctx->col + 1 + (size_t) width <= ctx->columns)
        { if (ts_sbuf_ch (&ctx->b, ' ')) return ts_std_oom (ctx->error); ctx->col++; }
      ctx->pending_space = false;
      if (ctx->col && ctx->col + (size_t) width > ctx->columns)
        { st = newline (ctx, true); if (st) return st; }
      if (ctx->ansi && !ts_style_equal (style, ctx->style))
        { if (ts_style_ansi (&ctx->b, style)) return ts_std_oom (ctx->error); ctx->style = style; }
      if (ts_sbuf_put (&ctx->b, s + at, bytes)) return ts_std_oom (ctx->error);
      ctx->col += (size_t) width; at += bytes;
      if (ctx->b.len > 16u * 1024u * 1024u) { ts_error_set (ctx->error, TS_ERR_LIMIT, 0, 0, "HTML render exceeds 16 MiB"); return TS_ERR_LIMIT; }
    }
  return TS_OK;
}

static TS_Status
render_node (render_ctx *ctx, lxb_dom_node_t *node, ts_cell_style_t style, bool pre, unsigned depth)
{
  lxb_dom_node_t *child;
  bool block = false;
  TS_Status st;
  if (depth > 256) { ts_error_set (ctx->error, TS_ERR_LIMIT, 0, 0, "HTML render depth exceeds 256"); return TS_ERR_LIMIT; }
  if (node->type == LXB_DOM_NODE_TYPE_TEXT)
    {
      lxb_dom_character_data_t *data = lxb_dom_interface_character_data (node);
      return render_text (ctx, (const char *) data->data.data, data->data.length, style, pre);
    }
  if (node->type == LXB_DOM_NODE_TYPE_ELEMENT)
    {
      size_t len;
      const lxb_char_t *name = lxb_dom_element_local_name (lxb_dom_interface_element (node), &len);
      const lxb_css_rule_declaration_t *d;
#define TAG(s) (len == sizeof(s) - 1 && !memcmp (name, s, sizeof(s) - 1))
      if (TAG("script") || TAG("style") || TAG("head") || TAG("template")) return TS_OK;
      d = ts_css_property (node, "display", false);
      if (d && d->u.display->a == LXB_CSS_VALUE_NONE) return TS_OK;
      st = ts_css_cell_style (node, &style, ctx->error); if (st) return st;
      if (style.attrs & TS_ATTR_HIDDEN) return TS_OK;
      if (TAG("b") || TAG("strong") || TAG("h1") || TAG("h2") || TAG("h3")) style.attrs |= TS_ATTR_BOLD;
      if (TAG("i") || TAG("em")) style.attrs |= TS_ATTR_ITALIC;
      if (TAG("u") || TAG("a")) style.attrs |= TS_ATTR_UNDERLINE;
      if (TAG("s") || TAG("del")) style.attrs |= TS_ATTR_STRIKE;
      block = TAG("div") || TAG("p") || TAG("section") || TAG("article") || TAG("header") || TAG("footer") || TAG("ul") || TAG("ol") || TAG("li") || TAG("pre") || TAG("blockquote") || TAG("tr") || TAG("h1") || TAG("h2") || TAG("h3") || TAG("h4") || TAG("h5") || TAG("h6");
      if (block) { st = newline (ctx, false); if (st) return st; }
      if (TAG("br")) return newline (ctx, true);
      if (TAG("li")) { st = render_text (ctx, "• ", strlen ("• "), style, true); if (st) return st; }
      if (TAG("pre")) pre = true;
      d = ts_css_property (node, "white-space", true);
      if (d && (d->u.white_space->type == LXB_CSS_VALUE_PRE || d->u.white_space->type == LXB_CSS_VALUE_PRE_WRAP)) pre = true;
#undef TAG
    }
  for (child = node->first_child; child; child = child->next)
    { st = render_node (ctx, child, style, pre, depth + 1); if (st) return st; }
  return block ? newline (ctx, false) : TS_OK;
}

static TS_Status
h_render (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  render_ctx ctx = {0};
  int64_t width = 80;
  lxb_dom_node_t *node;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 1, 3, e, "render") || (n > 1 && ts_std_int (a + 1, &width, e, "render"))) return TS_ERR_INVAL;
  if (width < 2 || width > 10000) return ts_std_inval (e, "render: width must be 2..10000");
  if (n == 3 && a[2].type != TS_BOOL) return ts_std_inval (e, "render: bool ANSI selector required");
  node = ts_html_node (a, e); if (!node) return TS_ERR_INVAL;
  ctx.columns = (size_t) width; ctx.ansi = n < 3 || a[2].as.boolean; ctx.style = ts_style_default (); ctx.error = e;
  ts_sbuf_init (&ctx.b);
  st = render_node (&ctx, node, ctx.style, false, 0);
  if (!st && ctx.ansi && ts_sbuf_str (&ctx.b, "\033[0m")) st = ts_std_oom (e);
  if (st) { ts_sbuf_free (&ctx.b); return st; }
  return ts_std_ret_sbuf (ret, &ctx.b, e);
}

static const TS_FuncDef funcs[] = {
  {"parse", h_parse, NULL}, {"query", h_query, NULL}, {"query_all", h_query, (void *) 1},
  {"text", h_get, NULL}, {"serialize", h_get, (void *) 1}, {"tag", h_get, (void *) 2},
  {"parent", h_get, (void *) 3}, {"children", h_get, (void *) 4}, {"body", h_get, (void *) 5},
  {"attr", h_attr, NULL}, {"set_attr", h_attr, (void *) 1}, {"escape", h_escape, NULL},
  {"render", h_render, NULL}, {NULL, NULL, NULL}
};
const TS_Module ts_std_html_module = {"std.html", funcs};
