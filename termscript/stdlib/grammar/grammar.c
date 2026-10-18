#include "grammar/grammar.h"
#include "common/ts_std_common.h"
#include <NovoParse.h>
#include <stdlib.h>
#include <string.h>

enum { SYMBOL_LIMIT = 128, RULE_LIMIT = 256, BODY_LIMIT = 32,
       NAME_LIMIT = 64, TEXT_LIMIT = 65536, INPUT_LIMIT = 4096,
       TREE_LIMIT = 16384, TREE_DEPTH = 128 };
typedef struct
{
  np_symbol_spec symbols[SYMBOL_LIMIT];
  np_production_spec rules[RULE_LIMIT];
  size_t symbol_count, rule_count, text_size;
  char *start;
  np_parser *parser;
  bool closed, parsed;
} grammar_t;
static const char tag;

static void
release (grammar_t *g)
{
  np_parser_destroy (g->parser);
  g->parser = NULL;
  for (size_t i = 0; i < g->symbol_count; i++)
    { free ((char *)g->symbols[i].name); free ((char *)g->symbols[i].value); }
  for (size_t i = 0; i < g->rule_count; i++)
    { free ((int *)g->rules[i].body); free ((char *)g->rules[i].label); }
  free (g->start);
  g->start = NULL;
  g->symbol_count = g->rule_count = g->text_size = 0;
  g->closed = true;
}
static void destroy (void *p) { release (p); free (p); }
static TS_Status limit (TS_Error *e, const char *message)
{ ts_error_set (e, TS_ERR_LIMIT, 0, 0, "%s", message); return TS_ERR_LIMIT; }
static bool name_ok (const char *s)
{
  size_t n = strlen (s);
  if (!n || n > NAME_LIMIT) return false;
  for (size_t i = 0; i < n; i++)
    if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z')
          || s[i] == '_' || (i && s[i] >= '0' && s[i] <= '9'))) return false;
  return true;
}
static int find (grammar_t *g, const char *name)
{
  for (size_t i = 0; i < g->symbol_count; i++)
    if (!strcmp (name, g->symbols[i].name)) return (int)i;
  return -1;
}
static TS_Status
create (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  (void)u;
  const char *start;
  if (ts_std_argc (vm, n, 1, 1, e, "grammar.new")
      || ts_std_str (a, &start, e, "start symbol")) return TS_ERR_INVAL;
  if (!name_ok (start)) return ts_std_inval (e, "invalid start symbol name");
  grammar_t *g = calloc (1, sizeof *g);
  if (!g) return ts_std_oom (e);
  g->start = strdup (start);
  if (!g->start) { destroy (g); return ts_std_oom (e); }
  TS_Status st = ts_value_make_handle (r, g, destroy, "GLR grammar", &tag);
  if (st) { destroy (g); return ts_std_oom (e); }
  return TS_OK;
}

static TS_Status
symbol (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  (void)r;
  np_symbol_kind kind = (np_symbol_kind)(intptr_t)u;
  size_t count = kind == NP_NONTERMINAL ? 2 : 3;
  const char *name, *text = NULL;
  if (ts_std_argc (vm, n, count, count, e, "grammar symbol")
      || ts_std_str (a + 1, &name, e, "symbol name")
      || (count == 3 && ts_std_str (a + 2, &text, e, "terminal text"))) return TS_ERR_INVAL;
  grammar_t *g = ts_std_handle (a, &tag, e, "grammar");
  if (!g) return TS_ERR_INVAL;
  if (g->closed || g->parser) return ts_std_inval (e, "grammar is closed or already built");
  if (!name_ok (name) || find (g, name) >= 0) return ts_std_inval (e, "invalid or duplicate symbol name");
  if (text && !*text) return ts_std_inval (e, "empty terminals are not allowed; use an empty rule");
  size_t bytes = strlen (name) + 1 + (text ? strlen (text) + 1 : 0);
  if (g->symbol_count == SYMBOL_LIMIT || bytes > TEXT_LIMIT - g->text_size)
    return limit (e, "grammar symbol/text limit exceeded");
  char *name_copy = strdup (name), *text_copy = text ? strdup (text) : NULL;
  if (!name_copy || (text && !text_copy))
    { free (name_copy); free (text_copy); return ts_std_oom (e); }
  g->symbols[g->symbol_count++] = (np_symbol_spec){ kind, name_copy, text_copy, 0, NP_ASSOC_NONE };
  g->text_size += bytes;
  return TS_OK;
}

