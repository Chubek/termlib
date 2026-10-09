#include "emulator/emulator.h"
#include "common/emulator.h"
#include "common/compositor.h"
#include "compositor/compositor.h"
#include "vterm/vterm.h"
#include "font/font.h"
#include "pty/pty.h"
#include "poll/poll.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#define INPUT_LIMIT 65536u
typedef struct {
  TS_Value compositor, surface, vt, font, child, poll;
  size_t rows, cols, cw, ch, used;
  unsigned char input[INPUT_LIMIT];
  int fd;
  bool closed, eof, dirty;
} emulator;
static const char emulator_tag;
static TS_Status comp (TS_VM *vm, const char *name, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{ return ts_module_call (vm, &ts_std_compositor_module, name, a, n, r, e); }
static emulator *get (const TS_Value *v, TS_Error *e)
{
  emulator *t = ts_std_handle (v, &emulator_tag, e, "emulator");
  if (t && t->closed) { ts_std_inval (e, "emulator: closed"); return NULL; }
  return t;
}
static void dispose (emulator *t)
{
  if (t->closed) return;
  TS_Error error = {0}; TS_Value ignored = {0};
  if (t->fd >= 0)
    { TS_Value args[] = {t->compositor, ts_arg_int (t->fd)}; comp (NULL, "unwatch", args, 2, &ignored, &error); }
  if (t->surface.type != TS_NIL) comp (NULL, "destroy", &t->surface, 1, &ignored, &error);
  ts_value_free (&ignored); ts_value_free (&t->poll); ts_value_free (&t->child);
  ts_value_free (&t->surface); ts_value_free (&t->compositor); ts_value_free (&t->vt); ts_value_free (&t->font);
  t->fd = -1; t->used = 0; t->closed = true;
}
static void destroy (void *p) { if (p) dispose (p); free (p); }
static bool geometry (int64_t rows, int64_t cols, int64_t cw, int64_t ch)
{
  return rows > 0 && cols > 0 && cw >= 4 && ch >= 4 && cw <= 256 && ch <= 256 &&
    rows <= 32767/ch && cols <= 32767/cw && rows*ch*cols*cw <= 8388608;
}

static TS_Status
e_new (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  emulator *t; int64_t rows, cols, cw = 8, ch = 16; const char *pattern = "monospace";
  TS_Value tmp = {0}; TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 3, 6, e, "emulator.new") || n == 5 || ts_std_int (a+1, &rows, e, "rows") ||
      ts_std_int (a+2, &cols, e, "cols") || (n >= 4 && ts_std_str (a+3, &pattern, e, "font")) ||
      (n == 6 && (ts_std_int (a+4, &cw, e, "cell width") || ts_std_int (a+5, &ch, e, "cell height")))) return ts_std_inval (e, "emulator.new: expected compositor rows cols [font [cell_width cell_height]]");
  if (!geometry (rows, cols, cw, ch)) return ts_std_inval (e, "emulator: invalid geometry");
  t = calloc (1, sizeof *t); if (!t) return ts_std_oom (e);
  t->fd = -1; t->rows = rows; t->cols = cols; t->cw = cw; t->ch = ch; t->dirty = true;
  st = ts_value_copy (&t->compositor, a);
  if (!st) { TS_Value args[] = {a[0], ts_arg_int (cols*cw), ts_arg_int (rows*ch)}; st = comp (vm, "surface", args, 3, &t->surface, e); }
  if (!st) { TS_Value args[] = {a[1], a[2]}; st = ts_module_call (vm, &ts_std_vterm_module, "new", args, 2, &t->vt, e); }
  if (!st) { TS_Value arg = ts_arg_str (pattern); st = ts_module_call (vm, &ts_std_font_module, "match", &arg, 1, &t->font, e); }
  if (!st && t->font.type == TS_NIL) { ts_error_set (e, TS_ERR_NOTFOUND, 0, 0, "emulator: no matching font"); st = TS_ERR_NOTFOUND; }
  if (!st) st = ts_module_call (vm, &ts_std_poll_module, "new", NULL, 0, &t->poll, e);
  if (!st) { TS_Value args[] = {t->compositor, t->surface}; st = comp (vm, "focus", args, 2, &tmp, e); }
  ts_value_free (&tmp);
  if (st) { destroy (t); return st; }
  if (ts_value_make_handle (r, t, destroy, "<emulator>", &emulator_tag)) { destroy (t); return ts_std_oom (e); }
  return TS_OK;
}

