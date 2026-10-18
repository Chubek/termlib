#include "termscript/stdlib/ts_stdlib.h"
#include "termscript/stdlib/common/ts_std_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TS_Error error;
#define CHECK(x) do { if (!(x)) { fprintf (stderr, "%d: %s (%s)\n", __LINE__, #x, error.message); exit (1); } } while (0)
static TS_Value str (const char *s) { TS_Value v = { .type = TS_STRING, .as.string = (char *)s }; return v; }
static TS_Value integer (int64_t i) { TS_Value v = { .type = TS_INT, .as.integer = i }; return v; }
static TS_Status call (const TS_Module *m, const char *name, TS_Value *r, size_t count, TS_Value *args)
{
  ts_value_free (r); ts_error_clear (&error);
  const TS_FuncDef *f = ts_module_find_func (m, name); CHECK (f);
  return f->func (NULL, f->userdata, args, count, r, &error);
}
#define PEG(f,r,n,a) call (&ts_std_peg_module,f,r,n,a)
#define GLR(f,r,n,a) call (&ts_std_grammar_module,f,r,n,a)
static TS_Value field (TS_Value *v, const char *path)
{
  ts_dom_t *d = ts_dom_unwrap (v, &error, "tree"); CHECK (d);
  const ts_dom_t *node = ts_dom_path (d, path); CHECK (node);
  TS_Value r = {0}; CHECK (ts_dom_to_value (node, &r, &error) == TS_OK); return r;
}
static void text_field (TS_Value *v, const char *path, const char *expected)
{
  TS_Value r = field (v, path);
  CHECK (r.type == TS_STRING && !strcmp (r.as.string, expected)); ts_value_free (&r);
}
static void int_field (TS_Value *v, const char *path, int64_t expected)
{
  TS_Value r = field (v, path); CHECK (r.type == TS_INT && r.as.integer == expected); ts_value_free (&r);
}
static void check_peg (const char *source, const char *input, bool matches)
{
  TS_Value grammar = {0}, r = {0}, s = str (source);
  CHECK (PEG ("compile", &grammar, 1, &s) == TS_OK);
  TS_Value a[] = { grammar, str (input) };
  CHECK (PEG ("test", &r, 2, a) == TS_OK && r.type == TS_BOOL && r.as.boolean == matches);
  CHECK (PEG ("parse", &r, 2, a) == (matches ? TS_OK : TS_ERR_PARSE));
  if (!matches) CHECK (error.offset <= strlen (input));
  ts_value_free (&r); ts_value_free (&grammar);
}
static void peg_tests (void)
{
  check_peg ("S <- 'a' / 'ab'", "ab", false); // ordered choice, not CFG alternation
  check_peg ("S <- 'ab' / 'a'", "ab", true);
  check_peg ("S <- ('a' 'b') / 'a'", "a", true);
  check_peg ("S <- &'a' !'b' [a-z]+ !.", "abc", true);
  check_peg ("S <- [^0-9]* [0-9]? !.", "hello5", true);
  check_peg ("S <- [^0-9]* [0-9]? !.", "hello56", false);
  check_peg ("S <- ('(' S ')') / 'x'", "(((x)))", true);
  check_peg ("S <- ('(' S ')') / 'x'", "((x)", false);
  check_peg ("S <- '' !.", "", true);
  check_peg ("S <-", "", true);
  check_peg ("# comment\nS <- '\\x41' '\\n' # tail\n", "A\n", true);
  check_peg ("S <- [\\]\\-\\^]+", "]-^", true);
  check_peg ("S <- A B !.; A <- 'a'; B <- 'b'", "ab", true);
  check_peg ("S <- 'a'* 'a'", "a", false); // repetition is possessive
  check_peg ("S <- '&' / '&&'", "&&", false);
  const char *bad[] = { "", "S = 'a'", "S <- Missing", "S <- 'x'\nS <- 'y'",
                        "S <- S 'x' / ''", "A <- B; B <- A / 'x'",
                        "S <- ('a'?)*", "S <- ('')+", "S <- (!'a')*",
                        "A <- B? A / 'x'; B <- ''", "S <- [z-a]", "S <- []",
                        "S <- '\\q'", "S <- '\\x0'", "S <- '\\x00'", "S <- [a",
                        "S <- ('x'", "S <- 'x", "S <- 'a' **", "S <- !S" };
  TS_Value r = {0};
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
    {
      TS_Value s = str (bad[i]); TS_Status st = PEG ("compile", &r, 1, &s);
      CHECK (st == TS_ERR_PARSE || st == TS_ERR_UNSUPPORTED);
      CHECK (r.type == TS_NIL && error.message[0] && error.offset <= strlen (bad[i]));
    }
  TS_Value grammar = {0}, source = str ("Root <- &A A !.; A <- 'ab'");
  CHECK (PEG ("compile", &grammar, 1, &source) == TS_OK);
  TS_Value a[] = { grammar, str ("ab"), integer (1) };
  CHECK (PEG ("parse", &r, 3, a) == TS_ERR_LIMIT);
  CHECK (PEG ("test", &r, 3, a) == TS_ERR_LIMIT); // not swallowed as mismatch
  CHECK (PEG ("parse", &r, 2, a) == TS_OK);
  text_field (&r, "name", "Root"); text_field (&r, "children.0.name", "A");
  int_field (&r, "children.0.start", 0); int_field (&r, "children.0.end", 2);
  ts_dom_t *d = ts_dom_unwrap (&r, &error, "tree");
  CHECK (ts_dom_len (ts_dom_path (d, "children")) == 1); // lookahead leaves no captures
  TS_Value saved = {0}; CHECK (ts_value_copy (&saved, &r) == TS_OK);
  a[1] = str ("ax"); CHECK (PEG ("parse", &r, 2, a) == TS_ERR_PARSE && error.offset == 1);
  TS_Value alias = {0}; CHECK (ts_value_copy (&alias, &grammar) == TS_OK);
  CHECK (PEG ("close", &r, 1, &grammar) == TS_OK);
  CHECK (PEG ("close", &r, 1, &alias) == TS_OK);
  CHECK (PEG ("test", &r, 2, a) == TS_ERR_INVAL);
  text_field (&saved, "children.0.text", "ab");
  ts_value_free (&saved); ts_value_free (&alias); ts_value_free (&grammar);
  source = str ("S <- &'abcdef' 'a'"); CHECK (PEG ("compile", &grammar, 1, &source) == TS_OK);
  a[0] = grammar; a[1] = str ("abcdef");
  CHECK (PEG ("parse", &r, 2, a) == TS_ERR_PARSE && error.offset == 1);
  ts_value_free (&grammar);
  source = str ("S <- Missing");
  CHECK (PEG ("compile", &grammar, 1, &source) == TS_ERR_PARSE && error.offset == 5);
  CHECK (strstr (error.message, "Missing"));
  source = str ("S <- ('x' S) / ''"); CHECK (PEG ("compile", &grammar, 1, &source) == TS_OK);
  char deep[300]; memset (deep, 'x', sizeof deep - 1); deep[sizeof deep - 1] = 0;
  a[0] = grammar; a[1] = str (deep);
  CHECK (PEG ("parse", &r, 2, a) == TS_ERR_LIMIT);
  a[1] = str ("x"); CHECK (PEG ("parse", &r, 2, a) == TS_OK); // usable after limit
  ts_value_free (&grammar);
  source = str ("S <- ('a' S 'b') / ('a' S 'c') / ''");
  CHECK (PEG ("compile", &grammar, 1, &source) == TS_OK);
  a[0] = grammar; a[1] = str ("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"); a[2] = integer (1000);
  CHECK (PEG ("parse", &r, 3, a) == TS_ERR_LIMIT);
  CHECK (PEG ("test", &r, 3, a) == TS_ERR_LIMIT);
  ts_value_free (&grammar);
  source = str ("S <- A*; A <- 'a'"); CHECK (PEG ("compile", &grammar, 1, &source) == TS_OK);
  char *many = malloc (65537); CHECK (many); memset (many, 'a', 65536); many[65536] = 0;
  a[0] = grammar; a[1] = str (many);
  CHECK (PEG ("test", &r, 2, a) == TS_OK && r.as.boolean);
  CHECK (PEG ("parse", &r, 2, a) == TS_ERR_LIMIT); // bounded captures, not unlimited DOM allocation
  a[2] = integer (0); CHECK (PEG ("test", &r, 3, a) == TS_ERR_LIMIT);
  free (many); ts_value_free (&grammar);
  // Compilation nesting and source-size limits.
  char groups[400]; strcpy (groups, "S <- ");
  memset (groups + 5, '(', 150); groups[155] = '\''; groups[156] = 'x'; groups[157] = '\'';
  memset (groups + 158, ')', 150); groups[308] = 0;
  source = str (groups); CHECK (PEG ("compile", &grammar, 1, &source) == TS_ERR_LIMIT);
  char *large = malloc (65538); CHECK (large); memset (large, 'x', 65537); large[65537] = 0;
  source = str (large); CHECK (PEG ("compile", &grammar, 1, &source) == TS_ERR_LIMIT); free (large);
  // Deterministic malformed-source corpus; compile/cleanup must not crash/hang.
  unsigned seed = 12345;
  static const char alphabet[] = "ab01 <-/'\"[]()!?*+&.#\\;\n";
  for (unsigned i = 0; i < 1000; i++)
    {
      char fuzz[100]; size_t len = i % (sizeof fuzz - 1);
      for (size_t j = 0; j < len; j++)
        { seed = seed * 1664525u + 1013904223u; fuzz[j] = alphabet[seed % (sizeof alphabet - 1)]; }
      fuzz[len] = 0; source = str (fuzz);
      TS_Status st = PEG ("compile", &grammar, 1, &source);
      CHECK (st == TS_OK || st == TS_ERR_PARSE || st == TS_ERR_LIMIT || st == TS_ERR_UNSUPPORTED);
      ts_value_free (&grammar);
    }
  ts_value_free (&r);
}
static void shell_peg_tests (void)
{
  FILE *file = fopen (TS_TEST_SHELL_EXAMPLE, "rb"); CHECK (file);
  CHECK (!fseek (file, 0, SEEK_END)); long size = ftell (file); CHECK (size > 0 && size < 1048576);
  rewind (file);
  char *text = malloc ((size_t)size + 1); CHECK (text);
  CHECK (fread (text, 1, (size_t)size, file) == (size_t)size); CHECK (!fclose (file)); text[size] = 0;
  const char *marker = "const GRAMMAR = peg.compile(r#\"";
  char *begin = strstr (text, marker); CHECK (begin); begin += strlen (marker);
  char *end = strstr (begin, "\"#)"); CHECK (end); *end = 0;
  TS_Value grammar = {0}, r = {0}, source = str (begin);
  CHECK (PEG ("compile", &grammar, 1, &source) == TS_OK);
  free (text); // the grammar must not borrow the source
  const char *valid[] = { "", "echo hello", "echo 'hi there' | cat", "echo $(echo hi)",
                          "x=word cmd 2>out && next;", "(echo hi; echo bye)", "echo \"$HOME\" # comment" };
  for (size_t i = 0; i < sizeof valid / sizeof *valid; i++)
    {
      TS_Value a[] = { grammar, str (valid[i]) };
      CHECK (PEG ("parse", &r, 2, a) == TS_OK);
    }
  TS_Value a[] = { grammar, str ("echo 'unterminated") };
  CHECK (PEG ("test", &r, 2, a) == TS_OK && !r.as.boolean);
  ts_value_free (&r); ts_value_free (&grammar);
}
static void add_symbol (TS_Value grammar, const char *kind, const char *name, const char *text)
{
  TS_Value a[] = { grammar, str (name), str (text ? text : "") }, r = {0};
  CHECK (GLR (kind, &r, text ? 3 : 2, a) == TS_OK); ts_value_free (&r);
}
static void add_rule (TS_Value grammar, const char *head, const char **symbols, size_t n, const char *label)
{
  ts_std_list_t *list = ts_std_list_new (); CHECK (list);
  for (size_t i = 0; i < n; i++) CHECK (ts_std_list_push_str (list, symbols[i]) == TS_OK);
  TS_Value body = {0}, r = {0}; CHECK (ts_std_list_wrap (&body, list, &error) == TS_OK);
  TS_Value a[] = { grammar, str (head), body, str (label ? label : "") };
  CHECK (GLR ("rule", &r, label ? 4 : 3, a) == TS_OK); ts_value_free (&r); ts_value_free (&body);
}
static void glr_tests (void)
{
  TS_Value grammar = {0}, r = {0}, start = str ("S");
  CHECK (GLR ("new", &grammar, 1, &start) == TS_OK);
  add_symbol (grammar, "nonterminal", "S", NULL);
  add_symbol (grammar, "literal", "a", "a");
  const char *pair[] = { "S", "S" }, *one[] = { "a" };
  add_rule (grammar, "S", pair, 2, "Pair"); add_rule (grammar, "S", one, 1, "Leaf");
  CHECK (GLR ("build", &r, 1, &grammar) == TS_OK);
  TS_Value a[] = { grammar, str ("a") };
  CHECK (GLR ("parse", &r, 2, a) == TS_OK); text_field (&r, "name", "Leaf");
  text_field (&r, "children.0.text", "a"); int_field (&r, "end", 1);
  TS_Value saved = {0}; CHECK (ts_value_copy (&saved, &r) == TS_OK);
  a[1] = str ("aaa"); CHECK (GLR ("parse", &r, 2, a) == TS_ERR_PARSE);
  CHECK (strstr (error.message, "ambiguous"));
  CHECK (GLR ("ambiguous", &r, 1, &grammar) == TS_OK && r.as.boolean);
  a[1] = str ("ab"); CHECK (GLR ("parse", &r, 2, a) == TS_ERR_PARSE && error.offset <= 2);
  CHECK (GLR ("build", &r, 1, &grammar) == TS_ERR_INVAL);
  CHECK (GLR ("close", &r, 1, &grammar) == TS_OK);
  CHECK (GLR ("close", &r, 1, &grammar) == TS_OK);
  CHECK (GLR ("parse", &r, 2, a) == TS_ERR_INVAL);
  text_field (&saved, "children.0.text", "a"); ts_value_free (&saved); ts_value_free (&grammar);
  CHECK (GLR ("new", &grammar, 1, &start) == TS_OK);
  add_symbol (grammar, "nonterminal", "S", NULL); add_symbol (grammar, "literal", "a", "a");
  add_rule (grammar, "S", pair, 2, "Pair"); add_rule (grammar, "S", one, 1, "Leaf");
  a[0] = grammar; a[1] = str ("first"); CHECK (GLR ("build", &r, 2, a) == TS_OK);
  a[1] = str ("aaa"); CHECK (GLR ("parse", &r, 2, a) == TS_OK);
  CHECK (GLR ("ambiguous", &r, 1, &grammar) == TS_OK && r.as.boolean);
  ts_value_free (&grammar);
  // Empty input, pattern errors, build recovery and default-no-trivia behavior.
  CHECK (GLR ("new", &grammar, 1, &start) == TS_OK);
  add_symbol (grammar, "nonterminal", "S", NULL); add_rule (grammar, "S", NULL, 0, "Empty");
  CHECK (GLR ("build", &r, 1, &grammar) == TS_OK);
  a[0] = grammar; a[1] = str (""); CHECK (GLR ("parse", &r, 2, a) == TS_OK); text_field (&r, "name", "Empty");
  ts_value_free (&grammar);
  CHECK (GLR ("new", &grammar, 1, &start) == TS_OK);
  CHECK (GLR ("build", &r, 1, &grammar) == TS_ERR_INVAL);
  add_symbol (grammar, "nonterminal", "S", NULL); add_symbol (grammar, "pattern", "word", "[a-z]+");
  const char *word[] = { "word" }; add_rule (grammar, "S", word, 1, NULL);
  CHECK (GLR ("build", &r, 1, &grammar) == TS_OK);
  a[0] = grammar; a[1] = str ("abc"); CHECK (GLR ("parse", &r, 2, a) == TS_OK);
  a[1] = str (" abc"); CHECK (GLR ("parse", &r, 2, a) == TS_ERR_PARSE);
  char big[4098]; memset (big, 'a', sizeof big - 1); big[sizeof big - 1] = 0;
  a[1] = str (big); CHECK (GLR ("parse", &r, 2, a) == TS_ERR_LIMIT);
  ts_value_free (&grammar);
  CHECK (GLR ("new", &grammar, 1, &start) == TS_OK);
  add_symbol (grammar, "nonterminal", "S", NULL); add_symbol (grammar, "pattern", "bad", "[");
  const char *bad[] = { "bad" }; add_rule (grammar, "S", bad, 1, NULL);
  CHECK (GLR ("build", &r, 1, &grammar) == TS_ERR_PARSE);
  ts_value_free (&grammar); ts_value_free (&r);
  CHECK (GLR ("new", &grammar, 1, &start) == TS_OK);
  add_symbol (grammar, "nonterminal", "S", NULL);
  TS_Value invalid[] = { grammar, str ("S"), str ("x") };
  CHECK (GLR ("literal", &r, 3, invalid) == TS_ERR_INVAL); // duplicate name, no replacement
  invalid[1] = str ("empty"); invalid[2] = str ("");
  CHECK (GLR ("literal", &r, 3, invalid) == TS_ERR_INVAL);
  for (unsigned i = 1; i < 128; i++)
    { char name[32]; snprintf (name, sizeof name, "N%u", i); add_symbol (grammar, "nonterminal", name, NULL); }
  invalid[1] = str ("overflow");
  CHECK (GLR ("nonterminal", &r, 2, invalid) == TS_ERR_LIMIT);
  ts_value_free (&grammar); ts_value_free (&r);
}
int main (void) { peg_tests (); shell_peg_tests (); glr_tests (); puts ("parser modules OK"); return 0; }
