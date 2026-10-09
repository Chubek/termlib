#include <glr/disambstd.h>
#include <glr/parser.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Keep assertions enabled in Release builds too. */
#ifdef NDEBUG
#undef NDEBUG
#undef assert
#include <assert.h>
#endif

static bool accept (const glr_disambig_context_t *c,
                    const glr_disambig_candidate_t *a, void *data)
{ (void)c; (void)data; return a->precedence > 0; }
static int rank (const glr_disambig_context_t *c,
                 const glr_disambig_candidate_t *a, void *data)
{ (void)c; (void)data; return a->precedence; }
static const char *text (const glr_disambig_context_t *c,
                        const glr_disambig_candidate_t *a, void *data)
{ (void)c; (void)data; return a->user_data; }
static bool attribute (const glr_disambig_context_t *c, const glr_forest_node_t *n,
    const glr_disambig_candidate_t *a, const double *children, size_t count,
    double *value, void *data)
{
  (void)c; (void)a; (void)data;
  *value = n->symbol_id;
  for (size_t i = 0; i < count; ++i) *value += children[i];
  return *value >= 0;
}
static bool layout (const glr_disambig_context_t *c,
    const glr_disambig_candidate_t *a, glr_disambstd_layout_t *out, void *data)
{
  static const glr_disambstd_constraint_t valid[] = {{1, 0, 4}, {0, 1, -4}};
  static const glr_disambstd_constraint_t invalid[] = {{1, 0, 3}, {0, 1, -4}};
  (void)c; (void)data;
  *out = (glr_disambstd_layout_t){2, a->precedence > 0 ? valid : invalid, 2};
  return true;
}
static bool derivation (const glr_disambstd_derivation_t *d,
                        glr_disambstd_layout_t *out, void *data)
{
  (void)d; (void)out; (void)data; return true;
}
static bool stop_refining (const glr_disambstd_derivation_t *a,
                           const glr_disambstd_derivation_t *b, void *data)
{ (void)a; (void)b; (void)data; return false; }

static bool invalid_layout (const glr_disambig_context_t *c,
    const glr_disambig_candidate_t *a, glr_disambstd_layout_t *out, void *data)
{
  static const glr_disambstd_constraint_t invalid = {2, 0, 0};
  (void)c; (void)a; (void)data;
  *out = (glr_disambstd_layout_t){2, &invalid, 1};
  return true;
}
static double counted_score (const glr_disambig_context_t *c,
    const glr_forest_node_t *n, const glr_disambig_candidate_t *a, void *data)
{
  (void)c; (void)n; (void)a;
  ++*(int *)data;
  return 0.5;
}

static void scoring (void)
{
  glr_forest_node_t leaf = {0};
  glr_forest_node_t *children[] = {&leaf, &leaf};
  glr_forest_node_t root = {.children = children, .child_count = 2};
  glr_disambig_candidate_t a[2] = {{.node = &root}, {.score = 2}};
  glr_disambig_context_t c = {.candidates = a, .candidate_count = 2};
  glr_disambstd_options_t o; glr_disambstd_options_init (&o);
  int calls = 0; size_t winner = SIZE_MAX;
  o.score = counted_score; o.user_data = &calls;
  glr_disambig_hook_t *h = glr_disambstd_create (GLR_DISAMBSTD_DYNAMIC_PROGRAMMING, &o);
  assert (h->fn (&c, &winner, h->user_data) == GLR_DISAMBIG_RESOLVED);
  assert (winner == 0 && calls == 2); /* shared leaf evaluated once */
  children[0] = &root;
  assert (h->fn (&c, NULL, h->user_data) == GLR_DISAMBIG_ERROR);
  glr_disambig_hook_destroy (h);
  o.score = NULL;
  h = glr_disambstd_create (GLR_DISAMBSTD_PROBABILITY, &o);
  a[0] = (glr_disambig_candidate_t){.probability = 0};
  a[1] = (glr_disambig_candidate_t){.probability = 0.25};
  assert (h->fn (&c, &winner, h->user_data) == GLR_DISAMBIG_RESOLVED);
  assert (winner == 1);
  a[1].probability = NAN;
  assert (h->fn (&c, NULL, h->user_data) == GLR_DISAMBIG_ERROR);
  glr_disambig_hook_destroy (h);
  /* Products far below DBL_MIN must still be distinguishable. */
  glr_forest_node_t chain[600] = {0};
  glr_forest_node_t *links[599];
  for (size_t i = 0; i < 599; ++i)
    { links[i] = &chain[i + 1]; chain[i].children = &links[i]; chain[i].child_count = 1; }
  o.score = counted_score;
  a[0] = (glr_disambig_candidate_t){.node = chain, .probability = 1e-200};
  a[1] = (glr_disambig_candidate_t){.node = chain, .probability = 2e-200};
  h = glr_disambstd_create (GLR_DISAMBSTD_PROBABILITY, &o);
  assert (h->fn (&c, &winner, h->user_data) == GLR_DISAMBIG_RESOLVED);
  assert (winner == 1);
  glr_disambig_hook_destroy (h);
}

