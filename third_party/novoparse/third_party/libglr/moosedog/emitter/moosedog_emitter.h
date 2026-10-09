#ifndef MOOSEDOG_EMITTER_H
#define MOOSEDOG_EMITTER_H
/* moosedog_emitter.h -- C artifact generation from an md_spec_t. */
#include "../parser/moosedog_parser.h"
#include "../entrypoint/moosedog_entrypoint.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct md_emit_options
{
  const char *stem;   /* e.g. "JSON" (defaults to grammar name) */
  const char *output_dir; /* defaults to "." */
  int emit_makefile;
  int emit_cmakefile;
  int debug;
  int no_rewrite;   /* skip rewrite emission */
  int max_errors;   /* -1 => use Config */
  int use_cache;    /* -1 => use Config, 0/1 override */
  const char *cache_dir; /* NULL => use Config */
  int strict_unicode;    /* -1 => use Config */
  const char *moosedog_include; /* path to Moosedog.h source for copying */
} md_emit_options_t;

/* Emit <basename>.ast.h, .lexer.h, .parser.h, .parser.c (and optionally
 * build files + Moosedog.h) into output_dir. Returns 0 on success. */
int md_emit_all (const md_spec_t *spec, const md_build_config_t *cfg,
                 const md_emit_options_t *opt, md_error_t *err);

/* Basename helpers: derive "JSON" from "JSON.grm" or grammar name. */
void md_emit_default_basename (const md_spec_t *spec, const char *grm_path,
                               char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* MOOSEDOG_EMITTER_H */
