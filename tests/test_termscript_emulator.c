#include "termscript/stdlib/ts_stdlib.h"
#include "termscript/stdlib/common/ts_std_common.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf (stderr, "%s:%d: %s (%s)\n", __FILE__, __LINE__, #c, error.message); exit (1); } } while (0)
static TS_Error error;
static TS_VM *vm;
static TS_Value I (int64_t i) { TS_Value v = {0}; v.type = TS_INT; v.as.integer = i; return v; }
static TS_Value S (const char *s) { TS_Value v = {0}; v.type = TS_STRING; v.as.string = (char *) s; return v; }
static TS_Status
call (const TS_Module *m, const char *name, TS_Value *r, size_t n, ...)
{
  TS_Value a[8]; va_list ap;
  CHECK (n <= 8);
  va_start (ap, n); for (size_t i = 0; i < n; i++) a[i] = va_arg (ap, TS_Value); va_end (ap);
  ts_value_free (r); ts_error_clear (&error);
  const TS_FuncDef *f = ts_module_find_func (m, name);
  if (f) return f->func (vm, f->userdata, a, n, r, &error);
  CHECK (0); return TS_ERR_INVAL;
}
#define P(name, r, n, ...) call (&ts_std_poll_module, name, r, n, __VA_ARGS__)
static void signal_handler (int sig) { (void) sig; }
static void *interrupt_wait (void *p)
{
  pthread_t target = *(pthread_t *) p;
  struct timespec delay = {0, 10000000};
  for (int i = 0; i < 15; i++) { nanosleep (&delay, NULL); pthread_kill (target, SIGUSR1); }
  return NULL;
}
static int64_t now_ms (void)
{
  struct timespec t; CHECK (!clock_gettime (CLOCK_MONOTONIC, &t));
  return (int64_t) t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void
poll_tests (void)
{
  TS_Value set = {0}, r = {0}, alias = {0};
  ts_std_list_t *l;
  int fds[2];
  CHECK (!pipe (fds));
  CHECK (!call (&ts_std_poll_module, "new", &set, 0));
  CHECK (!ts_value_copy (&alias, &set));
  CHECK (!P ("watch", &r, 3, set, I (fds[0]), I (1)));
  CHECK (!P ("wait", &r, 2, set, I (0)));
  l = ts_std_list_unwrap (&r, &error, "test"); CHECK (l && !ts_std_list_len (l));
  CHECK (write (fds[1], "x", 1) == 1);
  CHECK (!P ("watch", &r, 3, set, I (fds[0]), I (1))); /* update, no duplicate */
  CHECK (!P ("wait", &r, 2, set, I (100)));
  l = ts_std_list_unwrap (&r, &error, "test"); CHECK (ts_std_list_len (l) == 2);
  CHECK (ts_std_list_get (l, 0)->as.integer == fds[0]); CHECK (ts_std_list_get (l, 1)->as.integer & 1);
  CHECK (!P ("watch", &r, 3, set, I (fds[1]), I (2)));
  CHECK (!P ("wait", &r, 2, set, I (0)));
  CHECK (ts_std_list_len (ts_std_list_unwrap (&r, &error, "test")) == 4);
  CHECK (!P ("remove", &r, 2, set, I (fds[1])) && r.as.boolean);
  CHECK (!P ("remove", &r, 2, set, I (fds[1])) && !r.as.boolean);
  close (fds[1]);
  CHECK (!P ("wait", &r, 2, set, I (0)));
  l = ts_std_list_unwrap (&r, &error, "test"); CHECK (ts_std_list_get (l, 1)->as.integer & 4);
  close (fds[0]);
  CHECK (!P ("wait", &r, 2, set, I (0)));
  l = ts_std_list_unwrap (&r, &error, "test"); CHECK (ts_std_list_get (l, 1)->as.integer & 16);
  CHECK (P ("watch", &r, 3, set, I (-1), I (1)) == TS_ERR_INVAL);
  CHECK (P ("watch", &r, 3, set, I (0), I (8)) == TS_ERR_INVAL);
  CHECK (P ("wait", &r, 2, set, I (-2)) == TS_ERR_INVAL);
  CHECK (!P ("remove", &r, 2, set, I (fds[0])));
  {
    struct sigaction sa = {0}, old;
    pthread_t thread, self = pthread_self ();
    int64_t start;
    sa.sa_handler = signal_handler; sigemptyset (&sa.sa_mask);
    CHECK (!sigaction (SIGUSR1, &sa, &old));
    CHECK (!pthread_create (&thread, NULL, interrupt_wait, &self));
    start = now_ms ();
    CHECK (!P ("wait", &r, 2, set, I (80)));
    CHECK (now_ms () - start >= 60 && now_ms () - start < 220);
    CHECK (!pthread_join (thread, NULL)); CHECK (!sigaction (SIGUSR1, &old, NULL));
  }
  for (int i = 0; i < 1024; i++) CHECK (!P ("watch", &r, 3, set, I (i), I (1)));
  CHECK (P ("watch", &r, 3, set, I (1024), I (1)) == TS_ERR_LIMIT);
  CHECK (!P ("close", &r, 1, set)); CHECK (!P ("close", &r, 1, alias));
  CHECK (P ("wait", &r, 2, alias, I (0)) == TS_ERR_INVAL);
  CHECK (fcntl (STDERR_FILENO, F_GETFD) >= 0); /* watches never own fds */
  ts_value_free (&set); ts_value_free (&alias); ts_value_free (&r);
}

static void
backend_tests (void)
{
  TS_Value r = {0};
  bool enabled;
  int fd = open ("/dev/null", O_RDWR); CHECK (fd >= 0);
  CHECK (!call (&ts_std_drm_module, "available", &r, 0)); enabled = r.as.boolean;
  CHECK (call (&ts_std_drm_module, "open", &r, 1, S ("/nonexistent/termscript/card")) ==
         (enabled ? TS_ERR_SYSTEM : TS_ERR_UNSUPPORTED));
  int next = dup (fd); CHECK (next >= 0); close (next);
  for (int i = 0; i < 32; i++)
    CHECK (call (&ts_std_drm_module, "from_fd", &r, 1, I (fd)) ==
           (enabled ? TS_ERR_SYSTEM : TS_ERR_UNSUPPORTED));
  int after = dup (fd); CHECK (after == next); close (after);
  CHECK (fcntl (fd, F_GETFD) >= 0); close (fd);
  CHECK (!call (&ts_std_x11_module, "available", &r, 0)); enabled = r.as.boolean;
  CHECK (call (&ts_std_x11_module, "open", &r, 4, S ("test"), I (32), I (32), S ("unix:65535")) ==
         (enabled ? TS_ERR_SYSTEM : TS_ERR_UNSUPPORTED));
  if (enabled)
    {
      CHECK (call (&ts_std_x11_module, "open", &r, 3, S ("test"), I (0), I (32)) == TS_ERR_INVAL);
      CHECK (call (&ts_std_x11_module, "fd", &r, 1, I (0)) == TS_ERR_INVAL);
    }
  ts_value_free (&r);
}

/* Opt-in integration test under Xvfb (also usable with a real DISPLAY). */
static void
window_tests (void)
{
  TS_Value w = {0}, r = {0}, alias = {0}, pixels = {0};
  ts_std_list_t *l = ts_std_list_new ();
  int fd;
  CHECK (l);
  CHECK (!call (&ts_std_x11_module, "open", &w, 3, S ("Termscript test"), I (64), I (32)));
  CHECK (!ts_value_copy (&alias, &w));
  CHECK (!call (&ts_std_x11_module, "fd", &r, 1, w)); fd = (int) r.as.integer;
  CHECK (fcntl (fd, F_GETFD) & FD_CLOEXEC);
  CHECK (!call (&ts_std_x11_module, "fill", &r, 6, w, I (0), I (0), I (64), I (32), I (0x123456)));
  CHECK (!ts_std_list_push_int (l, 0xff0000)); CHECK (!ts_std_list_push_int (l, 0x00ff00));
  CHECK (!ts_std_list_wrap (&pixels, l, &error));
  CHECK (!call (&ts_std_x11_module, "image", &r, 6, w, I (1), I (1), I (2), I (1), pixels));
  CHECK (call (&ts_std_x11_module, "image", &r, 6, w, I (0), I (0), I (3), I (1), pixels) == TS_ERR_INVAL);
  CHECK (!call (&ts_std_x11_module, "flush", &r, 1, w));
  bool exposed = false;
  for (int i = 0; i < 100 && !exposed; i++)
    {
      CHECK (!call (&ts_std_x11_module, "event", &r, 1, w));
      if (r.type == TS_NIL) { struct timespec delay = {0, 10000000}; nanosleep (&delay, NULL); continue; }
      ts_dom_t *obj = ts_dom_unwrap (&r, &error, "event"); TS_Value type = {0}; CHECK (obj);
      CHECK (!ts_dom_to_value (ts_dom_path (obj, "type"), &type, &error));
      exposed = !strcmp (type.as.string, "expose"); ts_value_free (&type);
    }
  CHECK (exposed);
  CHECK (!call (&ts_std_x11_module, "close", &r, 1, w));
  CHECK (!call (&ts_std_x11_module, "close", &r, 1, alias));
  CHECK (fcntl (fd, F_GETFD) < 0 && errno == EBADF);
  CHECK (call (&ts_std_x11_module, "fd", &r, 1, alias) == TS_ERR_INVAL);
  ts_value_free (&w); ts_value_free (&alias); ts_value_free (&pixels); ts_value_free (&r);
}
static void
device_tests (const char *path)
{
  TS_Value d = {0}, copy = {0}, r = {0}, snapshot = {0};
  int fd;
  CHECK (!call (&ts_std_drm_module, "open", &d, 1, S (path)));
  CHECK (!call (&ts_std_drm_module, "fd", &r, 1, d)); fd = (int) r.as.integer;
  CHECK (fcntl (fd, F_GETFD) & FD_CLOEXEC);
  CHECK (fcntl (fd, F_GETFL) & O_NONBLOCK);
  CHECK (!call (&ts_std_drm_module, "from_fd", &copy, 1, I (fd)));
  CHECK (!call (&ts_std_drm_module, "driver", &r, 1, d)); CHECK (r.type == TS_STRING && *r.as.string);
  CHECK (!call (&ts_std_drm_module, "cap", &r, 2, d, S ("dumb_buffer")));
  CHECK (!call (&ts_std_drm_module, "connectors", &snapshot, 1, d));
  CHECK (!call (&ts_std_drm_module, "close", &r, 1, d));
  CHECK (!call (&ts_std_drm_module, "close", &r, 1, d));
  CHECK (call (&ts_std_drm_module, "fd", &r, 1, d) == TS_ERR_INVAL);
  CHECK (fcntl (fd, F_GETFD) < 0 && errno == EBADF);
  CHECK (!call (&ts_std_drm_module, "driver", &r, 1, copy));
  ts_dom_t *doc = ts_dom_unwrap (&snapshot, &error, "snapshot"); CHECK (doc);
  CHECK (!strcmp (ts_dom_kind_str (doc), "array"));
  CHECK (!call (&ts_std_drm_module, "close", &r, 1, copy));
  ts_value_free (&d); ts_value_free (&copy); ts_value_free (&r); ts_value_free (&snapshot);
}

int main (int argc, char **argv)
{
  ts_error_clear (&error); vm = ts_vm_create (&error); CHECK (vm);
  CHECK (!ts_stdlib_register_all (vm, &error));
  if (argc == 2 && !strcmp (argv[1], "--x11")) window_tests ();
  else if (argc == 3 && !strcmp (argv[1], "--drm")) device_tests (argv[2]);
  else { poll_tests (); backend_tests (); }
  ts_vm_free (vm);
  puts ("emulator modules OK"); return 0;
}
