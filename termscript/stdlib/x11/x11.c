#include "x11/x11.h"
#include "common/ts_std_common.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef TS_STD_HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <fcntl.h>

typedef struct { Display *display; Window window; GC gc; Atom protocols, delete; Visual *visual; int depth; } x_window;
static const char x_tag;

static void
x_dispose (x_window *w)
{
  if (!w->display) return;
  if (w->gc) XFreeGC (w->display, w->gc);
  if (w->window) XDestroyWindow (w->display, w->window);
  XCloseDisplay (w->display);
  w->display = NULL; w->window = 0; w->gc = NULL;
}
static void x_free (void *p) { if (p) x_dispose (p); free (p); }
static x_window *
x_get (const TS_Value *v, TS_Error *e)
{
  x_window *w = ts_std_handle (v, &x_tag, e, "x11");
  if (w && (!w->display || !w->window)) { ts_std_inval (e, "x11: closed window"); return NULL; }
  return w;
}

static TS_Status
x_open (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  const char *title, *name = NULL;
  int64_t width, height;
  x_window *w;
  int screen, fd, flags;
  (void) ud;
  if (ts_std_argc (vm, n, 3, 4, e, "x11.open") ||
      ts_std_str (a, &title, e, "title") || ts_std_int (a + 1, &width, e, "width") ||
      ts_std_int (a + 2, &height, e, "height") ||
      (n == 4 && ts_std_str (a + 3, &name, e, "display"))) return TS_ERR_INVAL;
  if (width < 1 || height < 1 || width > 32767 || height > 32767 || strlen (title) > 4096)
    return ts_std_inval (e, "x11.open: invalid dimensions or title");
  w = calloc (1, sizeof *w);
  if (!w) return ts_std_oom (e);
  w->display = XOpenDisplay (name && *name ? name : NULL);
  if (!w->display)
    { free (w); ts_error_set (e, TS_ERR_SYSTEM, 0, 0, "x11: cannot open display"); return TS_ERR_SYSTEM; }
  fd = ConnectionNumber (w->display); flags = fcntl (fd, F_GETFD);
  if (flags < 0 || fcntl (fd, F_SETFD, flags | FD_CLOEXEC) < 0)
    { TS_Status st = ts_std_system (e, "x11 descriptor"); x_free (w); return st; }
  screen = DefaultScreen (w->display);
  w->visual = DefaultVisual (w->display, screen); w->depth = DefaultDepth (w->display, screen);
  if (w->visual->class != TrueColor)
    { x_free (w); ts_error_set (e, TS_ERR_UNSUPPORTED, 0, 0, "x11: TrueColor visual required"); return TS_ERR_UNSUPPORTED; }
  w->window = XCreateSimpleWindow (w->display, RootWindow (w->display, screen), 0, 0,
                                   (unsigned) width, (unsigned) height, 0, 0, 0);
  w->gc = XCreateGC (w->display, w->window, 0, NULL);
  if (!w->window || !w->gc) { x_free (w); return ts_std_oom (e); }
  w->protocols = XInternAtom (w->display, "WM_PROTOCOLS", False);
  w->delete = XInternAtom (w->display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols (w->display, w->window, &w->delete, 1);
  XStoreName (w->display, w->window, title);
  XSelectInput (w->display, w->window, ExposureMask | StructureNotifyMask | KeyPressMask |
                KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | FocusChangeMask);
  XMapWindow (w->display, w->window); XFlush (w->display);
  if (ts_value_make_handle (r, w, x_free, "<x11-window>", &x_tag))
    { x_free (w); return ts_std_oom (e); }
  return TS_OK;
}

static TS_Status
x_state (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  intptr_t op = (intptr_t) ud;
  x_window *w;
  if (ts_std_argc (vm, n, 1, 1, e, "x11")) return TS_ERR_INVAL;
  if (op == 0)
    {
      w = ts_std_handle (a, &x_tag, e, "x11.close");
      if (!w) return TS_ERR_INVAL;
      x_dispose (w); ts_std_ret_nil (r); return TS_OK;
    }
  w = x_get (a, e); if (!w) return TS_ERR_INVAL;
  if (op == 1) ts_std_ret_int (r, ConnectionNumber (w->display));
  if (op == 2) { XFlush (w->display); ts_std_ret_nil (r); }
  if (op == 3) ts_std_ret_int (r, XPending (w->display));
  return TS_OK;
}

static TS_Status
x_event (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  x_window *w;
  XEvent ev;
  ts_dom_t *obj;
  const char *type = "other";
  (void) ud;
  if (ts_std_argc (vm, n, 1, 1, e, "x11.event")) return TS_ERR_INVAL;
  w = x_get (a, e); if (!w) return TS_ERR_INVAL;
  if (!XPending (w->display)) { ts_std_ret_nil (r); return TS_OK; }
  XNextEvent (w->display, &ev);
  obj = ts_dom_new_obj (); if (!obj) return ts_std_oom (e);
#define FIELD(k, v) do { if (ts_dom_obj_set (obj, (k), (v))) goto oom; } while (0)
  FIELD ("code", ts_dom_new_int (ev.type));
  switch (ev.type)
    {
    case Expose:
      type = "expose";
      FIELD ("x", ts_dom_new_int (ev.xexpose.x)); FIELD ("y", ts_dom_new_int (ev.xexpose.y));
      FIELD ("width", ts_dom_new_int (ev.xexpose.width)); FIELD ("height", ts_dom_new_int (ev.xexpose.height));
      FIELD ("count", ts_dom_new_int (ev.xexpose.count)); break;
    case ConfigureNotify:
      type = "resize";
      FIELD ("width", ts_dom_new_int (ev.xconfigure.width)); FIELD ("height", ts_dom_new_int (ev.xconfigure.height)); break;
    case KeyPress: case KeyRelease:
      {
        char text[64]; KeySym sym = NoSymbol;
        int count = XLookupString (&ev.xkey, text, sizeof text - 1, &sym, NULL);
        text[count] = 0; type = ev.type == KeyPress ? "key_press" : "key_release";
        FIELD ("keycode", ts_dom_new_int (ev.xkey.keycode)); FIELD ("keysym", ts_dom_new_int (sym));
        FIELD ("state", ts_dom_new_int (ev.xkey.state)); FIELD ("text", ts_dom_new_str (text)); break;
      }
    case ButtonPress: case ButtonRelease:
      type = ev.type == ButtonPress ? "button_press" : "button_release";
      FIELD ("button", ts_dom_new_int (ev.xbutton.button)); FIELD ("state", ts_dom_new_int (ev.xbutton.state));
      FIELD ("x", ts_dom_new_int (ev.xbutton.x)); FIELD ("y", ts_dom_new_int (ev.xbutton.y)); break;
    case MotionNotify:
      type = "motion"; FIELD ("state", ts_dom_new_int (ev.xmotion.state));
      FIELD ("x", ts_dom_new_int (ev.xmotion.x)); FIELD ("y", ts_dom_new_int (ev.xmotion.y)); break;
    case FocusIn: type = "focus_in"; break;
    case FocusOut: type = "focus_out"; break;
    case ClientMessage:
      if (ev.xclient.message_type == w->protocols && ev.xclient.format == 32 &&
          (Atom) ev.xclient.data.l[0] == w->delete) type = "close";
      break;
    case DestroyNotify: type = "destroy"; w->window = 0; break;
    }
  FIELD ("type", ts_dom_new_str (type));
#undef FIELD
  return ts_dom_wrap (r, obj, e);
oom:
  ts_dom_free (obj); return ts_std_oom (e);
}

static unsigned long
channel (unsigned value, unsigned long mask)
{
  unsigned shift = 0;
  if (!mask) return 0;
  while (!(mask & 1)) { mask >>= 1; shift++; }
  return (((unsigned long) value * mask + 127) / 255) << shift;
}
static unsigned long
pixel (x_window *w, uint32_t rgb)
{
  return channel ((rgb >> 16) & 255, w->visual->red_mask) |
         channel ((rgb >> 8) & 255, w->visual->green_mask) | channel (rgb & 255, w->visual->blue_mask);
}

static TS_Status
x_draw (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  intptr_t op = (intptr_t) ud;
  x_window *w;
  int64_t x, y, width, height, rgb;
  ts_std_list_t *pixels;
  XImage *image;
  if (ts_std_argc (vm, n, 6, 6, e, "x11.draw") ||
      ts_std_int (a + 1, &x, e, "x") || ts_std_int (a + 2, &y, e, "y") ||
      ts_std_int (a + 3, &width, e, "width") || ts_std_int (a + 4, &height, e, "height")) return TS_ERR_INVAL;
  if (x < -32768 || x > 32767 || y < -32768 || y > 32767 ||
      width < 1 || height < 1 || width > 32767 || height > 32767)
    return ts_std_inval (e, "x11.draw: invalid rectangle");
  w = x_get (a, e); if (!w) return TS_ERR_INVAL;
  if (!op)
    {
      if (ts_std_int (a + 5, &rgb, e, "RGB") || rgb < 0 || rgb > 0xffffff)
        return ts_std_inval (e, "x11.fill: RGB must be in 0..16777215");
      XSetForeground (w->display, w->gc, pixel (w, (uint32_t) rgb));
      XFillRectangle (w->display, w->window, w->gc, (int) x, (int) y, (unsigned) width, (unsigned) height);
    }
  else
    {
      if (width * height > 1048576)
        { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "x11.image: pixel limit exceeded"); return TS_ERR_LIMIT; }
      pixels = ts_std_list_unwrap (a + 5, e, "x11.image"); if (!pixels) return TS_ERR_INVAL;
      if (ts_std_list_len (pixels) != (size_t) (width * height)) return ts_std_inval (e, "x11.image: pixel count mismatch");
      for (size_t i = 0; i < ts_std_list_len (pixels); i++)
        {
          const TS_Value *v = ts_std_list_get (pixels, i);
          if (v->type != TS_INT || v->as.integer < 0 || v->as.integer > 0xffffff)
            return ts_std_inval (e, "x11.image: invalid RGB pixel");
        }
      image = XCreateImage (w->display, w->visual, (unsigned) w->depth, ZPixmap, 0, NULL,
                            (unsigned) width, (unsigned) height, 32, 0);
      if (!image) return ts_std_oom (e);
      image->data = calloc ((size_t) height, (size_t) image->bytes_per_line);
      if (!image->data) { XDestroyImage (image); return ts_std_oom (e); }
      for (int j = 0; j < height; j++) for (int i = 0; i < width; i++)
        XPutPixel (image, i, j, pixel (w, (uint32_t) ts_std_list_get (pixels, (size_t) (j * width + i))->as.integer));
      XPutImage (w->display, w->window, w->gc, image, 0, 0, (int) x, (int) y, (unsigned) width, (unsigned) height);
      XDestroyImage (image);
    }
  ts_std_ret_nil (r); return TS_OK;
}
#else
static TS_Status
x_unsupported (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  (void) vm; (void) ud; (void) a; (void) n; (void) r;
  ts_error_set (e, TS_ERR_UNSUPPORTED, 0, 0, "std.x11 was built without Xlib"); return TS_ERR_UNSUPPORTED;
}
#define x_open x_unsupported
#define x_state x_unsupported
#define x_event x_unsupported
#define x_draw x_unsupported
#endif

static TS_Status
x_available (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  (void) ud; (void) a;
  if (ts_std_argc (vm, n, 0, 0, e, "x11.available")) return TS_ERR_INVAL;
#ifdef TS_STD_HAVE_X11
  ts_std_ret_bool (r, true);
#else
  ts_std_ret_bool (r, false);
#endif
  return TS_OK;
}
static const TS_FuncDef funcs[] = {
  {"available", x_available, NULL}, {"open", x_open, NULL},
  {"close", x_state, (void *) 0}, {"fd", x_state, (void *) 1},
  {"flush", x_state, (void *) 2}, {"pending", x_state, (void *) 3},
  {"event", x_event, NULL}, {"fill", x_draw, (void *) 0}, {"image", x_draw, (void *) 1},
};
const TS_Module ts_std_x11_module = TS_MODULE_INIT ("std.x11", funcs);
