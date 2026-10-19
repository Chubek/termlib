/* Behavioral tests: Unicode boundaries, VT chunking, CSS ownership/cascade,
 * terminal restoration/cancellation, child lifecycle and VM concurrency. */
#include "termscript/stdlib/ts_stdlib.h"
#include "termscript/stdlib/common/ts_std_common.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit (1); } } while (0)
static TS_VM *vm;
static TS_Error error;
static TS_Value S (const char *s) { TS_Value v = {0}; v.type = TS_STRING; v.as.string = (char *) s; return v; }
static TS_Value I (int64_t i) { TS_Value v = {0}; v.type = TS_INT; v.as.integer = i; return v; }
static TS_Value B (bool b) { TS_Value v = {0}; v.type = TS_BOOL; v.as.boolean = b; return v; }
static TS_Status
invoke (const char *module, const char *function, TS_Value *ret, size_t argc, ...)
{
  TS_Value args[8];
  va_list ap;
  CHECK (argc <= 8);
  va_start (ap, argc);
  for (size_t i = 0; i < argc; i++) args[i] = va_arg (ap, TS_Value);
  va_end (ap);
  ts_value_free (ret); ts_error_clear (&error);
  TS_Value g = {0}, mod = {0}, name = S (module);
  TS_Status st = ts_rt_get (vm, "G", &g, 0, &error);
  CHECK (!st);
  if (!strcmp (module, "G")) CHECK (!ts_value_copy (&mod, &g));
  else CHECK (!ts_rt_call (vm, &g, "G", "load", &name, 1, &mod, 0, &error));
  st = ts_rt_call (vm, &mod, module, function, args, argc, ret, 0, &error);
  ts_value_free (&g); ts_value_free (&mod);
  return st;
}
#define OK(...) do { TS_Status st_ = invoke (__VA_ARGS__); if (st_) { fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, error.message); exit (1); } } while (0)
static void text_is (const TS_Value *v, const char *s) { CHECK (v && v->type == TS_STRING); if (strcmp (v->as.string, s)) { fprintf (stderr, "expected <%s>, got <%s>\n", s, v->as.string); exit (1); } }
static int master, slave;
static void
open_pty (void)
{
  char name[256];
  master = posix_openpt (O_RDWR | O_NOCTTY); CHECK (master >= 0);
  CHECK (!grantpt (master) && !unlockpt (master));
  CHECK (!ptsname_r (master, name, sizeof name));
  slave = open (name, O_RDWR | O_NOCTTY); CHECK (slave >= 0);
}

