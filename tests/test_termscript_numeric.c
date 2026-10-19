#define _POSIX_C_SOURCE 200809L
#include "termscript/include/Termscript.h"
#ifdef TS_NUMERIC_STDLIB
#include "termscript/stdlib/ts_stdlib.h"
#endif
#include <float.h>
#include <locale.h>
#include <math.h>
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

static TS_Status
bad_float (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r,
           TS_Error *e)
{
  (void)vm;
  (void)u;
  (void)a;
  (void)n;
  (void)e;
  r->type = TS_FLOAT;
  r->as.real = INFINITY;
  return TS_OK;
}
static const TS_FuncDef functions[] = { { "invalid", bad_float, NULL } };
static const TS_Module fixture = TS_MODULE_INIT ("numeric_fixture", functions);

int
main (void)
{
  TS_Error error = { 0 };
  TS_Value v = { 0 }, copy = { 0 };
  CHECK (ts_native_abi_version () == 2);
  CHECK (ts_value_make_string (&v, "owned") == TS_OK);
  CHECK (ts_value_make_float (&v, NAN) == TS_ERR_INVAL);
  CHECK (v.type == TS_STRING && !strcmp (v.as.string, "owned"));
  CHECK (ts_value_make_float (&v, 1.25) == TS_OK);
  CHECK (v.type == TS_FLOAT && ts_value_truthy (&v));
  CHECK (ts_value_copy (&copy, &v) == TS_OK && copy.as.real == 1.25);
  char *text = NULL;
  CHECK (ts_value_to_string (&v, &text, &error) == TS_OK);
  CHECK (!strcmp (text, "1.25"));
  free (text);
  CHECK (ts_value_make_float (&v, -0.0) == TS_OK && !ts_value_truthy (&v));
  CHECK (ts_value_to_string (&v, &text, &error) == TS_OK);
  CHECK (!strcmp (text, "-0.0"));
  free (text);
  CHECK (ts_value_parse_float (&v, "5e-324", &error) == TS_OK
         && v.as.real > 0.0);
  CHECK (ts_value_parse_float (&v, "1.7976931348623157e308", &error) == TS_OK
         && v.as.real == DBL_MAX);
  const char *bad[] = { "nan",  "inf",   "-inf", "1e999", "1e-999", " 1.0",
                        "1.0 ", "0x1p2", "1e",   ".",     "1.0x" };
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
    {
      CHECK (ts_value_parse_float (&v, bad[i], &error) != TS_OK);
      CHECK (v.type == TS_FLOAT && v.as.real == DBL_MAX);
    }
  const char *locales[] = { "de_DE.UTF-8", "fr_FR.UTF-8", "fa_IR.UTF-8" };
  bool comma_locale_tested = false;
  for (size_t i = 0; i < sizeof locales / sizeof *locales; i++)
    {
      locale_t locale = newlocale (LC_NUMERIC_MASK, locales[i], (locale_t)0);
      if (!locale)
        continue; /* No dependency on installed optional locales. */
      locale_t previous = uselocale (locale);
      if (!strcmp (localeconv ()->decimal_point, ","))
        comma_locale_tested = true;
      CHECK (ts_value_parse_float (&v, "1.5", &error) == TS_OK);
      CHECK (ts_value_to_string (&v, &text, &error) == TS_OK);
      CHECK (!strcmp (text, "1.5"));
      free (text);
      CHECK (uselocale ((locale_t)0) == locale);
      uselocale (previous);
      freelocale (locale);
    }
  if (getenv ("TS_TEST_REQUIRE_COMMA"))
    CHECK (comma_locale_tested);
  ts_value_free (&v);
  ts_value_free (&copy);
  TS_VM *vm = ts_vm_create (&error);
  CHECK (vm);
  CHECK (!strcmp (ts_version_string (), "0.4.0"));
  TS_Value invalid_value = { .type = TS_FLOAT };
  invalid_value.as.real = NAN;
  CHECK (ts_rt_set (vm, "bad", &invalid_value, false, 1, &error)
         == TS_ERR_INVAL);
  TS_Value result = { 0 }, module = { 0 };
  CHECK (ts_rt_get (vm, "G", &module, 1, &error) == TS_OK);
  CHECK (ts_rt_call (vm, &module, "G", "type", &invalid_value, 1, &result, 1,
                     &error)
         == TS_ERR_INVAL);
  ts_value_free (&module);
  TS_Module old = fixture;
  old.abi_version = 1;
  CHECK (ts_vm_register_module (vm, &old, &error) == TS_ERR_UNSUPPORTED);
  CHECK (ts_vm_register_module (vm, &fixture, &error) == TS_OK);
  const char *failures[]
      = { "G:puts(1.0 / 0.0);",
          "G:puts(1e308 * 1e308);",
          "G:puts(1.0 % 0.0);",
          "G:puts(1.0 << 1);",
          "G:puts(G:float(\"nan\"));",
          "G:puts(G:int(9223372036854775808.0));",
          "const F = G:load(\"numeric_fixture\"); F:invalid();" };
  for (size_t i = 0; i < sizeof failures / sizeof *failures; i++)
    {
      char *output = NULL;
      CHECK (ts_vm_run_string (vm, failures[i], &output, &error) != TS_OK);
      free (output);
      CHECK (ts_vm_run_string (vm, "G:puts(1.5 + 2);", &output, &error)
             == TS_OK);
      CHECK (output && !strcmp (output, "3.5\n"));
      free (output);
    }
  const char *invalid[] = { "G:puts(1e309);", "G:puts(1e-999);", "G:puts(1e);",
                            "G:puts(1.2.3);" };
  for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
    CHECK (!ts_vm_compile_to_c (vm, invalid[i], "bad", &error)
           && error.code == TS_ERR_PARSE);
  CHECK (!ts_vm_compile_to_c (vm, "fn float(x) { return x }", "bad", &error)
         && error.code == TS_ERR_INVAL);
  CHECK (!ts_vm_compile_to_c (vm, "fn int(x) { return x }", "bad", &error)
         && error.code == TS_ERR_INVAL);
  const char *compiled
      = ts_vm_compile_to_c (vm, "G:puts(1.25);", "float_literal", &error);
  CHECK (compiled && strstr (compiled, "ts_float_bits"));
  CHECK (
      !strstr (compiled, "\"1.25\"")); /* No run-time literal text parsing. */
  CHECK (ts_check_syntax ("G:puts(1e309);", &error) == TS_ERR_PARSE);
  CHECK (error.offset == 7 && strstr (error.message, "out of range"));
#ifdef TS_NUMERIC_STDLIB
  CHECK (ts_stdlib_register_all (vm, &error) == TS_OK);
  locale_t selected = (locale_t)0, previous = (locale_t)0;
  for (size_t i = 0; i < sizeof locales / sizeof *locales; i++)
    if ((selected = newlocale (LC_NUMERIC_MASK, locales[i], (locale_t)0)))
      {
        previous = uselocale (selected);
        break;
      }
  if (getenv ("TS_TEST_REQUIRE_COMMA"))
    CHECK (selected && !strcmp (localeconv ()->decimal_point, ","));
  char *output = NULL;
  CHECK (ts_vm_run_file (vm, TS_NUMERIC_SOURCE, &output, &error) == TS_OK);
  CHECK (output && !strcmp (output, "finite numeric core OK\n"));
  free (output);
  if (selected)
    {
      CHECK (uselocale ((locale_t)0) == selected);
      uselocale (previous);
      freelocale (selected);
    }
#endif
  ts_vm_free (vm);
  return 0;
}
