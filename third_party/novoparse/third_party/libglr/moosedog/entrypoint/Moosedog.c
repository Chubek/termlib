/* Moosedog.c -- shared runtime for generated parsers. */
#include "Moosedog.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

const char *
moosedog_version (void)
{
  return MOOSEDOG_VERSION_STRING;
}

int
moosedog_expand_env (const char *in, char *out, size_t out_size)
{
  size_t n = 0;
  if (!in)
    in = "";
  if (out_size == 0)
    return -1;
  while (*in)
    {
      if (*in == '$' && in[1] == '{')
        {
          const char *s = in + 2;
          const char *e = strchr (s, '}');
          char name[256];
          const char *val;
          if (!e)
            e = s + strlen (s);
          if ((size_t) (e - s) >= sizeof (name))
            return -1;
          memcpy (name, s, (size_t) (e - s));
          name[e - s] = '\0';
          val = getenv (name);
          if (!val)
            val = "";
          while (*val)
            {
              if (n + 1 >= out_size)
                return -1;
              out[n++] = *val++;
            }
          in = *e ? e + 1 : e;
        }
      else
        {
          if (n + 1 >= out_size)
            return -1;
          out[n++] = *in++;
        }
    }
  out[n] = '\0';
  return 0;
}

int
moosedog_join_path (const char *dir, const char *file, char *out,
                    size_t out_size)
{
  int rc;
  if (!dir || !dir[0])
    rc = snprintf (out, out_size, "%s", file ? file : "");
  else if (!file || !file[0])
    rc = snprintf (out, out_size, "%s", dir);
  else
    {
      size_t L = strlen (dir);
      if (dir[L - 1] == '/')
        rc = snprintf (out, out_size, "%s%s", dir, file);
      else
        rc = snprintf (out, out_size, "%s/%s", dir, file);
    }
  return (rc < 0 || (size_t) rc >= out_size) ? -1 : 0;
}
