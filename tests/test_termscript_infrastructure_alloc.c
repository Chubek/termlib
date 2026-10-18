#include "termscript/stdlib/ts_stdlib.h"
#include "termscript/stdlib/common/ts_std_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Link-time fault injection only; no allocator hooks/global state in libraries. */
void *__real_malloc (size_t);
void *__real_calloc (size_t, size_t);
void *__real_realloc (void *, size_t);
void __real_free (void *);
static void *pointers[4096];
static size_t live, remaining = SIZE_MAX;
static bool injected;
static bool fail (void)
{
  if (remaining == SIZE_MAX) return false;
  if (remaining) { remaining--; return false; }
  remaining = SIZE_MAX; injected = true; return true;
}
static void remember (void *p)
{
  if (!p) return;
  for (size_t i = 0; i < 4096; i++)
    if (!pointers[i]) { pointers[i] = p; live++; return; }
  abort ();
}
static void forget (void *p)
{
  if (!p) return;
  for (size_t i = 0; i < 4096; i++)
    if (pointers[i] == p) { pointers[i] = NULL; live--; return; }
}
void *__wrap_malloc (size_t n)
{ if (fail ()) return NULL; void *p = __real_malloc (n); remember (p); return p; }
void *__wrap_calloc (size_t n, size_t s)
{ if (fail ()) return NULL; void *p = __real_calloc (n, s); remember (p); return p; }
void *__wrap_realloc (void *old, size_t n)
{
  if (fail ()) return NULL;
  forget (old); void *p = __real_realloc (old, n); remember (p ? p : old); return p;
}
char *__wrap_strdup (const char *s)
{ size_t n = strlen (s) + 1; char *p = __wrap_malloc (n); if (p) memcpy (p, s, n); return p; }
void __wrap_free (void *p) { forget (p); __real_free (p); }
static TS_Error error;
#define CHECK(x) do { if (!(x)) { fprintf (stderr, "%d: %s (%s)\n", __LINE__, #x, error.message); exit (1); } } while (0)
static TS_Value str (const char *s) { TS_Value v = { .type = TS_STRING, .as.string = (char *)s }; return v; }
static TS_Value integer (int64_t i) { TS_Value v = { .type = TS_INT, .as.integer = i }; return v; }
static TS_Status call (const TS_Module *m, const char *name, TS_Value *r, size_t n, TS_Value *a)
{
  ts_value_free (r); ts_error_clear (&error);
  const TS_FuncDef *f = ts_module_find_func (m, name); CHECK (f);
  return f->func (NULL, f->userdata, a, n, r, &error);
}
int main (void)
{
  int pipefd[2]; CHECK (!pipe (pipefd)); CHECK (write (pipefd[1], "x", 1) == 1);
  /* Enumerate all allocations along successful creation, mutation and results. */
  for (unsigned op = 0; op < 9; op++)
    {
      bool succeeded = false;
      for (size_t at = 0; at < 128; at++)
        {
          TS_Value s = {0}, r = {0}, wire = {0};
          const TS_Module *m = op < 3 ? &ts_std_index_module : op < 5 ? &ts_std_multiplex_module : &ts_std_protocols_module;
          bool setup = op == 1 || op == 2 || op == 4 || op == 7 || op == 8;
          if (setup) CHECK (call (m, "new", &s, 0, NULL) == TS_OK);
          TS_Value a[] = { s, str ("alpha"), integer (42) };
          if (op == 2) CHECK (call (m, "set", &r, 3, a) == TS_OK);
          if (op == 4)
            { a[1] = integer (pipefd[0]); a[2] = integer (1); CHECK (call (m, "watch", &r, 3, a) == TS_OK); a[1] = integer (0); }
          if (op == 7 || op == 8)
            {
              TS_Value enc[] = { integer (42), str ("0061ff") };
              CHECK (call (m, "encode_hex", &wire, 2, enc) == TS_OK);
              a[1] = wire;
              if (op == 8)
                {
                  TS_Value first[] = { s, str ("545350460100002a00000003") };
                  CHECK (call (m, "feed_hex", &r, 2, first) == TS_OK);
                  a[1] = str (wire.as.string + 24);
                }
            }
          ts_value_free (&r);
          injected = false; remaining = at;
          TS_Status st;
          if (op == 0 || op == 3 || op == 5) st = call (m, "new", &r, 0, NULL);
          else if (op == 1) st = call (m, "set", &r, 3, a);
          else if (op == 2) { a[1] = str ("a"); st = call (m, "prefix", &r, 2, a); }
          else if (op == 4) st = call (m, "wait", &r, 2, a);
          else if (op == 6) { a[0] = integer (42); a[1] = str ("0061ff"); st = call (m, "encode_hex", &r, 2, a); }
          else st = call (m, "feed_hex", &r, 2, a);
          remaining = SIZE_MAX;
          if (injected)
            {
              CHECK (st == TS_ERR_NOMEM && error.code == TS_ERR_NOMEM && r.type == TS_NIL);
              if (op == 1) CHECK (call (m, "len", &r, 1, &s) == TS_OK && r.as.integer == 0);
              if (op == 7 || op == 8) CHECK (call (m, "reset", &r, 1, &s) == TS_OK);
            }
          else { CHECK (st == TS_OK); succeeded = true; }
          ts_value_free (&r); ts_value_free (&s); ts_value_free (&wire); CHECK (!live);
          if (succeeded) break;
        }
      CHECK (succeeded);
    }
  /* C decoder payload-allocation failure consumes only the header and can be
   * retried with its precise unconsumed suffix, including a zero-byte retry. */
  uint8_t wire[32]; size_t written, consumed; TS_ProtocolFrame frame;
  TS_ProtocolDecoder *d;
  CHECK (ts_protocol_encode (1, "abc", 3, wire, sizeof wire, &written, &error) == TS_OK);
  CHECK (ts_protocol_decoder_create (NULL, &d, &error) == TS_OK);
  remaining = 0;
  CHECK (ts_protocol_decoder_feed (d, wire, written, &consumed, &frame, &error) == TS_ERR_NOMEM && consumed == 16);
  remaining = SIZE_MAX;
  CHECK (ts_protocol_decoder_feed (d, wire + consumed, written - consumed, &consumed, &frame, &error) == TS_OK);
  CHECK (frame.size == 3 && !memcmp (frame.payload, "abc", 3));
  ts_protocol_decoder_free (d); CHECK (!live);
  close (pipefd[0]); close (pipefd[1]); puts ("infrastructure allocation cleanup OK"); return 0;
}
