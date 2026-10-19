#include "termscript/stdlib/ts_stdlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TS_Error error;
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

static TS_Value
str (const char *s)
{
  TS_Value v = { 0 };
  v.type = TS_STRING;
  v.as.string = (char *)s;
  return v;
}

static TS_Value
num (int64_t n)
{
  TS_Value v = { 0 };
  v.type = TS_INT;
  v.as.integer = n;
  return v;
}

static TS_Status
call (const char *name, TS_Value *a, size_t n, TS_Value *ret)
{
  const TS_FuncDef *fn = ts_module_find_func (&ts_std_termart_module, name);
  CHECK (fn);
  ts_error_clear (&error);
  return fn->func (NULL, fn->userdata, a, n, ret, &error);
}

int
main (void)
{
  TS_Value ret = { 0 }, a[4] = { str ("hi"), num (1), num (2), num (3) };
  CHECK (call ("frame", a, 1, &ret) == TS_OK);
  CHECK (!strcmp (ret.as.string, "+----+\n| hi |\n+----+"));
  a[0] = str ("界\ne\xcc\x81");
  a[1] = num (0);
  CHECK (call ("frame", a, 2, &ret) == TS_OK);
  CHECK (!strcmp (ret.as.string, "+--+\n|界|\n|e\xcc\x81 |\n+--+"));
  a[0] = str ("");
  CHECK (call ("banner", a, 1, &ret) == TS_OK && !strcmp (ret.as.string, ""));
  a[0] = str ("\nA\n");
  CHECK (call ("banner", a, 1, &ret) == TS_OK);
  CHECK (!strncmp (ret.as.string, "\n\n\n\n\n\n\n ### ", 12));
  a[0] = str ("ABC xyz 0123456789 -.!?");
  CHECK (call ("banner", a, 1, &ret) == TS_OK);
  a[0] = str ("hi");
  CHECK (call ("color", a, 4, &ret) == TS_OK);
  CHECK (!strcmp (ret.as.string, "\033[38;2;0;2;3mhi\033[0m"));
  a[1] = num (-1);
  CHECK (call ("color", a, 4, &ret) == TS_ERR_INVAL);
  a[1] = num (256);
  CHECK (call ("color", a, 4, &ret) == TS_ERR_INVAL);
  a[1] = num (-1);
  CHECK (call ("frame", a, 2, &ret) == TS_ERR_INVAL);
  a[1] = num (4097);
  CHECK (call ("center", a, 2, &ret) == TS_ERR_INVAL);
  a[0] = num (1);
  CHECK (call ("banner", a, 1, &ret) == TS_ERR_INVAL);
  a[0] = str ("@東京");
  CHECK (call ("banner", a, 1, &ret) == TS_ERR_INVAL);
  a[0] = str ("a");
  a[1] = str ("##");
  CHECK (call ("banner", a, 2, &ret) == TS_ERR_INVAL);
  a[1] = str (" ");
  CHECK (call ("banner", a, 2, &ret) == TS_ERR_INVAL);
  CHECK (call ("banner", a, 0, &ret) == TS_ERR_INVAL);
  a[0] = str ("\033[31mBAD");
  CHECK (call ("frame", a, 1, &ret) == TS_ERR_INVAL);
  a[0] = str ("bad\ttext");
  CHECK (call ("frame", a, 1, &ret) == TS_ERR_INVAL);
  a[0] = str ("\xc2\x9b");
  CHECK (call ("frame", a, 1, &ret) == TS_ERR_INVAL);
  a[0] = str ("\xff");
  CHECK (call ("frame", a, 1, &ret) != TS_OK);
  char *large = malloc (1024 * 1024 + 2);
  CHECK (large);
  memset (large, 'A', 1024 * 1024 + 1);
  large[1024 * 1024 + 1] = 0;
  a[0] = str (large);
  CHECK (call ("frame", a, 1, &ret) == TS_ERR_LIMIT);
  large[683] = 0;
  CHECK (call ("banner", a, 1, &ret) == TS_ERR_INVAL);
  /* Valid input that expands beyond the output limit; cleanup after growth. */
  for (size_t i = 0; i < 60000; i++)
    large[i] = i % 101 == 100 ? '\n' : 'A';
  large[60000] = 0;
  CHECK (call ("banner", a, 1, &ret) == TS_ERR_LIMIT);
  a[0] = str ("ok");
  CHECK (call ("frame", a, 1, &ret) == TS_OK);
  free (large);
  ts_value_free (&ret);
  return 0;
}
