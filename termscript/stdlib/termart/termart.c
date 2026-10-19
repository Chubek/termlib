/* std.termart -- bounded, side-effect-free logo and terminal art strings. */
#include "termart/termart.h"
#include "common/text.h"
#include "common/ts_std_common.h"

#include <string.h>

#define ART_LIMIT (1024u * 1024u)
#define ART_WIDTH 4096

/* Original 5x7 bitmap alphabet. Each row uses its low five bits. */
static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -.!?";
static const unsigned char glyphs[][7]
    = { { 14, 17, 17, 31, 17, 17, 17 }, { 30, 17, 17, 30, 17, 17, 30 },
        { 14, 17, 16, 16, 16, 17, 14 }, { 30, 17, 17, 17, 17, 17, 30 },
        { 31, 16, 16, 30, 16, 16, 31 }, { 31, 16, 16, 30, 16, 16, 16 },
        { 14, 17, 16, 23, 17, 17, 15 }, { 17, 17, 17, 31, 17, 17, 17 },
        { 31, 4, 4, 4, 4, 4, 31 },      { 7, 2, 2, 2, 18, 18, 12 },
        { 17, 18, 20, 24, 20, 18, 17 }, { 16, 16, 16, 16, 16, 16, 31 },
        { 17, 27, 21, 21, 17, 17, 17 }, { 17, 25, 21, 19, 17, 17, 17 },
        { 14, 17, 17, 17, 17, 17, 14 }, { 30, 17, 17, 30, 16, 16, 16 },
        { 14, 17, 17, 17, 21, 18, 13 }, { 30, 17, 17, 30, 20, 18, 17 },
        { 15, 16, 16, 14, 1, 1, 30 },   { 31, 4, 4, 4, 4, 4, 4 },
        { 17, 17, 17, 17, 17, 17, 14 }, { 17, 17, 17, 17, 17, 10, 4 },
        { 17, 17, 17, 21, 21, 21, 10 }, { 17, 17, 10, 4, 10, 17, 17 },
        { 17, 17, 10, 4, 4, 4, 4 },     { 31, 1, 2, 4, 8, 16, 31 },
        { 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 },
        { 14, 17, 1, 2, 4, 8, 31 },     { 30, 1, 1, 14, 1, 1, 30 },
        { 2, 6, 10, 18, 31, 2, 2 },     { 31, 16, 16, 30, 1, 1, 30 },
        { 14, 16, 16, 30, 17, 17, 14 }, { 31, 1, 2, 4, 8, 8, 8 },
        { 14, 17, 17, 14, 17, 17, 14 }, { 14, 17, 17, 15, 1, 1, 14 },
        { 0, 0, 0, 0, 0, 0, 0 },        { 0, 0, 0, 31, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 4 },        { 4, 4, 4, 4, 4, 0, 4 },
        { 14, 17, 1, 2, 4, 0, 4 } };

static TS_Status
put (ts_sbuf_t *b, const char *s, size_t n, TS_Error *error)
{
  if (n > ART_LIMIT - b->len)
    {
      ts_error_set (error, TS_ERR_LIMIT, 0, 0,
                    "termart: output exceeds 1 MiB");
      return TS_ERR_LIMIT;
    }
  return ts_sbuf_put (b, s, n) == 0 ? TS_OK : ts_std_oom (error);
}

/* All paths after buffer initialization converge on cleanup. */
#define PUT(s, n)                                                             \
  do                                                                          \
    {                                                                         \
      st = put (&b, (s), (n), error);                                         \
      if (st != TS_OK)                                                        \
        goto done;                                                            \
    }                                                                         \
  while (0)

static TS_Status
text_arg (const TS_Value *v, const char **s, TS_Error *error)
{
  size_t i, n;
  TS_Status st = ts_std_str (v, s, error, "termart");
  if (st != TS_OK)
    return st;
  n = strlen (*s);
  if (n > ART_LIMIT)
    {
      ts_error_set (error, TS_ERR_LIMIT, 0, 0, "termart: input exceeds 1 MiB");
      return TS_ERR_LIMIT;
    }
  st = ts_utf8_validate (*s, n, error);
  if (st != TS_OK)
    return st;
  for (i = 0; i < n; i++)
    if (((unsigned char)(*s)[i] < 32 && (*s)[i] != '\n') || (*s)[i] == 127
        || ((unsigned char)(*s)[i] == 0xc2 && i + 1 < n
            && (unsigned char)(*s)[i + 1] >= 0x80
            && (unsigned char)(*s)[i + 1] <= 0x9f))
      return ts_std_inval (error,
                           "termart: control characters are not allowed");
  return TS_OK;
}

