/* ts_parser_ng.c -- NovoParse front end adapter for Termscript.ng. */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "TermscriptParser.h"

#if __STDC_VERSION__ >= 201112L
#define TS_TLS _Thread_local
#else
#define TS_TLS
#endif

void *ts_parse_alloc (size_t size);
void ts_parse_free (void *pointer);
void ts_oom (void);
unsigned ts_curline (const char *pos);

void *ts_mk_seq (void);
void *ts_seq_append (void *seq, void *stmt);
void *ts_mk_const (char *name, void *expr, unsigned line);
void *ts_mk_assign (char *name, void *expr, unsigned line);
void *ts_mk_exprstmt (void *expr, unsigned line);
void *ts_mk_while (void *cond, void *body, unsigned line);
void *ts_mk_if (void *cond, void *thenb, void *elseb, unsigned line);
void *ts_mk_or (void *lhs, void *rhs, unsigned line);
void *ts_mk_unary (char *name, void *tail, unsigned line);
void *ts_mk_callrest (char *func, void *args, unsigned line);
void *ts_mk_ref (char *name, unsigned line);
void *ts_mk_number (char *tok, unsigned line);
void *ts_mk_string (char *tok, unsigned line);
void *ts_mk_argstr (char *tok, unsigned line);
void *ts_mk_argnum (char *tok, unsigned line);
void *ts_mk_argword (char *tok, unsigned line);
void *ts_mk_argref (char *tok, unsigned line);
void *ts_args_new (void);
void *ts_args_add (void *args, void *arg);

static TS_TLS size_t ts_ng_last_offset;
static TS_TLS char ts_ng_last_message[256];

static void *ts_np_stmt (const np_node *node, const char *input);
static void *ts_np_expr (const np_node *node, const char *input);
static void *ts_np_program (const np_node *node, const char *input);

static void
ts_ng_set_error (size_t offset, const char *message)
{
  ts_ng_last_offset = offset;
  snprintf (ts_ng_last_message, sizeof ts_ng_last_message, "%s",
            message ? message : "termscript syntax error");
}

size_t
ts_ng_error_offset (void)
{
  return ts_ng_last_offset;
}

const char *
ts_ng_error_message (void)
{
  return ts_ng_last_message[0] ? ts_ng_last_message : "termscript syntax error";
}

static bool
ts_np_is (const np_node *node, const char *name)
{
  const char *got = np_node_name (node);
  return got && strcmp (got, name) == 0;
}

static unsigned
ts_np_line (const np_node *node, const char *input)
{
  return ts_curline (input + np_node_start (node));
}

static char *
ts_np_textdup (const np_node *node)
{
  size_t len = np_node_text_length (node);
  const char *text = np_node_text (node);
  char *copy;
  if (!text)
    {
      ts_oom ();
      return NULL;
    }
  copy = ts_parse_alloc (len + 1);
  if (!copy)
    {
      ts_oom ();
      return NULL;
    }
  memcpy (copy, text, len);
  copy[len] = '\0';
  return copy;
}

static char *
ts_np_rangedup (const char *input, size_t start, size_t end)
{
  char *copy;
  if (!input || end < start)
    {
      ts_oom ();
      return NULL;
    }
  copy = ts_parse_alloc (end - start + 1);
  if (!copy)
    {
      ts_oom ();
      return NULL;
    }
  memcpy (copy, input + start, end - start);
  copy[end - start] = '\0';
  return copy;
}

static const np_node *
ts_np_first (const np_node *node, const char *name)
{
  size_t i, n = np_node_child_count (node);
  for (i = 0; i < n; i++)
    {
      const np_node *child = np_node_child (node, i);
      if (ts_np_is (child, name))
        return child;
    }
  return NULL;
}

static bool
ts_np_is_expr_node (const np_node *node)
{
  return ts_np_is (node, "Expr") || ts_np_is (node, "OrExpr") ||
         ts_np_is (node, "RefExpr") || ts_np_is (node, "CallExpr") ||
         ts_np_is (node, "Call") || ts_np_is (node, "IdentExpr") ||
         ts_np_is (node, "NumberExpr") || ts_np_is (node, "StringExpr");
}

