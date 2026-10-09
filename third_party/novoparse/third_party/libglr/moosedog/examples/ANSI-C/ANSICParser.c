/*
 * ANSICParser.c -- demonstration driver for the ANSI-C Moosedog example.
 *
 * Moosedog itself (the .grm frontend that would emit Moosedog.h,
 * ANSI-C.ast.h, ANSI-C.lexer.h and ANSI-C.parser.h) is not implemented
 * yet, so this file demonstrates the runtime machinery directly against
 * libglr: it builds a small ANSI-C-shaped grammar in code (declarations,
 * expression statements and the dangling-else ambiguity), compiles its
 * ATN, runs lookahead prediction, and parses with adaptive lookahead
 * enabled. Unambiguous forests are projected to owned ASTs and printed.
 * This small grammar is not a generated implementation of ANSI-C.grm.
 *
 * Build (from the libglr source root, after cmake --build build):
 *
 *   gcc -std=c11 -Wall -Wextra -o ansic_demo \
 *       moosedog/examples/ANSI-C/ANSICParser.c \
 *       -Iinclude build/libglr.a build/libsfsexp.a -pthread
 *
 * Run: ./ansic_demo
 */
#include <glr/glr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Terminal spellings double as literal terminal names. */
#define T_IF "if"
#define T_ELSE "else"
#define T_LP "("
#define T_RP ")"
#define T_INT "int"
#define T_ID "a"
#define T_ASSIGN "="
#define T_NUM "3"
#define T_SEMI ";"

static int
add_terminal (glr_grammar_t *grammar, const char *name)
{
  return glr_grammar_add_symbol (grammar, GLR_SYMBOL_TERMINAL, name);
}

static int
add_nonterminal (glr_grammar_t *grammar, const char *name)
{
  return glr_grammar_add_symbol (grammar, GLR_SYMBOL_NONTERMINAL, name);
}

/* Build: S -> Stmt; Stmt -> Decl | Expr ; | if ( Expr ) Stmt |
 *        if ( Expr ) Stmt else Stmt; Decl -> int a ; | int a = 3 ;;
 *        Expr -> a = Expr | a | 3. */
static glr_grammar_t *
build_demo_grammar (void)
{
  glr_grammar_t *grammar = glr_grammar_create ();
  int s, stmt, decl, expr;
  int t_if, t_else, t_lp, t_rp, t_int, t_id, t_assign, t_num, t_semi;
  glr_symbol_t *body[7];

  if (grammar == NULL)
    return NULL;

  s = add_nonterminal (grammar, "S");
  stmt = add_nonterminal (grammar, "Stmt");
  decl = add_nonterminal (grammar, "Decl");
  expr = add_nonterminal (grammar, "Expr");
  t_if = add_terminal (grammar, T_IF);
  t_else = add_terminal (grammar, T_ELSE);
  t_lp = add_terminal (grammar, T_LP);
  t_rp = add_terminal (grammar, T_RP);
  t_int = add_terminal (grammar, T_INT);
  t_id = add_terminal (grammar, T_ID);
  t_assign = add_terminal (grammar, T_ASSIGN);
  t_num = add_terminal (grammar, T_NUM);
  t_semi = add_terminal (grammar, T_SEMI);
  if (s < 0 || stmt < 0 || decl < 0 || expr < 0 || t_if < 0 || t_else < 0
      || t_lp < 0 || t_rp < 0 || t_int < 0 || t_id < 0 || t_assign < 0
      || t_num < 0 || t_semi < 0)
    goto fail;

#define SYM(id) glr_grammar_get_symbol (grammar, (id))
#define PROD(head, n)                                                     \
  do                                                                      \
    {                                                                     \
      if (glr_grammar_add_production (grammar, (head), body, (n)) < 0)     \
        goto fail;                                                        \
    }                                                                     \
  while (0)

  body[0] = SYM (stmt);
  PROD (s, 1);

  body[0] = SYM (decl);
  PROD (stmt, 1);
  body[0] = SYM (expr);
  body[1] = SYM (t_semi);
  PROD (stmt, 2);
  body[0] = SYM (t_if);
  body[1] = SYM (t_lp);
  body[2] = SYM (expr);
  body[3] = SYM (t_rp);
  body[4] = SYM (stmt);
  PROD (stmt, 5);
  body[5] = SYM (t_else);
  body[6] = SYM (stmt);
  PROD (stmt, 7);

  body[0] = SYM (t_int);
  body[1] = SYM (t_id);
  body[2] = SYM (t_semi);
  PROD (decl, 3);
  body[2] = SYM (t_assign);
  body[3] = SYM (t_num);
  body[4] = SYM (t_semi);
  PROD (decl, 5);

  body[0] = SYM (t_id);
  body[1] = SYM (t_assign);
  body[2] = SYM (expr);
  PROD (expr, 3);
  body[0] = SYM (t_id);
  PROD (expr, 1);
  body[0] = SYM (t_num);
  PROD (expr, 1);
#undef SYM
#undef PROD

  if (glr_grammar_set_start_symbol (grammar, s) != 0)
    goto fail;
  return grammar;

fail:
  glr_grammar_destroy (grammar);
  return NULL;
}

