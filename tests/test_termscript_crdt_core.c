#include "rga.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                              \
  do                                                                          \
    {                                                                         \
      if (!(x))                                                               \
        {                                                                     \
          fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
          exit (1);                                                           \
        }                                                                     \
    }                                                                         \
  while (0)
#define OK(x) CHECK ((x) == TS_CRDT_OK)
static TS_CRDT *
make (uint64_t site, size_t limit)
{
  TS_CRDT *d = NULL;
  OK (ts_crdt_create (site, limit, &d));
  return d;
}
static void
text_is (const TS_CRDT *d, const char *expected)
{
  char *s = NULL;
  size_t bytes, length;
  OK (ts_crdt_text (d, &s, &bytes));
  CHECK (bytes == strlen (expected));
  CHECK (!memcmp (s, expected, bytes + 1));
  free (s);
  OK (ts_crdt_length (d, &length));
  CHECK (length <= bytes);
}
static void
equal (const TS_CRDT *a, const TS_CRDT *b)
{
  unsigned char *x = NULL, *y = NULL;
  size_t xn, yn;
  OK (ts_crdt_encode (a, &x, &xn));
  OK (ts_crdt_encode (b, &y, &yn));
  CHECK (xn == yn && !memcmp (x, y, xn));
  free (x);
  free (y);
  char *s = NULL, *t = NULL;
  OK (ts_crdt_text (a, &s, &xn));
  OK (ts_crdt_text (b, &t, &yn));
  CHECK (xn == yn && !memcmp (s, t, xn));
  free (s);
  free (t);
}
static void
local_and_unicode (void)
{
  TS_CRDT *a = make (1, 32);
  TS_CRDTBatch delta = { 0 };
  text_is (a, "");
  OK (ts_crdt_insert (a, 0, "ab", 2, &delta));
  CHECK (delta.count == 2);
  ts_crdt_batch_free (&delta);
  ts_crdt_batch_free (&delta);
  OK (ts_crdt_insert (a, 1, "\xd9\x85\xe2\x80\x8c\xf0\x9f\x98\x80", 9, NULL));
  text_is (a, "a\xd9\x85\xe2\x80\x8c\xf0\x9f\x98\x80"
              "b");
  size_t length;
  OK (ts_crdt_length (a, &length));
  CHECK (length == 5);
  TS_CRDTId id;
  OK (ts_crdt_id_at (a, 1, &id));
  size_t at;
  int found;
  OK (ts_crdt_index_of (a, id, &at, &found));
  CHECK (found && at == 1);
  OK (ts_crdt_delete (a, 1, 3, &delta));
  CHECK (delta.count == 3);
  ts_crdt_batch_free (&delta);
  text_is (a, "ab");
  CHECK (ts_crdt_size (a) == 5);
  OK (ts_crdt_index_of (a, id, &at, &found));
  CHECK (!found);
  OK (ts_crdt_insert (a, 1, "X", 1, NULL));
  text_is (a, "aXb");
  OK (ts_crdt_delete (a, 3, 0, NULL));
  OK (ts_crdt_insert (a, 3, NULL, 0, NULL));
  CHECK (ts_crdt_insert (a, 4, "x", 1, NULL) == TS_CRDT_INVALID);
  CHECK (ts_crdt_delete (a, 2, SIZE_MAX, NULL) == TS_CRDT_INVALID);
  CHECK (ts_crdt_id_at (a, 3, &id) == TS_CRDT_INVALID);
  const char *invalid[]
      = { "\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80" };
  for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
    {
      CHECK (ts_crdt_insert (a, 0, invalid[i], strlen (invalid[i]), &delta)
             == TS_CRDT_INVALID);
      CHECK (!delta.ops && !delta.count);
      text_is (a, "aXb");
    }
  ts_crdt_free (a);
  a = make (1, 2);
  OK (ts_crdt_insert (a, 0, "\0", 1, NULL));
  char *s = NULL;
  size_t bytes;
  OK (ts_crdt_text (a, &s, &bytes));
  CHECK (bytes == 1 && !s[0] && !s[1]);
  free (s);
  ts_crdt_free (a);
}
static void
concurrent (void)
{
  TS_CRDT *a = make (1, 128), *b = NULL;
  OK (ts_crdt_insert (a, 0, "ab", 2, NULL));
  OK (ts_crdt_clone (a, 2, &b));
  OK (ts_crdt_insert (a, 1, "XY", 2, NULL));
  OK (ts_crdt_insert (b, 1, "pq", 2, NULL));
  OK (ts_crdt_merge (a, b));
  OK (ts_crdt_merge (b, a));
  equal (a, b);
  text_is (a, "apqXYb");
  OK (ts_crdt_merge (a, a));
  equal (a, b);
  OK (ts_crdt_delete (a, 0, 1, NULL));
  OK (ts_crdt_insert (b, 1, "!", 1, NULL));
  OK (ts_crdt_merge (b, a));
  OK (ts_crdt_merge (a, b));
  equal (a, b);
  text_is (a, "!pqXYb");
  ts_crdt_free (a);
  ts_crdt_free (b);
}
/* Every permutation, including delete-before-insert and child-before-parent.
 */
