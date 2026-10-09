/* EBNF-Frontend.c -- minimal EBNF (*.ebnf) to .grm translator plugin. */
#include "EBNF-Frontend.h"
#include "../../include/Moosedog-Plugin.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct
{
  char *data;
  size_t len;
  size_t cap;
} buf_t;

static int
put (buf_t *b, const char *s)
{
  size_t L = strlen (s);
  if (b->len + L + 1 > b->cap)
    {
      size_t nc = b->cap ? b->cap * 2 : 4096;
      while (nc < b->len + L + 1)
        nc *= 2;
      {
        char *nd = realloc (b->data, nc);
        if (!nd)
          return -1;
        b->data = nd;
        b->cap = nc;
      }
    }
  memcpy (b->data + b->len, s, L);
  b->len += L;
  b->data[b->len] = '\0';
  return 0;
}

typedef struct
{
  const char *p;
  const char *end;
} cur_t;

static void
skip_ws (cur_t *c)
{
  while (c->p < c->end)
    {
      if (isspace ((unsigned char) *c->p))
        c->p++;
      else if (c->p + 1 < c->end && c->p[0] == '(' && c->p[1] == '*')
        {
          c->p += 2;
          while (c->p + 1 < c->end && !(c->p[0] == '*' && c->p[1] == ')'))
            c->p++;
          if (c->p + 1 < c->end)
            c->p += 2;
        }
      else
        break;
    }
}

static int
parse_ident (cur_t *c, char *out, size_t n)
{
  size_t k = 0;
  skip_ws (c);
  while (c->p < c->end
         && (isalnum ((unsigned char) *c->p) || *c->p == '_' || *c->p == '-'))
    {
      if (k + 1 < n)
        out[k++] = *c->p;
      c->p++;
    }
  out[k] = '\0';
  return k ? 0 : -1;
}

