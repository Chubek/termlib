#define _POSIX_C_SOURCE 200809L
#include "termscript/stdlib/ts_stdlib.h"
#include "termscript/stdlib/common/ts_std_common.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef TS_TEST_HAVE_MQUEUE
#include <mqueue.h>
#endif

static TS_Error error;
static char shared_name[128], queue_name[128];
static void cleanup (void)
{
  if (shared_name[0]) shm_unlink (shared_name);
#ifdef TS_TEST_HAVE_MQUEUE
  if (queue_name[0]) mq_unlink (queue_name);
#endif
}
#define CHECK(x) do { if (!(x)) { fprintf (stderr, "%d: %s (%s)\n", __LINE__, #x, error.message); exit (1); } } while (0)
static TS_Value integer (int64_t i) { TS_Value v = { .type = TS_INT, .as.integer = i }; return v; }
static TS_Value string (const char *s) { TS_Value v = { .type = TS_STRING, .as.string = (char *)s }; return v; }
static TS_Status call (const TS_Module *m, const char *name, TS_Value *r, size_t n, TS_Value *a)
{
  ts_value_free (r); ts_error_clear (&error);
  const TS_FuncDef *f = ts_module_find_func (m, name); CHECK (f);
  return f->func (NULL, f->userdata, a, n, r, &error);
}
#define SHM(name,r,n,a) call (&ts_std_shm_module, name, r, n, a)
#define MQ(name,r,n,a) call (&ts_std_mqueue_module, name, r, n, a)

static int descriptor_count (void)
{
  int count = 0;
  for (int fd = 0; fd < 1024; fd++) if (fcntl (fd, F_GETFD) >= 0) count++;
  return count;
}

