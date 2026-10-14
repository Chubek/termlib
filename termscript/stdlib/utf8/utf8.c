#include "utf8/utf8.h"
#include "common/text.h"
#include <stdlib.h>
#include <string.h>

static TS_Status
u_measure (TS_VM *vm, void *ud, const TS_Value *a, size_t n,
           TS_Value *ret, TS_Error *e)
{
  const char *s;
  size_t len, at = 0, count = 0;
  int32_t cp;
  TS_Status st;
  if (ts_std_argc (vm, n, 1, 1, e, "utf8") || ts_std_str (a, &s, e, "utf8"))
    return TS_ERR_INVAL;
  len = strlen (s);
  st = ts_utf8_validate (s, len, e);
  if ((intptr_t) ud == 0)
    {
      ts_std_ret_bool (ret, st == TS_OK);
      if (st == TS_ERR_PARSE) ts_error_clear (e);
      return st == TS_ERR_LIMIT ? st : TS_OK;
    }
  if (st) return st;
  if ((intptr_t) ud == 2) count = ts_text_width (s, len);
  else if ((intptr_t) ud == 3) count = len;
  else while (at < len)
    { at += (size_t) ts_utf8_decode (s + at, len - at, &cp); count++; }
  ts_std_ret_int (ret, (int64_t) count);
  return TS_OK;
}

