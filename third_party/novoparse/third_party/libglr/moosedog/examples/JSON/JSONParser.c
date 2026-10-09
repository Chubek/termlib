/*
 * JSONParser.c -- application-side entry point for a Moosedog-generated JSON
 * parser.  The concrete Moosedog types/functions are emitted by the frontend;
 * this file intentionally contains only the generated-type declarations used
 * by applications embedding the example.
 */
#include "Moosedog.h"
#include "JSON.ast.h"
#include "JSON.lexer.h"
#include "JSON.parser.h"

int
main(void)
{
  /* Generated headers provide the concrete implementation.  Keeping this
     translation unit deliberately small makes it safe to regenerate all
     parser artifacts without hand-editing generated code. */
  return 0;
}
