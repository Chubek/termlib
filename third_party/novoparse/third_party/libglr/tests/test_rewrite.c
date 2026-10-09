#include <glr/rewrite.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg)                                                     \
  do                                                                          \
    {                                                                         \
      if (cond)                                                               \
        {                                                                     \
          tests_passed++;                                                     \
        }                                                                     \
      else                                                                    \
        {                                                                     \
          printf ("FAILED: %s\n", msg);                                      \
          tests_failed++;                                                     \
        }                                                                     \
    }                                                                         \
  while (0)

static glr_grammar_t *
build_sample_grammar (void)
{
  glr_grammar_t *grammar = glr_grammar_create ();
  int s;
  int a;
  int b;
  int c;
  int ta;
  int tb;
  glr_symbol_t *body[3];

  if (grammar == NULL)
    {
      return NULL;
    }

  s = glr_grammar_add_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "S");
  a = glr_grammar_add_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "A");
  b = glr_grammar_add_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "B");
  c = glr_grammar_add_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "C");
  ta = glr_grammar_add_symbol (grammar, GLR_SYMBOL_TERMINAL, "a");
  tb = glr_grammar_add_symbol (grammar, GLR_SYMBOL_TERMINAL, "b");

  glr_grammar_set_start_symbol (grammar, s);

  body[0] = glr_grammar_get_symbol (grammar, a);
  glr_grammar_add_production (grammar, s, body, 1);

  body[0] = glr_grammar_get_symbol (grammar, a);
  body[1] = glr_grammar_get_symbol (grammar, ta);
  glr_grammar_add_production (grammar, a, body, 2);
  body[0] = glr_grammar_get_symbol (grammar, b);
  glr_grammar_add_production (grammar, a, body, 1);
  glr_grammar_add_production (grammar, a, NULL, 0);

  body[0] = glr_grammar_get_symbol (grammar, tb);
  glr_grammar_add_production (grammar, b, body, 1);
  body[0] = glr_grammar_get_symbol (grammar, ta);
  glr_grammar_add_production (grammar, c, body, 1);

  return grammar;
}

static int
count_symbol_named (const glr_grammar_t *grammar, const char *name)
{
  size_t i;

  for (i = 0; i < grammar->symbol_count; i++)
    {
      if (strcmp (grammar->symbols[i]->name, name) == 0)
        {
          return 1;
        }
    }

  return 0;
}

static int
has_left_recursive_production (const glr_grammar_t *grammar)
{
  size_t i;

  for (i = 0; i < grammar->production_count; i++)
    {
      glr_production_t *production = grammar->productions[i];
      if (production->body_length > 0 && production->body[0] == production->head)
        {
          return 1;
        }
    }

  return 0;
}

static int
has_unit_production (const glr_grammar_t *grammar)
{
  size_t i;

  for (i = 0; i < grammar->production_count; i++)
    {
      glr_production_t *production = grammar->productions[i];
      if (production->body_length == 1
          && production->body[0]->type == GLR_SYMBOL_NONTERMINAL)
        {
          return 1;
        }
    }

  return 0;
}

static int
has_named_epsilon (const glr_grammar_t *grammar, const char *name)
{
  size_t i;

  for (i = 0; i < grammar->production_count; i++)
    {
      glr_production_t *production = grammar->productions[i];
      if (production->body_length == 0
          && strcmp (production->head->name, name) == 0)
        {
          return 1;
        }
    }

  return 0;
}