static void
unicode_tests (void)
{
  TS_Value v = {0}, c = {0}, screen = {0};
  OK ("std.utf8", "len", &v, 1, S ("سلام")); CHECK (v.as.integer == 4);
  OK ("std.utf8", "width", &v, 1, S ("界e\xcc\x81👩‍💻🇮🇷")); CHECK (v.as.integer == 7);
  OK ("std.utf8", "graphemes", &v, 1, S ("e\xcc\x81👩‍💻🇮🇷")); CHECK (ts_std_list_len (ts_std_list_unwrap (&v, &error, "test")) == 3);
  OK ("std.utf8", "slice", &v, 3, S ("سلام"), I (1), I (2)); text_is (&v, "لا");
  OK ("std.utf8", "reverse", &v, 1, S ("e\xcc\x81界")); text_is (&v, "界e\xcc\x81");
  OK ("std.utf8", "normalize", &v, 1, S ("e\xcc\x81")); text_is (&v, "é");
  OK ("std.utf8", "casefold", &v, 1, S ("Straße")); text_is (&v, "strasse");
  OK ("std.utf8", "valid", &v, 1, S ("\xc0\xaf")); CHECK (!v.as.boolean);
  CHECK (invoke ("std.utf8", "len", &v, 1, S ("a\xf0\x9f")) == TS_ERR_PARSE); CHECK (error.offset == 1);
  CHECK (invoke ("std.utf8", "char", &v, 1, I (0xd800)) == TS_ERR_INVAL);
  OK ("std.utf8", "sanitize", &v, 1, S ("a\xff")); text_is (&v, "a�");
  char cluster[132] = "x";
  for (size_t i = 1; i < 131; i += 2) { cluster[i] = '\xcc'; cluster[i + 1] = '\x81'; }
  cluster[131] = 0;
  CHECK (invoke ("std.cell", "new", &v, 1, S (cluster)) == TS_ERR_LIMIT);
  char *oversized = malloc (1048578); CHECK (oversized);
  memset (oversized, 'a', 1048577); oversized[1048577] = 0;
  CHECK (invoke ("std.utf8", "len", &v, 1, S (oversized)) == TS_ERR_LIMIT);
  free (oversized);
  OK ("std.prompt", "width", &v, 1, S ("\033]2;title\033\\\033[31mسلام\033[0m界")); CHECK (v.as.integer == 6);
  OK ("std.prompt", "width", &v, 1, S ("\001invisible\002界")); CHECK (v.as.integer == 2);
  OK ("std.style", "strip", &v, 1, S ("\033]8;;https://a\033\\سلام\033]8;;\033\\")); text_is (&v, "سلام");
  OK ("std.cell", "new", &c, 4, S ("界"), S ("#123456"), I (200), I (5));
  OK ("std.cell", "width", &v, 1, c); CHECK (v.as.integer == 2);
  OK ("std.cell", "foreground", &v, 1, c); CHECK (v.as.integer == 0x1123456);
  CHECK (invoke ("std.cell", "new", &v, 1, S ("ab")) == TS_ERR_INVAL);
  OK ("std.screen", "new", &screen, 2, I (2), I (4));
  OK ("std.screen", "put", &v, 4, screen, I (0), I (1), c);
  OK ("std.screen", "cell", &v, 3, screen, I (0), I (2));
  TS_Value tail = {0}; CHECK (!ts_value_copy (&tail, &v));
  CHECK (invoke ("std.screen", "fill", &v, 2, screen, tail) == TS_ERR_INVAL);
  ts_value_free (&tail);
  OK ("std.screen", "put", &v, 4, screen, I (0), I (2), S ("x"));
  OK ("std.screen", "line", &v, 2, screen, I (0)); text_is (&v, "  x");
  OK ("std.screen", "put", &v, 4, screen, I (1), I (2), c);
  OK ("std.screen", "resize", &v, 3, screen, I (2), I (3));
  OK ("std.screen", "line", &v, 2, screen, I (1)); text_is (&v, "");
  ts_value_free (&c); ts_value_free (&screen); ts_value_free (&v);
}

static void
vt_tests (void)
{
  TS_Value vt = {0}, v = {0}, c = {0}, rendered = {0}, other = {0};
  OK ("std.vterm", "new", &vt, 2, I (3), I (5));
  const char *input = "\033[1;3;38;2;12;34;56;48;5;200m界e\xcc\x81\033[22;23;39;49mX\r\nسلام";
  /* Every byte boundary is a possible transport chunk, including UTF-8. */
  for (size_t i = 0; input[i]; i++) { char byte[2] = {input[i], 0}; OK ("std.vterm", "feed", &v, 2, vt, S (byte)); }
  OK ("std.vterm", "cell", &c, 3, vt, I (0), I (0));
  OK ("std.cell", "text", &v, 1, c); text_is (&v, "界");
  OK ("std.cell", "foreground", &v, 1, c); CHECK (v.as.integer == (0x1000000 | 12 << 16 | 34 << 8 | 56));
  OK ("std.cell", "background", &v, 1, c); CHECK (v.as.integer == 200);
  OK ("std.cell", "attributes", &v, 1, c); CHECK (v.as.integer == 5);
  OK ("std.vterm", "cell", &c, 3, vt, I (0), I (2));
  OK ("std.cell", "text", &v, 1, c); text_is (&v, "e\xcc\x81");
  OK ("std.vterm", "text", &v, 1, vt); text_is (&v, "界e\xcc\x81X\nسلام\n\n");
  OK ("std.vterm", "render", &rendered, 1, vt);
  OK ("std.vterm", "new", &other, 2, I (3), I (5));
  OK ("std.vterm", "feed", &v, 2, other, rendered);
  OK ("std.vterm", "text", &v, 1, other); text_is (&v, "界e\xcc\x81X\nسلام\n\n");
  OK ("std.vterm", "cell", &c, 3, other, I (0), I (0));
  OK ("std.cell", "attributes", &v, 1, c); CHECK (v.as.integer == 5);
  OK ("std.vterm", "feed", &v, 2, vt, S ("\033[2;4H\033[2J"));
  OK ("std.vterm", "cursor", &v, 1, vt); text_is (&v, "1,3");
  OK ("std.vterm", "feed", &v, 2, vt, S ("\t\033[0K\033[999999999999999999999999M"));
  OK ("std.vterm", "reset", &v, 1, vt);
  OK ("std.vterm", "feed", &v, 2, vt, S ("base\033[?1049halt\033[?1049l"));
  OK ("std.vterm", "text", &v, 1, vt); text_is (&v, "base\n\n\n");
  OK ("std.vterm", "feed", &v, 2, vt, S ("\033]2;ignored"));
  OK ("std.vterm", "feed", &v, 2, vt, S ("\033\\\033[?25l"));
  OK ("std.vterm", "cursor_visible", &v, 1, vt); CHECK (!v.as.boolean);
  OK ("std.vterm", "reset", &v, 1, vt);
  OK ("std.vterm", "feed", &v, 2, vt, S ("\xff\xc0\xaf\033[38:2::255:0:0mX"));
  OK ("std.vterm", "cell", &c, 3, vt, I (0), I (3));
  OK ("std.cell", "foreground", &v, 1, c); CHECK (v.as.integer == 0x1ff0000);
  ts_value_free (&vt); ts_value_free (&v); ts_value_free (&c); ts_value_free (&rendered); ts_value_free (&other);
}

