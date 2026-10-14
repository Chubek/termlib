/* std.shell behavior across the C extension ABI and real child processes. */
#include "termscript/stdlib/ts_stdlib.h"
#include "termscript/stdlib/common/ts_std_common.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit (1); } } while (0)
static TS_VM *vm;
static TS_Error error;
static char *self;
static TS_Value S (const char *s) { TS_Value v = {0}; v.type = TS_STRING; v.as.string = (char *) s; return v; }
static TS_Value I (int64_t i) { TS_Value v = {0}; v.type = TS_INT; v.as.integer = i; return v; }
static TS_Value B (bool b) { TS_Value v = {0}; v.type = TS_BOOL; v.as.boolean = b; return v; }

static TS_Status
invoke (const char *function, TS_Value *ret, size_t argc, ...)
{
  TS_Value args[8];
  va_list ap;
  CHECK (argc <= 8);
  va_start (ap, argc);
  for (size_t i = 0; i < argc; i++) args[i] = va_arg (ap, TS_Value);
  va_end (ap);
  ts_value_free (ret); ts_error_clear (&error);
  const TS_FuncDef *f = ts_module_find_func (&ts_std_shell_module, function);
  if (f) return f->func (vm, f->userdata, args, argc, ret, &error);
  CHECK (0); return TS_ERR_NOTFOUND;
}
#define OK(...) do { TS_Status st_ = invoke (__VA_ARGS__); if (st_) { fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, error.message); exit (1); } } while (0)
static void text_is (const TS_Value *v, const char *s) { CHECK (v->type == TS_STRING); if (strcmp (v->as.string, s)) { fprintf (stderr, "expected <%s>, got <%s>\n", s, v->as.string); exit (1); } }

static void
field_is (const TS_Value *report, const char *path, TS_Value expected)
{
  TS_Value field = {0};
  ts_dom_t *d = ts_dom_unwrap (report, &error, "report"); CHECK (d);
  CHECK (!ts_dom_to_value (ts_dom_path (d, path), &field, &error));
  CHECK (field.type == expected.type);
  if (field.type == TS_STRING) text_is (&field, expected.as.string);
  else if (field.type == TS_INT) CHECK (field.as.integer == expected.as.integer);
  else if (field.type == TS_BOOL) CHECK (field.as.boolean == expected.as.boolean);
  ts_value_free (&field);
}

static TS_Value
list (size_t argc, ...)
{
  ts_std_list_t *l = ts_std_list_new (); CHECK (l);
  va_list ap; va_start (ap, argc);
  for (size_t i = 0; i < argc; i++) { TS_Value v = va_arg (ap, TS_Value); CHECK (!ts_std_list_push (l, &v)); }
  va_end (ap);
  TS_Value result = {0}; CHECK (!ts_std_list_wrap (&result, l, &error)); return result;
}

static int
fd_count (void)
{
  int count = 0;
  for (int i = 0; i < 1024; i++) if (fcntl (i, F_GETFD) >= 0) count++;
  return count;
}

