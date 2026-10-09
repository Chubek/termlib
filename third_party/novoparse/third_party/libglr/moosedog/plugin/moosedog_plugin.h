#ifndef MOOSEDOG_PLUGIN_CORE_H
#define MOOSEDOG_PLUGIN_CORE_H
/* moosedog_plugin.h -- plugin registry and loader. */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct md_plugin md_plugin_t;
struct md_plugin
{
  char *name;
  char *path;
  void *handle;
  const void *descriptor; /* borrowed moosedog_plugin_t */
};

typedef struct md_plugin_registry
{
  md_plugin_t *items;
  size_t count;
} md_plugin_registry_t;

void md_plugins_init (md_plugin_registry_t *reg);
void md_plugins_destroy (md_plugin_registry_t *reg);

/* Load a shared object exposing moosedog_plugin_entry. */
int md_plugin_load (md_plugin_registry_t *reg, const char *path, char *err,
                    size_t err_size);

/* Translate source text with the plugin registered for suffix (e.g. ".ebnf").
 * Returns 0 on success with malloc'd *out_grm; -1 when no plugin matches. */
int md_plugin_translate (md_plugin_registry_t *reg, const char *suffix,
                         const char *source, size_t len, char **out_grm,
                         char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* MOOSEDOG_PLUGIN_CORE_H */