static void
html_tests (void)
{
  TS_Value doc = {0}, css = {0}, node = {0}, v = {0}, style = {0};
  OK ("std.html", "parse", &doc, 1, S ("<div class='parent' style='color: green'><span id='x'>سلام &amp; é</span><span>two</span></div>"));
  OK ("std.css", "parse", &css, 1, S (".parent {font-style: italic} span {color: blue} #x {color: #123456 !important; background-color: rgb(12,34,56); font-weight: bold; text-decoration-line: underline}"));
  OK ("std.css", "apply", &v, 2, css, doc);
  OK ("std.html", "query", &node, 2, doc, S ("div.parent > span#x")); CHECK (node.type == TS_HANDLE);
  ts_value_free (&css); ts_value_free (&doc); /* Node keeps both alive. */
  OK ("std.html", "text", &v, 1, node); text_is (&v, "سلام & é");
  OK ("std.css", "get", &v, 2, node, S ("color")); text_is (&v, "#123456");
  OK ("std.css", "style", &style, 1, node);
  OK ("std.cell", "attributes", &v, 1, style); CHECK (v.as.integer == 13);
  OK ("std.cell", "background", &v, 1, style); CHECK (v.as.integer == 0x10c2238);
  OK ("std.html", "render", &v, 3, node, I (20), B (false)); text_is (&v, "سلام & é");
  OK ("std.html", "set_attr", &v, 3, node, S ("style"), S ("color: red !important"));
  OK ("std.css", "style", &style, 1, node);
  OK ("std.cell", "foreground", &v, 1, style); CHECK (v.as.integer == 0x1ff0000);
  OK ("std.html", "query_all", &v, 2, node, S ("not-found")); CHECK (ts_std_list_len (ts_std_list_unwrap (&v, &error, "test")) == 0);
  CHECK (invoke ("std.html", "query", &v, 2, node, S ("[")) == TS_ERR_PARSE);
  OK ("std.html", "escape", &v, 1, S ("<&\"'")); text_is (&v, "&lt;&amp;&quot;&#39;");
  ts_value_free (&node); ts_value_free (&v); ts_value_free (&style);
}

