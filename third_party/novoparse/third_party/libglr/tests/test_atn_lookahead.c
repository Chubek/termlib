#include <glr/atn.h>
#include <glr/disambiguate.h>
#include <glr/parser.h>
#include <glr/parsetbl.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Grammar: S -> X, X -> a b | a c (ids: S=0 X=1 a=2 b=3 c=4). */
static glr_grammar_t *
make_predict_grammar (void)
{
  glr_grammar_t *g = glr_grammar_create ();
  int s, x, a, b, c;
  glr_symbol_t *body[2];
  if (g == NULL)
    return NULL;
  s = glr_grammar_add_symbol (g, GLR_SYMBOL_NONTERMINAL, "S");
  x = glr_grammar_add_symbol (g, GLR_SYMBOL_NONTERMINAL, "X");
  a = glr_grammar_add_symbol (g, GLR_SYMBOL_TERMINAL, "a");
  b = glr_grammar_add_symbol (g, GLR_SYMBOL_TERMINAL, "b");
  c = glr_grammar_add_symbol (g, GLR_SYMBOL_TERMINAL, "c");
  if (s != 0 || x != 1 || a != 2 || b != 3 || c != 4)
    goto fail;
  body[0] = glr_grammar_get_symbol (g, x);
  if (glr_grammar_add_production (g, s, body, 1) != 0)
    goto fail;
  body[0] = glr_grammar_get_symbol (g, a);
  body[1] = glr_grammar_get_symbol (g, b);
  if (glr_grammar_add_production (g, x, body, 2) != 1)
    goto fail;
  body[1] = glr_grammar_get_symbol (g, c);
  if (glr_grammar_add_production (g, x, body, 2) != 2)
    goto fail;
  if (glr_grammar_set_start_symbol (g, s) != 0)
    goto fail;
  return g;
fail:
  glr_grammar_destroy (g);
  return NULL;
}

static void
test_production_starts (void)
{
  glr_grammar_t *g = make_predict_grammar ();
  glr_atn_t *atn;
  assert (g != NULL);
  atn = glr_atn_from_grammar (g);
  assert (atn != NULL);
  assert (glr_atn_production_start (atn, 0) != UINT32_MAX);
  assert (glr_atn_production_start (atn, 1) != UINT32_MAX);
  assert (glr_atn_production_start (atn, 2) != UINT32_MAX);
  assert (glr_atn_production_start (atn, 3) == UINT32_MAX);
  assert (glr_atn_production_start (atn, -1) == UINT32_MAX);
  assert (glr_atn_production_start (NULL, 0) == UINT32_MAX);
  glr_atn_destroy (atn);
  glr_grammar_destroy (g);
}

static void
test_prefix_viability (void)
{
  glr_grammar_t *g = make_predict_grammar ();
  glr_atn_t *atn;
  int ab[] = { 2, 3 };
  int ac[] = { 2, 4 };
  int ba[] = { 3, 2 };
  int eof[] = { 2, GLR_ATN_LOOKAHEAD_EOF, 3 };
  uint32_t rule;
  assert (g != NULL);
  atn = glr_atn_from_grammar (g);
  assert (atn != NULL);
  rule = glr_atn_rule_start (atn, 1);
  assert (rule != UINT32_MAX);
  assert (glr_atn_is_prefix_viable (atn, rule, ab, 2) == 1);
  assert (glr_atn_is_prefix_viable (atn, rule, ac, 2) == 1);
  assert (glr_atn_is_prefix_viable (atn, rule, ba, 2) == 0);
  assert (glr_atn_is_prefix_viable (atn, rule, eof, 3) == 1);
  assert (glr_atn_is_prefix_viable (atn, rule, NULL, 0) == 1);
  assert (glr_atn_is_prefix_viable (NULL, rule, ab, 2) == -1);
  assert (glr_atn_viable_prefix_length (atn, rule, ab, 2) == 2);
  assert (glr_atn_viable_prefix_length (atn, rule, ac, 2) == 2);
  assert (glr_atn_viable_prefix_length (atn, rule, ba, 2) == 0);
  assert (glr_atn_viable_prefix_length (atn, rule, eof, 3) == 1);
  assert (glr_atn_viable_prefix_length (atn, rule, NULL, 0) == 0);
  glr_atn_destroy (atn);
  glr_grammar_destroy (g);
}

