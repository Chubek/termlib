#include "termscript/stdlib/ts_stdlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                              \
  do                                                                          \
    {                                                                         \
      if (!(x))                                                               \
        {                                                                     \
          fprintf (stderr, "%d: %s (%s)\n", __LINE__, #x, error.message);     \
          exit (1);                                                           \
        }                                                                     \
    }                                                                         \
  while (0)

static int destroyed;
static const char tag;
static void
dispose (void *p)
{
  (void)p;
  destroyed++;
}
static TS_Status
resource (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r,
          TS_Error *e)
{
  (void)vm;
  (void)ud;
  (void)a;
  (void)n;
  (void)e;
  return ts_value_make_handle (r, &destroyed, dispose, "resource", &tag);
}
static const TS_FuncDef funcs[] = { { "new", resource, NULL } };
static const TS_Module fixture = TS_MODULE_INIT ("fixture", funcs);

int
main (void)
{
  TS_Error error = { 0 };
  TS_VM *vm = ts_vm_create (&error);
  CHECK (vm);
  CHECK (ts_stdlib_register_all (vm, &error) == TS_OK);
  CHECK (ts_vm_register_module (vm, &fixture, &error) == TS_OK);
  CHECK (ts_vm_run_string (vm,
                           "use http\nhttp.fetch(\"http://127.0.0.1/\", 0)",
                           NULL, &error)
         == TS_ERR_INVAL);
  CHECK (ts_vm_run_string (vm, "http.fetch(\"http://127.0.0.1/\", 1, 1048577)",
                           NULL, &error)
         == TS_ERR_INVAL);
  CHECK (ts_vm_run_string (vm, "http.fetch(42)", NULL, &error)
         == TS_ERR_INVAL);
  CHECK (ts_vm_run_string (
             vm,
             "const Fixture = G:Import(\"fixture\")\n"
             "fn take() { var xs = [Fixture:new()]; return xs[-1] }\n"
             "var kept = take()",
             NULL, &error)
         == TS_OK);
  CHECK (destroyed
         == 0); /* Returned alias survives temporary list destruction. */
  CHECK (ts_vm_run_string (vm, "kept = nil", NULL, &error) == TS_OK);
  CHECK (destroyed == 1);
  const char *bad[] = {
    "fn bad_one() { var xs = [Fixture:new()]; return xs[true] }\nbad_one()",
    "fn bad_two() { var xs = [Fixture:new()]; return xs[\"key\"] }\nbad_two()",
    "fn bad_three() { var xs = {\"a\": Fixture:new()}; return xs[0] "
    "}\nbad_three()"
  };
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
    {
      CHECK (ts_vm_run_string (vm, bad[i], NULL, &error) == TS_ERR_INVAL);
      CHECK (destroyed == (int)i + 2);
    }
  CHECK (
      ts_vm_run_string (vm, "print(r#\"parser still usable\"#)", NULL, &error)
      == TS_OK);
  ts_vm_free (vm);
  CHECK (destroyed == 4);
  return 0;
}
