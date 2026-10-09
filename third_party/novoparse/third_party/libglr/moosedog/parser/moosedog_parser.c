/* moosedog_parser.c -- hand-written .grm specification parser. */
#include "moosedog_parser.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void
md_spec_init (md_spec_t *spec)
{
  memset (spec, 0, sizeof (*spec));
}

static void
free_option (md_option_t *o)
{
  free (o->long_name);
  free (o->short_name);
  free (o->define);
  free (o->type);
  free (o->def_value);
  free (o->range_min);
  free (o->range_max);
  free (o->description);
  free (o->bind);
}

static void
free_strings (char **v, size_t n)
{
  for (size_t i = 0; i < n; i++)
    free (v[i]);
  free (v);
}

void
md_spec_destroy (md_spec_t *spec)
{
  if (!spec)
    return;
  free (spec->start_rule);
  for (size_t i = 0; i < spec->node_count; i++)
    {
      md_ast_node_t *n = &spec->nodes[i];
      free (n->name);
      free (n->tag);
      free (n->kind);
      free_strings (n->variants, n->variant_count);
      for (size_t f = 0; f < n->field_count; f++)
        {
          free (n->fields[f].name);
          free (n->fields[f].type);
          free (n->fields[f].arity);
        }
      free (n->fields);
    }
  free (spec->nodes);
  for (size_t i = 0; i < spec->token_count; i++)
    {
      free (spec->tokens[i].name);
      free (spec->tokens[i].pattern);
      free (spec->tokens[i].classifier);
    }
  free (spec->tokens);
  for (size_t i = 0; i < spec->rule_count; i++)
    {
      md_rule_t *r = &spec->rules[i];
      free (r->name);
      free (r->returns);
      for (size_t a = 0; a < r->alt_count; a++)
        {
          free_strings (r->alts[a].seq, r->alts[a].seq_count);
          free (r->alts[a].action);
        }
      free (r->alts);
    }
  free (spec->rules);
  {
    md_entrypoint_t *e = &spec->entry;
    free (e->lang_name);
    free (e->lang_type);
    free (e->standard_url);
    free_strings (e->authors, e->author_count);
    free (e->license);
    free_strings (e->revisions, e->revision_count);
    free_strings (e->disambiguators, e->disambiguator_count);
    free_strings (e->rewriters, e->rewriter_count);
    free_strings (e->queries, e->query_count);
    for (size_t i = 0; i < e->cfg_count; i++)
      {
        free (e->cfg_keys[i]);
        free (e->cfg_values[i]);
      }
    free (e->cfg_keys);
    free (e->cfg_values);
    for (size_t i = 0; i < e->atn_count; i++)
      {
        free (e->atn_keys[i]);
        free (e->atn_values[i]);
      }
    free (e->atn_keys);
    free (e->atn_values);
    for (size_t i = 0; i < e->option_count; i++)
      free_option (&e->options[i]);
    free (e->options);
    free_strings (e->pipeline, e->pipeline_count);
  }
  md_spec_init (spec);
}

/* ================= tokenizer ================= */

typedef enum
{
  T_EOF,
  T_IDENT,
  T_STRING,
  T_NUMBER,
  T_LBRACE,
  T_RBRACE,
  T_LPAREN,
  T_RPAREN,
  T_LBRACKET,
  T_RBRACKET,
  T_EQUALS,
  T_COMMA,
  T_SEMI,
  T_ARROW,
  T_AT
} tok_kind_t;

typedef struct
{
  tok_kind_t kind;
  char *text; /* owned for IDENT/STRING/NUMBER/AT */
  int line;
} token_t;

typedef struct
{
  const char *src;
  size_t len;
  size_t pos;
  int line;
  const char *origin;
  token_t saved;
  int has_saved;
  md_error_t *err;
} lexer_t;

static void
tok_free (token_t *t)
{
  free (t->text);
  t->text = NULL;
}

static char *
xstrndup (const char *s, size_t n)
{
  char *o = malloc (n + 1);
  if (o)
    {
      memcpy (o, s, n);
      o[n] = '\0';
    }
  return o;
}

static char *
xstrdup (const char *s)
{
  if (!s)
    return NULL;
  return xstrndup (s, strlen (s));
}

static void
lex_error (lexer_t *lx, const char *msg)
{
  if (lx->err && !lx->err->message[0])
    {
      snprintf (lx->err->message, sizeof (lx->err->message), "%s: %s at line %d",
                lx->origin ? lx->origin : "<input>", msg, lx->line);
      lx->err->line = lx->line;
    }
}

static void
skip_ws_comments (lexer_t *lx)
{
  while (lx->pos < lx->len)
    {
      char c = lx->src[lx->pos];
      if (c == '\n')
        {
          lx->line++;
          lx->pos++;
        }
      else if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f')
        lx->pos++;
      else if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '*')
        {
          lx->pos += 2;
          while (lx->pos + 1 < lx->len
                 && !(lx->src[lx->pos] == '*' && lx->src[lx->pos + 1] == '/'))
            {
              if (lx->src[lx->pos] == '\n')
                lx->line++;
              lx->pos++;
            }
          if (lx->pos + 1 < lx->len)
            lx->pos += 2;
        }
      else if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '/')
        {
          while (lx->pos < lx->len && lx->src[lx->pos] != '\n')
            lx->pos++;
        }
      else
        break;
    }
}

/* Decode C-style escapes inside a double-quoted string but preserve the
 * raw spelling for regex patterns: we keep backslashes verbatim except
 * for \" and \\ and \n-style line continuations which we normalize. */
