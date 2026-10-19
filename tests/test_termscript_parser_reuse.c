#include "termscript/include/Termscript.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                              \
  do                                                                          \
    {                                                                         \
      if (!(x))                                                               \
        {                                                                     \
          fprintf (stderr, "%d: %s: %s\n", __LINE__, #x, error.message);      \
          exit (1);                                                           \
        }                                                                     \
    }                                                                         \
  while (0)

static void *
exercise (void *unused)
{
  TS_Error error = { 0 };
  TS_VM *vm = ts_vm_create (&error);
  (void)unused;
  CHECK (vm);
  for (int i = 0; i < 12; i++)
    {
      char source[] = "G:puts(123);";
      char *output = NULL;
      CHECK (ts_vm_run_string (vm, source, &output, &error) == TS_OK);
      memset (source, 'x',
              sizeof source - 1); /* Parser and AST never borrow input. */
      CHECK (output && !strcmp (output, "123\n"));
      free (output);
      output = NULL;
      CHECK (ts_vm_run_string (vm, "if {", &output, &error) == TS_ERR_PARSE);
      free (output);
      output = NULL;
      CHECK (ts_vm_compile_to_c (vm, "G:puts(456);", "reuse", &error));
      TS_CompileOptions options;
      ts_compile_options_init (&options);
      options.backend = TS_BACKEND_QBE;
      CHECK (ts_vm_compile_backend (vm, "G:puts(789);", &options, &error));
      CHECK (ts_vm_run_string (vm, "G:puts(42);", &output, &error) == TS_OK);
      CHECK (output && !strcmp (output, "42\n"));
      free (output);
    }
  ts_vm_free (vm);
  return NULL;
}

int
main (void)
{
  TS_Error error = { 0 };
  pthread_t threads[4];
  CHECK (ts_check_syntax ("G:puts(1);", &error) == TS_OK);
  CHECK (ts_check_syntax ("if {", &error) == TS_ERR_PARSE);
  for (int i = 0; i < 4; i++)
    CHECK (pthread_create (&threads[i], NULL, exercise, NULL) == 0);
  for (int i = 0; i < 4; i++)
    CHECK (pthread_join (threads[i], NULL) == 0);
  return 0;
}