static void
state_tests (void)
{
  TS_Value sh = {0}, other = {0}, v = {0}, report = {0}, args = {0};
  char cwd[4096], current[4096]; CHECK (getcwd (cwd, sizeof cwd));
  const char *host_value = getenv ("TERMSCRIPT_SHELL_TEST_VALUE");
  char *host = host_value ? strdup (host_value) : NULL;
  CHECK (!host_value || host);
  OK ("new", &sh, 1, B (false)); OK ("new", &other, 0);
  OK ("get", &v, 2, sh, S ("PATH")); CHECK (v.type == TS_NIL);
  OK ("set", &v, 3, sh, S ("TERMSCRIPT_SHELL_TEST_VALUE"), S ("one two"));
  CHECK (host ? !strcmp (host, getenv ("TERMSCRIPT_SHELL_TEST_VALUE")) : !getenv ("TERMSCRIPT_SHELL_TEST_VALUE"));
  OK ("get", &v, 2, other, S ("TERMSCRIPT_SHELL_TEST_VALUE"));
  if (host) text_is (&v, host); else CHECK (v.type == TS_NIL);
  OK ("get", &v, 3, sh, S ("missing"), I (42)); CHECK (v.as.integer == 42);
  CHECK (invoke ("set", &v, 3, sh, S ("bad-name"), S ("x")) == TS_ERR_INVAL);
  OK ("capture", &v, 2, sh, S ("printf '%s' \"$TERMSCRIPT_SHELL_TEST_VALUE\"")); text_is (&v, "one two");
  OK ("capture", &v, 2, sh, S ("KEEP=unchanged; readonly KEEP"));
  CHECK (invoke ("set", &v, 3, sh, S ("KEEP"), S ("new")) == TS_ERR_INVAL);
  CHECK (invoke ("unset", &v, 2, sh, S ("KEEP")) == TS_ERR_INVAL);
  OK ("get", &v, 2, sh, S ("KEEP")); text_is (&v, "unchanged");
  OK ("unset", &v, 2, sh, S ("missing"));
  args = list (2, S ("a b"), S ("c")); OK ("args", &v, 2, sh, args);
  OK ("capture", &v, 2, sh, S ("printf '%s|%s|%s' \"$0\" \"$1\" \"$2\"")); text_is (&v, "termscript|a b|c");
  OK ("result", &report, 1, sh); field_is (&report, "ok", B (true));
  OK ("run", &v, 2, sh, S ("false")); CHECK (v.as.integer == 1);
  field_is (&report, "code", I (0)); /* Report owns a snapshot. */
  OK ("status", &v, 1, sh); CHECK (v.as.integer == 1);
  OK ("capture", &v, 2, sh, S ("printf '%s' \"$?\"")); text_is (&v, "1");
  OK ("stdout", &v, 1, sh); text_is (&v, "1");
  OK ("capture", &v, 2, sh, S ("greet() { printf 'hi:%s' \"$1\"; }; greet friend")); text_is (&v, "hi:friend");
  OK ("capture", &v, 2, sh, S ("greet again")); text_is (&v, "hi:again");
  OK ("capture", &v, 2, sh, S ("for item in first second; do printf '%s,' \"$item\"; done")); text_is (&v, "first,second,");
  OK ("capture", &v, 2, sh, S ("if true; then printf yes; else printf no; fi")); text_is (&v, "yes");
  OK ("capture", &v, 2, sh, S ("printf '%s' \"$(printf '%s' nested)\"")); text_is (&v, "nested");
  CHECK (getcwd (current, sizeof current) && !strcmp (cwd, current));
  OK ("close", &v, 1, sh); OK ("close", &v, 1, sh);
  CHECK (invoke ("cwd", &v, 1, sh) == TS_ERR_INVAL);
  field_is (&report, "ok", B (true));
  ts_value_free (&sh); ts_value_free (&other); ts_value_free (&v); ts_value_free (&report); ts_value_free (&args);
  free (host);
}

