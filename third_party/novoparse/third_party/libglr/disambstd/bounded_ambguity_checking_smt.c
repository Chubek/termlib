#include "internal.h"

GLR_STD_RUN(bounded_smt)
{
  return glr_std_analyze (context, winner, options, GLR_DISAMBSTD_BOUNDED_SMT);
}