static bool
ts_np_is_arg_node (const np_node *node)
{
  return ts_np_is (node, "ArgString") || ts_np_is (node, "ArgNumber") ||
          ts_np_is (node, "ArgRef") || ts_np_is (node, "ArgWord");
}

static bool
ts_np_is_call_arg_container (const np_node *node)
{
  return ts_np_is (node, "SpaceArgs") || ts_np_is (node, "ParenArgs") ||
         ts_np_is (node, "EmptyParenArgs") || ts_np_is (node, "ArgList");
}

static bool
ts_np_is_stmt_node (const np_node *node)
{
  return ts_np_is (node, "Stmt") || ts_np_is (node, "ConstDecl") ||
         ts_np_is (node, "AssignStmt") || ts_np_is (node, "ExprStmt") ||
         ts_np_is (node, "WhileStmt") || ts_np_is (node, "IfStmt");
}

static const np_node *
ts_np_first_expr (const np_node *node)
{
  size_t i, n = np_node_child_count (node);
  for (i = 0; i < n; i++)
    {
      const np_node *child = np_node_child (node, i);
      if (ts_np_is_expr_node (child))
        return child;
    }
  return NULL;
}

static void *
ts_np_arg (const np_node *node, const char *input)
{
  const np_node *tok;
  unsigned line = ts_np_line (node, input);
  if (ts_np_is (node, "ArgString"))
    return (tok = ts_np_first (node, "STRING")) ?
           ts_mk_argstr (ts_np_textdup (tok), line) : NULL;
  if (ts_np_is (node, "ArgNumber"))
    return (tok = ts_np_first (node, "NUMBER")) ?
           ts_mk_argnum (ts_np_textdup (tok), line) : NULL;
  if (ts_np_is (node, "ArgRef"))
    return (tok = ts_np_first (node, "IDENT")) ?
           ts_mk_argref (ts_np_textdup (tok), line) : NULL;
  if (ts_np_is (node, "ArgWord"))
    return (tok = ts_np_first (node, "IDENT")) ?
           ts_mk_argword (ts_np_textdup (tok), line) : NULL;
  return NULL;
}

static void *
ts_np_grouped_arg (const np_node *call, const char *input, size_t *index)
{
  const np_node *node = np_node_child (call, *index);
  unsigned line = ts_np_line (node, input);
  if (ts_np_is (node, "ArgWord") || ts_np_is (node, "ArgNumber"))
    {
      bool number = ts_np_is (node, "ArgNumber");
      size_t end = np_node_end (node);
      size_t n = np_node_child_count (call);
      while (*index + 1 < n)
        {
          const np_node *next = np_node_child (call, *index + 1);
          if (!ts_np_is (next, number ? "ArgNumber" : "ArgWord") ||
              np_node_start (next) != end)
            break;
          end = np_node_end (next);
          (*index)++;
        }
      if (number)
        return ts_mk_argnum (ts_np_rangedup (input, np_node_start (node), end),
                             line);
      return ts_mk_argword (ts_np_rangedup (input, np_node_start (node), end),
                            line);
    }
  if (ts_np_is (node, "ArgRef"))
    {
      const np_node *ident = ts_np_first (node, "IDENT");
      size_t start, end, n;
      if (!ident)
        return NULL;
      start = np_node_start (ident);
      end = np_node_end (ident);
      n = np_node_child_count (call);
      while (*index + 1 < n)
        {
          const np_node *next = np_node_child (call, *index + 1);
          if (!ts_np_is (next, "ArgWord") || np_node_start (next) != end)
            break;
          end = np_node_end (next);
          (*index)++;
        }
      return ts_mk_argref (ts_np_rangedup (input, start, end), line);
    }
  return ts_np_arg (node, input);
}

