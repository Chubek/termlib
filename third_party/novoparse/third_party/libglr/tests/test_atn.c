#include <glr/atn.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_nfa(void)
{
  glr_atn_t *a = glr_atn_create();
  uint32_t b = glr_atn_add_state(a);
  uint32_t c = glr_atn_add_state(a);
  uint32_t d = glr_atn_add_state(a);
  int a1[] = {1};
  int ab[] = {1, 2};
  assert(a != NULL && b != UINT32_MAX && c != UINT32_MAX && d != UINT32_MAX);
  assert(glr_atn_add_epsilon(a, glr_atn_start_state(a), b) == 0);
  assert(glr_atn_add_symbol(a, b, c, 1) == 0);
  assert(glr_atn_add_symbol(a, c, d, 2) == 0);
  assert(glr_atn_set_accepting(a, d, true, -1) == 0);
  assert(glr_atn_match(a, a1, 1) == 0);
  assert(glr_atn_match(a, ab, 2) == 1);
  glr_atn_destroy(a);
}

static void test_grammar_compilation(void)
{
  glr_symbol_t start = { GLR_SYMBOL_NONTERMINAL, 0, (char *)"S", NULL };
  glr_symbol_t token = { GLR_SYMBOL_TERMINAL, 1, (char *)"x", NULL };
  glr_symbol_t *symbols[] = { &start, &token };
  glr_symbol_t *body[] = { &token };
  glr_production_t production;
  glr_production_t *productions[] = { &production };
  glr_grammar_t grammar;
  glr_atn_t *atn;
  int input[] = {1};

  memset(&production, 0, sizeof(production));
  production.id = 0;
  production.head = &start;
  production.body = body;
  production.body_length = 1;

  memset(&grammar, 0, sizeof(grammar));
  grammar.symbols = symbols;
  grammar.symbol_count = 2;
  grammar.productions = productions;
  grammar.production_count = 1;
  grammar.start_symbol = &start;

  atn = glr_atn_from_grammar(&grammar);
  assert(atn != NULL);
  assert(glr_atn_rule_start(atn, 0) != UINT32_MAX);
  assert(glr_atn_match_from(atn, glr_atn_rule_start(atn, 0), input, 1) == 1);
  glr_atn_destroy(atn);
}

int main(void)
{
  test_nfa();
  test_grammar_compilation();
  puts("ATN tests passed");
  return 0;
}
