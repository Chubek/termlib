/* In-process surface compositor with a nested X11 output. No global state. */
#include "compositor/compositor.h"
#include "common/compositor.h"
#include "x11/x11.h"
#include "poll/poll.h"
#include "drm/drm.h"
#include <stdlib.h>
#include <limits.h>

#define PIXELS 8388608u
#define TOTAL_PIXELS (4u * PIXELS)
#define SURFACES 64
#define EVENTS 64
typedef struct compositor compositor;
typedef struct surface surface;
struct surface {
  compositor *owner;
  uint64_t id;
  bool closed, mapped, pending;
  int x, y, px, py;
  size_t w, h, pw, ph;
  uint32_t *front, *back;
};
struct compositor {
  size_t refs, w, h, count, allocated, head, queued;
  uint64_t next_id, serial, focus;
  bool closed, dirty;
  uint32_t *frame;
  surface *surfaces[SURFACES]; /* weak; each surface retains its owner */
  TS_Value output, poll, events[EVENTS];
};
static const char comp_tag, surface_tag;

static TS_Status limit (TS_Error *e, const char *s)
{ ts_error_set (e, TS_ERR_LIMIT, 0, 0, "%s", s); return TS_ERR_LIMIT; }
static bool dimensions (int64_t w, int64_t h)
{ return w > 0 && h > 0 && w <= 32767 && h <= 32767 && w * h <= PIXELS; }
static compositor *comp_get (const TS_Value *v, TS_Error *e)
{
  compositor *c = ts_std_handle (v, &comp_tag, e, "compositor");
  if (c && c->closed) { ts_std_inval (e, "compositor: closed"); return NULL; }
  return c;
}
static surface *surf_get (const TS_Value *v, TS_Error *e)
{
  surface *s = ts_std_handle (v, &surface_tag, e, "surface");
  if (s && (s->closed || s->owner->closed)) { ts_std_inval (e, "surface: closed"); return NULL; }
  return s;
}
static void surf_dispose (surface *s)
{
  compositor *c = s->owner;
  if (s->closed) return;
  for (size_t i = 0; i < c->count; i++) if (c->surfaces[i] == s)
    { memmove (c->surfaces + i, c->surfaces + i + 1, (--c->count - i) * sizeof s); break; }
  if (s->front) c->allocated -= s->w * s->h;
  if (s->back) c->allocated -= s->pw * s->ph;
  free (s->front); free (s->back); s->front = s->back = NULL;
  if (c->focus == s->id) c->focus = 0;
  s->closed = true; c->dirty = true;
}
static void comp_dispose (compositor *c)
{
  if (c->closed) return;
  while (c->count) surf_dispose (c->surfaces[c->count - 1]);
  ts_value_free (&c->output); ts_value_free (&c->poll);
  for (size_t i = 0; i < EVENTS; i++) ts_value_free (&c->events[i]);
  c->queued = 0; free (c->frame); c->frame = NULL; c->closed = true;
}
static void comp_release (void *p)
{ compositor *c = p; if (c && !--c->refs) { comp_dispose (c); free (c); } }
static void surf_free (void *p)
{ surface *s = p; if (s) { surf_dispose (s); comp_release (s->owner); free (s); } }

TS_Status
ts_surface_pixels (const TS_Value *v, size_t w, size_t h, const uint32_t *pixels, TS_Error *e)
{
  surface *s = surf_get (v, e);
  uint32_t *copy;
  size_t old, count;
  if (!s) return TS_ERR_INVAL;
  if (w > 32767 || h > 32767 || !dimensions ((int64_t) w, (int64_t) h)) return ts_std_inval (e, "surface: invalid dimensions");
  count = w * h; old = s->back ? s->pw * s->ph : 0;
  if (s->owner->allocated - old + count > TOTAL_PIXELS) return limit (e, "compositor: buffer budget exceeded");
  copy = calloc (count, sizeof *copy); if (!copy) return ts_std_oom (e);
  if (pixels) memcpy (copy, pixels, count * sizeof *copy);
  free (s->back); s->back = copy; s->pw = w; s->ph = h; s->pending = true;
  s->owner->allocated = s->owner->allocated - old + count;
  return TS_OK;
}

