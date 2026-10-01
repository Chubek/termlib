#include "font/font.h"
#include "common/text.h"
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb-ot.h>
#include <stdlib.h>
#include <string.h>

typedef struct { FT_Library library; FT_Face face; hb_font_t *hb; char *file; } font_t;
static const char font_tag;
static void
font_free (void *p)
{
  font_t *f = p;
  if (!f) return;
  if (f->hb) hb_font_destroy (f->hb);
  if (f->face) FT_Done_Face (f->face);
  if (f->library) FT_Done_FreeType (f->library);
  free (f->file); free (f);
}

static TS_Status
open_font (TS_Value *ret, const char *file, int index, TS_Error *e)
{
  font_t *f = calloc (1, sizeof *f);
  FT_Error r;
  if (!f) return ts_std_oom (e);
  f->file = strdup (file);
  if (!f->file) { font_free (f); return ts_std_oom (e); }
  r = FT_Init_FreeType (&f->library);
  if (!r) r = FT_New_Face (f->library, file, index, &f->face);
  if (!r) r = FT_Select_Charmap (f->face, FT_ENCODING_UNICODE);
  if (r) { font_free (f); ts_error_set (e, TS_ERR_SYSTEM, 0, 0, "FreeType: cannot open Unicode font (error %d)", r); return TS_ERR_SYSTEM; }
  hb_face_t *face = hb_ft_face_create_referenced (f->face);
  f->hb = hb_font_create (face);
  hb_face_destroy (face);
  hb_ot_font_set_funcs (f->hb);
  if (ts_value_make_handle (ret, f, font_free, "<font>", &font_tag)) { font_free (f); return ts_std_oom (e); }
  return TS_OK;
}

static TS_Status
f_match (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  const char *name = "monospace";
  FcConfig *config;
  FcPattern *pattern, *match = NULL;
  FcResult result;
  FcChar8 *file;
  int index = 0;
  TS_Status st = TS_OK;
  (void) ud;
  if (ts_std_argc (vm, n, 0, 1, e, "match") || (n && ts_std_str (a, &name, e, "match"))) return TS_ERR_INVAL;
  config = FcInitLoadConfigAndFonts ();
  pattern = FcNameParse ((const FcChar8 *) name);
  if (!config || !pattern) { if (config) FcConfigDestroy (config); if (pattern) FcPatternDestroy (pattern); return ts_std_oom (e); }
  FcConfigSubstitute (config, pattern, FcMatchPattern);
  FcDefaultSubstitute (pattern);
  match = FcFontMatch (config, pattern, &result);
  if (!match || FcPatternGetString (match, FC_FILE, 0, &file) != FcResultMatch) ts_std_ret_nil (ret);
  else { FcPatternGetInteger (match, FC_INDEX, 0, &index); st = open_font (ret, (char *) file, index, e); }
  if (match) FcPatternDestroy (match);
  FcPatternDestroy (pattern); FcConfigDestroy (config);
  return st;
}

static TS_Status
f_open (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  const char *file;
  int64_t index = 0;
  (void) ud;
  if (ts_std_argc (vm, n, 1, 2, e, "open") || ts_std_str (a, &file, e, "open") ||
      (n == 2 && ts_std_int (a + 1, &index, e, "open"))) return TS_ERR_INVAL;
  if (index < 0 || index > INT32_MAX) return ts_std_inval (e, "open: invalid font index");
  return open_font (ret, file, (int) index, e);
}

static TS_Status
f_get (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  font_t *f;
  if (ts_std_argc (vm, n, 1, 1, e, "font")) return TS_ERR_INVAL;
  f = ts_std_handle (a, &font_tag, e, "font");
  if (!f) return TS_ERR_INVAL;
  if ((intptr_t) ud == 3) { ts_std_ret_int (ret, f->face->face_index); return TS_OK; }
  if ((intptr_t) ud == 4) { ts_std_ret_int (ret, hb_face_get_upem (hb_font_get_face (f->hb))); return TS_OK; }
  return ts_std_ret_str (ret, !ud ? f->file : (intptr_t) ud == 1 ? f->face->family_name : f->face->style_name, e);
}

