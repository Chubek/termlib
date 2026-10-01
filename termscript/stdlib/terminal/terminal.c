#include "terminal/terminal.h"
#include "common/terminal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static const char terminal_tag;

static void
terminal_dispose (ts_terminal_t *s)
{
  if (s->flags_changed && s->input >= 0) (void) fcntl (s->input, F_SETFL, s->original_flags);
  if (s->flags_changed && s->output >= 0) (void) fcntl (s->output, F_SETFL, s->original_output_flags);
  s->flags_changed = false;
  ts_tty_dispose (&s->tty);
  if (s->input >= 0) close (s->input);
  if (s->output >= 0) close (s->output);
  if (s->cancel[0] >= 0) close (s->cancel[0]);
  if (s->cancel[1] >= 0) close (s->cancel[1]);
  s->input = s->output = s->cancel[0] = s->cancel[1] = -1;
  s->status = "closed";
}
static void terminal_free (void *p) { if (p) terminal_dispose (p); free (p); }

ts_terminal_t *
ts_terminal_unwrap (const TS_Value *a, TS_Error *e)
{
  ts_terminal_t *s = ts_std_handle (a, &terminal_tag, e, "terminal");
  if (s && s->input < 0) { ts_std_inval (e, "terminal: closed session"); return NULL; }
  return s;
}

TS_Status
ts_terminal_raw (ts_terminal_t *s, TS_Error *e)
{
  if (s->tty.fd < 0) return ts_std_inval (e, "raw: input is not a TTY");
  ts_tty_raw (&s->tty.current);
  while (tcsetattr (s->tty.fd, TCSANOW, &s->tty.current))
    { if (errno == EINTR) continue; return ts_std_system (e, "raw termios"); }
  s->tty.dirty = true;
  return TS_OK;
}

static TS_Status
t_new (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  int64_t input = 0, output = 1;
  ts_terminal_t *s;
  TS_Status st;
  int i;
  (void) ud;
  if (ts_std_argc (vm, n, 0, 2, e, "terminal.new") ||
      (n && ts_std_int (a, &input, e, "terminal.new")) ||
      (n > 1 && ts_std_int (a + 1, &output, e, "terminal.new"))) return TS_ERR_INVAL;
  if (input < 0 || output < 0 || input > INT_MAX || output > INT_MAX) return ts_std_inval (e, "terminal.new: invalid fd");
  s = calloc (1, sizeof *s);
  if (!s) return ts_std_oom (e);
  s->input = s->output = s->cancel[0] = s->cancel[1] = s->tty.fd = -1;
  s->status = "ok";
  s->input = ts_dup_cloexec ((int) input);
  s->output = ts_dup_cloexec ((int) output);
  if (s->input < 0 || s->output < 0 || pipe (s->cancel))
    { st = ts_std_system (e, "terminal descriptors"); terminal_free (s); return st; }
  for (i = 0; i < 2; i++)
    if (fcntl (s->cancel[i], F_SETFD, FD_CLOEXEC) || fcntl (s->cancel[i], F_SETFL, O_NONBLOCK))
      { st = ts_std_system (e, "cancel pipe flags"); terminal_free (s); return st; }
  s->original_flags = fcntl (s->input, F_GETFL);
  s->original_output_flags = fcntl (s->output, F_GETFL);
  if (s->original_flags < 0 || s->original_output_flags < 0) { st = ts_std_system (e, "terminal flags"); terminal_free (s); return st; }
  if (isatty (s->input))
    { st = ts_tty_save (&s->tty, s->input, e); if (st) { terminal_free (s); return st; } }
  if (ts_value_make_handle (ret, s, terminal_free, "<terminal>", &terminal_tag))
    { terminal_free (s); return ts_std_oom (e); }
  return TS_OK;
}

