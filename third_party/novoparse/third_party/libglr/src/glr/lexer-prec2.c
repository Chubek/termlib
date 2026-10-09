/* PCRE2-style ("prec2") anchored lexer adapter for glr_lexer_hooks_t.
 *
 * Rule storage uses CTL vectors (third_party/ctl). When built with
 * -DGLR_LEXER_USE_PCRE2 and linked against the vendored PCRE2 8-bit
 * library (third_party/PCRE2, PCRE2_CODE_UNIT_WIDTH=8), patterns compile
 * with pcre2_compile_8() and match with pcre2_match_8(). Otherwise the
 * adapter uses POSIX ERE with a documented option mapping, so it always
 * builds and behaves predictably. */

#include <glr/lexer-hooks.h>
#include <glr/lexer-prec2.h>

#include "containers.h"

#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(GLR_LEXER_USE_PCRE2)
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#endif

#define GLR_PREC2_WINDOW_MAX_CODEPOINTS 4096u
#define GLR_PREC2_WINDOW_MAX_UTF8 65536u
#define GLR_PREC2_SKIP_ITERATION_LIMIT 1024u

typedef struct glr_lexer_prec2_rule glr_lexer_prec2_rule_t;

struct glr_lexer_prec2_rule
{
  regex_t compiled;
  bool has_compiled;
#if defined(GLR_LEXER_USE_PCRE2)
  pcre2_code_8 *pcre2;
  pcre2_match_data_8 *match_data;
#endif
  bool is_skip;
  bool is_literal;
  char *pattern_text;
  char *terminal;
  unsigned char *literal;
  size_t literal_length;
  int priority;
  size_t order;
  glr_lexer_prec2_options_t options;
};

typedef glr_lexer_prec2_rule_t *glr_lexer_prec2_rule_ptr_t;

#define T glr_lexer_prec2_rule_ptr_t
#define P
#include <ctl/vec.h>

struct glr_lexer_prec2
{
  vec_glr_lexer_prec2_rule_ptr_t rules;
  size_t next_order;
  char error[256];
};

static char *
prec2_strdup (const char *text)
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
prec2_set_error (glr_lexer_prec2_t *lexer, const char *message)
{
  if (lexer == NULL)
    return;
  if (message == NULL)
    message = "unknown error";
  snprintf (lexer->error, sizeof (lexer->error), "%s", message);
}

static void
prec2_rule_free (glr_lexer_prec2_rule_t *rule)
{
  if (rule == NULL)
    return;
  if (rule->has_compiled)
    regfree (&rule->compiled);
#if defined(GLR_LEXER_USE_PCRE2)
  if (rule->match_data != NULL)
    pcre2_match_data_free_8 (rule->match_data);
  if (rule->pcre2 != NULL)
    pcre2_code_free_8 (rule->pcre2);
#endif
  free (rule->pattern_text);
  free (rule->terminal);
  free (rule->literal);
  free (rule);
}

static bool
prec2_input_is_be (const unsigned char *input, size_t length)
{
  return length >= 2 && input[0] == 0xFE && input[1] == 0xFF;
}

static uint16_t
prec2_read_u16 (const unsigned char *p, bool be)
{
  if (be)
    return (uint16_t) (((uint16_t) p[0] << 8) | p[1]);
  return (uint16_t) (((uint16_t) p[1] << 8) | p[0]);
}

static size_t
prec2_encode_utf8 (uint32_t codepoint, char out[4])
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

