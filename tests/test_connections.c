/* Self-contained transport regressions; no external server is contacted. */
#define _POSIX_C_SOURCE 200809L
#include "termlib.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

#ifdef TS_TEST_ALLOC
static int allocation_failure;
void *__real_malloc (size_t size);
void *__real_calloc (size_t count, size_t size);
void *
__wrap_malloc (size_t size)
{
  if (allocation_failure && --allocation_failure == 0)
    {
      errno = ENOMEM;
      return NULL;
    }
  return __real_malloc (size);
}
void *
__wrap_calloc (size_t count, size_t size)
{
  if (allocation_failure && --allocation_failure == 0)
    {
      errno = ENOMEM;
      return NULL;
    }
  return __real_calloc (count, size);
}
#endif
#define CHECK(x)                                                              \
  do                                                                          \
    {                                                                         \
      if (!(x))                                                               \
        {                                                                     \
          fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
          failures++;                                                         \
        }                                                                     \
    }                                                                         \
  while (0)
#define REQUIRE(x)                                                            \
  do                                                                          \
    {                                                                         \
      if (!(x))                                                               \
        {                                                                     \
          perror (#x);                                                        \
          exit (2);                                                           \
        }                                                                     \
    }                                                                         \
  while (0)

typedef struct
{
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  int ready, go;
} StartGate;

static void *
concurrent_recording (void *arg)
{
  StartGate *gate = arg;
  FILE *stream;
  DT_Recorder *recorder;
  DT_Replayer *replayer;
  DT_RecordEvent event = { 0 }, read_event;
  int failed = 0;
  REQUIRE (pthread_mutex_lock (&gate->mutex) == 0);
  gate->ready++;
  REQUIRE (pthread_cond_broadcast (&gate->cond) == 0);
  while (!gate->go)
    REQUIRE (pthread_cond_wait (&gate->cond, &gate->mutex) == 0);
  REQUIRE (pthread_mutex_unlock (&gate->mutex) == 0);
  stream = tmpfile ();
  REQUIRE (stream != NULL);
  recorder = dt_recorder_create (stream, false, NULL);
  REQUIRE (recorder != NULL);
  event.type = DT_RECORD_OUTPUT;
  event.timestamp_ns = 1;
  event.data = (const uint8_t *)"123456789";
  event.size = 9;
  for (int i = 0; i < 64; i++)
    if (dt_recorder_write (recorder, &event, NULL) != DT_OK)
      failed = 1;
  dt_recorder_free (recorder);
  rewind (stream);
  replayer = dt_replayer_create (stream, false, NULL);
  REQUIRE (replayer != NULL);
  dt_replayer_set_mode (replayer, DT_REPLAY_FAST);
  for (int i = 0; i < 64; i++)
    {
      if (dt_replayer_next (replayer, &read_event, NULL) != DT_OK
          || read_event.size != 9 || memcmp (read_event.data, event.data, 9))
        failed = 1;
    }
  dt_replayer_free (replayer);
  fclose (stream);
  return failed ? "recording failed" : NULL;
}

static void
crc_first_use (void)
{
  StartGate gate
      = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0 };
  pthread_t threads[8];
  for (size_t i = 0; i < 8; i++)
    REQUIRE (pthread_create (&threads[i], NULL, concurrent_recording, &gate)
             == 0);
  REQUIRE (pthread_mutex_lock (&gate.mutex) == 0);
  while (gate.ready != 8)
    REQUIRE (pthread_cond_wait (&gate.cond, &gate.mutex) == 0);
  gate.go = 1;
  REQUIRE (pthread_cond_broadcast (&gate.cond) == 0);
  REQUIRE (pthread_mutex_unlock (&gate.mutex) == 0);
  for (size_t i = 0; i < 8; i++)
    {
      void *result;
      REQUIRE (pthread_join (threads[i], &result) == 0);
      CHECK (result == NULL);
    }
  REQUIRE (pthread_mutex_destroy (&gate.mutex) == 0);
  REQUIRE (pthread_cond_destroy (&gate.cond) == 0);
}

static void
write_all (int fd, const void *data, size_t size)
{
  const char *p = data;
  while (size)
    {
      ssize_t n = write (fd, p, size);
      if (n < 0 && errno == EINTR)
        continue;
      REQUIRE (n > 0);
      p += n;
      size -= (size_t)n;
    }
}