static void *
ts_np_collect_args (const np_node *node, const char *input, void *args)
{
  size_t i, n;
  if (!args || !node)
    return NULL;
  if (ts_np_is_arg_node (node))
    {
      void *arg = ts_np_arg (node, input);
      return ts_args_add (args, arg);
    }
  if (!ts_np_is_call_arg_container (node))
    return args;
  n = np_node_child_count (node);
  for (i = 0; i < n; i++)
    {
      args = ts_np_collect_args (np_node_child (node, i), input, args);
      if (!args)
        return NULL;
    }
  return args;
}

static void *
ts_np_call (const np_node *node, const char *input)
{
  size_t i, n = np_node_child_count (node), ident_count = 0;
  char *module = NULL, *func = NULL;
  void *args = ts_args_new ();
  void *tail;
  if (!args)
    return NULL;
  for (i = 0; i < n; i++)
    {
      const np_node *child = np_node_child (node, i);
      if (ts_np_is (child, "IDENT"))
        {
          char *text = ts_np_textdup (child);
          if (!text)
            return NULL;
          if (ident_count++ == 0)
            module = text;
          else if (!func)
            func = text;
          else
            ts_parse_free (text);
        }
      else if (ts_np_is_call_arg_container (child))
        {
          args = ts_np_collect_args (child, input, args);
          if (!args)
            return NULL;
        }
      else if (ts_np_is_arg_node (child))
        {
          void *arg = ts_np_grouped_arg (node, input, &i);
          args = ts_args_add (args, arg);
          if (!args)
            return NULL;
        }
    }
  if (!module || !func)
    {
      ts_parse_free (module);
      ts_parse_free (func);
      return NULL;
    }
  tail = ts_mk_callrest (func, args, ts_np_line (node, input));
  if (!tail)
    {
      ts_parse_free (module);
      return NULL;
    }
  return ts_mk_unary (module, tail, ts_np_line (node, input));
}

static void *
ts_np_expr (const np_node *node, const char *input)
{
  const np_node *tok;
  unsigned line;
  if (!node)
    return NULL;
  if (ts_np_is (node, "Expr") || ts_np_is (node, "CallExpr"))
    return ts_np_expr (np_node_child (node, 0), input);
  line = ts_np_line (node, input);
  if (ts_np_is (node, "OrExpr"))
    {
      const np_node *lhs = NULL, *rhs = NULL;
      size_t i, n = np_node_child_count (node);
      for (i = 0; i < n; i++)
        {
          const np_node *child = np_node_child (node, i);
          if (ts_np_is_expr_node (child))
            { if (!lhs) lhs = child; else rhs = child; }
        }
      if (lhs && rhs)
        {
          void *left = ts_np_expr (lhs, input);
          void *right = left ? ts_np_expr (rhs, input) : NULL;
          return left && right ? ts_mk_or (left, right, line) : NULL;
        }
      return NULL;
    }
  if (ts_np_is (node, "Call"))
    return ts_np_call (node, input);
  if (ts_np_is (node, "RefExpr"))
    return (tok = ts_np_first (node, "IDENT")) ?
           ts_mk_ref (ts_np_textdup (tok), line) : NULL;
  if (ts_np_is (node, "IdentExpr"))
    return (tok = ts_np_first (node, "IDENT")) ?
           ts_mk_unary (ts_np_textdup (tok), NULL, line) : NULL;
  if (ts_np_is (node, "NumberExpr"))
    return (tok = ts_np_first (node, "NUMBER")) ?
           ts_mk_number (ts_np_textdup (tok), line) : NULL;
  if (ts_np_is (node, "StringExpr"))
    return (tok = ts_np_first (node, "STRING")) ?
           ts_mk_string (ts_np_textdup (tok), line) : NULL;
  return NULL;
}

static void *
ts_np_program (const np_node *node, const char *input)
{
  void *seq = ts_mk_seq ();
  size_t i, n;
  if (!seq || !node)
    return NULL;
  n = np_node_child_count (node);
  for (i = 0; i < n; i++)
    {
      const np_node *child = np_node_child (node, i);
      void *stmt;
      if (!ts_np_is_stmt_node (child))
        continue;
      stmt = ts_np_stmt (child, input);
      seq = ts_seq_append (seq, stmt);
      if (!seq)
        return NULL;
    }
  return seq;
}

