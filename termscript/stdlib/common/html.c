#include "common/html.h"
#include <stdlib.h>
#include <string.h>

static const char document_tag, node_tag;
typedef struct { TS_Value owner; lxb_dom_node_t *node; } node_ref;

lxb_status_t
ts_lexbor_write (const lxb_char_t *data, size_t len, void *ctx)
{
  ts_sbuf_t *b = ctx;
  if (len > 16u * 1024u * 1024u - b->len) return LXB_STATUS_ERROR_OVERFLOW;
  return ts_sbuf_put (b, (const char *) data, len) ? LXB_STATUS_ERROR_MEMORY_ALLOCATION : LXB_STATUS_OK;
}

TS_Status
ts_lexbor_error (TS_Error *e, lxb_status_t st, const char *operation)
{
  TS_Status code = st == LXB_STATUS_ERROR_MEMORY_ALLOCATION ? TS_ERR_NOMEM :
                   st == LXB_STATUS_ERROR_OVERFLOW ? TS_ERR_LIMIT : TS_ERR_PARSE;
  ts_error_set (e, code, 0, 0, "%s: Lexbor status %u", operation, (unsigned) st);
  return code;
}

static void
document_free (void *p)
{
  ts_html_t *h = p;
  if (!h) return;
  if (h->document) { lxb_style_destroy (h->document); lxb_html_document_destroy (h->document); }
  ts_std_list_free (h->sheets); free (h);
}

static void
node_free (void *p)
{
  node_ref *ref = p;
  ts_value_free (&ref->owner); free (ref);
}

ts_html_t *
ts_html_owner (const TS_Value *v, TS_Error *e)
{
  ts_html_t *h = ts_value_handle (v, &document_tag);
  node_ref *r;
  if (h) return h;
  r = ts_value_handle (v, &node_tag);
  if (r) return ts_value_handle (&r->owner, &document_tag);
  ts_std_inval (e, "HTML document/node required");
  return NULL;
}

lxb_dom_node_t *
ts_html_node (const TS_Value *v, TS_Error *e)
{
  node_ref *r = ts_value_handle (v, &node_tag);
  ts_html_t *h;
  if (r) return r->node;
  h = ts_html_owner (v, e);
  return h ? lxb_dom_interface_node (h->document) : NULL;
}

TS_Status
ts_html_wrap_node (TS_Value *ret, const TS_Value *owner, lxb_dom_node_t *node, TS_Error *e)
{
  node_ref *r, *source;
  if (!node) { ts_std_ret_nil (ret); return TS_OK; }
  r = calloc (1, sizeof *r);
  if (!r) return ts_std_oom (e);
  source = ts_value_handle (owner, &node_tag);
  if (ts_value_copy (&r->owner, source ? &source->owner : owner)) { free (r); return ts_std_oom (e); }
  r->node = node;
  if (ts_value_make_handle (ret, r, node_free, "<html node>", &node_tag)) { node_free (r); return ts_std_oom (e); }
  return TS_OK;
}

TS_Status
ts_html_parse (TS_Value *ret, const char *source, TS_Error *e)
{
  ts_html_t *h;
  lxb_status_t status;
  TS_Status st = ts_utf8_validate (source, strlen (source), e);
  if (st) return st;
  h = calloc (1, sizeof *h);
  if (!h) return ts_std_oom (e);
  h->document = lxb_html_document_create (); h->sheets = ts_std_list_new ();
  if (!h->document || !h->sheets) { document_free (h); return ts_std_oom (e); }
  status = lxb_style_init (h->document);
  if (!status) status = lxb_html_document_parse (h->document, (const lxb_char_t *) source, strlen (source));
  if (status) { document_free (h); return ts_lexbor_error (e, status, "parse HTML"); }
  if (ts_value_make_handle (ret, h, document_free, "<html document>", &document_tag)) { document_free (h); return ts_std_oom (e); }
  return TS_OK;
}

typedef struct { const TS_Value *owner; TS_Value *ret; ts_std_list_t *list; TS_Error *error; TS_Status status; } select_ctx;
static lxb_status_t
select_cb (lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *p)
{
  select_ctx *ctx = p;
  TS_Value v = {0};
  (void) spec;
  ctx->status = ts_html_wrap_node (ctx->list ? &v : ctx->ret, ctx->owner, node, ctx->error);
  if (!ctx->status && ctx->list) ctx->status = ts_std_list_push (ctx->list, &v);
  ts_value_free (&v);
  if (ctx->status) return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
  return ctx->list ? LXB_STATUS_OK : LXB_STATUS_STOP;
}

TS_Status
ts_html_select (const TS_Value *root, const char *selector, bool all, TS_Value *ret, TS_Error *e)
{
  lxb_dom_node_t *node = ts_html_node (root, e);
  lxb_css_parser_t *parser = NULL;
  lxb_selectors_t *selectors = NULL;
  lxb_css_selector_list_t *list = NULL;
  lxb_status_t status;
  select_ctx ctx = {root, ret, NULL, e, TS_OK};
  TS_Status st = TS_OK;
  if (!node) return TS_ERR_INVAL;
  if (strlen (selector) > 4096) { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "selector exceeds 4096 bytes"); return TS_ERR_LIMIT; }
  ts_std_ret_nil (ret);
  parser = lxb_css_parser_create (); selectors = lxb_selectors_create ();
  if (!parser || !selectors) { st = ts_std_oom (e); goto done; }
  status = lxb_css_parser_init (parser, NULL);
  if (!status) status = lxb_selectors_init (selectors);
  if (status) { st = ts_lexbor_error (e, status, "init selector"); goto done; }
  list = lxb_css_selectors_parse (parser, (const lxb_char_t *) selector, strlen (selector));
  if (!list || parser->status) { st = ts_lexbor_error (e, parser->status ? parser->status : LXB_STATUS_ERROR, "parse selector"); goto done; }
  if (all) { ctx.list = ts_std_list_new (); if (!ctx.list) { st = ts_std_oom (e); goto done; } }
  lxb_selectors_opt_set (selectors, LXB_SELECTORS_OPT_MATCH_FIRST);
  status = lxb_selectors_find (selectors, node, list, select_cb, &ctx);
  if (status && status != LXB_STATUS_STOP) { st = ctx.status ? ctx.status : ts_lexbor_error (e, status, "query selector"); goto done; }
  if (all) { st = ts_std_list_wrap (ret, ctx.list, e); ctx.list = NULL; }
done:
  ts_std_list_free (ctx.list);
  if (selectors) lxb_selectors_destroy (selectors, true);
  if (list) lxb_css_selector_list_destroy_memory (list);
  if (parser) lxb_css_parser_destroy (parser, true);
  return st;
}