static const TS_CRDTOp events[]
    = { { { 1, 1 }, { 0, 0 }, 'a', TS_CRDT_INSERT },
        { { 1, 2 }, { 1, 1 }, 'b', TS_CRDT_INSERT },
        { { 2, 2 }, { 1, 1 }, 'X', TS_CRDT_INSERT },
        { { 2, 3 }, { 2, 2 }, 'Y', TS_CRDT_INSERT },
        { { 1, 1 }, { 0, 0 }, 0, TS_CRDT_DELETE },
        { { 2, 2 }, { 0, 0 }, 0, TS_CRDT_DELETE } };
static void
permutations (size_t *order, size_t at, TS_CRDT *expected)
{
  if (at == 6)
    {
      TS_CRDT *d = make (7, 32);
      for (size_t i = 0; i < 6; i++)
        {
          OK (ts_crdt_apply (d, events + order[i], 1));
          OK (ts_crdt_apply (d, events + order[i], 1));
        }
      equal (d, expected);
      text_is (d, "Yb");
      ts_crdt_free (d);
      return;
    }
  for (size_t i = at; i < 6; i++)
    {
      size_t t = order[at];
      order[at] = order[i];
      order[i] = t;
      permutations (order, at + 1, expected);
      t = order[at];
      order[at] = order[i];
      order[i] = t;
    }
}
static void
delivery (void)
{
  TS_CRDT *d = make (9, 32);
  OK (ts_crdt_apply (d, events, 6));
  size_t order[] = { 0, 1, 2, 3, 4, 5 };
  permutations (order, 0, d);
  ts_crdt_free (d);
  d = make (3, 32);
  OK (ts_crdt_apply (d, events + 3, 1));
  text_is (d, "");
  OK (ts_crdt_apply (d, events + 2, 1));
  text_is (d, "");
  unsigned char *snapshot = NULL;
  size_t bytes, offset;
  TS_CRDT *copy = NULL;
  OK (ts_crdt_encode (d, &snapshot, &bytes));
  OK (ts_crdt_decode (4, 32, snapshot, bytes, &copy, &offset));
  free (snapshot);
  OK (ts_crdt_apply (copy, events, 1));
  text_is (copy, "aXY");
  ts_crdt_free (copy);
  ts_crdt_free (d);
}
static void
invalid_and_limits (void)
{
  TS_CRDT *d = make (1, 2), *before = NULL;
  OK (ts_crdt_insert (d, 0, "ab", 2, NULL));
  OK (ts_crdt_clone (d, 8, &before));
  CHECK (ts_crdt_insert (d, 2, "c", 1, NULL) == TS_CRDT_LIMIT);
  equal (d, before);
  TS_CRDTOp invalid[] = {
    { { 0, 1 }, { 0, 0 }, 'x', 1 },      { { 1, 0 }, { 0, 0 }, 'x', 1 },
    { { 1, 1 }, { 1, 1 }, 'x', 1 },      { { 2, 3 }, { 0, 1 }, 'x', 1 },
    { { 2, 3 }, { 1, 4 }, 'x', 1 },      { { 2, 3 }, { 0, 0 }, 0xd800, 1 },
    { { 2, 3 }, { 0, 0 }, 0x110000, 1 }, { { 2, 3 }, { 0, 0 }, 'x', 2 },
    { { 2, 3 }, { 0, 0 }, 'x', 4 }
  };
  for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
    {
      CHECK (ts_crdt_apply (d, invalid + i, 1) == TS_CRDT_INVALID);
      equal (d, before);
    }
  TS_CRDTOp conflict[]
      = { { { 3, 3 }, { 0, 0 }, 'z', 1 }, { { 1, 1 }, { 0, 0 }, 'x', 1 } };
  CHECK (ts_crdt_apply (d, conflict, 2) == TS_CRDT_CONFLICT);
  equal (d, before);
  OK (ts_crdt_apply (d, events, 2));
  equal (d, before); /* Full but duplicate-safe. */
  TS_CRDTOp unseen_delete = { { 99, 9 }, { 0, 0 }, 0, 2 };
  CHECK (ts_crdt_apply (d, &unseen_delete, 1) == TS_CRDT_LIMIT);
  equal (d, before);
  OK (ts_crdt_delete (d, 0, 2, NULL));
  text_is (d, "");
  CHECK (ts_crdt_insert (d, 0, "c", 1, NULL)
         == TS_CRDT_LIMIT); /* No unsafe GC. */
  ts_crdt_free (before);
  ts_crdt_free (d);
  d = make (1, 8);
  TS_CRDTOp overflow = { { 2, UINT64_MAX }, { 0, 0 }, 'x', 1 };
  OK (ts_crdt_apply (d, &overflow, 1));
  CHECK (ts_crdt_insert (d, 0, "c", 1, NULL) == TS_CRDT_LIMIT);
  text_is (d, "x");
  ts_crdt_free (d);
  d = (TS_CRDT *)1;
  CHECK (ts_crdt_create (0, 8, &d) == TS_CRDT_INVALID && !d);
  CHECK (ts_crdt_create (1, TS_CRDT_MAX_NODES + 1, &d) == TS_CRDT_INVALID
         && !d);
  CHECK (ts_crdt_insert (NULL, 0, "", 0, NULL) == TS_CRDT_INVALID);
  ts_crdt_free (NULL);
  ts_crdt_batch_free (NULL);
}
static uint32_t
crc32 (const unsigned char *p, size_t n)
{
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < n; i++)
    {
      crc ^= p[i];
      for (unsigned b = 0; b < 8; b++)
        crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
  return ~crc;
}
static void
fix_crc (unsigned char *p, size_t n)
{
  uint32_t crc
      = crc32 (p + TS_CRDT_SNAPSHOT_HEADER, n - TS_CRDT_SNAPSHOT_HEADER);
  for (unsigned i = 0; i < 4; i++)
    p[16 + i] = (unsigned char)(crc >> (i * 8));
}
static void
snapshots (void)
{
  /* Golden bytes pin wire order/width and an independently computed CRC32. */
  TS_CRDT *golden_doc = make (1, 8);
  OK (ts_crdt_insert (golden_doc, 0, "A", 1, NULL));
  unsigned char expected[64] = { 'T', 'S', 'R', 'G', 'A', 0, 1, 0 };
  expected[8] = 1;
  expected[12] = 40;
  expected[16] = 0x4a;
  expected[17] = 0x0f;
  expected[18] = 0xf9;
  expected[19] = 0xea;
  expected[24] = 1;
  expected[32] = 1;
  expected[56] = 'A';
  expected[60] = 1;
  unsigned char *golden = NULL;
  size_t golden_size;
  OK (ts_crdt_encode (golden_doc, &golden, &golden_size));
  CHECK (golden_size == sizeof expected
         && !memcmp (golden, expected, golden_size));
  free (golden);
  ts_crdt_free (golden_doc);
  TS_CRDT *d = make (1, 32), *copy = NULL;
  OK (ts_crdt_apply (d, events, 6));
  unsigned char *data = NULL;
  size_t bytes, offset;
  OK (ts_crdt_encode (d, &data, &bytes));
  OK (ts_crdt_decode (2, 32, data, bytes, &copy, &offset));
  CHECK (offset == 0);
  equal (d, copy);
  OK (ts_crdt_insert (copy, 1, "!", 1, NULL));
  text_is (copy, "Y!b");
  ts_crdt_free (copy);
  for (size_t i = 0; i < bytes; i++)
    {
      copy = NULL;
      CHECK (ts_crdt_decode (3, 32, data, i, &copy, &offset) == TS_CRDT_FORMAT
             && !copy);
      CHECK (offset <= i);
    }
  for (size_t i = 0; i < bytes; i++)
    {
      data[i] ^= 1;
      CHECK (ts_crdt_decode (3, 32, data, bytes, &copy, &offset) != TS_CRDT_OK
             && !copy);
      CHECK (offset <= bytes);
      data[i] ^= 1;
    }
  CHECK (ts_crdt_decode (3, 1, data, bytes, &copy, &offset) == TS_CRDT_LIMIT
         && !copy);
  /* Recompute checksums to exercise semantic validation, not just CRC checks.
   */
  unsigned char *bad = malloc (bytes + 1);
  CHECK (bad);
  memcpy (bad, data, bytes);
  bad[bytes] = 0;
  CHECK (ts_crdt_decode (3, 32, bad, bytes + 1, &copy, &offset)
         == TS_CRDT_FORMAT);
  bad[TS_CRDT_SNAPSHOT_HEADER] = 0;
  fix_crc (bad, bytes);
  CHECK (ts_crdt_decode (3, 32, bad, bytes, &copy, &offset) == TS_CRDT_FORMAT);
  memcpy (bad, data, bytes);
  bad[TS_CRDT_SNAPSHOT_HEADER + 36] = 8;
  fix_crc (bad, bytes);
  CHECK (ts_crdt_decode (3, 32, bad, bytes, &copy, &offset) == TS_CRDT_FORMAT);
  memcpy (bad, data, bytes);
  memcpy (bad + TS_CRDT_SNAPSHOT_HEADER + 40, bad + TS_CRDT_SNAPSHOT_HEADER,
          40);
  fix_crc (bad, bytes);
  CHECK (ts_crdt_decode (3, 32, bad, bytes, &copy, &offset) == TS_CRDT_FORMAT);
  free (bad);
  free (data);
  ts_crdt_free (d);
}
static uint32_t seed = 0x12345678;
static uint32_t
random32 (void)
{
  seed ^= seed << 13;
  seed ^= seed >> 17;
  seed ^= seed << 5;
  return seed;
}
static void
randomized (void)
{
  for (unsigned trial = 0; trial < 12; trial++)
    {
      TS_CRDT *d[3] = { make (1, 1024), make (2, 1024), make (3, 1024) };
      TS_CRDTOp log[2048];
      size_t count = 0;
      for (unsigned step = 0; step < 180; step++)
        {
          size_t who = random32 () % 3, length;
          OK (ts_crdt_length (d[who], &length));
          TS_CRDTBatch delta = { 0 };
          if (length && random32 () % 3 == 0)
            OK (ts_crdt_delete (d[who], random32 () % length, 1, &delta));
          else
            {
              const char *s = random32 () % 2 ? "ab" : "\xd9\x85";
              OK (ts_crdt_insert (d[who], random32 () % (length + 1), s,
                                  strlen (s), &delta));
            }
          CHECK (count + delta.count <= 2048);
          memcpy (log + count, delta.ops, delta.count * sizeof *log);
          count += delta.count;
          if (random32 () % 2)
            OK (ts_crdt_apply (d[random32 () % 3], delta.ops, delta.count));
          ts_crdt_batch_free (&delta);
          if (step % 19 == 0)
            OK (ts_crdt_merge (d[random32 () % 3], d[random32 () % 3]));
        }
      OK (ts_crdt_merge (d[0], d[1]));
      OK (ts_crdt_merge (d[0], d[2]));
      OK (ts_crdt_merge (d[1], d[0]));
      OK (ts_crdt_merge (d[2], d[0]));
      equal (d[0], d[1]);
      equal (d[0], d[2]);
      for (size_t i = count; i > 1; i--)
        {
          size_t j = random32 () % i;
          TS_CRDTOp tmp = log[i - 1];
          log[i - 1] = log[j];
          log[j] = tmp;
        }
      TS_CRDT *replay = make (8, 1024);
      for (size_t i = 0; i < count; i++)
        OK (ts_crdt_apply (replay, log + i, 1));
      equal (replay, d[0]);
      ts_crdt_free (replay);
      for (size_t i = 0; i < 3; i++)
        ts_crdt_free (d[i]);
    }
}
static void
deep_and_fuzz (void)
{
  TS_CRDT *d = make (1, TS_CRDT_MAX_NODES);
  char *text = malloc (TS_CRDT_MAX_NODES);
  CHECK (text);
  memset (text, 'x', TS_CRDT_MAX_NODES);
  OK (ts_crdt_insert (d, 0, text, TS_CRDT_MAX_NODES, NULL));
  free (text);
  size_t length;
  OK (ts_crdt_length (d, &length));
  CHECK (length == TS_CRDT_MAX_NODES);
  OK (ts_crdt_delete (d, 0, length, NULL));
  text_is (d, "");
  ts_crdt_free (d);
  unsigned char garbage[320];
  for (unsigned i = 0; i < 2000; i++)
    {
      size_t size = random32 () % sizeof garbage;
      for (size_t j = 0; j < size; j++)
        garbage[j] = (unsigned char)random32 ();
      size_t offset;
      d = NULL;
      CHECK (ts_crdt_decode (1, 128, garbage, size, &d, &offset) != TS_CRDT_OK
             && !d);
    }
}
int
main (void)
{
  CHECK (!strcmp (ts_crdt_version (), "1.0.0"));
  CHECK ((ts_crdt_features () & TS_CRDT_FEATURE_OUT_OF_ORDER) != 0);
  local_and_unicode ();
  concurrent ();
  delivery ();
  invalid_and_limits ();
  snapshots ();
  randomized ();
  deep_and_fuzz ();
  puts ("CRDT core: convergence, replay, Unicode, snapshots and limits OK");
  return 0;
}