int
ebnf_to_grm (const char *source, size_t len, const char *grammar_name,
             char **out_grm, char *err, size_t err_size)
{
  cur_t c;
  buf_t rules;
  char first_rule[256] = { 0 };
  char *literals[256];
  size_t literal_count = 0;
  buf_t out;
  memset (&out, 0, sizeof (out));
  memset (&rules, 0, sizeof (rules));
  c.p = source;
  c.end = source + len;
  put (&rules, "");
  for (;;)
    {
      char name[256];
      skip_ws (&c);
      if (c.p >= c.end)
        break;
      if (parse_ident (&c, name, sizeof (name)) != 0)
        {
          if (err)
            snprintf (err, err_size, "expected rule name");
          free (rules.data);
          return -1;
        }
      skip_ws (&c);
      if (c.end - c.p < 3 || strncmp (c.p, "::=", 3))
        {
          if (err)
            snprintf (err, err_size, "expected ::= after %s", name);
          free (rules.data);
          return -1;
        }
      c.p += 3;
      if (!first_rule[0])
        snprintf (first_rule, sizeof (first_rule), "%s", name);
      {
        char tmp[1024];
        int wr = snprintf (tmp, sizeof (tmp), "    Rule(%s) -> CNode\n    {\n",
                               name);
        if (wr < 0 || (size_t) wr >= sizeof (tmp) || put (&rules, tmp) != 0)
          goto fail;
      }
      /* Alternatives separated by '|', terminated by ';'. */
      for (;;)
        {
          char seq[2048] = { 0 };
          size_t k = 0;
          skip_ws (&c);
          while (c.p < c.end && *c.p != '|' && *c.p != ';')
            {
              if (*c.p == '"' || *c.p == '\'')
                {
                  char q = *c.p++;
                  char tok[256];
                  size_t t = 0;
                  while (c.p < c.end && *c.p != q && t + 1 < sizeof (tok))
                    tok[t++] = *c.p++;
                  tok[t] = '\0';
                  if (c.p < c.end)
                    c.p++;
                  /* Quoted literals become declared terminals. */
                  {
                    size_t li;
                    char litname[32];
                    for (li = 0; li < literal_count; li++)
                      if (!strcmp (literals[li], tok))
                        break;
                    if (li == literal_count)
                      {
                        if (literal_count >= sizeof (literals) / sizeof (literals[0]))
                          {
                            if (err)
                              snprintf (err, err_size, "too many literals");
                            goto fail;
                          }
                        literals[literal_count] = strdup (tok);
                        if (!literals[literal_count])
                          goto fail;
                        literal_count++;
                      }
                    {
                      int wr;
                      snprintf (litname, sizeof (litname), "LIT_%zu", li + 1);
                      wr = snprintf (seq + k, sizeof (seq) - k, "%s%s",
                                     k ? " " : "", litname);
                      if (wr < 0 || (size_t) wr >= sizeof (seq) - k)
                        {
                          if (err)
                            snprintf (err, err_size, "alternative too long");
                          goto fail;
                        }
                      k += (size_t) wr;
                    }
                  }
                }
              else if (isalnum ((unsigned char) *c.p) || *c.p == '_'
                       || *c.p == '-')
                {
                  char w[256];
                  size_t t = 0;
                  while (c.p < c.end
                         && (isalnum ((unsigned char) *c.p) || *c.p == '_'
                             || *c.p == '-')
                         && t + 1 < sizeof (w))
                    w[t++] = *c.p++;
                  w[t] = '\0';
                  {
                    int wr = snprintf (seq + k, sizeof (seq) - k, "%s%s",
                                       k ? " " : "", w);
                    if (wr < 0 || (size_t) wr >= sizeof (seq) - k)
                      {
                        if (err)
                          snprintf (err, err_size, "alternative too long");
                        goto fail;
                      }
                    k += (size_t) wr;
                  }
                }
              else if (*c.p == '[' || *c.p == '{' || *c.p == '(')
                {
                  /* EBNF grouping: flatten to its contents. */
                  c.p++;
                }
              else if (*c.p == ']' || *c.p == '}' || *c.p == ')')
                {
                  c.p++;
                }
              else if (*c.p == '*' || *c.p == '+' || *c.p == '?')
                {
                  c.p++;
                }
              else
                c.p++;
              skip_ws (&c);
            }
          {
            char tmp[2350];
            int wr = snprintf (tmp, sizeof (tmp),
                               "        Alt { Seq = \"%s\" Action = \"return $1;\" }\n",
                               seq);
            if (wr < 0 || (size_t) wr >= sizeof (tmp))
              {
                if (err)
                  snprintf (err, err_size, "alternative too long");
                goto fail;
              }
            if (put (&rules, tmp) != 0)
              goto fail;
          }
          skip_ws (&c);
          if (c.p < c.end && *c.p == '|')
            {
              c.p++;
              continue;
            }
          if (c.p < c.end && *c.p == ';')
            {
              c.p++;
              break;
            }
          if (err)
            snprintf (err, err_size, "expected '|' or ';' in rule %s", name);
          for (size_t li = 0; li < literal_count; li++)
            free (literals[li]);
          free (rules.data);
          return -1;
        }
      if (put (&rules, "    }\n") != 0)
        goto fail;
    }
  if (!first_rule[0])
    {
      if (err)
        snprintf (err, err_size, "no rules found");
      for (size_t li = 0; li < literal_count; li++)
        free (literals[li]);
      free (rules.data);
      return -1;
    }
  {
    char tmp[1024];
    snprintf (tmp, sizeof (tmp),
              "Entrypoint\n{\n    LanguageInfo\n    {\n"
              "        Name = \"%s\"\n"
              "        Type = LanguageType(\"EBNFImport\")\n"
              "    }\n    Config { MainLexer = \"Scannerless\" }\n}\n\n",
              grammar_name ? grammar_name : "EBNF");
    if (put (&out, tmp) != 0)
      goto fail;
    if (literal_count)
      {
        put (&out, "Lexical\n{\n");
        for (size_t li = 0; li < literal_count; li++)
          {
            /* C-escape the literal for the .grm string. */
            char esc[512];
            size_t ek = 0;
            for (const char *p = literals[li]; *p && ek + 2 < sizeof (esc); p++)
              {
                if (*p == '\\' || *p == '"')
                  esc[ek++] = '\\';
                esc[ek++] = *p;
              }
            esc[ek] = '\0';
            {
              int wr = snprintf (tmp, sizeof (tmp),
                                 "    Token(LIT_%zu) { Pattern = \"%s\" Literal = Yes }\n",
                                 li + 1, esc);
              if (wr < 0 || (size_t) wr >= sizeof (tmp)
                  || put (&out, tmp) != 0)
                goto fail;
            }
          }
        if (put (&out, "}\n\n") != 0)
          goto fail;
      }
    snprintf (tmp, sizeof (tmp), "Syntactic\n{\n    StartRule = \"%s\"\n\n",
              first_rule);
    if (put (&out, tmp) != 0
        || put (&out, rules.data ? rules.data : "") != 0
        || put (&out, "}\n") != 0)
      goto fail;
    for (size_t li = 0; li < literal_count; li++)
      free (literals[li]);
    free (rules.data);
    *out_grm = out.data;
    return 0;
  }
fail:
  for (size_t li = 0; li < literal_count; li++)
    free (literals[li]);
  free (rules.data);
  free (out.data);
  out.data = NULL;
  if (err && !err[0])
    snprintf (err, err_size, "ebnf translation failed");
  return -1;
}

/* Plugin descriptor for dlopen discovery. */
static int
ebnf_translate (const char *source, size_t length, char **out_grm, char *err,
                size_t err_size)
{
  return ebnf_to_grm (source, length, "EBNF", out_grm, err, err_size);
}

static const moosedog_plugin_t ebnf_plugin = {
  MOOSEDOG_PLUGIN_ABI, "ebnf-frontend", "1.0.0", ".ebnf", ebnf_translate
};

const moosedog_plugin_t *
moosedog_plugin_entry (void)
{
  return &ebnf_plugin;
}
