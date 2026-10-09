/* moosedog_plugin.c -- dlopen-based frontend plugin loader. */
#include "moosedog_plugin.h"
#include "../../include/Moosedog-Plugin.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void
md_plugins_init (md_plugin_registry_t *reg)
{
  memset (reg, 0, sizeof (*reg));
}

void
md_plugins_destroy (md_plugin_registry_t *reg)
{
  if (!reg)
    return;
  for (size_t i = 0; i < reg->count; i++)
    {
      free (reg->items[i].name);
      free (reg->items[i].path);
      if (reg->items[i].handle)
        dlclose (reg->items[i].handle);
    }
  free (reg->items);
  memset (reg, 0, sizeof (*reg));
}

int
md_plugin_load (md_plugin_registry_t *reg, const char *path, char *err,
                size_t err_size)
{
  void *h = dlopen (path, RTLD_NOW | RTLD_LOCAL);
  md_plugin_t *np;
  if (!h)
    {
      if (err)
        snprintf (err, err_size, "dlopen %s: %s", path, dlerror ());
      return -1;
    }
  dlerror ();
  /* ISO C forbids direct object-pointer to function-pointer casts;
   * copy the bits through a union instead. */
  {
    union
    {
      void *obj;
      moosedog_plugin_entry_fn fn;
    } u;
    moosedog_plugin_entry_fn entry;
    const char *sym_err;
    u.obj = dlsym (h, "moosedog_plugin_entry");
    sym_err = dlerror ();
    entry = u.fn;
    if (!entry || sym_err)
      {
        if (err)
          snprintf (err, err_size, "%s: missing moosedog_plugin_entry", path);
        dlclose (h);
        return -1;
      }
    {
      const moosedog_plugin_t *desc = entry ();
    if (!desc || desc->abi != MOOSEDOG_PLUGIN_ABI || !desc->name)
      {
        if (err)
          snprintf (err, err_size, "%s: bad plugin descriptor", path);
        dlclose (h);
        return -1;
      }
    np = realloc (reg->items, (reg->count + 1) * sizeof (*np));
    if (!np)
      {
        dlclose (h);
        return -1;
      }
    reg->items = np;
    np = &reg->items[reg->count++];
    memset (np, 0, sizeof (*np));
    np->name = strdup (desc->name);
    np->path = strdup (path);
    np->handle = h;
    np->descriptor = desc;
    if (!np->name || !np->path)
      {
        free (np->name);
        free (np->path);
        dlclose (h);
        reg->count--;
        if (err)
          snprintf (err, err_size, "%s: out of memory", path);
        return -1;
      }
    }
  }
  return 0;
}

int
md_plugin_translate (md_plugin_registry_t *reg, const char *suffix,
                     const char *source, size_t len, char **out_grm, char *err,
                     size_t err_size)
{
  for (size_t i = 0; i < reg->count; i++)
    {
      const moosedog_plugin_t *d
          = (const moosedog_plugin_t *) reg->items[i].descriptor;
      if (d && d->input_suffix && suffix
          && !strcmp (d->input_suffix, suffix))
        return d->translate (source, len, out_grm, err, err_size);
    }
  if (err)
    snprintf (err, err_size, "no plugin handles \"%s\"",
              suffix ? suffix : "(none)");
  return -1;
}
