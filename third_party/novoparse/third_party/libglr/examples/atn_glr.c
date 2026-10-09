/**
 * @file atn_glr.c
 * @brief Shows an ATN and the GLR parser operating on the same grammar.
 *
 * The ATN is the generator-side production representation; the GLR parser
 * remains responsible for the full context-free parse and SPPF construction.
 */
#include <glr/glr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static glr_grammar_t *make_grammar(void)
{
  glr_grammar_t *g = glr_grammar_create();
  int expr, n, plus;
  glr_symbol_t *body[3];
  if (g == NULL)
    return NULL;
  expr = glr_grammar_add_symbol(g, GLR_SYMBOL_NONTERMINAL, "expr");
  n = glr_grammar_add_symbol(g, GLR_SYMBOL_TERMINAL, "n");
  plus = glr_grammar_add_symbol(g, GLR_SYMBOL_TERMINAL, "+");
  if (expr < 0 || n < 0 || plus < 0)
    goto fail;
  body[0] = glr_grammar_get_symbol(g, expr);
  body[1] = glr_grammar_get_symbol(g, plus);
  body[2] = glr_grammar_get_symbol(g, expr);
  if (glr_grammar_add_production(g, expr, body, 3) < 0)
    goto fail;
  body[0] = glr_grammar_get_symbol(g, n);
  if (glr_grammar_add_production(g, expr, body, 1) < 0
      || glr_grammar_set_start_symbol(g, expr) != 0)
    goto fail;
  return g;
fail:
  glr_grammar_destroy(g);
  return NULL;
}

int main(void)
{
  glr_grammar_t *g = make_grammar();
  glr_atn_t *atn;
  glr_parser_t *parser;
  glr_parse_result_t result;
  int expr, n, plus;
  int production_body[] = {0, 0, 0};
  (void)production_body;

  if (g == NULL)
    return 1;
  atn = glr_atn_from_grammar(g);
  if (atn == NULL)
    {
      glr_grammar_destroy(g);
      return 1;
    }
  expr = glr_grammar_find_symbol(g, "expr", GLR_SYMBOL_NONTERMINAL);
  n = glr_grammar_find_symbol(g, "n", GLR_SYMBOL_TERMINAL);
  plus = glr_grammar_find_symbol(g, "+", GLR_SYMBOL_TERMINAL);
  {
    int rhs[] = {n};
    printf("expr -> n through ATN: %s\n",
           glr_atn_match_from(atn, glr_atn_rule_start(atn, expr), rhs, 1) == 1
             ? "accepted" : "rejected");
    (void)plus;
  }

  /* The same grammar is now consumed by the ordinary GLR parser. */
  parser = glr_parser_create(g);
  if (parser == NULL)
    {
      glr_atn_destroy(atn);
      glr_grammar_destroy(g);
      return 1;
    }
  result = glr_parse(parser, "n+n", strlen("n+n"));
  printf("GLR parse of n+n: %s (position %zu)\n",
         result.error == GLR_PARSE_SUCCESS ? "accepted" : "rejected",
         result.position);

  glr_parser_destroy(parser);
  glr_atn_destroy(atn);
  glr_grammar_destroy(g);
  return result.error == GLR_PARSE_SUCCESS ? 0 : 1;
}