static TS_Status
c_new (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  int64_t w, h; const char *title;
  compositor *c; TS_Status st; TS_Value tmp = {0};
  (void) ud;
  if (ts_std_argc (vm, n, 2, 3, e, "compositor.new") || ts_std_int (a, &w, e, "width") || ts_std_int (a+1, &h, e, "height") ||
      (n == 3 && ts_std_str (a+2, &title, e, "title"))) return TS_ERR_INVAL;
  if (!dimensions (w, h)) return ts_std_inval (e, "compositor: invalid output dimensions");
  c = calloc (1, sizeof *c); if (!c) return ts_std_oom (e);
  c->refs = 1; c->w = (size_t) w; c->h = (size_t) h; c->dirty = true;
  c->frame = calloc (c->w * c->h, sizeof *c->frame);
  if (!c->frame) { comp_release (c); return ts_std_oom (e); }
  st = ts_module_call (vm, &ts_std_poll_module, "new", NULL, 0, &c->poll, e);
  if (!st && n == 3)
    {
      TS_Value args[] = {a[2], a[0], a[1]};
      st = ts_module_call (vm, &ts_std_x11_module, "open", args, 3, &c->output, e);
      if (!st) st = ts_module_call (vm, &ts_std_x11_module, "fd", &c->output, 1, &tmp, e);
      if (!st) { TS_Value watch[] = {c->poll, tmp, ts_arg_int (1)}; TS_Value ignored = {0};
        st = ts_module_call (vm, &ts_std_poll_module, "watch", watch, 3, &ignored, e); ts_value_free (&ignored); }
    }
  ts_value_free (&tmp);
  if (st) { comp_release (c); return st; }
  if (ts_value_make_handle (r, c, comp_release, "<compositor>", &comp_tag)) { comp_release (c); return ts_std_oom (e); }
  return TS_OK;
}
static TS_Status
c_surface (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  compositor *c; surface *s; int64_t w, h;
  (void) ud;
  if (ts_std_argc (vm, n, 3, 3, e, "compositor.surface") || ts_std_int (a+1, &w, e, "width") || ts_std_int (a+2, &h, e, "height")) return TS_ERR_INVAL;
  c = comp_get (a, e); if (!c) return TS_ERR_INVAL;
  if (!dimensions (w, h)) return ts_std_inval (e, "surface: invalid dimensions");
  if (c->count == SURFACES || c->next_id == INT64_MAX) return limit (e, "compositor: surface limit");
  s = calloc (1, sizeof *s); if (!s) return ts_std_oom (e);
  s->owner = c; s->pw = (size_t) w; s->ph = (size_t) h;
  if (ts_value_make_handle (r, s, surf_free, "<surface>", &surface_tag)) { free (s); return ts_std_oom (e); }
  s->id = ++c->next_id; c->refs++; c->surfaces[c->count++] = s;
  return TS_OK;
}

static TS_Status
s_buffer (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  surface *s; TS_Status st; uint32_t *p; int64_t color;
  intptr_t op = (intptr_t) ud;
  if (ts_std_argc (vm, n, 2, 2, e, "surface buffer")) return TS_ERR_INVAL;
  s = surf_get (a, e); if (!s) return TS_ERR_INVAL;
  if (!op)
    {
      ts_std_list_t *l = ts_std_list_unwrap (a+1, e, "attach"); if (!l) return TS_ERR_INVAL;
      if (ts_std_list_len (l) != s->pw * s->ph) return ts_std_inval (e, "attach: pixel count mismatch");
      p = malloc (s->pw * s->ph * sizeof *p); if (!p) return ts_std_oom (e);
      for (size_t i = 0; i < s->pw * s->ph; i++)
        { const TS_Value *v = ts_std_list_get (l, i);
          if (v->type != TS_INT || v->as.integer < 0 || v->as.integer > UINT32_MAX)
            { free (p); return ts_std_inval (e, "attach: invalid ARGB pixel"); }
          p[i] = (uint32_t) v->as.integer; }
      st = ts_surface_pixels (a, s->pw, s->ph, p, e); free (p);
    }
  else
    {
      if (ts_std_int (a+1, &color, e, "fill") || color < 0 || color > UINT32_MAX) return ts_std_inval (e, "fill: invalid ARGB");
      st = ts_surface_pixels (a, s->pw, s->ph, NULL, e);
      if (!st) for (size_t i = 0; i < s->pw * s->ph; i++) s->back[i] = (uint32_t) color;
    }
  ts_std_ret_nil (r); return st;
}

