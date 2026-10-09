/* moosedog_emitter.c -- generate C artifacts from a .grm spec. */
#include "moosedog_emitter.h"
#include "../lexer/moosedog_lexer.h"
#include "../rewrite/moosedog_rewrite.h"
#include "../confuse/moosedog_confuse.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

void
md_emit_default_basename (const md_spec_t *spec, const char *grm_path,
                          char *out, size_t out_size)
{
  const char *base = grm_path ? strrchr (grm_path, '/') : NULL;
  base = base ? base + 1 : (grm_path ? grm_path : "Grammar");
  {
    const char *dot = strrchr (base, '.');
    size_t n = dot ? (size_t) (dot - base) : strlen (base);
    if (n >= out_size)
      n = out_size - 1;
    memcpy (out, base, n);
    out[n] = '\0';
  }
  if (spec->entry.lang_name && spec->entry.lang_name[0])
    {
      char clean[256];
      size_t n = 0;
      for (const char *p = spec->entry.lang_name; *p && n + 1 < sizeof (clean); p++)
        if (isalnum ((unsigned char) *p) || *p == '_')
          clean[n++] = *p;
      clean[n] = '\0';
      if (n > 0 && (!strcmp (out, "Grammar") || !strcmp (out, "grammar")))
        snprintf (out, out_size, "%s", clean);
    }
  {
    char tmp[256];
    size_t n = 0;
    for (size_t i = 0; out[i] && n + 1 < sizeof (tmp); i++)
      if (isalnum ((unsigned char) out[i]) || out[i] == '_')
        tmp[n++] = out[i];
    tmp[n] = '\0';
    if (n == 0 || isdigit ((unsigned char) tmp[0]))
      snprintf (out, out_size, "G_%s", tmp);
    else
      snprintf (out, out_size, "%s", tmp);
  }
}

static int
mkdir_p (const char *dir)
{
  char tmp[4096];
  snprintf (tmp, sizeof (tmp), "%s", dir);
  for (char *p = tmp + 1; *p; p++)
    {
      if (*p == '/')
        {
          *p = '\0';
          mkdir (tmp, 0755);
          *p = '/';
        }
    }
  if (mkdir (tmp, 0755) != 0 && errno != EEXIST)
    return -1;
  return 0;
}

static void
guard_of (const char *stem, const char *suffix, char *out, size_t n)
{
  size_t k = 0;
  for (size_t i = 0; stem[i] && k + 1 < n; i++)
    out[k++] = (char) toupper ((unsigned char) stem[i]);
  for (size_t i = 0; suffix[i] && k + 1 < n; i++)
    out[k++] = (char) toupper ((unsigned char) suffix[i]);
  out[k] = '\0';
}

static int
write_file (const char *path, const char *content, md_error_t *err)
{
  FILE *f = fopen (path, "wb");
  if (!f)
    {
      if (err)
        snprintf (err->message, sizeof (err->message), "cannot write %s: %s",
                  path, strerror (errno));
      return -1;
    }
  {
    size_t L = strlen (content);
    if (fwrite (content, 1, L, f) != L)
      {
        if (err)
          snprintf (err->message, sizeof (err->message), "write failed: %s",
                    path);
        fclose (f);
        return -1;
      }
  }
  fclose (f);
  return 0;
}

typedef struct
{
  char *data;
  size_t len;
  size_t cap;
  int failed; /* sticky allocation-failure flag */
} sbuf_t;

static int
sb_put (sbuf_t *sb, const char *s)
{
  if (sb->failed)
    return -1;
  size_t L = strlen (s);
  if (sb->len + L + 1 > sb->cap)
    {
      size_t nc = sb->cap ? sb->cap * 2 : 8192;
      while (nc < sb->len + L + 1)
        nc *= 2;
      {
        char *nd = realloc (sb->data, nc);
        if (!nd)
          {
            sb->failed = 1;
            return -1;
          }
        sb->data = nd;
        sb->cap = nc;
      }
    }
  memcpy (sb->data + sb->len, s, L);
  sb->len += L;
  sb->data[sb->len] = '\0';
  return 0;
}

