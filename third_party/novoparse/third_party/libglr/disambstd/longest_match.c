#include "internal.h"

GLR_STD_RUN(longest_match)
{
  size_t length = 0, start = 0; bool found = false;
  (void)options;
  for (size_t i = 0; i < context->candidate_count; ++i)
    {
      glr_disambig_candidate_t *c = &context->candidates[i];
      if (c->rejected) continue;
      if (c->end_position < c->start_position) return GLR_DISAMBIG_ERROR;
      if (found && start != c->start_position) return GLR_DISAMBIG_ERROR;
      start = c->start_position; found = true;
      size_t n = c->end_position - start;
      if (n > length) length = n;
    }
  for (size_t i = 0; i < context->candidate_count; ++i)
    if (!context->candidates[i].rejected
        && context->candidates[i].end_position - start < length)
      context->candidates[i].rejected = true;
  return glr_std_finish (context, winner);
}
