#ifndef MOOSEDOG_LEXER_H
#define MOOSEDOG_LEXER_H
/* moosedog_lexer.h -- Lexical block helpers: validation and C escaping. */
#include "../parser/moosedog_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Validate one token definition: must have a Pattern or a Classifier
 * (TYPEDEF_NAME style), patterns must be nonempty and brace-balanced.
 * PCRE-only constructs libglr's POSIX engine cannot honor (lookarounds,
 * \p classes, \d\s\w shorthands) are rejected; `(?:` groups and `\xHH`
 * escapes are accepted because the emitter normalizes them (see
 * md_lexer_normalize_pattern). Literal spellings skip regex checks.
 * Returns 0 when valid, -1 with err set otherwise. */
int md_lexer_validate (const md_spec_t *spec, md_error_t *err);

/* Emit a C string literal for a token pattern (adds quotes + escapes).
 * out must hold the result; returns 0 on success, -1 on truncation. */
int md_lexer_c_escape (const char *pattern, char *out, size_t out_size);

/* Count of non-skip tokens. */
size_t md_lexer_token_count (const md_spec_t *spec);
/* Count of skip (trivia) tokens. */
size_t md_lexer_skip_count (const md_spec_t *spec);

/* Normalize PCRE-isms in a token pattern into POSIX ERE that libglr's
 * scannerless engine honors:
 *   \xHH  -> the literal byte HH (so [^\x00-\x1f] really excludes
 *            controls instead of excluding the letter 'x'). A NUL byte
 *            cannot appear in a C-string regex: \x00 drops out, except
 *            \x00- which becomes \x01- (identical on text input, where
 *            NUL cannot occur).
 *   (?:   -> (   (non-capturing groups capture; libglr only needs the
 *            match span, so this is behavior-preserving here).
 * Escaped backslashes (\\) are never treated as introducing one of the
 * above. Returns a malloc'd string, or NULL on allocation failure. */
char *md_lexer_normalize_pattern (const char *pattern);

#ifdef __cplusplus
}
#endif

#endif /* MOOSEDOG_LEXER_H */
