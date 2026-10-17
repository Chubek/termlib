#include "termscript/include/Termscript.h"
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
static const char resource_tag;
static void
destroy (void *p)
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
  return ts_value_make_handle (r, &destroyed, destroy, "resource",
                               &resource_tag);
}
static const TS_FuncDef functions[] = { { "new", resource, NULL } };
static const TS_Module fixture = TS_MODULE_INIT ("fixture", functions);

int
main (void)
{
  TS_Error error = { 0 };
  TS_VM *vm = ts_vm_create (&error);
  CHECK (vm);
  CHECK (ts_vm_register_module (vm, &fixture, &error) == TS_OK);
  const TS_BackendAPI *api = ts_backend_get (TS_BACKEND_VM);
  CHECK (api && api->abi_version == TS_BACKEND_ABI_VERSION
         && api->available ());
  CHECK (!api->compile && api->run);
  CHECK (!ts_backend_get ((TS_Backend)999));
  char *out = NULL;
  CHECK (
      api->run (vm,
                "const F = G:Import(\"fixture\")\n"
                "fn success(x: int) { lifetime h := F:new(); return x + 1 }\n"
                "print(success(41))\n",
                &out, &error)
      == TS_OK);
  CHECK (!strcmp (out, "42\n") && destroyed == 1);
  free (out);
  out = NULL;
  /* Functions own their trees and survive the source run that defined them. */
  CHECK (ts_vm_run_string (vm, "print(success(9))", &out, &error) == TS_OK);
  CHECK (!strcmp (out, "10\n") && destroyed == 2);
  free (out);
  out = NULL;
  CHECK (ts_vm_run_string (vm,
                           "fn failure() { lifetime h := F:new(); "
                           "G:die(\"original\") }\n failure()",
                           NULL, &error)
         == TS_ERR_CANCELLED);
  CHECK (destroyed == 3 && !strcmp (error.message, "original"));
  CHECK (ts_vm_run_string (vm, "fn recurse() { return recurse() }\nrecurse()",
                           NULL, &error)
         == TS_ERR_LIMIT);
  CHECK (ts_vm_run_string (vm, "print(success(1))", &out, &error) == TS_OK);
  CHECK (!strcmp (out, "2\n") && destroyed == 4);
  free (out);
  CHECK (ts_vm_run_string (vm, "failure()", NULL, NULL) == TS_ERR_CANCELLED);
  CHECK (destroyed == 5);
  CHECK (ts_check_syntax ("return 1", &error) == TS_ERR_INVAL);
  CHECK (ts_check_syntax ("fn duplicate(a, a) {}", &error) == TS_ERR_INVAL);
  CHECK (ts_check_syntax ("fn outer() { fn inner() {} }", &error)
         == TS_ERR_INVAL);
  CHECK (ts_check_syntax ("fn main(a, b) {}", &error) == TS_ERR_INVAL);
  CHECK (ts_check_syntax ("let x = (1 + )", &error) == TS_ERR_PARSE);
  CHECK (ts_check_syntax ("while false do end\nModule:render(1);", &error)
         == TS_OK);
  CHECK (ts_check_syntax ("print(-9223372036854775809)", &error)
         == TS_ERR_PARSE);
  CHECK (ts_check_syntax ("print(1 + 9999999999999999999999)", &error)
         == TS_ERR_PARSE);
  CHECK (ts_vm_run_string (
             vm, "while false do end\nconst render = 42; print(render)", &out,
             &error)
         == TS_OK);
  CHECK (!strcmp (out, "42\n"));
  free (out);
  out = NULL;
  CHECK (ts_vm_run_string (
             vm,
             "# comment G:die(\"must not execute\")\nprint(42) # EOF comment",
             &out, &error)
         == TS_OK);
  CHECK (!strcmp (out, "42\n"));
  free (out);
  ts_vm_free (vm);
  return 0;
}