static void
test_follow_sets (void)
{
  glr_grammar_t *g = make_predict_grammar ();
  glr_atn_follow_t *follow;
  assert (g != NULL);
  follow = glr_atn_follow_compute (g);
  assert (follow != NULL);
  /* S -> X at end: FOLLOW(X) = FOLLOW(S) = {EOF}. */
  assert (glr_atn_follow_contains (follow, 0, GLR_ATN_LOOKAHEAD_EOF));
  assert (glr_atn_follow_contains (follow, 1, GLR_ATN_LOOKAHEAD_EOF));
  assert (!glr_atn_follow_contains (follow, 1, 2));
  assert (!glr_atn_follow_contains (follow, 1, 3));
  assert (!glr_atn_follow_contains (follow, 2, 2));
  assert (!glr_atn_follow_contains (follow, -1, 2));
  assert (!glr_atn_follow_contains (follow, 1, 99));
  assert (!glr_atn_follow_contains (NULL, 1, 2));
  assert (glr_atn_first_contains (follow, 1, 2));
  assert (!glr_atn_first_contains (follow, 1, 3));
  assert (glr_atn_first_contains (follow, 2, 2));
  assert (!glr_atn_first_contains (follow, 2, 3));
  glr_atn_follow_destroy (follow);
  glr_atn_follow_destroy (NULL);
  glr_grammar_destroy (g);
  assert (glr_atn_follow_compute (NULL) == NULL);
}

static void
test_predict_production (void)
{
  glr_grammar_t *g = make_predict_grammar ();
  glr_atn_t *atn;
  int ab[] = { 2, 3 };
  int ac[] = { 2, 4 };
  int a[] = { 2 };
  int out = -1;
  assert (g != NULL);
  atn = glr_atn_from_grammar (g);
  assert (atn != NULL);
  assert (glr_atn_predict_production (atn, g, 1, ab, 2, &out) == 1);
  assert (out == 1);
  assert (glr_atn_predict_production (atn, g, 1, ac, 2, &out) == 1);
  assert (out == 2);
  /* Shared one-token prefix: undecided, output untouched. */
  out = -1;
  assert (glr_atn_predict_production (atn, g, 1, a, 1, &out) == 0);
  assert (out == -1);
  assert (glr_atn_predict_production (NULL, g, 1, ab, 2, &out) == -1);
  assert (glr_atn_predict_production (atn, NULL, 1, ab, 2, &out) == -1);
  assert (glr_atn_predict_production (atn, g, 2, ab, 2, &out) == -1);
  assert (glr_atn_predict_production (atn, g, 1, ab, 2, NULL) == -1);
  glr_atn_destroy (atn);
  glr_grammar_destroy (g);
}

static void
test_scan_window (void)
{
  glr_grammar_t *g = make_predict_grammar ();
  int window[4];
  size_t count = 0;
  bool deterministic = false;
  assert (g != NULL);
  assert (glr_atn_scan_window (g, "ab", 2, NULL, 0, window, 4, &count,
                               &deterministic)
          == 0);
  assert (count == 2 && deterministic);
  assert (window[0] == 2 && window[1] == 3);
  assert (glr_atn_scan_window (g, "ab", 2, " ", 1, window, 4, &count,
                               &deterministic)
          == 0);
  assert (count == 1 && window[0] == 3);
  assert (glr_atn_scan_window (NULL, "ab", 2, NULL, 0, window, 4, &count,
                               &deterministic)
          == -1);
  glr_grammar_destroy (g);
}

