#include "TSC-Backend.h"
static bool
available ()
{
  return true;
}
static const char *
compile (TS_VM *vm, const char *source, const TS_CompileOptions *options,
         TS_Error *error)
{
  TS_CompileOptions opts;
  if (options && options->struct_size != sizeof *options)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "unsupported compile options size");
      return nullptr;
    }
  if (options)
    opts = *options;
  else
    ts_compile_options_init (&opts);
  opts.backend = TS_BACKEND_QBE;
  return ts_vm_compile_backend (vm, source, &opts, error);
}
extern "C" const TS_BackendAPI *
ts_qbe_backend (void)
{
  static const TS_BackendAPI api = { sizeof (TS_BackendAPI),
                                     TS_BACKEND_ABI_VERSION,
                                     TS_BACKEND_QBE,
                                     "qbe",
                                     available,
                                     compile,
                                     nullptr };
  return &api;
}