static TS_Status watch_child (TS_VM *vm, emulator *t, TS_Error *e)
{
  TS_Value tmp = {0}; TS_Status st;
  TS_Value args[] = {t->poll, ts_arg_int (t->fd), ts_arg_int (t->used ? 3 : 1)};
  st = ts_module_call (vm, &ts_std_poll_module, "watch", args, 3, &tmp, e);
  args[0] = t->compositor;
  if (!st) st = comp (vm, "watch", args, 3, &tmp, e);
  ts_value_free (&tmp); return st;
}
static TS_Status
e_spawn (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  emulator *t; TS_Value tmp = {0}; TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 2, 2, e, "emulator.spawn")) return TS_ERR_INVAL;
  t = get (a, e); if (!t) return TS_ERR_INVAL;
  if (t->child.type != TS_NIL) return ts_std_inval (e, "emulator: already spawned");
  TS_Value args[] = {a[1], ts_arg_int (t->rows), ts_arg_int (t->cols)};
  st = ts_module_call (vm, &ts_std_pty_module, "spawn", args, 3, &t->child, e);
  if (!st) { TS_Value nb[] = {t->child, {0}}; nb[1].type = TS_BOOL; nb[1].as.boolean = true;
    st = ts_module_call (vm, &ts_std_pty_module, "nonblocking", nb, 2, &tmp, e); }
  if (!st) st = ts_module_call (vm, &ts_std_pty_module, "fd", &t->child, 1, &tmp, e);
  if (!st) { t->fd = (int) tmp.as.integer; st = watch_child (vm, t, e); }
  if (st)
    {
      TS_Error ignored = {0};
      if (t->fd >= 0) { TS_Value unwatch[] = {t->poll, ts_arg_int (t->fd)};
        ts_module_call (vm, &ts_std_poll_module, "remove", unwatch, 2, &tmp, &ignored);
        unwatch[0] = t->compositor; comp (vm, "unwatch", unwatch, 2, &tmp, &ignored); }
      ts_value_free (&t->child); t->fd = -1;
    }
  ts_value_free (&tmp); ts_std_ret_nil (r); return st;
}

static TS_Status
queue_bytes (TS_VM *vm, emulator *t, const unsigned char *bytes, size_t len, TS_Error *e)
{
  if (t->fd < 0 || t->eof) return ts_std_inval (e, "emulator: no writable child");
  if (len > INPUT_LIMIT - t->used) { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "emulator input queue full"); return TS_ERR_LIMIT; }
  size_t old = t->used;
  memcpy (t->input + old, bytes, len); t->used += len;
  TS_Status st = watch_child (vm, t, e); if (st) t->used = old;
  return st;
}
static TS_Status
e_feed (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  emulator *t; const char *text; TS_Status st;
  if (ts_std_argc (vm, n, 2, 2, e, "emulator input") || ts_std_str (a+1, &text, e, "text")) return TS_ERR_INVAL;
  t = get (a, e); if (!t) return TS_ERR_INVAL;
  size_t len = strlen (text);
  if (ud) st = queue_bytes (vm, t, (const unsigned char *) text, len, e);
  else { st = ts_vterm_bytes (&t->vt, text, len, e); t->dirty = true; }
  ts_std_ret_int (r, (int64_t) len); return st;
}