static TS_Status
s_state (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  intptr_t op = (intptr_t) ud;
  surface *s; int64_t x, y;
  size_t want = op == 2 || op == 3 ? 3 : 1;
  if (ts_std_argc (vm, n, want, want, e, "surface")) return TS_ERR_INVAL;
  if (!op) { s = ts_std_handle (a, &surface_tag, e, "surface.close"); if (!s) return TS_ERR_INVAL; surf_dispose (s); ts_std_ret_nil (r); return TS_OK; }
  s = surf_get (a, e); if (!s) return TS_ERR_INVAL;
  if (op == 1) { ts_std_ret_int (r, (int64_t) s->id); return TS_OK; }
  if (op == 2 || op == 3)
    {
      if (ts_std_int (a+1, &x, e, "surface") || ts_std_int (a+2, &y, e, "surface")) return TS_ERR_INVAL;
      if (op == 3) { TS_Status st = ts_surface_pixels (a, (size_t) x, (size_t) y, NULL, e); if (st) return st; }
      else { if (x < -32768 || x > 32767 || y < -32768 || y > 32767) return ts_std_inval (e, "surface: position out of range"); s->px = (int) x; s->py = (int) y; }
    }
  if (op == 4)
    {
      if (s->pending)
        { if (s->front) s->owner->allocated -= s->w * s->h;
          free (s->front); s->front = s->back; s->back = NULL;
          s->w = s->pw; s->h = s->ph; s->pending = false; s->mapped = true; }
      s->x = s->px; s->y = s->py; s->owner->dirty = true;
    }
  if (op == 5)
    {
      compositor *c = s->owner;
      for (size_t i = 0; i < c->count; i++) if (c->surfaces[i] == s)
        { memmove (c->surfaces + i, c->surfaces + i + 1, (c->count - i - 1) * sizeof s); c->surfaces[c->count - 1] = s; break; }
      c->dirty = true;
    }
  ts_std_ret_nil (r); return TS_OK;
}

static TS_Status
route_event (compositor *c, TS_Value *event, TS_Error *e)
{
  ts_dom_t *d = ts_dom_unwrap (event, e, "event");
  TS_Value type = {0}; surface *target = NULL;
  TS_Status st;
  if (!d) return TS_ERR_INVAL;
  const ts_dom_t *field = ts_dom_path (d, "type");
  if (!field) return ts_std_inval (e, "event: type required");
  st = ts_dom_to_value (field, &type, e); if (st) return st;
  if (type.type != TS_STRING) { ts_value_free (&type); return ts_std_inval (e, "event: type must be string"); }
  bool pointer = !strcmp (type.as.string, "motion") || !strcmp (type.as.string, "button_press") || !strcmp (type.as.string, "button_release");
  bool key = !strcmp (type.as.string, "key_press") || !strcmp (type.as.string, "key_release");
  if (pointer)
    {
      int64_t x = ts_doc_int (d, "x"), y = ts_doc_int (d, "y");
      /* Bounds avoid overflow even for caller-injected document integers. */
      if (x >= -32768 && x <= 65534 && y >= -32768 && y <= 65534)
        for (size_t i = c->count; i > 0; i--)
          { surface *s = c->surfaces[i-1];
            if (s->mapped && x >= s->x && y >= s->y && x - s->x < (int64_t) s->w && y - s->y < (int64_t) s->h)
              { target = s; break; } }
      if (target)
        {
          if (ts_dom_obj_set (d, "local_x", ts_dom_new_int (x - target->x)) ||
              ts_dom_obj_set (d, "local_y", ts_dom_new_int (y - target->y))) st = ts_std_oom (e);
          if (!strcmp (type.as.string, "button_press")) c->focus = target->id;
        }
    }
  else if (key)
    for (size_t i = 0; i < c->count; i++) if (c->surfaces[i]->id == c->focus) target = c->surfaces[i];
  if (!strcmp (type.as.string, "expose")) c->dirty = true;
  if (!st && ts_dom_obj_set (d, "surface", ts_dom_new_int (target ? (int64_t) target->id : 0))) st = ts_std_oom (e);
  ts_value_free (&type); return st;
}