/* AST projection for this demo's production numbering. Each mask selects
 * semantic children in source order; delimiters and assignment punctuation
 * are omitted. The full .grm supplies actions for all C11 constructs.
 */
static const struct
{
  const char *kind;
  unsigned int children;
} ast_rules[] = {
  { NULL, 1 },                       /* S -> Stmt: forward */
  { NULL, 1 },                       /* Stmt -> Decl: forward */
  { "expression-statement", 1 },
  { "if", (1u << 2) | (1u << 4) },
  { "if-else", (1u << 2) | (1u << 4) | (1u << 6) },
  { "declaration", (1u << 0) | (1u << 1) },
  { "initialized-declaration", (1u << 0) | (1u << 1) | (1u << 3) },
  { "assignment", (1u << 0) | (1u << 2) },
  { "identifier", 1 },
  { "integer", 1 }
};

static char *
copy_slice (const char *text, size_t length)
{
  char *copy = malloc (length + 1);
  if (copy != NULL)
    {
      memcpy (copy, text, length);
      copy[length] = '\0';
    }
  return copy;
}

/* Used only for partial trees, before ownership transfers to glr_ast_t. */
static void
destroy_ast_node (glr_ast_node_t *node)
{
  if (node == NULL)
    return;
  for (size_t i = 0; i < node->child_count; ++i)
    destroy_ast_node (node->children[i]);
  free (node->name);
  free (node->value);
  free (node->children);
  free (node);
}

static glr_ast_node_t *
project_ast (const glr_forest_node_t *forest, const char *input)
{
  glr_ast_node_t *node;
  if (forest->type == GLR_NODE_NONTERMINAL)
    {
      /* Never silently pick the first packed alternative. */
      if (forest->child_count != 1)
        return NULL;
      return project_ast (forest->children[0], input);
    }
  if (forest->type == GLR_NODE_CONSTRUCTOR)
    {
      if (forest->symbol_id < 0
          || (size_t) forest->symbol_id >= sizeof ast_rules / sizeof ast_rules[0])
        return NULL;
      if (ast_rules[forest->symbol_id].kind == NULL)
        return project_ast (forest->children[0], input);
    }
  node = calloc (1, sizeof (*node));
  if (node == NULL)
    return NULL;
  node->start = forest->position;
  node->end = forest->end_position;
  if (forest->type == GLR_NODE_TERMINAL)
    {
      node->value = copy_slice (input + node->start, node->end - node->start);
      if (node->value == NULL)
        goto fail;
      return node;
    }
  const char *kind = ast_rules[forest->symbol_id].kind;
  node->name = copy_slice (kind, strlen (kind));
  node->capacity = forest->child_count;
  node->children = calloc (node->capacity, sizeof (*node->children));
  if (node->name == NULL || node->children == NULL)
    goto fail;
  for (size_t i = 0; i < forest->child_count; ++i)
    if (ast_rules[forest->symbol_id].children & (1u << i))
      {
        glr_ast_node_t *child = project_ast (forest->children[i], input);
        if (child == NULL)
          goto fail;
        node->children[node->child_count++] = child;
      }
  return node;
fail:
  destroy_ast_node (node);
  return NULL;
}

static int
print_ast (const glr_forest_t *forest, const char *input)
{
  if (glr_forest_is_ambiguous (forest->root))
    {
      puts ("AST deferred: resolve dangling else before selecting a tree.");
      return 0;
    }
  glr_ast_t *ast = glr_ast_create ();
  glr_ast_node_t *root = project_ast (forest->root, input);
  char *text = NULL;
  size_t length = 0;
  if (ast == NULL || root == NULL || glr_ast_set_root (ast, root) != 0)
    {
      destroy_ast_node (root);
      glr_ast_destroy (ast);
      return -1;
    }
  int status = glr_ast_to_sexp (glr_ast_root (ast), &text, &length);
  if (status == 0)
    {
      fputs ("AST: ", stdout);
      fwrite (text, 1, length, stdout);
      if (length == 0 || text[length - 1] != '\n')
        putchar ('\n');
    }
  free (text);
  glr_ast_destroy (ast);
  return status;
}

static int
expect_parse (glr_parser_t *parser, const char *input, const char *label)
{
  glr_parse_result_t result = glr_parse (parser, input, strlen (input));
  if (result.error != GLR_PARSE_SUCCESS)
    {
      fprintf (stderr, "FAIL: could not parse %s at byte %zu\n", label,
               result.position);
      return -1;
    }
  printf ("parsed %-28s forest nodes: %zu\n", label,
          glr_forest_total_nodes (result.forest));
  if (print_ast (result.forest, input) != 0)
    {
      fprintf (stderr, "FAIL: AST projection failed for %s\n", label);
      return -1;
    }
  return 0;
}

