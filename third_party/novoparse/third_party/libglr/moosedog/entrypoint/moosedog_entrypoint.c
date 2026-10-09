/* moosedog_entrypoint.c -- Entrypoint interpretation. */
#include "moosedog_entrypoint.h"
#include "../stdext/moosedog_stdext.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *
cfg_str (const md_spec_t *spec, const char *key, const char *dflt)
{
  const char *v = md_config_get (spec, key);
  if (!v)
    return dflt;
  /* Unexpanded GPP macros (no gpp run yet): fall back to the Support.gpp
   * defaults documented in the ANSI-C example. */
  if (!strchr (v, '"') && strchr (v, '_') && !strchr (v, ' '))
    {
      if (!strcmp (v, "ANSI_C_GRAMMAR_NAME"))
        return "ANSI-C";
      return dflt;
    }
  return v;
}

static int
cfg_bool (const md_spec_t *spec, const char *key, int dflt)
{
  const char *v = md_config_get (spec, key);
  if (!v)
    return dflt;
  /* Unexpanded Support.gpp macros: ANSI_C_USE_ATN defaults true. */
  if (!strcmp (v, "ANSI_C_USE_ATN"))
    return 1;
  return md_config_get_bool (spec, key, dflt);
}

void
md_entrypoint_resolve (const md_spec_t *spec, md_build_config_t *cfg)
{
  memset (cfg, 0, sizeof (*cfg));
  cfg->main_lexer = cfg_str (spec, "MainLexer", "Scannerless");
  cfg->fallback_lexer = cfg_str (spec, "FallbackLexer", "Scannerless");
  cfg->use_atn = cfg_bool (spec, "UseATN", 0);
  cfg->use_incremental = cfg_bool (spec, "UseIncremental", 0);
  cfg->use_cache = cfg_bool (spec, "UseCache", 0);
  cfg->cache_dir = cfg_str (spec, "CacheDirectory", "${TMPDIR}/MoosedogCache");
  cfg->main_allocator = cfg_str (spec, "MainAllocator", "malloc");
  cfg->fallback_allocator = cfg_str (spec, "FallbackAllocator", "malloc");
  cfg->emit_listener = cfg_bool (spec, "EmitASTListener", 1);
  cfg->emit_visitor = cfg_bool (spec, "EmitASTVisitor", 1);
  cfg->cache_lexer = cfg_bool (spec, "CacheLexer", 0);
  cfg->cache_parser = cfg_bool (spec, "CacheParser", 0);
  cfg->error_recovery = cfg_bool (spec, "ErrorRecovery", 0);
  cfg->max_errors = (int) md_config_get_int (spec, "MaxErrors", 10);
  cfg->recovery_strategy = cfg_str (spec, "RecoveryStrategy", "PanicMode");
  cfg->output_encoding = cfg_str (spec, "OutputEncoding", "UTF-8");
  cfg->strict_unicode = cfg_bool (spec, "StrictUnicode", 1);
  cfg->debug_level = (int) md_config_get_int (spec, "DebugLevel", 0);
  cfg->ast_annotations = cfg_bool (spec, "ASTAnnotations", 0);
  /* ATN block: AdaptiveLookahead/MaxDepth/PreserveAmbiguity/EmitStatistics. */
  {
    const char *v = md_atn_get (spec, "AdaptiveLookahead");
    if (!v)
      cfg->adaptive_lookahead = cfg->use_atn;
    else if (!strcmp (v, "ANSI_C_USE_ATN"))
      cfg->adaptive_lookahead = cfg->use_atn; /* Support.gpp default */
    else
      cfg->adaptive_lookahead = (!strcmp (v, "true") || !strcmp (v, "Yes")
                                 || !strcmp (v, "1") || strstr (v, "true") != NULL);
    v = md_atn_get (spec, "MaxDepth");
    if (!v)
      cfg->lookahead_depth = 8;
    else if (!strcmp (v, "ANSI_C_LOOKAHEAD_DEPTH"))
      cfg->lookahead_depth = 8; /* Support.gpp default */
    else
      {
        cfg->lookahead_depth = v ? (int) strtol (v, NULL, 10) : 8;
        if (cfg->lookahead_depth <= 0)
          cfg->lookahead_depth = 8;
      }
    v = md_atn_get (spec, "PreserveAmbiguity");
    cfg->preserve_ambiguity = v ? (!strcmp (v, "true") || strstr (v, "true") != NULL) : 1;
    v = md_atn_get (spec, "EmitStatistics");
    cfg->emit_statistics = v ? (!strcmp (v, "true") || strstr (v, "true") != NULL) : 0;
  }
  /* GPP-resolved booleans such as ANSI_C_USE_ATN ("true"/"false") and
   * ANSI_C_DEBUG_LEVEL ("1"/"0") arrive as Config values already; honor
   * numeric forms for UseATN/DebugLevel too. */
  {
    const char *v = md_config_get (spec, "UseATN");
    if (v && (!strcmp (v, "1") || !strcmp (v, "0")))
      cfg->use_atn = !strcmp (v, "1");
    v = md_config_get (spec, "DebugLevel");
    if (!v)
      ;
    else if (!strcmp (v, "ANSI_C_DEBUG_LEVEL"))
      cfg->debug_level = 0; /* Support.gpp default */
    else if (v[0] >= '0' && v[0] <= '9' && v[1] == '\0')
      cfg->debug_level = v[0] - '0';
  }
  /* Prediction is enabled only when both Config.UseATN and
   * ATN.AdaptiveLookahead agree (README contract). */
  if (!cfg->use_atn)
    cfg->adaptive_lookahead = 0;
}