/* Obtain a real wire frame through the public send API. */
static size_t
make_frame (unsigned char *wire, size_t cap)
{
  int fd[2];
  DT_LocalConnOptions opts;
  DT_Error error;
  DT_Message *msg = dt_message_create (DT_MESSAGE_DATA, "a\0b", 3, NULL);
  DT_Envelope *env = dt_envelope_create (42, msg, NULL);
  DT_LocalConn *conn;
  ssize_t n;
  REQUIRE (env != NULL);
  REQUIRE (socketpair (AF_UNIX, SOCK_STREAM, 0, fd) == 0);
  dt_localconn_options_init (&opts);
  opts.fd = fd[0];
  opts.take_ownership = true;
  conn = dt_localconn_open (&opts, &error);
  REQUIRE (conn != NULL);
  REQUIRE (dt_localconn_send (conn, env, &error) == DT_OK);
  n = read (fd[1], wire, cap);
  REQUIRE (n == 20);
  dt_envelope_free (env);
  dt_localconn_close (conn);
  close (fd[1]);
  return (size_t)n;
}

static void
fragmented_receive (const unsigned char *wire, size_t size, int timeout)
{
  /* Every possible split, including the length, message id and checksum. */
  for (size_t split = 1; split < size; split++)
    {
      int fd[2];
      DT_LocalConnOptions opts;
      DT_LocalConn *conn;
      DT_Envelope *env = NULL;
      DT_Error error;
      size_t len = 0;
      REQUIRE (socketpair (AF_UNIX, SOCK_STREAM, 0, fd) == 0);
      dt_localconn_options_init (&opts);
      opts.fd = fd[0];
      opts.take_ownership = true;
      opts.nonblocking = timeout == 0;
      opts.read_timeout_ms = timeout;
      conn = dt_localconn_open (&opts, &error);
      REQUIRE (conn != NULL);
      write_all (fd[1], wire, split);
      CHECK (dt_localconn_receive (conn, &env, &error)
             == (timeout == 0 ? DT_ERR_WOULD_BLOCK : DT_ERR_TIMEOUT));
      CHECK (env == NULL);
      CHECK (dt_localconn_state (conn) == DT_CONN_OPEN);
      write_all (fd[1], wire + split, size - split);
      CHECK (dt_localconn_receive (conn, &env, &error) == DT_OK);
      if (env)
        {
          const uint8_t *data
              = dt_message_data (dt_envelope_message (env), &len);
          CHECK (dt_envelope_message_id (env) == 42);
          CHECK (len == 3 && memcmp (data, "a\0b", 3) == 0);
          dt_envelope_free (env);
          env = NULL;
        }
      /* The next complete frame must still be aligned. */
      write_all (fd[1], wire, size);
      CHECK (dt_localconn_receive (conn, &env, &error) == DT_OK);
      dt_envelope_free (env);
      dt_localconn_close (conn);
      close (fd[1]);
    }
}

static void
truncated_receive (const unsigned char *wire, size_t size)
{
  for (size_t split = 0; split < size; split++)
    {
      int fd[2];
      DT_LocalConnOptions opts;
      DT_LocalConn *conn;
      DT_Envelope *env = NULL;
      DT_Error error;
      REQUIRE (socketpair (AF_UNIX, SOCK_STREAM, 0, fd) == 0);
      dt_localconn_options_init (&opts);
      opts.fd = fd[0];
      opts.take_ownership = true;
      conn = dt_localconn_open (&opts, &error);
      REQUIRE (conn != NULL);
      write_all (fd[1], wire, split);
      close (fd[1]);
      CHECK (dt_localconn_receive (conn, &env, &error)
             == (split ? DT_ERR_PROTOCOL : DT_ERR_EOF));
      CHECK (env == NULL);
      CHECK (dt_localconn_state (conn) == DT_CONN_SHUTDOWN);
      dt_localconn_close (conn);
    }
}

