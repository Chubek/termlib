/* Streaming framing for std.key's complete key/mouse/paste event decoder. */
#include "input/input.h"
#include "common/text.h"
#include "key/key.h"
#include <stdlib.h>
#include <string.h>

enum
{
  INPUT_LIMIT = 1024 * 1024
};
typedef struct
{
  ts_sbuf_t pending;
  bool closed;
} input_t;
static const char tag;

static void
destroy (void *ptr)
{
  input_t *input = ptr;
  ts_sbuf_free (&input->pending);
  free (input);
}

static TS_Status
create (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r,
        TS_Error *e)
{
  (void)u;
  (void)a;
  if (ts_std_argc (vm, n, 0, 0, e, "input.new"))
    return TS_ERR_INVAL;
  input_t *input = calloc (1, sizeof *input);
  if (!input)
    return ts_std_oom (e);
  TS_Status st
      = ts_value_make_handle (r, input, destroy, "input decoder", &tag);
  if (st)
    {
      destroy (input);
      return ts_std_oom (e);
    }
  return TS_OK;
}

/* Zero means incomplete. No consumption on malformed/truncated UTF-8. Escape
 * prefixes are flushed as unknown events when final=true, not silently
 * dropped.
 */
static TS_Status
frame (const char *s, size_t n, bool final, size_t *length, TS_Error *e)
{
  *length = 0;
  if (!n)
    return TS_OK;
  if ((unsigned char)s[0] == 27)
    {
      static const char begin[] = "\033[200~", end[] = "\033[201~";
      if (n <= 6 && !memcmp (s, begin, n) && !final)
        return TS_OK;
      if (n >= 6 && !memcmp (s, begin, 6))
        {
          const char *stop = strstr (s + 6, end);
          if (!stop)
            {
              if (final)
                {
                  ts_error_set (e, TS_ERR_PARSE, 0, n,
                                "unterminated bracketed paste");
                  return TS_ERR_PARSE;
                }
              return TS_OK;
            }
          *length = (size_t)(stop - s) + 6;
          return TS_OK;
        }
      if (n == 1)
        {
          if (final)
            *length = 1;
          return TS_OK;
        }
      if (s[1] == '[' || s[1] == 'O')
        {
          for (size_t i = 2; i < n; i++)
            if ((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7e)
              {
                *length = i + 1;
                return TS_OK;
              }
          if (final)
            *length = n;
          return TS_OK;
        }
      /* No Alt-key convention is guessed: emit Escape then the following key.
       */
      *length = 1;
      return TS_OK;
    }
  unsigned char first = (unsigned char)s[0];
  size_t need = first < 0x80                     ? 1
                : first >= 0xc2 && first <= 0xdf ? 2
                : first >= 0xe0 && first <= 0xef ? 3
                : first >= 0xf0 && first <= 0xf4 ? 4
                                                 : 1;
  /* Validate partial prefixes too, so invalid continuation bytes never stall.
   */
  for (size_t i = 1; i < need && i < n; i++)
    if (((unsigned char)s[i] & 0xc0) != 0x80
        || (i == 1
            && ((first == 0xe0 && (unsigned char)s[i] < 0xa0)
                || (first == 0xed && (unsigned char)s[i] >= 0xa0)
                || (first == 0xf0 && (unsigned char)s[i] < 0x90)
                || (first == 0xf4 && (unsigned char)s[i] >= 0x90))))
      {
        ts_error_set (e, TS_ERR_PARSE, 0, i, "invalid input UTF-8 prefix");
        return TS_ERR_PARSE;
      }
  if (n < need && !final)
    return TS_OK;
  TS_Status st = ts_utf8_validate (s, n < need ? n : need, e);
  if (!st)
    *length = need;
  return st;
}

static TS_Status
operation (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r,
           TS_Error *e)
{
  intptr_t op = (intptr_t)ud;
  if (ts_std_argc (vm, n, op == 2 ? 2 : 1, op == 2 || op == 3 ? 2 : 1, e,
                   "input"))
    return TS_ERR_INVAL;
  input_t *input = ts_std_handle (a, &tag, e, "input");
  if (!input)
    return TS_ERR_INVAL;
  if (op == 0)
    {
      ts_sbuf_free (&input->pending);
      input->closed = true;
      return TS_OK;
    }
  if (input->closed)
    return ts_std_inval (e, "input decoder is closed");
  if (op == 1)
    {
      ts_sbuf_clear (&input->pending);
      return TS_OK;
    }
  if (op == 4)
    {
      ts_std_ret_int (r, (int64_t)input->pending.len);
      return TS_OK;
    }
  if (op == 2)
    {
      const char *text;
      if (ts_std_str (a + 1, &text, e, "input.feed"))
        return TS_ERR_INVAL;
      size_t len = strlen (text);
      if (len > INPUT_LIMIT - input->pending.len)
        {
          ts_error_set (e, TS_ERR_LIMIT, 0, 0,
                        "input pending text exceeds 1 MiB");
          return TS_ERR_LIMIT;
        }
      if (ts_sbuf_put (&input->pending, text, len))
        return ts_std_oom (e);
      ts_std_ret_int (r, (int64_t)len);
      return TS_OK;
    }
  if (n == 2 && a[1].type != TS_BOOL)
    return ts_std_inval (e, "input.next: final must be boolean");
  size_t length;
  TS_Status st = frame (input->pending.data, input->pending.len,
                        n == 2 && a[1].as.boolean, &length, e);
  if (st || !length)
    return st;
  char *text = malloc (length + 1);
  if (!text)
    return ts_std_oom (e);
  memcpy (text, input->pending.data, length);
  text[length] = 0;
  /* A paste's contents must be valid text as well. */
  st = ts_utf8_validate (text, length, e);
  if (!st)
    {
      const TS_FuncDef *event
          = ts_module_find_func (&ts_std_key_module, "event");
      TS_Value arg = { .type = TS_STRING, .as.string = text };
      st = event->func (vm, event->userdata, &arg, 1, r, e);
    }
  if (!st && r->type == TS_NIL)
    {
      ts_dom_t *d = ts_dom_new_obj ();
      if (!d || ts_dom_obj_set (d, "type", ts_dom_new_str ("unknown"))
          || ts_dom_obj_set (d, "sequence", ts_dom_new_str (text)))
        {
          ts_dom_free (d);
          st = ts_std_oom (e);
        }
      else
        st = ts_dom_wrap (r, d, e);
    }
  free (text);
  if (!st)
    {
      input->pending.len -= length;
      memmove (input->pending.data, input->pending.data + length,
               input->pending.len + 1);
    }
  return st;
}

static const TS_FuncDef functions[] = {
  { "new", create, NULL },           { "close", operation, (void *)0 },
  { "reset", operation, (void *)1 }, { "feed", operation, (void *)2 },
  { "next", operation, (void *)3 },  { "pending", operation, (void *)4 }
};
const TS_Module ts_std_input_module = TS_MODULE_INIT ("std.input", functions);
