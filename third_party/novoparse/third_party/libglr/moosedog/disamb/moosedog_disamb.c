/* moosedog_disamb.c -- disambiguator import checks. */
#include "moosedog_disamb.h"

#include <string.h>
#include <stdio.h>

const char *
md_disamb_hook_of (const char *entry)
{
  const char *pct = strchr (entry ? entry : "", '%');
  return pct;
}

static int
hook_known (const char *hook)
{
  static const char *known[] = {
    "%IsCaseSensitive", "%LongestMatch", "%PreferAvoid", "%Precedence",
    "%Associativity", "%Lexical", "%Semantic", "%Predicate", "%Island",
    "%NameResolution", "%LayoutSensitive", "%Probability", "%TreeScore",
    "%PostParseFiltering", "%ScannerlessPriorities", "%AttributeGrammar",
    "%DynamicProgramming", "%BoundedAmbiguity", "%CounterExampleGuided",
    NULL
  };
  for (size_t i = 0; known[i]; i++)
    if (!strcmp (known[i], hook))
      return 1;
  /* Any %Hook spelling is accepted structurally; unknown hooks warn
   * at generation time but still emit a registration stub. */
  return 0;
}

int
md_disamb_validate (const md_spec_t *spec, md_error_t *err)
{
  (void) hook_known;
  for (size_t i = 0; i < spec->entry.disambiguator_count; i++)
    {
      const char *e = spec->entry.disambiguators[i];
      if (!strchr (e, '%'))
        {
          if (err)
            snprintf (err->message, sizeof (err->message),
                      "disambiguator \"%s\" must reference a %%Hook", e);
          return -1;
        }
    }
  return 0;
}