static token_t
lex_one (lexer_t *lx)
{
  token_t t;
  char c = 0;
  memset (&t, 0, sizeof (t));
retry:
  memset (&t, 0, sizeof (t));
  for (;;)
    {
      skip_ws_comments (lx);
      t.line = lx->line;
      if (lx->pos >= lx->len)
        {
          t.kind = T_EOF;
          return t;
        }
      {
        char k = lx->src[lx->pos];
        /* Stray punctuation outside any grammar construct: skip without
         * recursion (a long run must not consume stack). */
        if (k == '#' || k == '*' || k == '.' || k == '%'
            || k == '+' || k == '/' || k == '!' || k == '?' || k == '|'
            || k == '&' || k == '^' || k == '~' || k == ':' || k == '\\'
            || k == '\'' || k == '<' || k == '>' || k == '`')
          {
            lx->pos++;
            continue;
          }
      }
      break;
    }
  c = lx->src[lx->pos];
  if (c == '@')
    {
      size_t s = ++lx->pos;
      while (lx->pos < lx->len
             && (isalnum ((unsigned char) lx->src[lx->pos])
                 || lx->src[lx->pos] == '_'))
        lx->pos++;
      t.kind = T_AT;
      t.text = xstrndup (lx->src + s, lx->pos - s);
      return t;
    }
  if (c == '"')
    {
      size_t cap = 64, n = 0;
      char *buf = malloc (cap);
      if (!buf)
        {
          t.kind = T_EOF;
          return t;
        }
      lx->pos++; /* opening quote */
      while (lx->pos < lx->len && lx->src[lx->pos] != '"')
        {
          char ch = lx->src[lx->pos];
          if (ch == '\n')
            {
              lx->line++;
              if (n + 1 >= cap)
                {
                  cap *= 2;
                  char *nb = realloc (buf, cap);
                  if (!nb)
                    {
                      free (buf);
                      t.kind = T_EOF;
                      return t;
                    }
                  buf = nb;
                }
              buf[n++] = ch;
              lx->pos++;
            }
          else if (ch == '\\' && lx->pos + 1 < lx->len)
            {
              if (n + 2 >= cap)
                {
                  cap *= 2;
                  char *nb = realloc (buf, cap);
                  if (!nb)
                    {
                      free (buf);
                      t.kind = T_EOF;
                      return t;
                    }
                  buf = nb;
                }
              buf[n++] = ch;
              buf[n++] = lx->src[lx->pos + 1];
              lx->pos += 2;
            }
          else
            {
              if (n + 1 >= cap)
                {
                  cap *= 2;
                  char *nb = realloc (buf, cap);
                  if (!nb)
                    {
                      free (buf);
                      t.kind = T_EOF;
                      return t;
                    }
                  buf = nb;
                }
              buf[n++] = ch;
              lx->pos++;
            }
        }
      if (lx->pos < lx->len)
        lx->pos++; /* closing quote */
      if (n + 1 >= cap)
        {
          char *nb = realloc (buf, n + 1);
          if (nb)
            buf = nb;
        }
      buf[n] = '\0';
      t.kind = T_STRING;
      t.text = buf;
      return t;
    }
  if (isalpha ((unsigned char) c) || c == '_' || c == '$'
      || ((unsigned char) c >= 0x80))
    {
      size_t s = lx->pos;
      while (lx->pos < lx->len
             && (isalnum ((unsigned char) lx->src[lx->pos])
                 || lx->src[lx->pos] == '_' || lx->src[lx->pos] == '$'
                 || lx->src[lx->pos] == '.' || lx->src[lx->pos] == '/'
                 || lx->src[lx->pos] == '-' || lx->src[lx->pos] == '+'
                 || lx->src[lx->pos] == ':' || lx->src[lx->pos] == '%'
                 || ((unsigned char) lx->src[lx->pos] >= 0x80)))
        {
          /* Stop before structural characters. */
          char k = lx->src[lx->pos];
          if (k == '{' || k == '}' || k == '(' || k == ')' || k == '['
              || k == ']' || k == '=' || k == ',' || k == ';' || k == '"'
              || k == '@')
            break;
          lx->pos++;
        }
      /* Identifiers with trailing punctuation (e.g. "false," handled by
       * comma token) -- trim trailing . / - etc. handled above loosely;
       * keep it simple: raw slice. */
      t.kind = T_IDENT;
      t.text = xstrndup (lx->src + s, lx->pos - s);
      /* A lone "-" "+" "." etc. is not an identifier; re-lex as needed. */
      if (t.text && strlen (t.text) == 0)
        {
          free (t.text);
          t.text = NULL;
          lx->pos = s + 1;
          goto retry;
        }
      return t;
    }
  if (isdigit ((unsigned char) c)
      || (c == '-' && lx->pos + 1 < lx->len
          && isdigit ((unsigned char) lx->src[lx->pos + 1])))
    {
      size_t s = lx->pos;
      if (c == '-')
        lx->pos++;
      while (lx->pos < lx->len
             && (isalnum ((unsigned char) lx->src[lx->pos])
                 || lx->src[lx->pos] == '.' || lx->src[lx->pos] == '_'
                 || lx->src[lx->pos] == 'x' || lx->src[lx->pos] == 'X'))
        lx->pos++;
      t.kind = T_NUMBER;
      t.text = xstrndup (lx->src + s, lx->pos - s);
      return t;
    }
  lx->pos++;
  switch (c)
    {
    case '{':
      t.kind = T_LBRACE;
      break;
    case '}':
      t.kind = T_RBRACE;
      break;
    case '(':
      t.kind = T_LPAREN;
      break;
    case ')':
      t.kind = T_RPAREN;
      break;
    case '[':
      t.kind = T_LBRACKET;
      break;
    case ']':
      t.kind = T_RBRACKET;
      break;
    case '=':
      t.kind = T_EQUALS;
      break;
    case ',':
      t.kind = T_COMMA;
      break;
    case ';':
      t.kind = T_SEMI;
      break;
    case '-':
      if (lx->pos < lx->len && lx->src[lx->pos] == '>')
        {
          lx->pos++;
          t.kind = T_ARROW;
        }
      else
        {
          t.kind = T_IDENT;
          t.text = xstrdup ("-");
        }
      break;
    default:
      /* Skip stray characters (e.g. '*', '#', '$' alone) and continue. */
      goto retry;
    }
  return t;
}

static token_t
next_tok (lexer_t *lx)
{
  if (lx->has_saved)
    {
      lx->has_saved = 0;
      return lx->saved;
    }
  return lex_one (lx);
}

static void
unget_tok (lexer_t *lx, token_t t)
{
  if (lx->has_saved)
    tok_free (&lx->saved);
  lx->saved = t;
  lx->has_saved = 1;
}

static int
expect (lexer_t *lx, tok_kind_t k, token_t *out)
{
  token_t t = next_tok (lx);
  if (t.kind != k)
    {
      char msg[128];
      snprintf (msg, sizeof (msg), "expected token %d, got %d", (int) k,
                (int) t.kind);
      lex_error (lx, msg);
      tok_free (&t);
      return -1;
    }
  if (out)
    *out = t;
  else
    tok_free (&t);
  return 0;
}

/* Parse a value and return its canonical string form:
 *  STRING -> contents
 *  IDENT [ ( args ) ] -> Name(args...) as written, e.g. LanguageType("DataExchange")
 *  NUMBER -> digits
 *  { ... } handled by callers (lists).
 *  Nested braces inside call args (e.g. Allocators("x")) are consumed. */
