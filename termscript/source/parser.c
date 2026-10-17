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
void *ts_mk_argexpr (void *expr);
void *ts_mk_design (int kind, char *name, char *other, void *body,
                    unsigned line);
void *ts_mk_flow (void *value, void *on_error, void *after_error,
                  void *on_success, unsigned line);
void *ts_mk_field (char *name, char *field, unsigned line);
void *ts_mk_not (void *expr, unsigned line);
void *ts_mk_function (char *, void *, void *, unsigned);
void *ts_mk_return (void *, unsigned);
void *ts_mk_and (void *, void *, unsigned);

static char *
ts_np_dup (const char *text)
{
  char *copy = ts_parse_alloc (strlen (text) + 1);
  if (copy)
    strcpy (copy, text);
  return copy;
}

static void *
ts_np_builtin (const char *name, void *lhs, void *rhs, unsigned line)
{
  if (!lhs)
    return NULL;
  void *args = ts_args_new ();
  if (lhs)
    args = ts_args_add (args, ts_mk_argexpr (lhs));
  if (rhs)
    args = ts_args_add (args, ts_mk_argexpr (rhs));
  return ts_mk_unary (ts_np_dup ("G"),
                      ts_mk_callrest (ts_np_dup (name), args, line), line);
}

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
  return ts_ng_last_message[0] ? ts_ng_last_message
                               : "termscript syntax error";
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

static char *
ts_np_name (const np_node *node)
{
  const np_node *tok;
  if (!node)
    return NULL;
  if (ts_np_is (node, "IDENT") || ts_np_is (node, "VARIABLE")
      || ts_np_is (node, "WORD"))
    return ts_np_textdup (node);
  tok = ts_np_first (node, "IDENT");
  if (!tok)
    tok = ts_np_first (node, "WORD");
  if (!tok)
    tok = ts_np_first (node, "VARIABLE");
  if (!tok)
    tok = ts_np_first (node, "Name");
  return tok ? ts_np_name (tok) : NULL;
}

static bool
ts_np_is_expr_node (const np_node *node)
{
  return ts_np_is (node, "Expr") || ts_np_is (node, "OrExpr")
         || ts_np_is (node, "RefExpr") || ts_np_is (node, "CallExpr")
         || ts_np_is (node, "Call") || ts_np_is (node, "IdentExpr")
         || ts_np_is (node, "NumberExpr") || ts_np_is (node, "StringExpr")
         || ts_np_is (node, "GroupExpr") || ts_np_is (node, "SymbolExpr")
         || ts_np_is (node, "FieldExpr") || ts_np_is (node, "NotExpr")
         || ts_np_is (node, "FlowExpr") || ts_np_is (node, "SuccessExpr")
         || ts_np_is (node, "ListExpr") || ts_np_is (node, "MapExpr")
         || ts_np_is (node, "BinaryExpr") || ts_np_is (node, "AndExpr")
         || ts_np_is (node, "BitOrExpr") || ts_np_is (node, "BitXorExpr")
         || ts_np_is (node, "BitAndExpr") || ts_np_is (node, "NegExpr")
         || ts_np_is (node, "DotCall") || ts_np_is (node, "InvokeCall");
}

static bool
ts_np_is_arg_node (const np_node *node)
{
  return ts_np_is (node, "ArgString") || ts_np_is (node, "ArgNumber")
         || ts_np_is (node, "ArgRef") || ts_np_is (node, "ArgWord")
         || ts_np_is (node, "ArgSymbol");
}

static bool
ts_np_is_call_arg_container (const np_node *node)
{
  return ts_np_is (node, "SpaceArgs") || ts_np_is (node, "ParenArgs")
         || ts_np_is (node, "EmptyParenArgs") || ts_np_is (node, "ArgList");
}

