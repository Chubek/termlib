#ifndef TS_STD_HTTP_H
#define TS_STD_HTTP_H
#include <termscript/termscript.h>

extern const TS_Module ts_std_http_module;

#ifdef __cplusplus
extern "C" {
#endif
/* Native callbacks: arguments borrowed; returned record/string owns copies. */
TS_Status ts_http_fetch (TS_VM *, void *, const TS_Value *, size_t,
                         TS_Value *, TS_Error *);
TS_Status ts_http_resolve (TS_VM *, void *, const TS_Value *, size_t,
                           TS_Value *, TS_Error *);
#ifdef __cplusplus
}
#endif

#endif /* TS_STD_HTTP_H */