static void
bidi_tui_tests (void)
{
  TS_Value v = {0}, t = {0}, c = {0}, frame = {0}, vt = {0}, map = {0};
  OK ("std.bidi", "direction", &v, 1, S ("سلام world")); text_is (&v, "rtl");
  OK ("std.bidi", "visual", &v, 1, S ("سلام")); CHECK (strcmp (v.as.string, "سلام"));
  CHECK (!ts_value_copy (&frame, &v));
  OK ("std.utf8", "valid", &v, 1, frame); CHECK (v.as.boolean);
  OK ("std.bidi", "logical_to_visual", &map, 2, S ("abc אבג"), S ("ltr"));
  ts_std_list_t *l = ts_std_list_unwrap (&map, &error, "test"); CHECK (ts_std_list_len (l) == 7); CHECK (ts_std_list_get (l, 3)->as.integer == 3); CHECK (ts_std_list_get (l, 4)->as.integer == 6);
  OK ("std.tui", "new", &t, 2, I (3), I (12));
  OK ("std.tui", "box", &v, 6, t, I (0), I (0), I (3), I (12), S ("hi"));
  OK ("std.cell", "new", &c, 4, S (" "), S ("red"), S ("blue"), I (1));
  OK ("std.tui", "put_visual", &v, 5, t, I (1), I (2), S ("سلام"), c);
  OK ("std.tui", "render", &frame, 1, t); CHECK (strstr (frame.as.string, "\033[0;1;31;44m"));
  OK ("std.vterm", "new", &vt, 2, I (3), I (12));
  OK ("std.vterm", "feed", &v, 2, vt, frame);
  OK ("std.vterm", "cell", &c, 3, vt, I (1), I (2));
  OK ("std.cell", "foreground", &v, 1, c); CHECK (v.as.integer == 1);
  OK ("std.tui", "render", &v, 1, t); text_is (&v, "");
  OK ("std.tui", "resize", &v, 3, t, I (2), I (5));
  OK ("std.tui", "render", &v, 1, t); CHECK (strlen (v.as.string) > 0);
  OK ("std.tui", "screen", &c, 1, t);
  OK ("std.screen", "resize", &v, 3, c, I (4), I (20));
  OK ("std.tui", "render", &v, 1, t); CHECK (strlen (v.as.string) > 0);
  OK ("std.key", "event", &v, 1, S ("\033[<20;9;5M"));
  OK ("std.json", "get", &c, 2, v, S ("row")); CHECK (c.as.integer == 4);
  OK ("std.key", "parse", &v, 1, S ("ش")); text_is (&v, "ش");
  ts_value_free (&v); ts_value_free (&t); ts_value_free (&c); ts_value_free (&frame); ts_value_free (&vt); ts_value_free (&map);
}

