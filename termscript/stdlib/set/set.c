/* Insertion-ordered string sets, O(n) lookup, maximum 65536 elements. */
#include "set/set.h"
#include "common/ts_std_common.h"
#include <stdlib.h>
#include <string.h>
typedef struct
{
  char **items;
  size_t size, capacity;
} string_set;
static const char tag;
static void
clear (string_set *set)
{
  for (size_t i = 0; i < set->size; i++)
    free (set->items[i]);
  set->size = 0;
}
static void
destroy (void *ptr)
{
  string_set *set = ptr;
  clear (set);
  free (set->items);
  free (set);
}
static TS_Status
create (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r,
        TS_Error *e)
{
  (void)u;
  (void)a;
  if (ts_std_argc (vm, n, 0, 0, e, "new"))
    return TS_ERR_INVAL;
  string_set *set = calloc (1, sizeof *set);
  if (!set)
    return ts_std_oom (e);
  if (ts_value_make_handle (r, set, destroy, "set", &tag))
    {
      destroy (set);
      return ts_std_oom (e);
    }
  return TS_OK;
}
static TS_Status
operation (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r,
           TS_Error *e)
{
  intptr_t op = (intptr_t)u;
  if (ts_std_argc (vm, n, op < 3 ? 2 : 1, op < 3 ? 2 : 1, e, "set"))
    return TS_ERR_INVAL;
  string_set *set = ts_std_handle (a, &tag, e, "set");
  if (!set)
    return TS_ERR_INVAL;
  if (op == 3)
    {
      ts_std_ret_int (r, (int64_t)set->size);
      return TS_OK;
    }
  if (op == 4)
    {
      clear (set);
      return TS_OK;
    }
  if (op == 5)
    {
      ts_std_list_t *list = ts_std_list_new ();
      if (!list)
        return ts_std_oom (e);
      for (size_t i = 0; i < set->size; i++)
        if (ts_std_list_push_str (list, set->items[i]))
          {
            ts_std_list_free (list);
            return ts_std_oom (e);
          }
      return ts_std_list_wrap (r, list, e);
    }
  const char *item;
  if (ts_std_str (a + 1, &item, e, "set"))
    return TS_ERR_INVAL;
  size_t index = 0;
  while (index < set->size && strcmp (set->items[index], item))
    index++;
  bool found = index < set->size;
  if (op == 0 && !found)
    {
      if (set->size == 65536)
        {
          ts_error_set (e, TS_ERR_LIMIT, 0, 0, "set size limit exceeded");
          return TS_ERR_LIMIT;
        }
      if (set->size == set->capacity)
        {
          size_t capacity = set->capacity ? set->capacity * 2 : 8;
          char **items = realloc (set->items, capacity * sizeof *items);
          if (!items)
            return ts_std_oom (e);
          set->items = items;
          set->capacity = capacity;
        }
      char *copy = strdup (item);
      if (!copy)
        return ts_std_oom (e);
      set->items[set->size++] = copy;
    }
  else if (op == 2 && found)
    {
      free (set->items[index]);
      memmove (set->items + index, set->items + index + 1,
               (set->size - index - 1) * sizeof *set->items);
      set->size--;
    }
  ts_std_ret_bool (r, op == 0 ? !found : found);
  return TS_OK;
}
static const TS_FuncDef functions[]
    = { { "new", create, NULL },           { "add", operation, (void *)0 },
        { "has", operation, (void *)1 },   { "remove", operation, (void *)2 },
        { "len", operation, (void *)3 },   { "clear", operation, (void *)4 },
        { "values", operation, (void *)5 } };
const TS_Module ts_std_set_module = TS_MODULE_INIT ("std.set", functions);
