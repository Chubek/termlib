/* smoke.c -- build-time test: generated parsers compile, link, run.
 *
 * Exercises the JSON parser Moosedog emits at build time (grammar build,
 * trivia/ATN wiring, real parses) plus the frontend library surface used
 * by the CLI: spec parsing/validation, entrypoint resolution, Optparse
 * lookup, env expansion, Lua syntax checks, stdext helpers, the plugin
 * registry miss path, and the EBNF translator.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glr/glr.h>

#include "JSON.parser.h"
#include "moosedog_parser.h"
#include "moosedog_entrypoint.h"
#include "moosedog_lexer.h"
#include "moosedog_rewrite.h"
#include "moosedog_disamb.h"
#include "moosedog_lua.h"
#include "moosedog_stdext.h"
#include "moosedog_confuse.h"
#include "moosedog_plugin.h"
#include "EBNF-Frontend.h"

#ifndef MOOSEDOG_SRC_DIR
#define MOOSEDOG_SRC_DIR "."
#endif

static int failures = 0;

#define CHECK(cond, msg)                                                  \
  do                                                                      \
    {                                                                     \
      if (!(cond))                                                        \
        {                                                                 \
          fprintf (stderr, "smoke FAIL: %s (line %d)\n", (msg), __LINE__); \
          failures++;                                                     \
        }                                                                 \
    }                                                                     \
  while (0)

static char *
read_file (const char *path, size_t *len_out)
{
  FILE *f = fopen (path, "rb");
  char *buf;
  size_t len = 0, cap = 1 << 16, n;
  if (!f)
    return NULL;
  buf = malloc (cap + 1);
  if (!buf)
    {
      fclose (f);
      return NULL;
    }
  for (;;)
    {
      if (len + 4096 > cap)
        {
          cap *= 2;
          {
            char *nb = realloc (buf, cap + 1);
            if (!nb)
              {
                free (buf);
                fclose (f);
                return NULL;
              }
            buf = nb;
          }
        }
      n = fread (buf + len, 1, cap - len, f);
      len += n;
      if (n == 0)
        break;
    }
  fclose (f);
  buf[len] = '\0';
  if (len_out)
    *len_out = len;
  return buf;
}

static void
test_generated_json (void)
{
  char err[512] = { 0 };
  glr_grammar_t *g = JSON_build_grammar (err, sizeof (err));
  CHECK (g != NULL, "JSON_build_grammar");
  if (!g)
    return;
  {
    glr_parser_t *p = JSON_create_parser (g);
    static const char *good[] = {
      "{\"a\": 1}",
      "{\"a\": [1, 2], \"b\": true}",
      "[1, {\"x\": null}]",
      /* Regression: \xNN in the TOK_STRING pattern once excluded the
       * letter 'x' from strings (POSIX vs PCRE semantics). */
      "{\"x\": 1}",
      "{\"xyz\": [true, false, null]}",
    };
    CHECK (p != NULL, "JSON_create_parser");
    for (size_t i = 0; i < sizeof (good) / sizeof (good[0]); i++)
      {
        glr_parse_result_t r = glr_parse (p, good[i], strlen (good[i]));
        CHECK (r.error == GLR_PARSE_SUCCESS, "valid JSON parses");
      }
    {
      /* Malformed input must not parse. */
      glr_parse_result_t r = glr_parse (p, "{\"a\": }", 7);
      CHECK (r.error != GLR_PARSE_SUCCESS, "invalid JSON rejected");
    }
    glr_parser_destroy (p);
  }
  glr_grammar_destroy (g);
  CHECK (JSON_info.max_errors == 10, "JSON metadata max_errors");
}

static void
test_spec_pipeline (void)
{
  char path[4096];
  char *text;
  md_spec_t spec;
  md_error_t err;
  md_build_config_t cfg;
  snprintf (path, sizeof (path), "%s/examples/JSON/JSON.grm", MOOSEDOG_SRC_DIR);
  text = read_file (path, NULL);
  CHECK (text != NULL, "read JSON.grm");
  if (!text)
    return;
  /* Raw text still carries GPP conditionals; the CLI preprocesses, but
   * the raw form must at least parse structurally. */
  CHECK (md_parse_string (text, strlen (text), path, &spec, &err) == 0,
         "md_parse_string JSON");
  if (failures == 0)
    {
      CHECK (md_spec_validate (&spec, &err) == 0, "md_spec_validate JSON");
      CHECK (md_lexer_validate (&spec, &err) == 0, "md_lexer_validate JSON");
      CHECK (md_entrypoint_validate (&spec, &err) == 0,
             "md_entrypoint_validate JSON");
      CHECK (md_disamb_validate (&spec, &err) == 0, "md_disamb_validate");
      CHECK (md_rewrite_validate (&spec, &err) == 0, "md_rewrite_validate");
      md_entrypoint_resolve (&spec, &cfg);
      CHECK (cfg.max_errors == 10, "resolve max_errors");
      {
        const md_option_t *o = md_entrypoint_find_option (&spec, "--debug");
        CHECK (o != NULL && o->short_name
               && !strcmp (o->short_name, "-d"),
               "find --debug");
        CHECK (md_entrypoint_find_option (&spec, "-e") != NULL, "find -e");
        CHECK (md_entrypoint_find_option (&spec, "--nope") == NULL,
               "miss --nope");
      }
      CHECK (md_lexer_token_count (&spec) == 11, "11 match tokens");
      CHECK (md_lexer_skip_count (&spec) == 1, "1 skip token");
    }
  md_spec_destroy (&spec);
  free (text);
}

static void
test_helpers (void)
{
  char out[256];
  char dir[256];
  char err[128];
  char *grm = NULL;
  CHECK (md_stdext_known ("URL") == 1, "stdext URL known");
  CHECK (md_stdext_known ("Bogus") == 0, "stdext Bogus unknown");
  CHECK (md_stdext_eval ("URL( \"https://json.org\" )") != NULL,
         "stdext eval URL");
  CHECK (md_conf_expand ("${MOOSEDOG_NOPE}x", out, sizeof (out)) == 0
         && !strcmp (out, "x"),
         "conf expand unset");
  CHECK (md_conf_resolve_dir ("${TMPDIR}/MoosedogCache", dir, sizeof (dir))
           == 0
         && dir[0] != '\0',
         "conf resolve dir");
  CHECK (md_lua_check_syntax ("function f() end\n", 17, err, sizeof (err))
           == 0,
         "lua ok");
  CHECK (md_lua_check_syntax ("--[[ never closed", 17, err, sizeof (err))
           != 0,
         "lua unclosed comment");
  {
    md_plugin_registry_t reg;
    md_plugins_init (&reg);
    CHECK (md_plugin_translate (&reg, ".ebnf", "x", 1, &grm, err,
                                 sizeof (err))
             != 0,
           "empty registry misses");
    md_plugins_destroy (&reg);
  }
  CHECK (ebnf_to_grm ("A ::= \"a\" ;\n", 11, "T", &grm, err, sizeof (err))
           == 0
         && grm != NULL && strstr (grm, "LIT_1") != NULL,
         "ebnf literal token");
  free (grm);
}

int
main (void)
{
  test_generated_json ();
  test_spec_pipeline ();
  test_helpers ();
  if (failures == 0)
    puts ("moosedog smoke: all passed");
  return failures ? 1 : 0;
}
