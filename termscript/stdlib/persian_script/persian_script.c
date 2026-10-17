#include "persian_script/persian_script.h"
#include "common/text.h"
#include <string.h>

static TS_Status
normalize (TS_VM *vm, void *fold, const TS_Value *a, size_t n, TS_Value *r,
           TS_Error *e)
{
  const char *text;
  if (ts_std_argc (vm, n, 1, 1, e, "normalize")
      || ts_std_str (a, &text, e, "normalize"))
    return TS_ERR_INVAL;
  size_t size = strlen (text), at = 0;
  TS_Status st = ts_utf8_validate (text, size, e);
  if (st != TS_OK)
    return st;
  ts_sbuf_t out;
  ts_sbuf_init (&out);
  while (at < size)
    {
      int32_t cp;
      at += (size_t)ts_utf8_decode (text + at, size - at, &cp);
      if (cp == 0x064a || cp == 0x0649)
        cp = 0x06cc;
      if (cp == 0x0643)
        cp = 0x06a9;
      if (fold
          && (cp == 0x200c || cp == 0x0640 || (cp >= 0x064b && cp <= 0x065f)))
        continue;
      if (fold && cp >= 'A' && cp <= 'Z')
        cp += 'a' - 'A';
      unsigned char bytes[4];
      size_t length = (size_t)utf8proc_encode_char (cp, bytes);
      if (ts_sbuf_put (&out, (const char *)bytes, length))
        {
          ts_sbuf_free (&out);
          return ts_std_oom (e);
        }
    }
  return ts_std_ret_sbuf (r, &out, e);
}
static const TS_FuncDef functions[]
    = { { "normalize", normalize, NULL },
        { "search_fold", normalize, (void *)1 } };
const TS_Module ts_std_persian_script_module
    = TS_MODULE_INIT ("std.persian_script", functions);
