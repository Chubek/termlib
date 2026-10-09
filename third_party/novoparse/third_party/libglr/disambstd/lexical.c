#include "internal.h"

/* Lexical admissibility followed by lexical class priority. */
GLR_STD_RUN(lexical)
{
  if (options->predicate)
    {
      glr_disambig_result_t r = glr_std_filter (context, winner, options);
      if (r != GLR_DISAMBIG_NO_MATCH) return r;
    }
  return glr_std_rank (context, winner, options);
}
