#include "internal.h"

/* Positive rank prefers, zero is neutral, negative rank avoids.
   An all-avoid set retains its best alternatives instead of rejecting all. */
GLR_STD_RUN(prefer_avoid)
{
  return glr_std_rank (context, winner, options);
}