static void
tty_tests (void)
{
  TS_Value state = {0}, terminal = {0}, v = {0}, tui = {0};
  struct termios original, current;
  struct winsize ws;
  open_pty (); CHECK (!tcgetattr (slave, &original));
  OK ("std.termios", "get", &state, 1, I (slave));
  OK ("std.termios", "raw", &v, 1, state);
  OK ("std.termios", "apply", &v, 1, state);
  CHECK (!tcgetattr (slave, &current)); CHECK (!(current.c_lflag & (ICANON | ECHO)));
  ts_value_free (&state); CHECK (!tcgetattr (slave, &current)); CHECK (current.c_lflag == original.c_lflag);
  OK ("std.baudrate", "set", &v, 2, I (slave), I (9600));
  OK ("std.baudrate", "get", &v, 1, I (slave)); CHECK (v.as.integer == 9600);
  CHECK (invoke ("std.baudrate", "set", &v, 2, I (slave), I (12345)) == TS_ERR_INVAL);
  CHECK (!tcsetattr (slave, TCSANOW, &original));
  OK ("std.terminal", "new", &terminal, 2, I (slave), I (slave));
  CHECK (invoke ("std.terminal", "nonblocking", &v, 1, terminal) == TS_ERR_INVAL);
  OK ("std.terminal", "raw", &v, 1, terminal);
  OK ("std.terminal", "read", &v, 3, terminal, I (32), I (5)); CHECK (v.type == TS_NIL);
  OK ("std.terminal", "status", &v, 1, terminal); text_is (&v, "timeout");
  OK ("std.terminal", "nonblocking", &v, 2, terminal, B (true));
  OK ("std.terminal", "read", &v, 1, terminal); CHECK (v.type == TS_NIL);
  OK ("std.terminal", "status", &v, 1, terminal); text_is (&v, "would_block");
  CHECK (write (master, "abc", 3) == 3);
  OK ("std.terminal", "read", &v, 3, terminal, I (2), I (100)); text_is (&v, "ab");
  OK ("std.terminal", "read", &v, 3, terminal, I (2), I (100)); text_is (&v, "c");
  OK ("std.terminal", "cancel", &v, 1, terminal);
  OK ("std.terminal", "read", &v, 3, terminal, I (32), I (-1)); CHECK (v.type == TS_NIL);
  OK ("std.terminal", "status", &v, 1, terminal); text_is (&v, "cancelled");
  OK ("std.terminal", "reset_cancel", &v, 1, terminal);
  OK ("std.terminal", "resize", &v, 3, terminal, I (37), I (91)); CHECK (!ioctl (slave, TIOCGWINSZ, &ws)); CHECK (ws.ws_row == 37 && ws.ws_col == 91);
  OK ("std.tui", "new", &tui, 2, I (2), I (10));
  OK ("std.tui", "begin", &v, 2, tui, terminal);
  OK ("std.tui", "put", &v, 4, tui, I (0), I (0), S ("سلام"));
  OK ("std.tui", "present", &v, 1, tui);
  OK ("std.terminal", "cancel", &v, 1, terminal);
  ts_value_free (&tui); CHECK (!tcgetattr (slave, &current)); CHECK (current.c_lflag == original.c_lflag);
  OK ("std.terminal", "close", &v, 1, terminal);
  OK ("std.terminal", "close", &v, 1, terminal);
  CHECK (!(fcntl (slave, F_GETFL) & O_NONBLOCK));
  ts_value_free (&terminal); ts_value_free (&v); close (master); close (slave);
  int pipefd[2]; CHECK (!pipe (pipefd));
  OK ("std.terminal", "new", &terminal, 2, I (pipefd[0]), I (pipefd[1]));
  /* Session owns duplicates; closing the caller's input is safe. */
  close (pipefd[0]);
  OK ("std.terminal", "write", &v, 2, terminal, S ("data")); CHECK (v.as.integer == 4);
  OK ("std.terminal", "read", &v, 3, terminal, I (32), I (100)); text_is (&v, "data");
  CHECK (invoke ("std.terminal", "raw", &v, 1, terminal) == TS_ERR_INVAL);
  ts_value_free (&terminal); ts_value_free (&v); close (pipefd[1]);
  int pair[2]; CHECK (!socketpair (AF_UNIX, SOCK_STREAM, 0, pair));
  CHECK (fcntl (pair[0], F_SETFL, O_NONBLOCK) == 0);
  OK ("std.terminal", "new", &terminal, 2, I (pair[0]), I (pair[0]));
  char *large = malloc (1048577); CHECK (large);
  memset (large, 'x', 1048576); large[1048576] = 0;
  OK ("std.terminal", "write", &v, 2, terminal, S (large));
  CHECK (v.as.integer > 0 && v.as.integer < 1048576);
  OK ("std.terminal", "write", &v, 2, terminal, S (large)); CHECK (v.as.integer == 0);
  OK ("std.terminal", "status", &v, 1, terminal); text_is (&v, "would_block");
  CHECK (invoke ("std.terminal", "write_all", &v, 3, terminal, S ("x"), I (5)) == TS_ERR_TIMEOUT);
  /* Drain outgoing data before the peer closes, ensuring an orderly EOF. */
  CHECK (!fcntl (pair[1], F_SETFL, O_NONBLOCK));
  char drain[4096]; while (read (pair[1], drain, sizeof drain) > 0) ;
  close (pair[1]);
  OK ("std.terminal", "read", &v, 1, terminal); CHECK (v.type == TS_NIL);
  OK ("std.terminal", "status", &v, 1, terminal); text_is (&v, "eof");
  free (large); ts_value_free (&terminal); ts_value_free (&v); close (pair[0]);
}

static void
wait_pty_event (const TS_Value *child, TS_Value *event)
{
  struct timespec start, now;
  CHECK (clock_gettime (CLOCK_MONOTONIC, &start) == 0);
  for (;;)
    {
      OK ("std.pty", "wait_event", event, 1, *child);
      if (event->type != TS_NIL) return;
      CHECK (clock_gettime (CLOCK_MONOTONIC, &now) == 0);
      int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000 + now.tv_nsec - start.tv_nsec;
      CHECK (elapsed < INT64_C(2000000000));
      struct timespec pause = {0, 1000000};
      while (nanosleep (&pause, &pause) < 0 && errno == EINTR) {}
    }
}

