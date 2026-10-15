/* Section validation, observer ordering, and success/failure ownership. */
#include "termscript/termscript.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit (1); } } while (0)
static unsigned closed[16], nclosed, next_id, observed;
static const char tag;

static void
close_resource (void *ptr)
{
  unsigned *id = ptr;
  CHECK (nclosed < 16); closed[nclosed++] = *id; free (id);
}

static TS_Status
make_resource (TS_VM *vm, void *ud, const TS_Value *args, size_t argc,
               TS_Value *ret, TS_Error *error)
{
  CHECK (ts_check_argc (vm, args, argc, 0, error) == TS_OK);
  unsigned *id = malloc (sizeof *id); CHECK (id); *id = ++next_id;
  CHECK (ts_value_make_handle (ret, id, close_resource, "fixture", &tag) == TS_OK);
  if (ud) { ts_error_set (error, TS_ERR_SYSTEM, EIO, 17, "initialization failed"); return TS_ERR_SYSTEM; }
  return TS_OK;
}

static TS_Status
observe (TS_VM *vm, void *ud, const TS_Value *a, size_t n,
         TS_Value *ret, TS_Error *error)
{
  (void) vm; (void) ret; (void) error;
  if (ud)
    {
      CHECK (n == 4 && a[0].type == TS_STRING && !strcmp (a[0].as.string, "initialization failed"));
      CHECK (a[1].type == TS_INT && a[1].as.integer == TS_ERR_SYSTEM);
      CHECK (a[2].type == TS_INT && a[2].as.integer == EIO);
      CHECK (a[3].type == TS_INT && a[3].as.integer == 17);
      observed = observed * 10 + 1;
    }
  else { CHECK (n == 0); observed = observed * 10 + 2; }
  return TS_OK;
}

static const TS_FuncDef funcs[] = {
  { "new", make_resource, NULL }, { "fail", make_resource, (void *) &tag },
  { "observe", observe, (void *) &tag }, { "after", observe, NULL }
};
static const TS_Module module = TS_MODULE_INIT ("fixture", funcs);

static TS_Status
run (TS_VM *vm, bool jit, const char *source, char **out, TS_Error *e)
{
  ts_error_clear (e);
  return jit ? ts_vm_run_backend (vm, source, TS_BACKEND_JITERATI, out, e)
             : ts_vm_run_string (vm, source, out, e);
}

