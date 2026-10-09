#include "internal.h"

GLR_STD_RUN(counter_example_guided)
{
  return glr_std_analyze (context, winner, options, GLR_DISAMBSTD_COUNTER_EXAMPLE_GUIDED);
}