static TS_Status
rule (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  (void)u; (void)r;
  const char *head, *label = NULL;
  if (ts_std_argc (vm, n, 3, 4, e, "grammar.rule")
      || ts_std_str (a + 1, &head, e, "rule head")
      || (n == 4 && ts_std_str (a + 3, &label, e, "rule label"))) return TS_ERR_INVAL;
  grammar_t *g = ts_std_handle (a, &tag, e, "grammar");
  ts_std_list_t *body = ts_std_list_unwrap (a + 2, e, "rule body");
  if (!g || !body) return TS_ERR_INVAL;
  if (g->closed || g->parser) return ts_std_inval (e, "grammar is closed or already built");
  int head_id = find (g, head);
  if (head_id < 0 || g->symbols[head_id].kind != NP_NONTERMINAL
      || (label && !name_ok (label))) return ts_std_inval (e, "rule needs a declared nonterminal head and valid label");
  size_t count = ts_std_list_len (body), bytes = label ? strlen (label) + 1 : 0;
  if (count > BODY_LIMIT || g->rule_count == RULE_LIMIT || bytes > TEXT_LIMIT - g->text_size)
    return limit (e, "grammar rule/body/text limit exceeded");
  int ids[BODY_LIMIT];
  for (size_t i = 0; i < count; i++)
    {
      const char *name;
      if (ts_std_str (ts_std_list_get (body, i), &name, e, "rule symbol")) return TS_ERR_INVAL;
      ids[i] = find (g, name);
      if (ids[i] < 0) return ts_std_inval (e, "rule references an undeclared symbol");
    }
  int *copy = count ? malloc (count * sizeof *copy) : NULL;
  char *label_copy = label ? strdup (label) : NULL;
  if ((count && !copy) || (label && !label_copy))
    { free (copy); free (label_copy); return ts_std_oom (e); }
  if (count) memcpy (copy, ids, count * sizeof *copy);
  g->rules[g->rule_count++] = (np_production_spec){ head_id, copy, count, label_copy, 0, NP_ASSOC_NONE };
  g->text_size += bytes;
  return TS_OK;
}

static TS_Status
build (grammar_t *g, const char *policy, TS_Error *e)
{
  if (g->parser) return ts_std_inval (e, "grammar is already built");
  if (strcmp (policy, "reject") && strcmp (policy, "first"))
    return ts_std_inval (e, "ambiguity policy must be reject or first");
  int start = find (g, g->start);
  if (start < 0 || g->symbols[start].kind != NP_NONTERMINAL || !g->rule_count)
    return ts_std_inval (e, "grammar needs a declared start nonterminal and rules");
  for (size_t i = 0; i < g->symbol_count; i++)
    if (g->symbols[i].kind == NP_NONTERMINAL)
      {
        bool defined = false;
        for (size_t j = 0; j < g->rule_count; j++)
          if (g->rules[j].head == (int)i) defined = true;
        if (!defined) return ts_std_inval (e, "nonterminal has no production");
      }
  np_grammar_spec spec = { "TermscriptGrammar", g->symbols, g->symbol_count,
                           g->rules, g->rule_count, start, NP_SKIP_NONE, NULL,
                           !strcmp (policy, "first") ? NP_FIRST : NP_REJECT, NULL };
  char message[TS_ERROR_MSG_SIZE] = {0};
  g->parser = np_parser_create (&spec, message, sizeof message);
  if (!g->parser)
    {
      TS_Status status = strstr (message, "memory") ? TS_ERR_NOMEM : TS_ERR_PARSE;
      ts_error_set (e, status, 0, 0, "grammar build: %s", message);
      return status;
    }
  return TS_OK;
}

