#include "termscript/stdlib/ts_stdlib.h"
#include "termscript/stdlib/common/ts_std_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TS_Error error;
#define CHECK(x) do { if (!(x)) { fprintf (stderr, "%d: %s (%s)\n", __LINE__, #x, error.message); exit (1); } } while (0)
static TS_Value string (const char *s)
{ TS_Value v = { .type = TS_STRING, .as.string = (char *)s }; return v; }
static TS_Status call (const TS_Module *m, const char *name, TS_Value *r, size_t n, TS_Value *a)
{
  ts_value_free (r);
  ts_error_clear (&error);
  const TS_FuncDef *f = ts_module_find_func (m, name);
  CHECK (f);
  return f->func (NULL, f->userdata, a, n, r, &error);
}
static TS_Status op (const char *name, TS_Value *r, size_t n, TS_Value *a)
{ return call (&ts_std_input_module, name, r, n, a); }
static void field (TS_Value *event, const char *key, const char *expected)
{
  ts_dom_t *d = ts_dom_unwrap (event, &error, "event");
  const ts_dom_t *item = ts_dom_key (d, key);
  TS_Value value = {0};
  CHECK (item && ts_dom_to_value (item, &value, &error) == TS_OK);
  CHECK (value.type == TS_STRING && !strcmp (value.as.string, expected));
  ts_value_free (&value);
}
int main (void)
{
  TS_Value decoder = {0}, r = {0};
  CHECK (op ("new", &decoder, 0, NULL) == TS_OK);
  const char *events[] = { "\033[A", "\033[1;5D", "\033OP", "\033[<0;3;4M",
                           "\033[200~hello\033[201~", "\xd9\x81", "\xf0\x9f\x98\x80" };
  const char *types[] = { "key", "key", "key", "mouse", "paste", "key", "key" };
  for (size_t k = 0; k < sizeof events / sizeof *events; k++)
    for (size_t split = 1; split < strlen (events[k]); split++)
      {
        CHECK (op ("reset", &r, 1, &decoder) == TS_OK);
        char prefix[64];
        memcpy (prefix, events[k], split); prefix[split] = 0;
        TS_Value feed[] = { decoder, string (prefix) };
        CHECK (op ("feed", &r, 2, feed) == TS_OK);
        CHECK (op ("next", &r, 1, &decoder) == TS_OK && r.type == TS_NIL);
        feed[1] = string (events[k] + split);
        CHECK (op ("feed", &r, 2, feed) == TS_OK);
        CHECK (op ("next", &r, 1, &decoder) == TS_OK);
        field (&r, "type", types[k]);
        CHECK (op ("pending", &r, 1, &decoder) == TS_OK && r.as.integer == 0);
      }
  TS_Value feed[] = { decoder, string ("ab\033[AZ") };
  CHECK (op ("feed", &r, 2, feed) == TS_OK);
  CHECK (op ("next", &r, 1, &decoder) == TS_OK); field (&r, "name", "a");
  TS_Value retained = {0}; CHECK (ts_value_copy (&retained, &r) == TS_OK);
  CHECK (op ("next", &r, 1, &decoder) == TS_OK); field (&r, "name", "b");
  CHECK (op ("next", &r, 1, &decoder) == TS_OK); field (&r, "name", "up");
  CHECK (op ("next", &r, 1, &decoder) == TS_OK); field (&r, "name", "Z");
  field (&retained, "name", "a"); ts_value_free (&retained);
  feed[1] = string ("\033");
  CHECK (op ("feed", &r, 2, feed) == TS_OK);
  CHECK (op ("next", &r, 1, &decoder) == TS_OK && r.type == TS_NIL);
  TS_Value final[] = { decoder, { .type = TS_BOOL, .as.boolean = true } };
  CHECK (op ("next", &r, 2, final) == TS_OK); field (&r, "name", "escape");
  final[1] = string ("not boolean");
  CHECK (op ("next", &r, 2, final) == TS_ERR_INVAL);
  const char *bad[] = { "\xff", "\xe0\x80", "\xe2x", "\xed\xa0", "\xf4\x90" };
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
    {
      CHECK (op ("reset", &r, 1, &decoder) == TS_OK);
      feed[1] = string (bad[i]); CHECK (op ("feed", &r, 2, feed) == TS_OK);
      CHECK (op ("next", &r, 1, &decoder) == TS_ERR_PARSE);
      CHECK (op ("pending", &r, 1, &decoder) == TS_OK && r.as.integer == (int64_t)strlen (bad[i]));
    }
  CHECK (op ("reset", &r, 1, &decoder) == TS_OK);
  feed[1] = string ("\xe2"); CHECK (op ("feed", &r, 2, feed) == TS_OK);
  CHECK (op ("next", &r, 1, &decoder) == TS_OK && r.type == TS_NIL);
  final[1] = (TS_Value){ .type = TS_BOOL, .as.boolean = true };
  CHECK (op ("next", &r, 2, final) == TS_ERR_PARSE);
  CHECK (op ("reset", &r, 1, &decoder) == TS_OK);
  feed[1] = string ("\033[200~unfinished"); CHECK (op ("feed", &r, 2, feed) == TS_OK);
  CHECK (op ("next", &r, 2, final) == TS_ERR_PARSE);
  CHECK (op ("reset", &r, 1, &decoder) == TS_OK);
  feed[1] = string ("\033[99~"); CHECK (op ("feed", &r, 2, feed) == TS_OK);
  CHECK (op ("next", &r, 1, &decoder) == TS_OK); field (&r, "type", "unknown");
  const char *bad_mouse[] = { "\033[<99999999999999999999999999;1;1M",
                             "\033[<0;999999999999999999999999;1M",
                             "\033[<0;1;999999999999999999999999m",
                             "\033[<256;1;1M", "\033[<0;65536;1M", "\033[<0;0;1M",
                             "\033[<0;1;1Mextra", "\033[<+0;1;1M", "\033[<0;1;1" };
  for (size_t i = 0; i < sizeof bad_mouse / sizeof *bad_mouse; i++)
    {
      TS_Value sequence = string (bad_mouse[i]);
      CHECK (call (&ts_std_key_module, "event", &r, 1, &sequence) == TS_OK && r.type == TS_NIL);
    }
  char *large = malloc (1048577); CHECK (large);
  memset (large, 'x', 1048576); large[1048576] = 0;
  feed[1] = string (large); CHECK (op ("feed", &r, 2, feed) == TS_OK);
  feed[1] = string ("x"); CHECK (op ("feed", &r, 2, feed) == TS_ERR_LIMIT);
  CHECK (op ("pending", &r, 1, &decoder) == TS_OK && r.as.integer == 1048576);
  free (large);
  TS_Value alias = {0}; CHECK (ts_value_copy (&alias, &decoder) == TS_OK);
  CHECK (op ("close", &r, 1, &decoder) == TS_OK);
  CHECK (op ("close", &r, 1, &alias) == TS_OK);
  CHECK (op ("next", &r, 1, &alias) == TS_ERR_INVAL);
  ts_value_free (&alias); ts_value_free (&decoder);
  /* No local font installation is needed to test boundary failures. */
  TS_Value invalid[] = { {0}, string ("abc") };
  CHECK (call (&ts_std_font_shape_module, "shape", &r, 2, invalid) == TS_ERR_INVAL);
  invalid[0] = string ("/nonexistent/termscript-test-font");
  CHECK (call (&ts_std_font_shape_module, "open", &r, 1, invalid) == TS_ERR_SYSTEM);
  TS_Value font = {0}, file = string (TS_TEST_FONT);
  CHECK (call (&ts_std_font_shape_module, "open", &font, 1, &file) == TS_OK);
  TS_Value shape[] = { font, string ("abc"), string ("ltr"), string ("en") };
  CHECK (call (&ts_std_font_shape_module, "shape", &r, 4, shape) == TS_OK);
  ts_std_list_t *glyphs = ts_std_list_unwrap (&r, &error, "glyphs");
  CHECK (glyphs && ts_std_list_len (glyphs) == 3);
  CHECK (call (&ts_std_font_module, "has", &r, 2, shape) == TS_OK && r.as.boolean);
  shape[1] = string ("\xff");
  CHECK (call (&ts_std_font_shape_module, "shape", &r, 4, shape) == TS_ERR_PARSE);
  shape[1] = string ("abc"); shape[2] = string ("invalid");
  CHECK (call (&ts_std_font_shape_module, "shape", &r, 4, shape) == TS_ERR_INVAL);
  ts_value_free (&font);
  ts_value_free (&r);
  return 0;
}
