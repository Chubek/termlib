#include "internal.h"
#include <stdlib.h>
#include <string.h>

static const char *const names[] = {
  "precedence", "associativity", "predicate", "semantic",
  "dynamic-programming", "probability", "attribute-grammar",
  "case-sensitivity", "indentation-and-layout", "island-parsing",
  "layout-sensitive", "lexical", "longest-match", "name-resolution",
  "post-parse-filtering", "prefer-avoid", "scannerless-priorities",
  "bounded-sat", "bounded-smt", "counter-example-guided"
};

void glr_disambstd_options_init (glr_disambstd_options_t *o)
{
  if (!o) return;
  memset (o, 0, sizeof (*o));
  o->max_tokens = 8; o->max_depth = 8;
  o->max_derivations = 10000; o->max_iterations = 32;
}

const char *glr_disambstd_name (glr_disambstd_kind_t k)
{
  return k >= 0 && k < GLR_DISAMBSTD_COUNT ? names[k] : NULL;
}

glr_disambig_result_t glr_std_finish (glr_disambig_context_t *c, size_t *w)
{
  size_t n = glr_disambig_context_active_count (c);
  if (!n) return GLR_DISAMBIG_ERROR;
  if (n != 1) return GLR_DISAMBIG_NO_MATCH;
  if (w) *w = glr_disambig_context_first_active (c);
  return GLR_DISAMBIG_RESOLVED;
}

glr_disambig_result_t glr_std_filter (glr_disambig_context_t *c, size_t *w,
                                     const glr_disambstd_options_t *o)
{
  for (size_t i = 0; i < c->candidate_count; ++i)
    if (!c->candidates[i].rejected
        && !o->predicate (c, &c->candidates[i], o->user_data))
      c->candidates[i].rejected = true;
  return glr_std_finish (c, w);
}

glr_disambig_result_t glr_std_rank (glr_disambig_context_t *c, size_t *w,
                                   const glr_disambstd_options_t *o)
{
  int *values = calloc (c->candidate_count, sizeof (*values));
  int best = 0; bool found = false;
  if (!values && c->candidate_count) return GLR_DISAMBIG_ERROR;
  for (size_t i = 0; i < c->candidate_count; ++i)
    if (!c->candidates[i].rejected)
      {
        values[i] = o->rank ? o->rank (c, &c->candidates[i], o->user_data)
                            : c->candidates[i].precedence;
        if (!found || values[i] > best) best = values[i];
        found = true;
      }
  for (size_t i = 0; i < c->candidate_count; ++i)
    if (!c->candidates[i].rejected && values[i] < best)
      c->candidates[i].rejected = true;
  free (values);
  return glr_std_finish (c, w);
}

typedef struct {
  glr_disambstd_options_t options;
  glr_std_run_fn run;
} state_t;
static void destroy_state (void *data)
{
  state_t *s = data;
  if (s->options.destroy) s->options.destroy (s->options.user_data);
  free (s);
}
static glr_disambig_result_t run (glr_disambig_context_t *c, size_t *w, void *data)
{
  state_t *s = data;
  if (w) *w = SIZE_MAX;
  if (!c || (c->candidate_count && !c->candidates)) return GLR_DISAMBIG_ERROR;
  return s->run (c, w, &s->options);
}

glr_disambig_hook_t *glr_disambstd_create (
    glr_disambstd_kind_t k, const glr_disambstd_options_t *options)
{
  glr_disambstd_options_t defaults;
  glr_disambstd_options_init (&defaults);
  const glr_disambstd_options_t *o = options ? options : &defaults;
  const char *name = o->name ? o->name : glr_disambstd_name (k);
  if (!glr_disambstd_name (k)) return NULL;
  switch (k) {
  case GLR_DISAMBSTD_PRECEDENCE:
    return glr_disambig_precedence_hook_create (name, o->priority, o->rank, o->user_data, o->destroy);
  case GLR_DISAMBSTD_ASSOCIATIVITY:
    return glr_disambig_associativity_hook_create (name, o->priority, o->rank, o->associativity, o->user_data, o->destroy);
  case GLR_DISAMBSTD_PREDICATE:
    return glr_disambig_predicate_hook_create (name, o->priority, o->predicate, o->user_data, o->destroy);
  case GLR_DISAMBSTD_SEMANTIC:
    return glr_disambig_semantic_hook_create (name, o->priority, o->predicate, o->user_data, o->destroy);
  case GLR_DISAMBSTD_DYNAMIC_PROGRAMMING:
    return glr_disambig_dynamic_programming_hook_create (name, o->priority, o->score, o->user_data, o->destroy);
  case GLR_DISAMBSTD_PROBABILITY:
    return glr_disambig_probability_hook_create (name, o->priority, o->score, o->user_data, o->destroy);
  default: break;
  }
  static glr_std_run_fn const runners[] = {
    glr_std_attribute_grammar, glr_std_case_sensitivity,
    glr_std_indentation_and_layout, glr_std_island_parsing,
    glr_std_layout_sensitive, glr_std_lexical, glr_std_longest_match,
    glr_std_name_resolution, glr_std_post_parse_filtering,
    glr_std_prefer_avoid, glr_std_scannerless_priorities,
    glr_std_bounded_sat, glr_std_bounded_smt, glr_std_counter_example_guided
  };
  if ((k == GLR_DISAMBSTD_ATTRIBUTE_GRAMMAR && !o->attribute)
      || (k == GLR_DISAMBSTD_CASE_SENSITIVITY && (!o->text || !o->expected_text))
      || ((k == GLR_DISAMBSTD_INDENTATION_AND_LAYOUT || k == GLR_DISAMBSTD_LAYOUT_SENSITIVE) && !o->layout)
      || ((k == GLR_DISAMBSTD_NAME_RESOLUTION || k == GLR_DISAMBSTD_POST_PARSE_FILTERING) && !o->predicate)
      || (k == GLR_DISAMBSTD_ISLAND_PARSING && !o->rank)
      || (k == GLR_DISAMBSTD_COUNTER_EXAMPLE_GUIDED && (!o->refine || !o->derivation_filter)))
    return NULL;
  state_t *s = malloc (sizeof (*s));
  if (!s) return NULL;
  s->options = *o; s->run = runners[k - GLR_DISAMBSTD_ATTRIBUTE_GRAMMAR];
  glr_disambig_hook_t *hook = glr_disambig_hook_create (name, o->priority, run, s, destroy_state);
  if (!hook) free (s);
  return hook;
}

glr_disambig_hook_t *glr_disambstd_create_named (
    const char *name, const glr_disambstd_options_t *o)
{
  if (name)
    for (int k = 0; k < GLR_DISAMBSTD_COUNT; ++k)
      if (!strcmp (name, names[k])) return glr_disambstd_create ((glr_disambstd_kind_t)k, o);
  return NULL;
}

glr_disambig_result_t glr_std_analyze (glr_disambig_context_t *c, size_t *w,
    const glr_disambstd_options_t *o, glr_disambstd_kind_t k)
{
  glr_disambstd_report_t local = {0};
  glr_disambstd_report_t *report = o->report ? o->report : &local;
  glr_disambstd_check_result_t result = glr_disambstd_check (k, c->grammar, o, report);
  (void)w;
  if (!o->report) glr_disambstd_report_clear (&local);
  /* Analysis never silently selects a parse or treats bounded clarity as proof. */
  return result == GLR_DISAMBSTD_CHECK_ERROR || result == GLR_DISAMBSTD_CHECK_LIMIT
      ? GLR_DISAMBIG_ERROR : GLR_DISAMBIG_NO_MATCH;
}
