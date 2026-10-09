/**
 * @file atn_nfa.c
 * @brief Stand-alone epsilon-NFA/ATN construction and matching example.
 *
 * The network recognizes either "ab" or "ac".  This is the primitive ATN
 * operation used by grammar front ends before they hand a grammar to GLR.
 */
#include <glr/atn.h>
#include <stdio.h>

int main(void)
{
  glr_atn_t *atn = glr_atn_create();
  uint32_t split, a, b, c;
  int ab[] = {1, 2};
  int ac[] = {1, 3};
  int ad[] = {1, 4};

  if (atn == NULL)
    return 1;
  split = glr_atn_add_state(atn);
  a = glr_atn_add_state(atn);
  b = glr_atn_add_state(atn);
  c = glr_atn_add_state(atn);
  if (split == UINT32_MAX || a == UINT32_MAX || b == UINT32_MAX || c == UINT32_MAX)
    return 1;

  /* start --epsilon--> split --1--> a --2/3--> b/c */
  if (glr_atn_add_epsilon(atn, glr_atn_start_state(atn), split) != 0
      || glr_atn_add_symbol(atn, split, a, 1) != 0
      || glr_atn_add_symbol(atn, a, b, 2) != 0
      || glr_atn_add_symbol(atn, a, c, 3) != 0
      || glr_atn_set_accepting(atn, b, true, -1) != 0
      || glr_atn_set_accepting(atn, c, true, -1) != 0)
    {
      glr_atn_destroy(atn);
      return 1;
    }

  printf("ab: %s\n", glr_atn_match(atn, ab, 2) == 1 ? "accepted" : "rejected");
  printf("ac: %s\n", glr_atn_match(atn, ac, 2) == 1 ? "accepted" : "rejected");
  printf("ad: %s\n", glr_atn_match(atn, ad, 2) == 1 ? "accepted" : "rejected");
  glr_atn_destroy(atn);
  return 0;
}
