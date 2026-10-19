/* dt_util.c -- CRC32, poll and exact-transfer stream helpers. */
#include "dt_internal.h"
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>

static int64_t
dt_monotonic_ms (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t
dt_timeout_deadline (int timeout_ms)
{
  return timeout_ms > 0 ? dt_monotonic_ms () + timeout_ms : 0;
}

int
dt_timeout_remaining (int timeout_ms, int64_t deadline)
{
  int64_t left;
  if (timeout_ms <= 0)
    return timeout_ms;
  left = deadline - dt_monotonic_ms ();
  return left > 0 ? (int)left : 0;
}

static uint32_t dt_crc_table[256];
static pthread_once_t dt_crc_once = PTHREAD_ONCE_INIT;

static void
dt_crc_init (void)
{
  uint32_t i, j, c;
  for (i = 0; i < 256; i++)
    {
      c = i;
      for (j = 0; j < 8; j++)
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      dt_crc_table[i] = c;
    }
}

uint32_t
dt_crc32_update (uint32_t crc, const void *data, size_t len)
{
  const uint8_t *p = (const uint8_t *)data;
  size_t i;
  pthread_once (&dt_crc_once, dt_crc_init);
  for (i = 0; i < len; i++)
    crc = dt_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

uint32_t
dt_crc32 (const void *data, size_t len)
{
  return dt_crc32_update (0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

int
dt_poll_wait (int fd, short events, int timeout_ms)
{
  struct pollfd pfd;
  int r;
  int64_t deadline = dt_timeout_deadline (timeout_ms);
  if (fd < 0)
    {
      errno = EBADF;
      return -1;
    }
  pfd.fd = fd;
  pfd.events = events;
  pfd.revents = 0;
  for (;;)
    {
      r = poll (&pfd, 1, dt_timeout_remaining (timeout_ms, deadline));
      if (r < 0 && errno == EINTR)
        {
          if (timeout_ms > 0 && !dt_timeout_remaining (timeout_ms, deadline))
            return 0;
          continue;
        }
      return r; /* 1 ready, 0 timeout, -1 error. */
    }
}

static int
dt_cancel_check (const atomic_int *cancelled)
{
  return cancelled && atomic_load (cancelled);
}

static int
dt_transfer_wait (int fd, short events, int timeout_ms, int64_t deadline,
                  const atomic_int *cancelled, int *errn)
{
  for (;;)
    {
      int wait_ms, ready;
      if (dt_cancel_check (cancelled))
        return DT_ERR_CANCELLED;
      wait_ms = dt_timeout_remaining (timeout_ms, deadline);
      if (timeout_ms > 0 && wait_ms == 0)
        return DT_ERR_TIMEOUT;
      /* shutdown wakes sockets; bounded polling also wakes pipe readers. */
      if (cancelled && (wait_ms < 0 || wait_ms > 100))
        wait_ms = 100;
      ready = dt_poll_wait (fd, events, wait_ms);
      if (ready > 0)
        return 0;
      if (ready < 0)
        {
          if (errn)
            *errn = errno;
          return DT_ERR_SYSTEM;
        }
      if (timeout_ms == 0)
        return DT_ERR_WOULD_BLOCK;
    }
}

int
dt_stream_read_full (int fd, void *buf, size_t len, int timeout_ms,
                     const atomic_int *cancelled, int *errn)
{
  size_t got = 0;
  return dt_stream_read_progress (fd, buf, len, &got, timeout_ms, cancelled,
                                  errn);
}

int
dt_stream_read_progress (int fd, void *buf, size_t len, size_t *got,
                         int timeout_ms, const atomic_int *cancelled,
                         int *errn)
{
  uint8_t *p = (uint8_t *)buf;
  int64_t deadline = dt_timeout_deadline (timeout_ms);
  if (errn)
    *errn = 0;
  if (len == 0)
    return 0;
  while (*got < len)
    {
      ssize_t r;
      int ready = dt_transfer_wait (fd, POLLIN, timeout_ms, deadline,
                                    cancelled, errn);
      if (ready != 0)
        return ready;
      if (dt_cancel_check (cancelled))
        return DT_ERR_CANCELLED;
      r = read (fd, p + *got, len - *got);
      if (r < 0)
        {
          if (errno == EINTR)
            continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              if (timeout_ms == 0)
                return DT_ERR_WOULD_BLOCK;
              continue; /* poll again under the timeout budget. */
            }
          if (errn)
            *errn = errno;
          return DT_ERR_SYSTEM;
        }
      if (r == 0)
        return DT_ERR_EOF;
      *got += (size_t)r;
    }
  return 0;
}

int
dt_stream_write_full (int fd, const void *buf, size_t len, int timeout_ms,
                      const atomic_int *cancelled, int *errn)
{
  size_t done = 0;
  return dt_stream_write_progress (fd, buf, len, &done, timeout_ms, cancelled,
                                   errn);
}

int
dt_stream_write_progress (int fd, const void *buf, size_t len, size_t *done,
                          int timeout_ms, const atomic_int *cancelled,
                          int *errn)
{
  const uint8_t *p = (const uint8_t *)buf;
  int64_t deadline = dt_timeout_deadline (timeout_ms);
  if (errn)
    *errn = 0;
  if (len == 0)
    return 0;
  while (*done < len)
    {
      ssize_t r;
      int ready = dt_transfer_wait (fd, POLLOUT, timeout_ms, deadline,
                                    cancelled, errn);
      if (ready != 0)
        return ready;
      if (dt_cancel_check (cancelled))
        return DT_ERR_CANCELLED;
#ifdef MSG_NOSIGNAL
      r = send (fd, p + *done, len - *done, MSG_NOSIGNAL | MSG_DONTWAIT);
      if (r < 0 && errno == ENOTSOCK)
#endif
        r = write (fd, p + *done, len - *done);
      if (r < 0)
        {
          if (errno == EINTR)
            continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              if (timeout_ms == 0)
                return DT_ERR_WOULD_BLOCK;
              continue;
            }
          if (errno == EPIPE)
            return DT_ERR_EOF;
          if (errn)
            *errn = errno;
          return DT_ERR_SYSTEM;
        }
      if (r == 0)
        return DT_ERR_EOF;
      *done += (size_t)r;
    }
  return 0;
}

DT_Status
dt_stream_read_line (int fd, char *buf, size_t cap, int timeout_ms,
                     const atomic_int *cancelled, DT_Error *error)
{
  size_t n = 0;
  int64_t deadline = dt_timeout_deadline (timeout_ms);
  if (!buf || cap == 0)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad line buffer");
      return DT_ERR_INVALID_ARGUMENT;
    }
  buf[0] = '\0';
  while (n + 1 < cap)
    {
      char c;
      int rc, errn = 0;
      rc = dt_stream_read_full (fd, &c, 1,
                                dt_timeout_remaining (timeout_ms, deadline),
                                cancelled, &errn);
      if (rc == DT_ERR_WOULD_BLOCK && timeout_ms > 0)
        rc = DT_ERR_TIMEOUT;
      if (rc == DT_ERR_EOF)
        {
          if (n == 0)
            {
              dt_err_set (error, DT_ERR_EOF, 0, 0, "eof in handshake");
              return DT_ERR_EOF;
            }
          dt_err_set (error, DT_ERR_PROTOCOL, 0, n,
                      "unterminated handshake reply");
          return DT_ERR_PROTOCOL;
        }
      if (rc != 0)
        {
          if (rc == DT_ERR_TIMEOUT)
            dt_err_set (error, DT_ERR_TIMEOUT, 0, 0, "handshake timeout");
          else if (rc == DT_ERR_CANCELLED)
            dt_err_set (error, DT_ERR_CANCELLED, 0, 0, "cancelled");
          else
            dt_err_set (error, DT_ERR_SYSTEM, errn, 0,
                        "handshake read failed");
          return (DT_Status)rc;
        }
      if (c == '\0')
        {
          dt_err_set (error, DT_ERR_PROTOCOL, 0, n, "NUL in handshake reply");
          return DT_ERR_PROTOCOL;
        }
      buf[n++] = c;
      buf[n] = '\0';
      if (c == '\n')
        return DT_OK;
    }
  dt_err_set (error, DT_ERR_LIMIT, 0, n, "handshake reply too long");
  return DT_ERR_LIMIT;
}