static char *
parse_value (lexer_t *lx)
{
  token_t t = next_tok (lx);
  if (t.kind == T_STRING)
    {
      char *v = t.text;
      t.text = NULL;
      tok_free (&t);
      return v;
    }
  if (t.kind == T_NUMBER || t.kind == T_IDENT)
    {
      char *head = t.text;
      t.text = NULL;
      tok_free (&t);
      token_t n = next_tok (lx);
      if (n.kind == T_LPAREN)
        {
          /* Collect balanced call text: Name(arg, arg, ...) */
          size_t cap = strlen (head) + 64, len = strlen (head);
          char *buf = malloc (cap);
          if (!buf)
            {
              tok_free (&n);
              free (head);
              return NULL;
            }
          memcpy (buf, head, len);
          buf[len++] = '(';
          int depth = 1;
          int first = 1;
          while (depth > 0)
            {
              token_t a = next_tok (lx);
              if (a.kind == T_EOF)
                {
                  tok_free (&a);
                  break;
                }
              if (a.kind == T_LPAREN)
                depth++;
              if (a.kind == T_RPAREN)
                {
                  depth--;
                  if (depth == 0)
                    {
                      tok_free (&a);
                      break;
                    }
                }
              const char *txt = "";
              switch (a.kind)
                {
                case T_STRING:
                  txt = NULL; /* handled below */
                  break;
                case T_LBRACE:
                  txt = "{";
                  break;
                case T_RBRACE:
                  txt = "}";
                  break;
                case T_LPAREN:
                  txt = "(";
                  break;
                case T_RPAREN:
                  txt = ")";
                  break;
                case T_LBRACKET:
                  txt = "[";
                  break;
                case T_RBRACKET:
                  txt = "]";
                  break;
                case T_EQUALS:
                  txt = "=";
                  break;
                case T_COMMA:
                  txt = ",";
                  break;
                case T_SEMI:
                  txt = ";";
                  break;
                case T_ARROW:
                  txt = "->";
                  break;
                default:
                  break;
                }
              char piece[1024];
              if (a.kind == T_STRING)
                snprintf (piece, sizeof (piece), "\"%s\"", a.text ? a.text : "");
              else if (a.kind == T_IDENT || a.kind == T_NUMBER || a.kind == T_AT)
                snprintf (piece, sizeof (piece), "%s%s",
                          first ? "" : " ",
                          a.text ? a.text : "");
              else
                snprintf (piece, sizeof (piece), "%s", txt);
              if (a.kind == T_COMMA)
                snprintf (piece, sizeof (piece), ",");
              size_t pl = strlen (piece);
              while (len + pl + 4 >= cap)
                {
                  cap *= 2;
                  char *nb = realloc (buf, cap);
                  if (!nb)
                    {
                      free (buf);
                      tok_free (&a);
                      free (head);
                      tok_free (&n);
                      return NULL;
                    }
                  buf = nb;
                }
              memcpy (buf + len, piece, pl);
              len += pl;
              if (a.kind != T_COMMA && a.kind != T_LPAREN)
                {
                  /* separate tokens with space except punctuation */
                  if (len + 1 < cap)
                    buf[len++] = ' ';
                }
              first = 0;
              tok_free (&a);
            }
          /* trim trailing space */
          while (len > 0 && buf[len - 1] == ' ')
            len--;
          if (len + 2 < cap)
            {
              buf[len++] = ')';
              buf[len] = '\0';
            }
          else
            buf[len] = '\0';
          tok_free (&n);
          free (head);
          return buf;
        }
      unget_tok (lx, n);
      return head;
    }
  if (t.kind == T_LBRACE)
    {
      /* Anonymous set value: join inner values with '|'. Rare; keep marker. */
      tok_free (&t);
      free (parse_value (lx));
      return xstrdup ("{}");
    }
  lex_error (lx, "expected a value");
  tok_free (&t);
  return NULL;
}

/* Skip a balanced block {...} assuming the opening brace was consumed. */
static int
skip_block (lexer_t *lx)
{
  int depth = 1;
  while (depth > 0)
    {
      token_t t = next_tok (lx);
      if (t.kind == T_EOF)
        {
          tok_free (&t);
          return -1;
        }
      if (t.kind == T_LBRACE)
        depth++;
      else if (t.kind == T_RBRACE)
        depth--;
      tok_free (&t);
    }
  return 0;
}

/* Parse { "a", "b", ... } or { Ident(...), ... } into a string list. */
static int
parse_string_list_block (lexer_t *lx, char ***out, size_t *out_n)
{
  *out = NULL;
  *out_n = 0;
  size_t cap = 0;
  if (expect (lx, T_LBRACE, NULL) != 0)
    return -1;
  for (;;)
    {
      token_t t = next_tok (lx);
      if (t.kind == T_RBRACE)
        {
          tok_free (&t);
          break;
        }
      if (t.kind == T_COMMA)
        {
          tok_free (&t);
          continue;
        }
      unget_tok (lx, t);
      char *v = parse_value (lx);
      if (!v)
        return -1;
      if (*out_n + 1 > cap)
        {
          cap = cap ? cap * 2 : 8;
          char **nb = realloc (*out, cap * sizeof (*nb));
          if (!nb)
            {
              free (v);
              return -1;
            }
          *out = nb;
        }
      (*out)[(*out_n)++] = v;
      token_t s = next_tok (lx);
      if (s.kind == T_COMMA)
        tok_free (&s);
      else if (s.kind == T_RBRACE)
        {
          tok_free (&s);
          break;
        }
      else
        unget_tok (lx, s);
    }
  return 0;
}

static int md_parse_entrypoint (lexer_t *, md_spec_t *);
static int md_parse_ast (lexer_t *, md_spec_t *);
static int md_parse_lexical (lexer_t *, md_spec_t *);
static int md_parse_syntactic (lexer_t *, md_spec_t *);

/* ================= Entrypoint ================= */

static int
entry_add_cfg (md_spec_t *spec, char *k, char *v)
{
  md_entrypoint_t *e = &spec->entry;
  char **nk = realloc (e->cfg_keys, (e->cfg_count + 1) * sizeof (*nk));
  char **nv = realloc (e->cfg_values, (e->cfg_count + 1) * sizeof (*nv));
  if (!nk || !nv)
    {
      free (k);
      free (v);
      return -1;
    }
  e->cfg_keys = nk;
  e->cfg_values = nv;
  e->cfg_keys[e->cfg_count] = k;
  e->cfg_values[e->cfg_count] = v;
  e->cfg_count++;
  return 0;
}

static int
entry_add_atn (md_spec_t *spec, char *k, char *v)
{
  md_entrypoint_t *e = &spec->entry;
  char **nk = realloc (e->atn_keys, (e->atn_count + 1) * sizeof (*nk));
  char **nv = realloc (e->atn_values, (e->atn_count + 1) * sizeof (*nv));
  if (!nk || !nv)
    {
      free (k);
      free (v);
      return -1;
    }
  e->atn_keys = nk;
  e->atn_values = nv;
  e->atn_keys[e->atn_count] = k;
  e->atn_values[e->atn_count] = v;
  e->atn_count++;
  return 0;
}

static int
entry_add_pipeline (md_spec_t *spec, const char *name)
{
  md_entrypoint_t *e = &spec->entry;
  char **np = realloc (e->pipeline, (e->pipeline_count + 1) * sizeof (*np));
  if (!np)
    return -1;
  e->pipeline = np;
  e->pipeline[e->pipeline_count] = xstrdup (name);
  if (!e->pipeline[e->pipeline_count])
    return -1;
  e->pipeline_count++;
  return 0;
}

