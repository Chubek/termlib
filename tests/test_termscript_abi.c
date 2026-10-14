/* Counted native tables, rejected descriptors and callback result ownership.
 */
#include "termscript/termscript.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                              \
  do                                                                          \
    {                                                                         \
      if (!(x))                                                               \
        {                                                                     \
          fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
          exit (1);                                                           \
        }                                                                     \
    }                                                                         \
  while (0)

static int destroyed;
static const char resource_tag;

static void
destroy_resource (void *ptr)
{
  CHECK (ptr == &destroyed);
  destroyed++;
}

static TS_Status
resource (TS_VM *vm, void *userdata, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  TS_Status st = ts_check_argc (vm, argv, argc, 0, error);
  if (st != TS_OK)
    return st;
  CHECK (ret->type == TS_NIL);
  st = ts_value_make_handle (ret, &destroyed, destroy_resource, "resource",
                             &resource_tag);
  if (st != TS_OK)
    return st;
  if (userdata)
    {
      ts_error_set (error, TS_ERR_SYSTEM, EIO, 17, "native failure");
      return TS_ERR_SYSTEM;
    }
  return TS_OK;
}

/* Deliberately no sentinel. The last function must be callable. */
static const TS_FuncDef funcs[]
    = { { "new", resource, NULL }, { "fail", resource, &destroyed } };
static const TS_Module module = TS_MODULE_INIT ("fixture", funcs);

static void
validation_tests (TS_VM *vm)
{
  TS_Error error;
  TS_Module bad = module;
  ts_error_set (&error, TS_ERR_PARSE, 0, 123, "original");
  CHECK (ts_native_abi_version () == TS_NATIVE_ABI_VERSION);
  CHECK (ts_module_validate (&module, &error) == TS_OK);
  CHECK (error.code == TS_ERR_PARSE && error.offset == 123);
  CHECK (!strcmp (error.message, "original"));
  CHECK (ts_module_find_func (&module, "fail") == &funcs[1]);
  CHECK (!ts_module_find_func (&module, "missing"));
  CHECK (!ts_module_find_func (NULL, "new"));
  CHECK (!ts_module_find_func (&module, NULL));
  CHECK (ts_module_validate (NULL, &error) == TS_ERR_INVAL);
  CHECK (ts_vm_register_module (NULL, &module, &error) == TS_ERR_INVAL);

  /* Incompatible metadata must be rejected before dereferencing functions. */
  bad.abi_version++;
  bad.funcs = (const TS_FuncDef *)&destroyed;
  CHECK (ts_vm_register_module (vm, &bad, &error) == TS_ERR_UNSUPPORTED);
  bad = module;
  bad.struct_size--;
  bad.funcs = (const TS_FuncDef *)&destroyed;
  CHECK (ts_vm_register_module (vm, &bad, NULL) == TS_ERR_UNSUPPORTED);
  bad = module;
  bad.reserved[3] = 1;
  CHECK (ts_vm_register_module (vm, &bad, &error) == TS_ERR_INVAL);
  bad = module;
  bad.func_count = TS_MAX_MODULE_FUNCS + 1;
  CHECK (ts_vm_register_module (vm, &bad, &error) == TS_ERR_LIMIT);
  bad = module;
  bad.funcs = NULL;
  CHECK (ts_vm_register_module (vm, &bad, &error) == TS_ERR_INVAL);
  bad = module;
  bad.name = "";
  CHECK (ts_vm_register_module (vm, &bad, &error) == TS_ERR_INVAL);

  TS_FuncDef invalid[]
      = { { "new", resource, NULL }, { "new", resource, NULL } };
  ts_module_init (&bad, "fixture", invalid, 2);
  CHECK (ts_vm_register_module (vm, &bad, &error) == TS_ERR_INVAL);
  invalid[1].name = NULL;
  CHECK (ts_module_validate (&bad, &error) == TS_ERR_INVAL);
  invalid[1].name = "";
  CHECK (ts_module_validate (&bad, &error) == TS_ERR_INVAL);
  invalid[1].name = "broken";
  invalid[1].func = NULL;
  CHECK (ts_module_validate (&bad, &error) == TS_ERR_INVAL);
  /* Invalid storage beyond the advertised count is never inspected. */
  bad.func_count = 1;
  CHECK (ts_module_validate (&bad, &error) == TS_OK);
  CHECK (!ts_module_find_func (&bad, "broken"));
  ts_module_init (&bad, "empty", NULL, 0);
  CHECK (ts_module_validate (&bad, &error) == TS_OK);
  CHECK (!ts_module_find_func (&bad, "new"));
  ts_module_init (NULL, NULL, NULL, 0);

  /* Every failed registration above used this same name. */
  CHECK (ts_vm_register_module (vm, &module, &error) == TS_OK);
  CHECK (ts_vm_register_module (vm, &module, &error) == TS_ERR_INVAL);
}

static void
execution_tests (TS_VM *vm, bool jit)
{
  const char *failures[]
      = { "M = G:load(\"fixture\"); M:fail();",
          "M = G:load(\"fixture\"); const value = M:fail();",
          "M = G:load(\"fixture\"); if M:fail() do end",
          "M = G:load(\"fixture\"); while M:fail() do end" };
  TS_Error error;
  char *out = NULL;
  for (size_t i = 0; i < sizeof failures / sizeof failures[0]; i++)
    {
      int before = destroyed;
      ts_error_clear (&error);
      TS_Status st
          = jit ? ts_vm_run_backend (vm, failures[i], TS_BACKEND_JITERATI,
                                     &out, &error)
                : ts_vm_run_string (vm, failures[i], &out, &error);
      CHECK (st == TS_ERR_SYSTEM && error.code == TS_ERR_SYSTEM);
      CHECK (error.system_errno == EIO && error.offset == 17);
      CHECK (!strcmp (error.message, "native failure"));
      CHECK (destroyed == before + 1);
      free (out);
      out = NULL;
    }
  CHECK (ts_vm_run_string (
             vm,
             "M = G:load(\"fixture\"); const h = M:new(); const alias = h;",
             NULL, &error)
         == TS_OK);
  TS_Value mod = { 0 }, ret = { 0 };
  CHECK (ts_rt_get (vm, "M", &mod, 0, &error) == TS_OK);
  int before = destroyed;
  CHECK (ts_rt_call (vm, &mod, "M", "fail", NULL, 0, &ret, 0, &error)
         == TS_ERR_SYSTEM);
  CHECK (ret.type == TS_NIL && destroyed == before + 1);
  CHECK (ts_rt_call (vm, &mod, "M", "missing", NULL, 0, &ret, 0, &error)
         == TS_ERR_NOTFOUND);
  CHECK (ts_rt_call (vm, &mod, "M", "new", NULL, 1, &ret, 0, &error)
         == TS_ERR_INVAL);
  ts_value_free (&mod);
  ts_value_free (&ret);
}

int
main (void)
{
  TS_Error error;
  TS_VM *vm = ts_vm_create (&error);
  CHECK (vm);
  validation_tests (vm);
  execution_tests (vm, false);
  int before = destroyed;
  ts_vm_free (vm);
  CHECK (destroyed == before + 1); /* Two aliases, one destructor. */
  if (ts_backend_available (TS_BACKEND_JITERATI))
    {
      vm = ts_vm_create (&error);
      CHECK (vm && ts_vm_register_module (vm, &module, &error) == TS_OK);
      execution_tests (vm, true);
      before = destroyed;
      ts_vm_free (vm);
      CHECK (destroyed == before + 1);
    }
  puts ("Termscript native ABI tests passed");
  return 0;
}
