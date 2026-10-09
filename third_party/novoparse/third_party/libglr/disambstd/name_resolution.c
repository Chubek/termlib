#include "internal.h"

/* The predicate resolves names in the frontend's scope/symbol environment. */
GLR_STD_RUN(name_resolution)
{
  return glr_std_filter (context, winner, options);
}
