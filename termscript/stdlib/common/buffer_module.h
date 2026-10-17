/* Named storage adapters. Buffer owns the allocation and its destructor; these
 * modules reuse its validated byte-offset API, not a second storage engine. */
#ifndef TS_BUFFER_MODULE_H
#define TS_BUFFER_MODULE_H
#include "buffer/buffer.h"
#include "common/module_call.h"

static TS_Status
ts_text_create (TS_VM *vm, void *kind, const TS_Value *a, size_t n,
                TS_Value *r, TS_Error *e)
{
  if (ts_std_argc (vm, n, 0, 1, e, "new") != TS_OK)
    return TS_ERR_INVAL;
  TS_Value args[2] = { ts_arg_str (kind), n ? a[0] : ts_arg_str ("") };
  return ts_module_call (vm, &ts_std_buffer_module, "new", args, 2, r, e);
}

static TS_Status
ts_text_forward (TS_VM *vm, void *name, const TS_Value *a, size_t n,
                 TS_Value *r, TS_Error *e)
{
  return ts_module_call (vm, &ts_std_buffer_module, name, a, n, r, e);
}

#define TS_TEXT_MODULE(symbol, module_name, kind)                             \
  static const TS_FuncDef functions[]                                         \
      = { { "new", ts_text_create, (void *)kind },                            \
          { "empty", ts_text_create, (void *)kind },                          \
          { "from", ts_text_create, (void *)kind },                           \
          { "insert", ts_text_forward, (void *)"insert" },                    \
          { "delete", ts_text_forward, (void *)"delete" },                    \
          { "replace", ts_text_forward, (void *)"replace" },                  \
          { "slice", ts_text_forward, (void *)"slice" },                      \
          { "line", ts_text_forward, (void *)"line" },                        \
          { "len", ts_text_forward, (void *)"len" },                          \
          { "contents", ts_text_forward, (void *)"contents" },                \
          { "to_string", ts_text_forward, (void *)"contents" } };             \
  const TS_Module symbol = TS_MODULE_INIT (module_name, functions)
#endif
