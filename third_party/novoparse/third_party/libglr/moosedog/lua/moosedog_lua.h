#ifndef MOOSEDOG_LUA_H
#define MOOSEDOG_LUA_H
/* moosedog_lua.h -- Lua query files (PrettyPrint/Linter) handling. */
#include "../parser/moosedog_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Validate Queries entries: must end in .lua and (when base_dir is set)
 * exist on disk. base_dir may be NULL to skip existence checks. */
int md_lua_validate (const md_spec_t *spec, const char *base_dir,
                     md_error_t *err);

/* Minimal Lua sanity check: balanced long-bracket comments/strings and
 * balanced function/end keywords count. Returns 0 when plausible. */
int md_lua_check_syntax (const char *source, size_t len, char *err,
                         size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* MOOSEDOG_LUA_H */