static int
sb_printf (sbuf_t *sb, const char *fmt, ...)
{
  va_list ap;
  int need;
  va_start (ap, fmt);
  need = vsnprintf (NULL, 0, fmt, ap);
  va_end (ap);
  if (sb->failed)
    return -1;
  if (need < 0)
    {
      sb->failed = 1;
      return -1;
    }
  {
    size_t want = sb->len + (size_t) need + 1;
    if (want > sb->cap)
      {
        size_t nc = sb->cap ? sb->cap * 2 : 8192;
        while (nc < want)
          nc *= 2;
        {
          char *nd = realloc (sb->data, nc);
          if (!nd)
            return -1;
          sb->data = nd;
          sb->cap = nc;
        }
      }
    va_start (ap, fmt);
    need = vsnprintf (sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    va_end (ap);
    if (need < 0)
      {
        sb->failed = 1;
        return -1;
      }
    sb->len += (size_t) need;
  }
  return 0;
}

/* Append one double-quoted C string literal with full escaping.
 * Grows dynamically; returns 0 on success, -1 on allocation failure. */
static int
sb_put_cstring (sbuf_t *sb, const char *s)
{
  static const char hex[] = "0123456789abcdef";
  if (!s)
    s = "";
  if (sb_put (sb, "\"") != 0)
    return -1;
  for (const unsigned char *p = (const unsigned char *) s; *p; p++)
    {
      char esc[8];
      switch (*p)
        {
        case '\\':
          if (sb_put (sb, "\\\\") != 0)
            return -1;
          break;
        case '"':
          if (sb_put (sb, "\\\"") != 0)
            return -1;
          break;
        case '\n':
          if (sb_put (sb, "\\n") != 0)
            return -1;
          break;
        case '\r':
          if (sb_put (sb, "\\r") != 0)
            return -1;
          break;
        case '\t':
          if (sb_put (sb, "\\t") != 0)
            return -1;
          break;
        default:
          if (*p < 0x20 || *p == 0x7f)
            {
              esc[0] = '\\';
              esc[1] = 'x';
              esc[2] = hex[(*p >> 4) & 0xf];
              esc[3] = hex[*p & 0xf];
              esc[4] = '\0';
              if (sb_put (sb, esc) != 0)
                return -1;
            }
          else
            {
              esc[0] = (char) *p;
              esc[1] = '\0';
              if (sb_put (sb, esc) != 0)
                return -1;
            }
          break;
        }
    }
  return sb_put (sb, "\"");
}

static const char *
c_type_of (const char *t)
{
  if (!t)
    return "void*";
  return t;
}

static int
emit_ast_h (const md_spec_t *spec, const char *base, sbuf_t *sb)
{
  char guard[300];
  guard_of (base, "_AST_H", guard, sizeof (guard));
  sb_printf (sb, "/* %s.ast.h -- Moosedog-generated AST types for %s. */\n", base,
             spec->entry.lang_name ? spec->entry.lang_name : base);
  sb_printf (sb, "/* Generated by moosedog 1.0.0; do not hand-edit. */\n");
  sb_printf (sb, "#ifndef %s\n#define %s\n\n", guard, guard);
  sb_printf (sb, "#include <stddef.h>\n\n");
  sb_printf (sb, "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n");
  /* Forward declarations so structs can reference each other. */
  for (size_t i = 0; i < spec->node_count; i++)
    {
      const md_ast_node_t *fn = &spec->nodes[i];
      if (fn->tag && fn->kind && strcmp (fn->kind, "Union"))
        sb_printf (sb, "typedef struct %s %s;\n", fn->tag, fn->tag);
    }
  if (spec->node_count)
    sb_put (sb, "\n");
  for (size_t i = 0; i < spec->node_count; i++)
    {
      const md_ast_node_t *n = &spec->nodes[i];
      sb_printf (sb, "/* Node %s (%s) */\n", n->name,
                 n->kind ? n->kind : "Struct");
      if (n->kind && !strcmp (n->kind, "Union"))
        {
          sb_printf (sb, "typedef struct %s %s;\n",
                     n->tag ? n->tag : n->name, n->tag ? n->tag : n->name);
          sb_printf (sb, "typedef enum %s_kind {\n", n->name);
          for (size_t v = 0; v < n->variant_count; v++)
            sb_printf (sb, "  %s_kind_%s = %zu,\n", n->name, n->variants[v], v);
          sb_printf (sb, "} %s_kind_t;\n\n", n->name);
        }
      else
        {
          sb_printf (sb, "typedef struct %s {\n",
                     n->tag ? n->tag : n->name);
          for (size_t f = 0; f < n->field_count; f++)
            {
              const char *ft = n->fields[f].type ? n->fields[f].type : "void*";
              /* Map node names (e.g. "JsonMember*") to their C tags
               * (e.g. "json_member_t"); primitives pass through. */
              char fbase[256];
              size_t fl = strlen (ft);
              int is_ptr = 0;
              const char *tag = NULL;
              while (fl > 0 && (ft[fl - 1] == '*' || ft[fl - 1] == ' '))
                {
                  if (ft[fl - 1] == '*')
                    is_ptr = 1;
                  fl--;
                }
              if (fl >= sizeof (fbase))
                fl = sizeof (fbase) - 1;
              memcpy (fbase, ft, fl);
              fbase[fl] = '\0';
              for (size_t k = 0; k < spec->node_count; k++)
                if (!strcmp (spec->nodes[k].name, fbase) && spec->nodes[k].tag)
                  tag = spec->nodes[k].tag;
              if (n->fields[f].arity && !strcmp (n->fields[f].arity, "Many"))
                sb_printf (sb, "  struct %s_list *%s; /* Many %s */\n", base,
                           n->fields[f].name, ft);
              else if (tag)
                sb_printf (sb, "  %s %s%s; /* One %s */\n", tag,
                           is_ptr ? "*" : "", n->fields[f].name, ft);
              else
                sb_printf (sb, "  %s %s;\n", c_type_of (n->fields[f].type),
                           n->fields[f].name);
            }
          if (n->field_count == 0)
            sb_printf (sb, "  int _empty;\n");
          sb_printf (sb, "} %s;\n\n", n->tag ? n->tag : n->name);
        }
    }
  sb_printf (sb, "/* Generic ordered child list used by Many fields. */\n");
  sb_printf (sb, "typedef struct %s_list {\n", base);
  sb_printf (sb, "  void *item;\n  struct %s_list *next;\n} %s_list_t;\n\n",
             base, base);
  if (spec->node_count)
    {
      const char *root = spec->nodes[0].name;
      const char *roottag = spec->nodes[0].tag ? spec->nodes[0].tag : root;
      sb_printf (sb, "typedef %s %s_root_t; /* first AST node is the root */\n\n",
                 roottag, base);
    }
  else
    sb_printf (sb, "typedef void *%s_root_t; /* no AST block: untyped */\n\n",
               base);
  sb_printf (sb, "/* Listener/visitor hooks (emitted per Config). */\n");
  sb_printf (sb, "typedef void (*%s_listener_fn)(const char *node_kind,\n", base);
  sb_printf (sb, "    const char *node_name, void *user_data);\n");
  sb_printf (sb, "typedef int (*%s_visitor_fn)(const char *node_kind,\n", base);
  sb_printf (sb, "    const char *node_name, void *user_data);\n\n");
  sb_printf (sb, "#ifdef __cplusplus\n}\n#endif\n\n#endif /* %s */\n", guard);
  return 0;
}

static int
emit_lexer_h (const md_spec_t *spec, const md_build_config_t *cfg,
              const char *base, sbuf_t *sb)
{
  char guard[300];
  guard_of (base, "_LEXER_H", guard, sizeof (guard));
  sb_printf (sb, "/* %s.lexer.h -- Moosedog-generated lexer tables. */\n", base);
  sb_printf (sb, "/* Generated by moosedog; do not hand-edit. */\n");
  sb_printf (sb, "#ifndef %s\n#define %s\n\n", guard, guard);
  sb_printf (sb, "#include <stddef.h>\n\n");
  sb_printf (sb, "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n");
  sb_printf (sb, "typedef enum %s_token_kind {\n", base);
  for (size_t i = 0; i < spec->token_count; i++)
    sb_printf (sb, "  %s_TOK_%s = %zu,%s\n", base, spec->tokens[i].name, i,
               spec->tokens[i].is_skip ? " /* skip */" : "");
  sb_printf (sb, "  %s_TOK_COUNT = %zu\n} %s_token_kind_t;\n\n", base,
             spec->token_count, base);
  sb_printf (sb, "typedef struct %s_token_def {\n", base);
  sb_printf (sb, "  const char *name;\n  const char *pattern;\n");
  sb_printf (sb, "  const char *classifier; /* NULL unless contextual */\n");
  sb_printf (sb, "  int literal;\n  int is_skip;\n} %s_token_def_t;\n\n", base);
  sb_printf (sb, "extern const %s_token_def_t %s_tokens[];\n", base, base);
  sb_printf (sb, "extern const size_t %s_token_count;\n\n", base);
  sb_printf (sb, "/* Backends selected by Config: main=%s fallback=%s. */\n",
             cfg->main_lexer, cfg->fallback_lexer);
  sb_printf (sb, "#ifdef __cplusplus\n}\n#endif\n\n#endif /* %s */\n", guard);
  return 0;
}

static int
emit_parser_h (const md_spec_t *spec, const md_build_config_t *cfg,
               const char *base, sbuf_t *sb)
{
  char guard[300];
  guard_of (base, "_PARSER_H", guard, sizeof (guard));
  sb_printf (sb, "/* %s.parser.h -- Moosedog-generated parser entry points. */\n",
             base);
  sb_printf (sb, "/* Generated by moosedog; do not hand-edit. */\n");
  sb_printf (sb, "#ifndef %s\n#define %s\n\n", guard, guard);
  sb_printf (sb, "#include \"Moosedog.h\"\n#include \"%s.ast.h\"\n", base);
  sb_printf (sb, "#include \"%s.lexer.h\"\n", base);
  sb_printf (sb, "#include <glr/glr.h>\n\n");
  sb_printf (sb, "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n");
  sb_printf (sb, "/* Grammar metadata for %s. */\n",
             spec->entry.lang_name ? spec->entry.lang_name : base);
  sb_printf (sb, "extern const moosedog_info_t %s_info;\n\n", base);
  sb_printf (sb, "/* Build a libglr grammar for %s. Returns NULL on failure. */\n",
             base);
  sb_printf (sb,
             "glr_grammar_t *%s_build_grammar (char *error, size_t error_size);\n\n",
             base);
  sb_printf (sb, "/* Create a parser with trivia + ATN settings applied. */\n");
  sb_printf (sb, "glr_parser_t *%s_create_parser (glr_grammar_t *grammar);\n\n",
             base);
  sb_printf (sb, "/* Start rule: %s. */\n",
             spec->start_rule ? spec->start_rule : "?");
  sb_printf (sb, "/* Rules: %zu  Terminals+skips: %zu  ATN: %s (depth %d) */\n",
             spec->rule_count, spec->token_count,
             cfg->adaptive_lookahead ? "adaptive" : "plain",
             cfg->lookahead_depth);
  sb_printf (sb, "#ifdef __cplusplus\n}\n#endif\n\n#endif /* %s */\n", guard);
  return 0;
}

static int
emit_c_string (sbuf_t *sb, const char *s)
{
  return sb_put_cstring (sb, s ? s : "");
}

static int
emit_parser_c (const md_spec_t *spec, const md_build_config_t *cfg,
               const char *base, const md_emit_options_t *opt, sbuf_t *sb)
{
  sb_printf (sb, "/* %s.parser.c -- Moosedog-generated grammar builder. */\n", base);
  sb_printf (sb, "/* Grammar: %s  Standard: %s */\n",
             spec->entry.lang_name ? spec->entry.lang_name : base,
             spec->entry.standard_url ? spec->entry.standard_url : "n/a");
  sb_printf (sb, "/* Generated by moosedog; do not hand-edit. */\n");
  sb_printf (sb, "#include \"%s.parser.h\"\n", base);
  sb_printf (sb, "#include <glr/scannerless.h>\n");
  sb_printf (sb, "#include <string.h>\n#include <stdio.h>\n\n");
  sb_printf (sb, "const moosedog_info_t %s_info = {\n", base);
  sb_printf (sb, "  ");
  emit_c_string (sb, spec->entry.lang_name ? spec->entry.lang_name : base);
  sb_put (sb, ", ");
  {
    const char *ver = spec->entry.revision_count
                          ? spec->entry.revisions[spec->entry.revision_count - 1]
                          : "1.0.0";
    emit_c_string (sb, ver);
  }
  sb_put (sb, ", \"\", ");
  emit_c_string (sb, spec->entry.standard_url);
  sb_put (sb, ", ");
  emit_c_string (sb, spec->entry.license);
  sb_printf (sb, ",\n  %d, %d, %d, %d, %d, %d, %d };\n\n", cfg->use_atn,
             cfg->adaptive_lookahead, cfg->lookahead_depth, cfg->error_recovery,
             cfg->max_errors, cfg->strict_unicode, cfg->debug_level);
  sb_printf (sb, "const %s_token_def_t %s_tokens[] = {\n", base, base);
  for (size_t i = 0; i < spec->token_count; i++)
    {
      const md_token_def_t *t = &spec->tokens[i];
      /* Emit the POSIX-normalized pattern: what the table documents is
       * what the builder registers below. */
      char *norm = t->pattern ? md_lexer_normalize_pattern (t->pattern) : NULL;
      sb_printf (sb, "  { \"%s\", ", t->name);
      if (norm)
        emit_c_string (sb, norm);
      else if (t->pattern)
        emit_c_string (sb, t->pattern);
      else
        sb_put (sb, "NULL");
      sb_put (sb, ", ");
      if (t->classifier)
        emit_c_string (sb, t->classifier);
      else
        sb_put (sb, "NULL");
      sb_printf (sb, ", %d, %d },\n", t->literal, t->is_skip);
      free (norm);
    }
  sb_printf (sb, "};\nconst size_t %s_token_count = %zu;\n\n", base,
             spec->token_count);
  if (spec->entry.rewriter_count && !opt->no_rewrite)
    {
      sb_printf (sb, "/* Rewriters (rewritelib .grl programs, applied in order): */\n");
      for (size_t i = 0; i < spec->entry.rewriter_count; i++)
        sb_printf (sb, "/*   %s (%s) */\n", spec->entry.rewriters[i],
                   md_rewrite_stem_of (spec->entry.rewriters[i]));
      sb_put (sb, "\n");
    }
  if (spec->entry.disambiguator_count)
    {
      sb_printf (sb, "/* Disambiguators (disambstd hooks): */\n");
      for (size_t i = 0; i < spec->entry.disambiguator_count; i++)
        sb_printf (sb, "/*   %s */\n", spec->entry.disambiguators[i]);
      sb_put (sb, "\n");
    }
  if (spec->entry.query_count)
    {
      sb_printf (sb, "/* Lua queries bundled with the grammar: */\n");
      for (size_t i = 0; i < spec->entry.query_count; i++)
        sb_printf (sb, "/*   %s */\n", spec->entry.queries[i]);
      sb_put (sb, "\n");
    }
  sb_printf (sb, "glr_grammar_t *\n%s_build_grammar (char *error, size_t error_size)\n{\n",
             base);
  sb_printf (sb, "  glr_grammar_t *g = glr_grammar_create ();\n");
  sb_printf (sb, "  glr_symbol_t *body[128];\n");
  sb_printf (sb, "  size_t n;\n  int head;\n");
  sb_printf (sb, "  if (!g)\n    return NULL;\n");
  for (size_t i = 0; i < spec->token_count; i++)
    {
      const md_token_def_t *t = &spec->tokens[i];
      sb_printf (sb, "  { int id = glr_grammar_add_symbol (g, GLR_SYMBOL_TERMINAL, \"%s\");\n",
                 t->name);
      sb_printf (sb, "    if (id < 0) goto fail;\n");
      if (t->classifier)
        sb_printf (sb,
                   "    /* contextual %s: classified at parse time, no regex */\n",
                   t->classifier);
      else if (t->pattern && t->literal)
        {
          sb_printf (sb, "    { const char *lit = ");
          emit_c_string (sb, t->pattern);
          sb_printf (sb, ";\n      glr_scannerless_set_literal (g, id, lit, strlen (lit)); }\n");
        }
      else if (t->pattern)
        {
          /* Register the normalized form (see token table above). */
          char *norm = md_lexer_normalize_pattern (t->pattern);
          sb_printf (sb, "    { char perr[256] = {0};\n");
          sb_printf (sb, "      const char *pat = ");
          emit_c_string (sb, norm ? norm : t->pattern);
          free (norm);
          sb_printf (sb, ";\n");
          sb_printf (sb, "      if (glr_scannerless_set_pattern (g, id, pat, perr, sizeof perr) != 0)\n");
          sb_printf (sb, "        {\n");
          sb_printf (sb,
                     "          if (error) snprintf (error, error_size, \"token %s: %%s\", perr);\n",
                     t->name);
          sb_printf (sb, "          goto fail;\n        } }\n");
        }
      sb_printf (sb, "  }\n");
    }
  for (size_t i = 0; i < spec->rule_count; i++)
    sb_printf (sb,
               "  if (glr_grammar_add_symbol (g, GLR_SYMBOL_NONTERMINAL, \"%s\") < 0)\n    goto fail;\n",
               spec->rules[i].name);
  for (size_t i = 0; i < spec->rule_count; i++)
    {
      const md_rule_t *r = &spec->rules[i];
      for (size_t a = 0; a < r->alt_count; a++)
        {
          const md_alt_t *alt = &r->alts[a];
          sb_printf (sb, "  { /* %s ->", r->name);
          for (size_t s = 0; s < alt->seq_count; s++)
            sb_printf (sb, " %s", alt->seq[s]);
          sb_put (sb, " */\n");
          if (alt->action)
            {
              char one[512];
              size_t k = 0;
              for (const char *p = alt->action; *p && k + 1 < sizeof (one); p++)
                {
                  if (*p == '*' && p[1] == '/')
                    {
                      one[k++] = '*';
                      one[k++] = ' ';
                      p++;
                    }
                  else
                    one[k++] = *p;
                }
              one[k] = '\0';
              sb_printf (sb, "    /* Action: %s */\n", one);
            }
          sb_printf (sb,
                     "    head = glr_grammar_find_symbol (g, \"%s\", GLR_SYMBOL_NONTERMINAL);\n",
                     r->name);
          sb_printf (sb, "    if (head < 0) goto fail;\n");
          sb_printf (sb, "    n = %zu;\n", alt->seq_count);
          sb_printf (sb, "    if (n > 128) goto fail;\n");
          for (size_t s = 0; s < alt->seq_count; s++)
            {
              sb_printf (sb,
                         "    { int sid = glr_grammar_find_symbol_any (g, \"%s\");\n",
                         alt->seq[s]);
              sb_printf (sb, "      if (sid < 0) goto fail;\n");
              sb_printf (sb, "      body[%zu] = glr_grammar_get_symbol (g, sid); }\n", s);
            }
          sb_printf (sb, "    if (glr_grammar_add_production (g, head, body, n) < 0)\n");
          sb_printf (sb, "      goto fail;\n  }\n");
        }
    }
  sb_printf (sb, "  { int start = glr_grammar_find_symbol (g, \"%s\", GLR_SYMBOL_NONTERMINAL);\n",
             spec->start_rule ? spec->start_rule : "");
  sb_printf (sb, "    if (start < 0) goto fail;\n");
  sb_printf (sb, "    if (glr_grammar_set_start_symbol (g, start) != 0) goto fail; }\n");
  sb_printf (sb, "  return g;\n");
  sb_printf (sb, "fail:\n  if (error && !error[0])\n");
  sb_printf (sb, "    snprintf (error, error_size, \"%s: grammar build failed\");\n",
             base);
  sb_printf (sb, "  glr_grammar_destroy (g);\n  return NULL;\n}\n\n");
  /* Parser constructor with trivia + ATN wiring. */
  sb_printf (sb, "glr_parser_t *\n%s_create_parser (glr_grammar_t *grammar)\n{\n",
             base);
  sb_printf (sb, "  glr_parser_t *p = glr_parser_create (grammar);\n");
  sb_printf (sb, "  if (!p) return NULL;\n");
  {
    int has_skip = 0;
    for (size_t i = 0; i < spec->token_count; i++)
      if (spec->tokens[i].is_skip)
        has_skip = 1;
    if (has_skip)
      {
        sb_put (sb, "  /* Skip tokens are whitespace-like; use space trivia. */\n");
        sb_put (sb, "  glr_parser_set_trivia (p, \" \");\n");
      }
  }
  if (cfg->adaptive_lookahead)
    {
      sb_put (sb, "  glr_parser_set_adaptive_lookahead (p, true);\n");
      sb_printf (sb, "  glr_parser_set_adaptive_lookahead_depth (p, %d);\n",
                 cfg->lookahead_depth);
    }
  sb_put (sb, "  return p;\n}\n");
  return 0;
}

static int
emit_makefile (const char *base, sbuf_t *sb)
{
  sb_printf (sb, "# GNU Makefile for the %s Moosedog-generated parser.\n", base);
  sb_printf (sb, "# Generated by moosedog; adapt CC/CFLAGS/LDFLAGS as needed.\n");
  sb_printf (sb, "CC ?= cc\nCFLAGS ?= -std=c11 -Wall -Wextra -I/usr/local/include\n");
  sb_printf (sb, "LDFLAGS ?= $(shell pkg-config --libs --static libglr 2>/dev/null)\n");
  sb_printf (sb, "OBJS = %s.parser.o\n", base);
  sb_printf (sb, "all: lib%s_parser.a\n", base);
  sb_printf (sb, "lib%s_parser.a: $(OBJS)\n", base);
  sb_printf (sb, "\tar rcs $@ $(OBJS)\n");
  sb_printf (sb, "%%.o: %%.c %s.parser.h %s.ast.h %s.lexer.h\n", base, base, base);
  sb_printf (sb, "\t$(CC) $(CFLAGS) -c $< -o $@\n");
  sb_printf (sb, "clean:\n\trm -f $(OBJS) lib%s_parser.a\n", base);
  return 0;
}

static int
emit_cmake (const char *base, sbuf_t *sb)
{
  sb_printf (sb, "# CMakeLists for the %s Moosedog-generated parser.\n", base);
  sb_printf (sb, "cmake_minimum_required(VERSION 3.16)\n");
  sb_printf (sb, "project(%s_parser C)\n", base);
  sb_printf (sb, "find_package(libglr 1.0 REQUIRED)\n");
  sb_printf (sb, "add_library(%s_parser STATIC %s.parser.c)\n", base, base);
  sb_printf (sb, "target_link_libraries(%s_parser PUBLIC libglr::libglr)\n", base);
  sb_printf (sb, "target_include_directories(%s_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})\n",
             base);
  return 0;
}

static char *make_out_path (const char *dir, const char *stem,
                            const char *suffix, md_error_t *err);

static int
copy_moosedog_h (const char *outdir, const char *src_hint, md_error_t *err)
{
  char *dst = make_out_path (outdir, "Moosedog", ".h", err);
  if (!dst)
    return -1;
  {
    FILE *probe = fopen (dst, "rb");
    if (probe)
      {
        fclose (probe);
        free (dst);
        return 0; /* keep an existing hand-maintained copy */
      }
  }
  if (src_hint && src_hint[0])
    {
      FILE *in = fopen (src_hint, "rb");
      if (in)
        {
          FILE *out = fopen (dst, "wb");
          char buf[8192];
          size_t n;
          if (!out)
            {
              fclose (in);
              goto fallback;
            }
          while ((n = fread (buf, 1, sizeof (buf), in)) > 0)
            if (fwrite (buf, 1, n, out) != n)
              {
                fclose (in);
                fclose (out);
                goto fallback;
              }
          fclose (in);
          fclose (out);
          free (dst);
          return 0;
        }
    }
fallback:
  /* Minimal fallback header so generated code still compiles. */
  {
    static const char fallback[] =
        "#ifndef MOOSEDOG_H\n#define MOOSEDOG_H\n"
        "#include <stddef.h>\n"
        "#define MOOSEDOG_VERSION_STRING \"1.0.0\"\n"
        "typedef struct moosedog_info {\n"
        "  const char *grammar_name;\n  const char *grammar_version;\n"
        "  const char *grammar_date;\n  const char *standard_url;\n"
        "  const char *license;\n  int use_atn;\n  int adaptive_lookahead;\n"
        "  int lookahead_depth;\n  int error_recovery;\n  int max_errors;\n"
        "  int strict_unicode;\n  int debug_level;\n} moosedog_info_t;\n"
        "const char *moosedog_version (void);\n"
        "#endif\n";
    int rc = write_file (dst, fallback, err);
    free (dst);
    return rc;
  }
}

/* Build dir + "/" + stem + suffix as a malloc'd string, or NULL w/ err.
 * An empty stem skips the separator (used for "Makefile.<Base>" style). */
static char *
make_out_path (const char *dir, const char *stem, const char *suffix,
               md_error_t *err)
{
  int need = stem[0] ? snprintf (NULL, 0, "%s/%s%s", dir, stem, suffix)
                     : snprintf (NULL, 0, "%s%s", dir, suffix);
  char *path;
  if (need < 0)
    {
      if (err)
        snprintf (err->message, sizeof (err->message), "bad output path");
      return NULL;
    }
  path = malloc ((size_t) need + 1);
  if (!path)
    {
      if (err)
        snprintf (err->message, sizeof (err->message),
                  "out of memory for output path");
      return NULL;
    }
  if (stem[0])
    snprintf (path, (size_t) need + 1, "%s/%s%s", dir, stem, suffix);
  else
    snprintf (path, (size_t) need + 1, "%s%s", dir, suffix);
  return path;
}

static int
emit_one (sbuf_t *sb, const char *path, md_error_t *err)
{
  if (sb->failed || !sb->data)
    {
      if (err)
        snprintf (err->message, sizeof (err->message),
                  "out of memory generating %s", path);
      free (sb->data);
      memset (sb, 0, sizeof (*sb));
      return -1;
    }
  if (write_file (path, sb->data, err) != 0)
    {
      free (sb->data);
      memset (sb, 0, sizeof (*sb));
      return -1;
    }
  free (sb->data);
  memset (sb, 0, sizeof (*sb));
  return 0;
}

int
md_emit_all (const md_spec_t *spec, const md_build_config_t *cfg,
             const md_emit_options_t *opt, md_error_t *err)
{
  char base[256];
  char dir[4096];
  md_build_config_t local;
  if (!cfg)
    {
      md_entrypoint_resolve (spec, &local);
      cfg = &local;
    }
  if (opt && opt->stem && opt->stem[0])
    {
      if (snprintf (base, sizeof (base), "%s", opt->stem) >= (int) sizeof (base))
        {
          if (err)
            snprintf (err->message, sizeof (err->message),
                      "basename too long");
          return -1;
        }
    }
  else
    md_emit_default_basename (spec, NULL, base, sizeof (base));
  {
    const char *d = (opt && opt->output_dir && opt->output_dir[0])
                        ? opt->output_dir
                        : ".";
    if (snprintf (dir, sizeof (dir), "%s", d) >= (int) sizeof (dir))
      {
        if (err)
          snprintf (err->message, sizeof (err->message),
                    "output directory path too long");
        return -1;
      }
  }
  if (mkdir_p (dir) != 0)
    {
      if (err)
        snprintf (err->message, sizeof (err->message), "cannot create %.512s", dir);
      return -1;
    }
  {
    sbuf_t sb;
    char *path;
    memset (&sb, 0, sizeof (sb));
    emit_ast_h (spec, base, &sb);
    path = make_out_path (dir, base, ".ast.h", err);
    if (!path || emit_one (&sb, path, err) != 0)
      {
        free (path);
        return -1;
      }
    free (path);
    memset (&sb, 0, sizeof (sb));
    emit_lexer_h (spec, cfg, base, &sb);
    path = make_out_path (dir, base, ".lexer.h", err);
    if (!path || emit_one (&sb, path, err) != 0)
      {
        free (path);
        return -1;
      }
    free (path);
    memset (&sb, 0, sizeof (sb));
    emit_parser_h (spec, cfg, base, &sb);
    path = make_out_path (dir, base, ".parser.h", err);
    if (!path || emit_one (&sb, path, err) != 0)
      {
        free (path);
        return -1;
      }
    free (path);
    memset (&sb, 0, sizeof (sb));
    emit_parser_c (spec, cfg, base, opt, &sb);
    path = make_out_path (dir, base, ".parser.c", err);
    if (!path || emit_one (&sb, path, err) != 0)
      {
        free (path);
        return -1;
      }
    free (path);
    if (opt && opt->emit_makefile)
      {
        char suffix[280];
        memset (&sb, 0, sizeof (sb));
        emit_makefile (base, &sb);
        if (snprintf (suffix, sizeof (suffix), "/Makefile.%s", base)
              >= (int) sizeof (suffix))
          {
            if (err)
              snprintf (err->message, sizeof (err->message),
                        "basename too long");
            free (sb.data);
            return -1;
          }
        path = make_out_path (dir, "", suffix, err);
        if (!path || emit_one (&sb, path, err) != 0)
          {
            free (path);
            return -1;
          }
        free (path);
      }
    if (opt && opt->emit_cmakefile)
      {
        char suffix[280];
        memset (&sb, 0, sizeof (sb));
        emit_cmake (base, &sb);
        if (snprintf (suffix, sizeof (suffix), "/CMakeLists.%s.txt", base)
              >= (int) sizeof (suffix))
          {
            if (err)
              snprintf (err->message, sizeof (err->message),
                        "basename too long");
            free (sb.data);
            return -1;
          }
        path = make_out_path (dir, "", suffix, err);
        if (!path || emit_one (&sb, path, err) != 0)
          {
            free (path);
            return -1;
          }
        free (path);
      }
  }
  if (copy_moosedog_h (dir, opt ? opt->moosedog_include : NULL, err) != 0)
    return -1;
  return 0;
}
