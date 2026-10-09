#ifndef MOOSEDOG_DISAMB_H
#define MOOSEDOG_DISAMB_H
/* moosedog_disamb.h -- Disambiguator imports (disambstd bridge). */
#include "../parser/moosedog_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Validate ImportDisamb entries: expect 'file, "%Hook"' shape and a
 * known disambstd hook name. Returns 0 or -1 w/ err. */
int md_disamb_validate (const md_spec_t *spec, md_error_t *err);

/* Extract the hook name ("%IsCaseSensitive") from an entry, or NULL. */
const char *md_disamb_hook_of (const char *entry);

#ifdef __cplusplus
}
#endif

#endif /* MOOSEDOG_DISAMB_H */
