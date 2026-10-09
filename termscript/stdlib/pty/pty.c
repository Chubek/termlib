/* pty.c -- std.pty: child processes under a pseudoterminal.
 *
 * A standalone-safe minipty (no libtermlib): spawn() forks /bin/sh -c
 * under a fresh pty pair; read()/write() move bytes; wait() reaps
 * (nil while running); close() kills and reaps.
 */
#include "pty/pty.h"

#include "common/ts_std_common.h"
#include "common/tty.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct
{
  int master;
  pid_t pid;
  bool reaped;
  int exit_code;
  int wait_status;
  const char *status;
} pty_t;

static const char pty_tag_id = 0;

static void
pty_free (void *p)
{
  pty_t *h = p;
  int st;
  if (!h)
    return;
  if (!h->reaped && h->pid > 0)
    {
      if (kill (-h->pid, SIGKILL) < 0) kill (h->pid, SIGKILL);
      while (waitpid (h->pid, &st, 0) < 0 && errno == EINTR)
        ;
    }
  if (h->master >= 0)
    close (h->master);
  free (h);
}

static pty_t *
pty_unwrap (const TS_Value *v, TS_Error *error, const char *what)
{
  return ts_std_handle (v, &pty_tag_id, error, what);
}

static TS_Status
p_spawn (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *cmd;
  int master, slave, errors[2] = {-1, -1};
  char slave_path[256];
  pid_t pid;
  pty_t *h;
  char desc[96];
  int64_t rows = 0, cols = 0;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 3, error, "spawn") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 2 || (argc == 3 && (ts_std_int (argv + 1, &rows, error, "rows") ||
      ts_std_int (argv + 2, &cols, error, "cols") || rows < 1 || rows > 65535 || cols < 1 || cols > 65535)))
    return ts_std_inval (error, "spawn: expected command [rows cols], dimensions 1..65535");
  if (ts_std_str (&argv[0], &cmd, error, "spawn") != TS_OK)
    return TS_ERR_INVAL;
  master = posix_openpt (O_RDWR | O_NOCTTY);
  if (master < 0)
    {
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "spawn: %s",
                    strerror (errno));
      return TS_ERR_SYSTEM;
    }
  if (grantpt (master) != 0 || unlockpt (master) != 0)
    {
      int e = errno;
      close (master);
      ts_error_set (error, TS_ERR_SYSTEM, e, 0, "spawn: %s",
                    strerror (e));
      return TS_ERR_SYSTEM;
    }
  {
    int copy = ts_dup_cloexec (master);
    close (master);
    master = copy;
  }
  if (master < 0) return ts_std_system (error, "spawn master duplicate");
#ifdef __linux__
  int name_error = ptsname_r (master, slave_path, sizeof slave_path);
  if (name_error) errno = name_error;
#else
  const char *name = ptsname (master);
  int name_error = !name || strlen (name) >= sizeof slave_path;
  if (!name_error) strcpy (slave_path, name);
