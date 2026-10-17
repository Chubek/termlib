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
  opts.backend = TS_BACKEND_C;
  return ts_vm_compile_backend (vm, source, &opts, error);
}
static const TS_BackendAPI c_backend = { sizeof (TS_BackendAPI),
                                         TS_BACKEND_ABI_VERSION,
                                         TS_BACKEND_C,
                                         "c",
                                         available,
                                         compile,
                                         nullptr };
static const TS_BackendAPI vm_backend = { sizeof (TS_BackendAPI),
                                          TS_BACKEND_ABI_VERSION,
                                          TS_BACKEND_VM,
                                          "vm",
                                          available,
                                          nullptr,
                                          ts_vm_run_string };
extern "C" const TS_BackendAPI *ts_qbe_backend (void);
static bool
jit_available ()
{
  return ts_backend_available (TS_BACKEND_JITERATI);
}
static TS_Status
jit_run (TS_VM *vm, const char *source, char **out, TS_Error *e)
{
  return ts_vm_run_backend (vm, source, TS_BACKEND_JITERATI, out, e);
}
static const TS_BackendAPI jit_backend = { sizeof (TS_BackendAPI),
                                           TS_BACKEND_ABI_VERSION,
                                           TS_BACKEND_JITERATI,
                                           "jiterati",
                                           jit_available,
                                           nullptr,
                                           jit_run };
extern "C" const TS_BackendAPI *
ts_backend_get (TS_Backend backend)
{
  switch (backend)
    {
    case TS_BACKEND_VM:
      return &vm_backend;
    case TS_BACKEND_C:
      return &c_backend;
    case TS_BACKEND_QBE:
      return ts_qbe_backend ();
    case TS_BACKEND_JITERATI:
      return &jit_backend;
    default:
      return nullptr;
    }
}