static TS_Status
art_banner (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
            TS_Value *ret, TS_Error *error)
{
  const char *s, *ink = "#", *p, *end, *g;
  size_t i, row, col, n;
  int first = 1;
  TS_Status st;
  ts_sbuf_t b;
  (void)ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "termart.banner") != TS_OK)
    return TS_ERR_INVAL;
  st = text_arg (&argv[0], &s, error);
  if (st != TS_OK)
    return st;
  if (argc == 2
      && ts_std_str (&argv[1], &ink, error, "termart.banner") != TS_OK)
    return TS_ERR_INVAL;
  /* One printable ASCII cell keeps the bitmap's cell geometry exact. */
  if (strlen (ink) != 1 || (unsigned char)ink[0] < 33
      || (unsigned char)ink[0] > 126)
    return ts_std_inval (
        error, "termart.banner: ink must be one visible ASCII character");
  for (i = 0; s[i]; i++)
    {
      unsigned char c = (unsigned char)s[i];
      if (c >= 'a' && c <= 'z')
        c -= 'a' - 'A';
      if (c != '\n' && !strchr (alphabet, c))
        return ts_std_inval (error, "termart.banner: unsupported character "
                                    "(use A-Z, 0-9, space, - . ! ?)");
    }
  ts_sbuf_init (&b);
  for (p = s; *p; p = *end ? end + 1 : end)
    {
      end = strchr (p, '\n');
      if (!end)
        end = p + strlen (p);
      n = (size_t)(end - p);
      if (n > (ART_WIDTH + 1) / 6)
        {
          st = ts_std_inval (error,
                             "termart.banner: line exceeds 4096 columns");
          goto done;
        }
      for (row = 0; row < 7; row++)
        {
          if (!first)
            PUT ("\n", 1);
          first = 0;
          for (i = 0; i < n; i++)
            {
              unsigned char c = (unsigned char)p[i];
              if (c >= 'a' && c <= 'z')
                c -= 'a' - 'A';
              g = strchr (alphabet, c);
              if (i)
                PUT (" ", 1);
              for (col = 0; col < 5; col++)
                PUT ((glyphs[g - alphabet][row] & (16u >> col)) ? ink : " ",
                     1);
            }
        }
    }
  st = ts_std_ret_sbuf (ret, &b, error);
done:
  ts_sbuf_free (&b);
  return st;
}

/* frame and center share line splitting, display width and padding rules. */
static TS_Status
art_layout (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
            TS_Value *ret, TS_Error *error)
{
  const char *s, *p, *end;
  int64_t option = 1;
  int frame = ud != NULL;
  size_t width, cells, left, right, i, n;
  TS_Status st;
  ts_sbuf_t b;
  if (ts_std_argc (vm, argc, frame ? 1 : 2, 2, error, "termart.layout")
      != TS_OK)
    return TS_ERR_INVAL;
  st = text_arg (&argv[0], &s, error);
  if (st != TS_OK)
    return st;
  if (argc == 2
      && ts_std_int (&argv[1], &option, error, "termart.layout") != TS_OK)
    return TS_ERR_INVAL;
  if (option < 0 || option > (frame ? 64 : ART_WIDTH))
    return ts_std_inval (error,
                         "termart: padding must be 0..64 or width 0..4096");
  width = ts_text_width (s, strlen (s));
  if (!frame && width < (size_t)option)
    width = (size_t)option;
  if (width + (frame ? 2 * (size_t)option + 2 : 0) > ART_WIDTH)
    return ts_std_inval (error, "termart: layout exceeds 4096 columns");
  ts_sbuf_init (&b);
  if (frame)
    {
      PUT ("+", 1);
      for (i = 0; i < width + 2 * (size_t)option; i++)
        PUT ("-", 1);
      PUT ("+\n", 2);
    }
  p = s;
  do
    {
      end = strchr (p, '\n');
      if (!end)
        end = p + strlen (p);
      n = (size_t)(end - p);
      cells = ts_text_width (p, n);
      left = frame ? (size_t)option : (width - cells) / 2;
      right = frame ? width - cells + (size_t)option : width - cells - left;
      if (frame)
        PUT ("|", 1);
      for (i = 0; i < left; i++)
        PUT (" ", 1);
      PUT (p, n);
      for (i = 0; i < right; i++)
        PUT (" ", 1);
      if (frame)
        PUT ("|", 1);
      if (*end || frame)
        PUT ("\n", 1);
      p = *end ? end + 1 : end;
    }
  while (*end && *p);
  if (frame)
    {
      PUT ("+", 1);
      for (i = 0; i < width + 2 * (size_t)option; i++)
        PUT ("-", 1);
      PUT ("+", 1);
    }
  st = ts_std_ret_sbuf (ret, &b, error);
done:
  ts_sbuf_free (&b);
  return st;
}

static TS_Status
art_color (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  const char *s;
  int64_t rgb[3];
  size_t i;
  TS_Status st;
  ts_sbuf_t b;
  (void)ud;
  if (ts_std_argc (vm, argc, 4, 4, error, "termart.color") != TS_OK)
    return TS_ERR_INVAL;
  st = text_arg (&argv[0], &s, error);
  if (st != TS_OK)
    return st;
  for (i = 0; i < 3; i++)
    {
      if (ts_std_int (&argv[i + 1], &rgb[i], error, "termart.color") != TS_OK)
        return TS_ERR_INVAL;
      if (rgb[i] < 0 || rgb[i] > 255)
        return ts_std_inval (error,
                             "termart.color: RGB components must be 0..255");
    }
  ts_sbuf_init (&b);
  if (ts_sbuf_printf (&b, "\033[38;2;%d;%d;%dm", (int)rgb[0], (int)rgb[1],
                      (int)rgb[2])
      != 0)
    {
      st = ts_std_oom (error);
      goto done;
    }
  PUT (s, strlen (s));
  PUT ("\033[0m", 4);
  st = ts_std_ret_sbuf (ret, &b, error);
done:
  ts_sbuf_free (&b);
  return st;
}

static const int frame_mode = 1;
static const TS_FuncDef art_funcs[] = {
  { "banner", art_banner, NULL },
  { "frame", art_layout, (void *)&frame_mode },
  { "center", art_layout, NULL },
  { "color", art_color, NULL },
};

const TS_Module ts_std_termart_module
    = TS_MODULE_INIT ("std.termart", art_funcs);