static TS_Status
e_pump (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  emulator *t; int64_t timeout = 0; TS_Value tmp = {0}; TS_Status st; size_t total = 0;
  (void) ud;
  if (ts_std_argc (vm, n, 1, 2, e, "emulator.pump") || (n == 2 && ts_std_int (a+1, &timeout, e, "timeout"))) return TS_ERR_INVAL;
  if (timeout < -1 || timeout > INT_MAX) return ts_std_inval (e, "pump: invalid timeout");
  t = get (a, e); if (!t) return TS_ERR_INVAL;
  if (t->fd < 0 || t->eof) { ts_std_ret_int (r, 0); return TS_OK; }
  TS_Value wait[] = {t->poll, ts_arg_int (timeout)};
  st = ts_module_call (vm, &ts_std_poll_module, "wait", wait, 2, &tmp, e); ts_value_free (&tmp); if (st) return st;
  /* Drain output even when a write is backpressured. Work per call is bounded. */
  while (total < 262144)
    {
      char buf[4096]; ssize_t got = read (t->fd, buf, sizeof buf);
      if (got < 0 && errno == EINTR) continue;
      if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (!got || (got < 0 && errno == EIO)) { t->eof = true; t->used = 0; break; }
      if (got < 0) return ts_std_system (e, "emulator read");
      total += (size_t) got; t->dirty = true;
      st = ts_vterm_bytes (&t->vt, buf, (size_t) got, e); if (st) return st;
    }
  while (t->used && !t->eof)
    {
      ssize_t sent = write (t->fd, t->input, t->used);
      if (sent < 0 && errno == EINTR) continue;
      if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (sent <= 0) { if (!sent) errno = EIO; return ts_std_system (e, "emulator write"); }
      t->used -= (size_t) sent; memmove (t->input, t->input + sent, t->used);
    }
  if (t->eof)
    { TS_Value args[] = {t->poll, ts_arg_int (t->fd)};
      st = ts_module_call (vm, &ts_std_poll_module, "remove", args, 2, &tmp, e);
      args[0] = t->compositor; if (!st) st = comp (vm, "unwatch", args, 2, &tmp, e); }
  else st = watch_child (vm, t, e);
  ts_value_free (&tmp); ts_std_ret_int (r, (int64_t) total); return st;
}

static TS_Status
e_render (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  emulator *t; size_t row, col; bool cursor; TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 1, 1, e, "emulator.render")) return TS_ERR_INVAL;
  t = get (a, e); if (!t) return TS_ERR_INVAL;
  if (!t->dirty) { ts_std_ret_nil (r); return TS_OK; }
  const ts_grid_t *grid = ts_vterm_grid (&t->vt, &row, &col, &cursor, e); if (!grid) return TS_ERR_INVAL;
  size_t width = t->cols*t->cw, height = t->rows*t->ch;
  uint32_t *pixels = malloc (width*height*sizeof *pixels); if (!pixels) return ts_std_oom (e);
  st = ts_font_grid (&t->font, grid, (unsigned) t->cw, (unsigned) t->ch, pixels, e);
  if (!st && cursor && row < t->rows && col < t->cols)
    for (size_t x = 0; x < t->cw; x++) pixels[((row+1)*t->ch-1)*width+col*t->cw+x] ^= 0xffffff;
  if (!st) st = ts_surface_pixels (&t->surface, width, height, pixels, e);
  free (pixels);
  if (!st) st = comp (vm, "commit", &t->surface, 1, r, e);
  if (!st) t->dirty = false;
  return st;
}

static TS_Status
e_state (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  emulator *t; intptr_t op = (intptr_t) ud;
  if (ts_std_argc (vm, n, 1, 1, e, "emulator")) return TS_ERR_INVAL;
  if (!op) { t = ts_std_handle (a, &emulator_tag, e, "emulator.close"); if (!t) return TS_ERR_INVAL; dispose (t); ts_std_ret_nil (r); return TS_OK; }
  t = get (a, e); if (!t) return TS_ERR_INVAL;
  switch (op)
    {
    case 1: return ts_value_copy (r, &t->surface);
    case 2: return ts_module_call (vm, &ts_std_vterm_module, "text", &t->vt, 1, r, e);
    case 3: ts_std_ret_int (r, t->fd); return TS_OK;
    case 4: return ts_std_ret_str (r, t->fd < 0 ? "idle" : t->eof ? "eof" : "running", e);
    case 5: if (t->child.type == TS_NIL) { ts_std_ret_nil (r); return TS_OK; }
      return ts_module_call (vm, &ts_std_pty_module, "wait_event", &t->child, 1, r, e);
    case 6: ts_std_ret_int (r, (int64_t) t->used); return TS_OK;
    }
  return TS_ERR_INVAL;
}