static void
execution (bool jit)
{
  TS_Error e; char *out = NULL;
  TS_VM *vm = ts_vm_create (&e); CHECK (vm);
  CHECK (ts_vm_register_module (vm, &module, &e) == TS_OK);
  next_id = nclosed = observed = 0;
  const char *ok =
    "@alias Global :G\n@alias Runtime :R\n@alias Module :M\n@define $ENVELOPE $E\n"
    ".interface R:Declare:Section(:Interface) R:Declare:Section(:Prelude) R:Declare:Section(:Program) .end\n"
    ".section Interface G:Declare:Symbol(:Module) .end\n"
    ".section Prelude lifetime Module := G:Import(\"fixture\") => R:Inject($E->Library, G:Module); lifetime first := M:new(); .end\n"
    ".section Program G:OnStartup(:Prelude); config answer := G:add(20, G:add(20, 2)); "
    "lifetime second := M:new() => M:after(); "
    "if not G:eq(answer, 42) then G:die(\"bad answer\"); end "
    "G:puts(answer); R:Exit(:EXIT_SUCCESS); G:die(\"unreachable\"); .end";
  CHECK (run (vm, jit, ok, &out, &e) == TS_OK);
  CHECK (out && !strcmp (out, "42\n")); free (out); out = NULL;
  CHECK (nclosed == 2 && closed[0] == 2 && closed[1] == 1 && observed == 2);
  TS_Value v = {0};
  CHECK (ts_rt_get (vm, "first", &v, 0, &e) == TS_ERR_NOTFOUND);
  CHECK (ts_rt_get (vm, "Module", &v, 0, &e) == TS_ERR_NOTFOUND);
  CHECK (run (vm, jit, "answer = 1;", NULL, &e) == TS_ERR_INVAL);

  next_id = nclosed = observed = 0;
  CHECK (run (vm, jit,
    "M = G:load(\"fixture\"); lifetime first := M:new(); lifetime second := M:new(); "
    "lifetime broken := M:fail() ~> M:observe($E->Msg, $E->Code, $E->Errno, $E->Offset) "
    "-> M:after() => G:die(\"unreachable\");", NULL, &e) == TS_ERR_SYSTEM);
  CHECK (e.code == TS_ERR_SYSTEM && e.system_errno == EIO && e.offset == 17);
  CHECK (!strcmp (e.message, "initialization failed") && observed == 12);
  CHECK (nclosed == 3 && closed[0] == 3 && closed[1] == 2 && closed[2] == 1);
  CHECK (run (vm, jit, "G:puts($E->Msg);", NULL, &e) == TS_ERR_NOTFOUND);
  CHECK (run (vm, jit, "G:Import(\"missing\") ~> R:ImportError($E->Msg) -> R:Exit(:EXIT_SUCCESS);", NULL, &e) == TS_ERR_NOTFOUND);
  CHECK (strstr (e.message, "missing"));
  CHECK (run (vm, jit, "nil ~> G:die(\"wrong branch\") => G:puts($E->Value);", &out, &e) == TS_OK);
  CHECK (!strcmp (out, "nil\n")); free (out); out = NULL;
  CHECK (run (vm, jit, "true => G:puts((42 => G:puts($E->Value)));", &out, &e) == TS_OK);
  CHECK (!strcmp (out, "42\n42\n")); free (out); out = NULL;

  next_id = nclosed = 0;
  CHECK (run (vm, jit, "lifetime x := M:new(); alias = x;", NULL, &e) == TS_OK);
  CHECK (nclosed == 0); /* Retained alias transfers the remaining reference. */
  CHECK (run (vm, jit, "alias = nil;", NULL, &e) == TS_OK);
  CHECK (nclosed == 1 && closed[0] == 1);
  next_id = nclosed = 0;
  CHECK (run (vm, jit, "lifetime x := M:new(); lifetime x := M:new();", NULL, &e) == TS_ERR_INVAL);
  CHECK (nclosed == 2 && closed[0] == 2 && closed[1] == 1);
  CHECK (run (vm, jit, "lifetime x := M:new(); while true do end", NULL, &e) == TS_ERR_LIMIT);
  CHECK (nclosed == 3);
  ts_vm_free (vm);
}

