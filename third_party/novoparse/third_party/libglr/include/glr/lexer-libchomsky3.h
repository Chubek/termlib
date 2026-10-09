#ifndef GLR_LEXER_LIBCHOMSKY3_H
#define GLR_LEXER_LIBCHOMSKY3_H

#include <glr/lexer-hooks.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @file lexer-libchomsky3.h
   * @brief libchomsky3 ERE lexer adapter built on @ref glr_lexer_hooks_t.
   *
   * Rules are kept in a CTL vector. Input handling mirrors the RE2/PCRE2
   * adapters: the UTF-16 window at `event->byte_offset` is transcoded to
   * UTF-8, the longest anchored match wins (ties: lower priority, then
   * registration order), and the UTF-8 length is mapped back to UTF-16
   * bytes.
   *
   * When `third_party/libchomsky3` is enabled at build time
   * (`GLR_LEXER_USE_CHOMSKY3`), patterns compile with
   * `chomsky3_compile()` (bytecode target) and execute with
   * `chomsky3_exec()`. Otherwise the adapter uses POSIX ERE with an
   * equivalent anchoring, so it always builds.
   */

  typedef struct glr_lexer_libchomsky3 glr_lexer_libchomsky3_t;

  /** Per-pattern options (NULL means defaults). */
  typedef struct
  {
    bool case_insensitive; /**< Case-insensitive match. */
    bool multiline;        /**< ^/$ match at line boundaries. */
    bool dotall;           /**< '.' matches newline. */
    bool optimize;         /**< Request optimizer/JIT path when available. */
  } glr_lexer_libchomsky3_options_t;

  glr_lexer_libchomsky3_t *glr_lexer_libchomsky3_create (void);
  void glr_lexer_libchomsky3_destroy (glr_lexer_libchomsky3_t *lexer);
  void glr_lexer_libchomsky3_clear (glr_lexer_libchomsky3_t *lexer);

  int glr_lexer_libchomsky3_add (glr_lexer_libchomsky3_t *lexer,
                                 const char *pattern, const char *terminal,
                                 int priority,
                                 const glr_lexer_libchomsky3_options_t *options);
  int glr_lexer_libchomsky3_add_literal (glr_lexer_libchomsky3_t *lexer,
                                         const void *literal, size_t length,
                                         const char *terminal, int priority);
  int glr_lexer_libchomsky3_add_skip (glr_lexer_libchomsky3_t *lexer,
                                      const char *pattern,
                                      const glr_lexer_libchomsky3_options_t *options);

  size_t glr_lexer_libchomsky3_rule_count (const glr_lexer_libchomsky3_t *lexer);
  const char *glr_lexer_libchomsky3_last_error (
      const glr_lexer_libchomsky3_t *lexer);

  bool glr_lexer_libchomsky3_hook (const glr_lexer_event_t *event,
                                   glr_lexer_response_t *response,
                                   void *user_data);

  int glr_lexer_libchomsky3_install (glr_lexer_libchomsky3_t *lexer,
                                     glr_lexer_hooks_t *hooks,
                                     const char *hook_name, int hook_priority);

#ifdef __cplusplus
}
#endif

#endif /* GLR_LEXER_LIBCHOMSKY3_H */