static TS_Status
t_state (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  intptr_t op = (intptr_t) ud;
  ts_terminal_t *s;
  TS_Status st = TS_OK;
  if (ts_std_argc (vm, n, op == 4 ? 2 : 1, op == 4 ? 2 : 1, e, "terminal")) return TS_ERR_INVAL;
  s = ts_std_handle (a, &terminal_tag, e, "terminal");
  if (!s) return TS_ERR_INVAL;
  if (op == 0) return ts_std_ret_str (ret, s->status, e);
  if (op == 1)
    {
      if (s->tty.dirty) { st = ts_tty_restore (&s->tty, e); if (st) return st; }
      terminal_dispose (s); ts_std_ret_nil (ret); return TS_OK;
    }
  if (s->input < 0) return ts_std_inval (e, "terminal: closed session");
  switch (op)
    {
    case 2: st = ts_terminal_raw (s, e); break;
    case 3: st = ts_tty_restore (&s->tty, e); break;
    case 4:
      if (a[1].type != TS_BOOL) return ts_std_inval (e, "nonblocking: bool required");
      if (fcntl (s->input, F_SETFL, a[1].as.boolean ? s->original_flags | O_NONBLOCK : s->original_flags & ~O_NONBLOCK))
        return ts_std_system (e, "nonblocking");
      s->flags_changed = true;
      if (fcntl (s->output, F_SETFL, a[1].as.boolean ? s->original_output_flags | O_NONBLOCK : s->original_output_flags & ~O_NONBLOCK))
        return ts_std_system (e, "nonblocking output");
      break;
    case 5: ts_std_ret_int (ret, s->input); return TS_OK;
    case 6: ts_std_ret_int (ret, s->output); return TS_OK;
    case 7:
      { ssize_t r; do { r = write (s->cancel[1], "c", 1); } while (r < 0 && errno == EINTR);
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return ts_std_system (e, "cancel"); }
      break;
    case 8:
      { char b[64]; ssize_t r;
        do { r = read (s->cancel[0], b, sizeof b); } while (r > 0 || (r < 0 && errno == EINTR)); }
      s->status = "ok";
      break;
    }
  ts_std_ret_bool (ret, st == TS_OK);
  return st;
}

static TS_Status
t_read (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  ts_terminal_t *s;
  int64_t max = 4096, timeout = -1;
  int ready;
  char *buf;
  ssize_t r;
  (void) ud;
  if (ts_std_argc (vm, n, 1, 3, e, "read") ||
      (n > 1 && ts_std_int (a + 1, &max, e, "read")) ||
      (n > 2 && ts_std_int (a + 2, &timeout, e, "read"))) return TS_ERR_INVAL;
  if (max < 1 || max > 1048576 || timeout < -1 || timeout > INT_MAX) return ts_std_inval (e, "read: invalid size/timeout");
  s = ts_terminal_unwrap (a, e);
  if (!s) return TS_ERR_INVAL;
  if (timeout == -1 && (fcntl (s->input, F_GETFL) & O_NONBLOCK)) timeout = 0;
  ready = ts_poll_fd (s->input, POLLIN, s->cancel[0], (int) timeout);
  if (ready == -1) { s->status = "system_error"; return ts_std_system (e, "read readiness"); }
  if (ready == -2 || ready == 0)
    { s->status = ready == -2 ? "cancelled" : timeout == 0 ? "would_block" : "timeout"; ts_std_ret_nil (ret); return TS_OK; }
  buf = malloc ((size_t) max + 1);
  if (!buf) return ts_std_oom (e);
  do { r = read (s->input, buf, (size_t) max); } while (r < 0 && errno == EINTR);
  if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    { free (buf); s->status = "would_block"; ts_std_ret_nil (ret); return TS_OK; }
  if (r < 0) { free (buf); s->status = "system_error"; return ts_std_system (e, "read"); }
  if (!r) { free (buf); s->status = "eof"; ts_std_ret_nil (ret); return TS_OK; }
  if (memchr (buf, 0, (size_t) r)) { free (buf); return ts_std_inval (e, "read: embedded NUL is not representable as a Termscript string"); }
  buf[r] = 0;
  ts_value_free (ret); ret->type = TS_STRING; ret->as.string = buf;
  s->status = "ok";
  return TS_OK;
}

