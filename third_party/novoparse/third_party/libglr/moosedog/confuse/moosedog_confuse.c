/* moosedog_confuse.c -- env expansion for Config values. */
#include "moosedog_confuse.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int
md_conf_expand (const char *in, char *out, size_t out_size)
{
  size_t n = 0;
  if (!in)
    in = "";
  if (out_size == 0)
    return -1;
  while (*in)
    {
      if (*in == '$' && (in[1] == '{' || in[1] == '$' || in[1] == '('))
        {
          /* ${VAR} / $(VAR) */
          char close = in[1] == '{' ? '}' : ')';
          const char *s = in + 2;
          const char *e = strchr (s, close);
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
      else if (*in == '$'
               && ((in[1] >= 'A' && in[1] <= 'Z')
                   || (in[1] >= 'a' && in[1] <= 'z') || in[1] == '_'))
        {
          const char *s = in + 1;
          const char *e = s;
          char name[256];
          const char *val;
          while ((*e >= 'A' && *e <= 'Z') || (*e >= 'a' && *e <= 'z')
                 || (*e >= '0' && *e <= '9') || *e == '_')
            e++;
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
          in = e;
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
md_conf_resolve_dir (const char *raw, char *out, size_t out_size)
{
  char tmp[4096];
  if (out_size == 0)
    return -1;
  if (md_conf_expand (raw ? raw : "", tmp, sizeof (tmp)) != 0)
    return -1;
  /* The stock default is "${TMPDIR}/MoosedogCache". When TMPDIR is unset
   * the expansion yields "/MoosedogCache" — repair that to /tmp. An
   * entirely empty result also falls back to /tmp/MoosedogCache. */
  if (!strcmp (tmp, "/MoosedogCache") || !tmp[0])
    snprintf (tmp, sizeof (tmp), "/tmp/MoosedogCache");
  else if (!strncmp (tmp, "$", 1))
    {
      /* Unexpanded remainder (e.g. unknown shell syntax): keep literally
       * only when it is an absolute path already. */
      if (tmp[0] != '/')
        snprintf (tmp, sizeof (tmp), "/tmp/MoosedogCache");
    }
  if (snprintf (out, out_size, "%s", tmp) >= (int) out_size)
    return -1;
  return 0;
}