static void
pty_tests (void)
{
  TS_Value child = {0}, v = {0}, event = {0};
  struct rlimit original, limited;
  CHECK (!getrlimit (RLIMIT_NOFILE, &original));
  limited = original; limited.rlim_cur = 6;
  CHECK (!setrlimit (RLIMIT_NOFILE, &limited));
  TS_Status failed = invoke ("std.pty", "spawn", &child, 1, S ("true"));
  CHECK (!setrlimit (RLIMIT_NOFILE, &original));
  CHECK (failed == TS_ERR_SYSTEM && error.system_errno == EMFILE);
  OK ("std.pty", "spawn", &child, 1, S ("printf hi; exit 7"));
  OK ("std.pty", "read", &v, 1, child); text_is (&v, "hi");
  OK ("std.pty", "read", &v, 1, child); CHECK (v.type == TS_NIL);
  wait_pty_event (&child, &event);
  CHECK (event.type == TS_HANDLE);
  OK ("std.json", "get", &v, 2, event, S ("code")); CHECK (v.as.integer == 7);
  OK ("std.pty", "close", &v, 1, child); OK ("std.pty", "close", &v, 1, child); ts_value_free (&child);
  OK ("std.pty", "spawn", &child, 1, S ("kill -STOP $$; exit 0"));
  wait_pty_event (&child, &event);
  CHECK (event.type == TS_HANDLE);
  OK ("std.json", "get", &v, 2, event, S ("state")); text_is (&v, "stopped");
  OK ("std.pty", "close", &v, 1, child); ts_value_free (&child);
  OK ("std.pty", "spawn", &child, 1, S ("kill -TERM $$"));
  wait_pty_event (&child, &event);
  OK ("std.json", "get", &v, 2, event, S ("signal")); CHECK (v.as.integer == SIGTERM);
  ts_value_free (&child); ts_value_free (&v); ts_value_free (&event);
}

static void
other_tests (void)
{
  TS_Value v = {0}, map = {0}, font = {0}, rect = {0};
  OK ("std.map", "new", &map, 0);
  OK ("std.map", "set", &v, 3, map, S ("first"), I (1));
  OK ("std.map", "set", &v, 3, map, S ("middle"), I (2));
  OK ("std.map", "set", &v, 3, map, S ("last"), I (3));
  OK ("std.map", "del", &v, 2, map, S ("first"));
  OK ("std.map", "keys", &v, 1, map);
  text_is (&v, "middle,last");
  OK ("std.map", "set", &v, 3, map, S ("label"), S ("سلام"));
  OK ("std.prompt_string", "expand", &v, 2, S ("${label} $$"), map); text_is (&v, "سلام $");
  CHECK (invoke ("std.prompt_string", "expand", &v, 1, S ("${bad")) == TS_ERR_PARSE);
  OK ("std.layout", "rect", &rect, 4, I (1), I (2), I (5), I (11));
  OK ("std.layout", "split", &v, 4, rect, S ("horizontal"), I (3), I (1)); CHECK (ts_std_list_len (ts_std_list_unwrap (&v, &error, "test")) == 3);
  CHECK (invoke ("std.layout", "inset", &v, 2, rect, I (INT64_MAX)) == TS_ERR_INVAL);
  OK ("std.layout", "align", &v, 3, S ("界"), I (5), S ("center")); text_is (&v, " 界  ");
  OK ("std.layout", "align", &v, 2, S ("\ttext"), I (3)); text_is (&v, "   ");
  CHECK (invoke ("G", "add", &v, 2, I (INT64_MAX), I (1)) == TS_ERR_PARSE);
  CHECK (invoke ("G", "mul", &v, 2, I (INT64_MIN), I (-1)) == TS_ERR_PARSE);
  CHECK (invoke ("G", "div", &v, 2, I (INT64_MIN), I (-1)) == TS_ERR_PARSE);
  OK ("G", "mod", &v, 2, I (INT64_MIN), I (-1)); CHECK (v.as.integer == 0);
  OK ("std.font", "match", &font, 1, S ("monospace:lang=fa"));
  if (font.type != TS_NIL)
    {
      OK ("std.font", "file", &v, 1, font); CHECK (access (v.as.string, R_OK) == 0);
      OK ("std.font", "shape", &v, 4, font, S ("سلام"), S ("rtl"), S ("fa"));
      ts_std_list_t *glyphs = ts_std_list_unwrap (&v, &error, "test"); CHECK (ts_std_list_len (glyphs) > 0);
      const ts_dom_t *glyph = ts_dom_unwrap (ts_std_list_get (glyphs, 0), &error, "glyph");
      TS_Value advance = {0}; CHECK (!ts_dom_to_value (ts_dom_key (glyph, "x_advance"), &advance, &error));
      CHECK (advance.type == TS_INT && advance.as.integer > 0); ts_value_free (&advance);
    }
  ts_value_free (&v); ts_value_free (&map); ts_value_free (&font); ts_value_free (&rect);
}