static void
graph_tests (void)
{
  TS_Value sh = {0}, cmd = {0}, pipe = {0}, upper = {0}, v = {0}, word = {0}, args = {0}, seq = {0};
  OK ("new", &sh, 0);
  const char *literal = "$HOME; printf injected | * 'quoted'";
  args = list (4, S ("printf"), S ("<%s><%s>"), S (literal), S (""));
  OK ("command", &cmd, 1, args); ts_value_free (&args);
  OK ("capture", &v, 2, sh, cmd);
  char expected[256]; snprintf (expected, sizeof expected, "<%s><>", literal); text_is (&v, expected);
  OK ("set", &v, 3, sh, S ("WORDS"), S ("one two"));
  OK ("word", &word, 1, S ("\"$WORDS\""));
  OK ("command", &upper, 3, S ("printf"), S ("[%s]"), word); ts_value_free (&word);
  OK ("capture", &v, 2, sh, upper); text_is (&v, "[one two]");
  OK ("expand", &v, 2, sh, S ("$WORDS")); CHECK (ts_std_list_len (ts_std_list_unwrap (&v, &error, "expanded")) == 2);
  OK ("expand", &v, 2, sh, S ("\"$WORDS\"")); CHECK (ts_std_list_len (ts_std_list_unwrap (&v, &error, "expanded")) == 1);
  OK ("command", &cmd, 2, S ("printf"), S ("abc\n"));
  OK ("parse", &upper, 1, S ("tr a-z A-Z"));
  args = list (2, cmd, upper); OK ("pipeline", &pipe, 1, args); ts_value_free (&args); ts_value_free (&cmd); ts_value_free (&upper);
  OK ("capture", &v, 2, sh, pipe); text_is (&v, "ABC\n");
  OK ("parse", &cmd, 1, S ("false")); OK ("parse", &upper, 1, S ("printf fallback"));
  OK ("sequence", &seq, 3, cmd, upper, S ("and"));
  OK ("capture", &v, 2, sh, seq); text_is (&v, "");
  OK ("sequence", &seq, 3, cmd, upper, S ("or"));
  OK ("capture", &v, 2, sh, seq); text_is (&v, "fallback");
  CHECK (invoke ("pipeline", &v, 1, seq) == TS_ERR_INVAL);
  CHECK (invoke ("word", &v, 1, S ("one two")) == TS_ERR_INVAL);
  CHECK (invoke ("parse", &v, 1, S ("printf '")) == TS_ERR_PARSE);
  CHECK (invoke ("parse", &v, 1, S ("printf x >")) == TS_ERR_PARSE);
  CHECK (invoke ("command", &v, 1, S ("")) == TS_ERR_INVAL);
  CHECK (invoke ("command", &v, 1, sh) == TS_ERR_INVAL);
  CHECK (invoke ("timeout", &v, 2, sh, I (-1)) == TS_ERR_INVAL);
  CHECK (invoke ("pipefail", &v, 2, sh, I (1)) == TS_ERR_INVAL);
  CHECK (invoke ("new", &v, 1, I (1)) == TS_ERR_INVAL);
  char nested[1024];
  memset (nested, '(', 256); memcpy (nested + 256, "true", 4);
  memset (nested + 260, ')', 256); nested[516] = 0;
  CHECK (invoke ("parse", &v, 1, S (nested)) == TS_ERR_LIMIT);
  for (size_t i = 0; i < 130; i++) memcpy (nested + 5 * i, "true;", 5);
  nested[650] = 0;
  CHECK (invoke ("parse", &v, 1, S (nested)) == TS_ERR_LIMIT);
  char *oversized = malloc (1048578); CHECK (oversized);
  memset (oversized, 'x', 1048577); oversized[1048577] = 0;
  CHECK (invoke ("parse", &v, 1, S (oversized)) == TS_ERR_LIMIT); free (oversized);
  for (size_t i = 0; i < 128; i++)
    {
      TS_Status status = invoke ("sequence", &v, 2, seq, upper);
      if (status == TS_ERR_LIMIT) break;
      CHECK (status == TS_OK && i < 127);
      ts_value_free (&seq); seq = v; memset (&v, 0, sizeof v);
    }
  ts_value_free (&sh); ts_value_free (&cmd); ts_value_free (&pipe); ts_value_free (&upper);
  ts_value_free (&v); ts_value_free (&word); ts_value_free (&seq);
}

