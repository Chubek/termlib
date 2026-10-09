#include "internal.h"

GLR_STD_RUN(layout_sensitive)
{
  for (size_t i = 0; i < context->candidate_count; ++i)
    if (!context->candidates[i].rejected)
      {
        glr_disambstd_layout_t layout = {0};
        if (!options->layout (context, &context->candidates[i], &layout, options->user_data))
          { context->candidates[i].rejected = true; continue; }
        int status = glr_std_layout_check (&layout);
        if (status < 0) return GLR_DISAMBIG_ERROR;
        if (!status) context->candidates[i].rejected = true;
      }
  return glr_std_finish (context, winner);
}
