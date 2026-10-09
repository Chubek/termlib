#ifndef MOOSEDOG_PARSER_H
#define MOOSEDOG_PARSER_H
/*
 * moosedog_parser.h -- .grm specification model and file parser.
 *
 * The .grm dialect has four top-level blocks:
 *   Entrypoint { ... }  AST { ... }  Lexical { ... }  Syntactic { ... }
 * This header defines the in-memory model for all four plus the
 * parse/validate entry points used by the emitter, CLI and tests.
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct md_error
{
  char message[1024];
  int line;
} md_error_t;

/* ---------- AST block ---------- */
typedef struct md_ast_field
{
  char *name;
  char *type;
  char *arity; /* "One" | "Many" */
} md_ast_field_t;

typedef struct md_ast_node
{
  char *name;
  char *tag;
  char *kind; /* "Union" | "Struct" | "Leaf" */
  char **variants;
  size_t variant_count;
  md_ast_field_t *fields;
  size_t field_count;
} md_ast_node_t;

/* ---------- Lexical block ---------- */
typedef struct md_token_def
{
  char *name;
  char *pattern;    /* NULL when Classifier is used */
  char *classifier; /* NULL when Pattern is used */
  int literal;      /* 1 == Yes, 0 == No (default 0) */
  int is_skip;      /* 1 for Skip(...), 0 for Token(...) */
} md_token_def_t;

/* ---------- Syntactic block ---------- */
typedef struct md_alt
{
  char **seq;      /* token/rule names in order */
  size_t seq_count;
  char *action;    /* raw action text, may be NULL */
} md_alt_t;

typedef struct md_rule
{
  char *name;
  char *returns; /* return type after -> */
  md_alt_t *alts;
  size_t alt_count;
} md_rule_t;

/* ---------- Entrypoint block ---------- */
typedef struct md_option
{
  char *long_name;   /* "--debug" */
  char *short_name;  /* "-d", may be NULL */
  char *define;      /* preprocessor symbol, may be NULL */
  char *type;        /* "Bool" | "String" | "Int" | "Path" */
  char *def_value;   /* Default = ... */
  char *range_min;   /* IntRange low, may be NULL */
  char *range_max;   /* IntRange high, may be NULL */
  char *description;
  char *bind;        /* Bind = Config.X, may be NULL */
  int bind_invert;
} md_option_t;

typedef struct md_entrypoint
{
  char *lang_name;
  char *lang_type;
  char *standard_url;
  char **authors;
  size_t author_count;
  char *license;
  char **revisions;
  size_t revision_count;
  char **disambiguators;
  size_t disambiguator_count;
  char **rewriters;
  size_t rewriter_count;
  char **queries;
  size_t query_count;
  /* Config key/value pairs (raw strings; known keys interpreted later). */
  char **cfg_keys;
  char **cfg_values;
  size_t cfg_count;
  /* ATN key/value pairs. */
  char **atn_keys;
  char **atn_values;
  size_t atn_count;
  md_option_t *options;
  size_t option_count;
  char **pipeline; /* e.g. Preprocess, Rewrite, CompileAST, ... */
  size_t pipeline_count;
} md_entrypoint_t;

typedef struct md_spec
{
  md_entrypoint_t entry;
  md_ast_node_t *nodes;
  size_t node_count;
  md_token_def_t *tokens;
  size_t token_count;
  md_rule_t *rules;
  size_t rule_count;
  char *start_rule;
} md_spec_t;

void md_spec_init (md_spec_t *spec);
void md_spec_destroy (md_spec_t *spec);

/* Parse one .grm file. Returns 0 on success, -1 on failure with err set. */
int md_parse_file (const char *path, md_spec_t *spec, md_error_t *err);
int md_parse_string (const char *text, size_t len, const char *origin,
                     md_spec_t *spec, md_error_t *err);

/* Cross-reference validation: StartRule exists, Seq names resolve to a
 * rule or token, AST return types resolve, option Bind targets exist.
 * Returns 0 when valid, -1 with err set otherwise. */
int md_spec_validate (const md_spec_t *spec, md_error_t *err);

/* Config helpers (search cfg_keys linearly). */
const char *md_config_get (const md_spec_t *spec, const char *key);
const char *md_atn_get (const md_spec_t *spec, const char *key);
int md_config_get_bool (const md_spec_t *spec, const char *key, int dflt);
long md_config_get_int (const md_spec_t *spec, const char *key, long dflt);

#ifdef __cplusplus
}
#endif

#endif /* MOOSEDOG_PARSER_H */
