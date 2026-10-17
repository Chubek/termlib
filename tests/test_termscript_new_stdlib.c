#define _POSIX_C_SOURCE 200809L
#include "termscript/stdlib/common/ts_std_common.h"
#include "termscript/stdlib/ts_stdlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x)                                                              \
  do                                                                          \
    {                                                                         \
      if (!(x))                                                               \
        {                                                                     \
          fprintf (stderr, "%d: %s (%s)\n", __LINE__, #x, error.message);     \
          exit (1);                                                           \
        }                                                                     \
    }                                                                         \
  while (0)
static TS_Error error;
static TS_Value
integer (int64_t i)
{
  TS_Value v = { 0 };
  v.type = TS_INT;
  v.as.integer = i;
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
call (TS_VM *vm, const TS_Module *m, const char *f, TS_Value *ret, size_t n,
      TS_Value *a)
{
  ts_value_free (ret);
  ts_error_clear (&error);
  const TS_FuncDef *fn = ts_module_find_func (m, f);
  CHECK (fn);
  return fn->func (vm, fn->userdata, a, n, ret, &error);
}
int
main (void)
{
  TS_VM *vm = ts_vm_create (&error);
  CHECK (vm);
  TS_Value memory = { 0 }, ret = { 0 };
  TS_Value a[] = { integer (8) };
  CHECK (call (vm, &ts_std_memory_module, "alloc", &memory, 1, a) == TS_OK);
  TS_Value bounds[] = { memory, integer (8) };
  CHECK (call (vm, &ts_std_memory_module, "get", &ret, 2, bounds)
         == TS_ERR_INVAL);
  TS_Value too_large = integer (16777217);
  CHECK (call (vm, &ts_std_memory_module, "alloc", &ret, 1, &too_large)
         == TS_ERR_LIMIT);
  CHECK (call (vm, &ts_std_memory_module, "close", &ret, 1, &memory) == TS_OK);
  CHECK (call (vm, &ts_std_memory_module, "close", &ret, 1, &memory) == TS_OK);
  CHECK (call (vm, &ts_std_memory_module, "len", &ret, 1, &memory)
         == TS_ERR_INVAL);
  ts_value_free (&memory);
  TS_Value sender = { 0 }, receiver = { 0 }, zero = integer (0);
  CHECK (call (vm, &ts_std_datagram_module, "bind", &receiver, 1, &zero)
         == TS_OK);
  CHECK (call (vm, &ts_std_datagram_module, "bind", &sender, 1, &zero)
         == TS_OK);
  CHECK (call (vm, &ts_std_datagram_module, "recv", &ret, 1, &receiver)
         == TS_ERR_WOULD_BLOCK);
  CHECK (call (vm, &ts_std_datagram_module, "port", &ret, 1, &receiver)
         == TS_OK);
  TS_Value send[] = { sender, string ("127.0.0.1"), ret, string ("hello") };
  TS_Value sent = { 0 };
  CHECK (call (vm, &ts_std_datagram_module, "send_to", &sent, 4, send)
         == TS_OK);
  CHECK (sent.as.integer == 5);
  CHECK (call (vm, &ts_std_datagram_module, "recv", &ret, 1, &receiver)
         == TS_OK);
  ts_std_list_t *packet = ts_std_list_unwrap (&ret, &error, "packet");
  CHECK (packet && ts_std_list_len (packet) == 3);
  CHECK (!strcmp (ts_std_list_get (packet, 0)->as.string, "hello"));
  CHECK (call (vm, &ts_std_datagram_module, "send_to", &sent, 4, send)
         == TS_OK);
  TS_Value small[] = { receiver, integer (2) };
  CHECK (call (vm, &ts_std_datagram_module, "recv", &ret, 2, small)
         == TS_ERR_LIMIT);
  CHECK (call (vm, &ts_std_datagram_module, "recv", &ret, 1, &receiver)
         == TS_ERR_WOULD_BLOCK);
  send[3] = string ("");
  CHECK (call (vm, &ts_std_datagram_module, "send_to", &sent, 4, send)
         == TS_OK);
  CHECK (call (vm, &ts_std_datagram_module, "recv", &ret, 1, &receiver)
         == TS_OK);
  packet = ts_std_list_unwrap (&ret, &error, "empty packet");
  CHECK (packet && !strcmp (ts_std_list_get (packet, 0)->as.string, ""));
  CHECK (call (vm, &ts_std_datagram_module, "close", &ret, 1, &receiver)
         == TS_OK);
  CHECK (call (vm, &ts_std_datagram_module, "close", &ret, 1, &receiver)
         == TS_OK);
  CHECK (call (vm, &ts_std_datagram_module, "recv", &ret, 1, &receiver)
         == TS_ERR_INVAL);
  ts_value_free (&sender);
  ts_value_free (&receiver);
  ts_value_free (&sent);
  ts_value_free (&ret);
  TS_Value invalid_utf8 = string ("\xff");
  CHECK (call (vm, &ts_std_persian_script_module, "normalize", &ret, 1,
               &invalid_utf8)
         == TS_ERR_PARSE);
  FILE *input = tmpfile ();
  CHECK (input && fputs ("line\n", input) >= 0 && fflush (input) == 0);
  rewind (input);
  int saved_stdin = dup (STDIN_FILENO);
  CHECK (saved_stdin >= 0 && dup2 (fileno (input), STDIN_FILENO) >= 0);
  clearerr (stdin);
  CHECK (call (vm, &ts_std_stdio_module, "read_line", &ret, 0, NULL) == TS_OK);
  CHECK (ret.type == TS_STRING && !strcmp (ret.as.string, "line"));
  CHECK (call (vm, &ts_std_stdio_module, "read_line", &ret, 0, NULL) == TS_OK
         && ret.type == TS_NIL);
  CHECK (dup2 (saved_stdin, STDIN_FILENO) >= 0);
  close (saved_stdin);
  fclose (input);
  clearerr (stdin);
  ts_value_free (&ret);
  ts_vm_free (vm);
  return 0;
}