static TS_Status
e_resize (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  emulator *t; int64_t rows, cols; TS_Value next = {0}, tmp = {0}; TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 3, 3, e, "emulator.resize") || ts_std_int (a+1, &rows, e, "rows") || ts_std_int (a+2, &cols, e, "cols")) return TS_ERR_INVAL;
  t = get (a, e); if (!t) return TS_ERR_INVAL;
  if (!geometry (rows, cols, t->cw, t->ch)) return ts_std_inval (e, "emulator: invalid geometry");
  if ((size_t) rows == t->rows && (size_t) cols == t->cols) { ts_std_ret_nil (r); return TS_OK; }
  st = ts_vterm_resized (&t->vt, (size_t) rows, (size_t) cols, &next, e);
  if (!st) st = ts_surface_pixels (&t->surface, (size_t) cols*t->cw, (size_t) rows*t->ch, NULL, e);
  if (!st && t->child.type != TS_NIL && !t->eof)
    { TS_Value args[] = {t->child, a[1], a[2]}; st = ts_module_call (vm, &ts_std_pty_module, "resize", args, 3, &tmp, e); }
  if (!st) { ts_value_free (&t->vt); t->vt = next; next.type = TS_NIL; t->rows = rows; t->cols = cols; t->dirty = true; }
  ts_value_free (&next); ts_value_free (&tmp); ts_std_ret_nil (r); return st;
}

static TS_Status
e_handle (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  emulator *t; ts_dom_t *d; TS_Value type = {0}, id = {0}, text = {0}; TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 2, 2, e, "emulator.handle")) return TS_ERR_INVAL;
  t = get (a, e); if (!t) return TS_ERR_INVAL;
  ts_std_ret_bool (r, false);
  if (a[1].type == TS_NIL) return TS_OK;
  d = ts_dom_unwrap (a+1, e, "event"); if (!d) return TS_ERR_INVAL;
  const ts_dom_t *field = ts_dom_path (d, "type"); if (!field) return ts_std_inval (e, "event: type required");
  st = ts_dom_to_value (field, &type, e); if (st) return st;
  if (type.type != TS_STRING || strcmp (type.as.string, "key_press")) { ts_value_free (&type); return TS_OK; }
  ts_value_free (&type);
  st = comp (vm, "id", &t->surface, 1, &id, e); if (st) return st;
  bool focused = id.as.integer == ts_doc_int (d, "surface"); ts_value_free (&id);
  if (!focused) return TS_OK;
  const char *seq = NULL;
  int64_t sym = ts_doc_int (d, "keysym"), state = ts_doc_int (d, "state");
  switch (sym)
    {
    case 0xff51: seq = "\033[D"; break; case 0xff52: seq = "\033[A"; break;
    case 0xff53: seq = "\033[C"; break; case 0xff54: seq = "\033[B"; break;
    case 0xff50: seq = "\033[H"; break; case 0xff57: seq = "\033[F"; break;
    case 0xff55: seq = "\033[5~"; break; case 0xff56: seq = "\033[6~"; break;
    case 0xff63: seq = "\033[2~"; break; case 0xffff: seq = "\033[3~"; break;
    case 0xff08: seq = "\177"; break; case 0xff09: seq = "\t"; break;
    case 0xff0d: seq = "\r"; break; case 0xff1b: seq = "\033"; break;
    }
  unsigned char bytes[256]; size_t len = 0;
  if (seq) { size_t count = strlen (seq); memcpy (bytes+len, seq, count); len += count; }
  else if ((state & 4) && (sym == ' ' || sym == '@')) bytes[len++] = 0;
  else
    {
      field = ts_dom_path (d, "text");
      if (field) { st = ts_dom_to_value (field, &text, e); if (st) return st; }
      if (text.type != TS_STRING) { ts_value_free (&text); return TS_OK; }
      size_t count = strlen (text.as.string);
      if (count > 63) { ts_value_free (&text); return ts_std_inval (e, "key text too long"); }
      /* XLookupString is Latin-1; direct input() accepts UTF-8 instead. */
      for (size_t i = 0; i < count; i++)
        { unsigned char b = (unsigned char) text.as.string[i];
          if (b >= 128) { bytes[len++] = 0xc0 | (b >> 6); bytes[len++] = 0x80 | (b & 63); }
          else bytes[len++] = b; }
      ts_value_free (&text);
    }
  if (!len) return TS_OK;
  if (state & 8) { memmove (bytes+1, bytes, len); bytes[0] = 27; len++; } /* Mod1 / Alt */
  st = queue_bytes (vm, t, bytes, len, e); if (!st) ts_std_ret_bool (r, true); return st;
}