static void
test_scan_window_ambiguity (void)
{
  glr_grammar_t *g = glr_grammar_create ();
  int s, a, b;
  glr_symbol_t *body[1];
  int window[4];
  size_t count = 0;
  bool deterministic = true;
  assert (g != NULL);
  s = glr_grammar_add_symbol (g, GLR_SYMBOL_NONTERMINAL, "S");
  a = glr_grammar_add_symbol (g, GLR_SYMBOL_TERMINAL, "a");
  b = glr_grammar_add_symbol (g, GLR_SYMBOL_TERMINAL, "a");
  assert (s == 0 && a == 1 && b == 2);
  body[0] = glr_grammar_get_symbol (g, a);
  assert (glr_grammar_add_production (g, s, body, 1) == 0);
  body[0] = glr_grammar_get_symbol (g, b);
  assert (glr_grammar_add_production (g, s, body, 1) == 1);
  assert (glr_grammar_set_start_symbol (g, s) == 0);
  assert (glr_atn_scan_window (g, "a", 1, NULL, 0, window, 4, &count,
                               &deterministic)
          == 0);
  assert (count == 0 && !deterministic);
  glr_grammar_destroy (g);
}

static void
test_lookahead_filter (void)
{
  /* S -> A x, A -> y (ids S=0 A=1 x=2 y=3): FOLLOW(A) = {x}. */
  glr_grammar_t *g = glr_grammar_create ();
  glr_atn_follow_t *follow;
  glr_parse_table_t *table;
  glr_action_t action;
  int s, a, x, y;
  glr_symbol_t *body[2];
  glr_action_set_t actions = { 0 };
  bool keep[2] = { false, false };
  int win_x[] = { 2 };
  int win_y[] = { 3 };
  int win_ab[] = { 2, 3 };
  int win_ay[] = { 2, 2 };
  assert (g != NULL);
  s = glr_grammar_add_symbol (g, GLR_SYMBOL_NONTERMINAL, "S");
  a = glr_grammar_add_symbol (g, GLR_SYMBOL_NONTERMINAL, "A");
  x = glr_grammar_add_symbol (g, GLR_SYMBOL_TERMINAL, "x");
  y = glr_grammar_add_symbol (g, GLR_SYMBOL_TERMINAL, "y");
  assert (s == 0 && a == 1 && x == 2 && y == 3);
  body[0] = glr_grammar_get_symbol (g, a);
  body[1] = glr_grammar_get_symbol (g, x);
  assert (glr_grammar_add_production (g, s, body, 2) == 0);
  body[0] = glr_grammar_get_symbol (g, y);
  assert (glr_grammar_add_production (g, a, body, 1) == 1);
  assert (glr_grammar_set_start_symbol (g, s) == 0);
  follow = glr_atn_follow_compute (g);
  assert (follow != NULL);

  table = glr_parse_table_create (2, 5, 4);
  assert (table != NULL);
  memset (&action, 0, sizeof (action));
  action.type = GLR_ACTION_SHIFT;
  action.shift.next_state = 1;
  assert (glr_parse_table_add_action (table, 0, (uint32_t) x, action) == 0);
  memset (&action, 0, sizeof (action));
  action.type = GLR_ACTION_SHIFT;
  action.shift.next_state = 1;
  assert (glr_parse_table_add_action (table, 1, (uint32_t) y, action) == 0);

  /* Single REDUCE A -> y: kept under x, dropped under y. */
  actions.actions = &action;
  actions.action_count = 1;
  memset (&action, 0, sizeof (action));
  action.type = GLR_ACTION_REDUCE;
  action.reduce.production_id = 1;
  assert (glr_atn_lookahead_filter (g, follow, table, 0, &actions, win_x, 1,
                                    false, keep)
          == 0);
  assert (keep[0]);
  assert (glr_atn_lookahead_filter (g, follow, table, 0, &actions, win_y, 1,
                                    false, keep)
          == 0);
  assert (!keep[0]);

  /* Single SHIFT on x: kept (own lookahead always viable); with a
     deterministic deeper window it needs next-state viability. */
  memset (&action, 0, sizeof (action));
  action.type = GLR_ACTION_SHIFT;
  action.shift.next_state = 1;
  assert (glr_atn_lookahead_filter (g, follow, table, 0, &actions, win_x, 1,
                                    false, keep)
          == 0);
  assert (keep[0]);
  assert (glr_atn_lookahead_filter (g, follow, table, 0, &actions, win_ab, 2,
                                    true, keep)
          == 0);
  assert (keep[0]); /* state 1 shifts on y(3) */
  assert (glr_atn_lookahead_filter (g, follow, table, 0, &actions, win_ay, 2,
                                    true, keep)
          == 0);
  assert (!keep[0]); /* state 1 has no action on x(2) */

  /* ACCEPT only on end-of-input. */
  memset (&action, 0, sizeof (action));
  action.type = GLR_ACTION_ACCEPT;
  {
    int win_eof[] = { GLR_ATN_LOOKAHEAD_EOF };
    assert (glr_atn_lookahead_filter (g, follow, table, 0, &actions, win_eof,
                                      1, false, keep)
            == 0);
    assert (keep[0]);
    assert (glr_atn_lookahead_filter (g, follow, table, 0, &actions, win_x, 1,
                                      false, keep)
            == 0);
    assert (!keep[0]);
  }
  assert (glr_atn_lookahead_filter (NULL, follow, table, 0, &actions, win_x,
                                    1, false, keep)
          == -1);
  glr_parse_table_destroy (table);
  glr_atn_follow_destroy (follow);
  glr_grammar_destroy (g);
}