#endif
  if (name_error)
    {
      int e = errno;
      close (master);
      ts_error_set (error, TS_ERR_SYSTEM, e, 0, "spawn: %s",
                    strerror (e));
      return TS_ERR_SYSTEM;
    }
  slave = open (slave_path, O_RDWR | O_NOCTTY);
  if (slave < 0)
    {
      int e = errno;
      close (master);
      ts_error_set (error, TS_ERR_SYSTEM, e, 0, "spawn: %s",
                    strerror (e));
      return TS_ERR_SYSTEM;
    }
  {
    int copy = ts_dup_cloexec (slave); close (slave); slave = copy;
  }
  if (slave < 0 || pipe (errors) || fcntl (errors[0], F_SETFD, FD_CLOEXEC) ||
      fcntl (errors[1], F_SETFD, FD_CLOEXEC))
    {
      int e = errno;
      if (slave >= 0) close (slave);
      if (errors[0] >= 0) close (errors[0]);
      if (errors[1] >= 0) close (errors[1]);
      close (master); errno = e; return ts_std_system (error, "spawn descriptors");
    }
  for (int i = 0; i < 2; i++)
    if (errors[i] < 3)
      {
        int copy = ts_dup_cloexec (errors[i]);
        if (copy < 0)
          { int e = errno; close (slave); close (master); close (errors[0]); close (errors[1]); errno = e; return ts_std_system (error, "spawn error pipe"); }
        close (errors[i]); errors[i] = copy;
      }
  /* Initial geometry must be set before the child can exec or query it. */
  if (argc == 3)
    {
      struct winsize ws = {0}; int rc;
      ws.ws_row = (unsigned short) rows; ws.ws_col = (unsigned short) cols;
      do { rc = ioctl (slave, TIOCSWINSZ, &ws); } while (rc < 0 && errno == EINTR);
      if (rc < 0)
        { int saved = errno; close (slave); close (master); close (errors[0]); close (errors[1]); errno = saved; return ts_std_system (error, "spawn size"); }
    }
  pid = fork ();
  if (pid < 0)
    {
      int e = errno;
      close (slave);
      close (master);
      close (errors[0]); close (errors[1]);
      ts_error_set (error, TS_ERR_SYSTEM, e, 0, "spawn: %s",
                    strerror (e));
      return TS_ERR_SYSTEM;
    }
  if (pid == 0)
    {
      /* Child: new session, controlling terminal, stdio on slave. */
      int child_error;
      close (errors[0]);
      if (setsid () < 0 || ioctl (slave, TIOCSCTTY, 0) < 0 ||
          dup2 (slave, 0) < 0 || dup2 (slave, 1) < 0 || dup2 (slave, 2) < 0)
        goto child_failed;
      if (slave > 2)
        close (slave);
      close (master);
      execl ("/bin/sh", "sh", "-c", cmd, (char *) NULL);
child_failed:
      child_error = errno;
      while (write (errors[1], &child_error, sizeof child_error) < 0 && errno == EINTR) ;
      _exit (127);
    }
  close (slave);
  close (errors[1]);
  {
    int child_error = 0;
    ssize_t got;
    do { got = read (errors[0], &child_error, sizeof child_error); } while (got < 0 && errno == EINTR);
    int e = got < 0 ? errno : got ? child_error : 0;
    close (errors[0]);
    if (got)
      {
        int st;
        if (kill (-pid, SIGKILL) < 0) kill (pid, SIGKILL);
        while (waitpid (pid, &st, 0) < 0 && errno == EINTR) ;
        close (master); errno = e ? e : EIO;
        return ts_std_system (error, "spawn child setup/exec");
      }
  }
  h = calloc (1, sizeof *h);
  if (!h)
    {
      int st;
      if (kill (-pid, SIGKILL) < 0) kill (pid, SIGKILL);
      while (waitpid (pid, &st, 0) < 0 && errno == EINTR)
        ;
      close (master);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  h->master = master;
  h->pid = pid;
  h->status = "ok";
  snprintf (desc, sizeof desc, "<pty pid=%d>", (int) pid);
  if (ts_value_make_handle (ret, h, pty_free, desc, &pty_tag_id) !=
      TS_OK)
    {
      pty_free (h);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
p_read (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  int64_t max = 65536;
  char *buf;
  ssize_t r;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "read") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "read");
  if (!h)
    return TS_ERR_INVAL;
  if (h->master < 0)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  if (argc == 2 && ts_std_int (&argv[1], &max, error, "read") != TS_OK)
    return TS_ERR_INVAL;
  if (max < 1 || max > 1000000)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "read: bad size");
      return TS_ERR_INVAL;
    }
  buf = malloc ((size_t) max + 1);
  if (!buf)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  for (;;)
    {
      r = read (h->master, buf, (size_t) max);
      if (r < 0 && errno == EINTR)
        continue;
      break;
    }
  if (r < 0 && errno != EIO && errno != EAGAIN && errno != EWOULDBLOCK)
    { free (buf); h->status = "system_error"; return ts_std_system (error, "pty read"); }
  if (r <= 0)
    {
      free (buf);
      h->status = r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? "would_block" : "eof";
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  buf[r] = '\0';
  if (memchr (buf, 0, (size_t) r)) { free (buf); return ts_std_inval (error, "pty read: embedded NUL"); }
  h->status = "ok";
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = buf;
  return TS_OK;
}

static TS_Status
p_write (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  const char *text;
  size_t n, sent = 0;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "write") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "write");
  if (!h)
    return TS_ERR_INVAL;
  if (h->master < 0)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  if (ts_std_str (&argv[1], &text, error, "write") != TS_OK)
    return TS_ERR_INVAL;
  n = strlen (text);
  while (sent < n)
    {
      ssize_t r = write (h->master, text + sent, n - sent);
      if (r < 0)
        {
          if (errno == EINTR)
            continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            { h->status = "would_block"; ts_std_ret_int (ret, (int64_t) sent); return TS_OK; }
          h->status = "system_error";
          ts_error_set (error, TS_ERR_SYSTEM, errno, sent, "pty write: %s", strerror (errno));
          return TS_ERR_SYSTEM;
        }
      if (!r) { errno = EIO; return ts_std_system (error, "pty write"); }
      sent += (size_t) r;
    }
  h->status = "ok";
  ts_std_ret_int (ret, (int64_t) sent);
  return TS_OK;
}