/* Parse Config { K = V ... } or ATN { ... } generically. */
static int
parse_kv_block (lexer_t *lx, md_spec_t *spec, int is_atn)
{
  if (expect (lx, T_LBRACE, NULL) != 0)
    return -1;
  for (;;)
    {
      token_t t = next_tok (lx);
      if (t.kind == T_RBRACE)
        {
          tok_free (&t);
          break;
        }
      if (t.kind == T_EOF)
        {
          tok_free (&t);
          lex_error (lx, "unexpected end in Config/ATN block");
          return -1;
        }
      if (t.kind != T_IDENT)
        {
          tok_free (&t);
          continue;
        }
      char *key = t.text;
      t.text = NULL;
      tok_free (&t);
      token_t e = next_tok (lx);
      if (e.kind != T_EQUALS)
        {
          /* Might be a bare flag or nested block; skip gracefully. */
          if (e.kind == T_LBRACE)
            {
              tok_free (&e);
              free (key);
              if (skip_block (lx) != 0)
                return -1;
              continue;
            }
          tok_free (&e);
          free (key);
          continue;
        }
      tok_free (&e);
      char *val = parse_value (lx);
      if (!val)
        {
          free (key);
          return -1;
        }
      if (is_atn)
        {
          if (entry_add_atn (spec, key, val) != 0)
            return -1;
        }
      else
        {
          if (entry_add_cfg (spec, key, val) != 0)
            return -1;
        }
    }
  return 0;
}

static int
parse_option_block (lexer_t *lx, md_spec_t *spec, const char *long_name)
{
  md_option_t opt;
  memset (&opt, 0, sizeof (opt));
  opt.long_name = xstrdup (long_name);
  if (!opt.long_name)
    return -1;
  if (expect (lx, T_LBRACE, NULL) != 0)
    {
      free_option (&opt);
      return -1;
    }
  for (;;)
    {
      token_t t = next_tok (lx);
      if (t.kind == T_RBRACE)
        {
          tok_free (&t);
          break;
        }
      if (t.kind == T_EOF)
        {
          tok_free (&t);
          free_option (&opt);
          return -1;
        }
      if (t.kind != T_IDENT)
        {
          tok_free (&t);
          continue;
        }
      char *key = t.text;
      t.text = NULL;
      tok_free (&t);
      token_t e = next_tok (lx);
      if (e.kind != T_EQUALS)
        {
          tok_free (&e);
          free (key);
          continue;
        }
      tok_free (&e);
      char *val = parse_value (lx);
      if (!val)
        {
          free (key);
          free_option (&opt);
          return -1;
        }
      if (!strcmp (key, "Short"))
        {
          free (opt.short_name);
          opt.short_name = val;
        }
      else if (!strcmp (key, "Define"))
        {
          free (opt.define);
          opt.define = val;
        }
      else if (!strcmp (key, "Type"))
        {
          free (opt.type);
          opt.type = val;
        }
      else if (!strcmp (key, "Default"))
        {
          free (opt.def_value);
          opt.def_value = val;
        }
      else if (!strcmp (key, "Description"))
        {
          free (opt.description);
          opt.description = val;
        }
      else if (!strcmp (key, "Bind"))
        {
          free (opt.bind);
          opt.bind = val;
        }
      else if (!strcmp (key, "BindInvert"))
        {
          opt.bind_invert = (!strcmp (val, "true") || !strcmp (val, "Yes")
                             || !strcmp (val, "1"));
          free (val);
        }
      else if (!strcmp (key, "Range"))
        {
          /* IntRange(lo, hi): value looks like "IntRange( 1 , 256 )" */
          char *p = strchr (val, '(');
          if (p)
            {
              long lo = 0, hi = 0;
              if (sscanf (p, "(%ld ,%ld", &lo, &hi) == 2
                  || sscanf (p, "(%ld,%ld", &lo, &hi) == 2)
                {
                  char b[64];
                  snprintf (b, sizeof (b), "%ld", lo);
                  opt.range_min = xstrdup (b);
                  snprintf (b, sizeof (b), "%ld", hi);
                  opt.range_max = xstrdup (b);
                }
            }
          free (val);
        }
      else
        free (val);
      free (key);
    }
  {
    md_entrypoint_t *en = &spec->entry;
    md_option_t *no = realloc (en->options,
                              (en->option_count + 1) * sizeof (*no));
    if (!no)
      {
        free_option (&opt);
        return -1;
      }
    en->options = no;
    en->options[en->option_count++] = opt;
  }
  return 0;
}

static int
parse_optparse (lexer_t *lx, md_spec_t *spec)
{
  if (expect (lx, T_LBRACE, NULL) != 0)
    return -1;
  for (;;)
    {
      token_t t = next_tok (lx);
      if (t.kind == T_RBRACE)
        {
          tok_free (&t);
          break;
        }
      if (t.kind == T_EOF)
        {
          tok_free (&t);
          return -1;
        }
      if (t.kind == T_IDENT && t.text && !strcmp (t.text, "Option"))
        {
          tok_free (&t);
          if (expect (lx, T_LPAREN, NULL) != 0)
            return -1;
          token_t nm = next_tok (lx);
          char *longn = NULL;
          if (nm.kind == T_STRING || nm.kind == T_IDENT)
            longn = xstrdup (nm.text ? nm.text : "");
          tok_free (&nm);
          if (expect (lx, T_RPAREN, NULL) != 0)
            {
              free (longn);
              return -1;
            }
          if (!longn)
            return -1;
          int rc = parse_option_block (lx, spec, longn);
          free (longn);
          if (rc != 0)
            return -1;
        }
      else if (t.kind == T_LBRACE)
        {
          tok_free (&t);
          if (skip_block (lx) != 0)
            return -1;
        }
      else
        tok_free (&t);
    }
  return 0;
}

static int
parse_lang_info (lexer_t *lx, md_spec_t *spec)
{
  if (expect (lx, T_LBRACE, NULL) != 0)
    return -1;
  for (;;)
    {
      token_t t = next_tok (lx);
      if (t.kind == T_RBRACE)
        {
          tok_free (&t);
          break;
        }
      if (t.kind != T_IDENT)
        {
          tok_free (&t);
          if (t.kind == T_EOF)
            return -1;
          continue;
        }
      char *key = t.text;
      t.text = NULL;
      tok_free (&t);
      token_t e = next_tok (lx);
      if (e.kind != T_EQUALS)
        {
          tok_free (&e);
          free (key);
          continue;
        }
      tok_free (&e);
      char *val = parse_value (lx);
      if (!val)
        {
          free (key);
          return -1;
        }
      if (!strcmp (key, "Name"))
        {
          free (spec->entry.lang_name);
          spec->entry.lang_name = val;
        }
      else if (!strcmp (key, "Type"))
        {
          free (spec->entry.lang_type);
          spec->entry.lang_type = val;
        }
      else if (!strcmp (key, "Standard"))
        {
          free (spec->entry.standard_url);
          spec->entry.standard_url = val;
        }
      else
        free (val);
      free (key);
    }
  return 0;
}

