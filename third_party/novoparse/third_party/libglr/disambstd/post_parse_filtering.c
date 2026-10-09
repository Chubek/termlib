#include "internal.h"

/* Call on completed derivation candidates to apply a whole-tree predicate. */
GLR_STD_RUN(post_parse_filtering)
{
  return glr_std_filter (context, winner, options);
}