static void *
ts_np_if_tail (const np_node *node, const char *input)
{
  if (!node || ts_np_is (node, "NoElseTail"))
    return NULL;
  if (ts_np_is (node, "ElseTail"))
    return ts_np_program (ts_np_first (node, "Program"), input);
  return NULL;
}

static void *
ts_np_stmt (const np_node *node, const char *input)
{
  const np_node *name, *expr, *body, *tail;
  unsigned line;
  if (!node)
    return NULL;
  if (ts_np_is (node, "Stmt"))
    return ts_np_stmt (np_node_child (node, 0), input);
  line = ts_np_line (node, input);
  if (ts_np_is (node, "ConstDecl"))
    {
      name = ts_np_first (node, "IDENT");
      expr = ts_np_first_expr (node);
      if (name && expr)
        {
          char *text = ts_np_textdup (name);
          void *rhs = text ? ts_np_expr (expr, input) : NULL;
          return text && rhs ? ts_mk_const (text, rhs, line) : NULL;
        }
      return NULL;
    }
  if (ts_np_is (node, "AssignStmt"))
    {
      name = ts_np_first (node, "IDENT");
      expr = ts_np_first_expr (node);
      if (name && expr)
        {
          char *text = ts_np_textdup (name);
          void *rhs = text ? ts_np_expr (expr, input) : NULL;
          return text && rhs ? ts_mk_assign (text, rhs, line) : NULL;
        }
      return NULL;
    }
  if (ts_np_is (node, "ExprStmt"))
    {
      expr = ts_np_first_expr (node);
      if (expr)
        {
          void *rhs = ts_np_expr (expr, input);
          return rhs ? ts_mk_exprstmt (rhs, line) : NULL;
        }
      return NULL;
    }
  if (ts_np_is (node, "WhileStmt"))
    {
      expr = ts_np_first_expr (node);
      body = ts_np_first (node, "Program");
      if (expr && body)
        {
          void *cond = ts_np_expr (expr, input);
          void *block = cond ? ts_np_program (body, input) : NULL;
          return cond && block ? ts_mk_while (cond, block, line) : NULL;
        }
      return NULL;
    }
  if (ts_np_is (node, "IfStmt"))
    {
      expr = ts_np_first_expr (node);
      body = ts_np_first (node, "Program");
      tail = ts_np_first (node, "ElseTail");
      if (!tail)
        tail = ts_np_first (node, "NoElseTail");
      if (expr && body)
        {
          void *cond = ts_np_expr (expr, input);
          void *thenb = cond ? ts_np_program (body, input) : NULL;
          void *elseb = thenb ? ts_np_if_tail (tail, input) : NULL;
          return cond && thenb ? ts_mk_if (cond, thenb, elseb, line) : NULL;
        }
      return NULL;
    }
  return NULL;
}

void *
ts_ng_parse (const char *input)
{
  char error[512];
  np_status status;
  np_parser *parser;
  void *tree;
  ts_ng_last_offset = 0;
  ts_ng_last_message[0] = '\0';
  if (!input)
    {
      ts_ng_set_error (0, "input is NULL");
      return NULL;
    }
  parser = Termscript_create (error, sizeof error);
  if (!parser)
    {
      ts_ng_set_error (0, error[0] ? error : "Termscript grammar rejected");
      return NULL;
    }
  status = np_parser_parse (parser, input, strlen (input));
  if (status != NP_SUCCESS)
    {
      ts_ng_set_error (np_parser_position (parser), np_parser_error (parser));
      np_parser_destroy (parser);
      return NULL;
    }
  tree = ts_np_program (np_parser_root (parser), input);
  if (!tree)
    ts_ng_set_error (np_parser_position (parser),
                     "unsupported or invalid Termscript syntax tree");
  np_parser_destroy (parser);
  return tree;
}