static int
parse_grammar_info (lexer_t *lx, md_spec_t *spec)
{
  if (expect (lx, T_LBRACE, NULL) != 0)
    return -1;
  for (;;)
    {
      token_t t = next_tok (lx);
      if (t.kind == T_RBRACE)
        {
          tok_free (&t);
          break;
        }
      if (t.kind != T_IDENT)
        {
          tok_free (&t);
          if (t.kind == T_EOF)
            return -1;
          continue;
        }
      char *key = t.text;
      t.text = NULL;
      tok_free (&t);
      token_t e = next_tok (lx);
      if (e.kind != T_EQUALS)
        {
          tok_free (&e);
          free (key);
          continue;
        }
      tok_free (&e);
      if (!strcmp (key, "Authors") || !strcmp (key, "RevisionHistory"))
        {
          char **list = NULL;
          size_t n = 0;
          if (parse_string_list_block (lx, &list, &n) != 0)
            {
              free (key);
              return -1;
            }
          if (!strcmp (key, "Authors"))
            {
              free_strings (spec->entry.authors, spec->entry.author_count);
              spec->entry.authors = list;
              spec->entry.author_count = n;
            }
          else
            {
              free_strings (spec->entry.revisions, spec->entry.revision_count);
              spec->entry.revisions = list;
              spec->entry.revision_count = n;
            }
        }
      else
        {
          char *val = parse_value (lx);
          if (!val)
            {
              free (key);
              return -1;
            }
          if (!strcmp (key, "License"))
            {
              free (spec->entry.license);
              spec->entry.license = val;
            }
          else
            free (val);
        }
      free (key);
    }
  return 0;
}

static int
md_parse_entrypoint (lexer_t *lx, md_spec_t *spec)
{
  if (expect (lx, T_LBRACE, NULL) != 0)
    return -1;
  for (;;)
    {
      token_t t = next_tok (lx);
      if (t.kind == T_RBRACE)
        {
          tok_free (&t);
          break;
        }
      if (t.kind == T_EOF)
        {
          tok_free (&t);
          lex_error (lx, "unexpected end in Entrypoint");
          return -1;
        }
      if (t.kind == T_AT)
        {
          /* @ifndef SYM ... @endif: parse inner content unconditionally. */
          char *directive = t.text;
          t.text = NULL;
          tok_free (&t);
          if (directive
              && (!strcmp (directive, "ifndef") || !strcmp (directive, "ifdef")
                  || !strcmp (directive, "if")))
            {
              token_t sym = next_tok (lx);
              tok_free (&sym);
            }
          else if (directive && !strcmp (directive, "endif"))
            {
              /* nothing */
            }
          else if (directive && !strcmp (directive, "else"))
            {
              /* nothing */
            }
          else
            {
              /* Unknown @directive: skip one balanced group if present. */
              token_t n = next_tok (lx);
              unget_tok (lx, n);
            }
          free (directive);
          continue;
        }
      if (t.kind != T_IDENT)
        {
          tok_free (&t);
          continue;
        }
      char *name = t.text;
      t.text = NULL;
      tok_free (&t);
      token_t n = next_tok (lx);
      if (n.kind == T_LBRACE)
        {
          tok_free (&n);
          unget_tok (lx, n);
          int rc = -1;
          if (!strcmp (name, "LanguageInfo"))
            rc = parse_lang_info (lx, spec);
          else if (!strcmp (name, "GrammarInfo"))
            rc = parse_grammar_info (lx, spec);
          else if (!strcmp (name, "Config"))
            rc = parse_kv_block (lx, spec, 0);
          else if (!strcmp (name, "ATN"))
            rc = parse_kv_block (lx, spec, 1);
          else if (!strcmp (name, "Optparse"))
            rc = parse_optparse (lx, spec);
          else
            {
              /* Unknown block: skip. */
              if (expect (lx, T_LBRACE, NULL) == 0)
                rc = skip_block (lx);
              else
                rc = -1;
            }
          free (name);
          if (rc != 0)
            return -1;
          continue;
        }
      if (n.kind == T_EQUALS)
        {
          tok_free (&n);
          /* Disambiguators = {...} / Rewriters / Queries */
          if (!strcmp (name, "Disambiguators") || !strcmp (name, "Rewriters")
              || !strcmp (name, "Queries"))
            {
              char **list = NULL;
              size_t count = 0;
              if (parse_string_list_block (lx, &list, &count) != 0)
                {
                  free (name);
                  return -1;
                }
              if (!strcmp (name, "Disambiguators"))
                {
                  free_strings (spec->entry.disambiguators,
                                spec->entry.disambiguator_count);
                  spec->entry.disambiguators = list;
                  spec->entry.disambiguator_count = count;
                }
              else if (!strcmp (name, "Rewriters"))
                {
                  free_strings (spec->entry.rewriters,
                                spec->entry.rewriter_count);
                  spec->entry.rewriters = list;
                  spec->entry.rewriter_count = count;
                }
              else
                {
                  free_strings (spec->entry.queries, spec->entry.query_count);
                  spec->entry.queries = list;
                  spec->entry.query_count = count;
                }
            }
          else
            {
              char *val = parse_value (lx);
              free (val);
            }
          free (name);
          continue;
        }
      if (n.kind == T_LPAREN)
        {
          tok_free (&n);
          /* Pipeline call Name(args): consume to matching ')'. */
          int depth = 1;
          while (depth > 0)
            {
              token_t a = next_tok (lx);
              if (a.kind == T_EOF)
                {
                  tok_free (&a);
                  free (name);
                  return -1;
                }
              if (a.kind == T_LPAREN)
                depth++;
              else if (a.kind == T_RPAREN)
                depth--;
              tok_free (&a);
            }
          entry_add_pipeline (spec, name);
          free (name);
          continue;
        }
      /* Bare identifier (pipeline step without parens?) */
      tok_free (&n);
      entry_add_pipeline (spec, name);
      free (name);
    }
  return 0;
}

/* ================= AST ================= */

