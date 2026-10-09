/* RE2-style anchored lexer adapter for glr_lexer_hooks_t.
 *
 * Rule storage uses CTL vectors (third_party/ctl); matching uses POSIX ERE
 * with RE2-compatible anchoring. The vendored RE2 engine
 * (third_party/re2) is C++; this C11 unit intentionally stays link-free so
 * the adapter always builds. To use real RE2, compile the optional C++
 * shim with -DGLR_LEXER_RE2_USE_CXX and link re2. */

#include <glr/lexer-hooks.h>
#include <glr/lexer-re2.h>

#include "containers.h"

#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLR_RE2_WINDOW_MAX_CODEPOINTS 4096u
#define GLR_RE2_WINDOW_MAX_UTF8 65536u
#define GLR_RE2_SKIP_ITERATION_LIMIT 1024u

typedef struct glr_lexer_re2_rule glr_lexer_re2_rule_t;

struct glr_lexer_re2_rule
{
  regex_t compiled;
  bool has_compiled;
  bool is_skip;
  bool is_literal;
  char *pattern_text;
  char *terminal;
  unsigned char *literal;
  size_t literal_length;
  int priority;
  size_t order;
  bool case_insensitive;
  bool dot_nl;
};

typedef glr_lexer_re2_rule_t *glr_lexer_re2_rule_ptr_t;

#define T glr_lexer_re2_rule_ptr_t
#define P
#include <ctl/vec.h>

struct glr_lexer_re2
{
  vec_glr_lexer_re2_rule_ptr_t rules;
  size_t next_order;
  char error[256];
};

static char *
re2_strdup (const char *text)
{
  size_t length;
  char *copy;

  if (text == NULL)
    return NULL;
  length = strlen (text);
  copy = malloc (length + 1);
  if (copy == NULL)
    return NULL;
  memcpy (copy, text, length + 1);
  return copy;
}

static void
re2_set_error (glr_lexer_re2_t *lexer, const char *message)
{
  if (lexer == NULL)
    return;
  if (message == NULL)
    message = "unknown error";
  snprintf (lexer->error, sizeof (lexer->error), "%s", message);
}

static void
re2_rule_free (glr_lexer_re2_rule_t *rule)
{
  if (rule == NULL)
    return;
  if (rule->has_compiled)
    regfree (&rule->compiled);
  free (rule->pattern_text);
  free (rule->terminal);
  free (rule->literal);
  free (rule);
}

static bool
re2_input_is_be (const unsigned char *input, size_t length)
{
  return length >= 2 && input[0] == 0xFE && input[1] == 0xFF;
}

static uint16_t
re2_read_u16 (const unsigned char *p, bool be)
{
  if (be)
    return (uint16_t) (((uint16_t) p[0] << 8) | p[1]);
  return (uint16_t) (((uint16_t) p[1] << 8) | p[0]);
}

static size_t
re2_encode_utf8 (uint32_t codepoint, char out[4])
{
  if (codepoint < 0x80)
    {
      out[0] = (char) codepoint;
      return 1;
    }
  if (codepoint < 0x800)
    {
      out[0] = (char) (0xC0 | (codepoint >> 6));
      out[1] = (char) (0x80 | (codepoint & 0x3F));
      return 2;
    }
  if (codepoint < 0x10000)
    {
      out[0] = (char) (0xE0 | (codepoint >> 12));
      out[1] = (char) (0x80 | ((codepoint >> 6) & 0x3F));
      out[2] = (char) (0x80 | (codepoint & 0x3F));
      return 3;
    }
  out[0] = (char) (0xF0 | (codepoint >> 18));
  out[1] = (char) (0x80 | ((codepoint >> 12) & 0x3F));
  out[2] = (char) (0x80 | ((codepoint >> 6) & 0x3F));
  out[3] = (char) (0x80 | (codepoint & 0x3F));
  return 4;
}

/* Transcode [base, input_length) from UTF-16 to a NUL-terminated UTF-8
   window. Returns 0 on success, -1 when the byte range is not well-formed
   UTF-16 (caller should try the raw-byte fallback). */
