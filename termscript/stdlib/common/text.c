#include "common/text.h"
#include <string.h>

int
ts_utf8_decode (const char *s, size_t n, int32_t *cp)
{
  utf8proc_ssize_t r;
  if (!n)
    return -1;
  r = utf8proc_iterate ((const utf8proc_uint8_t *) s,
                       (utf8proc_ssize_t) (n > 4 ? 4 : n), cp);
  return r > 0 ? (int) r : -1;
}

TS_Status
ts_utf8_validate (const char *s, size_t n, TS_Error *error)
{
  size_t i = 0;
  int32_t cp;
  if (n > TS_TEXT_LIMIT)
    {
      ts_error_set (error, TS_ERR_LIMIT, 0, 0, "text exceeds 1 MiB");
      return TS_ERR_LIMIT;
    }
  while (i < n)
    {
      int r = ts_utf8_decode (s + i, n - i, &cp);
      if (r < 0)
        {
          ts_error_set (error, TS_ERR_PARSE, 0, i, "invalid UTF-8 at byte %zu", i);
          return TS_ERR_PARSE;
        }
      i += (size_t) r;
    }
  return TS_OK;
}

int
ts_text_cluster (const char *s, size_t n, size_t *bytes, int *width)
{
  int32_t prev, cp, state = 0;
  int r, w;
  size_t at;
  bool emoji = false;
  if (!n)
    return 0;
  r = ts_utf8_decode (s, n, &prev);
  if (r < 0)
    return -1;
  at = (size_t) r;
  w = utf8proc_charwidth (prev);
  if (w < 0)
    w = 0;
  while (at < n)
    {
      r = ts_utf8_decode (s + at, n - at, &cp);
      if (r < 0)
        break; /* The next cluster will report the malformed byte. */
      if (utf8proc_grapheme_break_stateful (prev, cp, &state))
        break;
      if (utf8proc_charwidth (cp) > w)
        w = utf8proc_charwidth (cp);
      if (cp == 0xfe0f || cp == 0x20e3 ||
          (prev >= 0x1f1e6 && prev <= 0x1f1ff &&
           cp >= 0x1f1e6 && cp <= 0x1f1ff))
        emoji = true;
      prev = cp;
      at += (size_t) r;
    }
  *bytes = at;
  *width = emoji && w > 0 ? 2 : w;
  return 1;
}

size_t
ts_text_escape (const char *s, size_t n)
{
  size_t i;
  if (!n || (unsigned char) s[0] != 0x1b)
    return 0;
  if (n == 1)
    return 1;
  if (s[1] == '[')
    {
      for (i = 2; i < n; i++)
        if (s[i] >= '@' && s[i] <= '~')
          return i + 1;
      return n;
    }
  if (s[1] == ']' || s[1] == 'P' || s[1] == '^' || s[1] == '_')
    {
      for (i = 2; i < n; i++)
        if ((s[1] == ']' && s[i] == '\a') ||
            (s[i] == '\033' && i + 1 < n && s[i + 1] == '\\'))
          return i + (s[i] == '\a' ? 1 : 2);
      return n;
    }
  i = 1;
  while (i < n && s[i] >= ' ' && s[i] <= '/')
    i++;
  return i < n ? i + 1 : n;
}

size_t
ts_text_width (const char *s, size_t n)
{
  size_t at = 0, col = 0, best = 0, bytes;
  bool hidden = false;
  int w;
  while (at < n)
    {
      size_t esc = ts_text_escape (s + at, n - at);
      if (esc)
        { at += esc; continue; }
      if (s[at] == '\001')
        { hidden = true; at++; continue; }
      if (s[at] == '\002')
        { hidden = false; at++; continue; }
      if (hidden)
        { at++; continue; }
      if (s[at] == '\n' || s[at] == '\r')
        {
          if (col > best) best = col;
          col = 0;
          at++;
          continue;
        }
      if (s[at] == '\t')
        { col = (col + 8) & ~(size_t) 7; at++; continue; }
      if (ts_text_cluster (s + at, n - at, &bytes, &w) < 0)
        { bytes = 1; w = 1; }
      col += (size_t) w;
      at += bytes;
    }
  return col > best ? col : best;
}

int
ts_text_strip (ts_sbuf_t *b, const char *s, size_t n)
{
  size_t at = 0;
  bool hidden = false;
  while (at < n)
    {
      size_t esc = ts_text_escape (s + at, n - at);
      if (esc) { at += esc; continue; }
      if (s[at] == '\001') hidden = true;
      else if (s[at] == '\002') hidden = false;
      else if (!hidden && ts_sbuf_ch (b, s[at]) != 0) return -1;
      at++;
    }
  return 0;
}

int
ts_text_clip (ts_sbuf_t *b, const char *s, size_t n, size_t columns)
{
  size_t at = 0, col = 0, bytes;
  bool hidden = false;
  int w;
  while (at < n && s[at] != '\n' && s[at] != '\r')
    {
      size_t esc = ts_text_escape (s + at, n - at);
      if (esc)
        {
          if (ts_sbuf_put (b, s + at, esc)) return -1;
          at += esc;
          continue;
        }
      if (s[at] == '\001' || s[at] == '\002')
        {
          hidden = s[at] == '\001';
          if (ts_sbuf_ch (b, s[at++])) return -1;
          continue;
        }
      if (hidden)
        { if (ts_sbuf_ch (b, s[at++])) return -1; continue; }
      if (s[at] == '\t')
        {
          size_t spaces = 8 - col % 8;
          if (spaces > columns - col) spaces = columns - col;
          while (spaces--) { if (ts_sbuf_ch (b, ' ')) return -1; col++; }
          at++;
          continue;
        }
      if (ts_text_cluster (s + at, n - at, &bytes, &w) < 0)
        { bytes = 1; w = 1; }
      if (col + (size_t) w > columns) break;
      if (ts_sbuf_put (b, s + at, bytes)) return -1;
      col += (size_t) w;
      at += bytes;
    }
  return 0;
}
