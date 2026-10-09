/* moosedog_lexer.c -- lexical validation and C escaping. */
#include "moosedog_lexer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
hex_val (char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

char *
md_lexer_normalize_pattern (const char *pattern)
{
  size_t n, cap, o = 0;
  char *out;
  size_t i = 0;
  if (!pattern)
    return NULL;
  n = strlen (pattern);
  cap = n + 1;
  out = malloc (cap);
  if (!out)
    return NULL;
  while (pattern[i])
    {
      /* Escaped backslash pair: verbatim, never introduces \xHH. */
      if (pattern[i] == '\\' && pattern[i + 1] == '\\')
        {
          if (o + 2 >= cap)
            goto grow;
          out[o++] = '\\';
          out[o++] = '\\';
          i += 2;
          continue;
        }
      if (pattern[i] == '\\' && pattern[i + 1] == 'x'
          && hex_val (pattern[i + 2]) >= 0 && hex_val (pattern[i + 3]) >= 0)
        {
          unsigned byte = ((unsigned) hex_val (pattern[i + 2]) << 4)
                          | (unsigned) hex_val (pattern[i + 3]);
          if (byte == 0 && pattern[i + 4] == '-')
            byte = 1; /* \x00- means 0x00..: NUL is unmatchable, start at 1 */
          if (byte != 0)
            {
              if (o + 1 >= cap)
                goto grow;
              out[o++] = (char) byte;
            }
          i += 4;
          continue;
        }
      if (pattern[i] == '(' && pattern[i + 1] == '?' && pattern[i + 2] == ':')
        {
          if (o + 1 >= cap)
            goto grow;
          out[o++] = '(';
          i += 3;
          continue;
        }
      if (o + 1 >= cap)
        goto grow;
      out[o++] = pattern[i++];
      continue;
    grow:
      {
        char *nb;
        cap *= 2;
        nb = realloc (out, cap);
        if (!nb)
          {
            free (out);
            return NULL;
          }
        out = nb;
      }
      /* Re-process the pending character after growing. */
      continue;
    }
  out[o] = '\0';
  return out;
}

/* Reject PCRE-only constructs the POSIX engine cannot honor. The input
 * is the raw pattern; escaped backslash pairs are skipped so `\\d`
 * (match a literal backslash + 'd') is not misread as a \d class. */
static int
check_pcre (const char *pattern, const char *tname, md_error_t *err)
{
  size_t i = 0;
  while (pattern[i])
    {
      if (pattern[i] == '\\' && pattern[i + 1] == '\\')
        {
          i += 2;
          continue;
        }
      if (pattern[i] == '\\'
          && (pattern[i + 1] == 'd' || pattern[i + 1] == 'D'
              || pattern[i + 1] == 's' || pattern[i + 1] == 'S'
              || pattern[i + 1] == 'w' || pattern[i + 1] == 'W'
              || pattern[i + 1] == 'p' || pattern[i + 1] == 'P'))
        {
          if (err)
            snprintf (err->message, sizeof (err->message),
                      "token %.128s uses PCRE-only escape \\%.1s "
                      "(POSIX ERE has no shorthand classes)",
                      tname, pattern + i + 1);
          return -1;
        }
      if (pattern[i] == '(' && pattern[i + 1] == '?'
          && pattern[i + 2] != ':' && pattern[i + 2] != '\0')
        {
          if (err)
            snprintf (err->message, sizeof (err->message),
                      "token %.128s uses PCRE-only group '(?%.1s' "
                      "(only '(?:' is supported)",
                      tname, pattern + i + 2);
          return -1;
        }
      i++;
    }
  return 0;
}

int
md_lexer_validate (const md_spec_t *spec, md_error_t *err)
{
  for (size_t i = 0; i < spec->token_count; i++)
    {
      const md_token_def_t *t = &spec->tokens[i];
      if (!t->name || !t->name[0])
        {
          if (err)
            snprintf (err->message, sizeof (err->message),
                      "token %zu has no name", i);
          return -1;
        }
      /* Duplicate check. */
      for (size_t j = 0; j < i; j++)
        if (!strcmp (spec->tokens[j].name, t->name))
          {
            if (err)
              snprintf (err->message, sizeof (err->message),
                        "duplicate token %.128s", t->name);
            return -1;
          }
      if ((!t->pattern || !t->pattern[0]) && (!t->classifier || !t->classifier[0]))
        {
          if (err)
            snprintf (err->message, sizeof (err->message),
                      "token %.128s needs a Pattern or Classifier", t->name);
          return -1;
        }
      if (t->pattern && t->pattern[0] && !t->literal)
        {
          /* Balanced brackets/parens heuristic for regex sanity.
           * Literal spellings (Literal = Yes) are raw text, not regexes,
           * so "[", "(", "{" etc. are valid and skip this check. */
          int paren = 0, bracket = 0;
          int esc = 0;
          for (const char *p = t->pattern; *p; p++)
            {
              if (esc)
                {
                  esc = 0;
                  continue;
                }
              if (*p == '\\')
                {
                  esc = 1;
                  continue;
                }
              if (*p == '(')
                paren++;
              else if (*p == ')')
                paren--;
              else if (*p == '[')
                bracket++;
              else if (*p == ']')
                bracket--;
              if (paren < 0 || bracket < 0)
                {
                  if (err)
                    snprintf (err->message, sizeof (err->message),
                              "token %.128s has unbalanced pattern", t->name);
                  return -1;
                }
            }
          if (paren != 0 || bracket != 0)
            {
              if (err)
                snprintf (err->message, sizeof (err->message),
                          "token %.128s has unbalanced pattern", t->name);
              return -1;
            }
          if (check_pcre (t->pattern, t->name, err) != 0)
            return -1;
        }
    }
  return 0;
}

int
md_lexer_c_escape (const char *pattern, char *out, size_t out_size)
{
  size_t n = 0;
  if (!pattern)
    pattern = "";
  if (out_size == 0)
    return -1;
  out[n++] = '"';
  for (const char *p = pattern; *p; p++)
    {
      const char *esc = NULL;
      char tmp[8];
      switch (*p)
        {
        case '\\':
          esc = "\\\\";
          break;
        case '"':
          esc = "\\\"";
          break;
        case '\n':
          esc = "\\n";
          break;
        case '\r':
          esc = "\\r";
          break;
        case '\t':
          esc = "\\t";
          break;
        default:
          tmp[0] = *p;
          tmp[1] = '\0';
          esc = tmp;
          break;
        }
      size_t L = strlen (esc);
      if (n + L + 2 > out_size)
        return -1;
      memcpy (out + n, esc, L);
      n += L;
    }
  if (n + 2 > out_size)
    return -1;
  out[n++] = '"';
  out[n] = '\0';
  return 0;
}

size_t
md_lexer_token_count (const md_spec_t *spec)
{
  size_t n = 0;
  for (size_t i = 0; i < spec->token_count; i++)
    if (!spec->tokens[i].is_skip)
      n++;
  return n;
}

size_t
md_lexer_skip_count (const md_spec_t *spec)
{
  size_t n = 0;
  for (size_t i = 0; i < spec->token_count; i++)
    if (spec->tokens[i].is_skip)
      n++;
  return n;
}
