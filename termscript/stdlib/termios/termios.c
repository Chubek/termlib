#include "termios/termios.h"
#include "common/tty.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int
ts_dup_cloexec (int fd)
{
#ifdef F_DUPFD_CLOEXEC
  return fcntl (fd, F_DUPFD_CLOEXEC, 3);
#else
  int copy = fcntl (fd, F_DUPFD, 3);
  if (copy >= 0 && fcntl (copy, F_SETFD, FD_CLOEXEC)) { int saved = errno; close (copy); errno = saved; return -1; }
  return copy;
#endif
}

TS_Status
ts_tty_save (ts_tty_state_t *s, int fd, TS_Error *e)
{
  memset (s, 0, sizeof *s);
  s->fd = ts_dup_cloexec (fd);
  if (s->fd < 0) return ts_std_system (e, "dup tty");
  while (tcgetattr (s->fd, &s->original))
    {
      if (errno == EINTR) continue;
      { TS_Status st = ts_std_system (e, "tcgetattr"); close (s->fd); s->fd = -1; return st; }
    }
  s->current = s->original;
  return TS_OK;
}

TS_Status
ts_tty_restore (ts_tty_state_t *s, TS_Error *e)
{
  if (s->fd < 0) return TS_OK;
  while (tcsetattr (s->fd, TCSANOW, &s->original))
    { if (errno == EINTR) continue; return ts_std_system (e, "restore termios"); }
  s->current = s->original;
  s->dirty = false;
  return TS_OK;
}

void
ts_tty_dispose (ts_tty_state_t *s)
{
  if (!s || s->fd < 0) return;
  if (s->dirty) (void) ts_tty_restore (s, NULL);
  close (s->fd);
  s->fd = -1;
}

void
ts_tty_raw (struct termios *t)
{
  t->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  t->c_oflag &= ~OPOST;
  t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  t->c_cflag &= ~(CSIZE | PARENB);
  t->c_cflag |= CS8;
  t->c_cc[VMIN] = 1; t->c_cc[VTIME] = 0;
}

static int64_t
mono_ms (void)
{
  struct timespec t;
  if (clock_gettime (CLOCK_MONOTONIC, &t)) return -1;
  return (int64_t) t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int
ts_poll_fd (int fd, short events, int cancel_fd, int timeout)
{
  struct pollfd p[2] = {{fd, events, 0}, {cancel_fd, POLLIN, 0}};
  int64_t deadline = timeout >= 0 ? mono_ms () + timeout : 0;
  int remaining = timeout, r;
  for (;;)
    {
      r = poll (p, cancel_fd >= 0 ? 2 : 1, remaining);
      if (r < 0 && errno == EINTR)
        {
          if (timeout >= 0)
            { int64_t left = deadline - mono_ms (); remaining = left > 0 ? (int) left : 0; }
          continue;
        }
      if (r <= 0) return r;
      if (cancel_fd >= 0 && p[1].revents) return -2;
      if (p[0].revents & POLLNVAL) { errno = EBADF; return -1; }
      return p[0].revents;
    }
}

static const char termios_tag;
static void state_free (void *p) { ts_tty_dispose (p); free (p); }
static ts_tty_state_t *state (const TS_Value *a, TS_Error *e)
{
  ts_tty_state_t *s = ts_std_handle (a, &termios_tag, e, "termios");
  if (s && s->fd < 0) { ts_std_inval (e, "termios: closed state"); return NULL; }
  return s;
}

static TS_Status
t_get (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  int64_t fd = 0;
  ts_tty_state_t *s;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 0, 1, e, "get") || (n && ts_std_int (a, &fd, e, "get"))) return TS_ERR_INVAL;
  if (fd < 0 || fd > INT32_MAX) return ts_std_inval (e, "get: invalid fd");
  s = malloc (sizeof *s);
  if (!s) return ts_std_oom (e);
  st = ts_tty_save (s, (int) fd, e);
  if (st) { free (s); return st; }
  if (ts_value_make_handle (ret, s, state_free, "<termios>", &termios_tag)) { state_free (s); return ts_std_oom (e); }
  return TS_OK;
}

