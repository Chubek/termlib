/* IPv4 UDP, nonblocking by default, text payloads <=65507 bytes.
 * No authentication, encryption or reliability is implied by this transport.
 * Addresses are numeric; no hidden DNS lookups. fd() borrows the owned socket.
 */
#include "datagram/datagram.h"
#include "common/ts_std_common.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
typedef struct
{
  int fd;
} datagram;
static const char tag;
static void
destroy (void *p)
{
  datagram *d = p;
  if (d->fd >= 0)
    close (d->fd);
  free (d);
}
static TS_Status
io_error (TS_Error *e, const char *operation)
{
  if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
      ts_error_set (e, TS_ERR_WOULD_BLOCK, errno, 0, "%s would block",
                    operation);
      return TS_ERR_WOULD_BLOCK;
    }
  return ts_std_system (e, operation);
}
static bool
address (const char *text, int64_t port, struct sockaddr_in *out)
{
  memset (out, 0, sizeof *out);
  out->sin_family = AF_INET;
  if (port < 0 || port > 65535
      || inet_pton (AF_INET, text, &out->sin_addr) != 1)
    return false;
  out->sin_port = htons ((uint16_t)port);
  return true;
}
static TS_Status
bind_socket (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r,
             TS_Error *e)
{
  (void)u;
  int64_t port;
  const char *host = "127.0.0.1";
  struct sockaddr_in addr;
  if (ts_std_argc (vm, n, 1, 2, e, "bind") || ts_std_int (a, &port, e, "port")
      || (n == 2 && ts_std_str (a + 1, &host, e, "address")))
    return TS_ERR_INVAL;
  if (!address (host, port, &addr))
    return ts_std_inval (e, "invalid numeric IPv4 address/port");
  datagram *d = malloc (sizeof *d);
  if (!d)
    return ts_std_oom (e);
  d->fd = socket (AF_INET, SOCK_DGRAM, 0);
  if (d->fd < 0)
    {
      free (d);
      return ts_std_system (e, "UDP socket");
    }
  if (fcntl (d->fd, F_SETFD, FD_CLOEXEC) < 0
      || fcntl (d->fd, F_SETFL, O_NONBLOCK) < 0
      || bind (d->fd, (struct sockaddr *)&addr, sizeof addr) < 0)
    {
      int saved = errno;
      destroy (d);
      errno = saved;
      return ts_std_system (e, "UDP bind");
    }
  if (ts_value_make_handle (r, d, destroy, "datagram", &tag))
    {
      destroy (d);
      return ts_std_oom (e);
    }
  return TS_OK;
}
static TS_Status
operation (TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r,
           TS_Error *e)
{
  intptr_t op = (intptr_t)u;
  size_t lo = op == 3 ? 4 : 1, hi = op == 4 ? 2 : lo;
  if (ts_std_argc (vm, n, lo, hi, e, "datagram"))
    return TS_ERR_INVAL;
  datagram *d = ts_std_handle (a, &tag, e, "datagram");
  if (!d)
    return TS_ERR_INVAL;
  if (op == 0)
    {
      if (d->fd >= 0)
        close (d->fd);
      d->fd = -1;
      return TS_OK;
    }
  if (d->fd < 0)
    return ts_std_inval (e, "datagram is closed");
  if (op == 1)
    {
      ts_std_ret_int (r, d->fd);
      return TS_OK;
    }
  if (op == 2)
    {
      struct sockaddr_in addr;
      socklen_t size = sizeof addr;
      if (getsockname (d->fd, (struct sockaddr *)&addr, &size) < 0)
        return io_error (e, "UDP getsockname");
      ts_std_ret_int (r, ntohs (addr.sin_port));
      return TS_OK;
    }
  if (op == 3)
    {
      const char *host, *text;
      int64_t port;
      struct sockaddr_in addr;
      if (ts_std_str (a + 1, &host, e, "address")
          || ts_std_int (a + 2, &port, e, "port")
          || ts_std_str (a + 3, &text, e, "data"))
        return TS_ERR_INVAL;
      if (!address (host, port, &addr))
        return ts_std_inval (e, "invalid numeric IPv4 address/port");
      size_t len = strlen (text);
      if (len > 65507)
        {
          ts_error_set (e, TS_ERR_LIMIT, 0, 0,
                        "UDP payload exceeds 65507 bytes");
          return TS_ERR_LIMIT;
        }
      ssize_t sent;
      do
        {
          sent = sendto (d->fd, text, len, 0, (struct sockaddr *)&addr,
                         sizeof addr);
        }
      while (sent < 0 && errno == EINTR);
      if (sent < 0)
        return io_error (e, "UDP send");
      ts_std_ret_int (r, sent);
      return TS_OK;
    }
  int64_t limit = 65507;
  if ((n == 2 && ts_std_int (a + 1, &limit, e, "limit")) || limit < 1
      || limit > 65507)
    return ts_std_inval (e, "UDP receive limit must be in [1, 65507]");
  char *buffer = malloc ((size_t)limit + 1);
  if (!buffer)
    return ts_std_oom (e);
  struct sockaddr_in peer;
  struct iovec vec = { buffer, (size_t)limit };
  struct msghdr msg = { 0 };
  msg.msg_name = &peer;
  msg.msg_namelen = sizeof peer;
  msg.msg_iov = &vec;
  msg.msg_iovlen = 1;
  ssize_t received;
  do
    {
      received = recvmsg (d->fd, &msg, 0);
    }
  while (received < 0 && errno == EINTR);
  if (received < 0)
    {
      int saved = errno;
      free (buffer);
      errno = saved;
      return io_error (e, "UDP receive");
    }
  if (msg.msg_flags & MSG_TRUNC)
    {
      free (buffer);
      ts_error_set (e, TS_ERR_LIMIT, 0, 0,
                    "UDP datagram truncated (consumed)");
      return TS_ERR_LIMIT;
    }
  if (memchr (buffer, 0, (size_t)received))
    {
      free (buffer);
      ts_error_set (e, TS_ERR_UNSUPPORTED, 0, 0,
                    "binary UDP payload contains NUL (consumed)");
      return TS_ERR_UNSUPPORTED;
    }
  buffer[received] = 0;
  char host[INET_ADDRSTRLEN];
  if (!inet_ntop (AF_INET, &peer.sin_addr, host, sizeof host))
    {
      free (buffer);
      return io_error (e, "UDP peer address");
    }
  ts_std_list_t *list = ts_std_list_new ();
  if (!list)
    {
      free (buffer);
      return ts_std_oom (e);
    }
  TS_Status st = ts_std_list_push_str (list, buffer);
  free (buffer);
  if (st == TS_OK)
    st = ts_std_list_push_str (list, host);
  if (st == TS_OK)
    st = ts_std_list_push_int (list, ntohs (peer.sin_port));
  if (st != TS_OK)
    {
      ts_std_list_free (list);
      return ts_std_oom (e);
    }
  return ts_std_list_wrap (r, list, e);
}
static const TS_FuncDef functions[] = {
  { "bind", bind_socket, NULL },       { "close", operation, (void *)0 },
  { "fd", operation, (void *)1 },      { "port", operation, (void *)2 },
  { "send_to", operation, (void *)3 }, { "recv", operation, (void *)4 }
};
const TS_Module ts_std_datagram_module
    = TS_MODULE_INIT ("std.datagram", functions);
