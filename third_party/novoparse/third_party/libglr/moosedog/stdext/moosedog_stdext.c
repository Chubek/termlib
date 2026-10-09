/* moosedog_stdext.c -- canonical evaluation of Entrypoint helpers. */
#include "moosedog_stdext.h"

#include <stdio.h>
#include <string.h>

int
md_stdext_known (const char *name)
{
  return (!strcmp (name, "LanguageType") || !strcmp (name, "URL")
          || !strcmp (name, "GetLicense") || !strcmp (name, "Allocators")
          || !strcmp (name, "RecoveryMode") || !strcmp (name, "IntRange"));
}

const char *
md_stdext_eval (const char *call)
{
  static char buf[1024];
  const char *p;
  if (!call)
    return NULL;
  /* Calls arrive as e.g. 'LanguageType( "DataExchange" )' with inner
   * quotes preserved by the .grm parser. Normalize by trimming spaces. */
  if (!strncmp (call, "LanguageType(", 13))
    {
      p = strchr (call, '"');
      if (!p)
        return NULL;
      snprintf (buf, sizeof (buf), "LanguageType(%s", p);
      return buf;
    }
  if (!strncmp (call, "URL(", 4) || !strncmp (call, "GetLicense(", 11)
      || !strncmp (call, "Allocators(", 11) || !strncmp (call, "RecoveryMode(", 13)
      || !strncmp (call, "IntRange(", 9))
    return call;
  return NULL;
}
