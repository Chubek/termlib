/* Compile the independent engine with fault-injected allocators. Keeping the
 * hooks in this test avoids mutable allocator globals in the public library.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t remaining = SIZE_MAX, live;
static int injected;
static int
should_fail (void)
{
  if (remaining == SIZE_MAX)
    return 0;
  if (remaining)
    {
      remaining--;
      return 0;
    }
  remaining = SIZE_MAX;
  injected = 1;
  return 1;
}
static void *
test_malloc (size_t n)
{
  if (should_fail ())
    return NULL;
  void *p = malloc (n);
  if (p)
    live++;
  return p;
}
static void *
test_calloc (size_t n, size_t size)
{
  if (should_fail ())
    return NULL;
  void *p = calloc (n, size);
  if (p)
    live++;
  return p;
}
static void
test_free (void *p)
{
  if (p)
    {
      if (!live)
        abort ();
      live--;
      free (p);
    }
}
#define malloc test_malloc
#define calloc test_calloc
#define free test_free
#include "../termscript/stdlib/crdt/rga.c"
#undef malloc
#undef calloc
#undef free

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
#define OK(x) CHECK ((x) == TS_CRDT_OK)
static void
unchanged (TS_CRDT *a, TS_CRDT *b)
{
  unsigned char *x = NULL, *y = NULL;
  size_t xn, yn;
  OK (ts_crdt_encode (a, &x, &xn));
  OK (ts_crdt_encode (b, &y, &yn));
  CHECK (xn == yn && !memcmp (x, y, xn));
  test_free (x);
  test_free (y);
}
int
main (void)
{
  TS_CRDT *base = NULL;
  OK (ts_crdt_create (1, 32, &base));
  OK (ts_crdt_insert (base, 0, "abc", 3, NULL));
  unsigned char *snapshot = NULL;
  size_t bytes;
  OK (ts_crdt_encode (base, &snapshot, &bytes));
  TS_CRDTOp op = { { 2, 4 }, { 1, 1 }, 'X', TS_CRDT_INSERT };
  for (unsigned operation = 0; operation < 10; operation++)
    {
      int succeeded = 0;
      for (size_t fail_at = 0; fail_at < 32; fail_at++)
        {
          size_t before = live;
          TS_CRDT *doc = NULL, *output = NULL;
          OK (ts_crdt_clone (base, 1, &doc));
          TS_CRDTBatch batch = { 0 };
          unsigned char *encoded = NULL;
          char *text = NULL;
          size_t count = 0, offset = 0;
          remaining = fail_at;
          injected = 0;
          TS_CRDTStatus status;
          switch (operation)
            {
            case 0:
              status = ts_crdt_insert (doc, 1, "XY", 2, &batch);
              break;
            case 1:
              status = ts_crdt_delete (doc, 0, 2, &batch);
              break;
            case 2:
              status = ts_crdt_apply (doc, &op, 1);
              break;
            case 3:
              status = ts_crdt_text (doc, &text, &count);
              break;
            case 4:
              status = ts_crdt_encode (doc, &encoded, &count);
              break;
            case 5:
              status = ts_crdt_clone (doc, 2, &output);
              break;
            case 6:
              status
                  = ts_crdt_decode (2, 32, snapshot, bytes, &output, &offset);
              break;
            case 7:
              status = ts_crdt_encode_ops (&op, 1, &encoded, &count);
              break;
            case 8:
              status = ts_crdt_create (2, 32, &output);
              break;
            default:
              status = ts_crdt_length (doc, &count);
              break;
            }
          remaining = SIZE_MAX;
          if (injected)
            {
              CHECK (status == TS_CRDT_NOMEM);
              unchanged (doc, base);
              CHECK (!batch.ops && !batch.count && !encoded && !text
                     && !output);
            }
          else
            {
              CHECK (status == TS_CRDT_OK);
              succeeded = 1;
            }
          ts_crdt_batch_free (&batch);
          test_free (encoded);
          test_free (text);
          ts_crdt_free (output);
          ts_crdt_free (doc);
          CHECK (live == before);
          if (succeeded)
            break;
        }
      CHECK (succeeded);
    }
  test_free (snapshot);
  ts_crdt_free (base);
  CHECK (!live);
  puts ("CRDT allocation failures: atomic state and complete cleanup OK");
  return 0;
}