static TS_Status
e_step (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  emulator *t; TS_Value tmp = {0}; TS_Status st;
  int64_t timeout;
  (void) ud;
  if (ts_std_argc (vm, n, 2, 2, e, "emulator.step") || ts_std_int (a+1, &timeout, e, "timeout")) return TS_ERR_INVAL;
  if (timeout < -1 || timeout > INT_MAX) return ts_std_inval (e, "step: invalid timeout");
  t = get (a, e); if (!t) return TS_ERR_INVAL;
  /* Paint before sleeping so an initial frame is visible immediately. */
  st = e_render (vm, NULL, a, 1, &tmp, e);
  if (!st) st = comp (vm, "present", &t->compositor, 1, &tmp, e);
  if (!st) { TS_Value args[] = {t->compositor, ts_arg_int (t->eof ? 0 : timeout)}; st = comp (vm, "event", args, 2, r, e); }
  if (!st && r->type != TS_NIL)
    {
      ts_dom_t *d = ts_dom_unwrap (r, e, "event"); TS_Value type = {0};
      const ts_dom_t *field = ts_dom_path (d, "type");
      if (field) st = ts_dom_to_value (field, &type, e);
      if (!st && type.type == TS_STRING && !strcmp (type.as.string, "resize"))
        {
          int64_t width = ts_doc_int (d, "width"), height = ts_doc_int (d, "height");
          TS_Value args[] = {t->compositor, ts_arg_int (width), ts_arg_int (height)};
          st = comp (vm, "output_size", args, 3, &tmp, e);
          if (!st) { args[0] = a[0]; args[1] = ts_arg_int (height/(int64_t) t->ch > 0 ? height/(int64_t) t->ch : 1);
            args[2] = ts_arg_int (width/(int64_t) t->cw > 0 ? width/(int64_t) t->cw : 1); st = e_resize (vm, NULL, args, 3, &tmp, e); }
        }
      else if (!st) { TS_Value args[] = {a[0], *r}; st = e_handle (vm, NULL, args, 2, &tmp, e); }
      bool closing = type.type == TS_STRING && (!strcmp (type.as.string, "close") || !strcmp (type.as.string, "destroy"));
      ts_value_free (&type);
      if (closing) { ts_value_free (&tmp); return st; }
    }
  if (!st) st = e_pump (vm, NULL, a, 1, &tmp, e);
  if (!st) st = e_render (vm, NULL, a, 1, &tmp, e);
  if (!st) st = comp (vm, "present", &t->compositor, 1, &tmp, e);
  ts_value_free (&tmp); return st;
}

static const TS_FuncDef funcs[] = {
  {"new", e_new, NULL}, {"spawn", e_spawn, NULL},
  {"feed", e_feed, NULL}, {"input", e_feed, (void *) 1},
  {"pump", e_pump, NULL}, {"render", e_render, NULL},
  {"resize", e_resize, NULL}, {"handle", e_handle, NULL}, {"step", e_step, NULL},
  {"close", e_state, NULL}, {"surface", e_state, (void *) 1},
  {"text", e_state, (void *) 2}, {"fd", e_state, (void *) 3},
  {"status", e_state, (void *) 4}, {"wait", e_state, (void *) 5},
  {"pending", e_state, (void *) 6}, {NULL, NULL, NULL}
};
const TS_Module ts_std_emulator_module = {"std.emulator", funcs};
