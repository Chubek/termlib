#include "termscript/stdlib/mqueue/mqueue.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x)                                                              \
  do                                                                          \
    {                                                                         \
      if (!(x))                                                               \
        {                                                                     \
          fprintf (stderr, "%d: %s\n", __LINE__, #x);                         \
          exit (1);                                                           \
        }                                                                     \
    }                                                                         \
  while (0)

int
main (void)
{
  TS_Error error = { 0 };
  TS_Value ret = { 0 };
  CHECK (ts_module_validate (&ts_std_mqueue_module, &error) == TS_OK);
  const TS_FuncDef *available
      = ts_module_find_func (&ts_std_mqueue_module, "available");
  CHECK (available);
  CHECK (available->func (NULL, NULL, NULL, 0, &ret, &error) == TS_OK);
  CHECK (ret.type == TS_BOOL && !ret.as.boolean);
  ts_value_free (&ret);
  const char *operations[] = { "create", "open", "unlink", "close", "len",
                               "send", "receive", "message_size" };
  for (size_t i = 0; i < sizeof operations / sizeof *operations; i++)
    {
      const TS_FuncDef *fn
          = ts_module_find_func (&ts_std_mqueue_module, operations[i]);
      CHECK (fn);
      ts_error_clear (&error);
      CHECK (fn->func (NULL, NULL, NULL, 0, &ret, &error)
             == TS_ERR_UNSUPPORTED);
      CHECK (error.code == TS_ERR_UNSUPPORTED && error.message[0]);
      CHECK (ret.type == TS_NIL);
    }
  return 0;
}
