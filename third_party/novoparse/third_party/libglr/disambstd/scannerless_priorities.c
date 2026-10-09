#include "internal.h"

/* Predicate implements follow/reject restrictions; rank orders productions. */
GLR_STD_RUN(scannerless_priorities)
{
  return glr_std_lexical (context, winner, options);
}
