#include "termscript/stdlib/ts_stdlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TS_Error error;
#define CHECK(x)                                                              \
  do                                                                          \
    {                                                                         \
      if (!(x))                                                               \
        {                                                                     \
          fprintf (stderr, "%d: %s: %s\n", __LINE__, #x, error.message);      \
          exit (1);                                                           \
        }                                                                     \
    }                                                                         \
  while (0)
static TS_Value
integer (int64_t n)
{
  TS_Value v = { 0 };
  v.type = TS_INT;
  v.as.integer = n;
  return v;
}
static TS_Value
string (const char *s)
{
  TS_Value v = { 0 };
  v.type = TS_STRING;
  v.as.string = (char *)s;
  return v;
}
static TS_Status
call (const char *name, const TS_Value *args, size_t count, TS_Value *ret)
{
  const TS_FuncDef *f = ts_module_find_func (&ts_std_crdt_module, name);
  CHECK (f);
  ts_value_free (ret);
  ts_error_clear (&error);
  return f->func (NULL, f->userdata, args, count, ret, &error);
}
static void
text_is (TS_Value doc, const char *text)
{
  TS_Value r = { 0 };
  CHECK (call ("text", &doc, 1, &r) == TS_OK);
  CHECK (r.type == TS_STRING && !strcmp (r.as.string, text));
  ts_value_free (&r);
}
int
main (void)
{
  CHECK (ts_module_validate (&ts_std_crdt_module, &error) == TS_OK);
  TS_Value a = { 0 }, b = { 0 }, r = { 0 }, delta = { 0 }, deletion = { 0 },
           snap = { 0 }, id = { 0 };
  TS_Value args[3] = { integer (1), integer (16), { 0 } };
  CHECK (call ("new", args, 2, &a) == TS_OK);
  args[0] = a;
  args[1] = integer (0);
  args[2] = string ("ab");
  CHECK (call ("insert", args, 3, &delta) == TS_OK);
  CHECK (delta.type == TS_STRING && !strncmp (delta.as.string, "rga1:", 5));
  args[1] = integer (2);
  CHECK (call ("fork", args, 2, &b) == TS_OK);
  args[1] = integer (1);
  CHECK (call ("fork", args, 2, &r) == TS_ERR_INVAL);
  args[1] = integer (0);
  CHECK (call ("id_at", args, 2, &id) == TS_OK);
  args[1] = integer (1);
  args[2] = string ("X");
  CHECK (call ("insert", args, 3, &delta) == TS_OK);
  args[0] = b;
  args[2] = string ("Y");
  CHECK (call ("insert", args, 3, &r) == TS_OK);
  args[0] = a;
  args[1] = b;
  CHECK (call ("merge", args, 2, &r) == TS_OK);
  args[0] = b;
  args[1] = a;
  CHECK (call ("merge", args, 2, &r) == TS_OK);
  text_is (a, "aYXb");
  text_is (b, "aYXb");
  args[0] = a;
  args[1] = id;
  CHECK (call ("index_of", args, 2, &r) == TS_OK && r.type == TS_INT
         && r.as.integer == 0);
  args[1] = integer (0);
  args[2] = integer (1);
  CHECK (call ("delete", args, 3, &deletion) == TS_OK);
  args[0] = b;
  args[1] = deletion;
  CHECK (call ("apply", args, 2, &r) == TS_OK);
  CHECK (call ("apply", args, 2, &r) == TS_OK);
  text_is (b, "YXb");
  args[1] = id;
  CHECK (call ("index_of", args, 2, &r) == TS_OK && r.type == TS_NIL);
  CHECK (call ("snapshot", &a, 1, &snap) == TS_OK);
  TS_Value restored = { 0 };
  args[0] = integer (3);
  args[1] = snap;
  CHECK (call ("restore", args, 2, &restored) == TS_OK);
  text_is (restored, "YXb");
  const char *bad[] = { "", "rga2:00", "rga1:0", "rga1:gg", "rga1:00" };
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
    {
      args[0] = b;
      args[1] = string (bad[i]);
      CHECK (call ("apply", args, 2, &r) == TS_ERR_PARSE);
      text_is (b, "YXb");
    }
  args[0] = b;
  args[1] = string ("rga1:gg");
  CHECK (call ("apply", args, 2, &r) == TS_ERR_PARSE && error.offset == 5);
  args[1] = string ("invalid");
  CHECK (call ("index_of", args, 2, &r) == TS_ERR_INVAL);
  args[1] = integer (-1);
  args[2] = string ("bad");
  CHECK (call ("insert", args, 3, &r) == TS_ERR_INVAL);
  args[1] = integer (0);
  args[2] = string ("\xed\xa0\x80");
  CHECK (call ("insert", args, 3, &r) == TS_ERR_INVAL);
  text_is (b, "YXb");
  TS_Value alias = { 0 };
  CHECK (ts_value_copy (&alias, &a) == TS_OK);
  ts_value_free (&a);
  text_is (alias, "YXb");
  CHECK (call ("close", &alias, 1, &r) == TS_OK);
  CHECK (call ("close", &alias, 1, &r) == TS_OK);
  CHECK (call ("text", &alias, 1, &r) == TS_ERR_INVAL);
  TS_Value small = { 0 };
  args[0] = integer (5);
  args[1] = integer (1);
  CHECK (call ("new", args, 2, &small) == TS_OK);
  args[0] = small;
  args[1] = integer (0);
  args[2] = string ("xy");
  CHECK (call ("insert", args, 3, &r) == TS_ERR_LIMIT);
  text_is (small, "");
  args[1] = b;
  CHECK (call ("merge", args, 2, &r) == TS_ERR_LIMIT);
  text_is (small, "");
  args[0] = integer (2);
  args[1] = integer (16);
  TS_Value clash = { 0 };
  CHECK (call ("new", args, 2, &clash) == TS_OK);
  args[0] = clash;
  args[1] = integer (0);
  args[2] = string ("xyz");
  CHECK (call ("insert", args, 3, &r) == TS_OK);
  args[0] = b;
  args[1] = clash;
  CHECK (call ("merge", args, 2, &r) == TS_ERR_INVAL);
  text_is (b, "YXb");
  const TS_FuncDef *f = ts_module_find_func (&ts_std_crdt_module, "apply");
  args[0] = b;
  args[1] = string ("invalid");
  ts_value_free (&r);
  CHECK (f->func (NULL, f->userdata, args, 2, &r, NULL) == TS_ERR_PARSE);
  TS_CRDT *binary_doc = NULL;
  CHECK (ts_crdt_create (8, 8, &binary_doc) == TS_CRDT_OK);
  CHECK (ts_crdt_insert (binary_doc, 0, "\0", 1, NULL) == TS_CRDT_OK);
  unsigned char *bytes = NULL;
  size_t size;
  CHECK (ts_crdt_encode (binary_doc, &bytes, &size) == TS_CRDT_OK);
  char *encoded = malloc (6 + size * 2);
  CHECK (encoded);
  memcpy (encoded, "rga1:", 5);
  for (size_t i = 0; i < size; i++)
    snprintf (encoded + 5 + i * 2, 3, "%02x", bytes[i]);
  free (bytes);
  ts_crdt_free (binary_doc);
  args[0] = integer (9);
  args[1] = string (encoded);
  TS_Value nul_doc = { 0 };
  CHECK (call ("restore", args, 2, &nul_doc) == TS_OK);
  free (encoded);
  CHECK (call ("text", &nul_doc, 1, &r) == TS_ERR_UNSUPPORTED);
  CHECK (call ("len", &nul_doc, 1, &r) == TS_OK && r.as.integer == 1);
  ts_value_free (&nul_doc);
  ts_value_free (&alias);
  ts_value_free (&b);
  ts_value_free (&r);
  ts_value_free (&delta);
  ts_value_free (&deletion);
  ts_value_free (&snap);
  ts_value_free (&id);
  ts_value_free (&restored);
  ts_value_free (&small);
  ts_value_free (&clash);
  puts ("CRDT module OK");
  return 0;
}