static int
prec2_transcode_window (const unsigned char *input, size_t input_length,
                        size_t base, char **window_out,
                        size_t *window_length_out)
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

  be = prec2_input_is_be (input, input_length);
  window = malloc (GLR_PREC2_WINDOW_MAX_UTF8 + 1);
  if (window == NULL)
    return -1;

  cursor = base;
  while (cursor < input_length && count < GLR_PREC2_WINDOW_MAX_CODEPOINTS
         && used < GLR_PREC2_WINDOW_MAX_UTF8)
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
      first = prec2_read_u16 (input + cursor, be);
      if (first >= 0xD800 && first <= 0xDBFF)
        {
          if (input_length - cursor < 4)
            {
              free (window);
              return -1;
            }
          second = prec2_read_u16 (input + cursor + 2, be);
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

      encoded_length = prec2_encode_utf8 (codepoint, encoded);
      if (used + encoded_length > GLR_PREC2_WINDOW_MAX_UTF8)
        break;
      memcpy (window + used, encoded, encoded_length);
      used += encoded_length;
      count++;
    }

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

static size_t
prec2_utf8_to_utf16 (const unsigned char *input, size_t input_length,
                     size_t base, size_t utf8_budget)
{
  bool be;
  size_t cursor;
  size_t utf8_used = 0;

  if (input == NULL || base >= input_length || utf8_budget == 0)
    return 0;
  be = prec2_input_is_be (input, input_length);
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
      first = prec2_read_u16 (input + cursor, be);
      if (first >= 0xD800 && first <= 0xDBFF)
        {
          uint16_t second;
          if (input_length - cursor < 4)
            break;
          second = prec2_read_u16 (input + cursor + 2, be);
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

      encoded_length = prec2_encode_utf8 (codepoint, encoded);
      if (encoded[0] == '\0')
        break;
      if (utf8_used + encoded_length > utf8_budget)
        break;
      utf8_used += encoded_length;
      cursor += units;
    }
  return cursor - base;
}

glr_lexer_prec2_t *
glr_lexer_prec2_create (void)
{
  glr_lexer_prec2_t *lexer = calloc (1, sizeof (*lexer));

  if (lexer == NULL)
    return NULL;
  lexer->rules = vec_glr_lexer_prec2_rule_ptr_t_init ();
  lexer->error[0] = '\0';
  return lexer;
}

void
glr_lexer_prec2_clear (glr_lexer_prec2_t *lexer)
{
  if (lexer == NULL)
    return;
  for (size_t i = 0; i < lexer->rules.size; i++)
    prec2_rule_free (lexer->rules.value[i]);
  vec_glr_lexer_prec2_rule_ptr_t_clear (&lexer->rules);
  lexer->next_order = 0;
  lexer->error[0] = '\0';
}

void
glr_lexer_prec2_destroy (glr_lexer_prec2_t *lexer)
{
  if (lexer == NULL)
    return;
  glr_lexer_prec2_clear (lexer);
  vec_glr_lexer_prec2_rule_ptr_t_free (&lexer->rules);
  free (lexer);
}

/* Compile one anchored pattern. On success the caller owns *compiled and,
   under GLR_LEXER_USE_PCRE2, the rule's pcre2 members are filled later. */
static int
prec2_compile_posix (glr_lexer_prec2_t *lexer, const char *pattern,
                     const glr_lexer_prec2_options_t *options,
                     regex_t *compiled)
{
  size_t length;
  char *anchored = NULL;
  int flags = REG_EXTENDED;
  int rc;

  if (pattern == NULL || compiled == NULL)
    return -1;
  if (options != NULL && options->caseless)
    flags |= REG_ICASE;
  /* POSIX approximation: without dotall (or with multiline), REG_NEWLINE
     keeps '.' off newlines and lets ^/$ respect line boundaries. */
  if (options == NULL || !options->dotall || options->multiline)
    flags |= REG_NEWLINE;

  length = strlen (pattern);
  if (length > 65535)
    {
      prec2_set_error (lexer, "pattern too long");
      return -1;
    }
  anchored = malloc (length + 4);
  if (anchored == NULL)
    {
      prec2_set_error (lexer, "out of memory");
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
      prec2_set_error (lexer, message[0] != '\0' ? message : "invalid pattern");
      return -1;
    }
  if (regexec (compiled, "", 0, NULL, 0) == 0)
    {
      regfree (compiled);
      prec2_set_error (lexer, "pattern accepts an empty string");
      return -1;
    }
  return 0;
}

