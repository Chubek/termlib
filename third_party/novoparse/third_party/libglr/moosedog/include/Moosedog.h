#ifndef MOOSEDOG_H
#define MOOSEDOG_H
/*
 * Moosedog.h -- runtime header for Moosedog-generated parsers.
 * A generated <Grammar>.parser.c builds a libglr grammar programmatically;
 * this header provides version metadata, small helpers shared by generated
 * code, and the entry points a host application uses.
 *
 * Generated parsers always include <glr/glr.h> themselves; this header
 * does not require libglr so that tooling can inspect versions without it.
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOOSEDOG_VERSION_MAJOR 1
#define MOOSEDOG_VERSION_MINOR 0
#define MOOSEDOG_VERSION_PATCH 0
#define MOOSEDOG_VERSION_STRING "1.0.0"
#define MOOSEDOG_GRM_DIALECT "1"

/* Grammar metadata carried by every generated parser. */
typedef struct moosedog_info
{
  const char *grammar_name;
  const char *grammar_version;
  const char *grammar_date;
  const char *standard_url;
  const char *license;
  int use_atn;
  int adaptive_lookahead;
  int lookahead_depth;
  int error_recovery;
  int max_errors;
  int strict_unicode;
  int debug_level;
} moosedog_info_t;

const char *moosedog_version (void);

/* Expand ${VAR} and $VAR against the process environment.
 * Returns 0 on success; out is always NUL-terminated on success.
 * Unknown variables expand to the empty string. */
int moosedog_expand_env (const char *in, char *out, size_t out_size);

/* Join dir + "/" + file into out. Returns 0 on success, -1 on truncation. */
int moosedog_join_path (const char *dir, const char *file, char *out,
                        size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* MOOSEDOG_H */
