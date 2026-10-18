#include "font_shape/font_shape.h"
#include "font/font.h"

/* Keep one font representation/implementation, not a second incompatible ABI.
 */
static TS_Status
forward (TS_VM *vm, void *name, const TS_Value *a, size_t n, TS_Value *r,
         TS_Error *e)
{
  const TS_FuncDef *fn = ts_module_find_func (&ts_std_font_module, name);
  return fn->func (vm, fn->userdata, a, n, r, e);
}
static const TS_FuncDef functions[]
    = { { "open", forward, (void *)"open" },
        { "match", forward, (void *)"match" },
        { "has", forward, (void *)"has" },
        { "units_per_em", forward, (void *)"units_per_em" },
        { "shape", forward, (void *)"shape" } };
const TS_Module ts_std_font_shape_module
    = TS_MODULE_INIT ("std.font_shape", functions);