static void
socket_tests (void)
{
  TS_Value server = {0}, client = {0}, peer = {0}, v = {0};
  struct sockaddr_in address; socklen_t size = sizeof address;
  OK ("std.socket", "listen", &server, 1, I (0));
  OK ("std.socket", "fd", &v, 1, server);
  CHECK (!getsockname ((int) v.as.integer, (struct sockaddr *) &address, &size));
  OK ("std.socket", "nonblocking", &v, 2, server, B (true));
  CHECK (invoke ("std.socket", "accept", &v, 1, server) == TS_ERR_WOULD_BLOCK);
  OK ("std.socket", "nonblocking", &v, 2, server, B (false));
  OK ("std.socket", "set_timeout", &v, 2, server, I (5));
  CHECK (invoke ("std.socket", "accept", &v, 1, server) == TS_ERR_TIMEOUT);
  OK ("std.socket", "connect", &client, 2, S ("127.0.0.1"), I (ntohs (address.sin_port))); CHECK (client.type == TS_HANDLE);
  OK ("std.socket", "accept", &peer, 1, server); CHECK (peer.type == TS_HANDLE);
  CHECK (invoke ("std.socket", "recv", &v, 1, peer) == TS_ERR_TIMEOUT);
  OK ("std.socket", "send", &v, 2, client, S ("hello")); CHECK (v.as.integer == 5);
  OK ("std.socket", "recv", &v, 2, peer, I (2)); text_is (&v, "he");
  OK ("std.socket", "recv", &v, 1, peer); text_is (&v, "llo");
  OK ("std.socket", "nonblocking", &v, 2, peer, B (true));
  CHECK (invoke ("std.socket", "recv", &v, 1, peer) == TS_ERR_WOULD_BLOCK);
  OK ("std.socket", "close", &v, 1, client); OK ("std.socket", "close", &v, 1, client);
  OK ("std.socket", "recv", &v, 1, peer); CHECK (v.type == TS_NIL);
  ts_value_free (&server); ts_value_free (&client); ts_value_free (&peer); ts_value_free (&v);
}