static int
md_parse_ast (lexer_t *lx, md_spec_t *spec)
{
  if (expect (lx, T_LBRACE, NULL) != 0)
    return -1;
  for (;;)
    {
      token_t t = next_tok (lx);
      if (t.kind == T_RBRACE)
        {
          tok_free (&t);
          break;
        }
      if (t.kind == T_EOF)
        {
          tok_free (&t);
          return -1;
        }
      if (t.kind != T_IDENT || !t.text || strcmp (t.text, "Node"))
        {
          tok_free (&t);
          continue;
        }
      tok_free (&t);
      if (expect (lx, T_LPAREN, NULL) != 0)
        return -1;
      token_t nm = next_tok (lx);
      if (nm.kind != T_IDENT && nm.kind != T_STRING)
        {
          tok_free (&nm);
          lex_error (lx, "expected node name");
          return -1;
        }
      char *node_name = xstrdup (nm.text ? nm.text : "");
      tok_free (&nm);
      if (expect (lx, T_RPAREN, NULL) != 0)
        {
          free (node_name);
          return -1;
        }
      if (expect (lx, T_LBRACE, NULL) != 0)
        {
          free (node_name);
          return -1;
        }
      md_ast_node_t node;
      memset (&node, 0, sizeof (node));
      node.name = node_name;
      for (;;)
        {
          token_t f = next_tok (lx);
          if (f.kind == T_RBRACE)
            {
              tok_free (&f);
              break;
            }
          if (f.kind != T_IDENT)
            {
              tok_free (&f);
              if (f.kind == T_EOF)
                return -1;
              continue;
            }
          char *key = f.text;
          f.text = NULL;
          tok_free (&f);
          token_t e = next_tok (lx);
          if (e.kind != T_EQUALS)
            {
              tok_free (&e);
              free (key);
              continue;
            }
          tok_free (&e);
          if (!strcmp (key, "Tag") || !strcmp (key, "Kind"))
            {
              char *val = parse_value (lx);
              if (!val)
                {
                  free (key);
                  return -1;
                }
              if (!strcmp (key, "Tag"))
                node.tag = val;
              else
                node.kind = val;
            }
          else if (!strcmp (key, "Variants"))
            {
              char **list = NULL;
              size_t n = 0;
              if (parse_string_list_block (lx, &list, &n) != 0)
                {
                  free (key);
                  return -1;
                }
              node.variants = list;
              node.variant_count = n;
            }
          else if (!strcmp (key, "Fields"))
            {
              if (expect (lx, T_LBRACE, NULL) != 0)
                {
                  free (key);
                  return -1;
                }
              for (;;)
                {
                  token_t fl = next_tok (lx);
                  if (fl.kind == T_RBRACE)
                    {
                      tok_free (&fl);
                      break;
                    }
                  if (fl.kind != T_IDENT || !fl.text
                      || strcmp (fl.text, "Field"))
                    {
                      tok_free (&fl);
                      if (fl.kind == T_EOF)
                        {
                          free (key);
                          return -1;
                        }
                      continue;
                    }
                  tok_free (&fl);
                  if (expect (lx, T_LPAREN, NULL) != 0)
                    {
                      free (key);
                      return -1;
                    }
                  token_t fn = next_tok (lx);
                  char *fname = xstrdup (fn.text ? fn.text : "");
                  tok_free (&fn);
                  if (expect (lx, T_RPAREN, NULL) != 0)
                    {
                      free (fname);
                      free (key);
                      return -1;
                    }
                  if (expect (lx, T_LBRACE, NULL) != 0)
                    {
                      free (fname);
                      free (key);
                      return -1;
                    }
                  md_ast_field_t field;
                  memset (&field, 0, sizeof (field));
                  field.name = fname;
                  for (;;)
                    {
                      token_t fk = next_tok (lx);
                      if (fk.kind == T_RBRACE)
                        {
                          tok_free (&fk);
                          break;
                        }
                      if (fk.kind != T_IDENT)
                        {
                          tok_free (&fk);
                          continue;
                        }
                      char *fkey = fk.text;
                      fk.text = NULL;
                      tok_free (&fk);
                      token_t fe = next_tok (lx);
                      if (fe.kind != T_EQUALS)
                        {
                          tok_free (&fe);
                          free (fkey);
                          continue;
                        }
                      tok_free (&fe);
                      char *fval = parse_value (lx);
                      if (!strcmp (fkey, "Type"))
                        field.type = fval;
                      else if (!strcmp (fkey, "Arity"))
                        field.arity = fval;
                      else
                        free (fval);
                      free (fkey);
                    }
                  {
                    md_ast_field_t *nf = realloc (
                        node.fields, (node.field_count + 1) * sizeof (*nf));
                    if (!nf)
                      {
                        free (key);
                        return -1;
                      }
                    node.fields = nf;
                    node.fields[node.field_count++] = field;
                  }
                }
            }
          else
            {
              char *val = parse_value (lx);
              free (val);
            }
          free (key);
        }
      {
        md_ast_node_t *nn = realloc (spec->nodes,
                                    (spec->node_count + 1) * sizeof (*nn));
        if (!nn)
          return -1;
        spec->nodes = nn;
        spec->nodes[spec->node_count++] = node;
      }
    }
  return 0;
}

/* ================= Lexical ================= */

static int
parse_token_body (lexer_t *lx, md_token_def_t *def)
{
  if (expect (lx, T_LBRACE, NULL) != 0)
    return -1;
  for (;;)
    {
      token_t t = next_tok (lx);
      if (t.kind == T_RBRACE)
        {
          tok_free (&t);
          break;
        }
      if (t.kind != T_IDENT)
        {
          tok_free (&t);
          if (t.kind == T_EOF)
            return -1;
          continue;
        }
      char *key = t.text;
      t.text = NULL;
      tok_free (&t);
      token_t e = next_tok (lx);
      if (e.kind != T_EQUALS)
        {
          tok_free (&e);
          free (key);
          continue;
        }
      tok_free (&e);
      char *val = parse_value (lx);
      if (!val)
        {
          free (key);
          return -1;
        }
      if (!strcmp (key, "Pattern"))
        {
          free (def->pattern);
          def->pattern = val;
        }
      else if (!strcmp (key, "Classifier"))
        {
          free (def->classifier);
          def->classifier = val;
        }
      else if (!strcmp (key, "Literal"))
        {
          def->literal = (!strcmp (val, "Yes") || !strcmp (val, "true")
                          || !strcmp (val, "1"));
          free (val);
        }
      else
        free (val);
      free (key);
    }
  return 0;
}

