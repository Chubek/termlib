#ifndef MOOSEDOG_REWRITE_H
#define MOOSEDOG_REWRITE_H
/* moosedog_rewrite.h -- Rewriter imports (rewritelib bridge). */
#include "../parser/moosedog_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Validate ImportRewriter entries: they must name a rewritelib program
 * (contain ".grl" or a known pass name). Returns 0 or -1 w/ err. */
int md_rewrite_validate (const md_spec_t *spec, md_error_t *err);

/* Map an entry to a rewritelib file stem (e.g. "eliminate_unproductive_symbols").
 * Returns a borrowed pointer into a static buffer, or NULL. */
const char *md_rewrite_stem_of (const char *entry);

#ifdef __cplusplus
}
#endif

#endif /* MOOSEDOG_REWRITE_H */