static void
process_tests (void)
{
  TS_Value sh = {0}, cmd = {0}, second = {0}, pipe = {0}, v = {0}, report = {0};
  OK ("new", &sh, 1, B (false));
  OK ("command", &cmd, 3, S (self), S ("--child"), S ("io"));
  OK ("capture", &v, 2, sh, cmd); CHECK (strlen (v.as.string) == 262144);
  OK ("stderr", &v, 1, sh); CHECK (strlen (v.as.string) == 262144);
  OK ("command", &cmd, 3, S (self), S ("--child"), S ("cat"));
  OK ("capture", &v, 3, sh, cmd, S ("input without newline")); text_is (&v, "input without newline");
  OK ("set", &v, 3, sh, S ("SHELL_EXPORTED"), S ("exported"));
  OK ("set", &v, 3, sh, S ("SHELL_PRIVATE"), S ("private"));
  OK ("export", &v, 2, sh, S ("SHELL_EXPORTED"));
  OK ("command", &cmd, 3, S (self), S ("--child"), S ("env"));
  OK ("capture", &v, 2, sh, cmd); text_is (&v, "exported|unset");
  OK ("export", &v, 3, sh, S ("SHELL_EXPORTED"), B (false));
  OK ("capture", &v, 2, sh, cmd); text_is (&v, "unset|unset");
  OK ("command", &cmd, 4, S (self), S ("--child"), S ("exit"), S ("7"));
  OK ("capture", &v, 2, sh, cmd); text_is (&v, "");
  OK ("result", &report, 1, sh); field_is (&report, "code", I (7)); field_is (&report, "signaled", B (false));
  OK ("command", &second, 4, S (self), S ("--child"), S ("exit"), S ("0"));
  OK ("pipeline", &pipe, 2, cmd, second);
  OK ("capture", &v, 2, sh, pipe);
  OK ("result", &report, 1, sh); field_is (&report, "code", I (0)); field_is (&report, "pipeline.0.code", I (7)); field_is (&report, "pipeline.1.code", I (0));
  OK ("pipefail", &v, 2, sh, B (true));
  OK ("capture", &v, 2, sh, pipe); OK ("status", &v, 1, sh); CHECK (v.as.integer == 7);
  OK ("command", &cmd, 3, S (self), S ("--child"), S ("signal"));
  OK ("capture", &v, 2, sh, cmd); OK ("result", &report, 1, sh);
  field_is (&report, "code", I (128 + SIGTERM)); field_is (&report, "signaled", B (true)); field_is (&report, "signal", I (SIGTERM));
  OK ("command", &cmd, 1, S ("/no/such/termscript-shell-command"));
  OK ("capture", &v, 2, sh, cmd); OK ("result", &report, 1, sh);
  field_is (&report, "code", I (127)); field_is (&report, "diagnostics.0.code", I (1001));
  OK ("command", &cmd, 3, S (self), S ("--child"), S ("wait"));
  OK ("timeout", &v, 2, sh, I (20));
  int fds = fd_count ();
  for (int i = 0; i < 3; i++)
    {
      OK ("capture", &v, 2, sh, cmd); OK ("result", &report, 1, sh);
      field_is (&report, "code", I (124)); field_is (&report, "timed_out", B (true));
      CHECK (fd_count () == fds); CHECK (waitpid (-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    }
  OK ("timeout", &v, 2, sh, I (0));
  struct rlimit original, limited; CHECK (!getrlimit (RLIMIT_NOFILE, &original));
  limited = original; limited.rlim_cur = 4; CHECK (!setrlimit (RLIMIT_NOFILE, &limited));
  TS_Status failed = invoke ("capture", &v, 2, sh, cmd);
  CHECK (!setrlimit (RLIMIT_NOFILE, &original));
  CHECK (failed == TS_ERR_SYSTEM && error.system_errno == EMFILE); CHECK (fd_count () == fds);
  OK ("output_limit", &v, 2, sh, I (3));
  CHECK (invoke ("capture", &v, 2, sh, S ("printf four")) == TS_ERR_LIMIT);
  OK ("command", &cmd, 3, S (self), S ("--child"), S ("io"));
  CHECK (invoke ("capture", &v, 2, sh, cmd) == TS_ERR_LIMIT);
  OK ("output_limit", &v, 2, sh, I (1048576));
  OK ("command", &cmd, 3, S (self), S ("--child"), S ("nul"));
  CHECK (invoke ("capture", &v, 2, sh, cmd) == TS_ERR_UNSUPPORTED);
  CHECK (invoke ("capture", &v, 2, sh, S ("printf '\\0'")) == TS_ERR_UNSUPPORTED);
  CHECK (fd_count () == fds);
  ts_value_free (&sh); ts_value_free (&cmd); ts_value_free (&second); ts_value_free (&pipe); ts_value_free (&v); ts_value_free (&report);
}

static void
redirection_tests (void)
{
  TS_Value sh = {0}, cmd = {0}, redirected = {0}, v = {0}, pipe = {0}, second = {0};
  char directory[4096], cwd[4096], current[4096];
  const char *tmpdir = getenv ("TMPDIR");
  CHECK (snprintf (directory, sizeof directory, "%s/termscript-shell-XXXXXX", tmpdir ? tmpdir : P_tmpdir) < (int) sizeof directory);
  CHECK (getcwd (cwd, sizeof cwd)); CHECK (mkdtemp (directory));
  OK ("new", &sh, 0); OK ("cd", &v, 2, sh, S (directory));
  CHECK (getcwd (current, sizeof current) && !strcmp (cwd, current));
  OK ("cwd", &v, 1, sh); text_is (&v, directory);
  OK ("get", &v, 2, sh, S ("PWD")); text_is (&v, directory);
  OK ("command", &cmd, 2, S ("printf"), S ("file"));
  OK ("redirect", &redirected, 3, cmd, S ("stdout"), S ("out"));
  OK ("capture", &v, 2, sh, redirected); text_is (&v, "");
  OK ("capture", &v, 2, sh, cmd); text_is (&v, "file"); /* Builder left cmd intact. */
  OK ("redirect", &redirected, 4, cmd, S ("stdout"), S ("out"), S ("append"));
  OK ("run", &v, 2, sh, redirected); CHECK (v.as.integer == 0);
  OK ("command", &cmd, 3, S (self), S ("--child"), S ("cat"));
  OK ("command", &second, 3, S (self), S ("--child"), S ("cat"));
  OK ("pipeline", &pipe, 2, cmd, second);
  OK ("redirect", &redirected, 3, pipe, S ("stdin"), S ("out"));
  OK ("capture", &v, 2, sh, redirected); text_is (&v, "filefile");
  char borrowed_path[8192], script[128];
  snprintf (borrowed_path, sizeof borrowed_path, "%s/borrowed", directory);
  int borrowed_fd = open (borrowed_path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600); CHECK (borrowed_fd >= 0);
  snprintf (script, sizeof script, "cat out >&%d", borrowed_fd);
  OK ("capture", &v, 2, sh, S (script)); text_is (&v, "");
  CHECK (fcntl (borrowed_fd, F_GETFD) >= 0); CHECK (!close (borrowed_fd)); CHECK (!unlink (borrowed_path));
  CHECK (invoke ("redirect", &v, 4, cmd, S ("stdin"), S ("out"), S ("append")) == TS_ERR_INVAL);
  OK ("redirect", &redirected, 3, pipe, S ("stdout"), S ("/no/such/termscript-shell-dir/out"));
  int fds = fd_count ();
  CHECK (invoke ("capture", &v, 2, sh, redirected) == TS_ERR_SYSTEM);
  CHECK (error.system_errno == ENOENT && fd_count () == fds);
  OK ("capture", &v, 2, sh, S ("printf recovered")); text_is (&v, "recovered");
  char path[8192]; snprintf (path, sizeof path, "%s/out", directory); CHECK (!unlink (path)); CHECK (!rmdir (directory));
  ts_value_free (&sh); ts_value_free (&cmd); ts_value_free (&redirected); ts_value_free (&v); ts_value_free (&pipe); ts_value_free (&second);
}

static void *
worker (void *unused)
{
  (void) unused;
  TS_Error e = {0}; TS_VM *local = ts_vm_create (&e); CHECK (local);
  CHECK (!ts_stdlib_register_all (local, &e)); char *out = NULL;
  CHECK (!ts_vm_run_string (local, "const S = G:load \"std.shell\"; const sh = S:new; S:set sh \"LOCAL\" \"independent\"; const s = S:capture sh \"printf '%s' \\\"$LOCAL\\\"\"; G:puts s;", &out, &e));
  CHECK (!strcmp (out, "independent\n")); free (out); ts_vm_free (local); return NULL;
}

static void
write_all (int fd, const char *data, size_t size)
{
  while (size)
    {
      ssize_t n = write (fd, data, size);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) _exit (2);
      data += n; size -= (size_t) n;
    }
}

static int
child (int argc, char **argv)
{
  if (!strcmp (argv[2], "exit")) return argc == 4 ? atoi (argv[3]) : 2;
  if (!strcmp (argv[2], "signal")) { raise (SIGTERM); return 2; }
  if (!strcmp (argv[2], "wait")) { for (;;) pause (); }
  if (!strcmp (argv[2], "env"))
    { printf ("%s|%s", getenv ("SHELL_EXPORTED") ? getenv ("SHELL_EXPORTED") : "unset", getenv ("SHELL_PRIVATE") ? getenv ("SHELL_PRIVATE") : "unset"); return 0; }
  if (!strcmp (argv[2], "nul")) { write_all (1, "a\0b", 3); return 0; }
  char data[4096];
  if (!strcmp (argv[2], "io"))
    { memset (data, 'x', sizeof data); for (int i = 0; i < 64; i++) { write_all (1, data, sizeof data); write_all (2, data, sizeof data); } return 0; }
  if (!strcmp (argv[2], "cat"))
    { ssize_t n; while ((n = read (0, data, sizeof data)) != 0) { if (n < 0 && errno == EINTR) continue; if (n < 0) return 2; write_all (1, data, (size_t) n); } return 0; }
  return 2;
}

int
main (int argc, char **argv)
{
  if (argc >= 3 && !strcmp (argv[1], "--child")) return child (argc, argv);
  self = realpath (argv[0], NULL); CHECK (self);
  vm = ts_vm_create (&error); CHECK (vm); CHECK (!ts_stdlib_register_all (vm, &error));
  state_tests (); graph_tests (); process_tests (); redirection_tests ();
  pthread_t threads[2];
  for (int i = 0; i < 2; i++) CHECK (!pthread_create (&threads[i], NULL, worker, NULL));
  for (int i = 0; i < 2; i++) CHECK (!pthread_join (threads[i], NULL));
  ts_vm_free (vm); free (self); puts ("Termscript shell behavioral tests passed"); return 0;
}
