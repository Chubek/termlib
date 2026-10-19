#define _POSIX_C_SOURCE 200809L
#include "src/dt_internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x)                                                              \
  do                                                                          \
    {                                                                         \
      if (!(x))                                                               \
        {                                                                     \
          fprintf (stderr, "%d: %s\n", __LINE__, #x);                         \
          exit (1);                                                           \
        }                                                                     \
    }                                                                         \
  while (0)

#ifdef TS_TEST_ALLOC
static size_t fail_at, allocations;
static int active;
void *__real_malloc (size_t);
void *__real_calloc (size_t, size_t);
void *__real_realloc (void *, size_t);
static int
fail (void)
{
  return active && ++allocations == fail_at;
}
void *
__wrap_malloc (size_t n)
{
  if (fail ())
    {
      errno = ENOMEM;
      return NULL;
    }
  return __real_malloc (n);
}
void *
__wrap_calloc (size_t n, size_t s)
{
  if (fail ())
    {
      errno = ENOMEM;
      return NULL;
    }
  return __real_calloc (n, s);
}
void *
__wrap_realloc (void *p, size_t n)
{
  if (fail ())
    {
      errno = ENOMEM;
      return NULL;
    }
  return __real_realloc (p, n);
}
#endif

static FILE *
fixture (size_t large, FILE *expected)
{
  FILE *stream = tmpfile ();
  DT_Recorder *recorder;
  DT_RecordEvent event = { 0 };
  unsigned char bytes[70000];
  const uint64_t base = UINT64_C (18446744073700000000);
  CHECK (stream && large <= sizeof bytes);
  for (size_t i = 0; i < sizeof bytes; i++)
    bytes[i] = (unsigned char)i;
  recorder = dt_recorder_create (stream, false, NULL);
  CHECK (recorder);
  event.type = DT_RECORD_INPUT;
  event.timestamp_ns = base;
  event.data = (const uint8_t *)"secret input";
  event.size = 12;
  CHECK (dt_recorder_write (recorder, &event, NULL) == DT_OK);
  event.type = DT_RECORD_OUTPUT;
  event.timestamp_ns = base + 1000000;
  event.data = bytes;
  event.size = 300;
  CHECK (dt_recorder_write (recorder, &event, NULL) == DT_OK);
  if (expected)
    CHECK (fwrite (bytes, 1, 300, expected) == 300);
  event.type = DT_RECORD_RESIZE;
  event.timestamp_ns = base + 2000000;
  event.rows = 24;
  event.columns = 80;
  event.data = NULL;
  event.size = 0;
  CHECK (dt_recorder_write (recorder, &event, NULL) == DT_OK);
  event.type = DT_RECORD_OUTPUT;
  event.timestamp_ns = base + 3000000;
  event.data = bytes;
  event.size = large;
  CHECK (dt_recorder_write (recorder, &event, NULL) == DT_OK);
  if (expected)
    CHECK (fwrite (bytes, 1, large, expected) == large);
  event.data = (const uint8_t *)"\xe2\x82";
  event.size = 2;
  CHECK (dt_recorder_write (recorder, &event, NULL) == DT_OK);
  if (expected)
    CHECK (fwrite (event.data, 1, 2, expected) == 2);
  event.timestamp_ns = base + 4000000;
  event.data = (const uint8_t *)"\xac\0\xff";
  event.size = 3;
  CHECK (dt_recorder_write (recorder, &event, NULL) == DT_OK);
  if (expected)
    CHECK (fwrite (event.data, 1, 3, expected) == 3);
  event.type = DT_RECORD_META;
  event.timestamp_ns = base + 2000000;
  event.data = (const uint8_t *)"\"</script>\\\n";
  event.size = strlen ((const char *)event.data);
  CHECK (dt_recorder_write (recorder, &event, NULL) == DT_OK);
  event.type = DT_RECORD_HEARTBEAT;
  event.timestamp_ns = base + 4000000;
  event.data = NULL;
  event.size = 0;
  CHECK (dt_recorder_write (recorder, &event, NULL) == DT_OK);
  event.type = DT_RECORD_OUTPUT;
  CHECK (dt_recorder_write (recorder, &event, NULL) == DT_OK);
  dt_recorder_free (recorder);
  rewind (stream);
  return stream;
}

static const char *names[] = { "replay.tsc", "replay.js", "replay.wat",
                               "gpu.js",     "replay.py", "replay.p5.js" };

static void
save (const char *directory, const char *prefix, const char *name,
      const char *text)
{
  char path[4096];
  CHECK (snprintf (path, sizeof path, "%s/%s%s", directory, prefix, name)
         < (int)sizeof path);
  FILE *file = fopen (path, "wb");
  CHECK (file);
  CHECK (fwrite (text, 1, strlen (text), file) == strlen (text));
  CHECK (fclose (file) == 0);
}

int
main (int argc, char **argv)
{
  char *text = NULL;
  size_t length = 0, capacity = 0;
  dt_buf_str (&text, &length, &capacity, "abc");
  dt_buf_put (&text, &length, &capacity, "de", 2);
  CHECK (text && length == 5 && !strcmp (text, "abcde"));
  length = SIZE_MAX - 1;
  dt_buf_put (&text, &length, &capacity, "x", 1);
  CHECK (!text && length == 0 && capacity == SIZE_MAX);
  dt_buf_str (&text, &length, &capacity, "must not recover silently");
  CHECK (!text && capacity == SIZE_MAX);
  FILE *expected = NULL;
  if (argc == 3 && !strcmp (argv[1], "--emit"))
    {
      char path[4096];
      CHECK (snprintf (path, sizeof path, "%s/expected.bin", argv[2])
             < (int)sizeof path);
      expected = fopen (path, "wb");
      CHECK (expected);
    }
  FILE *stream = fixture (expected ? 70000 : 8192, expected);
  if (expected)
    CHECK (fclose (expected) == 0);
  DT_Replayer *replayer = dt_replayer_create (stream, false, NULL);
  CHECK (replayer);
  long position = ftell (stream);
  CHECK (!dt_replayer_compile (replayer, NULL, (DT_ReplayerTarget)999));
  CHECK (ftell (stream) == position);
#ifdef TS_TEST_ALLOC
  /* Prime the reusable input buffer so every fault sweep has identical state.
   */
  CHECK (dt_replayer_compile (replayer, NULL, DT_CCTARGET_JS));
#endif
  for (int target = 0; target <= DT_CCTARGET_PROCESSING; target++)
    {
      CHECK (dt_replayer_rewind (replayer, NULL) == DT_OK);
#ifdef TS_TEST_ALLOC
      active = 1;
      allocations = 0;
      fail_at = 0;
#endif
      const char *source
          = dt_replayer_compile (replayer, NULL, (DT_ReplayerTarget)target);
#ifdef TS_TEST_ALLOC
      active = 0;
      size_t total = allocations;
#endif
      CHECK (source && *source);
      if (expected)
        save (argv[2], "", names[target], source);
#ifdef TS_TEST_ALLOC
      char *copy = strdup (source);
      CHECK (copy);
      for (size_t fault = 1; fault <= total; fault++)
        {
          CHECK (dt_replayer_rewind (replayer, NULL) == DT_OK);
          fail_at = fault;
          allocations = 0;
          active = 1;
          const char *failed = dt_replayer_compile (replayer, NULL,
                                                    (DT_ReplayerTarget)target);
          active = 0;
          CHECK (!failed);
          CHECK (!strcmp (
              source,
              copy)); /* Failed compile never replaces previous text. */
        }
      free (copy);
#endif
      CHECK (dt_replayer_rewind (replayer, NULL) == DT_OK);
      source = dt_replayer_compile (replayer, "prefix{{events}}suffix",
                                    (DT_ReplayerTarget)target);
      CHECK (source && !strncmp (source, "prefix", 6));
      CHECK (!strcmp (source + strlen (source) - 6, "suffix"));
    }
  dt_replayer_free (replayer);
  fclose (stream);
  /* Empty recordings must generate syntactically valid, callable programs. */
  stream = tmpfile ();
  CHECK (stream);
  DT_Recorder *recorder = dt_recorder_create (stream, false, NULL);
  CHECK (recorder);
  dt_recorder_free (recorder);
  rewind (stream);
  replayer = dt_replayer_create (stream, false, NULL);
  CHECK (replayer);
  for (int target = 0; target <= DT_CCTARGET_PROCESSING; target++)
    {
      CHECK (dt_replayer_rewind (replayer, NULL) == DT_OK);
      const char *source
          = dt_replayer_compile (replayer, NULL, (DT_ReplayerTarget)target);
      CHECK (source && *source);
      if (expected)
        save (argv[2], "empty-", names[target], source);
    }
  dt_replayer_free (replayer);
  fclose (stream);
  /* Bound the number of events even when payloads are empty. */
  if (!expected)
    {
      stream = tmpfile ();
      CHECK (stream);
      recorder = dt_recorder_create (stream, false, NULL);
      CHECK (recorder);
      DT_RecordEvent event = { 0 };
      event.type = DT_RECORD_HEARTBEAT;
      event.timestamp_ns = 1;
      for (size_t i = 0; i <= DT_REPLAY_MAX_EVENTS; i++)
        CHECK (dt_recorder_write (recorder, &event, NULL) == DT_OK);
      dt_recorder_free (recorder);
      rewind (stream);
      replayer = dt_replayer_create (stream, false, NULL);
      CHECK (replayer);
      CHECK (!dt_replayer_compile (replayer, NULL, DT_CCTARGET_JS));
      dt_replayer_free (replayer);
      fclose (stream);

      /* The aggregate budget applies across records, not only per record. */
      unsigned char *payload = calloc (DT_REPLAY_MAX_PAYLOAD, 1);
      CHECK (payload);
      stream = tmpfile ();
      CHECK (stream);
      recorder = dt_recorder_create (stream, false, NULL);
      CHECK (recorder);
      event.type = DT_RECORD_OUTPUT;
      event.data = payload;
      event.size = DT_REPLAY_MAX_PAYLOAD;
      CHECK (dt_recorder_write (recorder, &event, NULL) == DT_OK);
      event.size = 1;
      CHECK (dt_recorder_write (recorder, &event, NULL) == DT_OK);
      free (payload);
      dt_recorder_free (recorder);
      rewind (stream);
      replayer = dt_replayer_create (stream, false, NULL);
      CHECK (replayer);
      CHECK (!dt_replayer_compile (replayer, NULL, DT_CCTARGET_JS));
      dt_replayer_free (replayer);
      fclose (stream);

      /* Truncated recordings must not replace previously compiled text. */
      stream = fixture (32, NULL);
      replayer = dt_replayer_create (stream, false, NULL);
      CHECK (replayer);
      const char *previous
          = dt_replayer_compile (replayer, NULL, DT_CCTARGET_JS);
      CHECK (previous);
      char *copy = strdup (previous);
      CHECK (copy);
      CHECK (ftruncate (fileno (stream), 8 + 16 + 2) == 0);
      CHECK (dt_replayer_rewind (replayer, NULL) == DT_OK);
      CHECK (!dt_replayer_compile (replayer, NULL, DT_CCTARGET_JS));
      CHECK (!strcmp (previous, copy));
      free (copy);
      dt_replayer_free (replayer);
      fclose (stream);
    }
  return 0;
}
