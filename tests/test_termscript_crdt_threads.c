#include "rga.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                              \
  do                                                                          \
    {                                                                         \
      if (!(x))                                                               \
        {                                                                     \
          fprintf (stderr, "%d: %s\n", __LINE__, #x);                         \
          abort ();                                                           \
        }                                                                     \
    }                                                                         \
  while (0)
#define OK(x) CHECK ((x) == TS_CRDT_OK)
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct
{
  TS_CRDT *shared;
  uint64_t site;
} context;
static void *
run (void *arg)
{
  context *c = arg;
  /* Distinct documents need no shared library lock. */
  for (size_t i = 0; i < 40; i++)
    {
      TS_CRDT *a = NULL, *b = NULL;
      OK (ts_crdt_create (c->site, 128, &a));
      OK (ts_crdt_insert (a, 0, "text", 4, NULL));
      OK (ts_crdt_clone (a, c->site + 100, &b));
      OK (ts_crdt_insert (a, 2, "a", 1, NULL));
      OK (ts_crdt_insert (b, 2, "b", 1, NULL));
      OK (ts_crdt_merge (a, b));
      OK (ts_crdt_merge (b, a));
      unsigned char *x = NULL, *y = NULL;
      size_t xn, yn;
      OK (ts_crdt_encode (a, &x, &xn));
      OK (ts_crdt_encode (b, &y, &yn));
      CHECK (xn == yn && !memcmp (x, y, xn));
      free (x);
      free (y);
      ts_crdt_free (a);
      ts_crdt_free (b);
      /* A shared document must serialize reads AND mutations externally. */
      CHECK (!pthread_mutex_lock (&lock));
      size_t length;
      OK (ts_crdt_length (c->shared, &length));
      OK (ts_crdt_insert (c->shared, length, "x", 1, NULL));
      CHECK (!pthread_mutex_unlock (&lock));
    }
  return NULL;
}
int
main (void)
{
  TS_CRDT *shared = NULL;
  OK (ts_crdt_create (99, 1024, &shared));
  pthread_t threads[4];
  context args[4];
  for (size_t i = 0; i < 4; i++)
    {
      args[i] = (context){ shared, i + 1 };
      CHECK (!pthread_create (threads + i, NULL, run, args + i));
    }
  for (size_t i = 0; i < 4; i++)
    CHECK (!pthread_join (threads[i], NULL));
  size_t length;
  OK (ts_crdt_length (shared, &length));
  CHECK (length == 160);
  ts_crdt_free (shared);
  CHECK (!pthread_mutex_destroy (&lock));
  puts ("CRDT independent and externally synchronized documents OK");
  return 0;
}