static int
prec2_push_rule (glr_lexer_prec2_t *lexer, glr_lexer_prec2_rule_t *rule)
{
  glr_lexer_prec2_rule_ptr_t *grown;

  grown = GLR_VECTOR_RESERVE (&lexer->rules, lexer->rules.size + 1);
  if (grown == NULL)
    {
      prec2_rule_free (rule);
      prec2_set_error (lexer, "out of memory");
      return -1;
    }
  lexer->rules.value = grown;
  vec_glr_lexer_prec2_rule_ptr_t_push_back (&lexer->rules, rule);
  lexer->error[0] = '\0';
  return 0;
}

int
glr_lexer_prec2_add (glr_lexer_prec2_t *lexer, const char *pattern,
                     const char *terminal, int priority,
                     const glr_lexer_prec2_options_t *options)
{
  glr_lexer_prec2_rule_t *rule;
  regex_t compiled;

  if (lexer == NULL || pattern == NULL || terminal == NULL
      || *terminal == '\0')
    {
      prec2_set_error (lexer, "invalid argument");
      return -1;
    }
  if (prec2_compile_posix (lexer, pattern, options, &compiled) != 0)
    return -1;

  rule = calloc (1, sizeof (*rule));
  if (rule == NULL)
    {
      regfree (&compiled);
      prec2_set_error (lexer, "out of memory");
      return -1;
    }
  rule->compiled = compiled;
  rule->has_compiled = true;
  rule->priority = priority;
  rule->order = lexer->next_order++;
  if (options != NULL)
    rule->options = *options;
  rule->pattern_text = prec2_strdup (pattern);
  rule->terminal = prec2_strdup (terminal);
  if (rule->pattern_text == NULL || rule->terminal == NULL)
    {
      prec2_rule_free (rule);
      prec2_set_error (lexer, "out of memory");
      return -1;
    }

#if defined(GLR_LEXER_USE_PCRE2)
  {
    int errorcode;
    PCRE2_SIZE erroroffset;
    uint32_t compile_options = PCRE2_ANCHORED | PCRE2_DOLLAR_ENDONLY;
    if (options != NULL)
      {
        if (options->caseless)
          compile_options |= PCRE2_CASELESS;
        if (options->multiline)
          compile_options |= PCRE2_MULTILINE;
        if (options->dotall)
          compile_options |= PCRE2_DOTALL;
        if (options->utf)
          compile_options |= PCRE2_UTF;
        if (options->ucp)
          compile_options |= PCRE2_UCP;
        compile_options |= options->extra_compile_options;
      }
    rule->pcre2 = pcre2_compile_8 ((PCRE2_SPTR8) pattern, PCRE2_ZERO_TERMINATED,
                                   compile_options, &errorcode, &erroroffset,
                                   NULL);
    if (rule->pcre2 == NULL)
      {
        PCRE2_UCHAR8 message[192];
        pcre2_get_error_message_8 (errorcode, message, sizeof (message));
        prec2_rule_free (rule);
        prec2_set_error (lexer, (const char *) message);
        return -1;
      }
    if (options != NULL && options->jit)
      (void) pcre2_jit_compile_8 (rule->pcre2, PCRE2_JIT_COMPLETE);
    rule->match_data = pcre2_match_data_create_8 (16, NULL);
    if (rule->match_data == NULL)
      {
        prec2_rule_free (rule);
        prec2_set_error (lexer, "out of memory");
        return -1;
      }
  }
#endif

  return prec2_push_rule (lexer, rule);
}

