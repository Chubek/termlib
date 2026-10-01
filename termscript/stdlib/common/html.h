#ifndef TS_STD_HTML_INTERNAL_H
#define TS_STD_HTML_INTERNAL_H
#include "common/grid.h"
#include <lexbor/html/html.h>
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>
#include <lexbor/style/style.h>

typedef struct {
  lxb_html_document_t *document;
  ts_std_list_t *sheets; /* Retains every attached CSS sheet. */
} ts_html_t;
typedef struct {
  lxb_css_parser_t *parser;
  lxb_css_stylesheet_t *sheet;
} ts_css_t;

lxb_status_t ts_lexbor_write (const lxb_char_t *data, size_t len, void *ctx);
TS_Status ts_lexbor_error (TS_Error *e, lxb_status_t st, const char *operation);
ts_html_t *ts_html_owner (const TS_Value *v, TS_Error *e);
lxb_dom_node_t *ts_html_node (const TS_Value *v, TS_Error *e);
TS_Status ts_html_wrap_node (TS_Value *ret, const TS_Value *owner,
                             lxb_dom_node_t *node, TS_Error *e);
TS_Status ts_html_parse (TS_Value *ret, const char *source, TS_Error *e);
TS_Status ts_html_select (const TS_Value *root, const char *selector, bool all,
                          TS_Value *ret, TS_Error *e);
TS_Status ts_css_attach (const TS_Value *sheet, const TS_Value *html, TS_Error *e);
const lxb_css_rule_declaration_t *ts_css_property (lxb_dom_node_t *node,
                                                  const char *name, bool inherit);
TS_Status ts_css_cell_style (lxb_dom_node_t *node, ts_cell_style_t *style, TS_Error *e);
#endif