static TS_Status
c_inject (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  compositor *c; ts_dom_t *d; TS_Value copy = {0}; TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 2, 2, e, "compositor.inject")) return TS_ERR_INVAL;
  c = comp_get (a, e); if (!c) return TS_ERR_INVAL;
  if (c->queued == EVENTS) return limit (e, "compositor: event queue full");
  d = ts_dom_unwrap (a+1, e, "event"); if (!d) return TS_ERR_INVAL;
  /* Validate and bound the owned snapshot before enqueue. */
  const ts_dom_t *type = ts_dom_path (d, "type");
  if (strcmp (ts_dom_kind_str (d), "object") || !type || strcmp (ts_dom_kind_str (type), "string"))
    return ts_std_inval (e, "event: object with string type required");
  char *text = NULL; ts_dom_t *cloned = NULL;
  st = ts_dom_stringify (d, &text); if (st) return ts_std_oom (e);
  if (strlen (text) > 16384) { free (text); return limit (e, "compositor: event exceeds 16 KiB"); }
  st = ts_dom_parse_json (text, &cloned, e); free (text); if (st) return st;
  st = ts_dom_wrap (&copy, cloned, e); if (st) return st;
  c->events[(c->head + c->queued++) % EVENTS] = copy;
  ts_std_ret_nil (r); return TS_OK;
}

static TS_Status
c_watch (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  compositor *c; TS_Value args[3];
  if (ts_std_argc (vm, n, ud ? 2 : 3, ud ? 2 : 3, e, "compositor.watch")) return TS_ERR_INVAL;
  c = comp_get (a, e); if (!c) return TS_ERR_INVAL;
  args[0] = c->poll; args[1] = a[1]; if (!ud) args[2] = a[2];
  return ts_module_call (vm, &ts_std_poll_module, ud ? "remove" : "watch", args, n, r, e);
}

static TS_Status
c_event (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  compositor *c; TS_Status st; TS_Value ready = {0};
  int64_t timeout;
  (void) ud;
  if (ts_std_argc (vm, n, 2, 2, e, "compositor.event") || ts_std_int (a+1, &timeout, e, "timeout")) return TS_ERR_INVAL;
  if (timeout < -1 || timeout > INT_MAX) return ts_std_inval (e, "invalid timeout");
  c = comp_get (a, e); if (!c) return TS_ERR_INVAL;
  if (c->queued)
    { *r = c->events[c->head]; c->events[c->head].type = TS_NIL; c->head = (c->head + 1) % EVENTS; c->queued--; return route_event (c, r, e); }
  if (c->output.type != TS_NIL)
    {
      st = ts_module_call (vm, &ts_std_x11_module, "event", &c->output, 1, r, e);
      if (st) return st;
      if (r->type != TS_NIL) return route_event (c, r, e);
    }
  TS_Value args[] = {c->poll, a[1]};
  st = ts_module_call (vm, &ts_std_poll_module, "wait", args, 2, &ready, e); if (st) return st;
  if (c->output.type != TS_NIL)
    {
      st = ts_module_call (vm, &ts_std_x11_module, "event", &c->output, 1, r, e);
      if (st || r->type != TS_NIL) { ts_value_free (&ready); return st ? st : route_event (c, r, e); }
    }
  ts_std_list_t *l = ts_std_list_unwrap (&ready, e, "ready");
  if (!l) { ts_value_free (&ready); return TS_ERR_INVAL; }
  if (!ts_std_list_len (l)) { ts_value_free (&ready); ts_std_ret_nil (r); return TS_OK; }
  ts_dom_t *d = ts_dom_new_obj (), *fds = ts_dom_new_arr ();
  if (!d || !fds) goto oom;
  for (size_t i = 0; i < ts_std_list_len (l); i++)
    if (ts_dom_arr_push (fds, ts_dom_new_int (ts_std_list_get (l, i)->as.integer))) goto oom;
  if (ts_dom_obj_set (d, "type", ts_dom_new_str ("ready"))) goto oom;
  st = ts_dom_obj_set (d, "fds", fds); fds = NULL; if (st) goto oom;
  ts_value_free (&ready); return ts_dom_wrap (r, d, e);
oom:
  ts_dom_free (d); ts_dom_free (fds); ts_value_free (&ready); return ts_std_oom (e);
}

