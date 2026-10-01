#include "prompt_string/prompt_string.h"
#include "common/text.h"
#include "map/map.h"
#include <errno.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static TS_Status
lookup (TS_VM *vm, const TS_Value *map, const char *name, TS_Value *ret, TS_Error *e)
{
  if (map)
    {
      TS_Value args[2] = { *map, {0} };
      TS_Status st = ts_value_make_string (&args[1], name);
      if (st) return ts_std_oom (e);
      const TS_FuncDef *fn = ts_std_map_module.funcs;
      while (strcmp (fn->name, "get")) fn++;
      st = fn->func (vm, fn->userdata, args, 2, ret, e);
      ts_value_free (&args[1]);
      if (st || ret->type != TS_NIL) return st;
    }
  if (!strcmp (name, "cwd") || !strcmp (name, "basename"))
    {
      char *cwd = getcwd (NULL, 0), *base;
      TS_Status st;
      if (!cwd) return ts_std_system (e, "getcwd");
      base = strrchr (cwd, '/');
      st = ts_std_ret_str (ret, !strcmp (name, "basename") && base && base[1] ? base + 1 : cwd, e);
      free (cwd); return st;
    }
  if (!strcmp (name, "host"))
    { char host[256]; if (gethostname (host, sizeof host)) return ts_std_system (e, "gethostname"); host[255] = 0; return ts_std_ret_str (ret, host, e); }
  if (!strcmp (name, "uid")) { ts_std_ret_int (ret, getuid ()); return TS_OK; }
  if (!strcmp (name, "user") || !strcmp (name, "home"))
    {
      struct passwd pw, *result = NULL;
      char buffer[16384];
      int r = getpwuid_r (getuid (), &pw, buffer, sizeof buffer, &result);
      if (r) { errno = r; return ts_std_system (e, "getpwuid_r"); }
      if (result) return ts_std_ret_str (ret, !strcmp (name, "user") ? pw.pw_name : pw.pw_dir, e);
      return ts_std_ret_str (ret, "", e);
    }
  if (!strcmp (name, "time") || !strncmp (name, "time:", 5))
    {
      time_t now = time (NULL); struct tm tm; char buffer[1024];
      if (!localtime_r (&now, &tm)) return ts_std_system (e, "localtime");
      const char *format = name[4] ? name + 5 : "%H:%M:%S";
      if (format[0] && !strftime (buffer, sizeof buffer, format, &tm))
        { ts_error_set (e, TS_ERR_LIMIT, 0, 0, "prompt time format exceeds 1023 bytes"); return TS_ERR_LIMIT; }
      if (!format[0]) buffer[0] = 0;
      return ts_std_ret_str (ret, buffer, e);
    }
  return ts_std_inval (e, "prompt_string: unknown placeholder");
}

static TS_Status
p_expand (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  const char *s;
  const TS_Value *map = n == 2 ? a + 1 : NULL;
  ts_sbuf_t b;
  TS_Status st;
  size_t i = 0, len;
  (void) ud;
  if (ts_std_argc (vm, n, 1, 2, e, "expand") || ts_std_str (a, &s, e, "expand")) return TS_ERR_INVAL;
  len = strlen (s); st = ts_utf8_validate (s, len, e); if (st) return st;
  ts_sbuf_init (&b);
  while (i < len)
    {
      if (s[i] == '$' && s[i + 1] == '$')
        { if (ts_sbuf_ch (&b, '$')) goto oom; i += 2; }
      else if (s[i] == '$' && s[i + 1] == '{')
        {
          const char *end = strchr (s + i + 2, '}');
          char name[256]; TS_Value value = {0}; char *text;
          if (!end || end == s + i + 2 || (size_t) (end - s - i - 2) >= sizeof name)
            { ts_sbuf_free (&b); ts_error_set (e, TS_ERR_PARSE, 0, i, "malformed prompt placeholder"); return TS_ERR_PARSE; }
          size_t count = (size_t) (end - s - i - 2);
          memcpy (name, s + i + 2, count); name[count] = 0;
          st = lookup (vm, map, name, &value, e);
          if (st) { ts_sbuf_free (&b); return st; }
          text = ts_std_to_str (&value, e); ts_value_free (&value);
          if (!text) goto oom;
          int r = ts_sbuf_str (&b, text); free (text); if (r) goto oom;
          i = (size_t) (end - s) + 1;
        }
      else { if (ts_sbuf_ch (&b, s[i++])) goto oom; }
      if (b.len > TS_TEXT_LIMIT) { ts_sbuf_free (&b); ts_error_set (e, TS_ERR_LIMIT, 0, i, "prompt exceeds 1 MiB"); return TS_ERR_LIMIT; }
    }
  return ts_std_ret_sbuf (ret, &b, e);
oom: ts_sbuf_free (&b); return ts_std_oom (e);
}

static TS_Status
p_text (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  const char *s;
  size_t len, at = 0;
  ts_sbuf_t b;
  if (ts_std_argc (vm, n, 1, 1, e, "prompt_string") || ts_std_str (a, &s, e, "prompt_string")) return TS_ERR_INVAL;
  len = strlen (s);
  if (!ud) { ts_std_ret_int (ret, (int64_t) ts_text_width (s, len)); return TS_OK; }
  ts_sbuf_init (&b);
  if ((intptr_t) ud == 1)
    { if (ts_text_strip (&b, s, len)) goto oom; }
  else
    while (at < len)
      {
        size_t esc = ts_text_escape (s + at, len - at);
        if (s[at] == '\001' || s[at] == '\002') { at++; continue; }
        if (esc)
          { if (ts_sbuf_ch (&b, '\001') || ts_sbuf_put (&b, s + at, esc) || ts_sbuf_ch (&b, '\002')) goto oom; at += esc; }
        else { if (ts_sbuf_ch (&b, s[at++])) goto oom; }
      }
  return ts_std_ret_sbuf (ret, &b, e);
oom: ts_sbuf_free (&b); return ts_std_oom (e);
}

static const TS_FuncDef funcs[] = {
  {"expand", p_expand, NULL}, {"width", p_text, NULL}, {"strip", p_text, (void *) 1},
  {"readline", p_text, (void *) 2}, {NULL, NULL, NULL}
};
const TS_Module ts_std_prompt_string_module = {"std.prompt_string", funcs};