int
glr_lexer_prec2_add_literal (glr_lexer_prec2_t *lexer, const void *literal,
                             size_t length, const char *terminal, int priority)
{
  glr_lexer_prec2_rule_t *rule;

  if (lexer == NULL || literal == NULL || length == 0 || terminal == NULL
      || *terminal == '\0')
    {
      prec2_set_error (lexer, "invalid argument");
      return -1;
    }
  if (memchr (literal, '\0', length) != NULL)
    {
      prec2_set_error (lexer, "literal must not contain NUL bytes");
      return -1;
    }
  rule = calloc (1, sizeof (*rule));
  if (rule == NULL)
    {
      prec2_set_error (lexer, "out of memory");
      return -1;
    }
  rule->is_literal = true;
  rule->priority = priority;
  rule->order = lexer->next_order++;
  rule->literal = malloc (length);
  rule->terminal = prec2_strdup (terminal);
  if (rule->literal == NULL || rule->terminal == NULL)
    {
      prec2_rule_free (rule);
      prec2_set_error (lexer, "out of memory");
      return -1;
    }
  memcpy (rule->literal, literal, length);
  rule->literal_length = length;
  return prec2_push_rule (lexer, rule);
}

int
glr_lexer_prec2_add_skip (glr_lexer_prec2_t *lexer, const char *pattern,
                          const glr_lexer_prec2_options_t *options)
{
  glr_lexer_prec2_options_t defaults;
  glr_lexer_prec2_rule_t *rule;
  regex_t compiled;

  if (lexer == NULL || pattern == NULL || *pattern == '\0')
    {
      prec2_set_error (lexer, "invalid argument");
      return -1;
    }
  if (options == NULL)
    {
      memset (&defaults, 0, sizeof (defaults));
      defaults.dotall = true;
      options = &defaults;
    }
  if (prec2_compile_posix (lexer, pattern, options, &compiled) != 0)
    return -1;
  rule = calloc (1, sizeof (*rule));
  if (rule == NULL)
    {
      regfree (&compiled);
      prec2_set_error (lexer, "out of memory");
      return -1;
    }
  rule->compiled = compiled;
  rule->has_compiled = true;
  rule->is_skip = true;
  rule->order = lexer->next_order++;
  rule->options = *options;
  rule->pattern_text = prec2_strdup (pattern);
  if (rule->pattern_text == NULL)
    {
      prec2_rule_free (rule);
      prec2_set_error (lexer, "out of memory");
      return -1;
    }
  return prec2_push_rule (lexer, rule);
}

size_t
glr_lexer_prec2_rule_count (const glr_lexer_prec2_t *lexer)
{
  return lexer != NULL ? lexer->rules.size : 0;
}

const char *
glr_lexer_prec2_last_error (const glr_lexer_prec2_t *lexer)
{
  return lexer != NULL ? lexer->error : "";
}

/* Length of the anchored match at utf8, or 0 for no match. Prefers the
   PCRE2 path when the rule was compiled with it. */
static size_t
prec2_rule_match_utf8 (const glr_lexer_prec2_rule_t *rule, const char *utf8)
{
#if defined(GLR_LEXER_USE_PCRE2)
  if (rule->pcre2 != NULL && rule->match_data != NULL && !rule->is_literal)
    {
      size_t subject_length = strlen (utf8);
      int rc = pcre2_match_8 (rule->pcre2, (PCRE2_SPTR8) utf8, subject_length,
                              0, PCRE2_ANCHORED, rule->match_data, NULL);
      PCRE2_SIZE *ovector;
      if (rc < 0)
        return 0;
      ovector = pcre2_get_ovector_pointer_8 (rule->match_data);
      if (ovector[0] != 0 || ovector[1] == PCRE2_UNSET
          || ovector[1] == 0)
        return 0;
      return (size_t) ovector[1];
    }
#else
  (void) 0;
#endif
  if (rule->is_literal)
    {
      size_t available = strlen (utf8);
      if (rule->literal_length <= available
          && memcmp (utf8, rule->literal, rule->literal_length) == 0)
        return rule->literal_length;
      return 0;
    }
  if (rule->has_compiled)
    {
      regmatch_t match;
      if (regexec (&rule->compiled, utf8, 1, &match, 0) != 0)
        return 0;
      if (match.rm_so == 0 && match.rm_eo > 0)
        return (size_t) match.rm_eo;
    }
  return 0;
}

