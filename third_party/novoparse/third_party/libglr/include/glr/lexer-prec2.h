#ifndef GLR_LEXER_PREC2_H
#define GLR_LEXER_PREC2_H

#include <glr/lexer-hooks.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @file lexer-prec2.h
   * @brief PCRE2-style anchored lexer adapter ("prec2").
   *
   * Same rule-vector / longest-match / UTF-16-to-UTF-8 architecture as the
   * RE2 adapter. When the PCRE2 8-bit library is available at build time
   * (`third_party/PCRE2`, `PCRE2_CODE_UNIT_WIDTH=8`, `GLR_LEXER_USE_PCRE2`),
   * patterns compile with `pcre2_compile_8()` and match with
   * `pcre2_match_8()` anchored at the cursor. Otherwise this translation
   * unit falls back to POSIX extended regex with a documented PCRE2 option
   * mapping (caseless/multiline/dotall/utf/ucp), so the adapter always
   * builds and behaves predictably.
   */

  typedef struct glr_lexer_prec2 glr_lexer_prec2_t;

  /** Per-pattern options (NULL means defaults: all false/zero). */
  typedef struct
  {
    bool caseless;   /**< Case-insensitive match. */
    bool multiline;  /**< ^/$ match at line boundaries. */
    bool dotall;     /**< '.' matches newline. */
    bool utf;        /**< UTF-8 mode. */
    bool ucp;        /**< Unicode character properties. */
    bool jit;        /**< Request JIT compile when PCRE2+JIT is present. */
    uint32_t extra_compile_options; /**< Raw PCRE2 bitmask OR-ed in. */
  } glr_lexer_prec2_options_t;

  glr_lexer_prec2_t *glr_lexer_prec2_create (void);
  void glr_lexer_prec2_destroy (glr_lexer_prec2_t *lexer);
  void glr_lexer_prec2_clear (glr_lexer_prec2_t *lexer);

  int glr_lexer_prec2_add (glr_lexer_prec2_t *lexer, const char *pattern,
                           const char *terminal, int priority,
                           const glr_lexer_prec2_options_t *options);
  int glr_lexer_prec2_add_literal (glr_lexer_prec2_t *lexer,
                                   const void *literal, size_t length,
                                   const char *terminal, int priority);
  int glr_lexer_prec2_add_skip (glr_lexer_prec2_t *lexer, const char *pattern,
                                const glr_lexer_prec2_options_t *options);

  size_t glr_lexer_prec2_rule_count (const glr_lexer_prec2_t *lexer);
  const char *glr_lexer_prec2_last_error (const glr_lexer_prec2_t *lexer);

  bool glr_lexer_prec2_hook (const glr_lexer_event_t *event,
                             glr_lexer_response_t *response, void *user_data);

  int glr_lexer_prec2_install (glr_lexer_prec2_t *lexer,
                               glr_lexer_hooks_t *hooks, const char *hook_name,
                               int hook_priority);

#ifdef __cplusplus
}
#endif

#endif /* GLR_LEXER_PREC2_H */
