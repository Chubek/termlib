/* moosedog_rewrite.c -- rewriter import checks. */
#include "moosedog_rewrite.h"

#include <string.h>
#include <stdio.h>

const char *
md_rewrite_stem_of (const char *entry)
{
  static char buf[256];
  const char *dot;
  size_t n;
  if (!entry)
    return NULL;
  /* Entries look like: rewritelib.eliminate_unproductive_symbols.grl
   * or ImportRewriter("...") canonical form preserved by the parser. */
  dot = strrchr (entry, '.');
  if (!dot)
    return entry;
  /* Find the segment before the final ".grl"). */
  const char *start = entry;
  const char *last_dot_before = NULL;
  for (const char *p = entry; p < dot; p++)
    if (*p == '.')
      last_dot_before = p;
  if (last_dot_before)
    start = last_dot_before + 1;
  n = (size_t) (dot - start);
  if (n >= sizeof (buf))
    n = sizeof (buf) - 1;
  memcpy (buf, start, n);
  buf[n] = '\0';
  return buf;
}

int
md_rewrite_validate (const md_spec_t *spec, md_error_t *err)
{
  for (size_t i = 0; i < spec->entry.rewriter_count; i++)
    {
      const char *e = spec->entry.rewriters[i];
      if (!strstr (e, ".grl") && !strstr (e, "eliminate")
          && !strstr (e, "remove") && !strstr (e, "rewrite")
          && !strstr (e, "left") && !strstr (e, "make-"))
        {
          if (err)
            snprintf (err->message, sizeof (err->message),
                      "rewriter \"%s\" does not name a .grl program", e);
          return -1;
        }
    }
  return 0;
}
