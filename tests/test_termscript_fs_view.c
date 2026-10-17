#define _GNU_SOURCE
#include "termscript/stdlib/common/ts_std_common.h"
#include "termscript/stdlib/ts_stdlib.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static TS_Error error;
#define CHECK(x)                                                              \
  do                                                                          \
    {                                                                         \
      if (!(x))                                                               \
        {                                                                     \
          fprintf (stderr, "%d: %s: %s\n", __LINE__, #x, error.message);      \
          exit (1);                                                           \
        }                                                                     \
    }                                                                         \
  while (0)
static TS_Value
text (const char *s)
{
  TS_Value v = { 0 };
  v.type = TS_STRING;
  v.as.string = (char *)s;
  return v;
}
static TS_Value
number (int64_t n)
{
  TS_Value v = { 0 };
  v.type = TS_INT;
  v.as.integer = n;
  return v;
}
static TS_Status
call (const TS_Module *module, const char *name, TS_Value *ret, size_t n,
      const TS_Value *a)
{
  ts_value_free (ret);
  ts_error_clear (&error);
  const TS_FuncDef *f = ts_module_find_func (module, name);
  CHECK (f);
  return f->func (NULL, f->userdata, a, n, ret, &error);
}
static int
descriptors (void)
{
  int count = 0;
  for (int fd = 0; fd < 256; fd++)
    if (fcntl (fd, F_GETFD) >= 0)
      count++;
  return count;
}
static void
fixture (const char *name, const void *bytes, size_t length)
{
  FILE *file = fopen (name, "wb");
  CHECK (file);
  CHECK (fwrite (bytes, 1, length, file) == length);
  CHECK (fclose (file) == 0);
}
static void
exercise (const TS_Module *module, const char *root)
{
  int before = descriptors ();
  TS_Value view = { 0 }, alias = { 0 }, ret = { 0 }, path = text (root);
  CHECK (call (module, "open", &view, 1, &path) == TS_OK);
  CHECK (ts_value_copy (&alias, &view) == TS_OK);
  ts_value_free (&view);
  TS_Value read_args[] = { alias, text ("nested/value"), number (5) };
  CHECK (call (module, "read", &ret, 3, read_args) == TS_OK);
  CHECK (ret.type == TS_STRING && !strcmp (ret.as.string, "value"));
  read_args[2] = number (4);
  CHECK (call (module, "read", &ret, 3, read_args) == TS_ERR_LIMIT);
  read_args[2] = number (0);
  CHECK (call (module, "read", &ret, 3, read_args) == TS_ERR_INVAL);
  read_args[1] = text ("a");
  CHECK (call (module, "read", &ret, 2, read_args) == TS_OK);
  CHECK (ret.type == TS_STRING && !strcmp (ret.as.string, ""));
  char long_path[4098];
  memset (long_path, 'a', sizeof long_path - 1);
  long_path[sizeof long_path - 1] = 0;
  read_args[1] = text (long_path);
  CHECK (call (module, "read", &ret, 2, read_args) == TS_ERR_LIMIT);
  const char *bad[]
      = { "../a", "nested/../a", "/a", "nested//value", "./a", "nested/" };
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
    {
      read_args[1] = text (bad[i]);
      CHECK (call (module, "read", &ret, 2, read_args) == TS_ERR_INVAL);
    }
  read_args[1] = text ("missing");
  CHECK (call (module, "read", &ret, 2, read_args) == TS_ERR_NOTFOUND);
  CHECK (error.system_errno != 0);
  read_args[1] = text ("nul");
  CHECK (call (module, "read", &ret, 2, read_args) == TS_ERR_UNSUPPORTED);
  read_args[1] = text ("pipe");
  CHECK (call (module, "read", &ret, 2, read_args) == TS_ERR_INVAL);
  read_args[1] = text ("filelink");
  CHECK (call (module, "read", &ret, 2, read_args) != TS_OK);
  read_args[1] = text ("link/value");
  CHECK (call (module, "read", &ret, 2, read_args) != TS_OK);
  for (int i = 0; i < 2; i++)
    {
      CHECK (call (module, "list", &ret, 1, &alias) == TS_OK);
      ts_std_list_t *list = ts_std_list_unwrap (&ret, &error, "listing");
      CHECK (list);
      CHECK (ts_std_list_len (list) == 7);
      for (size_t j = 1; j < ts_std_list_len (list); j++)
        CHECK (strcmp (ts_std_list_get (list, j - 1)->as.string,
                       ts_std_list_get (list, j)->as.string)
               < 0);
    }
  TS_Value list_args[] = { alias, text (""), number (1) };
  CHECK (call (module, "list", &ret, 3, list_args) == TS_ERR_LIMIT);
  for (int i = 0; i < 64; i++)
    {
      read_args[1] = text ("link/value");
      CHECK (call (module, "read", &ret, 2, read_args) != TS_OK);
      CHECK (call (module, "list", &ret, 3, list_args) == TS_ERR_LIMIT);
    }
  CHECK (call (module, "close", &ret, 1, &alias) == TS_OK);
  CHECK (call (module, "close", &ret, 1, &alias) == TS_OK);
  CHECK (call (module, "read", &ret, 2, read_args) == TS_ERR_INVAL);
  ts_value_free (&alias);
  ts_value_free (&ret);
  CHECK (call (module, "open", &view, 1, &path) == TS_OK);
  ts_value_free (&view); /* Destruction without explicit close owns cleanup. */
  path = text ("missing-root");
  CHECK (call (module, "open", &view, 1, &path) == TS_ERR_NOTFOUND);
  path = text ("link");
  CHECK (call (module, "open", &view, 1, &path) != TS_OK);
  path = text ("link/");
  CHECK (call (module, "open", &view, 1, &path) != TS_OK);
  CHECK (descriptors () == before);
}
int
main (void)
{
  char root[] = "/tmp/opencode/ts-fs-XXXXXX";
  CHECK (mkdtemp (root));
  int cwd = open (".", O_RDONLY | O_DIRECTORY);
  CHECK (cwd >= 0);
  CHECK (chdir (root) == 0);
  CHECK (mkdir ("nested", 0700) == 0);
  fixture ("nested/value", "value", 5);
  fixture ("a", "", 0);
  fixture ("z", "last", 4);
  fixture ("nul", "a\0b", 3);
  CHECK (mkfifo ("pipe", 0600) == 0);
  CHECK (symlink ("nested", "link") == 0);
  CHECK (symlink ("z", "filelink") == 0);
  exercise (&ts_std_procfs_module, root);
  exercise (&ts_std_sysfs_module, root);
  CHECK (unlink ("nested/value") == 0);
  CHECK (rmdir ("nested") == 0);
  const char *files[] = { "a", "z", "nul", "pipe", "link", "filelink" };
  for (size_t i = 0; i < sizeof files / sizeof *files; i++)
    CHECK (unlink (files[i]) == 0);
  CHECK (fchdir (cwd) == 0);
  close (cwd);
  CHECK (rmdir (root) == 0);
  return 0;
}