static int
re2_transcode_window (const unsigned char *input, size_t input_length,
                      size_t base, char **window_out, size_t *window_length_out)
{
  bool be;
  size_t cursor;
  size_t count = 0;
  char *window;
  size_t used = 0;

  if (window_out != NULL)
    *window_out = NULL;
  if (window_length_out != NULL)
    *window_length_out = 0;
  if (input == NULL || window_out == NULL || window_length_out == NULL
      || base > input_length)
    return -1;

  be = re2_input_is_be (input, input_length);
  window = malloc (GLR_RE2_WINDOW_MAX_UTF8 + 1);
  if (window == NULL)
    return -1;

  cursor = base;
  while (cursor < input_length && count < GLR_RE2_WINDOW_MAX_CODEPOINTS
         && used < GLR_RE2_WINDOW_MAX_UTF8)
    {
      uint16_t first;
      uint16_t second;
      uint32_t codepoint;
      char encoded[4];
      size_t encoded_length;

      if (input_length - cursor < 2)
        {
          free (window);
          return -1;
        }
      first = re2_read_u16 (input + cursor, be);
      if (first >= 0xD800 && first <= 0xDBFF)
        {
          if (input_length - cursor < 4)
            {
              free (window);
              return -1;
            }
          second = re2_read_u16 (input + cursor + 2, be);
          if (second < 0xDC00 || second > 0xDFFF)
            {
              free (window);
              return -1;
            }
          codepoint = 0x10000u
                      + ((((uint32_t) first - 0xD800u) << 10)
                         | ((uint32_t) second - 0xDC00u));
          cursor += 4;
        }
      else if (first >= 0xDC00 && first <= 0xDFFF)
        {
          free (window);
          return -1;
        }
      else
        {
          codepoint = first;
          cursor += 2;
        }

      encoded_length = re2_encode_utf8 (codepoint, encoded);
      if (used + encoded_length > GLR_RE2_WINDOW_MAX_UTF8)
        break;
      memcpy (window + used, encoded, encoded_length);
      used += encoded_length;
      count++;
    }

  /* POSIX regex operates on text: never expose an embedded NUL. */
  {
    void *nul = memchr (window, '\0', used);
    if (nul != NULL)
      used = (size_t) ((char *) nul - window);
  }
  window[used] = '\0';
  *window_out = window;
  *window_length_out = used;
  return 0;
}

/* Map a UTF-8 prefix length back to UTF-16 bytes, shrinking to the previous
   codepoint boundary when the length splits a character. Returns 0 when no
   progress is possible. */
static size_t
re2_utf8_to_utf16 (const unsigned char *input, size_t input_length,
                   size_t base, size_t utf8_budget)
{
  bool be;
  size_t cursor;
  size_t utf8_used = 0;

  if (input == NULL || base >= input_length || utf8_budget == 0)
    return 0;
  be = re2_input_is_be (input, input_length);
  cursor = base;
  while (cursor < input_length && utf8_used < utf8_budget)
    {
      uint16_t first;
      uint32_t codepoint;
      size_t units = 2;
      char encoded[4];
      size_t encoded_length;

      if (input_length - cursor < 2)
        break;
      first = re2_read_u16 (input + cursor, be);
      if (first >= 0xD800 && first <= 0xDBFF)
        {
          uint16_t second;
          if (input_length - cursor < 4)
            break;
          second = re2_read_u16 (input + cursor + 2, be);
          if (second < 0xDC00 || second > 0xDFFF)
            break;
          codepoint = 0x10000u
                      + ((((uint32_t) first - 0xD800u) << 10)
                         | ((uint32_t) second - 0xDC00u));
          units = 4;
        }
      else if (first >= 0xDC00 && first <= 0xDFFF)
        break;
      else
        codepoint = first;

      encoded_length = re2_encode_utf8 (codepoint, encoded);
      if (encoded[0] == '\0')
        break; /* U+0000 ends the text window. */
      if (utf8_used + encoded_length > utf8_budget)
        break;
      utf8_used += encoded_length;
      cursor += units;
    }
  return cursor - base;
}

glr_lexer_re2_t *
glr_lexer_re2_create (void)
{
  glr_lexer_re2_t *lexer = calloc (1, sizeof (*lexer));

  if (lexer == NULL)
    return NULL;
  lexer->rules = vec_glr_lexer_re2_rule_ptr_t_init ();
  lexer->error[0] = '\0';
  return lexer;
}

void
glr_lexer_re2_clear (glr_lexer_re2_t *lexer)
{
  if (lexer == NULL)
    return;
  for (size_t i = 0; i < lexer->rules.size; i++)
    re2_rule_free (lexer->rules.value[i]);
  vec_glr_lexer_re2_rule_ptr_t_clear (&lexer->rules);
  lexer->next_order = 0;
  lexer->error[0] = '\0';
}

