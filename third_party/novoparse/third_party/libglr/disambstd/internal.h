#ifndef GLR_DISAMBSTD_INTERNAL_H
#define GLR_DISAMBSTD_INTERNAL_H
#include <glr/disambstd.h>
typedef struct { glr_disambstd_options_t options; } glr_std_state_t;
typedef glr_disambig_result_t (*glr_std_run_fn) (
    glr_disambig_context_t *, size_t *, const glr_disambstd_options_t *);
glr_disambig_result_t glr_std_filter (glr_disambig_context_t *, size_t *,
                                     const glr_disambstd_options_t *);
glr_disambig_result_t glr_std_rank (glr_disambig_context_t *, size_t *,
                                   const glr_disambstd_options_t *);
glr_disambig_result_t glr_std_finish (glr_disambig_context_t *, size_t *);
int glr_std_layout_check (const glr_disambstd_layout_t *);
glr_disambig_result_t glr_std_tree_choose (
    glr_disambig_context_t *, size_t *, glr_disambig_score_fn, void *, bool);
glr_disambig_result_t glr_std_analyze (glr_disambig_context_t *, size_t *,
                                      const glr_disambstd_options_t *,
                                      glr_disambstd_kind_t);
#define GLR_STD_RUN(name) glr_disambig_result_t glr_std_##name ( \
  glr_disambig_context_t *context, size_t *winner, \
  const glr_disambstd_options_t *options)
GLR_STD_RUN(attribute_grammar);
GLR_STD_RUN(case_sensitivity);
GLR_STD_RUN(indentation_and_layout);
GLR_STD_RUN(island_parsing);
GLR_STD_RUN(layout_sensitive);
GLR_STD_RUN(lexical);
GLR_STD_RUN(longest_match);
GLR_STD_RUN(name_resolution);
GLR_STD_RUN(post_parse_filtering);
GLR_STD_RUN(prefer_avoid);
GLR_STD_RUN(scannerless_priorities);
GLR_STD_RUN(bounded_sat);
GLR_STD_RUN(bounded_smt);
GLR_STD_RUN(counter_example_guided);
#endif