static void
test_procedural_pipeline (void)
{
  glr_grammar_t *grammar = build_sample_grammar ();

  printf ("Testing: procedural rewrite pipeline... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (glr_rewrite_make_lr_compatible (grammar) == GLR_REWRITE_STATUS_OK,
          "pipeline should succeed");
  ASSERT (!has_left_recursive_production (grammar),
          "left recursion should be removed");
  ASSERT (!has_unit_production (grammar), "unit productions should be removed");
  ASSERT (!has_named_epsilon (grammar, "A"),
          "original nullable production should be removed");
  ASSERT (!count_symbol_named (grammar, "C"),
          "unreachable symbol should be removed");
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_grl_parser (void)
{
  static const char source[]
      = "(rewrite (name rewrite-test) (rules (rename-symbol B Bee)"
        " (set-start S)))";
  char error[256];
  glr_rewrite_program_t *program;
  glr_grammar_t *grammar = build_sample_grammar ();

  printf ("Testing: GRL parser and executor... ");
  program = glr_rewrite_program_parse (source, sizeof (source) - 1, error,
                                       sizeof (error));
  ASSERT (program != NULL, error);
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (glr_rewrite_program_apply (grammar, program, NULL)
              == GLR_REWRITE_STATUS_OK,
          "program should execute");
  ASSERT (count_symbol_named (grammar, "Bee"), "rename-symbol should apply");
  ASSERT (!count_symbol_named (grammar, "B"), "old symbol name should be gone");
  glr_rewrite_program_destroy (program);
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

/* Helpers for the extended-library tests: build tiny grammars by name. */

static glr_grammar_t *
new_grammar_with_start (const char *start)
{
  glr_grammar_t *grammar = glr_grammar_create ();
  int id;

  if (grammar == NULL)
    {
      return NULL;
    }
  id = glr_grammar_add_symbol (grammar, GLR_SYMBOL_NONTERMINAL, start);
  if (id < 0 || glr_grammar_set_start_symbol (grammar, id) != 0)
    {
      glr_grammar_destroy (grammar);
      return NULL;
    }
  return grammar;
}

static int
add_named_symbol (glr_grammar_t *grammar, glr_symbol_type_t type,
                  const char *name)
{
  int id = glr_grammar_add_symbol (grammar, type, name);
  return id;
}

/* Add head -> body (body entries resolved by name, any type). */
static int
add_named_production (glr_grammar_t *grammar, const char *head,
                      const char *const *body, size_t body_length)
{
  int head_id = glr_grammar_find_symbol (grammar, head,
                                         GLR_SYMBOL_NONTERMINAL);
  glr_symbol_t *resolved[128];
  size_t i;

  if (head_id < 0 || body_length > 128)
    {
      return -1;
    }
  for (i = 0; i < body_length; i++)
    {
      int id = glr_grammar_find_symbol_any (grammar, body[i]);
      if (id < 0)
        {
          return -1;
        }
      resolved[i] = glr_grammar_get_symbol (grammar, id);
    }
  return glr_grammar_add_production (grammar, head_id, resolved, body_length);
}

static size_t
count_productions_of (const glr_grammar_t *grammar, const char *head)
{
  size_t count = 0;
  size_t i;

  for (i = 0; i < grammar->production_count; i++)
    {
      if (strcmp (grammar->productions[i]->head->name, head) == 0)
        {
          count++;
        }
    }
  return count;
}

static int
has_right_recursive_production (const glr_grammar_t *grammar)
{
  size_t i;

  for (i = 0; i < grammar->production_count; i++)
    {
      glr_production_t *production = grammar->productions[i];
      if (production->body_length > 0
          && production->body[production->body_length - 1] == production->head)
        {
          return 1;
        }
    }

  return 0;
}

static int
max_body_length (const glr_grammar_t *grammar)
{
  size_t max = 0;
  size_t i;

  for (i = 0; i < grammar->production_count; i++)
    {
      if (grammar->productions[i]->body_length > max)
        {
          max = grammar->productions[i]->body_length;
        }
    }
  return (int) max;
}

static int
has_terminal_in_long_body (const glr_grammar_t *grammar)
{
  size_t i;

  for (i = 0; i < grammar->production_count; i++)
    {
      glr_production_t *production = grammar->productions[i];
      if (production->body_length < 2)
        {
          continue;
        }
      for (size_t j = 0; j < production->body_length; j++)
        {
          if (production->body[j]->type == GLR_SYMBOL_TERMINAL)
            {
              return 1;
            }
        }
    }

  return 0;
}

/* Chomsky check: every production is A -> B C, A -> terminal, or
   start -> epsilon. */
static int
is_chomsky_normal_form (const glr_grammar_t *grammar)
{
  size_t i;

  if (grammar->start_symbol == NULL)
    {
      return 0;
    }
  for (i = 0; i < grammar->production_count; i++)
    {
      glr_production_t *production = grammar->productions[i];
      if (production->body_length == 0)
        {
          if (production->head != grammar->start_symbol)
            {
              return 0;
            }
        }
      else if (production->body_length == 1)
        {
          if (production->body[0]->type != GLR_SYMBOL_TERMINAL)
            {
              return 0;
            }
        }
      else if (production->body_length == 2)
        {
          if (production->body[0]->type != GLR_SYMBOL_NONTERMINAL
              || production->body[1]->type != GLR_SYMBOL_NONTERMINAL)
            {
              return 0;
            }
        }
      else
        {
          return 0;
        }
    }
  for (i = 0; i < grammar->production_count; i++)
    {
      glr_production_t *production = grammar->productions[i];
      for (size_t j = 0; j < production->body_length; j++)
        {
          if (production->body[j] == grammar->start_symbol)
            {
              return 0;
            }
        }
    }
  return 1;
}

static void
test_remove_duplicate_productions (void)
{
  glr_grammar_t *grammar = new_grammar_with_start ("S");
  const char *body[] = { "a" };

  printf ("Testing: remove-duplicate-productions... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "a") >= 0,
          "terminal should be added");
  ASSERT (add_named_production (grammar, "S", body, 1) >= 0,
          "first production should be added");
  ASSERT (add_named_production (grammar, "S", body, 1) >= 0,
          "duplicate production should be added");
  ASSERT (grammar->production_count == 2, "two productions should exist");
  ASSERT (glr_rewrite_remove_duplicate_productions (grammar)
              == GLR_REWRITE_STATUS_OK,
          "rewrite should succeed");
  ASSERT (grammar->production_count == 1, "duplicate should be removed");
  ASSERT (glr_rewrite_remove_duplicate_productions (NULL)
              == GLR_REWRITE_STATUS_INVALID_ARGUMENT,
          "NULL grammar should fail");
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_remove_self_unit_productions (void)
{
  glr_grammar_t *grammar = new_grammar_with_start ("S");
  const char *self[] = { "A" };
  const char *other[] = { "b" };
  const char *top[] = { "A" };

  printf ("Testing: remove-self-unit-productions... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "A") >= 0,
          "nonterminal should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "b") >= 0,
          "terminal should be added");
  ASSERT (add_named_production (grammar, "A", self, 1) >= 0,
          "self production should be added");
  ASSERT (add_named_production (grammar, "A", other, 1) >= 0,
          "other production should be added");
  ASSERT (add_named_production (grammar, "S", top, 1) >= 0,
          "top production should be added");
  ASSERT (glr_rewrite_remove_self_unit_productions (grammar)
              == GLR_REWRITE_STATUS_OK,
          "rewrite should succeed");
  ASSERT (count_productions_of (grammar, "A") == 1,
          "self production should be removed");
  ASSERT (count_productions_of (grammar, "S") == 1,
          "non-self unit should be kept");
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_eliminate_unreachable_symbols (void)
{
  glr_grammar_t *grammar = new_grammar_with_start ("S");
  const char *sa[] = { "a" };
  const char *cc[] = { "c" };

  printf ("Testing: eliminate-unreachable-symbols... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "a") >= 0,
          "terminal a should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "C") >= 0,
          "nonterminal C should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "c") >= 0,
          "terminal c should be added");
  ASSERT (add_named_production (grammar, "S", sa, 1) >= 0,
          "S production should be added");
  ASSERT (add_named_production (grammar, "C", cc, 1) >= 0,
          "C production should be added");
  ASSERT (glr_rewrite_remove_unreachable_symbols (grammar)
              == GLR_REWRITE_STATUS_OK,
          "rewrite should succeed");
  ASSERT (!count_symbol_named (grammar, "C"), "C should be removed");
  ASSERT (!count_symbol_named (grammar, "c"), "c should be removed");
  ASSERT (count_symbol_named (grammar, "S"), "S should be kept");
  ASSERT (count_symbol_named (grammar, "a"), "a should be kept");
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_eliminate_unproductive_symbols (void)
{
  glr_grammar_t *grammar = new_grammar_with_start ("S");
  const char *to_a[] = { "A" };
  const char *to_a_term[] = { "a" };
  const char *self[] = { "A" };

  printf ("Testing: eliminate-unproductive-symbols... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "A") >= 0,
          "nonterminal A should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "a") >= 0,
          "terminal a should be added");
  ASSERT (add_named_production (grammar, "S", to_a, 1) >= 0,
          "S -> A should be added");
  ASSERT (add_named_production (grammar, "S", to_a_term, 1) >= 0,
          "S -> a should be added");
  ASSERT (add_named_production (grammar, "A", self, 1) >= 0,
          "A -> A should be added");
  ASSERT (glr_rewrite_remove_unproductive_symbols (grammar)
              == GLR_REWRITE_STATUS_OK,
          "rewrite should succeed");
  ASSERT (!count_symbol_named (grammar, "A"), "A should be removed");
  ASSERT (count_productions_of (grammar, "S") == 1,
          "only S -> a should remain");
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_eliminate_unused_terminals (void)
{
  glr_grammar_t *grammar = new_grammar_with_start ("S");
  const char *sa[] = { "a" };

  printf ("Testing: eliminate-unused-terminals... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "a") >= 0,
          "terminal a should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "z") >= 0,
          "terminal z should be added");
  ASSERT (add_named_production (grammar, "S", sa, 1) >= 0,
          "S production should be added");
  ASSERT (glr_rewrite_remove_unused_terminals (grammar)
              == GLR_REWRITE_STATUS_OK,
          "rewrite should succeed");
  ASSERT (!count_symbol_named (grammar, "z"), "z should be removed");
  ASSERT (count_symbol_named (grammar, "a"), "a should be kept");
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_augment_start_symbol (void)
{
  glr_grammar_t *grammar = new_grammar_with_start ("S");
  const char *sa[] = { "a" };
  glr_symbol_t *old_start;

  printf ("Testing: augment-start-symbol... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "a") >= 0,
          "terminal a should be added");
  ASSERT (add_named_production (grammar, "S", sa, 1) >= 0,
          "S production should be added");
  old_start = grammar->start_symbol;
  ASSERT (glr_rewrite_augment_start_symbol (grammar)
              == GLR_REWRITE_STATUS_OK,
          "rewrite should succeed");
  ASSERT (grammar->start_symbol != old_start, "start should change");
  ASSERT (strcmp (grammar->start_symbol->name, "S__aug") == 0,
          "fresh start should be S__aug");
  ASSERT (count_productions_of (grammar, "S__aug") == 1,
          "augmenting production should exist");
  ASSERT (count_symbol_named (grammar, "S"), "old start should be kept");
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_isolate_terminals (void)
{
  glr_grammar_t *grammar = new_grammar_with_start ("S");
  const char *mixed[] = { "A", "a" };
  const char *ab[] = { "b" };

  printf ("Testing: isolate-terminals... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "A") >= 0,
          "nonterminal A should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "a") >= 0,
          "terminal a should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "b") >= 0,
          "terminal b should be added");
  ASSERT (add_named_production (grammar, "S", mixed, 2) >= 0,
          "mixed production should be added");
  ASSERT (add_named_production (grammar, "A", ab, 1) >= 0,
          "A production should be added");
  ASSERT (glr_rewrite_isolate_terminals (grammar) == GLR_REWRITE_STATUS_OK,
          "rewrite should succeed");
  ASSERT (!has_terminal_in_long_body (grammar),
          "no terminals should remain in long bodies");
  ASSERT (count_symbol_named (grammar, "a__tok"), "wrapper should exist");
  ASSERT (count_productions_of (grammar, "a__tok") == 1,
          "wrapper production should exist");
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_left_binarize (void)
{
  glr_grammar_t *grammar = new_grammar_with_start ("S");
  const char *body[] = { "a", "b", "c", "d" };

  printf ("Testing: left-binarize... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "a") >= 0,
          "terminal a should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "b") >= 0,
          "terminal b should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "c") >= 0,
          "terminal c should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "d") >= 0,
          "terminal d should be added");
  ASSERT (add_named_production (grammar, "S", body, 4) >= 0,
          "long production should be added");
  ASSERT (glr_rewrite_left_binarize (grammar) == GLR_REWRITE_STATUS_OK,
          "rewrite should succeed");
  ASSERT (max_body_length (grammar) <= 2, "all bodies should be binary");
  ASSERT (count_productions_of (grammar, "S") == 1,
          "S should keep one production");
  ASSERT (strcmp (grammar->productions[grammar->production_count - 1]->head->name,
                  "S")
              == 0,
          "last production should belong to S");
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_right_binarize (void)
{
  glr_grammar_t *grammar = new_grammar_with_start ("S");
  const char *body[] = { "a", "b", "c", "d" };

  printf ("Testing: right-binarize... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "a") >= 0,
          "terminal a should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "b") >= 0,
          "terminal b should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "c") >= 0,
          "terminal c should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "d") >= 0,
          "terminal d should be added");
  ASSERT (add_named_production (grammar, "S", body, 4) >= 0,
          "long production should be added");
  ASSERT (glr_rewrite_right_binarize (grammar) == GLR_REWRITE_STATUS_OK,
          "rewrite should succeed");
  ASSERT (max_body_length (grammar) <= 2, "all bodies should be binary");
  ASSERT (count_productions_of (grammar, "S") == 1,
          "S should keep one production");
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_chomsky_normal_form (void)
{
  glr_grammar_t *grammar = new_grammar_with_start ("S");
  const char *s_body[] = { "A", "B", "C" };
  const char *aa[] = { "a" };
  const char *bb[] = { "b" };
  const char *cc[] = { "c" };

  printf ("Testing: chomsky-normal-form... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "A") >= 0,
          "A should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "B") >= 0,
          "B should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "C") >= 0,
          "C should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "a") >= 0,
          "a should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "b") >= 0,
          "b should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "c") >= 0,
          "c should be added");
  ASSERT (add_named_production (grammar, "S", s_body, 3) >= 0,
          "S production should be added");
  ASSERT (add_named_production (grammar, "A", aa, 1) >= 0,
          "A production should be added");
  ASSERT (add_named_production (grammar, "B", bb, 1) >= 0,
          "B production should be added");
  ASSERT (add_named_production (grammar, "C", cc, 1) >= 0,
          "C production should be added");
  ASSERT (glr_rewrite_chomsky_normal_form (grammar) == GLR_REWRITE_STATUS_OK,
          "rewrite should succeed");
  ASSERT (is_chomsky_normal_form (grammar), "grammar should be in CNF");
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_right_factor (void)
{
  glr_grammar_t *grammar = new_grammar_with_start ("S");
  const char *first[] = { "p", "X" };
  const char *second[] = { "q", "X" };
  const char *top[] = { "A" };
  const char *xx[] = { "x" };

  printf ("Testing: right-factor... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "A") >= 0,
          "A should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "X") >= 0,
          "X should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "p") >= 0,
          "p should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "q") >= 0,
          "q should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "x") >= 0,
          "x should be added");
  ASSERT (add_named_production (grammar, "A", first, 2) >= 0,
          "first production should be added");
  ASSERT (add_named_production (grammar, "A", second, 2) >= 0,
          "second production should be added");
  ASSERT (add_named_production (grammar, "S", top, 1) >= 0,
          "top production should be added");
  ASSERT (add_named_production (grammar, "X", xx, 1) >= 0,
          "X production should be added");
  ASSERT (glr_rewrite_right_factor (grammar) == GLR_REWRITE_STATUS_OK,
          "rewrite should succeed");
  ASSERT (count_productions_of (grammar, "A") == 1,
          "A should keep one factored production");
  ASSERT (count_symbol_named (grammar, "A__rf0"), "helper should exist");
  ASSERT (count_productions_of (grammar, "A__rf0") == 2,
          "helper should carry both prefixes");
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_remove_right_recursion (void)
{
  glr_grammar_t *grammar = new_grammar_with_start ("A");
  const char *rec[] = { "b", "A" };
  const char *base[] = { "c" };

  printf ("Testing: remove-right-recursion... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "b") >= 0,
          "b should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "c") >= 0,
          "c should be added");
  ASSERT (add_named_production (grammar, "A", rec, 2) >= 0,
          "recursive production should be added");
  ASSERT (add_named_production (grammar, "A", base, 1) >= 0,
          "base production should be added");
  ASSERT (has_right_recursive_production (grammar),
          "grammar should start right-recursive");
  ASSERT (glr_rewrite_remove_right_recursion (grammar)
              == GLR_REWRITE_STATUS_OK,
          "rewrite should succeed");
  ASSERT (!has_right_recursive_production (grammar),
          "right recursion should be removed");
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_reverse_productions (void)
{
  glr_grammar_t *grammar = new_grammar_with_start ("S");
  const char *body[] = { "A", "a" };
  const char *ab[] = { "b" };
  glr_production_t *reversed = NULL;
  size_t i;

  printf ("Testing: reverse-productions... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "A") >= 0,
          "A should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "a") >= 0,
          "a should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "b") >= 0,
          "b should be added");
  ASSERT (add_named_production (grammar, "S", body, 2) >= 0,
          "S production should be added");
  ASSERT (add_named_production (grammar, "A", ab, 1) >= 0,
          "A production should be added");
  ASSERT (glr_rewrite_reverse_productions (grammar) == GLR_REWRITE_STATUS_OK,
          "rewrite should succeed");
  for (i = 0; i < grammar->production_count; i++)
    {
      if (strcmp (grammar->productions[i]->head->name, "S") == 0
          && grammar->productions[i]->body_length == 2)
        {
          reversed = grammar->productions[i];
        }
    }
  ASSERT (reversed != NULL, "reversed S production should exist");
  ASSERT (strcmp (reversed->body[0]->name, "a") == 0,
          "first symbol should now be a");
  ASSERT (strcmp (reversed->body[1]->name, "A") == 0,
          "second symbol should now be A");
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_inline_single_production_nonterminals (void)
{
  glr_grammar_t *grammar = new_grammar_with_start ("S");
  const char *use[] = { "A", "a" };
  const char *def[] = { "b" };

  printf ("Testing: inline-single-production-nonterminals... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "A") >= 0,
          "A should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "a") >= 0,
          "a should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "b") >= 0,
          "b should be added");
  ASSERT (add_named_production (grammar, "S", use, 2) >= 0,
          "use production should be added");
  ASSERT (add_named_production (grammar, "A", def, 1) >= 0,
          "definition should be added");
  ASSERT (glr_rewrite_inline_single_production_nonterminals (grammar)
              == GLR_REWRITE_STATUS_OK,
          "rewrite should succeed");
  ASSERT (!count_symbol_named (grammar, "A"), "A should be inlined away");
  ASSERT (count_productions_of (grammar, "S") == 1,
          "S should keep one production");
  ASSERT (strcmp (grammar->productions[0]->body[0]->name, "b") == 0,
          "S body should start with b");
  ASSERT (strcmp (grammar->productions[0]->body[1]->name, "a") == 0,
          "S body should end with a");
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_merge_equivalent_nonterminals (void)
{
  glr_grammar_t *grammar = new_grammar_with_start ("S");
  const char *to_a[] = { "A" };
  const char *to_b[] = { "B" };
  const char *aa[] = { "a" };
  const char *ba[] = { "a" };

  printf ("Testing: merge-equivalent-nonterminals... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "A") >= 0,
          "A should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_NONTERMINAL, "B") >= 0,
          "B should be added");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "a") >= 0,
          "a should be added");
  ASSERT (add_named_production (grammar, "S", to_a, 1) >= 0,
          "S -> A should be added");
  ASSERT (add_named_production (grammar, "S", to_b, 1) >= 0,
          "S -> B should be added");
  ASSERT (add_named_production (grammar, "A", aa, 1) >= 0,
          "A production should be added");
  ASSERT (add_named_production (grammar, "B", ba, 1) >= 0,
          "B production should be added");
  ASSERT (glr_rewrite_merge_equivalent_nonterminals (grammar)
              == GLR_REWRITE_STATUS_OK,
          "rewrite should succeed");
  ASSERT (!count_symbol_named (grammar, "B")
              || !count_symbol_named (grammar, "A"),
          "one equivalent symbol should be gone");
  ASSERT (count_symbol_named (grammar, "S"), "S should be kept");
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_grl_extended_library (void)
{
  static const char source[]
      = "(rewrite (name extended-smoke)"
        " (rules (remove-duplicate-productions)"
        " (remove-self-unit-productions)"
        " (eliminate-unreachable-symbols)"
        " (eliminate-unproductive-symbols)"
        " (eliminate-unused-terminals)"
        " (augment-start-symbol)"
        " (isolate-terminals)"
        " (left-binarize)"
        " (right-binarize)"
        " (right-factor)"
        " (remove-right-recursion)"
        " (reverse-productions)"
        " (inline-single-production-nonterminals)"
        " (merge-equivalent-nonterminals)"
        " (chomsky-normal-form)))";
  char error[256];
  glr_rewrite_program_t *program;
  glr_grammar_t *grammar = new_grammar_with_start ("S");
  const char *sa[] = { "a" };

  printf ("Testing: GRL extended-library smoke... ");
  ASSERT (grammar != NULL, "grammar should be created");
  ASSERT (add_named_symbol (grammar, GLR_SYMBOL_TERMINAL, "a") >= 0,
          "terminal should be added");
  ASSERT (add_named_production (grammar, "S", sa, 1) >= 0,
          "production should be added");
  program = glr_rewrite_program_parse (source, sizeof (source) - 1, error,
                                       sizeof (error));
  ASSERT (program != NULL, error);
  ASSERT (program->rule_count == 15, "all fifteen rules should compile");
  ASSERT (glr_rewrite_program_apply (grammar, program, NULL)
              == GLR_REWRITE_STATUS_OK,
          "extended pipeline should execute");
  glr_rewrite_program_destroy (program);
  glr_grammar_destroy (grammar);
  printf ("PASSED\n");
}

static void
test_grl_library_files (void)
{
  static const char *files[] = {
    "remove-duplicate-productions.grl",
    "remove-self-unit-productions.grl",
    "eliminate-unreachable-symbols.grl",
    "eliminate-unproductive-symbols.grl",
    "eliminate-unused-terminals.grl",
    "augment-start-symbol.grl",
    "isolate-terminals.grl",
    "left-binarize.grl",
    "right-binarize.grl",
    "chomsky-normal-form.grl",
    "right-factor.grl",
    "remove-right-recursion.grl",
    "reverse-productions.grl",
    "inline-single-production-nonterminals.grl",
    "merge-equivalent-nonterminals.grl",
  };
  static const char *roots[] = {
    "rewritelib",
    "../rewritelib",
    "../../rewritelib",
    "../libglr/rewritelib",
  };
  char path[1024];
  char error[256];
  size_t loaded = 0;
  size_t f;
  size_t r;

  printf ("Testing: rewritelib files load... ");
  for (f = 0; f < sizeof (files) / sizeof (files[0]); f++)
    {
      int found = 0;
      for (r = 0; r < sizeof (roots) / sizeof (roots[0]); r++)
        {
          glr_rewrite_program_t *program;
          snprintf (path, sizeof (path), "%s/%s", roots[r], files[f]);
          program = glr_rewrite_program_load_file (path, error,
                                                   sizeof (error));
          if (program != NULL)
            {
              ASSERT (program->rule_count == 1,
                      "library file should hold one rule");
              glr_rewrite_program_destroy (program);
              found = 1;
              loaded++;
              break;
            }
        }
      if (!found)
        {
          const char *env = getenv ("GLR_REWRITELIB_DIR");
          if (env != NULL)
            {
              glr_rewrite_program_t *program;
              snprintf (path, sizeof (path), "%s/%s", env, files[f]);
              program = glr_rewrite_program_load_file (path, error,
                                                       sizeof (error));
              if (program != NULL)
                {
                  glr_rewrite_program_destroy (program);
                  loaded++;
                }
            }
        }
    }
  printf ("loaded %zu/15 ", loaded);
  printf ("PASSED\n");
}

int
main (void)
{
  printf ("=== LibGLR Rewrite Tests ===\n\n");
  test_procedural_pipeline ();
  test_grl_parser ();
  test_remove_duplicate_productions ();
  test_remove_self_unit_productions ();
  test_eliminate_unreachable_symbols ();
  test_eliminate_unproductive_symbols ();
  test_eliminate_unused_terminals ();
  test_augment_start_symbol ();
  test_isolate_terminals ();
  test_left_binarize ();
  test_right_binarize ();
  test_chomsky_normal_form ();
  test_right_factor ();
  test_remove_right_recursion ();
  test_reverse_productions ();
  test_inline_single_production_nonterminals ();
  test_merge_equivalent_nonterminals ();
  test_grl_extended_library ();
  test_grl_library_files ();
  printf ("\n=== Results ===\nPassed: %d\nFailed: %d\n", tests_passed,
          tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
