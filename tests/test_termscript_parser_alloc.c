#include "termscript/stdlib/ts_stdlib.h"
#include "termscript/stdlib/common/ts_std_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ELF link wrappers fault-inject the adapter/runtime/common allocations without
 * adding allocator globals to the library. NovoParse's shared-library internal
 * allocations are intentionally outside this harness; ASan covers those. */
void *__real_malloc (size_t);
void *__real_calloc (size_t, size_t);
void *__real_realloc (void *, size_t);
void __real_free (void *);
static void *pointers[100000];
static size_t live, remaining = SIZE_MAX;
static bool injected;
static bool fail (void)
{
  if (remaining == SIZE_MAX) return false;
  if (remaining) { remaining--; return false; }
  remaining = SIZE_MAX; injected = true; return true;
}
static void remember (void *p)
{
  if (!p) return;
  for (size_t i = 0; i < sizeof pointers / sizeof *pointers; i++)
    if (!pointers[i]) { pointers[i] = p; live++; return; }
  abort ();
}
static void forget (void *p)
{
  if (!p) return;
  for (size_t i = 0; i < sizeof pointers / sizeof *pointers; i++)
    if (pointers[i] == p) { pointers[i] = NULL; live--; return; }
}
void *__wrap_malloc (size_t size)
{ if (fail ()) return NULL; void *p = __real_malloc (size); remember (p); return p; }
void *__wrap_calloc (size_t count, size_t size)
{ if (fail ()) return NULL; void *p = __real_calloc (count, size); remember (p); return p; }
void *__wrap_realloc (void *old, size_t size)
{
  if (fail ()) return NULL;
  /* Forget before realloc so the old pointer is never used after successful
   * realloc. Restore tracking on failure (all tested callers use size > 0). */
  forget (old);
  void *p = __real_realloc (old, size);
  remember (p ? p : old); return p;
}
char *__wrap_strdup (const char *text)
{
  size_t size = strlen (text) + 1;
  char *p = __wrap_malloc (size);
  if (p) memcpy (p, text, size);
  return p;
}
void __wrap_free (void *p) { forget (p); __real_free (p); }
static TS_Error error;
#define CHECK(x) do { if (!(x)) { fprintf (stderr, "%d: %s (%s)\n", __LINE__, #x, error.message); exit (1); } } while (0)
static TS_Value str (const char *s) { TS_Value v = { .type = TS_STRING, .as.string = (char *)s }; return v; }
static TS_Status call (const TS_Module *m, const char *name, TS_Value *r, size_t count, TS_Value *a)
{
  ts_value_free (r); ts_error_clear (&error);
  const TS_FuncDef *f = ts_module_find_func (m, name); CHECK (f);
  return f->func (NULL, f->userdata, a, count, r, &error);
}
static void glr_setup (TS_Value *grammar)
{
  TS_Value s = str ("Root"), r = {0};
  CHECK (call (&ts_std_grammar_module, "new", grammar, 1, &s) == TS_OK);
  TS_Value args[] = { *grammar, str ("Root"), str ("abc") };
  CHECK (call (&ts_std_grammar_module, "nonterminal", &r, 2, args) == TS_OK);
  args[1] = str ("word");
  CHECK (call (&ts_std_grammar_module, "literal", &r, 3, args) == TS_OK);
  ts_std_list_t *list = ts_std_list_new (); CHECK (list);
  CHECK (ts_std_list_push_str (list, "word") == TS_OK);
  TS_Value body = {0}; CHECK (ts_std_list_wrap (&body, list, &error) == TS_OK);
  args[1] = str ("Root"); args[2] = body;
  CHECK (call (&ts_std_grammar_module, "rule", &r, 3, args) == TS_OK);
  CHECK (call (&ts_std_grammar_module, "build", &r, 1, grammar) == TS_OK);
  ts_value_free (&body); ts_value_free (&r);
}
int main (void)
{
  TS_Value source = str ("Root <- &(A / B) (A / B)+ !.; A <- 'a'; B <- [b-z]");
  TS_Value grammar = {0}, result = {0};
  for (unsigned operation = 0; operation < 4; operation++)
    {
      bool succeeded = false;
      if (operation == 1)
        CHECK (call (&ts_std_peg_module, "compile", &grammar, 1, &source) == TS_OK);
      if (operation == 3) glr_setup (&grammar);
      for (size_t index = 0; index < 1000; index++)
        {
          size_t before = live;
          TS_Value args[] = { grammar, str ("abc") };
          injected = false; remaining = index;
          TS_Status status;
          if (operation == 0) status = call (&ts_std_peg_module, "compile", &result, 1, &source);
          else if (operation == 1) status = call (&ts_std_peg_module, "parse", &result, 2, args);
          else if (operation == 3) status = call (&ts_std_grammar_module, "parse", &result, 2, args);
          else
            {
              TS_Value start = str ("S");
              status = call (&ts_std_grammar_module, "new", &result, 1, &start);
            }
          remaining = SIZE_MAX;
          if (injected)
            {
              CHECK (status == TS_ERR_NOMEM && error.code == TS_ERR_NOMEM);
              CHECK (result.type == TS_NIL);
            }
          else { CHECK (status == TS_OK); succeeded = true; }
          ts_value_free (&result); CHECK (live == before);
          if (succeeded) break;
        }
      CHECK (succeeded); ts_value_free (&grammar);
    }
  CHECK (!live);
  puts ("parser allocation cleanup OK");
  return 0;
}
