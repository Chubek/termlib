#define _GNU_SOURCE
#include "termscript/stdlib/json/json.h"
#include "termscript/stdlib/toml/toml.h"
#include "termscript/stdlib/yaml/yaml.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t calls, fail_at;
static int active;
void *__real_malloc (size_t);
void *__real_calloc (size_t, size_t);
void *__real_realloc (void *, size_t);
char *__real_strdup (const char *);
char *__real_strndup (const char *, size_t);
static int
fail (void)
{
  return active && ++calls == fail_at;
}
void *
__wrap_malloc (size_t n)
{
  if (fail ())
    {
      errno = ENOMEM;
      return NULL;
    }
  return __real_malloc (n);
}
void *
__wrap_calloc (size_t n, size_t s)
{
  if (fail ())
    {
      errno = ENOMEM;
      return NULL;
    }
  return __real_calloc (n, s);
}
void *
__wrap_realloc (void *p, size_t n)
{
  if (fail ())
    {
      errno = ENOMEM;
      return NULL;
    }
  return __real_realloc (p, n);
}
char *
__wrap_strdup (const char *s)
{
  if (fail ())
    {
      errno = ENOMEM;
      return NULL;
    }
  return __real_strdup (s);
}
char *
__wrap_strndup (const char *s, size_t n)
{
  if (fail ())
    {
      errno = ENOMEM;
      return NULL;
    }
  return __real_strndup (s, n);
}
#define CHECK(x)                                                              \
  do                                                                          \
    {                                                                         \
      if (!(x))                                                               \
        {                                                                     \
          fprintf (stderr, "%d: %s (%s)\n", __LINE__, #x, error.message);     \
          exit (1);                                                           \
        }                                                                     \
    }                                                                         \
  while (0)

static void
exercise (const TS_Module *module, const char *source)
{
  TS_Error error = { 0 };
  const TS_FuncDef *parse = ts_module_find_func (module, "parse");
  CHECK (parse);
  TS_Value argument = { 0 }, value = { 0 };
  CHECK (ts_value_make_string (&argument, source) == TS_OK);
  calls = 0;
  fail_at = 0;
  active = 1;
  TS_Status status
      = parse->func (NULL, parse->userdata, &argument, 1, &value, &error);
  active = 0;
  CHECK (status == TS_OK && value.type == TS_HANDLE);
  size_t count = calls;
  ts_value_free (&value);
  for (size_t fault = 1; fault <= count; fault++)
    {
      ts_error_clear (&error);
      calls = 0;
      fail_at = fault;
      active = 1;
      status
          = parse->func (NULL, parse->userdata, &argument, 1, &value, &error);
      active = 0;
      if (status != TS_ERR_NOMEM)
        fprintf (stderr, "%s allocation %zu/%zu returned %d\n", module->name,
                 fault, count, status);
      CHECK (status == TS_ERR_NOMEM && error.code == TS_ERR_NOMEM);
      CHECK (value.type == TS_NIL);
      CHECK (parse->func (NULL, parse->userdata, &argument, 1, &value, &error)
             == TS_OK);
      CHECK (value.type == TS_HANDLE);
      ts_value_free (&value);
    }
  ts_value_free (&argument);
}

int
main (void)
{
  exercise (
      &ts_std_json_module,
      "{\"ratio\":1.25,\"values\":[1.0,2.5],\"label\":\"\\uD83D\\uDE00\"}");
  exercise (&ts_std_toml_module, "ratio = 1.25\nvalues = [1.0, 2.5]\nlabel = "
                                 "\"text\"\nstrings = [\"one\", \"two\"]\n");
  exercise (&ts_std_yaml_module,
            "ratio: 1.25\nvalues:\n  - 1.0\n  - 2.5\nlabel: "
            "\"quoted\\ntext\"\nsingle: 'it''s text'\n");
  return 0;
}