static void
validation (void)
{
  const char *bad[] = {
    ".section Program .end .section Program .end",
    ".section Prelude .end",
    ".interface R:Declare:Section(:Missing) .end .section Program .end",
    ".interface R:Declare:Section(:Program) R:Declare:Section(:Program) .end .section Program .end",
    ".interface R:Declare:Section(:Program) .end .section Program .end .section Hidden .end",
    ".section Program G:OnStartup(:Missing); .end",
    ".section Program G:OnStartup(:Other); .end .section Other G:OnStartup(:Program); .end",
    ".section Program G:OnStartup(name); .end",
    "@define a b\n@define b a\nG:puts(a);",
    "@alias Foo :F\n@alias Bar :F\nG:puts(1);",
    ".section Program config x := 1; .section Nested .end .end",
    "lifetime x := G:add(1, 2) ~> ;",
    "config x := {\"a\": };",
    "G:puts([1,,2]);"
  };
  TS_Error e; TS_VM *vm = ts_vm_create (&e); CHECK (vm);
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    {
      ts_error_clear (&e); CHECK (ts_check_syntax (bad[i], &e) != TS_OK);
      CHECK (!ts_vm_compile_to_c (vm, bad[i], "bad", &e));
      CHECK (!ts_vm_compile_to_qbe (vm, bad[i], false, &e));
    }
  CHECK (ts_check_syntax ("", &e) == TS_OK);
  CHECK (ts_check_syntax ("G:puts(G:add(1, 2));", &e) == TS_OK);
  CHECK (ts_vm_run_string (vm, "config x := [1, 2];", NULL, &e) == TS_ERR_NOTFOUND);
  CHECK (strstr (e.message, "std.list"));
  char *large = malloc (TS_MAX_SOURCE_SIZE + 1); CHECK (large);
  memset (large, ' ', TS_MAX_SOURCE_SIZE); large[TS_MAX_SOURCE_SIZE] = '\0';
  CHECK (ts_check_syntax (large, &e) == TS_ERR_LIMIT); free (large);
  char nested[2048]; size_t len = 0;
  for (unsigned i = 0; i <= TS_MAX_SYNTAX_NESTING; i++) nested[len++] = '(';
  nested[len++] = '1';
  for (unsigned i = 0; i <= TS_MAX_SYNTAX_NESTING; i++) nested[len++] = ')';
  nested[len++] = ';'; nested[len] = '\0';
  CHECK (ts_check_syntax (nested, &e) == TS_ERR_LIMIT);
  ts_vm_free (vm);
}

static int
emit_fixture (const char *path, bool fail)
{
  const char *source = fail
    ? "M = G:load(\"fixture\"); lifetime first := M:new(); lifetime second := M:new(); M:after(M:new(), M:fail()) ~> M:observe($E->Msg, $E->Code, $E->Errno, $E->Offset) -> M:after();"
    : "M = G:load(\"fixture\"); lifetime first := M:new(); lifetime second := M:new() => M:after();";
  TS_Error e; TS_VM *vm = ts_vm_create (&e); CHECK (vm);
  const char *code = ts_vm_compile_to_c (vm, source, "section_fixture", &e);
  CHECK (code);
  FILE *file = fopen (path, "w"); CHECK (file);
  CHECK (fputs ("#define main ts_generated_main\n", file) >= 0);
  CHECK (fputs (code, file) >= 0);
  CHECK (fputs ("\n#undef main\n#define main ts_section_unit_tests_main\n#include \"tests/test_termscript_sections.c\"\n#undef main\nint main (void) { TS_Error e; ts_error_clear (&e); TS_VM *vm = ts_vm_create (&e); CHECK (vm); CHECK (ts_vm_register_module (vm, &module, &e) == TS_OK); TS_Status st = tsc_run_section_fixture (vm, &e);\n", file) >= 0);
  CHECK (fputs (fail
    ? "CHECK (st == TS_ERR_SYSTEM); CHECK (e.code == TS_ERR_SYSTEM && e.system_errno == EIO && e.offset == 17); CHECK (!strcmp (e.message, \"initialization failed\")); CHECK (observed == 12); CHECK (nclosed == 4 && closed[0] == 4 && closed[1] == 3 && closed[2] == 2 && closed[3] == 1);\n"
    : "CHECK (st == TS_OK); CHECK (observed == 2); CHECK (nclosed == 2 && closed[0] == 2 && closed[1] == 1);\n", file) >= 0);
  CHECK (fputs ("unsigned before = nclosed; ts_vm_free (vm); CHECK (nclosed == before); puts (\"compiled section ownership passed\"); return 0; }\n", file) >= 0);
  CHECK (fclose (file) == 0); ts_vm_free (vm); return 0;
}

int
main (int argc, char **argv)
{
  if (argc == 3 && (!strcmp (argv[1], "--emit-ok") || !strcmp (argv[1], "--emit-fail")))
    return emit_fixture (argv[2], !strcmp (argv[1], "--emit-fail"));
  validation (); execution (false);
  if (ts_backend_available (TS_BACKEND_JITERATI)) execution (true);
  puts ("Termscript section semantics passed"); return 0;
}