static TS_Status
c_outputs (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  (void) ud;
  return ts_module_call (vm, &ts_std_drm_module, "connectors", a, n, r, e);
}

static uint32_t blend (uint32_t dst, uint32_t src)
{
  uint32_t alpha = src >> 24, result = 0;
  for (unsigned shift = 0; shift < 24; shift += 8)
    result |= (((((src >> shift) & 255) * alpha + ((dst >> shift) & 255) * (255 - alpha) + 127) / 255) << shift);
  return result;
}
static TS_Status
c_present (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  compositor *c; ts_std_list_t *list; TS_Value pixels = {0}, tmp = {0}; TS_Status st = TS_OK;
  if (ts_std_argc (vm, n, 1, 1, e, "compositor.present")) return TS_ERR_INVAL;
  c = comp_get (a, e); if (!c) return TS_ERR_INVAL;
  if (!ud && c->dirty)
    {
      if (c->serial == INT64_MAX) return limit (e, "compositor: serial exhausted");
      memset (c->frame, 0, c->w * c->h * sizeof *c->frame);
      for (size_t i = 0; i < c->count; i++)
        {
          surface *s = c->surfaces[i]; if (!s->mapped) continue;
          for (size_t y = 0; y < s->h; y++)
            { int dy = s->y + (int) y; if (dy < 0 || (size_t) dy >= c->h) continue;
              for (size_t x = 0; x < s->w; x++)
                { int dx = s->x + (int) x; if (dx < 0 || (size_t) dx >= c->w) continue;
                  size_t at = (size_t) dy * c->w + (size_t) dx;
                  c->frame[at] = blend (c->frame[at], s->front[y * s->w + x]); } }
        }
      if (c->output.type != TS_NIL)
        {
          /* Bound each Xlib upload and intermediate TS_Value list. */
          size_t band = 262144 / c->w;
          for (size_t top = 0; top < c->h && !st; top += band)
            {
              size_t height = c->h-top < band ? c->h-top : band;
              list = ts_std_list_new (); if (!list) { st = ts_std_oom (e); break; }
              for (size_t i = 0; i < c->w * height; i++)
                if (ts_std_list_push_int (list, c->frame[top*c->w+i])) { st = ts_std_oom (e); break; }
              if (st) { ts_std_list_free (list); break; }
              st = ts_std_list_wrap (&pixels, list, e);
              if (!st) { TS_Value args[] = {c->output, ts_arg_int (0), ts_arg_int (top), ts_arg_int (c->w), ts_arg_int (height), pixels};
                st = ts_module_call (vm, &ts_std_x11_module, "image", args, 6, &tmp, e); }
              ts_value_free (&pixels);
            }
          if (!st) st = ts_module_call (vm, &ts_std_x11_module, "flush", &c->output, 1, &tmp, e);
          ts_value_free (&pixels); ts_value_free (&tmp); if (st) return st;
        }
      c->serial++; c->dirty = false;
    }
  if (!ud) { ts_std_ret_int (r, (int64_t) c->serial); return TS_OK; }
  list = ts_std_list_new (); if (!list) return ts_std_oom (e);
  for (size_t i = 0; i < c->w * c->h; i++) if (ts_std_list_push_int (list, c->frame[i]))
    { ts_std_list_free (list); return ts_std_oom (e); }
  return ts_std_list_wrap (r, list, e);
}