void
glr_lexer_re2_destroy (glr_lexer_re2_t *lexer)
{
  if (lexer == NULL)
    return;
  glr_lexer_re2_clear (lexer);
  vec_glr_lexer_re2_rule_ptr_t_free (&lexer->rules);
  free (lexer);
}

static int
re2_add_compiled (glr_lexer_re2_t *lexer, const char *pattern,
                  const char *terminal, int priority, bool is_skip,
                  const glr_lexer_re2_options_t *options, regex_t *compiled,
                  const char *anchored)
{
  glr_lexer_re2_rule_t *rule;
  glr_lexer_re2_rule_ptr_t *grown;

  (void) anchored;
  if (lexer == NULL || pattern == NULL || (!is_skip && terminal == NULL))
    return -1;

  rule = calloc (1, sizeof (*rule));
  if (rule == NULL)
    {
      re2_set_error (lexer, "out of memory");
      return -1;
    }
  rule->compiled = *compiled;
  rule->has_compiled = true;
  rule->is_skip = is_skip;
  rule->priority = priority;
  rule->order = lexer->next_order++;
  rule->case_insensitive
      = options != NULL ? options->case_insensitive : false;
  rule->dot_nl = options != NULL ? options->dot_nl : false;
  rule->pattern_text = re2_strdup (pattern);
  if (rule->pattern_text == NULL)
    {
      re2_rule_free (rule);
      re2_set_error (lexer, "out of memory");
      return -1;
    }
  if (!is_skip)
    {
      rule->terminal = re2_strdup (terminal);
      if (rule->terminal == NULL)
        {
          re2_rule_free (rule);
          re2_set_error (lexer, "out of memory");
          return -1;
        }
    }

  grown = GLR_VECTOR_RESERVE (&lexer->rules, lexer->rules.size + 1);
  if (grown == NULL)
    {
      re2_rule_free (rule);
      re2_set_error (lexer, "out of memory");
      return -1;
    }
  lexer->rules.value = grown;
  vec_glr_lexer_re2_rule_ptr_t_push_back (&lexer->rules, rule);
  lexer->error[0] = '\0';
  return 0;
}

static int
re2_compile_anchored (glr_lexer_re2_t *lexer, const char *pattern,
                      const glr_lexer_re2_options_t *options,
                      regex_t *compiled)
{
  size_t length;
  char *anchored = NULL;
  int flags = REG_EXTENDED;
  int rc;
  bool dot_nl = options != NULL ? options->dot_nl : false;

  if (pattern == NULL || compiled == NULL)
    return -1;
  if (options != NULL && options->case_insensitive)
    flags |= REG_ICASE;
  if (!dot_nl)
    flags |= REG_NEWLINE;

  length = strlen (pattern);
  if (length > 65535)
    {
      re2_set_error (lexer, "pattern too long");
      return -1;
    }
  anchored = malloc (length + 4);
  if (anchored == NULL)
    {
      re2_set_error (lexer, "out of memory");
      return -1;
    }
  anchored[0] = '^';
  anchored[1] = '(';
  memcpy (anchored + 2, pattern, length);
  anchored[length + 2] = ')';
  anchored[length + 3] = '\0';

  rc = regcomp (compiled, anchored, flags);
  free (anchored);
  if (rc != 0)
    {
      char message[192];
      regerror (rc, compiled, message, sizeof (message));
      re2_set_error (lexer, message[0] != '\0' ? message : "invalid pattern");
      return -1;
    }
  /* RE2 semantics never accept the empty string here: such a rule could
     match zero bytes and stall the reader cursor. */
  if (regexec (compiled, "", 0, NULL, 0) == 0)
    {
      regfree (compiled);
      re2_set_error (lexer, "pattern accepts an empty string");
      return -1;
    }
  return 0;
}

int
glr_lexer_re2_add_with_options (glr_lexer_re2_t *lexer, const char *pattern,
                                 const char *terminal, int priority,
                                 const glr_lexer_re2_options_t *options)
{
  regex_t compiled;

  if (lexer == NULL || pattern == NULL || terminal == NULL
      || *terminal == '\0')
    {
      re2_set_error (lexer, "invalid argument");
      return -1;
    }
  if (re2_compile_anchored (lexer, pattern, options, &compiled) != 0)
    return -1;
  if (re2_add_compiled (lexer, pattern, terminal, priority, false, options,
                        &compiled, NULL)
      != 0)
    {
      regfree (&compiled);
      return -1;
    }
  return 0;
}