static TS_Status
t_action (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  intptr_t op = (intptr_t) ud;
  ts_tty_state_t *s;
  const char *action = "now";
  int when = TCSANOW;
  TS_Status st = TS_OK;
  if (ts_std_argc (vm, n, 1, op == 1 ? 2 : 1, e, "termios")) return TS_ERR_INVAL;
  if (op == 3)
    {
      s = ts_std_handle (a, &termios_tag, e, "close");
      if (!s) return TS_ERR_INVAL;
      if (s->fd >= 0 && s->dirty) { st = ts_tty_restore (s, e); if (st) return st; }
      ts_tty_dispose (s);
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  s = state (a, e);
  if (!s) return TS_ERR_INVAL;
  if (op == 0) ts_tty_raw (&s->current);
  else if (op == 2) st = ts_tty_restore (s, e);
  else
    {
      if (n == 2 && ts_std_str (a + 1, &action, e, "apply")) return TS_ERR_INVAL;
      if (!strcmp (action, "drain")) when = TCSADRAIN;
      else if (!strcmp (action, "flush")) when = TCSAFLUSH;
      else if (strcmp (action, "now")) return ts_std_inval (e, "apply: expected now/drain/flush");
      while (tcsetattr (s->fd, when, &s->current))
        { if (errno == EINTR) continue; return ts_std_system (e, "apply termios"); }
      s->dirty = true;
    }
  ts_std_ret_bool (ret, true);
  return st;
}

typedef struct { const char *name; char group; tcflag_t flag; } flag_entry;
static const flag_entry flags[] = {
  {"IGNBRK", 'i', IGNBRK}, {"BRKINT", 'i', BRKINT}, {"IGNPAR", 'i', IGNPAR},
  {"PARMRK", 'i', PARMRK}, {"INPCK", 'i', INPCK}, {"ISTRIP", 'i', ISTRIP},
  {"INLCR", 'i', INLCR}, {"IGNCR", 'i', IGNCR}, {"ICRNL", 'i', ICRNL},
  {"IXON", 'i', IXON}, {"IXOFF", 'i', IXOFF}, {"IXANY", 'i', IXANY},
  {"OPOST", 'o', OPOST},
#ifdef ONLCR
  {"ONLCR", 'o', ONLCR},
#endif
  {"CSTOPB", 'c', CSTOPB}, {"CREAD", 'c', CREAD}, {"PARENB", 'c', PARENB},
  {"PARODD", 'c', PARODD}, {"HUPCL", 'c', HUPCL}, {"CLOCAL", 'c', CLOCAL},
#ifdef CRTSCTS
  {"CRTSCTS", 'c', CRTSCTS},
#endif
  {"ISIG", 'l', ISIG}, {"ICANON", 'l', ICANON}, {"ECHO", 'l', ECHO},
  {"ECHOE", 'l', ECHOE}, {"ECHOK", 'l', ECHOK}, {"ECHONL", 'l', ECHONL},
  {"NOFLSH", 'l', NOFLSH}, {"TOSTOP", 'l', TOSTOP}, {"IEXTEN", 'l', IEXTEN}
};

static TS_Status
t_flag (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  ts_tty_state_t *s;
  const char *name;
  size_t i;
  tcflag_t *group;
  if (ts_std_argc (vm, n, ud ? 3 : 2, ud ? 3 : 2, e, "flag") || ts_std_str (a + 1, &name, e, "flag")) return TS_ERR_INVAL;
  s = state (a, e);
  if (!s) return TS_ERR_INVAL;
  if (ud && a[2].type != TS_BOOL) return ts_std_inval (e, "set_flag: bool required");
  for (i = 0; i < sizeof flags / sizeof flags[0]; i++)
    if (!strcmp (name, flags[i].name))
      {
        group = flags[i].group == 'i' ? &s->current.c_iflag : flags[i].group == 'o' ? &s->current.c_oflag :
                flags[i].group == 'c' ? &s->current.c_cflag : &s->current.c_lflag;
        if (ud) { if (a[2].as.boolean) *group |= flags[i].flag; else *group &= ~flags[i].flag; }
        ts_std_ret_bool (ret, (*group & flags[i].flag) != 0);
        return TS_OK;
      }
  return ts_std_inval (e, "flag: unsupported termios flag");
}

static TS_Status
t_cc (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  static const struct { const char *name; int index; } cc[] = {
    {"VMIN", VMIN}, {"VTIME", VTIME}, {"VINTR", VINTR}, {"VQUIT", VQUIT},
    {"VERASE", VERASE}, {"VKILL", VKILL}, {"VEOF", VEOF}, {"VSTART", VSTART},
    {"VSTOP", VSTOP}, {"VSUSP", VSUSP}, {"VEOL", VEOL}
  };
  ts_tty_state_t *s;
  const char *name;
  int64_t value;
  size_t i;
  if (ts_std_argc (vm, n, ud ? 3 : 2, ud ? 3 : 2, e, "cc") || ts_std_str (a + 1, &name, e, "cc")) return TS_ERR_INVAL;
  s = state (a, e);
  if (!s) return TS_ERR_INVAL;
  if (ud && (ts_std_int (a + 2, &value, e, "set_cc") || value < 0 || value > 255))
    return ts_std_inval (e, "set_cc: value must be 0..255");
  for (i = 0; i < sizeof cc / sizeof cc[0]; i++)
    if (!strcmp (name, cc[i].name))
      {
        if (ud) s->current.c_cc[cc[i].index] = (cc_t) value;
        ts_std_ret_int (ret, s->current.c_cc[cc[i].index]);
        return TS_OK;
      }
  return ts_std_inval (e, "cc: unsupported control character");
}

static const TS_FuncDef funcs[] = {
  {"get", t_get, NULL}, {"save", t_get, NULL}, {"raw", t_action, NULL},
  {"apply", t_action, (void *) 1}, {"restore", t_action, (void *) 2},
  {"close", t_action, (void *) 3}, {"flag", t_flag, NULL}, {"set_flag", t_flag, (void *) 1},
  {"cc", t_cc, NULL}, {"set_cc", t_cc, (void *) 1}, {NULL, NULL, NULL}
};
const TS_Module ts_std_termios_module = {"std.termios", funcs};
