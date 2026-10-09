#ifndef MOOSEDOG_PLUGIN_H
#define MOOSEDOG_PLUGIN_H
/*
 * Moosedog-Plugin.h -- plugin interface for Moosedog frontends.
 *
 * A plugin is a shared object exporting `moosedog_plugin_entry` which
 * returns a pointer to a moosedog_plugin_t. The core registers the
 * plugin's frontend (e.g. an alternate grammar syntax such as EBNF)
 * and calls its translate callback to produce .grm source text.
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOOSEDOG_PLUGIN_ABI 1

typedef struct moosedog_plugin moosedog_plugin_t;

/* Translate alternate-syntax source text into .grm source text.
 * On success returns 0 and sets *out_grm to a malloc'd NUL-terminated
 * buffer owned by the caller. On failure returns nonzero and optionally
 * writes a diagnostic into err (err_size bytes). */
typedef int (*moosedog_translate_fn) (const char *source, size_t length,
                                      char **out_grm, char *err,
                                      size_t err_size);

struct moosedog_plugin
{
  int abi;                    /* Must equal MOOSEDOG_PLUGIN_ABI */
  const char *name;           /* e.g. "ebnf-frontend" */
  const char *version;        /* e.g. "1.0.0" */
  const char *input_suffix;   /* e.g. ".ebnf" */
  moosedog_translate_fn translate;
};

typedef const moosedog_plugin_t *(*moosedog_plugin_entry_fn) (void);

/* Well-known entry symbol every plugin shared object must export. */
const moosedog_plugin_t *moosedog_plugin_entry (void);

#ifdef __cplusplus
}
#endif

#endif /* MOOSEDOG_PLUGIN_H */