/* Ambiguous expression grammar with literal terminals. */
static glr_grammar_t *
make_expr_grammar (void)
{
  glr_grammar_t *g = glr_grammar_create ();
  int e, n, plus, star;
  glr_symbol_t *body[3];
  if (g == NULL)
    return NULL;
  e = glr_grammar_add_symbol (g, GLR_SYMBOL_NONTERMINAL, "E");
  n = glr_grammar_add_symbol (g, GLR_SYMBOL_TERMINAL, "n");
  plus = glr_grammar_add_symbol (g, GLR_SYMBOL_TERMINAL, "+");
  star = glr_grammar_add_symbol (g, GLR_SYMBOL_TERMINAL, "*");
  body[0] = glr_grammar_get_symbol (g, e);
  body[1] = glr_grammar_get_symbol (g, plus);
  body[2] = glr_grammar_get_symbol (g, e);
  if (glr_grammar_add_production (g, e, body, 3) < 0)
    goto fail;
  body[1] = glr_grammar_get_symbol (g, star);
  if (glr_grammar_add_production (g, e, body, 3) < 0)
    goto fail;
  body[0] = glr_grammar_get_symbol (g, n);
  if (glr_grammar_add_production (g, e, body, 1) < 0)
    goto fail;
  if (glr_grammar_set_start_symbol (g, e) != 0)
    goto fail;
  return g;
fail:
  glr_grammar_destroy (g);
  return NULL;
}