/* Copy only the selected syntax tree; never call it a packed parse forest. */
static ts_dom_t *
copy_tree (const np_node *node, size_t depth, size_t *nodes, size_t *bytes,
           TS_Status *status)
{
  if (depth == TREE_DEPTH || *nodes == TREE_LIMIT)
    { *status = TS_ERR_LIMIT; return NULL; }
  (*nodes)++;
  const char *text = np_node_text (node), *name = np_node_name (node);
  size_t size = strlen (name) + (text ? np_node_text_length (node) : 0);
  if (size > (1u << 20) - *bytes) { *status = TS_ERR_LIMIT; return NULL; }
  *bytes += size;
  ts_dom_t *d = ts_dom_new_obj (), *children = ts_dom_new_arr ();
  if (!d || !children) goto oom;
  for (size_t i = 0; i < np_node_child_count (node); i++)
    {
      ts_dom_t *child = copy_tree (np_node_child (node, i), depth + 1, nodes, bytes, status);
      if (!child) goto fail;
      if (ts_dom_arr_push (children, child)) goto oom;
    }
  if (ts_dom_obj_set (d, "children", children)) { children = NULL; goto oom; }
  children = NULL;
  if (ts_dom_obj_set (d, "name", ts_dom_new_str (name))
      || ts_dom_obj_set (d, "start", ts_dom_new_int ((int64_t)np_node_start (node)))
      || ts_dom_obj_set (d, "end", ts_dom_new_int ((int64_t)np_node_end (node)))
      || ts_dom_obj_set (d, "text", text ? ts_dom_new_str (text) : ts_dom_new_null ())) goto oom;
  return d;
oom:
  *status = TS_ERR_NOMEM;
fail:
  ts_dom_free (children); ts_dom_free (d); return NULL;
}

static TS_Status
operation (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  intptr_t op = (intptr_t)u;
  if (ts_std_argc (vm, n, op == 2 ? 2 : 1, op == 1 || op == 2 ? 2 : 1, e, "grammar")) return TS_ERR_INVAL;
  grammar_t *g = ts_std_handle (a, &tag, e, "grammar");
  if (!g) return TS_ERR_INVAL;
  if (!op) { release (g); return TS_OK; }
  if (g->closed) return ts_std_inval (e, "grammar is closed");
  if (op == 1)
    {
      const char *policy = "reject";
      if (n == 2 && ts_std_str (a + 1, &policy, e, "ambiguity policy")) return TS_ERR_INVAL;
      return build (g, policy, e);
    }
  if (!g->parser) return ts_std_inval (e, "grammar must be built before parsing");
  if (op == 3) { ts_std_ret_int (r, (int64_t)np_parser_conflicts (g->parser)); return TS_OK; }
  if (op == 4)
    {
      if (!g->parsed) return ts_std_inval (e, "grammar has not parsed input yet");
      ts_std_ret_bool (r, np_parser_ambiguous (g->parser) != 0); return TS_OK;
    }
  const char *input;
  if (ts_std_str (a + 1, &input, e, "grammar.parse")) return TS_ERR_INVAL;
  size_t size = strlen (input);
  if (size > INPUT_LIMIT) return limit (e, "GLR input exceeds 4096 bytes");
  np_status result = np_parser_parse (g->parser, input, size);
  g->parsed = true;
  if (result != NP_SUCCESS)
    {
      TS_Status status = result == NP_ERROR_MEMORY ? TS_ERR_NOMEM : TS_ERR_PARSE;
      ts_error_set (e, status, 0, np_parser_position (g->parser), "%s", np_parser_error (g->parser));
      return status;
    }
  size_t nodes = 0, bytes = 0;
  TS_Status status = TS_OK;
  ts_dom_t *tree = copy_tree (np_parser_root (g->parser), 0, &nodes, &bytes, &status);
  if (!tree)
    {
      if (status == TS_ERR_NOMEM) return ts_std_oom (e);
      return limit (e, "GLR result tree exceeds depth/node/text limit");
    }
  return ts_dom_wrap (r, tree, e);
}
static const TS_FuncDef functions[] = {
  { "new", create, NULL }, { "nonterminal", symbol, (void *)NP_NONTERMINAL },
  { "literal", symbol, (void *)NP_LITERAL }, { "pattern", symbol, (void *)NP_PATTERN },
  { "rule", rule, NULL }, { "close", operation, (void *)0 },
  { "build", operation, (void *)1 }, { "parse", operation, (void *)2 },
  { "conflicts", operation, (void *)3 }, { "ambiguous", operation, (void *)4 }
};
const TS_Module ts_std_grammar_module = TS_MODULE_INIT ("std.grammar", functions);