int main (void)
{
  snprintf (shared_name, sizeof shared_name, "/termscript-test-shm-%ld", (long)getpid ());
  snprintf (queue_name, sizeof queue_name, "/termscript-test-mq-%ld", (long)getpid ());
  atexit (cleanup);
  TS_Value shared = {0}, other = {0}, alias = {0}, r = {0};
  TS_Value a[] = { string (shared_name), integer (8), integer (0) };
  CHECK (SHM ("create", &shared, 2, a) == TS_OK);
  CHECK (SHM ("create", &r, 2, a) == TS_ERR_SYSTEM && error.system_errno == EEXIST);
  CHECK (SHM ("open", &other, 1, a) == TS_OK);
  CHECK (SHM ("len", &r, 1, &other) == TS_OK && r.as.integer == 8);
  CHECK (SHM ("fd", &r, 1, &other) == TS_OK);
  int fd = (int)r.as.integer;
  CHECK (fcntl (fd, F_GETFD) & FD_CLOEXEC);
  struct stat info; CHECK (!fstat (fd, &info) && (info.st_mode & 0777) == 0600);
  TS_Value access[] = { shared, integer (7), integer (255) };
  CHECK (SHM ("get", &r, 2, access) == TS_OK && r.as.integer == 0);
  CHECK (SHM ("set", &r, 3, access) == TS_OK);
  access[0] = other;
  CHECK (SHM ("get", &r, 2, access) == TS_OK && r.as.integer == 255);
  access[1] = integer (8); CHECK (SHM ("get", &r, 2, access) == TS_ERR_INVAL);
  access[1] = integer (-1); CHECK (SHM ("get", &r, 2, access) == TS_ERR_INVAL);
  access[1] = integer (0); access[2] = integer (256);
  CHECK (SHM ("set", &r, 3, access) == TS_ERR_INVAL);
  CHECK (SHM ("unlink", &r, 1, a) == TS_OK);
  CHECK (SHM ("open", &r, 1, a) == TS_ERR_NOTFOUND && error.system_errno == ENOENT);
  access[1] = integer (7); CHECK (SHM ("get", &r, 2, access) == TS_OK && r.as.integer == 255);
  CHECK (ts_value_copy (&alias, &other) == TS_OK);
  CHECK (SHM ("close", &r, 1, &other) == TS_OK);
  CHECK (SHM ("close", &r, 1, &alias) == TS_OK);
  CHECK (fcntl (fd, F_GETFD) < 0 && errno == EBADF);
  CHECK (SHM ("get", &r, 2, access) == TS_ERR_INVAL);
  ts_value_free (&alias); ts_value_free (&other); ts_value_free (&shared);
  a[1] = integer (0); CHECK (SHM ("create", &r, 2, a) == TS_ERR_LIMIT);
  a[1] = integer (16777217); CHECK (SHM ("create", &r, 2, a) == TS_ERR_LIMIT);
  a[0] = string ("bad/name"); CHECK (SHM ("open", &r, 1, a) == TS_ERR_INVAL);
  a[0] = string ("/"); CHECK (SHM ("open", &r, 1, a) == TS_ERR_INVAL);
  a[0] = string (shared_name);
  /* Zero-sized and oversized objects supplied by an external process. */
  fd = shm_open (shared_name, O_CREAT | O_EXCL | O_RDWR, 0600); CHECK (fd >= 0);
  int descriptors = descriptor_count ();
  for (int i = 0; i < 100; i++) CHECK (SHM ("open", &r, 1, a) == TS_ERR_LIMIT);
  CHECK (descriptor_count () == descriptors);
  CHECK (!ftruncate (fd, 16777217)); CHECK (SHM ("open", &r, 1, a) == TS_ERR_LIMIT);
  CHECK (!close (fd)); CHECK (SHM ("unlink", &r, 1, a) == TS_OK);

  CHECK (MQ ("available", &r, 0, NULL) == TS_OK && r.type == TS_BOOL);
  if (!r.as.boolean)
    { CHECK (MQ ("open", &r, 1, a) == TS_ERR_UNSUPPORTED); ts_value_free (&r); return 0; }
  a[0] = string (queue_name); a[1] = integer (2); a[2] = integer (8);
  TS_Value sender = {0}, receiver = {0};
  CHECK (MQ ("create", &sender, 3, a) == TS_OK);
  CHECK (MQ ("create", &r, 3, a) == TS_ERR_SYSTEM && error.system_errno == EEXIST);
  CHECK (MQ ("open", &receiver, 1, a) == TS_OK);
  CHECK (MQ ("message_size", &r, 1, &sender) == TS_OK && r.as.integer == 8);
  CHECK (MQ ("receive", &r, 1, &receiver) == TS_ERR_WOULD_BLOCK && error.system_errno == EAGAIN);
  TS_Value message[] = { sender, string ("low"), integer (0) };
  CHECK (MQ ("send", &r, 3, message) == TS_OK && r.as.integer == 3);
  message[1] = string ("high"); message[2] = integer (3);
  CHECK (MQ ("send", &r, 3, message) == TS_OK);
  CHECK (MQ ("send", &r, 3, message) == TS_ERR_WOULD_BLOCK);
  CHECK (MQ ("len", &r, 1, &sender) == TS_OK && r.as.integer == 2);
  CHECK (MQ ("receive", &r, 1, &receiver) == TS_OK);
  ts_std_list_t *packet = ts_std_list_unwrap (&r, &error, "message");
  CHECK (packet && ts_std_list_len (packet) == 2);
  CHECK (!strcmp (ts_std_list_get (packet, 0)->as.string, "high"));
  CHECK (ts_std_list_get (packet, 1)->as.integer == 3);
  CHECK (MQ ("receive", &r, 1, &receiver) == TS_OK);
  packet = ts_std_list_unwrap (&r, &error, "message");
  CHECK (!strcmp (ts_std_list_get (packet, 0)->as.string, "low"));
  message[1] = string ("123456789"); CHECK (MQ ("send", &r, 3, message) == TS_ERR_LIMIT);
  message[1] = string (""); message[2] = integer (-1);
  CHECK (MQ ("send", &r, 3, message) == TS_ERR_INVAL);
  message[2] = integer (0); CHECK (MQ ("send", &r, 3, message) == TS_OK && r.as.integer == 0);
  CHECK (MQ ("receive", &r, 1, &receiver) == TS_OK);
  packet = ts_std_list_unwrap (&r, &error, "empty message");
  CHECK (!strcmp (ts_std_list_get (packet, 0)->as.string, ""));
#ifdef TS_TEST_HAVE_MQUEUE
  mqd_t native = mq_open (queue_name, O_RDWR | O_NONBLOCK); CHECK (native != (mqd_t)-1);
  CHECK (!mq_send (native, "a\0b", 3, 0)); CHECK (!mq_close (native));
  CHECK (MQ ("receive", &r, 1, &receiver) == TS_ERR_UNSUPPORTED);
  CHECK (MQ ("receive", &r, 1, &receiver) == TS_ERR_WOULD_BLOCK);
#endif
  CHECK (MQ ("unlink", &r, 1, a) == TS_OK);
  CHECK (MQ ("open", &r, 1, a) == TS_ERR_NOTFOUND);
  CHECK (MQ ("send", &r, 2, message) == TS_OK);
  CHECK (ts_value_copy (&alias, &receiver) == TS_OK);
  CHECK (MQ ("close", &r, 1, &receiver) == TS_OK);
  CHECK (MQ ("close", &r, 1, &alias) == TS_OK);
  CHECK (MQ ("receive", &r, 1, &alias) == TS_ERR_INVAL);
  ts_value_free (&alias); ts_value_free (&receiver); ts_value_free (&sender);
  a[1] = integer (1025); CHECK (MQ ("create", &r, 3, a) == TS_ERR_LIMIT);
  a[1] = integer (1); a[2] = integer (65537); CHECK (MQ ("create", &r, 3, a) == TS_ERR_LIMIT);
  ts_value_free (&r);
  return 0;
}
