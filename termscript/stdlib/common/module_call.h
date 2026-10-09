/* Private calls between standard modules. Arguments are borrowed; result owned.
 * These calls do not re-enter the parser or charge additional script steps. */
#ifndef TS_STD_MODULE_CALL_H
#define TS_STD_MODULE_CALL_H
#include "common/ts_std_common.h"
#include <string.h>
static inline TS_Value ts_arg_int (int64_t x)
{ TS_Value v = {0}; v.type = TS_INT; v.as.integer = x; return v; }
static inline TS_Value ts_arg_str (const char *s)
{ TS_Value v = {0}; v.type = TS_STRING; v.as.string = (char *) s; return v; }
static inline TS_Status
ts_module_call (TS_VM *vm, const TS_Module *m, const char *name,
                const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  ts_value_free (r);
  for (const TS_FuncDef *f = m->funcs; f->name; f++)
    if (!strcmp (f->name, name)) return f->func (vm, f->userdata, a, n, r, e);
  return ts_std_inval (e, "internal module entry not found");
}
static inline int64_t ts_doc_int (const ts_dom_t *d, const char *key)
{
  TS_Value v = {0}; int64_t x = 0;
  const ts_dom_t *field = ts_dom_path (d, key);
  if (field && !ts_dom_to_value (field, &v, NULL) && v.type == TS_INT) x = v.as.integer;
  ts_value_free (&v); return x;
}
#endif
