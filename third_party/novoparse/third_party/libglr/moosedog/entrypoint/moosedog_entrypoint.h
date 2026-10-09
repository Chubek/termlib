#ifndef MOOSEDOG_ENTRYPOINT_H
#define MOOSEDOG_ENTRYPOINT_H
/* moosedog_entrypoint.h -- Entrypoint block interpretation. */
#include "../parser/moosedog_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct md_build_config
{
  const char *main_lexer;
  const char *fallback_lexer;
  int use_atn;
  int use_incremental;
  int use_cache;
  const char *cache_dir;
  const char *main_allocator;
  const char *fallback_allocator;
  int emit_listener;
  int emit_visitor;
  int cache_lexer;
  int cache_parser;
  int error_recovery;
  int max_errors;
  const char *recovery_strategy;
  const char *output_encoding;
  int strict_unicode;
  int debug_level;
  int ast_annotations;
  int adaptive_lookahead;
  int lookahead_depth;
  int preserve_ambiguity;
  int emit_statistics;
} md_build_config_t;

/* Resolve Config + ATN blocks into a build config with documented defaults.
 * Pointers borrow from spec (do not free). */
void md_entrypoint_resolve (const md_spec_t *spec, md_build_config_t *cfg);

/* Validate entrypoint: at least a language name, known lexer names,
 * lookahead depth in [1,64], max errors sane. Returns 0 or -1 w/ err. */
int md_entrypoint_validate (const md_spec_t *spec, md_error_t *err);

/* Find an Optparse option by long name ("--debug") or short ("-d"). */
const md_option_t *md_entrypoint_find_option (const md_spec_t *spec,
                                              const char *flag);

#ifdef __cplusplus
}
#endif

#endif /* MOOSEDOG_ENTRYPOINT_H */