static void runtime (void)
{
  glr_disambstd_options_t o;
  glr_disambstd_options_init (&o);
  o.rank = rank; o.predicate = accept; o.text = text;
  o.expected_text = "Name"; o.attribute = attribute; o.layout = layout;
  o.derivation_filter = derivation; o.refine = stop_refining;
  for (int k = 0; k < GLR_DISAMBSTD_COUNT; ++k)
    {
      glr_disambig_hook_t *h = glr_disambstd_create_named (glr_disambstd_name (k), &o);
      assert (h); glr_disambig_hook_destroy (h);
    }
  assert (!glr_disambstd_create_named ("unknown", &o));
  assert (!glr_disambstd_create (GLR_DISAMBSTD_COUNT, &o));
  assert (!glr_disambstd_create (GLR_DISAMBSTD_ATTRIBUTE_GRAMMAR, NULL));

  const glr_disambstd_kind_t kinds[] = {
    GLR_DISAMBSTD_LEXICAL, GLR_DISAMBSTD_ISLAND_PARSING,
    GLR_DISAMBSTD_NAME_RESOLUTION, GLR_DISAMBSTD_POST_PARSE_FILTERING,
    GLR_DISAMBSTD_SCANNERLESS_PRIORITIES, GLR_DISAMBSTD_PREFER_AVOID,
    GLR_DISAMBSTD_INDENTATION_AND_LAYOUT, GLR_DISAMBSTD_LAYOUT_SENSITIVE,
    GLR_DISAMBSTD_CASE_SENSITIVITY, GLR_DISAMBSTD_LONGEST_MATCH,
    GLR_DISAMBSTD_ATTRIBUTE_GRAMMAR
  };
  glr_forest_node_t bad = {.symbol_id = -1}, good = {.symbol_id = 1};
  for (size_t i = 0; i < sizeof (kinds) / sizeof (*kinds); ++i)
    {
      glr_disambig_candidate_t a[3] = {
        {.precedence = -1, .end_position = 1, .user_data = "name", .node = &bad},
        {.precedence = 1, .end_position = 3, .user_data = "Name", .node = &good},
        {.precedence = 100, .end_position = 100, .rejected = true}
      };
      glr_disambig_context_t c = {.candidates = a, .candidate_count = 3};
      glr_disambig_hook_t *h = glr_disambstd_create (kinds[i], &o);
      size_t winner = SIZE_MAX;
      assert (h->fn (&c, &winner, h->user_data) == GLR_DISAMBIG_RESOLVED);
      assert (winner == 1 && a[0].rejected && a[2].rejected);
      glr_disambig_hook_destroy (h);
    }
  /* Ties remain available to subsequent hooks. */
  glr_disambig_candidate_t a[3] = {{.precedence = 2}, {.precedence = 2}, {.precedence = 1}};
  glr_disambig_context_t c = {.candidates = a, .candidate_count = 3};
  glr_disambig_hook_t *h = glr_disambstd_create (GLR_DISAMBSTD_PREFER_AVOID, &o);
  assert (h->fn (&c, NULL, h->user_data) == GLR_DISAMBIG_NO_MATCH);
  assert (!a[0].rejected && !a[1].rejected && a[2].rejected);
  glr_disambig_hook_destroy (h);
  /* All rejected is an error, not an arbitrary winner. */
  a[0].precedence = a[1].precedence = -1;
  h = glr_disambstd_create (GLR_DISAMBSTD_LAYOUT_SENSITIVE, &o);
  assert (h->fn (&c, NULL, h->user_data) == GLR_DISAMBIG_ERROR);
  glr_disambig_hook_destroy (h);
  /* Attribute synthesis follows child order and detects forest cycles. */
  glr_forest_node_t *children[] = {&good, &good};
  glr_forest_node_t parent = {.symbol_id = 1, .children = children, .child_count = 2};
  a[0] = (glr_disambig_candidate_t){.node = &parent}; c.candidate_count = 1;
  h = glr_disambstd_create (GLR_DISAMBSTD_ATTRIBUTE_GRAMMAR, &o);
  assert (h->fn (&c, NULL, h->user_data) == GLR_DISAMBIG_RESOLVED);
  assert (a[0].score == 3);
  children[0] = &parent;
  assert (h->fn (&c, NULL, h->user_data) == GLR_DISAMBIG_ERROR);
  glr_disambig_hook_destroy (h);
  o.layout = invalid_layout;
  a[0].rejected = false;
  h = glr_disambstd_create (GLR_DISAMBSTD_LAYOUT_SENSITIVE, &o);
  assert (h->fn (&c, NULL, h->user_data) == GLR_DISAMBIG_ERROR);
  glr_disambig_hook_destroy (h);
}