static TS_Status
f_has (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  font_t *f;
  const char *s;
  size_t at = 0, len;
  TS_Status st;
  int32_t cp;
  bool has = true;
  (void) ud;
  if (ts_std_argc (vm, n, 2, 2, e, "has")) return TS_ERR_INVAL;
  f = ts_std_handle (a, &font_tag, e, "has");
  if (!f) return TS_ERR_INVAL;
  if (a[1].type == TS_INT)
    {
      int64_t value = a[1].as.integer;
      if (value < 1 || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return ts_std_inval (e, "has: invalid Unicode scalar");
      has = FT_Get_Char_Index (f->face, (FT_ULong) value) != 0;
    }
  else
    {
      if (ts_std_str (a + 1, &s, e, "has")) return TS_ERR_INVAL;
      len = strlen (s); st = ts_utf8_validate (s, len, e); if (st) return st;
      while (at < len)
        {
          at += (size_t) ts_utf8_decode (s + at, len - at, &cp);
          if (utf8proc_category (cp) != UTF8PROC_CATEGORY_CF && cp != '\n' && cp != '\r' && cp != '\t' && !FT_Get_Char_Index (f->face, (FT_ULong) cp)) has = false;
        }
    }
  ts_std_ret_bool (ret, has); return TS_OK;
}

static TS_Status
f_list (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  const char *name = "";
  int64_t limit = 256;
  FcConfig *config = NULL;
  FcPattern *pattern = NULL;
  FcObjectSet *objects = NULL;
  FcFontSet *set = NULL;
  ts_std_list_t *l = NULL;
  TS_Status st = TS_OK;
  int i;
  (void) ud;
  if (ts_std_argc (vm, n, 0, 2, e, "list") || (n && ts_std_str (a, &name, e, "list")) ||
      (n == 2 && ts_std_int (a + 1, &limit, e, "list"))) return TS_ERR_INVAL;
  if (limit < 0 || limit > 10000) return ts_std_inval (e, "list: limit must be 0..10000");
  config = FcInitLoadConfigAndFonts (); pattern = FcNameParse ((const FcChar8 *) name);
  objects = FcObjectSetBuild (FC_FAMILY, FC_STYLE, FC_FILE, FC_INDEX, NULL);
  l = ts_std_list_new ();
  if (!config || !pattern || !objects || !l) { st = ts_std_oom (e); goto done; }
  set = FcFontList (config, pattern, objects);
  if (!set) { st = ts_std_oom (e); goto done; }
  for (i = 0; i < set->nfont && i < limit; i++)
    {
      FcChar8 *family = (FcChar8 *) "", *style = (FcChar8 *) "", *file = (FcChar8 *) "";
      int index = 0;
      TS_Value v = {0};
      ts_dom_t *d = ts_dom_new_obj ();
      FcPatternGetString (set->fonts[i], FC_FAMILY, 0, &family);
      FcPatternGetString (set->fonts[i], FC_STYLE, 0, &style);
      FcPatternGetString (set->fonts[i], FC_FILE, 0, &file);
      FcPatternGetInteger (set->fonts[i], FC_INDEX, 0, &index);
      if (!d || ts_dom_obj_set (d, "family", ts_dom_new_str ((char *) family)) ||
          ts_dom_obj_set (d, "style", ts_dom_new_str ((char *) style)) ||
          ts_dom_obj_set (d, "file", ts_dom_new_str ((char *) file)) ||
          ts_dom_obj_set (d, "index", ts_dom_new_int (index)))
        { ts_dom_free (d); st = ts_std_oom (e); goto done; }
      st = ts_dom_wrap (&v, d, e);
      if (!st) st = ts_std_list_push (l, &v);
      ts_value_free (&v);
      if (st) goto done;
    }
  st = ts_std_list_wrap (ret, l, e); l = NULL;
done:
  if (set) FcFontSetDestroy (set);
  if (objects) FcObjectSetDestroy (objects);
  if (pattern) FcPatternDestroy (pattern);
  if (config) FcConfigDestroy (config);
  ts_std_list_free (l);
  return st;
}

static TS_Status
f_shape (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  font_t *f;
  const char *text, *direction = "auto", *language = "fa";
  hb_buffer_t *buffer;
  hb_glyph_info_t *info;
  hb_glyph_position_t *pos;
  unsigned count, i;
  ts_std_list_t *l;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, n, 2, 4, e, "shape") || ts_std_str (a + 1, &text, e, "shape") ||
      (n > 2 && ts_std_str (a + 2, &direction, e, "shape")) ||
      (n > 3 && ts_std_str (a + 3, &language, e, "shape"))) return TS_ERR_INVAL;
  f = ts_std_handle (a, &font_tag, e, "shape"); if (!f) return TS_ERR_INVAL;
  st = ts_utf8_validate (text, strlen (text), e); if (st) return st;
  if (strcmp (direction, "auto") && strcmp (direction, "rtl") && strcmp (direction, "ltr")) return ts_std_inval (e, "shape: direction must be auto/ltr/rtl");
  buffer = hb_buffer_create (); l = ts_std_list_new ();
  if (!l || !hb_buffer_allocation_successful (buffer)) { hb_buffer_destroy (buffer); ts_std_list_free (l); return ts_std_oom (e); }
  hb_buffer_add_utf8 (buffer, text, (int) strlen (text), 0, -1);
  if (strcmp (direction, "auto")) hb_buffer_set_direction (buffer, !strcmp (direction, "rtl") ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
  hb_buffer_set_language (buffer, hb_language_from_string (language, -1));
  hb_buffer_guess_segment_properties (buffer);
  hb_shape (f->hb, buffer, NULL, 0);
  if (!hb_buffer_allocation_successful (buffer)) { st = ts_std_oom (e); goto done; }
  info = hb_buffer_get_glyph_infos (buffer, &count); pos = hb_buffer_get_glyph_positions (buffer, &count);
  for (i = 0; i < count; i++)
    {
      ts_dom_t *d = ts_dom_new_obj ();
      TS_Value v = {0};
      if (!d || ts_dom_obj_set (d, "glyph", ts_dom_new_int (info[i].codepoint)) ||
          ts_dom_obj_set (d, "cluster", ts_dom_new_int (info[i].cluster)) ||
          ts_dom_obj_set (d, "x_advance", ts_dom_new_int (pos[i].x_advance)) ||
          ts_dom_obj_set (d, "y_advance", ts_dom_new_int (pos[i].y_advance)) ||
          ts_dom_obj_set (d, "x_offset", ts_dom_new_int (pos[i].x_offset)) ||
          ts_dom_obj_set (d, "y_offset", ts_dom_new_int (pos[i].y_offset)))
        { ts_dom_free (d); st = ts_std_oom (e); goto done; }
      st = ts_dom_wrap (&v, d, e); if (!st) st = ts_std_list_push (l, &v); ts_value_free (&v);
      if (st) goto done;
    }
  st = ts_std_list_wrap (ret, l, e); l = NULL;
done: hb_buffer_destroy (buffer); ts_std_list_free (l); return st;
}

static const TS_FuncDef funcs[] = {
  {"match", f_match, NULL}, {"open", f_open, NULL}, {"list", f_list, NULL},
  {"file", f_get, NULL}, {"family", f_get, (void *) 1}, {"style", f_get, (void *) 2},
   {"index", f_get, (void *) 3}, {"units_per_em", f_get, (void *) 4}, {"has", f_has, NULL}, {"shape", f_shape, NULL},
  {NULL, NULL, NULL}
};
const TS_Module ts_std_font_module = {"std.font", funcs};
const TS_Module ts_std_font_discovery_module = {"std.font_discovery", funcs};