static void
test_parser_adaptive_pipeline (void)
{
  glr_grammar_t *g = make_expr_grammar ();
  glr_parser_t *parser;
  glr_parse_result_t result;
  glr_parser_atn_stats_t stats = { 0 };
  assert (g != NULL);
  parser = glr_parser_create (g);
  assert (parser != NULL);
  assert (!glr_parser_get_adaptive_lookahead (parser));
  assert (glr_parser_get_adaptive_lookahead_depth (parser)
          == GLR_ATN_LOOKAHEAD_DEFAULT_DEPTH);
  assert (glr_parser_set_adaptive_lookahead (parser, true) == 0);
  assert (glr_parser_get_adaptive_lookahead (parser));
  assert (glr_parser_set_adaptive_lookahead_depth (parser, 1000) == 0);
  assert (glr_parser_get_adaptive_lookahead_depth (parser)
          == GLR_ATN_LOOKAHEAD_MAX_DEPTH);
  assert (glr_parser_set_adaptive_lookahead_depth (parser, 0) == 0);
  assert (glr_parser_get_adaptive_lookahead_depth (parser)
          == GLR_ATN_LOOKAHEAD_DEFAULT_DEPTH);
  assert (glr_parser_set_adaptive_lookahead (NULL, true) == -1);
  assert (glr_parser_set_adaptive_lookahead_depth (NULL, 2) == -1);
  assert (glr_parser_get_adaptive_stats (parser, &stats) == 0);
  assert (stats.conflicts_seen == 0);
  assert (glr_parser_get_adaptive_stats (NULL, &stats) == -1);
  assert (glr_parser_get_adaptive_stats (parser, NULL) == -1);

  assert (glr_parser_require_atn (parser) != NULL);
  assert (glr_parser_get_atn (parser) != NULL);
  assert (glr_parser_require_atn (NULL) == NULL);

  result = glr_parse (parser, "n+n*n", 5);
  assert (result.error == GLR_PARSE_SUCCESS);
  assert (glr_parser_get_adaptive_stats (parser, &stats) == 0);
  assert (stats.conflicts_seen > 0);
  assert (stats.conflicts_decided <= stats.conflicts_seen);
  assert (stats.actions_pruned >= stats.conflicts_decided);

  /* A custom ATN is reused as-is; detaching falls back to auto-build. */
  {
    glr_atn_t *custom = glr_atn_from_grammar (g);
    assert (custom != NULL);
    assert (glr_parser_set_atn (parser, custom, true) == 0);
    assert (glr_parser_get_atn (parser) == custom);
    result = glr_parse (parser, "n", 1);
    assert (result.error == GLR_PARSE_SUCCESS);
    assert (glr_parser_set_atn (parser, NULL, false) == 0);
    assert (glr_parser_get_atn (parser) == NULL);
    result = glr_parse (parser, "n", 1);
    assert (result.error == GLR_PARSE_SUCCESS);
    assert (glr_parser_set_atn (NULL, NULL, false) == -1);
  }

  /* Disabling restores plain forking without changing the outcome. */
  assert (glr_parser_set_adaptive_lookahead (parser, false) == 0);
  result = glr_parse (parser, "n+n*n", 5);
  assert (result.error == GLR_PARSE_SUCCESS);
  glr_parser_destroy (parser);
  glr_grammar_destroy (g);
}

static void
test_lookahead_hook (void)
{
  glr_grammar_t *g = make_predict_grammar ();
  glr_parser_t *parser;
  glr_parse_result_t result;
  struct glr_disambig_hook *hook;
  assert (g != NULL);
  parser = glr_parser_create (g);
  assert (parser != NULL);
  hook = glr_atn_lookahead_hook_create ("atn-test", 10, g, 4);
  assert (hook != NULL);
  assert (glr_parser_add_disambiguator (parser, hook) == 0);
  result = glr_parse (parser, "ab", 2);
  assert (result.error == GLR_PARSE_SUCCESS);
  assert (glr_atn_lookahead_hook_create ("x", 1, NULL, 4) == NULL);
  glr_parser_destroy (parser);
  glr_grammar_destroy (g);
}

int
main (void)
{
  test_production_starts ();
  test_prefix_viability ();
  test_follow_sets ();
  test_predict_production ();
  test_scan_window ();
  test_scan_window_ambiguity ();
  test_lookahead_filter ();
  test_parser_adaptive_pipeline ();
  test_lookahead_hook ();
  puts ("ATN lookahead tests passed");
  return 0;
}
