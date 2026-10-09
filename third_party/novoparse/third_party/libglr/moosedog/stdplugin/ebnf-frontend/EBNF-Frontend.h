#ifndef EBNF_FRONTEND_H
#define EBNF_FRONTEND_H
/* EBNF-Frontend.h -- EBNF syntax translated to .grm Syntactic blocks. */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Translate EBNF source ("rule ::= alt | alt ;") into a minimal .grm
 * document (Entrypoint + Syntactic). Returns 0 with malloc'd *out_grm. */
int ebnf_to_grm (const char *source, size_t len, const char *grammar_name,
                 char **out_grm, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* EBNF_FRONTEND_H */
