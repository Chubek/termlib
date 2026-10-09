/* moosedog_lua.c -- Lua query validation. */
#include "moosedog_lua.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int
md_lua_validate (const md_spec_t *spec, const char *base_dir, md_error_t *err)
{
  for (size_t i = 0; i < spec->entry.query_count; i++)
    {
      const char *q = spec->entry.queries[i];
      size_t L = strlen (q);
      if (L < 5 || strcmp (q + L - 4, ".lua"))
        {
          if (err)
            snprintf (err->message, sizeof (err->message),
                      "query \"%s\" must be a .lua file", q);
          return -1;
        }
      if (base_dir && base_dir[0])
        {
          char path[4096];
          int need = snprintf (NULL, 0, "%s/%s", base_dir, q);
          FILE *f;
          long fsize;
          char *content;
          char synerr[256];
          if (need < 0 || need >= (int) sizeof (path))
            {
              if (err)
                snprintf (err->message, sizeof (err->message),
                          "query path too long: %.256s/%.256s", base_dir, q);
              return -1;
            }
          snprintf (path, sizeof (path), "%s/%s", base_dir, q);
          f = fopen (path, "rb");
          if (!f)
            {
              if (err)
                snprintf (err->message, sizeof (err->message),
                          "query file not found: %.768s", path);
              return -1;
            }
          /* Bound the read: queries are small helpers, not data files. */
          if (fseek (f, 0, SEEK_END) != 0)
            {
              fclose (f);
              if (err)
                snprintf (err->message, sizeof (err->message),
                          "cannot seek query file: %.768s", path);
              return -1;
            }
          fsize = ftell (f);
          rewind (f);
          if (fsize < 0 || fsize > (1 << 20))
            {
              fclose (f);
              if (err)
                snprintf (err->message, sizeof (err->message),
                          "query file too large: %.768s", path);
              return -1;
            }
          content = malloc ((size_t) fsize + 1);
          if (!content)
            {
              fclose (f);
              return -1;
            }
          if (fsize > 0
              && fread (content, 1, (size_t) fsize, f) != (size_t) fsize)
            {
              free (content);
              fclose (f);
              if (err)
                snprintf (err->message, sizeof (err->message),
                          "cannot read query file: %.768s", path);
              return -1;
            }
          content[fsize] = '\0';
          fclose (f);
          if (md_lua_check_syntax (content, (size_t) fsize, synerr,
                                   sizeof (synerr))
              != 0)
            {
              if (err)
                snprintf (err->message, sizeof (err->message),
                          "query %.700s: %.200s", path, synerr);
              free (content);
              return -1;
            }
          free (content);
        }
    }
  return 0;
}

static int
is_bound (char c)
{
  return !(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
           || (c >= '0' && c <= '9'));
}

int
md_lua_check_syntax (const char *source, size_t len, char *err,
                     size_t err_size)
{
  /* Depth tracking over Lua block structure, ignoring strings and
   * comments. Openers (function/if/for/while/repeat) each consume one
   * end/until. `then`/`do`/`else`/`elseif` are deliberately not counted:
   * elseif carries a `then` without adding an `end`. Catches truncation
   * and stray closers. */
  int depth_comment = 0;
  int depth = 0;
  size_t i = 0;
  (void) len;
  while (source[i])
    {
      int is_bound_before = (i == 0 || is_bound (source[i - 1]));
      if (depth_comment)
        {
          if (!strncmp (source + i, "]]", 2))
            {
              depth_comment = 0;
              i += 2;
            }
          else
            i++;
          continue;
        }
      if (!strncmp (source + i, "--[[", 4))
        {
          depth_comment = 1;
          i += 4;
          continue;
        }
      if (!strncmp (source + i, "--", 2))
        {
          while (source[i] && source[i] != '\n')
            i++;
          continue;
        }
      if (source[i] == '"' || source[i] == '\'')
        {
          char q = source[i++];
          while (source[i] && source[i] != q)
            {
              if (source[i] == '\\' && source[i + 1])
                i += 2;
              else
                i++;
            }
          if (source[i])
            i++;
          continue;
        }
      if (is_bound_before
          && ((!strncmp (source + i, "function", 8) && is_bound (source[i + 8]))
              || (!strncmp (source + i, "repeat", 6)
                  && is_bound (source[i + 6]))))
        {
          depth++;
          i += (source[i] == 'f') ? 8 : 6;
          continue;
        }
      if (is_bound_before
          && ((!strncmp (source + i, "if", 2) && is_bound (source[i + 2]))
              || (!strncmp (source + i, "for", 3) && is_bound (source[i + 3]))
              || (!strncmp (source + i, "while", 5)
                  && is_bound (source[i + 5]))))
        {
          depth++;
          i += (source[i] == 'i') ? 2 : (source[i] == 'f' ? 3 : 5);
          continue;
        }
      if ((i == 0 || is_bound (source[i - 1]))
          && (!strncmp (source + i, "end", 3) && is_bound (source[i + 3])))
        {
          depth--;
          if (depth < 0)
            {
              if (err)
                snprintf (err, err_size, "stray 'end' without opener");
              return -1;
            }
          i += 3;
          continue;
        }
      if ((i == 0 || is_bound (source[i - 1]))
          && (!strncmp (source + i, "until", 5) && is_bound (source[i + 5])))
        {
          depth--;
          if (depth < 0)
            {
              if (err)
                snprintf (err, err_size, "stray 'until' without 'repeat'");
              return -1;
            }
          i += 5;
          continue;
        }
      i++;
    }
  if (depth_comment)
    {
      if (err)
        snprintf (err, err_size, "unterminated --[[ comment");
      return -1;
    }
  if (depth != 0)
    {
      if (err)
        snprintf (err, err_size, "unbalanced blocks (depth %d)", depth);
      return -1;
    }
  return 0;
}
