#define _POSIX_C_SOURCE 200809L
#include "termscript/include/Termscript.h"
#include "termscript/stdlib/ts_stdlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int
main (void)
{
  TS_Error error = { 0 };
  TS_VM *vm = ts_vm_create (&error);
  CHECK (vm && ts_stdlib_register_all (vm, &error) == TS_OK);
  FILE *capture = tmpfile ();
  CHECK (capture);
  int saved = dup (STDOUT_FILENO);
  CHECK (saved >= 0);
  CHECK (fflush (stdout) == 0);
  CHECK (dup2 (fileno (capture), STDOUT_FILENO) >= 0);
  char *output = NULL;
  CHECK (ts_vm_run_string (vm,
                           "const S = G:load(\"std.stdio\");\n"
                           "G:puts(S:write_hex(\"\"));\n"
                           "G:puts(S:write_hex(\"0061Ff0a\")); S:flush();\n",
                           &output, &error)
         == TS_OK);
  CHECK (output && !strcmp (output, "0\n4\n"));
  free (output);
  output = NULL;
  const char *invalid[] = { "S:write_hex(\"0\");", "S:write_hex(\"00zz\");",
                            "S:write_hex(123);" };
  for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
    {
      CHECK (ts_vm_run_string (vm, invalid[i], &output, &error)
             == TS_ERR_INVAL);
      free (output);
      output = NULL;
    }
  CHECK (fflush (stdout) == 0);
  CHECK (dup2 (saved, STDOUT_FILENO) >= 0);
  close (saved);
  CHECK (fseek (capture, 0, SEEK_SET) == 0);
  unsigned char bytes[5];
  CHECK (fread (bytes, 1, sizeof bytes, capture) == 4);
  CHECK (!memcmp (bytes, "\0a\xff\n", 4));
  fclose (capture);
  ts_vm_free (vm);
  return 0;
}
