/* Locale-independent UTF-8, grapheme and terminal escape helpers. Private. */
#ifndef TS_STD_TEXT_H
#define TS_STD_TEXT_H
#include "common/ts_std_common.h"
#include <utf8proc.h>

#define TS_TEXT_LIMIT (1024u * 1024u)
/* Strict decode: positive byte count, -1 for malformed/truncated input. */
int ts_utf8_decode (const char *s, size_t n, int32_t *cp);
TS_Status ts_utf8_validate (const char *s, size_t n, TS_Error *error);
/* One extended grapheme cluster, 0 at end, -1 for invalid UTF-8. */
int ts_text_cluster (const char *s, size_t n, size_t *bytes, int *width);
/* Escape-aware width, replacement width for invalid bytes. Controls are zero
 * width; tabs use eight-column stops; newlines start a new line. Returns the
 * widest line. Readline nonprinting regions are ignored. */
size_t ts_text_width (const char *s, size_t n);
size_t ts_text_escape (const char *s, size_t n);
int ts_text_strip (ts_sbuf_t *b, const char *s, size_t n);
/* Truncate at grapheme boundaries to a column budget (ANSI retained). */
int ts_text_clip (ts_sbuf_t *b, const char *s, size_t n, size_t columns);
#endif