int
glr_lexer_re2_add (glr_lexer_re2_t *lexer, const char *pattern,
                   const char *terminal, int priority)
{
  return glr_lexer_re2_add_with_options (lexer, pattern, terminal, priority,
                                         NULL);
}

int
glr_lexer_re2_add_literal (glr_lexer_re2_t *lexer, const void *literal,
                           size_t length, const char *terminal, int priority)
{
  glr_lexer_re2_rule_t *rule;
  glr_lexer_re2_rule_ptr_t *grown;

  if (lexer == NULL || literal == NULL || length == 0 || terminal == NULL
      || *terminal == '\0')
    {
      re2_set_error (lexer, "invalid argument");
      return -1;
    }
  if (memchr (literal, '\0', length) != NULL)
    {
      re2_set_error (lexer, "literal must not contain NUL bytes");
      return -1;
    }
  rule = calloc (1, sizeof (*rule));
  if (rule == NULL)
    {
      re2_set_error (lexer, "out of memory");
      return -1;
    }
  rule->is_literal = true;
  rule->priority = priority;
  rule->order = lexer->next_order++;
  rule->literal = malloc (length);
  if (rule->literal == NULL)
    {
      re2_rule_free (rule);
      re2_set_error (lexer, "out of memory");
      return -1;
    }
  memcpy (rule->literal, literal, length);
  rule->literal_length = length;
  rule->terminal = re2_strdup (terminal);
  if (rule->terminal == NULL)
    {
      re2_rule_free (rule);
      re2_set_error (lexer, "out of memory");
      return -1;
    }
  grown = GLR_VECTOR_RESERVE (&lexer->rules, lexer->rules.size + 1);
  if (grown == NULL)
    {
      re2_rule_free (rule);
      re2_set_error (lexer, "out of memory");
      return -1;
    }
  lexer->rules.value = grown;
  vec_glr_lexer_re2_rule_ptr_t_push_back (&lexer->rules, rule);
  lexer->error[0] = '\0';
  return 0;
}

int
glr_lexer_re2_add_skip (glr_lexer_re2_t *lexer, const char *pattern)
{
  regex_t compiled;
  glr_lexer_re2_options_t defaults = { false, true };

  if (lexer == NULL || pattern == NULL || *pattern == '\0')
    {
      re2_set_error (lexer, "invalid argument");
      return -1;
    }
  if (re2_compile_anchored (lexer, pattern, &defaults, &compiled) != 0)
    return -1;
  if (re2_add_compiled (lexer, pattern, NULL, 0, true, &defaults, &compiled,
                        NULL)
      != 0)
    {
      regfree (&compiled);
      return -1;
    }
  return 0;
}

size_t
glr_lexer_re2_rule_count (const glr_lexer_re2_t *lexer)
{
  return lexer != NULL ? lexer->rules.size : 0;
}

const char *
glr_lexer_re2_last_error (const glr_lexer_re2_t *lexer)
{
  return lexer != NULL ? lexer->error : "";
}

/* Longest skip prefix at utf8+base; returns UTF-8 bytes consumed (0 none). */
static size_t
re2_skip_prefix_utf8 (const glr_lexer_re2_t *lexer, const char *utf8)
{
  size_t best = 0;

  for (size_t i = 0; i < lexer->rules.size; i++)
    {
      const glr_lexer_re2_rule_t *rule = lexer->rules.value[i];
      regmatch_t match;

      if (!rule->is_skip)
        continue;
      if (rule->is_literal)
        {
          if (rule->literal_length > 0
              && strncmp (utf8, (const char *) rule->literal,
                          rule->literal_length)
                     == 0
              && strlen (utf8) >= rule->literal_length
              && rule->literal_length > best)
            best = rule->literal_length;
          continue;
        }
      if (!rule->has_compiled)
        continue;
      if (regexec (&rule->compiled, utf8, 1, &match, 0) != 0)
        continue;
      if (match.rm_so == 0 && match.rm_eo > 0
          && (size_t) match.rm_eo > best)
        best = (size_t) match.rm_eo;
    }
  return best;
}