static TS_Status
p_wait (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  int st;
  pid_t r;
  if (ts_std_argc (vm, argc, 1, 1, error, "wait") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "wait");
  if (!h)
    return TS_ERR_INVAL;
  if (h->reaped)
    {
      st = h->wait_status;
      goto report;
    }
  do { r = waitpid (h->pid, &st, WNOHANG | WUNTRACED | WCONTINUED); } while (r < 0 && errno == EINTR);
  if (r < 0)
    {
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "wait: %s",
                    strerror (errno));
      return TS_ERR_SYSTEM;
    }
  if (!r)
    {
      ts_std_ret_nil (ret); /* Still running. */
      return TS_OK;
    }
  h->wait_status = st;
  h->reaped = WIFEXITED (st) || WIFSIGNALED (st);
  h->exit_code = WIFEXITED (st) ? WEXITSTATUS (st) :
                 WIFSIGNALED (st) ? 128 + WTERMSIG (st) : 127;
report:
  if (ud)
    {
      ts_dom_t *d = ts_dom_new_obj ();
      const char *state = WIFEXITED (st) ? "exited" : WIFSIGNALED (st) ? "signalled" : WIFSTOPPED (st) ? "stopped" : "continued";
      if (!d || ts_dom_obj_set (d, "state", ts_dom_new_str (state)) ||
          ts_dom_obj_set (d, "code", ts_dom_new_int (WIFEXITED (st) ? WEXITSTATUS (st) : 0)) ||
          ts_dom_obj_set (d, "signal", ts_dom_new_int (WIFSIGNALED (st) ? WTERMSIG (st) : WIFSTOPPED (st) ? WSTOPSIG (st) : 0)))
        { ts_dom_free (d); return ts_std_oom (error); }
      return ts_dom_wrap (ret, d, error);
    }
  if (h->reaped) ts_std_ret_int (ret, h->exit_code); else ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
p_close (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  int st = 0;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "close") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "close");
  if (!h)
    return TS_ERR_INVAL;
  if (!h->reaped && h->pid > 0)
    {
      if (kill (-h->pid, SIGKILL) < 0) kill (h->pid, SIGKILL);
      while (waitpid (h->pid, &st, 0) < 0 && errno == EINTR)
        ;
      h->reaped = true;
      h->wait_status = st;
      h->exit_code = WIFEXITED (st) ? WEXITSTATUS (st) : WIFSIGNALED (st) ? 128 + WTERMSIG (st) : 127;
    }
  if (h->master >= 0)
    {
      close (h->master);
      h->master = -1;
    }
  h->status = "closed";
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
p_state (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  pty_t *h;
  intptr_t op = (intptr_t) ud;
  int64_t rows, cols;
  if (ts_std_argc (vm, n, op == 2 ? 2 : op == 3 ? 3 : 1, op == 2 ? 2 : op == 3 ? 3 : 1, e, "pty")) return TS_ERR_INVAL;
  h = pty_unwrap (a, e, "pty"); if (!h) return TS_ERR_INVAL;
  if (!op) return ts_std_ret_str (ret, h->status, e);
  if (h->master < 0) return ts_std_inval (e, "pty: closed session");
  if (op == 1) { ts_std_ret_int (ret, h->master); return TS_OK; }
  if (op == 2)
    {
      int flags = fcntl (h->master, F_GETFL);
      if (a[1].type != TS_BOOL) return ts_std_inval (e, "nonblocking: bool required");
      if (flags < 0 || fcntl (h->master, F_SETFL, a[1].as.boolean ? flags | O_NONBLOCK : flags & ~O_NONBLOCK)) return ts_std_system (e, "pty flags");
    }
  else
    {
      struct winsize ws = {0};
      if (ts_std_int (a + 1, &rows, e, "resize") || ts_std_int (a + 2, &cols, e, "resize") || rows < 1 || rows > 65535 || cols < 1 || cols > 65535)
        return ts_std_inval (e, "resize: dimensions must be 1..65535");
      ws.ws_row = (unsigned short) rows; ws.ws_col = (unsigned short) cols;
      if (ioctl (h->master, TIOCSWINSZ, &ws)) return ts_std_system (e, "pty resize");
    }
  ts_std_ret_bool (ret, true); return TS_OK;
}

static const TS_FuncDef pty_funcs[] = {
  { "spawn", p_spawn, NULL },
  { "read", p_read, NULL },
  { "write", p_write, NULL },
  { "wait", p_wait, NULL },
  { "wait_event", p_wait, (void *) 1 },
  { "status", p_state, NULL }, { "fd", p_state, (void *) 1 },
  { "nonblocking", p_state, (void *) 2 }, { "resize", p_state, (void *) 3 },
  { "close", p_close, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_pty_module = { "std.pty", pty_funcs };
