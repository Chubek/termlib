#include "internal.h"

/* The frontend ranks recognized island coverage (or negative water cost). */
GLR_STD_RUN(island_parsing)
{
  return glr_std_rank (context, winner, options);
}