static TS_Status
u_scalar (TS_VM *vm, void *ud, const TS_Value *a, size_t n,
        TS_Value *ret, TS_Error *e)
{
  int64_t cp;
  unsigned char out[5] = {0};
  (void) ud;
  if (ts_std_argc (vm, n, 1, 1, e, "char") || ts_std_int (a, &cp, e, "char"))
    return TS_ERR_INVAL;
  if (cp <= 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
    return ts_std_inval (e, "char: non-NUL Unicode scalar required");
  utf8proc_encode_char ((int32_t) cp, out);
  return ts_std_ret_str (ret, (char *) out, e);
}

static TS_Status
u_slice (TS_VM *vm, void *ud, const TS_Value *a, size_t n,
         TS_Value *ret, TS_Error *e)
{
  const char *s;
  int64_t start = 0, count = INT64_MAX;
  size_t at = 0, len, i = 0, begin, bytes;
  int32_t cp;
  TS_Status st;
  if (ts_std_argc (vm, n, 2, ud ? 2 : 3, e, "slice") ||
      ts_std_str (a, &s, e, "slice") || ts_std_int (a + 1, &start, e, "slice") ||
      (n == 3 && ts_std_int (a + 2, &count, e, "slice")))
    return TS_ERR_INVAL;
  if (start < 0 || count < 0) return ts_std_inval (e, "slice: negative index/count");
  len = strlen (s);
  st = ts_utf8_validate (s, len, e);
  if (st) return st;
  while (at < len && i++ < (uint64_t) start)
    at += (size_t) ts_utf8_decode (s + at, len - at, &cp);
  if (ud)
    {
      if (at == len) { ts_std_ret_nil (ret); return TS_OK; }
      ts_utf8_decode (s + at, len - at, &cp);
      ts_std_ret_int (ret, cp);
      return TS_OK;
    }
  begin = at;
  i = 0;
  while (at < len && i++ < (uint64_t) count)
    {
      bytes = (size_t) ts_utf8_decode (s + at, len - at, &cp);
      at += bytes;
    }
  return ts_std_ret_strn (ret, s + begin, at - begin, e);
}

static TS_Status
u_graphemes (TS_VM *vm, void *ud, const TS_Value *a, size_t n,
             TS_Value *ret, TS_Error *e)
{
  const char *s;
  size_t at = 0, len, bytes;
  int w;
  ts_std_list_t *list;
  TS_Value part = {0};
  TS_Status st;
  ts_sbuf_t b;
  if (ts_std_argc (vm, n, 1, 1, e, "graphemes") || ts_std_str (a, &s, e, "graphemes"))
    return TS_ERR_INVAL;
  len = strlen (s);
  st = ts_utf8_validate (s, len, e);
  if (st) return st;
  list = ts_std_list_new ();
  if (!list) return ts_std_oom (e);
  while (at < len)
    {
      ts_text_cluster (s + at, len - at, &bytes, &w);
      st = ts_std_ret_strn (&part, s + at, bytes, e);
      if (!st) st = ts_std_list_push (list, &part);
      ts_value_free (&part);
      if (st) { ts_std_list_free (list); return ts_std_oom (e); }
      at += bytes;
    }
  if (!ud) return ts_std_list_wrap (ret, list, e);
  ts_sbuf_init (&b);
  for (at = ts_std_list_len (list); at > 0; at--)
    if (ts_sbuf_str (&b, ts_std_list_get (list, at - 1)->as.string))
      { ts_sbuf_free (&b); ts_std_list_free (list); return ts_std_oom (e); }
  ts_std_list_free (list);
  return ts_std_ret_sbuf (ret, &b, e);
}

static TS_Status
u_normalize (TS_VM *vm, void *ud, const TS_Value *a, size_t n,
             TS_Value *ret, TS_Error *e)
{
  const char *s, *form = "NFC";
  utf8proc_uint8_t *out = NULL;
  utf8proc_option_t flags = UTF8PROC_STABLE;
  utf8proc_ssize_t r;
  TS_Status st;
  if (ts_std_argc (vm, n, 1, ud ? 1 : 2, e, "normalize") ||
      ts_std_str (a, &s, e, "normalize") ||
      (n == 2 && ts_std_str (a + 1, &form, e, "normalize"))) return TS_ERR_INVAL;
  st = ts_utf8_validate (s, strlen (s), e);
  if (st) return st;
  if (ud) flags |= UTF8PROC_CASEFOLD | UTF8PROC_COMPOSE;
  else if (!strcmp (form, "NFC")) flags |= UTF8PROC_COMPOSE;
  else if (!strcmp (form, "NFD")) flags |= UTF8PROC_DECOMPOSE;
  else if (!strcmp (form, "NFKC")) flags |= UTF8PROC_COMPOSE | UTF8PROC_COMPAT;
  else if (!strcmp (form, "NFKD")) flags |= UTF8PROC_DECOMPOSE | UTF8PROC_COMPAT;
  else return ts_std_inval (e, "normalize: expected NFC/NFD/NFKC/NFKD");
  r = utf8proc_map ((const unsigned char *) s, (utf8proc_ssize_t) strlen (s), &out, flags);
  if (r < 0) return ts_std_oom (e);
  st = ts_std_ret_str (ret, (char *) out, e);
  free (out);
  return st;
}

static TS_Status
u_sanitize (TS_VM *vm, void *ud, const TS_Value *a, size_t n,
            TS_Value *ret, TS_Error *e)
{
  const char *s;
  size_t at = 0, len;
  ts_sbuf_t b;
  int32_t cp;
  (void) ud;
  if (ts_std_argc (vm, n, 1, 1, e, "sanitize") || ts_std_str (a, &s, e, "sanitize"))
    return TS_ERR_INVAL;
  len = strlen (s);
  if (len > TS_TEXT_LIMIT) { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "text exceeds 1 MiB"); return TS_ERR_LIMIT; }
  ts_sbuf_init (&b);
  while (at < len)
    {
      int r = ts_utf8_decode (s + at, len - at, &cp);
      if (ts_sbuf_put (&b, r < 0 ? "\xef\xbf\xbd" : s + at, r < 0 ? 3 : (size_t) r))
        { ts_sbuf_free (&b); return ts_std_oom (e); }
      at += r < 0 ? 1 : (size_t) r;
    }
  return ts_std_ret_sbuf (ret, &b, e);
}

static const TS_FuncDef funcs[] = {
  {"valid", u_measure, NULL}, {"len", u_measure, (void *) 1},
  {"width", u_measure, (void *) 2}, {"bytes", u_measure, (void *) 3},
  {"char", u_scalar, NULL}, {"codepoint", u_slice, (void *) 1},
  {"slice", u_slice, NULL}, {"graphemes", u_graphemes, NULL},
  {"reverse", u_graphemes, (void *) 1}, {"normalize", u_normalize, NULL},
  {"casefold", u_normalize, (void *) 1}, {"sanitize", u_sanitize, NULL},
};
const TS_Module ts_std_utf8_module = TS_MODULE_INIT ("std.utf8", funcs);