static TS_Status
c_state (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e)
{
  compositor *c; intptr_t op = (intptr_t) ud;
  size_t want = op == 2 ? 2 : op == 3 ? 3 : 1;
  if (ts_std_argc (vm, n, want, want, e, "compositor")) return TS_ERR_INVAL;
  if (!op) { c = ts_std_handle (a, &comp_tag, e, "compositor.close"); if (!c) return TS_ERR_INVAL; comp_dispose (c); ts_std_ret_nil (r); return TS_OK; }
  c = comp_get (a, e); if (!c) return TS_ERR_INVAL;
  if (op == 1) c->dirty = true;
  if (op == 4)
    {
      ts_dom_t *d = ts_dom_new_obj ();
      if (!d || ts_dom_obj_set (d, "width", ts_dom_new_int (c->w)) ||
          ts_dom_obj_set (d, "height", ts_dom_new_int (c->h)) ||
          ts_dom_obj_set (d, "surfaces", ts_dom_new_int (c->count)) ||
          ts_dom_obj_set (d, "serial", ts_dom_new_int ((int64_t) c->serial)) ||
          ts_dom_obj_set (d, "focus", ts_dom_new_int ((int64_t) c->focus)) ||
          ts_dom_obj_set (d, "backend", ts_dom_new_str (c->output.type == TS_NIL ? "headless" : "x11")))
        { ts_dom_free (d); return ts_std_oom (e); }
      return ts_dom_wrap (r, d, e);
    }
  if (op == 2)
    {
      if (a[1].type == TS_NIL) c->focus = 0;
      else { surface *s = surf_get (a+1, e); if (!s) return TS_ERR_INVAL;
        if (s->owner != c) return ts_std_inval (e, "focus: foreign surface");
        c->focus = s->id; }
    }
  if (op == 3)
    {
      int64_t w, h; uint32_t *frame;
      if (ts_std_int (a+1, &w, e, "width") || ts_std_int (a+2, &h, e, "height") || !dimensions (w,h)) return ts_std_inval (e, "output: invalid dimensions");
      frame = calloc ((size_t) (w*h), sizeof *frame); if (!frame) return ts_std_oom (e);
      free (c->frame); c->frame = frame; c->w = (size_t) w; c->h = (size_t) h; c->dirty = true;
    }
  ts_std_ret_nil (r); return TS_OK;
}

static const TS_FuncDef funcs[] = {
  {"new", c_new, NULL}, {"surface", c_surface, NULL},
  {"attach", s_buffer, NULL}, {"fill", s_buffer, (void *) 1},
  {"destroy", s_state, NULL}, {"id", s_state, (void *) 1},
  {"position", s_state, (void *) 2}, {"resize", s_state, (void *) 3},
  {"commit", s_state, (void *) 4}, {"raise", s_state, (void *) 5},
  {"present", c_present, NULL}, {"frame", c_present, (void *) 1},
  {"close", c_state, NULL}, {"damage", c_state, (void *) 1},
  {"focus", c_state, (void *) 2}, {"output_size", c_state, (void *) 3},
  {"info", c_state, (void *) 4},
  {"inject", c_inject, NULL}, {"event", c_event, NULL},
  {"watch", c_watch, NULL}, {"unwatch", c_watch, (void *) 1},
  {"outputs", c_outputs, NULL}, {NULL, NULL, NULL}
};
const TS_Module ts_std_compositor_module = {"std.compositor", funcs};