int
main (void)
{
  glr_grammar_t *grammar = build_demo_grammar ();
  glr_parser_t *parser;
  glr_parse_table_t *table;
  glr_atn_t *atn;
  glr_parser_atn_stats_t stats;
  char error[256] = { 0 };
  int stmt, decl;
  int window[4];
  int winner = -1;
  int rc = 1;

  if (grammar == NULL)
    {
      fprintf (stderr, "FAIL: grammar construction failed\n");
      return 1;
    }

  parser = glr_parser_create (grammar);
  if (parser == NULL)
    {
      fprintf (stderr, "FAIL: parser construction failed\n");
      glr_grammar_destroy (grammar);
      return 1;
    }
  /* Whitespace between the literal terminals is insignificant. */
  if (glr_parser_set_trivia (parser, " ") != 0)
    goto done;

  /* Parse-table conflicts are what make this grammar need GLR (and what
     adaptive lookahead trims): report them up front. */
  table = glr_grammar_build_parse_table (grammar, error, sizeof (error));
  if (table == NULL)
    {
      fprintf (stderr, "FAIL: table build failed: %s\n", error);
      goto done;
    }
  printf ("parse table: %zu states, %zu conflict cells\n",
          table->state_count, glr_parse_table_conflict_count (table));
  if (glr_grammar_set_parse_table (grammar, table, true) != 0)
    {
      glr_parse_table_destroy (table);
      goto done;
    }

  /* The grammar ATN: one rule entry per nonterminal, one accepting path
     per production (see docs/vademecum chapter 14). */
  atn = glr_atn_from_grammar (grammar);
  if (atn == NULL)
    {
      fprintf (stderr, "FAIL: ATN compilation failed\n");
      goto done;
    }
  printf ("ATN: %zu states\n", glr_atn_state_count (atn));
  stmt = glr_grammar_find_symbol (grammar, "Stmt", GLR_SYMBOL_NONTERMINAL);
  decl = glr_grammar_find_symbol (grammar, "Decl", GLR_SYMBOL_NONTERMINAL);
  printf ("rule entry: Stmt=%u Decl=%u\n", glr_atn_rule_start (atn, stmt),
          glr_atn_rule_start (atn, decl));
  glr_atn_destroy (atn);

  /* Adaptive prediction over Stmt: "int ..." can only open a
     declaration, "a ..." can only open an expression statement. */
  window[0] = glr_grammar_find_symbol (grammar, T_INT, GLR_SYMBOL_TERMINAL);
  window[1] = glr_grammar_find_symbol (grammar, T_ID, GLR_SYMBOL_TERMINAL);
  if (glr_parser_require_atn (parser) == NULL
      || glr_atn_predict_production (glr_parser_get_atn (parser), grammar,
                                     stmt, window, 2, &winner)
             != 1)
    {
      fprintf (stderr, "FAIL: ATN prediction failed\n");
      goto done;
    }
  printf ("predict(Stmt, [int a]) -> production %d\n", winner);
  window[0] = glr_grammar_find_symbol (grammar, T_ID, GLR_SYMBOL_TERMINAL);
  window[1] = glr_grammar_find_symbol (grammar, T_ASSIGN, GLR_SYMBOL_TERMINAL);
  winner = -1;
  if (glr_atn_predict_production (glr_parser_get_atn (parser), grammar, stmt,
                                  window, 2, &winner)
      != 1)
    {
      fprintf (stderr, "FAIL: ATN prediction failed\n");
      goto done;
    }
  printf ("predict(Stmt, [a =])   -> production %d\n", winner);

  /* Adaptive lookahead: filter conflict cells through the ATN pipeline
     (FOLLOW + bounded deterministic scan, docs/vademecum chapter 15). */
  if (glr_parser_set_adaptive_lookahead (parser, true) != 0
      || glr_parser_set_adaptive_lookahead_depth (parser, 8) != 0)
    {
      fprintf (stderr, "FAIL: enabling adaptive lookahead failed\n");
      goto done;
    }
  if (expect_parse (parser, "int a = 3 ;", "\"int a = 3 ;\"") != 0
      || expect_parse (parser, "a = 3 ;", "\"a = 3 ;\"") != 0
      || expect_parse (parser, "if ( a ) if ( a ) a = 3 ; else a = 3 ;",
                       "dangling-else") != 0)
    goto done;
  if (glr_parser_get_adaptive_stats (parser, &stats) != 0)
    {
      fprintf (stderr, "FAIL: reading adaptive stats failed\n");
      goto done;
    }
  printf ("adaptive lookahead: seen=%llu decided=%llu pruned=%llu\n",
          stats.conflicts_seen, stats.conflicts_decided,
          stats.actions_pruned);

  /* Disabling the filter must accept the same language. */
  glr_parser_set_adaptive_lookahead (parser, false);
  if (expect_parse (parser, "if ( a ) if ( a ) a = 3 ; else a = 3 ;",
                    "dangling-else, plain GLR") != 0)
    goto done;

  printf ("ANSI-C ATN demo passed\n");
  rc = 0;

done:
  glr_parser_destroy (parser);
  glr_grammar_destroy (grammar);
  return rc;
}