static bool
prec2_best_at (const glr_lexer_prec2_t *lexer, const char *utf8, bool want_skip,
               const glr_lexer_prec2_rule_t **winner_out, size_t *utf8_len_out)
{
  const glr_lexer_prec2_rule_t *winner = NULL;
  size_t winner_length = 0;

  for (size_t i = 0; i < lexer->rules.size; i++)
    {
      const glr_lexer_prec2_rule_t *rule = lexer->rules.value[i];
      size_t candidate;

      if (rule->is_skip != want_skip)
        continue;
      candidate = prec2_rule_match_utf8 (rule, utf8);
      if (candidate == 0)
        continue;
      if (winner == NULL || candidate > winner_length
          || (candidate == winner_length
              && (rule->priority < winner->priority
                  || (rule->priority == winner->priority
                      && rule->order < winner->order))))
        {
          winner = rule;
          winner_length = candidate;
        }
    }
  if (winner_out != NULL)
    *winner_out = winner;
  if (utf8_len_out != NULL)
    *utf8_len_out = winner_length;
  return winner != NULL;
}

bool
glr_lexer_prec2_hook (const glr_lexer_event_t *event,
                      glr_lexer_response_t *response, void *user_data)
{
  glr_lexer_prec2_t *lexer = user_data;
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

  if (prec2_transcode_window (event->input, event->input_length,
                              event->byte_offset, &window, &window_length)
      != 0)
    {
      const char *raw = (const char *) event->input + event->byte_offset;
      size_t remaining = event->input_length - event->byte_offset;
      const char *nul;
      char *text;
      const glr_lexer_prec2_rule_t *winner = NULL;
      size_t winner_length = 0;

      nul = memchr (raw, '\0', remaining);
      remaining = nul != NULL ? (size_t) (nul - raw) : remaining;
      text = malloc (remaining + 1);
      if (text == NULL)
        return false;
      memcpy (text, raw, remaining);
      text[remaining] = '\0';
      prec2_best_at (lexer, text, false, &winner, &winner_length);
      free (text);
      if (winner == NULL || winner_length < event->default_bytes_consumed)
        return false;
      glr_lexer_response_accept (response, winner->terminal, winner_length);
      return true;
    }

  for (size_t iter = 0; iter < GLR_PREC2_SKIP_ITERATION_LIMIT; iter++)
    {
      const glr_lexer_prec2_rule_t *skip = NULL;
      size_t skip_utf8 = 0;
      size_t skip_utf16;

      if (!prec2_best_at (lexer, window + base_utf8, true, &skip, &skip_utf8)
          || skip_utf8 == 0 || base_utf8 + skip_utf8 > window_length)
        break;
      skip_utf16 = prec2_utf8_to_utf16 (event->input, event->input_length,
                                        event->byte_offset + base_utf16,
                                        skip_utf8);
      if (skip_utf16 == 0)
        break;
      base_utf8 += skip_utf8;
      base_utf16 += skip_utf16;
      (void) skip;
    }

  {
    const glr_lexer_prec2_rule_t *winner = NULL;
    size_t winner_utf8 = 0;
    size_t winner_utf16;

    if (!prec2_best_at (lexer, window + base_utf8, false, &winner,
                        &winner_utf8))
      {
        free (window);
        return false;
      }
    winner_utf16 = prec2_utf8_to_utf16 (event->input, event->input_length,
                                        event->byte_offset + base_utf16,
                                        winner_utf8);
    if (winner_utf16 == 0
        || base_utf16 + winner_utf16 < event->default_bytes_consumed
        || event->byte_offset + base_utf16 + winner_utf16
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
glr_lexer_prec2_install (glr_lexer_prec2_t *lexer, glr_lexer_hooks_t *hooks,
                         const char *hook_name, int hook_priority)
{
  if (lexer == NULL || hooks == NULL)
    return -1;
  return glr_lexer_hooks_add (hooks, hook_name, hook_priority,
                              glr_lexer_prec2_hook, lexer, NULL);
}
