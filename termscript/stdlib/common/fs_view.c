#include "common/fs_view.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum
{
  FS_LIMIT = 1048576,
  FS_ENTRIES = 65536,
  FS_PATH = 4096
};
typedef struct
{
  int fd;
} fs_view;
static const char view_tag;

static void
destroy (void *ptr)
{
  fs_view *view = ptr;
  if (view->fd >= 0)
    close (view->fd);
  free (view);
}

static TS_Status
system_error (TS_Error *error, const char *operation)
{
  int saved = errno;
  TS_Status status = saved == ENOENT || saved == ENOTDIR ? TS_ERR_NOTFOUND
                     : saved == EAGAIN || saved == EWOULDBLOCK
                         ? TS_ERR_WOULD_BLOCK
                         : TS_ERR_SYSTEM;
  ts_error_set (error, status, saved, 0, "%s: %s", operation,
                strerror (saved));
  return status;
}

static fs_view *
unwrap (const TS_Value *value, TS_Error *error)
{
  fs_view *view = ts_std_handle (value, &view_tag, error, "filesystem view");
  if (view && view->fd < 0)
    {
      ts_std_inval (error, "filesystem view is closed");
      return NULL;
    }
  return view;
}

TS_Status
ts_fs_open (TS_VM *vm, void *default_root, const TS_Value *args, size_t count,
            TS_Value *ret, TS_Error *error)
{
  const char *root = default_root;
  if (ts_std_argc (vm, count, 0, 1, error, "open")
      || (count && ts_std_str (args, &root, error, "root")))
    return TS_ERR_INVAL;
  /* A trailing slash makes the OS follow a final symlink before applying
   * O_NOFOLLOW. Remove it so the documented final-component rule holds. */
  char *normalized = strdup (root);
  if (!normalized)
    return ts_std_oom (error);
  size_t length = strlen (normalized);
  while (length > 1 && normalized[length - 1] == '/')
    normalized[--length] = 0;
  int fd;
  do
    {
      fd = open (normalized, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
  while (fd < 0 && errno == EINTR);
  int saved = errno;
  free (normalized);
  errno = saved;
  if (fd < 0)
    return system_error (error, "open root");
  fs_view *view = malloc (sizeof *view);
  if (!view)
    {
      close (fd);
      return ts_std_oom (error);
    }
  view->fd = fd;
  TS_Status status = ts_value_make_handle (
      ret, view, destroy, "read-only filesystem view", &view_tag);
  if (status != TS_OK)
    {
      destroy (view);
      return ts_std_oom (error);
    }
  return TS_OK;
}

/* Validate the entire relative path before opening anything. Each component
 * is opened relative to an already owned directory, never via realpath/access.
 * Symlinks are rejected at every traversal step, including the final entry. */
static TS_Status
open_relative (fs_view *view, const char *path, bool directory, int *out,
               TS_Error *error)
{
  size_t length = strlen (path);
  if (length > FS_PATH)
    {
      ts_error_set (error, TS_ERR_LIMIT, 0, 0, "path exceeds 4096 bytes");
      return TS_ERR_LIMIT;
    }
  if (*path == '/')
    return ts_std_inval (error, "path must be relative");
  for (const char *p = path; *p;)
    {
      const char *end = strchr (p, '/');
      size_t size = end ? (size_t)(end - p) : strlen (p);
      if (!size || (size == 1 && p[0] == '.')
          || (size == 2 && !memcmp (p, "..", 2)) || (end && !end[1]))
        return ts_std_inval (
            error, "empty, dot and parent path components are forbidden");
      p += size;
      if (*p)
        p++;
    }
  int fd;
  do
    {
      fd = openat (view->fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }
  while (fd < 0 && errno == EINTR);
  if (fd < 0)
    return system_error (error, "duplicate root");
  if (!length)
    {
      if (!directory)
        {
          close (fd);
          return ts_std_inval (error, "read requires a file path");
        }
      *out = fd;
      return TS_OK;
    }
  char copy[FS_PATH + 1];
  memcpy (copy, path, length + 1);
  char *part = copy;
  while (*part)
    {
      char *end = strchr (part, '/');
      if (end)
        *end = 0;
      int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
      if (end || directory)
        flags |= O_DIRECTORY;
      int next;
      do
        {
          next = openat (fd, part, flags);
        }
      while (next < 0 && errno == EINTR);
      int saved = errno;
      close (fd);
      errno = saved;
      if (next < 0)
        return system_error (error, "open relative path");
      fd = next;
      if (!end)
        break;
      part = end + 1;
    }
  *out = fd;
  return TS_OK;
}

TS_Status
ts_fs_read (TS_VM *vm, void *unused, const TS_Value *args, size_t count,
            TS_Value *ret, TS_Error *error)
{
  (void)unused;
  const char *path;
  int64_t limit = 65536;
  if (ts_std_argc (vm, count, 2, 3, error, "read")
      || ts_std_str (args + 1, &path, error, "path")
      || (count == 3 && ts_std_int (args + 2, &limit, error, "limit")))
    return TS_ERR_INVAL;
  if (limit < 1 || limit > FS_LIMIT)
    return ts_std_inval (error, "read limit must be in [1, 1048576]");
  fs_view *view = unwrap (args, error);
  if (!view)
    return TS_ERR_INVAL;
  int fd;
  TS_Status status = open_relative (view, path, false, &fd, error);
  if (status != TS_OK)
    return status;
  struct stat info;
  if (fstat (fd, &info) < 0)
    {
      int saved = errno;
      close (fd);
      errno = saved;
      return system_error (error, "stat file");
    }
  if (!S_ISREG (info.st_mode))
    {
      close (fd);
      return ts_std_inval (
          error, "only regular and regular-like virtual files may be read");
    }
  char *buffer = malloc ((size_t)limit + 1);
  if (!buffer)
    {
      close (fd);
      return ts_std_oom (error);
    }
  size_t used = 0;
  while (used <= (size_t)limit)
    {
      ssize_t size = read (fd, buffer + used, (size_t)limit + 1 - used);
      if (size < 0 && errno == EINTR)
        continue;
      if (size < 0)
        {
          status = system_error (error, "read file");
          break;
        }
      if (!size)
        break;
      used += (size_t)size;
      if (used > (size_t)limit)
        {
          ts_error_set (error, TS_ERR_LIMIT, 0, used,
                        "file exceeds read limit");
          status = TS_ERR_LIMIT;
          break;
        }
    }
  close (fd);
  if (status == TS_OK && memchr (buffer, 0, used))
    {
      ts_error_set (error, TS_ERR_UNSUPPORTED, 0, 0, "NUL in text file");
      status = TS_ERR_UNSUPPORTED;
    }
  if (status == TS_OK)
    status = ts_std_ret_strn (ret, buffer, used, error);
  free (buffer);
  return status;
}

static int
compare_names (const void *a, const void *b)
{
  return strcmp (*(char *const *)a, *(char *const *)b);
}

TS_Status
ts_fs_list (TS_VM *vm, void *unused, const TS_Value *args, size_t count,
            TS_Value *ret, TS_Error *error)
{
  (void)unused;
  const char *path = "";
  int64_t limit = 4096;
  if (ts_std_argc (vm, count, 1, 3, error, "list")
      || (count >= 2 && ts_std_str (args + 1, &path, error, "path"))
      || (count == 3 && ts_std_int (args + 2, &limit, error, "limit")))
    return TS_ERR_INVAL;
  if (limit < 1 || limit > FS_ENTRIES)
    return ts_std_inval (error, "entry limit must be in [1, 65536]");
  fs_view *view = unwrap (args, error);
  if (!view)
    return TS_ERR_INVAL;
  int fd;
  TS_Status status = open_relative (view, path, true, &fd, error);
  if (status != TS_OK)
    return status;
  DIR *directory = fdopendir (fd);
  if (!directory)
    {
      int saved = errno;
      close (fd);
      errno = saved;
      return system_error (error, "open directory stream");
    }
  char **names = calloc ((size_t)limit, sizeof *names);
  if (!names)
    {
      closedir (directory);
      return ts_std_oom (error);
    }
  size_t size = 0, bytes = 0;
  for (;;)
    {
      errno = 0;
      struct dirent *entry = readdir (directory);
      if (!entry)
        {
          if (errno == EINTR)
            continue;
          if (errno)
            status = system_error (error, "read directory");
          break;
        }
      if (!strcmp (entry->d_name, ".") || !strcmp (entry->d_name, ".."))
        continue;
      size_t length = strlen (entry->d_name) + 1;
      if (size == (size_t)limit || length > FS_LIMIT - bytes)
        {
          ts_error_set (error, TS_ERR_LIMIT, 0, 0,
                        "directory listing limit exceeded");
          status = TS_ERR_LIMIT;
          break;
        }
      names[size] = strdup (entry->d_name);
      if (!names[size])
        {
          status = ts_std_oom (error);
          break;
        }
      size++;
      bytes += length;
    }
  closedir (directory);
  ts_std_list_t *list = NULL;
  if (status == TS_OK)
    {
      qsort (names, size, sizeof *names, compare_names);
      list = ts_std_list_new ();
      if (!list)
        status = ts_std_oom (error);
      for (size_t i = 0; status == TS_OK && i < size; i++)
        if (ts_std_list_push_str (list, names[i]) != TS_OK)
          status = ts_std_oom (error);
    }
  for (size_t i = 0; i < size; i++)
    free (names[i]);
  free (names);
  if (status != TS_OK)
    {
      ts_std_list_free (list);
      return status;
    }
  return ts_std_list_wrap (ret, list, error);
}

TS_Status
ts_fs_close (TS_VM *vm, void *unused, const TS_Value *args, size_t count,
             TS_Value *ret, TS_Error *error)
{
  (void)unused;
  (void)ret;
  if (ts_std_argc (vm, count, 1, 1, error, "close"))
    return TS_ERR_INVAL;
  fs_view *view = ts_std_handle (args, &view_tag, error, "filesystem view");
  if (!view)
    return TS_ERR_INVAL;
  int fd = view->fd;
  view->fd = -1;
  if (fd >= 0 && close (fd) < 0)
    return system_error (error, "close root");
  return TS_OK;
}