static bool
re2_match_at (const glr_lexer_re2_t *lexer, const char *utf8,
              const glr_lexer_re2_rule_t **winner_out, size_t *utf8_len_out,
              bool skip_rules)
{
  const glr_lexer_re2_rule_t *winner = NULL;
  size_t winner_utf8 = 0;

  for (size_t i = 0; i < lexer->rules.size; i++)
    {
      const glr_lexer_re2_rule_t *rule = lexer->rules.value[i];
      size_t candidate = 0;

      if (rule->is_skip != skip_rules)
        continue;
      if (rule->is_literal)
        {
          size_t available = strlen (utf8);
          if (rule->literal_length <= available
              && memcmp (utf8, rule->literal, rule->literal_length) == 0)
            candidate = rule->literal_length;
        }
      else if (rule->has_compiled)
        {
          regmatch_t match;
          if (regexec (&rule->compiled, utf8, 1, &match, 0) != 0)
            continue;
          if (match.rm_so != 0 || match.rm_eo <= 0)
            continue;
          candidate = (size_t) match.rm_eo;
        }
      if (candidate == 0)
        continue;
      if (winner == NULL || candidate > winner_utf8
          || (candidate == winner_utf8
              && (rule->priority < winner->priority
                  || (rule->priority == winner->priority
                      && rule->order < winner->order))))
        {
          winner = rule;
          winner_utf8 = candidate;
        }
    }

  if (winner_out != NULL)
    *winner_out = winner;
  if (utf8_len_out != NULL)
    *utf8_len_out = winner_utf8;
  return winner != NULL;
}

bool
glr_lexer_re2_hook (const glr_lexer_event_t *event,
                    glr_lexer_response_t *response, void *user_data)
{
  glr_lexer_re2_t *lexer = user_data;
  char *window = NULL;
  size_t window_length = 0;
  size_t base_utf8 = 0;
  size_t base_utf16 = 0;

  if (response == NULL)
    return false;
  glr_lexer_response_reset (response);
  if (event == NULL || lexer == NULL || event->input == NULL
      || lexer->rules.size == 0)
    return false;
  if (event->byte_offset >= event->input_length)
    return false;

  if (re2_transcode_window (event->input, event->input_length,
                            event->byte_offset, &window, &window_length)
      != 0)
    {
      /* Raw-byte fallback for non-UTF-16 buffers. */
      const char *raw = (const char *) event->input + event->byte_offset;
      size_t remaining = event->input_length - event->byte_offset;
      const char *nul;
      char *text;
      const glr_lexer_re2_rule_t *winner = NULL;
      size_t winner_length = 0;

      nul = memchr (raw, '\0', remaining);
      remaining = nul != NULL ? (size_t) (nul - raw) : remaining;
      text = malloc (remaining + 1);
      if (text == NULL)
        return false;
      memcpy (text, raw, remaining);
      text[remaining] = '\0';
      re2_match_at (lexer, text, &winner, &winner_length, false);
      free (text);
      if (winner == NULL || winner_length < event->default_bytes_consumed)
        return false;
      glr_lexer_response_accept (response, winner->terminal, winner_length);
      return true;
    }

  for (size_t iter = 0; iter < GLR_RE2_SKIP_ITERATION_LIMIT; iter++)
    {
      size_t advance = re2_skip_prefix_utf8 (lexer, window + base_utf8);
      size_t as_utf16;

      if (advance == 0 || base_utf8 + advance > window_length)
        break;
      as_utf16 = re2_utf8_to_utf16 (event->input, event->input_length,
                                    event->byte_offset + base_utf16, advance);
      if (as_utf16 == 0)
        break;
      base_utf8 += advance;
      base_utf16 += as_utf16;
    }

  {
    const glr_lexer_re2_rule_t *winner = NULL;
    size_t winner_utf8 = 0;
    size_t winner_utf16;

    if (!re2_match_at (lexer, window + base_utf8, &winner, &winner_utf8,
                       false))
      {
        free (window);
        return false;
      }
    winner_utf16 = re2_utf8_to_utf16 (event->input, event->input_length,
                                      event->byte_offset + base_utf16,
                                      winner_utf8);
    if (winner_utf16 == 0)
      {
        free (window);
        return false;
      }
    if (base_utf16 + winner_utf16 < event->default_bytes_consumed)
      {
        free (window);
        return false;
      }
    if (event->byte_offset + base_utf16 + winner_utf16
        > event->input_length)
      {
        free (window);
        return false;
      }
    glr_lexer_response_accept (response, winner->terminal,
                               base_utf16 + winner_utf16);
    free (window);
    return true;
  }
}

int
glr_lexer_re2_install (glr_lexer_re2_t *lexer, glr_lexer_hooks_t *hooks,
                       const char *hook_name, int hook_priority)
{
  if (lexer == NULL || hooks == NULL)
    return -1;
  return glr_lexer_hooks_add (hooks, hook_name, hook_priority,
                              glr_lexer_re2_hook, lexer, NULL);
}