static TS_Status
write_all (ts_terminal_t *s, const char *data, size_t len, int timeout, TS_Error *e)
{
  size_t sent = 0;
  struct timespec now;
  int64_t deadline = 0;
  if (timeout >= 0)
    { clock_gettime (CLOCK_MONOTONIC, &now); deadline = (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000 + timeout; }
  while (sent < len)
    {
      int remain = -1, ready;
      ssize_t r;
      if (timeout >= 0)
        { int64_t left; clock_gettime (CLOCK_MONOTONIC, &now); left = deadline - ((int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000); remain = left > 0 ? (int) left : 0; }
      ready = ts_poll_fd (s->output, POLLOUT, s->cancel[0], remain);
      if (ready <= 0)
        {
          s->status = ready == -2 ? "cancelled" : ready == 0 ? "timeout" : "system_error";
          if (ready == -1) return ts_std_system (e, "write readiness");
          ts_error_set (e, ready == -2 ? TS_ERR_CANCELLED : TS_ERR_TIMEOUT, 0, sent,
                        "write %s after %zu bytes", s->status, sent);
          return ready == -2 ? TS_ERR_CANCELLED : TS_ERR_TIMEOUT;
        }
      r = write (s->output, data + sent, len - sent > 4096 ? 4096 : len - sent);
      if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      if (r <= 0) { if (!r) errno = EIO; s->status = "system_error"; return ts_std_system (e, "write"); }
      sent += (size_t) r;
    }
  s->status = "ok";
  return TS_OK;
}

TS_Status
ts_terminal_write_all (ts_terminal_t *s, const char *data, size_t len, int timeout, TS_Error *e)
{
  int flags = fcntl (s->output, F_GETFL);
  TS_Status st;
  if (flags < 0 || (!(flags & O_NONBLOCK) && fcntl (s->output, F_SETFL, flags | O_NONBLOCK)))
    { s->status = "system_error"; return ts_std_system (e, "write flags"); }
  st = write_all (s, data, len, timeout, e);
  if (!(flags & O_NONBLOCK) && fcntl (s->output, F_SETFL, flags) && !st)
    { s->status = "system_error"; return ts_std_system (e, "restore write flags"); }
  return st;
}

static TS_Status
t_write (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  ts_terminal_t *s;
  const char *text;
  int64_t timeout = -1;
  ssize_t r;
  TS_Status st;
  if (ts_std_argc (vm, n, 2, ud ? 3 : 2, e, "write") || ts_std_str (a + 1, &text, e, "write") ||
      (n == 3 && ts_std_int (a + 2, &timeout, e, "write_all"))) return TS_ERR_INVAL;
  if (timeout < -1 || timeout > INT_MAX) return ts_std_inval (e, "write_all: invalid timeout");
  s = ts_terminal_unwrap (a, e);
  if (!s) return TS_ERR_INVAL;
  if (ud)
    {
      st = ts_terminal_write_all (s, text, strlen (text), (int) timeout, e);
      if (!st) ts_std_ret_int (ret, (int64_t) strlen (text));
      return st;
    }
  do { r = write (s->output, text, strlen (text)); } while (r < 0 && errno == EINTR);
  if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    { s->status = "would_block"; ts_std_ret_int (ret, 0); return TS_OK; }
  if (r < 0) { s->status = "system_error"; return ts_std_system (e, "write"); }
  s->status = "ok"; ts_std_ret_int (ret, r);
  return TS_OK;
}

static TS_Status
t_size (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  int fd = 1;
  int64_t value, rows, cols;
  struct winsize ws = {0};
  if (ts_std_argc (vm, n, ud ? 3 : 0, ud ? 3 : 1, e, "size")) return TS_ERR_INVAL;
  if (n)
    {
      if (a[0].type == TS_HANDLE) { ts_terminal_t *s = ts_terminal_unwrap (a, e); if (!s) return TS_ERR_INVAL; fd = s->output; }
      else { if (ts_std_int (a, &value, e, "size") || value < 0 || value > INT_MAX) return ts_std_inval (e, "size: invalid fd"); fd = (int) value; }
    }
  if (ud)
    {
      if (ts_std_int (a + 1, &rows, e, "resize") || ts_std_int (a + 2, &cols, e, "resize") ||
          rows < 1 || rows > 65535 || cols < 1 || cols > 65535) return ts_std_inval (e, "resize: dimensions must be 1..65535");
      ws.ws_row = (unsigned short) rows; ws.ws_col = (unsigned short) cols;
      if (ioctl (fd, TIOCSWINSZ, &ws)) return ts_std_system (e, "resize");
      ts_std_ret_bool (ret, true); return TS_OK;
    }
  if (ioctl (fd, TIOCGWINSZ, &ws)) return ts_std_system (e, "size");
  {
    ts_std_list_t *l = ts_std_list_new ();
    if (!l) return ts_std_oom (e);
    if (ts_std_list_push_int (l, ws.ws_row) || ts_std_list_push_int (l, ws.ws_col))
      { ts_std_list_free (l); return ts_std_oom (e); }
    return ts_std_list_wrap (ret, l, e);
  }
}

static TS_Status
t_control (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  intptr_t op = (intptr_t) ud;
  ts_sbuf_t b;
  int64_t row, col;
  const char *s;
  size_t i;
  if (ts_std_argc (vm, n, op == 0 ? 2 : op == 5 ? 0 : 1, op == 0 ? 2 : op == 5 ? 0 : 1, e, "control")) return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  if (op == 0)
    {
      if (ts_std_int (a, &row, e, "move") || ts_std_int (a + 1, &col, e, "move") ||
          row < 0 || col < 0 || row > 9999 || col > 9999) return ts_std_inval (e, "move: invalid position");
      if (ts_sbuf_printf (&b, "\033[%lld;%lldH", (long long) row + 1, (long long) col + 1)) goto oom;
    }
  else if (op == 4)
    {
      if (ts_std_str (a, &s, e, "title")) return TS_ERR_INVAL;
      for (i = 0; s[i]; i++) if ((unsigned char) s[i] < 32 || (unsigned char) s[i] == 127) return ts_std_inval (e, "title: control characters are not allowed");
      if (ts_sbuf_printf (&b, "\033]2;%s\033\\", s)) goto oom;
    }
  else if (op == 5) { if (ts_sbuf_str (&b, "\033[H\033[2J")) goto oom; }
  else
    {
      if (a[0].type != TS_BOOL) return ts_std_inval (e, "control: bool required");
      if (op == 1) s = a[0].as.boolean ? "\033[?25h" : "\033[?25l";
      else if (op == 2) s = a[0].as.boolean ? "\033[?1049h" : "\033[?1049l";
      else if (op == 3) s = a[0].as.boolean ? "\033[?2004h" : "\033[?2004l";
      else s = a[0].as.boolean ? "\033[?1000h\033[?1006h" : "\033[?1000l\033[?1006l";
      if (ts_sbuf_str (&b, s)) goto oom;
    }
  return ts_std_ret_sbuf (ret, &b, e);
oom: ts_sbuf_free (&b); return ts_std_oom (e);
}

static const TS_FuncDef funcs[] = {
  {"new", t_new, NULL}, {"status", t_state, NULL}, {"close", t_state, (void *) 1},
  {"raw", t_state, (void *) 2}, {"restore", t_state, (void *) 3}, {"nonblocking", t_state, (void *) 4},
  {"fd", t_state, (void *) 5}, {"output_fd", t_state, (void *) 6},
  {"cancel", t_state, (void *) 7}, {"reset_cancel", t_state, (void *) 8},
  {"read", t_read, NULL}, {"write", t_write, NULL}, {"write_all", t_write, (void *) 1},
  {"size", t_size, NULL}, {"resize", t_size, (void *) 1},
  {"move", t_control, NULL}, {"cursor", t_control, (void *) 1}, {"alternate", t_control, (void *) 2},
  {"paste", t_control, (void *) 3}, {"title", t_control, (void *) 4}, {"clear", t_control, (void *) 5},
  {"mouse", t_control, (void *) 6}, {NULL, NULL, NULL}
};
const TS_Module ts_std_terminal_module = {"std.terminal", funcs};