static bool
ts_np_is_stmt_node (const np_node *node)
{
  return ts_np_is (node, "Stmt") || ts_np_is (node, "ConstDecl")
         || ts_np_is (node, "AssignStmt") || ts_np_is (node, "ExprStmt")
         || ts_np_is (node, "WhileStmt") || ts_np_is (node, "IfStmt")
         || ts_np_is (node, "ConfigDecl") || ts_np_is (node, "LifetimeDecl")
         || ts_np_is (node, "AliasDecl") || ts_np_is (node, "DefineDecl")
         || ts_np_is (node, "InterfaceDecl") || ts_np_is (node, "SectionDecl")
         || ts_np_is (node, "Declaration") || ts_np_is (node, "Simple")
         || ts_np_is (node, "UseDecl") || ts_np_is (node, "CompoundAssign")
         || ts_np_is (node, "FunctionDecl") || ts_np_is (node, "ReturnStmt");
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
    return (tok = ts_np_first (node, "STRING"))
               ? ts_mk_argstr (ts_np_textdup (tok), line)
               : NULL;
  if (ts_np_is (node, "ArgNumber"))
    return (tok = ts_np_first (node, "NUMBER"))
               ? ts_mk_argnum (ts_np_textdup (tok), line)
               : NULL;
  if (ts_np_is (node, "ArgRef"))
    return ts_mk_argref (ts_np_name (node), line);
  if (ts_np_is (node, "ArgWord"))
    return ts_mk_argword (ts_np_name (node), line);
  if (ts_np_is (node, "ArgSymbol"))
    return ts_mk_argexpr (ts_np_expr (node, input));
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
          if (!ts_np_is (next, number ? "ArgNumber" : "ArgWord")
              || np_node_start (next) != end)
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
  if (ts_np_is_expr_node (node))
    return ts_args_add (args, ts_mk_argexpr (ts_np_expr (node, input)));
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
      if (ts_np_is (child, "Name"))
        {
          char *text = ts_np_name (child);
          if (!text)
            return NULL;
          if (ident_count++ == 0)
            module = text;
          else if (!func)
            func = text;
          else
            ts_parse_free (text);
        }
      else if (ts_np_is (child, "WORD") && ts_np_is (node, "DotCall"))
        func = ts_np_textdup (child);
      else if (ts_np_is (child, "FunctionName"))
        func = ts_np_rangedup (input, np_node_start (child),
                               np_node_end (child));
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
  if (ts_np_is (node, "InvokeCall"))
    {
      if (module
          && (!strcmp (module, "print") || !strcmp (module, "min")
              || !strcmp (module, "max") || !strcmp (module, "error")))
        {
          func = !strcmp (module, "print") ? ts_np_dup ("puts") : module;
          module = ts_np_dup ("G");
        }
      else
        func = ts_np_dup ("__call");
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
  if (ts_np_is (node, "Expr") || ts_np_is (node, "CallExpr")
      || ts_np_is (node, "GroupExpr"))
    return ts_np_expr (ts_np_first_expr (node), input);
  line = ts_np_line (node, input);
  if (ts_np_is (node, "BinaryExpr") || ts_np_is (node, "BitOrExpr")
      || ts_np_is (node, "BitXorExpr") || ts_np_is (node, "BitAndExpr")
      || ts_np_is (node, "AndExpr"))
    {
      const np_node *left = NULL, *right = NULL;
      const char *op = ts_np_is (node, "BitOrExpr")    ? "bor"
                       : ts_np_is (node, "BitXorExpr") ? "bxor"
                                                       : "band";
      for (size_t i = 0; i < np_node_child_count (node); i++)
        {
          const np_node *ch = np_node_child (node, i);
          if (ts_np_is_expr_node (ch))
            {
              if (!left)
                left = ch;
              else
                right = ch;
            }
          else if (ts_np_is (ch, "Operator") || ts_np_is (ch, "compare_op")
                   || ts_np_is (ch, "shift_op") || ts_np_is (ch, "add_op")
                   || ts_np_is (ch, "mul_op"))
            {
              size_t start = np_node_start (ch), end = np_node_end (ch);
              static const char *const tokens[]
                  = { "+", "-", "*",  "/",  "%",  "==", "!=",
                      "<", ">", "<=", ">=", "<<", ">>" };
              static const char *const names[]
                  = { "add", "sub", "mul", "div", "mod", "eq", "ne",
                      "lt",  "gt",  "le",  "ge",  "shl", "shr" };
              for (size_t j = 0; j < sizeof tokens / sizeof *tokens; j++)
                if (strlen (tokens[j]) == end - start
                    && !memcmp (input + start, tokens[j], end - start))
                  op = names[j];
            }
        }
      if (!left || !right)
        return NULL;
      void *lhs = ts_np_expr (left, input), *rhs = ts_np_expr (right, input);
      if (!lhs || !rhs)
        return NULL;
      return ts_np_is (node, "AndExpr") ? ts_mk_and (lhs, rhs, line)
                                        : ts_np_builtin (op, lhs, rhs, line);
    }
  if (ts_np_is (node, "NegExpr"))
    {
      const np_node *operand = ts_np_first_expr (node);
      const np_node *number = ts_np_is (operand, "NumberExpr")
                                  ? ts_np_first (operand, "NUMBER")
                                  : NULL;
      if (number && np_node_text (number)[0] != '-')
        {
          size_t size = np_node_text_length (number);
          char *signed_number = ts_parse_alloc (size + 2);
          if (!signed_number)
            return NULL;
          signed_number[0] = '-';
          memcpy (signed_number + 1, np_node_text (number), size);
          signed_number[size + 1] = 0;
          return ts_mk_number (signed_number, line);
        }
      return ts_np_builtin ("neg", ts_np_expr (operand, input), NULL, line);
    }
  if (ts_np_is (node, "ListExpr") || ts_np_is (node, "MapExpr"))
    {
      bool map = ts_np_is (node, "MapExpr");
      void *args = ts_args_new ();
      const np_node *entries
          = ts_np_first (node, map ? "MapEntries" : "ArgList");
      if (!map && entries)
        args = ts_np_collect_args (entries, input, args);
      if (map && entries)
        for (size_t i = 0; i < np_node_child_count (entries); i++)
          {
            const np_node *entry = np_node_child (entries, i);
            if (!ts_np_is (entry, "MapEntry"))
              continue;
            const np_node *key = ts_np_first (entry, "STRING");
            if (!key)
              return NULL;
            args
                = ts_args_add (args, ts_mk_argstr (ts_np_textdup (key), line));
            args = ts_args_add (args, ts_mk_argexpr (ts_np_expr (
                                          ts_np_first_expr (entry), input)));
          }
      char *module = ts_parse_alloc (2), *func = ts_parse_alloc (5);
      if (!module || !func)
        {
          ts_oom ();
          return NULL;
        }
      strcpy (module, "G");
      strcpy (func, map ? "map" : "list");
      return ts_mk_unary (module, ts_mk_callrest (func, args, line), line);
    }
  if (ts_np_is (node, "NotExpr"))
    return ts_mk_not (ts_np_expr (ts_np_first_expr (node), input), line);
  if (ts_np_is (node, "FieldExpr"))
    return ts_mk_field (ts_np_name (ts_np_first (node, "Name")),
                        ts_np_name (ts_np_first (node, "IDENT")), line);
  if (ts_np_is (node, "SymbolExpr") || ts_np_is (node, "ArgSymbol"))
    return ts_mk_design (7, ts_np_name (node), NULL, NULL, line);
  if (ts_np_is (node, "FlowExpr") || ts_np_is (node, "SuccessExpr"))
    {
      void *value = NULL, *handler = NULL, *after = NULL, *success = NULL;
      size_t i, n = np_node_child_count (node);
      for (i = 0; i < n; i++)
        {
          const np_node *ch = np_node_child (node, i);
          if (ts_np_is_expr_node (ch))
            {
              if (!value)
                value = ts_np_expr (ch, input);
              else
                handler = ts_np_expr (ch, input);
            }
          else if (ts_np_is (ch, "ErrorTail"))
            {
              const np_node *ex = ts_np_first_expr (ch);
              const np_node *tail = ts_np_first (ch, "SuccessTail");
              if (ex)
                after = ts_np_expr (ex, input);
              if (tail)
                success = ts_np_expr (ts_np_first_expr (tail), input);
            }
        }
      if (ts_np_is (node, "SuccessExpr"))
        {
          success = handler;
          handler = NULL;
        }
      return ts_mk_flow (value, handler, after, success, line);
    }
  if (ts_np_is (node, "OrExpr"))
    {
      const np_node *lhs = NULL, *rhs = NULL;
      size_t i, n = np_node_child_count (node);
      for (i = 0; i < n; i++)
        {
          const np_node *child = np_node_child (node, i);
          if (ts_np_is_expr_node (child))
            {
              if (!lhs)
                lhs = child;
              else
                rhs = child;
            }
        }
      if (lhs && rhs)
        {
          void *left = ts_np_expr (lhs, input);
          void *right = left ? ts_np_expr (rhs, input) : NULL;
          return left && right ? ts_mk_or (left, right, line) : NULL;
        }
      return NULL;
    }
  if (ts_np_is (node, "Call") || ts_np_is (node, "DotCall")
      || ts_np_is (node, "InvokeCall"))
    return ts_np_call (node, input);
  if (ts_np_is (node, "RefExpr"))
    return ts_mk_ref (ts_np_name (node), line);
  if (ts_np_is (node, "IdentExpr"))
    return ts_mk_unary (ts_np_name (node), NULL, line);
  if (ts_np_is (node, "NumberExpr"))
    return (tok = ts_np_first (node, "NUMBER"))
               ? ts_mk_number (ts_np_textdup (tok), line)
               : NULL;
  if (ts_np_is (node, "StringExpr"))
    return (tok = ts_np_first (node, "STRING"))
               ? ts_mk_string (ts_np_textdup (tok), line)
               : NULL;
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
  if (ts_np_is (node, "Stmt") || ts_np_is (node, "Simple"))
    return ts_np_stmt (np_node_child (node, 0), input);
  line = ts_np_line (node, input);
  if (ts_np_is (node, "FunctionDecl"))
    {
      void *params = ts_args_new ();
      const np_node *list = ts_np_first (node, "Parameters");
      for (size_t i = 0; i < np_node_child_count (list); i++)
        {
          const np_node *p = np_node_child (list, i);
          if (ts_np_is (p, "Parameter"))
            params = ts_args_add (
                params,
                ts_mk_argword (ts_np_name (ts_np_first (p, "Name")), line));
        }
      return ts_mk_function (
          ts_np_name (ts_np_first (node, "Name")), params,
          ts_np_program (ts_np_first (node, "Program"), input), line);
    }
  if (ts_np_is (node, "ReturnStmt"))
    return ts_mk_return (ts_np_expr (ts_np_first_expr (node), input), line);
  if (ts_np_is (node, "UseDecl"))
    {
      void *seq = ts_mk_seq ();
      for (size_t i = 0; i < np_node_child_count (node); i++)
        {
          const np_node *ch = np_node_child (node, i);
          if (!ts_np_is (ch, "IDENT"))
            continue;
          char *binding = ts_np_textdup (ch);
          size_t size = strlen (binding) + 7;
          char *quoted = ts_parse_alloc (size);
          if (!quoted)
            return NULL;
          snprintf (quoted, size, "\"std.%s\"",
                    !strcmp (binding, "terminfo") ? "tinfo" : binding);
          void *call = ts_np_builtin ("Import", ts_mk_string (quoted, line),
                                      NULL, line);
          seq = ts_seq_append (seq, ts_mk_const (binding, call, line));
        }
      return seq;
    }
  if (ts_np_is (node, "CompoundAssign"))
    {
      char *name = ts_np_name (ts_np_first (node, "Name"));
      const np_node *op = ts_np_first (node, "compound_op");
      char c = input[np_node_start (op)];
      const char *func = c == '+'   ? "add"
                         : c == '-' ? "sub"
                         : c == '*' ? "mul"
                         : c == '/' ? "div"
                                    : "mod";
      return ts_mk_assign (
          name,
          ts_np_builtin (func, ts_mk_ref (ts_np_dup (name), line),
                         ts_np_expr (ts_np_first_expr (node), input), line),
          line);
    }
  if (ts_np_is (node, "Declaration"))
    {
      char *module = ts_np_name (ts_np_first (node, "Name"));
      char *func = NULL, *symbol = NULL;
      for (size_t i = 0; i < np_node_child_count (node); i++)
        {
          const np_node *ch = np_node_child (node, i);
          if (ts_np_is (ch, "IDENT") || ts_np_is (ch, "WORD"))
            {
              if (!func)
                {
                  char *part = ts_np_textdup (ch);
                  if (!part)
                    return NULL;
                  func = ts_parse_alloc (strlen (part) + 9);
                  if (!func)
                    {
                      ts_oom ();
                      return NULL;
                    }
                  sprintf (func, "Declare:%s", part);
                  ts_parse_free (part);
                }
              else
                symbol = ts_np_textdup (ch);
            }
        }
      void *args = ts_args_add (
          ts_args_new (),
          ts_mk_argexpr (ts_mk_design (7, symbol, NULL, NULL, line)));
      return ts_mk_exprstmt (
          ts_mk_unary (module, ts_mk_callrest (func, args, line), line), line);
    }
  if (ts_np_is (node, "AliasDecl") || ts_np_is (node, "DefineDecl"))
    {
      char *a = NULL, *b = NULL;
      size_t i;
      for (i = 0; i < np_node_child_count (node); i++)
        {
          const np_node *ch = np_node_child (node, i);
          if (ts_np_is (ch, "IDENT") || ts_np_is (ch, "Name"))
            {
              if (!a)
                a = ts_np_name (ch);
              else
                b = ts_np_name (ch);
            }
        }
      return ts_mk_design (ts_np_is (node, "AliasDecl") ? 1 : 2, a, b, NULL,
                           line);
    }
  if (ts_np_is (node, "InterfaceDecl") || ts_np_is (node, "SectionDecl"))
    return ts_mk_design (ts_np_is (node, "InterfaceDecl") ? 3 : 4,
                         ts_np_name (ts_np_first (node, "IDENT")), NULL,
                         ts_np_program (ts_np_first (node, "Program"), input),
                         line);
  if (ts_np_is (node, "ConfigDecl") || ts_np_is (node, "LifetimeDecl"))
    return ts_mk_design (ts_np_is (node, "ConfigDecl") ? 5 : 6,
                         ts_np_name (ts_np_first (node, "Name")), NULL,
                         ts_np_expr (ts_np_first_expr (node), input), line);
  if (ts_np_is (node, "ConstDecl"))
    {
      name = ts_np_first (node, "Name");
      expr = ts_np_first_expr (node);
      if (name && expr)
        {
          char *text = ts_np_name (name);
          void *rhs = text ? ts_np_expr (expr, input) : NULL;
          return text && rhs ? ts_mk_const (text, rhs, line) : NULL;
        }
      return NULL;
    }
  if (ts_np_is (node, "AssignStmt"))
    {
      name = ts_np_first (node, "Name");
      expr = ts_np_first_expr (node);
      if (name && expr)
        {
          char *text = ts_np_name (name);
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
  size_t length = strlen (input);
  const char *parse_input = input;
  size_t parse_length = length;
  if (!length || input[length - 1] != '\n')
    {
      char *normalized = ts_parse_alloc (length + 2);
      if (!normalized)
        {
          np_parser_destroy (parser);
          return NULL;
        }
      memcpy (normalized, input, length);
      normalized[length] = '\n';
      normalized[length + 1] = 0;
      parse_input = normalized;
      parse_length++;
    }
  status = np_parser_parse (parser, parse_input, parse_length);
  if (status != NP_SUCCESS)
    {
      size_t position = np_parser_position (parser);
      ts_ng_set_error (position > length ? length : position,
                       np_parser_error (parser));
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
