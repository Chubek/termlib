#include "internal.h"

/* Concrete indentation/alignment is expressed as integer differences too:
   pin columns with x-0<=column and 0-x<=-column, then add grammar relations. */
GLR_STD_RUN(indentation_and_layout)
{
  return glr_std_layout_sensitive (context, winner, options);
}
