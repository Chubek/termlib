#ifndef MOOSEDOG_CONFUSE_H
#define MOOSEDOG_CONFUSE_H
/* moosedog_confuse.h -- configuration values and ${VAR} expansion. */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Expand ${VAR} and $VAR against environment (unknown -> ""). */
int md_conf_expand (const char *in, char *out, size_t out_size);

/* Resolve CacheDirectory-style values: expand env, then expand
 * ${TMPDIR} default via P_tmpdir when TMPDIR is unset. */
int md_conf_resolve_dir (const char *raw, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* MOOSEDOG_CONFUSE_H */