static void
handshake (const char *reply, size_t len, DT_Status expected)
{
  struct sockaddr_in addr;
  socklen_t alen = sizeof addr;
  int listener = socket (AF_INET, SOCK_STREAM, 0), status;
  pid_t child;
  DT_RemoteConnOptions opts;
  DT_RemoteConn *conn;
  DT_Error error;
  REQUIRE (listener >= 0);
  memset (&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  REQUIRE (bind (listener, (struct sockaddr *)&addr, sizeof addr) == 0);
  REQUIRE (getsockname (listener, (struct sockaddr *)&addr, &alen) == 0);
  REQUIRE (listen (listener, 1) == 0);
  child = fork ();
  REQUIRE (child >= 0);
  if (child == 0)
    {
      char c;
      int fd = accept (listener, NULL, NULL);
      if (fd < 0)
        _exit (2);
      while (read (fd, &c, 1) == 1 && c != '\n')
        {
        }
      write_all (fd, reply, len);
      close (fd);
      close (listener);
      _exit (0);
    }
  dt_remoteconn_options_init (&opts);
  opts.host = "127.0.0.1";
  opts.port = ntohs (addr.sin_port);
  opts.connect_timeout_ms = 500;
  dt_error_clear (&error);
  conn = dt_remoteconn_open (&opts, &error);
  CHECK (expected == DT_OK ? conn != NULL : conn == NULL);
  if (expected != DT_OK)
    CHECK (error.code == expected);
  dt_remoteconn_close (conn);
  close (listener);
  REQUIRE (waitpid (child, &status, 0) == child);
  CHECK (WIFEXITED (status) && WEXITSTATUS (status) == 0);
}

static void
backpressure (void)
{
  int fd[2], small = 4096;
  DT_LocalConnOptions opts;
  DT_LocalConn *sender, *receiver;
  DT_Envelope *env, *other, *received = NULL;
  DT_Status sent, got = DT_ERR_WOULD_BLOCK;
  DT_Error error;
  const size_t size = 128 * 1024;
  unsigned char *data = malloc (size);
  REQUIRE (data != NULL);
  for (size_t i = 0; i < size; i++)
    data[i] = (unsigned char)i;
  REQUIRE (socketpair (AF_UNIX, SOCK_STREAM, 0, fd) == 0);
  REQUIRE (setsockopt (fd[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small)
           == 0);
  dt_localconn_options_init (&opts);
  opts.fd = fd[0];
  opts.nonblocking = true;
  opts.take_ownership = true;
  sender = dt_localconn_open (&opts, &error);
  opts.fd = fd[1];
  receiver = dt_localconn_open (&opts, &error);
  REQUIRE (sender && receiver);
  env = dt_envelope_create (
      123, dt_message_create (DT_MESSAGE_DATA, data, size, NULL), NULL);
  other = dt_envelope_create (124, dt_message_create_heartbeat (NULL), NULL);
  REQUIRE (env && other);
  sent = dt_localconn_send (sender, env, &error);
  CHECK (sent == DT_ERR_WOULD_BLOCK && error.code == sent);
  CHECK (dt_localconn_send (sender, other, &error) == DT_ERR_INVALID_ARGUMENT);
  for (int i = 0; i < 1000 && (sent != DT_OK || got != DT_OK); i++)
    {
      if (got != DT_OK)
        got = dt_localconn_receive (receiver, &received, &error);
      if (sent != DT_OK)
        sent = dt_localconn_send (sender, env, &error);
      if ((got != DT_OK && got != DT_ERR_WOULD_BLOCK)
          || (sent != DT_OK && sent != DT_ERR_WOULD_BLOCK))
        break;
    }
  CHECK (sent == DT_OK && got == DT_OK);
  if (received)
    {
      size_t n;
      const uint8_t *bytes
          = dt_message_data (dt_envelope_message (received), &n);
      CHECK (dt_envelope_message_id (received) == 123);
      CHECK (n == size && memcmp (bytes, data, size) == 0);
    }
  dt_envelope_free (received);
  CHECK (dt_localconn_send (sender, other, &error) == DT_OK);
  received = NULL;
  CHECK (dt_localconn_receive (receiver, &received, &error) == DT_OK);
  CHECK (dt_envelope_message_id (received) == 124);
  dt_envelope_free (received);
  /* Pending allocations are also cleaned up when callers abandon retries. */
  CHECK (dt_localconn_send (sender, env, &error) == DT_ERR_WOULD_BLOCK);
  CHECK (dt_localconn_receive (receiver, &received, &error)
         == DT_ERR_WOULD_BLOCK);
  dt_envelope_free (env);
  dt_envelope_free (other);
  dt_localconn_close (sender);
  dt_localconn_close (receiver);
  free (data);
}

static void *
blocked_receive (void *arg)
{
  DT_Envelope *env = NULL;
  DT_Status *result = malloc (sizeof *result);
  REQUIRE (result != NULL);
  *result = dt_localconn_receive (arg, &env, NULL);
  dt_envelope_free (env);
  return result;
}

static void
cancel_and_closed_peer (void)
{
  int fd[2];
  DT_LocalConnOptions opts;
  DT_LocalConn *conn;
  pthread_t thread;
  void *result;
  DT_Error error;
  dt_localconn_options_init (&opts);
  opts.take_ownership = true;
  for (int use_pipe = 0; use_pipe <= 1; use_pipe++)
    {
      struct timespec delay = { 0, 20000000 };
      REQUIRE (
          (use_pipe ? pipe (fd) : socketpair (AF_UNIX, SOCK_STREAM, 0, fd))
          == 0);
      opts.fd = fd[0];
      conn = dt_localconn_open (&opts, &error);
      REQUIRE (conn != NULL);
      REQUIRE (pthread_create (&thread, NULL, blocked_receive, conn) == 0);
      nanosleep (&delay, NULL); /* Exercise cancellation of an active wait. */
      dt_localconn_cancel (conn);
      REQUIRE (pthread_join (thread, &result) == 0);
      CHECK (*(DT_Status *)result == DT_ERR_CANCELLED);
      free (result);
      dt_localconn_close (conn);
      close (fd[1]);
    }
  REQUIRE (socketpair (AF_UNIX, SOCK_STREAM, 0, fd) == 0);
  opts.fd = fd[0];
  conn = dt_localconn_open (&opts, &error);
  REQUIRE (conn != NULL);
  close (fd[1]);
  /* Must report EOF, not terminate the host application with SIGPIPE. */
  CHECK (dt_localconn_ping (conn, 1, &error) == DT_ERR_EOF);
  CHECK (dt_localconn_state (conn) == DT_CONN_SHUTDOWN);
  dt_localconn_close (conn);
}

typedef struct
{
  int fd;
  const unsigned char *wire;
  size_t size;
} SlowFrame;

static void *
slow_frame (void *arg)
{
  SlowFrame *frame = arg;
  struct timespec delay = { 0, 10000000 };
  for (size_t i = 0; i < frame->size; i++)
    {
      write_all (frame->fd, frame->wire + i, 1);
      nanosleep (&delay, NULL);
    }
  return NULL;
}

static void
receive_deadline (const unsigned char *wire, size_t size)
{
  int fd[2];
  DT_LocalConnOptions opts;
  DT_LocalConn *conn;
  DT_Envelope *env = NULL;
  DT_Error error;
  pthread_t thread;
  REQUIRE (socketpair (AF_UNIX, SOCK_STREAM, 0, fd) == 0);
  SlowFrame frame = { fd[1], wire, size };
  dt_localconn_options_init (&opts);
  opts.fd = fd[0];
  opts.take_ownership = true;
  opts.read_timeout_ms = 40;
  conn = dt_localconn_open (&opts, &error);
  REQUIRE (conn != NULL);
  REQUIRE (pthread_create (&thread, NULL, slow_frame, &frame) == 0);
  CHECK (dt_localconn_receive (conn, &env, &error) == DT_ERR_TIMEOUT);
  CHECK (env == NULL);
  REQUIRE (pthread_join (thread, NULL) == 0);
  CHECK (dt_localconn_receive (conn, &env, &error) == DT_OK);
  dt_envelope_free (env);
  dt_localconn_close (conn);
  close (fd[1]);
}

#ifdef TS_TEST_ALLOC
static void
allocation_retries (const unsigned char *wire, size_t size)
{
  for (int fault = 1; fault <= 3; fault++)
    {
      int fd[2];
      DT_LocalConnOptions opts;
      DT_LocalConn *conn;
      DT_Envelope *env = NULL;
      DT_Error error;
      REQUIRE (socketpair (AF_UNIX, SOCK_STREAM, 0, fd) == 0);
      dt_localconn_options_init (&opts);
      opts.fd = fd[0];
      opts.take_ownership = true;
      conn = dt_localconn_open (&opts, &error);
      REQUIRE (conn != NULL);
      write_all (fd[1], wire, size);
      allocation_failure = fault; /* Envelope, wire buffer, decoded message. */
      DT_Status st = dt_localconn_receive (conn, &env, &error);
      allocation_failure = 0;
      CHECK (st == DT_ERR_NO_MEMORY && error.code == st);
      CHECK (env == NULL);
      CHECK (dt_localconn_state (conn) == DT_CONN_OPEN);
      CHECK (dt_localconn_receive (conn, &env, &error) == DT_OK);
      CHECK (dt_envelope_message_id (env) == 42);
      dt_envelope_free (env);
      dt_localconn_close (conn);
      close (fd[1]);
    }
  {
    int fd[2];
    DT_LocalConnOptions opts;
    DT_LocalConn *conn;
    DT_Error error;
    REQUIRE (socketpair (AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    dt_localconn_options_init (&opts);
    opts.fd = fd[0];
    opts.take_ownership = true;
    conn = dt_localconn_open (&opts, &error);
    REQUIRE (conn != NULL);
    /* ping's message allocation precedes the wire-buffer allocation. */
    allocation_failure = 2;
    DT_Status st = dt_localconn_ping (conn, 42, &error);
    allocation_failure = 0;
    CHECK (st == DT_ERR_NO_MEMORY);
    CHECK (dt_localconn_ping (conn, 42, &error) == DT_OK);
    dt_localconn_close (conn);
    close (fd[1]);
  }
}
#endif

int
main (void)
{
  unsigned char wire[64];
  char long_reply[300];
  crc_first_use ();
  size_t size = make_frame (wire, sizeof wire);
  fragmented_receive (wire, size, 0);
  fragmented_receive (wire, size, 1);
  truncated_receive (wire, size);
  backpressure ();
  cancel_and_closed_peer ();
  receive_deadline (wire, size);
#ifdef TS_TEST_ALLOC
  allocation_retries (wire, size);
#endif
  handshake ("DT/1.0 OK\n", 10, DT_OK);
  handshake ("DT/1.0 OK\r\n", 11, DT_OK);
  handshake ("DT/1.0 DENIED\n", 14, DT_ERR_AUTH);
  handshake ("DT/1.0 VERSION-MISMATCH\n", 24, DT_ERR_PROTOCOL);
  handshake ("DT/1.0 OKAY\n", 12, DT_ERR_PROTOCOL);
  handshake ("DT/1.0 OK\rignored\n", 18, DT_ERR_PROTOCOL);
  handshake ("DT/1.0 OK", 9, DT_ERR_PROTOCOL);
  handshake ("DT/1.0 OK\0ignored\n", 18, DT_ERR_PROTOCOL);
  memset (long_reply, 'x', sizeof long_reply);
  memcpy (long_reply, "DT/1.0 OK ", 10);
  long_reply[sizeof long_reply - 1] = '\n';
  handshake (long_reply, sizeof long_reply, DT_ERR_LIMIT);
  {
    char long_token[997];
    memset (long_token, 'a', sizeof long_token - 1);
    long_token[sizeof long_token - 1] = '\0';
    const char *bad_tokens[]
        = { "one\ntwo", "one\rtwo", "one two", "\t", "\177", long_token };
    for (size_t i = 0; i < sizeof bad_tokens / sizeof *bad_tokens; i++)
      {
        DT_RemoteConnOptions opts;
        DT_Error error;
        dt_remoteconn_options_init (&opts);
        opts.host = "127.0.0.1";
        opts.port = 1;
        opts.auth_token = bad_tokens[i];
        DT_RemoteConn *conn = dt_remoteconn_open (&opts, &error);
        CHECK (conn == NULL);
        CHECK (error.code == DT_ERR_INVALID_ARGUMENT);
        dt_remoteconn_close (conn);
      }
    DT_RemoteConnOptions opts;
    DT_Error error;
    dt_remoteconn_options_init (&opts);
    opts.host = "127.0.0.1";
    opts.port = 1;
    opts.connect_timeout_ms = UINT_MAX;
    DT_RemoteConn *conn = dt_remoteconn_open (&opts, &error);
    CHECK (conn == NULL && error.code == DT_ERR_INVALID_ARGUMENT);
    dt_remoteconn_close (conn);
  }
  return failures ? 1 : 0;
}