static int
lexer_known (const char *name)
{
  return (!strcmp (name, "PCRE2") || !strcmp (name, "Scannerless")
          || !strcmp (name, "RE2") || !strcmp (name, "libchomsky3")
          || !strcmp (name, "POSIX") || strstr (name, "PCRE") != NULL
          || strstr (name, "Scannerless") != NULL);
}

static int
helper_head_ok (const char *value, md_error_t *err, const char *field)
{
  char head[64];
  size_t k = 0;
  const char *p;
  if (!value || !value[0])
    return 0; /* absent: not this validator's concern */
  /* Plain words (macro fallbacks, bare names) carry no call to check. */
  if (!strchr (value, '('))
    return 0;
  for (p = value; *p && *p != '(' && k + 1 < sizeof (head); p++)
    if (!isspace ((unsigned char) *p))
      head[k++] = *p;
  head[k] = '\0';
  if (!md_stdext_known (head))
    {
      if (err)
        snprintf (err->message, sizeof (err->message),
                  "%s uses unknown helper \"%s\"", field, head);
      return -1;
    }
  if (!md_stdext_eval (value))
    {
      if (err)
        snprintf (err->message, sizeof (err->message),
                  "%s has malformed helper call \"%s\"", field, value);
      return -1;
    }
  return 0;
}

int
md_entrypoint_validate (const md_spec_t *spec, md_error_t *err)
{
  md_build_config_t cfg;
  md_entrypoint_resolve (spec, &cfg);
  if (!spec->entry.lang_name || !spec->entry.lang_name[0])
    {
      if (err)
        snprintf (err->message, sizeof (err->message),
                  "Entrypoint.LanguageInfo.Name is required");
      return -1;
    }
  if (helper_head_ok (spec->entry.lang_type, err, "LanguageInfo.Type") != 0
      || helper_head_ok (spec->entry.standard_url, err,
                         "LanguageInfo.Standard")
          != 0
      || helper_head_ok (spec->entry.license, err, "GrammarInfo.License")
             != 0)
    return -1;
  if (!lexer_known (cfg.main_lexer))
    {
      if (err)
        snprintf (err->message, sizeof (err->message),
                  "unknown MainLexer \"%s\"", cfg.main_lexer);
      return -1;
    }
  if (!lexer_known (cfg.fallback_lexer))
    {
      if (err)
        snprintf (err->message, sizeof (err->message),
                  "unknown FallbackLexer \"%s\"", cfg.fallback_lexer);
      return -1;
    }
  if (cfg.lookahead_depth < 1 || cfg.lookahead_depth > 64)
    {
      if (err)
        snprintf (err->message, sizeof (err->message),
                  "ATN.MaxDepth %d must be between 1 and 64",
                  cfg.lookahead_depth);
      return -1;
    }
  if (cfg.max_errors < 0 || cfg.max_errors > 10000)
    {
      if (err)
        snprintf (err->message, sizeof (err->message),
                  "Config.MaxErrors %d out of range", cfg.max_errors);
      return -1;
    }
  return 0;
}

const md_option_t *
md_entrypoint_find_option (const md_spec_t *spec, const char *flag)
{
  for (size_t i = 0; i < spec->entry.option_count; i++)
    {
      const md_option_t *o = &spec->entry.options[i];
      if (o->long_name && !strcmp (o->long_name, flag))
        return o;
      if (o->short_name && !strcmp (o->short_name, flag))
        return o;
    }
  return NULL;
}
