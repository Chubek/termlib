#ifndef GLR_LEXER_RE2_H
#define GLR_LEXER_RE2_H

#include <glr/lexer-hooks.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @file lexer-re2.h
   * @brief RE2-style anchored lexer adapter built on @ref glr_lexer_hooks_t.
   *
   * Rules are stored in a CTL vector of heap-owned entries. Matching is
   * anchored at the reader cursor: the UTF-16 window starting at
   * `event->byte_offset` is transcoded to UTF-8, every rule is tried, and the
   * longest match wins (ties break by lower priority, then registration
   * order). The winning UTF-8 length is mapped back to a UTF-16 byte count
   * so `glr_reader_next()` advances correctly.
   *
   * The vendored engine lives in `third_party/re2` (C++). This translation
   * unit stays C11 and uses POSIX extended regular expressions with RE2
   * compatible anchoring (`^(?:pattern)`, leftmost-longest, no backrefs in
   * the supported subset). When a C++ RE2 binding is desired, compile the
   * optional C++ shim with `-DGLR_LEXER_RE2_USE_CXX` and link `third_party/re2`.
   */

  typedef struct glr_lexer_re2 glr_lexer_re2_t;

  /** Per-pattern options (NULL means defaults: all false). */
  typedef struct
  {
    bool case_insensitive; /**< REG_ICASE. */
    bool dot_nl;           /**< When false, '.' does not match newline. */
  } glr_lexer_re2_options_t;

  glr_lexer_re2_t *glr_lexer_re2_create (void);
  void glr_lexer_re2_destroy (glr_lexer_re2_t *lexer);
  void glr_lexer_re2_clear (glr_lexer_re2_t *lexer);

  int glr_lexer_re2_add (glr_lexer_re2_t *lexer, const char *pattern,
                         const char *terminal, int priority);
  int glr_lexer_re2_add_with_options (glr_lexer_re2_t *lexer,
                                      const char *pattern,
                                      const char *terminal, int priority,
                                      const glr_lexer_re2_options_t *options);
  int glr_lexer_re2_add_literal (glr_lexer_re2_t *lexer, const void *literal,
                                 size_t length, const char *terminal,
                                 int priority);
  int glr_lexer_re2_add_skip (glr_lexer_re2_t *lexer, const char *pattern);

  size_t glr_lexer_re2_rule_count (const glr_lexer_re2_t *lexer);
  const char *glr_lexer_re2_last_error (const glr_lexer_re2_t *lexer);

  bool glr_lexer_re2_hook (const glr_lexer_event_t *event,
                           glr_lexer_response_t *response, void *user_data);

  int glr_lexer_re2_install (glr_lexer_re2_t *lexer, glr_lexer_hooks_t *hooks,
                             const char *hook_name, int hook_priority);

#ifdef __cplusplus
}
#endif

#endif /* GLR_LEXER_RE2_H */
