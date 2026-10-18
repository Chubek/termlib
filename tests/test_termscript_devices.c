#define _GNU_SOURCE
#include "termscript/stdlib/ts_stdlib.h"
#include "termscript/stdlib/common/ts_std_common.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static TS_Error error;
#define CHECK(x) do { if (!(x)) { fprintf (stderr, "%d: %s (%s)\n", __LINE__, #x, error.message); exit (1); } } while (0)
static TS_Value integer (int64_t i) { TS_Value v = { .type = TS_INT, .as.integer = i }; return v; }
static TS_Value string (const char *s) { TS_Value v = { .type = TS_STRING, .as.string = (char *)s }; return v; }
static TS_Status call (const TS_Module *m, const char *name, TS_Value *r, size_t n, TS_Value *a)
{
  ts_value_free (r); ts_error_clear (&error);
  const TS_FuncDef *f = ts_module_find_func (m, name); CHECK (f);
  return f->func (NULL, f->userdata, a, n, r, &error);
}
#define EV(f,r,n,a) call (&ts_std_evdev_module,f,r,n,a)
#define UV(f,r,n,a) call (&ts_std_uvdev_module,f,r,n,a)
static bool mocking;
static int mock_fd = -1, fail_at = -1, ioctl_count, creates, destroys, write_mode;
int __real_ioctl (int, unsigned long, ...);
ssize_t __real_write (int, const void *, size_t);
int __wrap_ioctl (int fd, unsigned long request, ...)
{
  va_list args; va_start (args, request);
  if (!mocking)
    { void *argument = va_arg (args, void *); va_end (args); return __real_ioctl (fd, request, argument); }
  if (mock_fd < 0) mock_fd = fd;
  CHECK (fd == mock_fd);
  if (request == UI_DEV_SETUP)
    {
      struct uinput_setup *setup = va_arg (args, void *);
      CHECK (!strcmp (setup->name, "Termscript fixture") && setup->id.bustype == BUS_VIRTUAL);
    }
  else (void)va_arg (args, unsigned long);
  va_end (args);
  if (request == UI_DEV_DESTROY) { destroys++; return 0; }
  if (ioctl_count++ == fail_at) { errno = EIO; return -1; }
  if (request == UI_DEV_CREATE) creates++;
  return 0;
}
ssize_t __wrap_write (int fd, const void *data, size_t size)
{
  if (mocking && fd == mock_fd)
    {
      if (write_mode == 1) { errno = EAGAIN; return -1; }
      if (write_mode == 2) { write_mode = 0; errno = EINTR; return -1; }
      if (write_mode == 3) return (ssize_t)(size / 2);
    }
  return __real_write (fd, data, size);
}
static int descriptors (void)
{ int n = 0; for (int fd = 0; fd < 1024; fd++) if (fcntl (fd, F_GETFD) >= 0) n++; return n; }
static void event_field (TS_Value *r, const char *key, int64_t expected)
{
  ts_dom_t *d = ts_dom_unwrap (r, &error, "event"); CHECK (d);
  TS_Value v = {0}; CHECK (ts_dom_to_value (ts_dom_key (d, key), &v, &error) == TS_OK);
  CHECK (v.type == TS_INT && v.as.integer == expected); ts_value_free (&v);
}
static void evdev_tests (void)
{
  TS_Value device = {0}, r = {0};
  CHECK (EV ("available", &r, 0, NULL) == TS_OK && r.as.boolean);
  struct input_event event = {0};
  event.type = EV_KEY; event.code = KEY_A; event.value = 1;
  event.input_event_sec = 7; event.input_event_usec = 123;
  int before = descriptors ();
  for (size_t split = 1; split < sizeof event; split++)
    {
      int fds[2]; CHECK (!pipe2 (fds, O_NONBLOCK | O_CLOEXEC));
      TS_Value fd = integer (fds[0]); CHECK (EV ("from_fd", &device, 1, &fd) == TS_OK);
      CHECK (EV ("fd", &r, 1, &device) == TS_OK && r.as.integer != fds[0]);
      int owned = (int)r.as.integer; CHECK (fcntl (owned, F_GETFD) & FD_CLOEXEC);
      CHECK (write (fds[1], &event, split) == (ssize_t)split);
      CHECK (EV ("read", &r, 1, &device) == TS_ERR_WOULD_BLOCK && error.system_errno == EAGAIN);
      CHECK (write (fds[1], (char *)&event + split, sizeof event - split) == (ssize_t)(sizeof event - split));
      CHECK (EV ("read", &r, 1, &device) == TS_OK);
      event_field (&r, "type", EV_KEY); event_field (&r, "code", KEY_A);
      event_field (&r, "value", 1); event_field (&r, "seconds", 7); event_field (&r, "microseconds", 123);
      CHECK (EV ("read", &r, 1, &device) == TS_ERR_WOULD_BLOCK);
      CHECK (!close (fds[1])); CHECK (EV ("read", &r, 1, &device) == TS_ERR_EOF);
      TS_Value alias = {0}; CHECK (ts_value_copy (&alias, &device) == TS_OK);
      CHECK (EV ("close", &r, 1, &device) == TS_OK);
      CHECK (EV ("close", &r, 1, &alias) == TS_OK);
      CHECK (fcntl (owned, F_GETFD) < 0 && errno == EBADF);
      CHECK (fcntl (fds[0], F_GETFL) & O_NONBLOCK); // borrowed descriptor remains live
      CHECK (EV ("read", &r, 1, &alias) == TS_ERR_INVAL);
      ts_value_free (&alias); ts_value_free (&device); CHECK (!close (fds[0]));
    }
  CHECK (descriptors () == before);
  int fds[2]; CHECK (!pipe2 (fds, O_NONBLOCK | O_CLOEXEC));
  TS_Value fd = integer (fds[0]); CHECK (EV ("from_fd", &device, 1, &fd) == TS_OK);
  CHECK (write (fds[1], &event, 1) == 1); CHECK (!close (fds[1]));
  CHECK (EV ("read", &r, 1, &device) == TS_ERR_PARSE && error.offset == 1);
  ts_value_free (&device); CHECK (!close (fds[0]));
  CHECK (!pipe (fds)); fd = integer (fds[0]);
  CHECK (EV ("from_fd", &device, 1, &fd) == TS_ERR_INVAL);
  CHECK (!(fcntl (fds[0], F_GETFL) & O_NONBLOCK)); CHECK (!close (fds[0]) && !close (fds[1]));
  CHECK (!pipe2 (fds, O_NONBLOCK | O_CLOEXEC)); fd = integer (fds[0]);
  CHECK (EV ("from_fd", &device, 1, &fd) == TS_OK);
  event.input_event_usec = 1000000; CHECK (write (fds[1], &event, sizeof event) == sizeof event);
  CHECK (EV ("read", &r, 1, &device) == TS_ERR_PARSE);
  event.input_event_usec = 0; CHECK (write (fds[1], &event, sizeof event) == sizeof event);
  CHECK (EV ("read", &r, 1, &device) == TS_OK);
  CHECK (EV ("name", &r, 1, &device) == TS_ERR_SYSTEM && error.system_errno == ENOTTY);
  ts_value_free (&device); CHECK (!close (fds[0]) && !close (fds[1])); ts_value_free (&r);
}
static void uvdev_tests (void)
{
  char path[] = "/tmp/opencode/termscript-uinput-XXXXXX";
  int file = mkstemp (path); CHECK (file >= 0);
  ts_std_list_t *keys = ts_std_list_new (); CHECK (keys);
  CHECK (ts_std_list_push_int (keys, KEY_A) == TS_OK && ts_std_list_push_int (keys, KEY_ENTER) == TS_OK);
  TS_Value enabled = {0}, device = {0}, r = {0};
  CHECK (ts_std_list_wrap (&enabled, keys, &error) == TS_OK);
  TS_Value a[] = { string ("Termscript fixture"), enabled, string (path) };
  CHECK (UV ("available", &r, 0, NULL) == TS_OK && r.as.boolean);
  mocking = true;
  int before = descriptors ();
  for (int i = 0; i < 5; i++)
    {
      fail_at = i; ioctl_count = creates = destroys = 0; mock_fd = -1;
      CHECK (UV ("create", &device, 3, a) == TS_ERR_SYSTEM && error.system_errno == EIO);
      CHECK (!creates && !destroys && descriptors () == before);
      CHECK (fcntl (mock_fd, F_GETFD) < 0 && errno == EBADF);
    }
  fail_at = -1; ioctl_count = creates = destroys = 0; mock_fd = -1;
  CHECK (UV ("create", &device, 3, a) == TS_OK && creates == 1);
  CHECK (UV ("fd", &r, 1, &device) == TS_OK && r.as.integer == mock_fd);
  CHECK (fcntl (mock_fd, F_GETFL) & O_NONBLOCK); CHECK (fcntl (mock_fd, F_GETFD) & FD_CLOEXEC);
  TS_Value key[] = { device, integer (KEY_A), integer (1) };
  write_mode = 1; CHECK (UV ("key", &r, 3, key) == TS_ERR_WOULD_BLOCK);
  write_mode = 2; CHECK (UV ("key", &r, 3, key) == TS_OK); // EINTR retry
  CHECK (UV ("sync", &r, 1, &device) == TS_OK);
  struct input_event events[2]; CHECK (pread (file, events, sizeof events, 0) == sizeof events);
  CHECK (events[0].type == EV_KEY && events[0].code == KEY_A && events[0].value == 1);
  CHECK (events[1].type == EV_SYN && events[1].code == SYN_REPORT);
  key[1] = integer (KEY_B); CHECK (UV ("key", &r, 3, key) == TS_ERR_INVAL);
  key[1] = integer (KEY_A); key[2] = integer (3); CHECK (UV ("key", &r, 3, key) == TS_ERR_INVAL);
  TS_Value alias = {0}; CHECK (ts_value_copy (&alias, &device) == TS_OK);
  CHECK (UV ("close", &r, 1, &device) == TS_OK && destroys == 1);
  CHECK (UV ("close", &r, 1, &alias) == TS_OK && destroys == 1);
  CHECK (UV ("sync", &r, 1, &alias) == TS_ERR_INVAL);
  ts_value_free (&alias); ts_value_free (&device);
  mock_fd = -1; CHECK (UV ("create", &device, 3, a) == TS_OK);
  write_mode = 3; CHECK (UV ("sync", &r, 1, &device) == TS_ERR_SYSTEM && error.system_errno == EIO);
  CHECK (UV ("fd", &r, 1, &device) == TS_ERR_INVAL); // fatal short record closes the device
  CHECK (fcntl (mock_fd, F_GETFD) < 0 && errno == EBADF);
  write_mode = 0; mocking = false;
  ts_value_free (&device); ts_value_free (&enabled); ts_value_free (&r);
  TS_Value path_value = string (path);
  CHECK (EV ("open", &r, 1, &path_value) == TS_ERR_INVAL); // no fake device from regular file
  CHECK (!close (file) && !unlink (path)); ts_value_free (&r);
}
int main (void) { evdev_tests (); uvdev_tests (); puts ("input device fixtures OK"); return 0; }
