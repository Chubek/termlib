/* Backend contract. Descriptors are immutable, borrowed process-lifetime data.
 * All calls borrow source/options; emitted text belongs to the VM until its
 * next compile or destruction. Execution output is allocated (caller frees).
 * A NULL callback denotes an unsupported operation, not a successful no-op.
 * A VM must not be used concurrently, including through different backends.
 */
#ifndef TSC_BACKEND_H
#define TSC_BACKEND_H
#include "TSC-API.h"
#ifdef __cplusplus
extern "C"
{
#endif
#define TS_BACKEND_ABI_VERSION 1u
  typedef struct
  {
    uint32_t struct_size;
    uint32_t abi_version;
    TS_Backend id;
    const char *name;
    bool (*available) (void);
    const char *(*compile) (TS_VM *, const char *, const TS_CompileOptions *,
                            TS_Error *);
    TS_Status (*run) (TS_VM *, const char *, char **, TS_Error *);
  } TS_BackendAPI;
  /* NULL for an unknown backend. VM is always available; optional JIT support
   * depends on the build. QBE availability means IL emission, not a linker. */
  const TS_BackendAPI *ts_backend_get (TS_Backend backend);
#ifdef __cplusplus
}
#endif
#endif
