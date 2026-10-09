#include "internal.h"
#include <string.h>

GLR_STD_RUN(case_sensitivity)
{
  for (size_t i = 0; i < context->candidate_count; ++i)
    if (!context->candidates[i].rejected)
      {
        const char *text = options->text (context, &context->candidates[i], options->user_data);
        if (!text || strcmp (text, options->expected_text))
          context->candidates[i].rejected = true;
      }
  return glr_std_finish (context, winner);
}