static int
md_parse_lexical (lexer_t *lx, md_spec_t *spec)
{
  if (expect (lx, T_LBRACE, NULL) != 0)
    return -1;
  for (;;)
    {
      token_t t = next_tok (lx);
      if (t.kind == T_RBRACE)
        {
          tok_free (&t);
          break;
        }
      if (t.kind == T_EOF)
        {
          tok_free (&t);
          return -1;
        }
      if (t.kind != T_IDENT
          || (strcmp (t.text, "Token") && strcmp (t.text, "Skip")))
        {
          tok_free (&t);
          continue;
        }
      int is_skip = !strcmp (t.text, "Skip");
      tok_free (&t);
      if (expect (lx, T_LPAREN, NULL) != 0)
        return -1;
      token_t nm = next_tok (lx);
      char *name = xstrdup (nm.text ? nm.text : "");
      tok_free (&nm);
      if (expect (lx, T_RPAREN, NULL) != 0)
        {
          free (name);
          return -1;
        }
      md_token_def_t def;
      memset (&def, 0, sizeof (def));
      def.name = name;
      def.is_skip = is_skip;
      if (parse_token_body (lx, &def) != 0)
        {
          free (def.name);
          free (def.pattern);
          free (def.classifier);
          return -1;
        }
      {
        md_token_def_t *nt = realloc (spec->tokens,
                                     (spec->token_count + 1) * sizeof (*nt));
        if (!nt)
          return -1;
        spec->tokens = nt;
        spec->tokens[spec->token_count++] = def;
      }
    }
  return 0;
}

/* ================= Syntactic ================= */

static int
split_seq (const char *s, char ***out, size_t *n)
{
  *out = NULL;
  *n = 0;
  size_t cap = 0;
  const char *p = s;
  while (*p)
    {
      while (*p && isspace ((unsigned char) *p))
        p++;
      if (!*p)
        break;
      const char *q = p;
      while (*q && !isspace ((unsigned char) *q))
        q++;
      char *w = xstrndup (p, (size_t) (q - p));
      if (!w)
        return -1;
      if (*n + 1 > cap)
        {
          cap = cap ? cap * 2 : 8;
          char **nb = realloc (*out, cap * sizeof (*nb));
          if (!nb)
            {
              free (w);
              return -1;
            }
          *out = nb;
        }
      (*out)[(*n)++] = w;
      p = q;
    }
  return 0;
}

static int
md_parse_syntactic (lexer_t *lx, md_spec_t *spec)
{
  if (expect (lx, T_LBRACE, NULL) != 0)
    return -1;
  for (;;)
    {
      token_t t = next_tok (lx);
      if (t.kind == T_RBRACE)
        {
          tok_free (&t);
          break;
        }
      if (t.kind == T_EOF)
        {
          tok_free (&t);
          return -1;
        }
      if (t.kind != T_IDENT)
        {
          tok_free (&t);
          continue;
        }
      if (!strcmp (t.text, "StartRule"))
        {
          tok_free (&t);
          token_t e = next_tok (lx);
          tok_free (&e); /* '=' */
          char *val = parse_value (lx);
          free (spec->start_rule);
          spec->start_rule = val;
          continue;
        }
      if (strcmp (t.text, "Rule"))
        {
          tok_free (&t);
          continue;
        }
      tok_free (&t);
      if (expect (lx, T_LPAREN, NULL) != 0)
        return -1;
      token_t rn = next_tok (lx);
      char *rname = xstrdup (rn.text ? rn.text : "");
      tok_free (&rn);
      if (expect (lx, T_RPAREN, NULL) != 0)
        {
          free (rname);
          return -1;
        }
      token_t ar = next_tok (lx);
      char *returns = xstrdup ("CNode");
      if (ar.kind == T_ARROW)
        {
          tok_free (&ar);
          token_t rt = next_tok (lx);
          if (rt.kind == T_IDENT || rt.kind == T_STRING)
            {
              free (returns);
              returns = xstrdup (rt.text ? rt.text : "CNode");
              /* Consume possible trailing '*' (e.g. JsonMember*). */
              token_t star = next_tok (lx);
              if (star.kind == T_IDENT && !strcmp (star.text, "*"))
                {
                  size_t L = strlen (returns);
                  char *w = malloc (L + 2);
                  if (w)
                    {
                      memcpy (w, returns, L);
                      w[L] = '*';
                      w[L + 1] = '\0';
                      free (returns);
                      returns = w;
                    }
                  tok_free (&star);
                }
              else
                unget_tok (lx, star);
            }
          tok_free (&rt);
        }
      else
        unget_tok (lx, ar);
      if (expect (lx, T_LBRACE, NULL) != 0)
        {
          free (rname);
          free (returns);
          return -1;
        }
      md_rule_t rule;
      memset (&rule, 0, sizeof (rule));
      rule.name = rname;
      rule.returns = returns;
      for (;;)
        {
          token_t a = next_tok (lx);
          if (a.kind == T_RBRACE)
            {
              tok_free (&a);
              break;
            }
          if (a.kind != T_IDENT || !a.text || strcmp (a.text, "Alt"))
            {
              tok_free (&a);
              if (a.kind == T_EOF)
                return -1;
              continue;
            }
          tok_free (&a);
          if (expect (lx, T_LBRACE, NULL) != 0)
            return -1;
          md_alt_t alt;
          memset (&alt, 0, sizeof (alt));
          for (;;)
            {
              token_t k = next_tok (lx);
              if (k.kind == T_RBRACE)
                {
                  tok_free (&k);
                  break;
                }
              if (k.kind != T_IDENT)
                {
                  tok_free (&k);
                  continue;
                }
              char *key = k.text;
              k.text = NULL;
              tok_free (&k);
              token_t e = next_tok (lx);
              if (e.kind != T_EQUALS)
                {
                  tok_free (&e);
                  free (key);
                  continue;
                }
              tok_free (&e);
              char *val = parse_value (lx);
              if (!strcmp (key, "Seq"))
                {
                  split_seq (val ? val : "", &alt.seq, &alt.seq_count);
                  free (val);
                }
              else if (!strcmp (key, "Action"))
                alt.action = val;
              else
                free (val);
              free (key);
            }
          {
            md_alt_t *na = realloc (rule.alts,
                                   (rule.alt_count + 1) * sizeof (*na));
            if (!na)
              return -1;
            rule.alts = na;
            rule.alts[rule.alt_count++] = alt;
          }
        }
      {
        md_rule_t *nr = realloc (spec->rules,
                                (spec->rule_count + 1) * sizeof (*nr));
        if (!nr)
          return -1;
        spec->rules = nr;
        spec->rules[spec->rule_count++] = rule;
      }
    }
  return 0;
}

/* ================= top level ================= */

