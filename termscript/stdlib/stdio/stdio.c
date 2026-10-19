/* Borrows process streams; never closes or restores their descriptors. */
#include "stdio/stdio.h"
#include "common/ts_std_common.h"
#include <errno.h>
#include <string.h>

static int
hex_digit (unsigned char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/* The language's strings are NUL-terminated text. Hex is an explicit binary
   boundary; validate the entire input before producing any output. */
static TS_Status
write_hex (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r,
           TS_Error *e)
{
  const char *hex;
  unsigned char bytes[4096];
  size_t len;
  (void)u;
  if (ts_std_argc (vm, n, 1, 1, e, "write_hex")
      || ts_std_str (a, &hex, e, "write_hex"))
    return TS_ERR_INVAL;
  len = strlen (hex);
  if (len > 32u * 1024 * 1024)
    {
      ts_error_set (e, TS_ERR_LIMIT, 0, 0, "write_hex exceeds 16 MiB");
      return TS_ERR_LIMIT;
    }
  if (len % 2)
    return ts_std_inval (e, "write_hex requires pairs of hexadecimal digits");
  for (size_t i = 0; i < len; i++)
    if (hex_digit ((unsigned char)hex[i]) < 0)
      return ts_std_inval (e, "write_hex requires hexadecimal digits");
  for (size_t pos = 0; pos < len;)
    {
      size_t count = (len - pos) / 2;
      if (count > sizeof bytes)
        count = sizeof bytes;
      for (size_t i = 0; i < count; i++, pos += 2)
        bytes[i] = (unsigned char)((hex_digit ((unsigned char)hex[pos]) << 4)
                                   | hex_digit ((unsigned char)hex[pos + 1]));
      if (fwrite (bytes, 1, count, stdout) != count)
        return ts_std_system (e, "stdio write_hex");
    }
  ts_std_ret_int (r, (int64_t)(len / 2));
  return TS_OK;
}

static TS_Status
write_text (TS_VM *vm, void *stream, const TS_Value *a, size_t n, TS_Value *r,
            TS_Error *e)
{
  (void)r;
  const char *text;
  if (ts_std_argc (vm, n, 1, 1, e, "write")
      || ts_std_str (a, &text, e, "write"))
    return TS_ERR_INVAL;
  if (fputs (text, stream ? stderr : stdout) == EOF)
    return ts_std_system (e, "stdio write");
  return TS_OK;
}
static TS_Status
flush (TS_VM *vm, void *stream, const TS_Value *a, size_t n, TS_Value *r,
       TS_Error *e)
{
  (void)a;
  (void)r;
  if (ts_std_argc (vm, n, 0, 0, e, "flush"))
    return TS_ERR_INVAL;
  return fflush (stream ? stderr : stdout) == 0
             ? TS_OK
             : ts_std_system (e, "stdio flush");
}
static TS_Status
read_line (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r,
           TS_Error *e)
{
  (void)u;
  (void)a;
  if (ts_std_argc (vm, n, 0, 0, e, "read_line"))
    return TS_ERR_INVAL;
  ts_sbuf_t line;
  ts_sbuf_init (&line);
  for (;;)
    {
      errno = 0;
      int ch = fgetc (stdin);
      if (ch == EOF)
        {
          if (ferror (stdin) && errno == EINTR)
            {
              clearerr (stdin);
              continue;
            }
          if (ferror (stdin))
            {
              int saved = errno;
              ts_sbuf_free (&line);
              errno = saved;
              if (saved == EAGAIN || saved == EWOULDBLOCK)
                {
                  clearerr (stdin);
                  ts_error_set (e, TS_ERR_WOULD_BLOCK, saved, 0,
                                "stdin would block");
                  return TS_ERR_WOULD_BLOCK;
                }
              return ts_std_system (e, "stdio read");
            }
          if (!line.len)
            {
              ts_sbuf_free (&line);
              ts_std_ret_nil (r);
              return TS_OK;
            }
          break;
        }
      if (ch == '\n')
        break;
      if (ch == 0 || line.len == 1048576)
        {
          ts_sbuf_free (&line);
          ts_error_set (e, ch ? TS_ERR_LIMIT : TS_ERR_UNSUPPORTED, 0, 0,
                        ch ? "stdin line exceeds 1 MiB" : "NUL in stdin text");
          return ch ? TS_ERR_LIMIT : TS_ERR_UNSUPPORTED;
        }
      if (ts_sbuf_ch (&line, (char)ch))
        {
          ts_sbuf_free (&line);
          return ts_std_oom (e);
        }
    }
  return ts_std_ret_sbuf (r, &line, e);
}
static const TS_FuncDef functions[] = {
  { "write", write_text, NULL },       { "write_hex", write_hex, NULL },
  { "eprint", write_text, (void *)1 }, { "flush", flush, NULL },
  { "flush_error", flush, (void *)1 }, { "read_line", read_line, NULL }
};
const TS_Module ts_std_stdio_module = TS_MODULE_INIT ("std.stdio", functions);