static void
parser_tests (void)
{
  const char *bad[] = {"const x = G:load \"std.utf8\"; G:puts(", "const x = \"\\x00\";", "const x = 99999999999999999999999;", "if true do G:puts \"x\";", "value = ;"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    {
      char *out = NULL;
      CHECK (ts_vm_run_string (vm, bad[i], &out, &error) == TS_ERR_PARSE);
      free (out);
    }
  char *out = NULL;
  CHECK (!ts_vm_run_string (vm, "const x = 1; if true do G:puts x; else G:puts 0; end", &out, &error));
  CHECK (!strcmp (out, "1\n")); free (out);
  CHECK (!ts_vm_run_string (vm, "const paren_x = G:add(40, 2); G:puts(paren_x); const paren_g = G:load(\"G\"); G:puts(paren_g);", &out, &error));
  CHECK (!strcmp (out, "42\n<module G>\n")); free (out);
}

static void
backend_api_tests (void)
{
  TS_CompileOptions options;
  const char *code;
  ts_compile_options_init (&options);
  CHECK (options.struct_size == sizeof options);
  CHECK (options.version == TS_COMPILE_OPTIONS_VERSION);
  CHECK (options.backend == TS_BACKEND_C);
  CHECK (!strcmp (ts_backend_name (TS_BACKEND_C), "c"));
  CHECK (!strcmp (ts_backend_name (TS_BACKEND_QBE), "qbe"));
  CHECK (!strcmp (ts_backend_name (TS_BACKEND_JITERATI), "jiterati"));
  CHECK (ts_backend_available (TS_BACKEND_C));
  CHECK (ts_backend_available (TS_BACKEND_QBE));
  bool have_jiterati = ts_backend_available (TS_BACKEND_JITERATI);
  options.unit_name = "backend_api";
  ts_error_clear (&error);
  code = ts_vm_compile_backend (vm, "G:puts(\"api\");", &options, &error);
  CHECK (code && strstr (code, "Termscript C backend"));
  options.backend = TS_BACKEND_QBE;
  code = ts_vm_compile_backend (vm, "G:puts(\"api\");", &options, &error);
  CHECK (code && strstr (code, "ts_qbe_run"));
  options.backend = TS_BACKEND_JITERATI;
  CHECK (!ts_vm_compile_backend (vm, "G:puts(\"api\");", &options, &error));
  CHECK (error.code == TS_ERR_UNSUPPORTED);
  char *out = NULL;
  if (have_jiterati)
    {
      CHECK (!ts_vm_run_backend (vm, "const j = G:add(40, 2); G:puts(j);",
                                 TS_BACKEND_JITERATI, &out, &error));
      CHECK (!strcmp (out, "42\n")); free (out); out = NULL;
      CHECK (!ts_vm_run_backend (vm,
        "const a = G:sub(44, 2); G:puts(a);"
        "const b = G:mul(6, 7); G:puts(b);"
        "const c = G:div(85, 2); G:puts(c);"
        "const d = G:mod(85, 2); G:puts(d);"
        "if G:eq(a, b) do G:puts(0); else G:puts(1); end"
        "z = 0; while G:lt(z, 2) do z = G:add(z, 1); G:puts(z); end"
        "const fallback = nil or G:add(1, 2); G:puts(fallback);",
        TS_BACKEND_JITERATI, &out, &error));
      CHECK (!strcmp (out, "42\n42\n42\n1\n0\n1\n2\n3\n")); free (out); out = NULL;
      CHECK (ts_vm_run_backend (vm, "const j = G:div(1, 0); G:puts(j);",
                                TS_BACKEND_JITERATI, &out, &error) == TS_ERR_PARSE);
    }
  else
    CHECK (ts_vm_run_backend (vm, "const j = G:add(40, 2); G:puts(j);",
                              TS_BACKEND_JITERATI, &out, &error) == TS_ERR_UNSUPPORTED);
  CHECK (ts_vm_run_backend (vm, "G:puts(\"api\");", TS_BACKEND_C,
                            &out, &error) == TS_ERR_UNSUPPORTED);
  options.backend = TS_BACKEND_C;
  options.reserved[0] = 1;
  CHECK (!ts_vm_compile_backend (vm, "G:puts(\"api\");", &options, &error));
  CHECK (error.code == TS_ERR_UNSUPPORTED);
}

static void *
worker (void *unused)
{
  TS_Error e; char *out = NULL; TS_VM *local = ts_vm_create (&e);
  (void) unused;
  CHECK (local); CHECK (!ts_stdlib_register_all (local, &e));
  CHECK (!ts_vm_run_string (local, "const U = G:load \"std.utf8\"; const n = U:width \"سلام界\"; G:puts n;", &out, &e));
  CHECK (!strcmp (out, "6\n")); free (out); ts_vm_free (local); return NULL;
}

int
main (void)
{
  vm = ts_vm_create (&error); CHECK (vm);
  CHECK (!ts_stdlib_register_all (vm, &error)); CHECK (!ts_stdlib_search_path (vm, &error));
  CHECK (ts_stdlib_module_count () == TS_STDLIB_NMODULES);
  for (size_t i = 0; i < ts_stdlib_module_count (); i++)
    {
      const char *name = ts_stdlib_module_at (i)->name + 4;
      if (!strcmp (name, "tinfo")) name = "terminfo";
      CHECK (!ts_vm_import (vm, name, &error));
    }
  unicode_tests (); vt_tests (); html_tests (); bidi_tui_tests (); tty_tests (); pty_tests (); other_tests (); socket_tests (); parser_tests (); backend_api_tests ();
  pthread_t threads[2];
  for (int i = 0; i < 2; i++) CHECK (!pthread_create (&threads[i], NULL, worker, NULL));
  for (int i = 0; i < 2; i++) CHECK (!pthread_join (threads[i], NULL));
  ts_vm_free (vm); puts ("Termscript stdlib behavioral tests passed"); return 0;
}
