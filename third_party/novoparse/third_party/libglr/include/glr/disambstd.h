#ifndef GLR_DISAMBSTD_H
#define GLR_DISAMBSTD_H

#include <glr/disambiguate.h>

#ifdef __cplusplus
extern "C" {
#endif

/* All factories return ordinary parser hooks. Options are copied; referenced
   objects must outlive the hook. user_data ownership transfers only on success. */
typedef enum {
  GLR_DISAMBSTD_PRECEDENCE, GLR_DISAMBSTD_ASSOCIATIVITY,
  GLR_DISAMBSTD_PREDICATE, GLR_DISAMBSTD_SEMANTIC,
  GLR_DISAMBSTD_DYNAMIC_PROGRAMMING, GLR_DISAMBSTD_PROBABILITY,
  GLR_DISAMBSTD_ATTRIBUTE_GRAMMAR, GLR_DISAMBSTD_CASE_SENSITIVITY,
  GLR_DISAMBSTD_INDENTATION_AND_LAYOUT, GLR_DISAMBSTD_ISLAND_PARSING,
  GLR_DISAMBSTD_LAYOUT_SENSITIVE, GLR_DISAMBSTD_LEXICAL,
  GLR_DISAMBSTD_LONGEST_MATCH, GLR_DISAMBSTD_NAME_RESOLUTION,
  GLR_DISAMBSTD_POST_PARSE_FILTERING, GLR_DISAMBSTD_PREFER_AVOID,
  GLR_DISAMBSTD_SCANNERLESS_PRIORITIES, GLR_DISAMBSTD_BOUNDED_SAT,
  GLR_DISAMBSTD_BOUNDED_SMT, GLR_DISAMBSTD_COUNTER_EXAMPLE_GUIDED,
  GLR_DISAMBSTD_COUNT
} glr_disambstd_kind_t;

/* Integer difference constraints: variable[x] - variable[y] <= upper.
   Variable 0 is the origin. Indices are local to one invocation. */
typedef struct { size_t x, y; int64_t upper; } glr_disambstd_constraint_t;
typedef struct {
  size_t variable_count;
  const glr_disambstd_constraint_t *constraints;
  size_t constraint_count;
} glr_disambstd_layout_t;
typedef bool (*glr_disambstd_layout_fn) (
    const glr_disambig_context_t *, const glr_disambig_candidate_t *,
    glr_disambstd_layout_t *, void *);

/* Bottom-up synthesized attribute. child_values are in grammar order.
   Returning false rejects the tree. Cyclic forests are reported as errors. */
typedef bool (*glr_disambstd_attribute_fn) (
    const glr_disambig_context_t *, const glr_forest_node_t *,
    const glr_disambig_candidate_t *, const double *child_values,
    size_t child_count, double *value, void *);
typedef const char *(*glr_disambstd_text_fn) (
    const glr_disambig_context_t *, const glr_disambig_candidate_t *, void *);

typedef struct {
  const int *tokens; size_t token_count;
  /* Preorder production IDs uniquely describe each ordered derivation. */
  const int *productions; size_t production_count;
} glr_disambstd_derivation_t;
typedef bool (*glr_disambstd_derivation_fn) (
    const glr_disambstd_derivation_t *, glr_disambstd_layout_t *, void *);
/* Return true after refining user state; false stops with this witness.
   The next iteration re-evaluates derivation_filter for every derivation. */
typedef bool (*glr_disambstd_refine_fn) (
    const glr_disambstd_derivation_t *, const glr_disambstd_derivation_t *, void *);
typedef enum {
  GLR_DISAMBSTD_CHECK_ERROR = -1, GLR_DISAMBSTD_CHECK_CLEAR = 0,
  GLR_DISAMBSTD_CHECK_AMBIGUOUS = 1, GLR_DISAMBSTD_CHECK_LIMIT = 2
} glr_disambstd_check_result_t;
typedef struct {
  glr_disambstd_check_result_t result;
  int *tokens; size_t token_count;
  int *productions[2]; size_t production_count[2];
  int64_t *layout_values; size_t layout_variable_count; /* SMT model, origin 0 */
  size_t iterations;
} glr_disambstd_report_t;

typedef struct {
  const char *name;
  unsigned int priority;
  void *user_data;
  glr_disambig_destroy_fn destroy;
  glr_disambig_predicate_fn predicate;
  glr_disambig_int_resolver_fn rank;
  glr_disambig_assoc_resolver_fn associativity;
  glr_disambig_score_fn score;
  glr_disambstd_attribute_fn attribute;
  glr_disambstd_text_fn text;
  const char *expected_text; /* case-sensitivity: exact byte comparison */
  glr_disambstd_layout_fn layout;
  glr_disambstd_derivation_fn derivation_filter;
  glr_disambstd_refine_fn refine;
  size_t max_tokens; /* includes the empty sentence when zero */
  size_t max_depth; /* maximum production expansions along a root/leaf path */
  size_t max_derivations; /* budget for partial plus completed derivation states */
  size_t max_iterations; /* counterexample refinement rounds */
  glr_disambstd_report_t *report; /* optional, initialized to zero by caller */
} glr_disambstd_options_t;

void glr_disambstd_options_init (glr_disambstd_options_t *);
const char *glr_disambstd_name (glr_disambstd_kind_t);
glr_disambig_hook_t *glr_disambstd_create (
    glr_disambstd_kind_t, const glr_disambstd_options_t *);
glr_disambig_hook_t *glr_disambstd_create_named (
    const char *, const glr_disambstd_options_t *);
/* Also usable without a parser, e.g. by a grammar compiler such as elkhound.
   CLEAR is only a result within both the token and derivation-depth bounds.
   Resource exhaustion returns LIMIT, never CLEAR. Does not own user_data. */
glr_disambstd_check_result_t glr_disambstd_check (
    glr_disambstd_kind_t, const glr_grammar_t *,
    const glr_disambstd_options_t *, glr_disambstd_report_t *);
void glr_disambstd_report_clear (glr_disambstd_report_t *);

#ifdef __cplusplus
}
#endif
#endif
