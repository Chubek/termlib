#include "termscript/stdlib/wayland/wayland.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf (stderr, "%d: %s\n", __LINE__, #x); exit (1); } } while (0)
int main (void)
{
  TS_Value r = {0}; TS_Error error = {0};
  CHECK (ts_module_validate (&ts_std_wayland_module, &error) == TS_OK);
  for (size_t i = 0; i < ts_std_wayland_module.func_count; i++)
    {
      const TS_FuncDef *fn = ts_std_wayland_module.funcs + i;
      TS_Status st = fn->func (NULL, fn->userdata, NULL, 0, &r, &error);
      if (!i) CHECK (st == TS_OK && r.type == TS_BOOL && !r.as.boolean);
      else CHECK (st == TS_ERR_UNSUPPORTED && error.code == TS_ERR_UNSUPPORTED);
      ts_value_free (&r);
    }
  return 0;
}