static glr_grammar_t *grammar (bool ambiguous, bool epsilon)
{
  glr_grammar_t *g = glr_grammar_create ();
  int s = glr_grammar_add_symbol (g, GLR_SYMBOL_NONTERMINAL, "S");
  int a = glr_grammar_add_symbol (g, GLR_SYMBOL_TERMINAL, "a");
  glr_symbol_t *body[] = {glr_grammar_get_symbol (g, a)};
  assert (glr_grammar_set_start_symbol (g, s) == 0);
  assert (glr_grammar_add_production (g, s, body, epsilon ? 0 : 1) >= 0);
  if (ambiguous) assert (glr_grammar_add_production (g, s, body, epsilon ? 0 : 1) >= 0);
  return g;
}
typedef struct { int blocked; bool separate; bool progress; } repair_t;
static bool constrain (const glr_disambstd_derivation_t *d,
                       glr_disambstd_layout_t *out, void *data)
{
  repair_t *r = data;
  static const glr_disambstd_constraint_t zero[] = {{1, 0, 0}, {0, 1, 0}};
  static const glr_disambstd_constraint_t one[] = {{1, 0, 1}, {0, 1, -1}};
  if (r->blocked == d->productions[0]) return false;
  if (r->separate)
    *out = (glr_disambstd_layout_t){2, d->productions[0] == 0 ? zero : one, 2};
  return true;
}
static bool refine (const glr_disambstd_derivation_t *a,
                    const glr_disambstd_derivation_t *b, void *data)
{
  repair_t *r = data; (void)b;
  if (r->progress) r->blocked = a->productions[0];
  return true;
}
static bool shared_layout (const glr_disambstd_derivation_t *d,
                           glr_disambstd_layout_t *out, void *data)
{
  static const glr_disambstd_constraint_t constraints[] = {{1, 0, 7}, {0, 1, -7}};
  (void)d; (void)data;
  *out = (glr_disambstd_layout_t){2, constraints, 2};
  return true;
}
static void analysis (void)
{
  glr_disambstd_options_t o; glr_disambstd_options_init (&o);
  glr_disambstd_report_t report = {0};
  glr_grammar_t *g = grammar (true, false);
  assert (glr_disambstd_check (GLR_DISAMBSTD_BOUNDED_SAT, g, &o, &report)
          == GLR_DISAMBSTD_CHECK_AMBIGUOUS);
  assert (report.token_count == 1 && report.tokens[0] == 1);
  assert (report.production_count[0] == 1 && report.production_count[1] == 1);
  assert (report.productions[0][0] != report.productions[1][0]);
  o.derivation_filter = shared_layout;
  assert (glr_disambstd_check (GLR_DISAMBSTD_BOUNDED_SMT, g, &o, &report)
          == GLR_DISAMBSTD_CHECK_AMBIGUOUS);
  assert (report.layout_variable_count == 2 && report.layout_values[0] == 0
          && report.layout_values[1] == 7);
  repair_t repair = {-1, true, true};
  o.user_data = &repair; o.derivation_filter = constrain;
  assert (glr_disambstd_check (GLR_DISAMBSTD_BOUNDED_SMT, g, &o, &report)
          == GLR_DISAMBSTD_CHECK_CLEAR);
  /* SAT deliberately ignores arithmetic; SMT requires one shared layout. */
  assert (glr_disambstd_check (GLR_DISAMBSTD_BOUNDED_SAT, g, &o, &report)
          == GLR_DISAMBSTD_CHECK_AMBIGUOUS);
  repair.separate = false; o.refine = refine;
  assert (glr_disambstd_check (GLR_DISAMBSTD_COUNTER_EXAMPLE_GUIDED, g, &o, &report)
          == GLR_DISAMBSTD_CHECK_CLEAR);
  assert (report.iterations == 2);
  repair.blocked = -1; repair.progress = false; o.max_iterations = 2;
  assert (glr_disambstd_check (GLR_DISAMBSTD_COUNTER_EXAMPLE_GUIDED, g, &o, &report)
          == GLR_DISAMBSTD_CHECK_LIMIT);
  glr_disambstd_options_init (&o); o.max_derivations = 1;
  assert (glr_disambstd_check (GLR_DISAMBSTD_BOUNDED_SAT, g, &o, &report)
          == GLR_DISAMBSTD_CHECK_LIMIT);
  glr_grammar_destroy (g);
  glr_disambstd_options_init (&o);
  g = grammar (true, true); o.max_tokens = 0;
  assert (glr_disambstd_check (GLR_DISAMBSTD_BOUNDED_SAT, g, &o, &report)
          == GLR_DISAMBSTD_CHECK_AMBIGUOUS);
  assert (report.token_count == 0);
  glr_grammar_destroy (g);
  g = grammar (false, false);
  assert (glr_disambstd_check (GLR_DISAMBSTD_BOUNDED_SAT, g, NULL, &report)
          == GLR_DISAMBSTD_CHECK_CLEAR);
  /* Recursive grammar S -> S S | a: two trees for three tokens. */
  glr_symbol_t *body[] = {g->start_symbol, g->start_symbol};
  assert (glr_grammar_add_production (g, g->start_symbol->id, body, 2) >= 0);
  glr_disambstd_options_init (&o); o.max_depth = 3; o.max_tokens = 3;
  assert (glr_disambstd_check (GLR_DISAMBSTD_BOUNDED_SAT, g, &o, &report)
          == GLR_DISAMBSTD_CHECK_AMBIGUOUS);
  assert (report.token_count == 3);
  o.max_tokens = 2;
  assert (glr_disambstd_check (GLR_DISAMBSTD_BOUNDED_SAT, g, &o, &report)
          == GLR_DISAMBSTD_CHECK_CLEAR);
  glr_grammar_destroy (g);
  glr_disambstd_report_clear (&report);
  glr_disambstd_report_clear (&report);
}

static void integration (void)
{
  glr_grammar_t *g = grammar (false, false);
  glr_parser_t *p = glr_parser_create (g);
  glr_disambstd_options_t o; glr_disambstd_options_init (&o);
  o.priority = 10;
  assert (!glr_parser_add_disambiguator (p, glr_disambstd_create_named ("prefer-avoid", &o)));
  o.priority = 0;
  assert (!glr_parser_add_disambiguator (p, glr_disambstd_create_named ("longest-match", &o)));
  glr_disambig_candidate_t a[] = {
    {.precedence = 1, .end_position = 1}, {.precedence = 1, .end_position = 2},
    {.precedence = -1, .end_position = 10}
  };
  glr_disambig_context_t c = {.candidates = a, .candidate_count = 3};
  size_t winner = SIZE_MAX;
  assert (glr_parser_run_disambiguators (p, &c, &winner) == GLR_DISAMBIG_RESOLVED);
  assert (winner == 1);
  glr_parser_destroy (p); glr_grammar_destroy (g);
}

int main (void)
{
  runtime (); scoring (); analysis (); integration ();
  puts ("disambstd: all tests passed");
  return 0;
}
