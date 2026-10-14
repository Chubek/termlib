/* UAX #9 plus Arabic/Persian joining, without FriBidi's global legacy flags. */
#include "bidi/bidi.h"
#include "common/text.h"
#include <fribidi.h>
#include <stdlib.h>
#include <string.h>

static TS_Status
b_convert (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  const char *s, *direction = "auto";
  intptr_t op = (intptr_t) ud;
  bool shape = op == 0;
  FriBidiParType requested = FRIBIDI_PAR_ON, resolved = FRIBIDI_PAR_ON;
  FriBidiChar *chars = NULL;
  FriBidiCharType *types = NULL;
  FriBidiBracketType *brackets = NULL;
  FriBidiArabicProp *joining = NULL;
  FriBidiLevel *levels = NULL;
  FriBidiStrIndex *map = NULL, *inverse = NULL;
  size_t len, at = 0, count = 0, start;
  ts_sbuf_t b;
  ts_std_list_t *list = NULL;
  TS_Status st;
  if (ts_std_argc (vm, n, 1, op < 2 ? 3 : 2, e, "bidi") || ts_std_str (a, &s, e, "bidi") ||
      (n > 1 && ts_std_str (a + 1, &direction, e, "bidi"))) return TS_ERR_INVAL;
  if (n == 3) { if (a[2].type != TS_BOOL) return ts_std_inval (e, "bidi: bool shaping selector required"); shape = a[2].as.boolean; }
  if (!strcmp (direction, "ltr")) requested = FRIBIDI_PAR_LTR;
  else if (!strcmp (direction, "rtl")) requested = FRIBIDI_PAR_RTL;
  else if (strcmp (direction, "auto")) return ts_std_inval (e, "bidi: direction must be auto/ltr/rtl");
  len = strlen (s);
  st = ts_utf8_validate (s, len, e);
  if (st) return st;
  /* At most one code point per input byte; the 1 MiB bound covers indices. */
  chars = calloc (len + 1, sizeof *chars);
  types = calloc (len + 1, sizeof *types);
  brackets = calloc (len + 1, sizeof *brackets);
  joining = calloc (len + 1, sizeof *joining);
  levels = calloc (len + 1, sizeof *levels);
  map = calloc (len + 1, sizeof *map);
  inverse = calloc (len + 1, sizeof *inverse);
  ts_sbuf_init (&b);
  if (!chars || !types || !brackets || !joining || !levels || !map || !inverse) { st = ts_std_oom (e); goto done; }
  while (at < len)
    { int32_t cp; at += (size_t) ts_utf8_decode (s + at, len - at, &cp); chars[count] = (FriBidiChar) cp; map[count] = (FriBidiStrIndex) count; count++; }
  for (start = 0; start < count;)
    {
      size_t end = start;
      FriBidiParType base = requested;
      while (end < count && chars[end] != '\n' && chars[end] != '\r' && chars[end] != 0x2029) end++;
      if (end > start)
        {
          FriBidiStrIndex length = (FriBidiStrIndex) (end - start);
          fribidi_get_bidi_types (chars + start, length, types + start);
          fribidi_get_bracket_types (chars + start, length, types + start, brackets + start);
          if (!fribidi_get_par_embedding_levels_ex (types + start, brackets + start, length, &base, levels + start))
            { st = ts_std_oom (e); goto done; }
          if (resolved == FRIBIDI_PAR_ON) resolved = base;
          if (shape)
            {
              fribidi_get_joining_types (chars + start, length, joining + start);
              fribidi_join_arabic (types + start, length, levels + start, joining + start);
            }
          fribidi_shape (FRIBIDI_FLAG_SHAPE_MIRRORING | (shape ? FRIBIDI_FLAGS_ARABIC : 0),
                         levels + start, length, shape ? joining + start : NULL, chars + start);
          if (!fribidi_reorder_line (FRIBIDI_FLAG_REORDER_NSM, types + start, length, 0, base,
                                    levels + start, chars + start, map + start))
            { st = ts_std_oom (e); goto done; }
        }
      start = end < count ? end + 1 : count;
    }
  for (at = 0; at < count; at++) inverse[map[at]] = (FriBidiStrIndex) at;
  if (op == 2)
    { st = ts_std_ret_str (ret, resolved == FRIBIDI_PAR_RTL ? "rtl" : "ltr", e); goto done; }
  if (op >= 3)
    {
      list = ts_std_list_new ();
      if (!list) { st = ts_std_oom (e); goto done; }
      for (at = 0; at < count; at++)
        if (ts_std_list_push_int (list, op == 3 ? levels[at] : op == 4 ? inverse[at] : map[at]))
          { st = ts_std_oom (e); goto done; }
      st = ts_std_list_wrap (ret, list, e); list = NULL;
      goto done;
    }
  for (at = 0; at < count; at++)
    {
      unsigned char out[5] = {0};
      FriBidiChar cp = chars[at];
      /* Remove formatting controls and lam-alef padding from display text.
       * Preserve ZWNJ/ZWJ, whose joining effect has already been applied. */
      if (cp == 0xfeff || cp == 0x200e || cp == 0x200f || cp == 0x061c ||
          (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069)) continue;
      int bytes = (int) utf8proc_encode_char ((int32_t) cp, out);
      if (ts_sbuf_put (&b, (char *) out, (size_t) bytes)) { st = ts_std_oom (e); goto done; }
    }
  st = ts_std_ret_sbuf (ret, &b, e);
done:
  ts_sbuf_free (&b); ts_std_list_free (list);
  free (chars); free (types); free (brackets); free (joining); free (levels); free (map); free (inverse);
  return st;
}

static const TS_FuncDef funcs[] = {
  {"visual", b_convert, NULL}, {"persian", b_convert, NULL},
  {"reorder", b_convert, (void *) 1}, {"direction", b_convert, (void *) 2},
  {"levels", b_convert, (void *) 3}, {"logical_to_visual", b_convert, (void *) 4},
  {"visual_to_logical", b_convert, (void *) 5}
};
const TS_Module ts_std_bidi_module = TS_MODULE_INIT ("std.bidi", funcs);
