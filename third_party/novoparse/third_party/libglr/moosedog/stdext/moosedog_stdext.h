#ifndef MOOSEDOG_STDEXT_H
#define MOOSEDOG_STDEXT_H
/* moosedog_stdext.h -- builtin Entrypoint helper functions. */
#ifdef __cplusplus
extern "C" {
#endif

/* Evaluate the canonical string form of a helper call produced by the
 * .grm parser (e.g. 'LanguageType( "DataExchange" )'). Returns a static
 * canonical string or NULL when the call is unknown/invalid. */
const char *md_stdext_eval (const char *call);

/* True when name is a known helper: LanguageType, URL, GetLicense,
 * Allocators, RecoveryMode, IntRange. */
int md_stdext_known (const char *name);

#ifdef __cplusplus
}
#endif

#endif /* MOOSEDOG_STDEXT_H */
