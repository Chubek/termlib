#include "termscript/stdlib/ts_stdlib.h"
#include "termscript/stdlib/common/ts_std_common.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static TS_VM *vm;
static TS_Error error;
#define CHECK(c) do { if (!(c)) { fprintf (stderr, "%s:%d: %s (%s)\n", __FILE__, __LINE__, #c, error.message); exit (1); } } while (0)
static TS_Value I (int64_t i) { TS_Value v = {0}; v.type = TS_INT; v.as.integer = i; return v; }
static TS_Value S (const char *s) { TS_Value v = {0}; v.type = TS_STRING; v.as.string = (char *) s; return v; }
static TS_Status
call (const TS_Module *m, const char *name, TS_Value *r, size_t n, ...)
{
  TS_Value a[8]; va_list ap;
  CHECK (n <= 8); va_start (ap, n);
  for (size_t i = 0; i < n; i++) a[i] = va_arg (ap, TS_Value);
  va_end (ap); ts_value_free (r); ts_error_clear (&error);
  for (const TS_FuncDef *f = m->funcs; f->name; f++) if (!strcmp (f->name, name)) return f->func (vm, f->userdata, a, n, r, &error);
  CHECK (0); return TS_ERR_INVAL;
}
#define C(name,r,n,...) call (&ts_std_compositor_module,name,r,n,__VA_ARGS__)
#define E(name,r,n,...) call (&ts_std_emulator_module,name,r,n,__VA_ARGS__)
static int64_t pixel (const TS_Value *frame, size_t at)
{
  ts_std_list_t *l = ts_std_list_unwrap (frame, &error, "frame"); CHECK (l);
  const TS_Value *v = ts_std_list_get (l, at); CHECK (v && v->type == TS_INT); return v->as.integer;
}
static TS_Value document (const char *text)
{
  ts_dom_t *d = NULL; TS_Value v = {0};
  CHECK (!ts_dom_parse_json (text, &d, &error)); CHECK (!ts_dom_wrap (&v, d, &error)); return v;
}
static int64_t field (const TS_Value *v, const char *key)
{
  ts_dom_t *d = ts_dom_unwrap (v, &error, "event"); TS_Value x = {0}; CHECK (d);
  CHECK (!ts_dom_to_value (ts_dom_path (d,key), &x, &error)); CHECK (x.type == TS_INT);
  int64_t result = x.as.integer; ts_value_free (&x); return result;
}
static void
scene_tests (void)
{
  TS_Value c = {0}, other = {0}, a = {0}, b = {0}, r = {0}, frame = {0}, event = {0}, foreign = {0};
  CHECK (!C ("new", &c, 2, I (4), I (4)));
  CHECK (C ("new", &r, 2, I (32767), I (32767)) == TS_ERR_INVAL);
  CHECK (!C ("surface", &a, 3, c, I (4), I (4)));
  CHECK (!C ("fill", &r, 2, a, I (0xffff0000)));
  CHECK (!C ("present", &r, 1, c)); CHECK (r.as.integer == 1);
  CHECK (!C ("frame", &frame, 1, c)); CHECK (pixel (&frame,0) == 0); /* not committed */
  CHECK (!C ("commit", &r, 1, a)); CHECK (!C ("present", &r, 1, c)); CHECK (r.as.integer == 2);
  CHECK (!C ("present", &r, 1, c)); CHECK (r.as.integer == 2); /* no new damage */
  CHECK (!C ("surface", &b, 3, c, I (2), I (2)));
  CHECK (!C ("fill", &r, 2, b, I (0x800000ff)));
  CHECK (!C ("position", &r, 3, b, I (1), I (1))); CHECK (!C ("commit", &r, 1, b));
  CHECK (!C ("present", &r, 1, c)); CHECK (!C ("frame", &frame, 1, c));
  CHECK (pixel (&frame,0) == 0xff0000 && pixel (&frame,5) == 0x7f0080);
  CHECK (!C ("position", &r, 3, b, I (-1), I (-1))); CHECK (!C ("present", &r, 1, c));
  CHECK (!C ("frame", &frame, 1, c)); CHECK (pixel (&frame,0) == 0xff0000); /* position staged */
  CHECK (!C ("commit", &r, 1, b)); CHECK (!C ("present", &r, 1, c));
  CHECK (!C ("frame", &frame, 1, c)); CHECK (pixel (&frame,0) == 0x7f0080 && pixel (&frame,5) == 0xff0000);
  event = document ("{\"type\":\"button_press\",\"x\":0,\"y\":0,\"button\":1}");
  CHECK (!C ("inject", &r, 2, c, event)); ts_value_free (&event);
  CHECK (!C ("event", &event, 2, c, I (0))); CHECK (field (&event,"surface") == 2 && field (&event,"local_x") == 1);
  ts_value_free (&event); event = document ("{\"type\":\"key_press\",\"keysym\":65,\"text\":\"A\"}");
  CHECK (!C ("inject", &r, 2, c, event)); CHECK (!C ("event", &r, 2, c, I (0))); CHECK (field (&r,"surface") == 2);
  CHECK (!C ("raise", &r, 1, a)); CHECK (!C ("present", &r, 1, c));
  CHECK (!C ("frame", &frame, 1, c)); CHECK (pixel (&frame,0) == 0xff0000);
  CHECK (!C ("new", &other, 2, I (1), I (1))); CHECK (!C ("surface", &foreign, 3, other, I (1), I (1)));
  CHECK (C ("focus", &r, 2, c, foreign) == TS_ERR_INVAL);
  CHECK (C ("resize", &r, 3, a, I (0), I (4)) == TS_ERR_INVAL);
  CHECK (C ("fill", &r, 2, a, I (-1)) == TS_ERR_INVAL);
  for (int i = 0; i < 64; i++) CHECK (!C ("inject", &r, 2, c, event));
  CHECK (C ("inject", &r, 2, c, event) == TS_ERR_LIMIT);
  for (int i = 0; i < 64; i++) CHECK (!C ("event", &r, 2, c, I (0)));
  CHECK (!C ("event", &r, 2, c, I (0))); CHECK (r.type == TS_NIL);
  ts_value_free (&event); event = document ("{\"type\":1}");
  CHECK (C ("inject", &r, 2, c, event) == TS_ERR_INVAL);
  CHECK (!C ("destroy", &r, 1, b)); CHECK (!C ("destroy", &r, 1, b));
  CHECK (C ("commit", &r, 1, b) == TS_ERR_INVAL);
  CHECK (!C ("close", &r, 1, c)); CHECK (!C ("close", &r, 1, c));
  CHECK (C ("commit", &r, 1, a) == TS_ERR_INVAL);
  CHECK (pixel (&frame,0) == 0xff0000); /* independent frame snapshot */
  ts_value_free (&c); ts_value_free (&a); ts_value_free (&b);
  ts_value_free (&other); CHECK (!C ("fill", &r, 2, foreign, I (0xffffffff))); /* surface retains owner */
  ts_value_free (&foreign); ts_value_free (&r); ts_value_free (&frame); ts_value_free (&event);
}
static void
emulator_tests (void)
{
  TS_Value c = {0}, t = {0}, r = {0}, frame = {0}, surface = {0};
  CHECK (!C ("new", &c, 2, I (160), I (64)));
  CHECK (!E ("new", &t, 3, c, I (4), I (20)));
  CHECK (!E ("feed", &r, 2, t, S ("\033[31mRed\033[0m\r\nUnicode: 界")));
  CHECK (!E ("text", &r, 1, t)); CHECK (strstr (r.as.string,"Red") && strstr (r.as.string,"界"));
  CHECK (!E ("render", &r, 1, t)); CHECK (!C ("present", &r, 1, c)); CHECK (!C ("frame", &frame, 1, c));
  bool changed = false; int64_t bg = pixel (&frame,0);
  for (size_t i = 0; i < 160*64; i++) if (pixel (&frame,i) != bg) changed = true;
  CHECK (changed);
  CHECK (E ("resize", &r, 3, t, I (0), I (20)) == TS_ERR_INVAL);
  CHECK (E ("input", &r, 2, t, S ("x")) == TS_ERR_INVAL);
  CHECK (!E ("spawn", &r, 2, t, S ("stty size; printf '\\000NUL-OK\\n'; exec cat")));
  CHECK (E ("spawn", &r, 2, t, S ("true")) == TS_ERR_INVAL);
  int fd;
  CHECK (!E ("fd", &r, 1, t)); fd = (int) r.as.integer; CHECK (fcntl (fd,F_GETFL) & O_NONBLOCK);
  bool seen = false;
  for (int i = 0; i < 100 && !seen; i++)
    { CHECK (!E ("pump", &r, 2, t, I (20))); CHECK (!E ("text", &r, 1, t)); seen = strstr (r.as.string,"NUL-OK") != NULL; }
  if (!seen || !strstr (r.as.string,"4 20")) fprintf (stderr,"initial PTY screen: <%s>\n",r.as.string);
  CHECK (seen && strstr (r.as.string,"4 20"));
  CHECK (!E ("resize", &r, 3, t, I (5), I (25)));
  struct winsize ws; CHECK (!ioctl (fd,TIOCGWINSZ,&ws)); CHECK (ws.ws_row == 5 && ws.ws_col == 25);
  CHECK (!E ("input", &r, 2, t, S ("INPUT-OK\n")));
  seen = false;
  for (int i = 0; i < 100 && !seen; i++)
    { CHECK (!E ("pump", &r, 2, t, I (20))); CHECK (!E ("text", &r, 1, t)); seen = strstr (r.as.string,"INPUT-OK") != NULL; }
  CHECK (seen);
  CHECK (!E ("surface", &surface, 1, t)); CHECK (!C ("id", &r, 1, surface));
  TS_Value event = document ("{\"type\":\"key_press\",\"keysym\":65293,\"text\":\"\"}");
  CHECK (!C ("inject", &r, 2, c, event)); CHECK (!E ("step", &r, 2, t, I (0)));
  CHECK (field (&r,"surface") > 0);
  ts_value_free (&event); event = document ("{\"type\":\"resize\",\"width\":192,\"height\":96}");
  CHECK (!C ("inject", &r, 2, c, event)); CHECK (!E ("step", &r, 2, t, I (0)));
  CHECK (!ioctl (fd,TIOCGWINSZ,&ws)); CHECK (ws.ws_row == 6 && ws.ws_col == 24);
  ts_value_free (&event);
  char *large = malloc (65538); CHECK (large); memset (large,'x',65537); large[65537] = 0;
  CHECK (E ("input", &r, 2, t, S (large)) == TS_ERR_LIMIT); free (large);
  CHECK (!E ("surface", &surface, 1, t)); CHECK (!E ("close", &r, 1, t)); CHECK (!E ("close", &r, 1, t));
  CHECK (fcntl (fd,F_GETFD) < 0 && errno == EBADF); CHECK (C ("commit", &r, 1, surface) == TS_ERR_INVAL);
  CHECK (E ("text", &r, 1, t) == TS_ERR_INVAL);
  ts_value_free (&t); ts_value_free (&surface);
  CHECK (!E ("new", &t, 3, c, I (4), I (20)));
  CHECK (!E ("spawn", &r, 2, t, S ("printf FINAL; exit 7")));
  seen = false;
  for (int i = 0; i < 100 && !seen; i++)
    { CHECK (!E ("pump", &r, 2, t, I (20))); CHECK (!E ("status", &r, 1, t)); seen = !strcmp (r.as.string,"eof"); }
  CHECK (seen); CHECK (!E ("text", &r, 1, t)); CHECK (strstr (r.as.string,"FINAL"));
  for (int i = 0; i < 100; i++)
    { CHECK (!E ("wait", &r, 1, t)); if (r.type != TS_NIL) break; struct timespec delay = {0,1000000}; nanosleep (&delay,NULL); }
  CHECK (field (&r,"code") == 7);
  CHECK (!E ("close", &r, 1, t)); ts_value_free (&t);
  CHECK (!E ("new", &t, 3, c, I (4), I (20)));
  CHECK (!E ("spawn", &r, 2, t, S ("stty -echo -icanon; printf READY; exec sleep 5")));
  seen = false;
  for (int i = 0; i < 100 && !seen; i++)
    { CHECK (!E ("pump", &r, 2, t, I (20))); CHECK (!E ("text", &r, 1, t)); seen = strstr (r.as.string,"READY") != NULL; }
  CHECK (seen);
  large = malloc (65537); CHECK (large); memset (large,'x',65536); large[65536] = 0;
  CHECK (!E ("input", &r, 2, t, S (large))); CHECK (!E ("pump", &r, 2, t, I (0)));
  CHECK (!E ("pending", &r, 1, t)); int64_t unsent = r.as.integer; CHECK (unsent > 0 && unsent <= 65536);
  CHECK (E ("input", &r, 2, t, S (large)) == TS_ERR_LIMIT);
  CHECK (!E ("pending", &r, 1, t)); CHECK (r.as.integer == unsent); free (large);
  CHECK (!C ("close", &r, 1, c)); CHECK (!E ("close", &r, 1, t));
  ts_value_free (&c); ts_value_free (&t); ts_value_free (&r); ts_value_free (&frame);
  CHECK (waitpid (-1,NULL,WNOHANG) == -1 && errno == ECHILD);
}
static void limit_tests (void)
{
  TS_Value c = {0}, surfaces[64] = {{0}}, r = {0}, event = {0};
  CHECK (!C ("new", &c, 2, I (1), I (1)));
  for (size_t i = 0; i < 64; i++) CHECK (!C ("surface", &surfaces[i], 3, c, I (1), I (1)));
  CHECK (C ("surface", &r, 3, c, I (1), I (1)) == TS_ERR_LIMIT);
  ts_value_free (&surfaces[0]); CHECK (!C ("surface", &surfaces[0], 3, c, I (1), I (1)));
  CHECK (!C ("id", &r, 1, surfaces[0])); CHECK (r.as.integer == 65);
  char *large = malloc (17001); CHECK (large); memset (large,'x',17000); large[17000] = 0;
  ts_dom_t *d = ts_dom_new_obj (); CHECK (d);
  CHECK (!ts_dom_obj_set (d,"type",ts_dom_new_str ("custom")));
  CHECK (!ts_dom_obj_set (d,"data",ts_dom_new_str (large))); free (large);
  CHECK (!ts_dom_wrap (&event,d,&error)); CHECK (C ("inject", &r, 2, c, event) == TS_ERR_LIMIT);
  CHECK (!C ("event", &r, 2, c, I (0))); CHECK (r.type == TS_NIL);
  CHECK (!C ("close", &r, 1, c)); ts_value_free (&c);
  for (size_t i = 0; i < 64; i++) ts_value_free (&surfaces[i]);
  ts_value_free (&r); ts_value_free (&event);
}
static void window_tests (void)
{
  TS_Value c = {0}, t = {0}, r = {0};
  CHECK (!C ("new", &c, 3, I (320), I (160), S ("Termscript compositor test")));
  CHECK (!E ("new", &t, 3, c, I (10), I (40)));
  CHECK (!E ("spawn", &r, 2, t, S ("printf '\\033[32mComposited terminal\\033[0m\\n'; exit 0")));
  for (int i = 0; i < 10; i++) CHECK (!E ("step", &r, 2, t, I (20)));
  CHECK (!E ("text", &r, 1, t)); CHECK (strstr (r.as.string,"Composited terminal"));
  CHECK (!E ("close", &r, 1, t)); CHECK (!C ("close", &r, 1, c));
  ts_value_free (&c); ts_value_free (&t); ts_value_free (&r);
}
int main (int argc, char **argv)
{
  ts_error_clear (&error); vm = ts_vm_create (&error); CHECK (vm);
  CHECK (!ts_stdlib_register_all (vm,&error));
  if (argc == 2 && !strcmp (argv[1],"--x11")) window_tests ();
  else { scene_tests (); limit_tests (); emulator_tests (); }
  ts_vm_free (vm); puts ("compositor and emulator OK"); return 0;
}