int
md_parse_string (const char *text, size_t len, const char *origin,
                 md_spec_t *spec, md_error_t *err)
{
  lexer_t lx;
  memset (&lx, 0, sizeof (lx));
  lx.src = text;
  lx.len = len;
  lx.pos = 0;
  lx.line = 1;
  lx.origin = origin;
  lx.err = err;
  if (err)
    {
      err->message[0] = '\0';
      err->line = 0;
    }
  md_spec_init (spec);
  for (;;)
    {
      token_t t = next_tok (&lx);
      if (t.kind == T_EOF)
        {
          tok_free (&t);
          break;
        }
      if (t.kind != T_IDENT)
        {
          tok_free (&t);
          continue;
        }
      if (!strcmp (t.text, "Entrypoint"))
        {
          tok_free (&t);
          if (md_parse_entrypoint (&lx, spec) != 0)
            goto fail;
        }
      else if (!strcmp (t.text, "AST"))
        {
          tok_free (&t);
          if (md_parse_ast (&lx, spec) != 0)
            goto fail;
        }
      else if (!strcmp (t.text, "Lexical"))
        {
          tok_free (&t);
          if (md_parse_lexical (&lx, spec) != 0)
            goto fail;
        }
      else if (!strcmp (t.text, "Syntactic"))
        {
          tok_free (&t);
          if (md_parse_syntactic (&lx, spec) != 0)
            goto fail;
        }
      else
        tok_free (&t);
    }
  if (lx.has_saved)
    tok_free (&lx.saved);
  if (!spec->start_rule && spec->rule_count > 0)
    spec->start_rule = xstrdup (spec->rules[0].name);
  return 0;
fail:
  if (lx.has_saved)
    tok_free (&lx.saved);
  if (err && !err->message[0])
    snprintf (err->message, sizeof (err->message), "%s: parse failed",
              origin ? origin : "<input>");
  md_spec_destroy (spec);
  return -1;
}

int
md_parse_file (const char *path, md_spec_t *spec, md_error_t *err)
{
  FILE *f = fopen (path, "rb");
  char *buf = NULL;
  size_t len = 0, cap = 0;
  int rc;
  if (!f)
    {
      if (err)
        snprintf (err->message, sizeof (err->message), "%s: cannot open", path);
      return -1;
    }
  cap = 65536;
  buf = malloc (cap + 1);
  if (!buf)
    {
      fclose (f);
      return -1;
    }
  for (;;)
    {
      size_t n;
      if (len + 8192 > cap)
        {
          cap *= 2;
          char *nb = realloc (buf, cap + 1);
          if (!nb)
            {
              free (buf);
              fclose (f);
              return -1;
            }
          buf = nb;
        }
      n = fread (buf + len, 1, cap - len, f);
      len += n;
      if (n == 0)
        break;
    }
  fclose (f);
  buf[len] = '\0';
  rc = md_parse_string (buf, len, path, spec, err);
  free (buf);
  return rc;
}

static int
find_rule (const md_spec_t *spec, const char *name)
{
  for (size_t i = 0; i < spec->rule_count; i++)
    if (!strcmp (spec->rules[i].name, name))
      return (int) i;
  return -1;
}

static int
find_token (const md_spec_t *spec, const char *name)
{
  for (size_t i = 0; i < spec->token_count; i++)
    if (!strcmp (spec->tokens[i].name, name))
      return (int) i;
  return -1;
}

static int
find_node (const md_spec_t *spec, const char *name)
{
  /* Return types may carry a trailing '*' (list types). Strip it. */
  char base[256];
  size_t n = strlen (name);
  while (n > 0 && (name[n - 1] == '*' || name[n - 1] == ' '))
    n--;
  if (n >= sizeof (base))
    return 0; /* long external type: assume opaque, accept */
  memcpy (base, name, n);
  base[n] = '\0';
  if (!base[0])
    return 0;
  /* Primitive C types are opaque and always accepted. */
  if (!strcmp (base, "CNode") || !strcmp (base, "char*")
      || !strcmp (base, "char") || !strcmp (base, "int")
      || !strcmp (base, "double") || !strcmp (base, "JsonValue")
      || !strcmp (base, "JsonObject") || !strcmp (base, "JsonMember")
      || !strcmp (base, "JsonArray") || !strcmp (base, "JsonString")
      || !strcmp (base, "JsonNumber") || !strcmp (base, "JsonBool")
      || !strcmp (base, "JsonNull"))
    return 0;
  for (size_t i = 0; i < spec->node_count; i++)
    if (!strcmp (spec->nodes[i].name, base))
      return 0;
  return -1;
}

int
md_spec_validate (const md_spec_t *spec, md_error_t *err)
{
  if (spec->rule_count == 0)
    {
      if (err)
        snprintf (err->message, sizeof (err->message),
                  "no syntactic rules defined");
      return -1;
    }
  if (!spec->start_rule || find_rule (spec, spec->start_rule) < 0)
    {
      if (err)
        snprintf (err->message, sizeof (err->message),
                  "StartRule \"%s\" does not name a rule",
                  spec->start_rule ? spec->start_rule : "(none)");
      return -1;
    }
  for (size_t i = 0; i < spec->rule_count; i++)
    {
      const md_rule_t *r = &spec->rules[i];
      if (find_node (spec, r->returns) != 0)
        {
          if (err)
            snprintf (err->message, sizeof (err->message),
                      "rule %s returns unknown AST type %s", r->name,
                      r->returns);
          return -1;
        }
      if (r->alt_count == 0)
        {
          if (err)
            snprintf (err->message, sizeof (err->message),
                      "rule %s has no alternatives", r->name);
          return -1;
        }
      for (size_t a = 0; a < r->alt_count; a++)
        {
          if (r->alts[a].seq_count == 0)
            {
              if (err)
                snprintf (err->message, sizeof (err->message),
                          "rule %s alt %zu has empty Seq", r->name, a);
              return -1;
            }
          for (size_t s = 0; s < r->alts[a].seq_count; s++)
            {
              const char *nm = r->alts[a].seq[s];
              if (find_rule (spec, nm) < 0 && find_token (spec, nm) < 0)
                {
                  if (err)
                    snprintf (err->message, sizeof (err->message),
                              "rule %s references unknown symbol %s", r->name,
                              nm);
                  return -1;
                }
            }
        }
    }
  return 0;
}

const char *
md_config_get (const md_spec_t *spec, const char *key)
{
  for (size_t i = 0; i < spec->entry.cfg_count; i++)
    if (!strcmp (spec->entry.cfg_keys[i], key))
      return spec->entry.cfg_values[i];
  return NULL;
}

const char *
md_atn_get (const md_spec_t *spec, const char *key)
{
  for (size_t i = 0; i < spec->entry.atn_count; i++)
    if (!strcmp (spec->entry.atn_keys[i], key))
      return spec->entry.atn_values[i];
  return NULL;
}

int
md_config_get_bool (const md_spec_t *spec, const char *key, int dflt)
{
  const char *v = md_config_get (spec, key);
  if (!v)
    return dflt;
  if (!strcmp (v, "true") || !strcmp (v, "Yes") || !strcmp (v, "1"))
    return 1;
  if (!strcmp (v, "false") || !strcmp (v, "No") || !strcmp (v, "0"))
    return 0;
  return dflt;
}

long
md_config_get_int (const md_spec_t *spec, const char *key, long dflt)
{
  const char *v = md_config_get (spec, key);
  char *end = NULL;
  long x;
  if (!v)
    return dflt;
  x = strtol (v, &end, 10);
  if (end == v)
    return dflt;
  return x;
}
