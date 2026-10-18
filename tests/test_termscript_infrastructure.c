#define _POSIX_C_SOURCE 200809L
#include "termscript/stdlib/ts_stdlib.h"
#include "termscript/stdlib/common/ts_std_common.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static TS_Error error;
#define CHECK(x) do { if (!(x)) { fprintf (stderr, "%d: %s (%s)\n", __LINE__, #x, error.message); exit (1); } } while (0)
static TS_Value integer (int64_t i) { TS_Value v = { .type = TS_INT, .as.integer = i }; return v; }
static TS_Value string (const char *s) { TS_Value v = { .type = TS_STRING, .as.string = (char *)s }; return v; }
static TS_Status call (const TS_Module *m, const char *name, TS_Value *r, size_t n, TS_Value *a)
{
  ts_value_free (r); ts_error_clear (&error);
  const TS_FuncDef *f = ts_module_find_func (m, name); CHECK (f);
  return f->func (NULL, f->userdata, a, n, r, &error);
}
#define IX(f,r,n,a) call (&ts_std_index_module,f,r,n,a)
#define MX(f,r,n,a) call (&ts_std_multiplex_module,f,r,n,a)
#define PR(f,r,n,a) call (&ts_std_protocols_module,f,r,n,a)
static void index_tests (void)
{
  TS_Value s = {0}, r = {0}, snapshot = {0}, alias = {0};
  CHECK (IX ("new", &s, 0, NULL) == TS_OK);
  TS_Value a[] = { s, string (""), integer (INT64_MIN) };
  CHECK (IX ("set", &r, 3, a) == TS_OK && r.as.boolean);
  for (int i = 100; i >= 0; i--)
    {
      char key[16]; snprintf (key, sizeof key, "key%03d", i); a[1] = string (key); a[2] = integer (i);
      CHECK (IX ("set", &r, 3, a) == TS_OK && r.as.boolean);
    }
  a[1] = string ("key050"); a[2] = integer (999);
  CHECK (IX ("set", &r, 3, a) == TS_OK && !r.as.boolean);
  CHECK (IX ("get", &r, 2, a) == TS_OK && r.as.integer == 999);
  a[1] = string ("missing"); CHECK (IX ("get", &r, 2, a) == TS_ERR_NOTFOUND);
  CHECK (IX ("remove", &r, 2, a) == TS_OK && !r.as.boolean);
  a[1] = string ("key00"); CHECK (IX ("prefix", &snapshot, 2, a) == TS_OK);
  ts_std_list_t *list = ts_std_list_unwrap (&snapshot, &error, "snapshot");
  CHECK (list && ts_std_list_len (list) == 10);
  CHECK (!strcmp (ts_std_list_get (list, 0)->as.string, "key000"));
  a[1] = string ("key050"); CHECK (IX ("remove", &r, 2, a) == TS_OK && r.as.boolean);
  CHECK (IX ("has", &r, 2, a) == TS_OK && !r.as.boolean);
  char large[4098]; memset (large, 'x', sizeof large - 1); large[sizeof large - 1] = 0;
  a[1] = string (large); CHECK (IX ("set", &r, 3, a) == TS_ERR_LIMIT);
  CHECK (IX ("clear", &r, 1, &s) == TS_OK);
  CHECK (IX ("len", &r, 1, &s) == TS_OK && r.as.integer == 0);
  /* Aggregate byte budget and failure atomicity. */
  large[4096] = 0;
  for (int i = 0; i < 1023; i++)
    {
      snprintf (large, 5, "%04d", i); large[4] = 'x'; a[1] = string (large);
      CHECK (IX ("set", &r, 3, a) == TS_OK);
    }
  memcpy (large, "last", 4); CHECK (IX ("set", &r, 3, a) == TS_ERR_LIMIT);
  CHECK (IX ("len", &r, 1, &s) == TS_OK && r.as.integer == 1023);
  CHECK (IX ("clear", &r, 1, &s) == TS_OK);
  for (unsigned i = 0; i < 65536; i++)
    {
      char key[16]; snprintf (key, sizeof key, "%05u", i); a[1] = string (key);
      CHECK (IX ("set", &r, 3, a) == TS_OK);
    }
  a[1] = string ("overflow"); CHECK (IX ("set", &r, 3, a) == TS_ERR_LIMIT);
  a[1] = string ("00000"); CHECK (IX ("set", &r, 3, a) == TS_OK && !r.as.boolean);
  CHECK (ts_value_copy (&alias, &s) == TS_OK);
  CHECK (IX ("close", &r, 1, &s) == TS_OK);
  CHECK (IX ("close", &r, 1, &alias) == TS_OK);
  CHECK (IX ("len", &r, 1, &alias) == TS_ERR_INVAL);
  CHECK (!strcmp (ts_std_list_get (list, 9)->as.string, "key009"));
  ts_value_free (&snapshot); ts_value_free (&alias); ts_value_free (&s); ts_value_free (&r);
}
static volatile sig_atomic_t signalled;
static void interrupt_wait (int sig) { (void)sig; signalled = 1; }
static void multiplex_tests (void)
{
  TS_Value s = {0}, r = {0};
  CHECK (MX ("new", &s, 0, NULL) == TS_OK);
  TS_Value a[] = { s, integer (0), integer (1) };
  CHECK (MX ("wait", &r, 2, a) == TS_ERR_TIMEOUT);
  int data[2], cancel[2]; CHECK (!pipe (data) && !pipe (cancel));
  a[1] = integer (data[0]); CHECK (MX ("watch", &r, 3, a) == TS_OK);
  CHECK (MX ("watch", &r, 3, a) == TS_OK);
  CHECK (MX ("len", &r, 1, &s) == TS_OK && r.as.integer == 1);
  a[2] = integer (4); CHECK (MX ("watch", &r, 3, a) == TS_ERR_INVAL);
  a[1] = integer (0); a[2] = integer (cancel[0]);
  CHECK (MX ("wait", &r, 3, a) == TS_ERR_TIMEOUT);
  CHECK (write (data[1], "x", 1) == 1);
  CHECK (MX ("wait", &r, 3, a) == TS_OK);
  ts_std_list_t *list = ts_std_list_unwrap (&r, &error, "events");
  CHECK (list && ts_std_list_len (list) == 2);
  CHECK (ts_std_list_get (list, 0)->as.integer == data[0]);
  CHECK (ts_std_list_get (list, 1)->as.integer & 1);
  CHECK (write (cancel[1], "c", 1) == 1);
  CHECK (MX ("wait", &r, 3, a) == TS_ERR_CANCELLED); /* Cancellation wins. */
  char byte; CHECK (read (cancel[0], &byte, 1) == 1);
  CHECK (read (data[0], &byte, 1) == 1);
  struct sigaction action = {0}, old;
  action.sa_handler = interrupt_wait; sigemptyset (&action.sa_mask);
  CHECK (!sigaction (SIGUSR1, &action, &old));
  pid_t child = fork (); CHECK (child >= 0);
  if (!child)
    {
      struct timespec pause = { 0, 20000000 };
      nanosleep (&pause, NULL); kill (getppid (), SIGUSR1);
      nanosleep (&pause, NULL); _exit (write (cancel[1], "c", 1) == 1 ? 0 : 1);
    }
  a[1] = integer (1000);
  CHECK (MX ("wait", &r, 3, a) == TS_ERR_CANCELLED && signalled);
  int status; CHECK (waitpid (child, &status, 0) == child && WIFEXITED (status) && !WEXITSTATUS (status));
  CHECK (!sigaction (SIGUSR1, &old, NULL));
  CHECK (read (cancel[0], &byte, 1) == 1);
  CHECK (!close (data[1])); a[1] = integer (0);
  CHECK (MX ("wait", &r, 2, a) == TS_OK);
  list = ts_std_list_unwrap (&r, &error, "events"); CHECK (ts_std_list_get (list, 1)->as.integer & 4);
  CHECK (!close (data[0])); CHECK (MX ("wait", &r, 2, a) == TS_OK);
  list = ts_std_list_unwrap (&r, &error, "events"); CHECK (ts_std_list_get (list, 1)->as.integer & 16);
  CHECK (!close (cancel[0]));
  CHECK (MX ("wait", &r, 3, a) == TS_ERR_SYSTEM && error.system_errno == EBADF);
  a[1] = integer (data[0]); CHECK (MX ("remove", &r, 2, a) == TS_OK && r.as.boolean);
  CHECK (MX ("remove", &r, 2, a) == TS_OK && !r.as.boolean);
  /* Readiness does not transfer ownership or require descriptors below FD_SETSIZE. */
  for (int i = 0; i < 1024; i++)
    { a[1] = integer (10000 + i); a[2] = integer (3); CHECK (MX ("watch", &r, 3, a) == TS_OK); }
  a[1] = integer (20000); CHECK (MX ("watch", &r, 3, a) == TS_ERR_LIMIT);
  a[1] = integer (10000); CHECK (MX ("watch", &r, 3, a) == TS_OK);
  CHECK (MX ("close", &r, 1, &s) == TS_OK && fcntl (cancel[1], F_GETFD) >= 0);
  CHECK (MX ("close", &r, 1, &s) == TS_OK);
  CHECK (MX ("wait", &r, 2, a) == TS_ERR_INVAL);
  close (cancel[1]); ts_value_free (&s); ts_value_free (&r);
}
static void protocol_tests (void)
{
  uint8_t wire[64], copy[64]; const uint8_t payload[] = { 0, 1, 255 };
  size_t written, consumed; TS_ProtocolFrame frame; TS_ProtocolDecoder *d = NULL;
  CHECK (ts_protocol_encode (65535, payload, 3, NULL, 0, &written, &error) == TS_ERR_LIMIT && written == 19);
  memset (wire, 0xa5, sizeof wire);
  CHECK (ts_protocol_encode (0, payload, 3, wire, sizeof wire, &written, &error) == TS_ERR_INVAL && wire[0] == 0xa5);
  CHECK (ts_protocol_encode (65535, payload, 3, wire, sizeof wire, &written, &error) == TS_OK && written == 19);
  CHECK (!memcmp (wire, "TSPF\1\0\377\377\0\0\0\3", 12));
  /* Independent Python zlib.crc32 reference vector. */
  CHECK (!memcmp (wire + 12, "\xd6\xae\xed\xde", 4));
  CHECK (ts_protocol_encode (1, payload, TS_PROTOCOL_MAX_PAYLOAD + 1, wire, sizeof wire, &written, &error) == TS_ERR_LIMIT);
  CHECK (ts_protocol_encode (65535, payload, 3, wire, sizeof wire, &written, &error) == TS_OK);
  TS_ProtocolOptions o; ts_protocol_options_init (&o);
  o.reserved[0] = 1; CHECK (ts_protocol_decoder_create (&o, &d, &error) == TS_ERR_INVAL && !d);
  o.reserved[0] = 0; CHECK (ts_protocol_decoder_create (&o, &d, &error) == TS_OK);
  for (size_t i = 0; i < written; i++)
    CHECK (ts_protocol_decoder_feed (d, wire + i, 1, &consumed, &frame, &error)
           == (i + 1 == written ? TS_OK : TS_ERR_WOULD_BLOCK) && consumed == 1);
  for (size_t split = 0; split < written; split++)
    {
      ts_protocol_decoder_reset (d);
      CHECK (ts_protocol_decoder_feed (d, wire, split, &consumed, &frame, &error) == TS_ERR_WOULD_BLOCK && consumed == split);
      CHECK (ts_protocol_decoder_feed (d, wire + split, written - split, &consumed, &frame, &error) == TS_OK);
      CHECK (consumed == written - split && frame.type == 65535 && frame.size == 3 && !memcmp (frame.payload, payload, 3));
      CHECK (ts_protocol_decoder_finish (d, &error) == TS_ERR_EOF);
      CHECK (ts_protocol_decoder_feed (d, wire, written, &consumed, &frame, &error) == TS_ERR_EOF && !consumed);
    }
  for (size_t truncated = 1; truncated < written; truncated++)
    {
      ts_protocol_decoder_reset (d);
      CHECK (ts_protocol_decoder_feed (d, wire, truncated, &consumed, &frame, &error) == TS_ERR_WOULD_BLOCK);
      CHECK (ts_protocol_decoder_finish (d, &error) == TS_ERR_PARSE && error.offset == truncated);
    }
  for (size_t corrupt = 0; corrupt < written; corrupt++)
    {
      memcpy (copy, wire, written); copy[corrupt] ^= 0x80; ts_protocol_decoder_reset (d);
      TS_Status st = ts_protocol_decoder_feed (d, copy, written, &consumed, &frame, &error);
      if (st == TS_ERR_WOULD_BLOCK) st = ts_protocol_decoder_finish (d, &error);
      CHECK (st == TS_ERR_PARSE || st == TS_ERR_UNSUPPORTED || st == TS_ERR_LIMIT);
      CHECK (ts_protocol_decoder_feed (d, wire, written, &consumed, &frame, &error) == st && !consumed);
    }
  memcpy (copy, wire, written); memcpy (copy + written, wire, written);
  ts_protocol_decoder_reset (d);
  CHECK (ts_protocol_decoder_feed (d, copy, 2*written, &consumed, &frame, &error) == TS_OK && consumed == written);
  CHECK (ts_protocol_decoder_feed (d, copy + consumed, written, &consumed, &frame, &error) == TS_OK);
  ts_protocol_decoder_free (d);
  o.max_payload = 2; CHECK (ts_protocol_decoder_create (&o, &d, &error) == TS_OK);
  CHECK (ts_protocol_decoder_feed (d, wire, written, &consumed, &frame, &error) == TS_ERR_LIMIT && error.offset == 8 && consumed == 16);
  ts_protocol_decoder_free (d); ts_protocol_decoder_free (NULL); ts_protocol_decoder_reset (NULL);
  CHECK (ts_protocol_encode (1, NULL, 0, wire, sizeof wire, &written, &error) == TS_OK && written == 16);
  o.max_payload = 0; CHECK (ts_protocol_decoder_create (&o, &d, &error) == TS_OK);
  CHECK (ts_protocol_decoder_feed (d, wire, written, &consumed, &frame, &error) == TS_OK && !frame.size);
  CHECK (ts_protocol_decoder_feed (d, NULL, 0, &consumed, &frame, &error) == TS_ERR_WOULD_BLOCK);
  CHECK (ts_protocol_decoder_finish (d, &error) == TS_ERR_EOF); ts_protocol_decoder_free (d);
  /* Overlapping encoding and strict script-facing hex validation. */
  memcpy (copy, payload, 3); CHECK (ts_protocol_encode (42, copy, 3, copy, sizeof copy, &written, &error) == TS_OK && !memcmp (copy + 16, payload, 3));
  TS_Value s = {0}, r = {0}; CHECK (PR ("new", &s, 0, NULL) == TS_OK);
  TS_Value a[] = { s, string ("0z") }; CHECK (PR ("feed_hex", &r, 2, a) == TS_ERR_PARSE && error.offset == 1);
  a[1] = string ("0"); CHECK (PR ("feed_hex", &r, 2, a) == TS_ERR_PARSE);
  a[1] = string (""); CHECK (PR ("feed_hex", &r, 2, a) == TS_OK && error.code == TS_OK);
  CHECK (PR ("finish", &r, 1, &s) == TS_ERR_EOF);
  CHECK (PR ("reset", &r, 1, &s) == TS_OK);
  CHECK (PR ("close", &r, 1, &s) == TS_OK && PR ("close", &r, 1, &s) == TS_OK);
  CHECK (PR ("reset", &r, 1, &s) == TS_ERR_INVAL);
  ts_value_free (&r); ts_value_free (&s);
}
int main (void)
{
  index_tests (); multiplex_tests (); protocol_tests ();
  puts ("infrastructure modules OK"); return 0;
}
