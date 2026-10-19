/* dt_conn.c -- transport-neutral messages and local/remote connections. */
#include "dt_internal.h"

#include <arpa/inet.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

/* ---------------- messages and envelopes ---------------- */

struct DT_Message
{
  DT_MessageType type;
  uint8_t *data;
  size_t len;
  unsigned short rows, columns;
  int err_code;
};

struct DT_Envelope
{
  uint64_t id;
  DT_Message *msg;
};

const char *
dt_message_type_string (DT_MessageType type)
{
  switch (type)
    {
    case DT_MESSAGE_DATA:
      return "DATA";
    case DT_MESSAGE_RESIZE:
      return "RESIZE";
    case DT_MESSAGE_CLOSE:
      return "CLOSE";
    case DT_MESSAGE_ERROR:
      return "ERROR";
    case DT_MESSAGE_HEARTBEAT:
      return "HEARTBEAT";
    case DT_MESSAGE_AUTH:
      return "AUTH";
    default:
      return "UNKNOWN";
    }
}

const char *
dt_conn_state_string (DT_ConnState state)
{
  switch (state)
    {
    case DT_CONN_CLOSED:
      return "CLOSED";
    case DT_CONN_CONNECTING:
      return "CONNECTING";
    case DT_CONN_OPEN:
      return "OPEN";
    case DT_CONN_SHUTDOWN:
      return "SHUTDOWN";
    default:
      return "UNKNOWN";
    }
}

static DT_Message *
dt_message_new (DT_MessageType type, const void *data, size_t size,
                DT_Error *error)
{
  DT_Message *m;
  if ((unsigned)type > (unsigned)DT_MESSAGE_AUTH)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad message type");
      return NULL;
    }
  if (size > DT_CONN_MAX_MESSAGE)
    {
      dt_err_set (error, DT_ERR_LIMIT, 0, 0, "message too large");
      return NULL;
    }
  if (size && !data)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL payload");
      return NULL;
    }
  m = calloc (1, sizeof *m);
  if (!m)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return NULL;
    }
  m->type = type;
  if (size)
    {
      m->data = malloc (size);
      if (!m->data)
        {
          free (m);
          dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
          return NULL;
        }
      memcpy (m->data, data, size);
      m->len = size;
    }
  return m;
}

DT_Message *
dt_message_create (DT_MessageType type, const void *data, size_t size,
                   DT_Error *error)
{
  if (type == DT_MESSAGE_RESIZE)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0,
                  "use dt_message_create_resize");
      return NULL;
    }
  return dt_message_new (type, data, size, error);
}

DT_Message *
dt_message_create_resize (unsigned short rows, unsigned short columns,
                          DT_Error *error)
{
  DT_Message *m = dt_message_new (DT_MESSAGE_RESIZE, NULL, 0, error);
  if (m)
    {
      m->rows = rows;
      m->columns = columns;
    }
  return m;
}

DT_Message *
dt_message_create_heartbeat (DT_Error *error)
{
  return dt_message_new (DT_MESSAGE_HEARTBEAT, NULL, 0, error);
}

DT_Message *
dt_message_create_error_msg (int code, const char *text, DT_Error *error)
{
  DT_Message *m;
  if (!text)
    text = "";
  m = dt_message_new (DT_MESSAGE_ERROR, text, strlen (text), error);
  if (m)
    m->err_code = code;
  return m;
}

void
dt_message_free (DT_Message *message)
{
  if (!message)
    return;
  free (message->data);
  free (message);
}

DT_MessageType
dt_message_type (const DT_Message *message)
{
  return message ? message->type : DT_MESSAGE_ERROR;
}

const uint8_t *
dt_message_data (const DT_Message *message, size_t *size)
{
  if (size)
    *size = 0;
  if (!message)
    return NULL;
  if (size)
    *size = message->len;
  return message->data;
}

DT_Status
dt_message_resize (const DT_Message *message, unsigned short *rows,
                   unsigned short *columns)
{
  if (!message || message->type != DT_MESSAGE_RESIZE)
    return DT_ERR_INVALID_ARGUMENT;
  if (rows)
    *rows = message->rows;
  if (columns)
    *columns = message->columns;
  return DT_OK;
}

DT_Envelope *
dt_envelope_create (uint64_t message_id, DT_Message *message, DT_Error *error)
{
  DT_Envelope *e;
  if (!message)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL message");
      return NULL;
    }
  e = malloc (sizeof *e);
  if (!e)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return NULL;
    }
  e->id = message_id;
  e->msg = message;
  return e;
}

void
dt_envelope_free (DT_Envelope *envelope)
{
  if (!envelope)
    return;
  dt_message_free (envelope->msg);
  free (envelope);
}

uint64_t
dt_envelope_message_id (const DT_Envelope *envelope)
{
  return envelope ? envelope->id : 0;
}

DT_Message *
dt_envelope_message (const DT_Envelope *envelope)
{
  return envelope ? envelope->msg : NULL;
}

DT_Message *
dt_envelope_take_message (DT_Envelope *envelope)
{
  DT_Message *m;
  if (!envelope)
    return NULL;
  m = envelope->msg;
  envelope->msg = NULL;
  free (envelope);
  return m;
}

/* ---------------- framing ---------------- */

#define DT_FRAME_OVERHEAD (8u + 1u + 4u) /* msgid + type + crc. */

typedef struct
{
  uint8_t *data;
  size_t size, sent;
} DT_FrameWriter;

static void
dt_frame_writer_reset (DT_FrameWriter *writer)
{
  free (writer->data);
  memset (writer, 0, sizeof *writer);
}

static DT_Status
dt_frame_send (int fd, DT_FrameWriter *writer, uint64_t id,
               const DT_Message *msg, int timeout_ms,
               const atomic_int *cancelled, DT_Error *error)
{
  uint8_t hdr[4 + 8 + 1];
  uint8_t crcbuf[4];
  uint8_t resize_payload[4];
  const uint8_t *payload = msg->data;
  size_t payload_len = msg->len;
  uint32_t body_len, crc;
  int rc, errn = 0;
  if (msg->type == DT_MESSAGE_RESIZE)
    {
      dt_put_u16le (resize_payload, msg->rows);
      dt_put_u16le (resize_payload + 2, msg->columns);
      payload = resize_payload;
      payload_len = 4;
    }
  if (payload_len > DT_CONN_MAX_MESSAGE)
    {
      dt_err_set (error, DT_ERR_LIMIT, 0, 0, "message too large");
      return DT_ERR_LIMIT;
    }
  body_len = (uint32_t)(8 + 1 + payload_len + 4);
  dt_put_u32be (hdr, body_len);
  dt_put_u64be (hdr + 4, id);
  hdr[12] = (uint8_t)msg->type;
  crc = dt_crc32_update (0xFFFFFFFFu, hdr + 4, 9);
  if (payload_len)
    crc = dt_crc32_update (crc, payload, payload_len);
  crc ^= 0xFFFFFFFFu;
  dt_put_u32be (crcbuf, crc);
  if (writer->data)
    {
      if (writer->size != sizeof hdr + payload_len + 4
          || memcmp (writer->data, hdr, sizeof hdr)
          || (payload_len
              && memcmp (writer->data + sizeof hdr, payload, payload_len)))
        {
          dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0,
                      "retry the pending envelope before sending another");
          return DT_ERR_INVALID_ARGUMENT;
        }
    }
  else
    {
      writer->size = sizeof hdr + payload_len + 4;
      writer->data = malloc (writer->size);
      if (!writer->data)
        {
          dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
          return DT_ERR_NO_MEMORY;
        }
      memcpy (writer->data, hdr, sizeof hdr);
      if (payload_len)
        memcpy (writer->data + sizeof hdr, payload, payload_len);
      memcpy (writer->data + sizeof hdr + payload_len, crcbuf, 4);
    }
  rc = dt_stream_write_progress (fd, writer->data, writer->size, &writer->sent,
                                 timeout_ms, cancelled, &errn);
  if (rc != 0)
    {
      if (rc == DT_ERR_EOF)
        dt_err_set (error, DT_ERR_EOF, 0, 0, "peer closed connection");
      else if (rc == DT_ERR_TIMEOUT)
        dt_err_set (error, DT_ERR_TIMEOUT, 0, 0, "send timeout");
      else if (rc == DT_ERR_CANCELLED)
        dt_err_set (error, DT_ERR_CANCELLED, 0, 0, "send cancelled");
      else if (rc == DT_ERR_WOULD_BLOCK)
        dt_err_set (error, DT_ERR_WOULD_BLOCK, 0, 0, "send would block");
      else
        dt_err_set (error, DT_ERR_SYSTEM, errn, 0, "send failed");
      return (DT_Status)rc;
    }
  dt_frame_writer_reset (writer);
  return DT_OK;
}

/* A connection retains its partial frame across timeout/would-block returns.
 */
typedef struct
{
  uint8_t header[4];
  size_t header_got;
  uint8_t *body;
  size_t body_got;
} DT_FrameReader;

static void
dt_frame_reset (DT_FrameReader *reader)
{
  free (reader->body);
  memset (reader, 0, sizeof *reader);
}

static DT_Status
dt_frame_receive (int fd, DT_FrameReader *reader, uint64_t *id,
                  DT_Message **msg, int timeout_ms,
                  const atomic_int *cancelled, DT_Error *error)
{
  uint32_t body_len, payload_len;
  int rc, errn = 0;
  unsigned mtype;
  DT_Message *m;
  int64_t deadline = dt_timeout_deadline (timeout_ms);
  rc = dt_stream_read_progress (fd, reader->header, sizeof reader->header,
                                &reader->header_got, timeout_ms, cancelled,
                                &errn);
  if (rc != 0)
    goto read_error;
  body_len = dt_get_u32be (reader->header);
  if (body_len < DT_FRAME_OVERHEAD
      || body_len > DT_FRAME_OVERHEAD + DT_CONN_MAX_MESSAGE)
    {
      DT_Status st
          = body_len < DT_FRAME_OVERHEAD ? DT_ERR_PROTOCOL : DT_ERR_LIMIT;
      dt_err_set (error, st, 0, 0, "bad frame length %u", body_len);
      return st;
    }
  if (!reader->body)
    {
      reader->body = malloc (body_len);
      if (!reader->body)
        {
          dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
          return DT_ERR_NO_MEMORY;
        }
    }
  rc = dt_stream_read_progress (fd, reader->body, body_len, &reader->body_got,
                                dt_timeout_remaining (timeout_ms, deadline),
                                cancelled, &errn);
  if (rc == DT_ERR_WOULD_BLOCK && timeout_ms > 0)
    rc = DT_ERR_TIMEOUT;
  if (rc != 0)
    goto read_error;
  payload_len = body_len - DT_FRAME_OVERHEAD;
  mtype = reader->body[8];
  if (mtype > (unsigned)DT_MESSAGE_AUTH)
    {
      dt_err_set (error, DT_ERR_PROTOCOL, 0, 0, "unsupported message type %u",
                  mtype);
      dt_frame_reset (reader);
      return DT_ERR_PROTOCOL;
    }
  if (dt_crc32 (reader->body, body_len - 4)
      != dt_get_u32be (reader->body + body_len - 4))
    {
      dt_err_set (error, DT_ERR_PROTOCOL, 0, 0, "frame checksum mismatch");
      dt_frame_reset (reader);
      return DT_ERR_PROTOCOL;
    }
  if (mtype == DT_MESSAGE_RESIZE && payload_len != 4)
    {
      dt_err_set (error, DT_ERR_PROTOCOL, 0, 0, "bad resize frame");
      dt_frame_reset (reader);
      return DT_ERR_PROTOCOL;
    }
  m = calloc (1, sizeof *m);
  if (!m)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return DT_ERR_NO_MEMORY;
    }
  *id = dt_get_u64be (reader->body);
  m->type = (DT_MessageType)mtype;
  if (mtype == DT_MESSAGE_RESIZE)
    {
      m->rows = dt_get_u16le (reader->body + 9);
      m->columns = dt_get_u16le (reader->body + 11);
    }
  else if (payload_len)
    {
      memmove (reader->body, reader->body + 9, payload_len);
      m->data = reader->body;
      m->len = payload_len;
      reader->body = NULL; /* Transfer the allocation to the message. */
    }
  dt_frame_reset (reader);
  *msg = m;
  return DT_OK;

read_error:
  if (rc == DT_ERR_EOF && reader->header_got)
    rc = DT_ERR_PROTOCOL;
  dt_err_set (error, (DT_Status)rc, errn, 0, "%s",
              rc == DT_ERR_PROTOCOL      ? "truncated frame"
              : rc == DT_ERR_EOF         ? "peer closed connection"
              : rc == DT_ERR_TIMEOUT     ? "receive timeout"
              : rc == DT_ERR_CANCELLED   ? "receive cancelled"
              : rc == DT_ERR_WOULD_BLOCK ? "no complete message ready"
                                         : "receive failed");
  return (DT_Status)rc;
}

/* ---------------- local connections ---------------- */

struct DT_LocalConn
{
  DT_FrameReader reader;
  DT_FrameWriter writer;
  int fd;
  bool own;
  int rto_ms;
  int wto_ms;
  atomic_int cancelled;
  DT_ConnState state;
};

void
dt_localconn_options_init (DT_LocalConnOptions *options)
{
  if (!options)
    return;
  options->struct_size = (uint32_t)sizeof *options;
  options->fd = -1;
  options->take_ownership = false;
  options->nonblocking = false;
  options->read_timeout_ms = -1;
  options->write_timeout_ms = -1;
  options->reserved = 0;
}

DT_LocalConn *
dt_localconn_open (const DT_LocalConnOptions *options, DT_Error *error)
{
  DT_LocalConn *c;
  if (!options || options->fd < 0)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0,
                  "invalid local-conn options/fd");
      return NULL;
    }
  c = calloc (1, sizeof *c);
  if (!c)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return NULL;
    }
  c->fd = options->fd;
  atomic_init (&c->cancelled, 0);
  c->own = options->take_ownership;
  c->rto_ms = options->read_timeout_ms;
  c->wto_ms = options->write_timeout_ms;
  c->state = DT_CONN_OPEN;
  if (options->nonblocking)
    {
      int flags = fcntl (c->fd, F_GETFL);
      if (flags < 0 || fcntl (c->fd, F_SETFL, flags | O_NONBLOCK) < 0)
        {
          int e = errno;
          free (c);
          dt_err_set (error, DT_ERR_SYSTEM, e, 0, "O_NONBLOCK failed");
          return NULL;
        }
      if (c->rto_ms < 0)
        c->rto_ms = 0;
      if (c->wto_ms < 0)
        c->wto_ms = 0;
    }
  return c;
}

void
dt_localconn_close (DT_LocalConn *connection)
{
  if (!connection)
    return;
  connection->state = DT_CONN_CLOSED;
  dt_frame_reset (&connection->reader);
  dt_frame_writer_reset (&connection->writer);
  if (connection->own)
    close (connection->fd);
  free (connection);
}

int
dt_localconn_fd (const DT_LocalConn *connection)
{
  return connection ? connection->fd : -1;
}

DT_ConnState
dt_localconn_state (const DT_LocalConn *connection)
{
  return connection ? connection->state : DT_CONN_CLOSED;
}

void
dt_localconn_cancel (DT_LocalConn *connection)
{
  if (!connection)
    return;
  atomic_store (&connection->cancelled, 1);
  shutdown (connection->fd, SHUT_RDWR); /* Wake blocked poll/recv. */
}

DT_Status
dt_localconn_send (DT_LocalConn *connection, const DT_Envelope *envelope,
                   DT_Error *error)
{
  DT_Status st;
  if (!connection || !envelope || !envelope->msg)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (connection->state != DT_CONN_OPEN)
    {
      dt_err_set (error, DT_ERR_IO, 0, 0, "connection is not open");
      return DT_ERR_IO;
    }
  st = dt_frame_send (connection->fd, &connection->writer, envelope->id,
                      envelope->msg, connection->wto_ms,
                      &connection->cancelled, error);
  if (st == DT_OK && envelope->msg->type == DT_MESSAGE_CLOSE)
    connection->state = DT_CONN_SHUTDOWN;
  if (st == DT_ERR_EOF)
    connection->state = DT_CONN_SHUTDOWN;
  return st;
}

DT_Status
dt_localconn_receive (DT_LocalConn *connection, DT_Envelope **envelope,
                      DT_Error *error)
{
  DT_Status st;
  DT_Envelope *e;
  uint64_t id = 0;
  DT_Message *msg = NULL;
  if (!connection || !envelope)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  *envelope = NULL;
  if (connection->state != DT_CONN_OPEN)
    {
      dt_err_set (error, DT_ERR_IO, 0, 0, "connection is not open");
      return DT_ERR_IO;
    }
  /* Allocate the envelope before consuming the frame, so OOM is retryable. */
  e = malloc (sizeof *e);
  if (!e)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return DT_ERR_NO_MEMORY;
    }
  st = dt_frame_receive (connection->fd, &connection->reader, &id, &msg,
                         connection->rto_ms, &connection->cancelled, error);
  if (st != DT_OK)
    {
      free (e);
      if (st == DT_ERR_EOF || st == DT_ERR_LIMIT
          || (st == DT_ERR_PROTOCOL && connection->reader.header_got))
        {
          connection->state = DT_CONN_SHUTDOWN;
          dt_frame_reset (&connection->reader);
        }
      return st;
    }
  e->id = id;
  e->msg = msg;
  if (msg->type == DT_MESSAGE_CLOSE)
    connection->state = DT_CONN_SHUTDOWN;
  *envelope = e;
  return DT_OK;
}

DT_Status
dt_localconn_ping (DT_LocalConn *connection, uint64_t message_id,
                   DT_Error *error)
{
  DT_Message *m;
  DT_Envelope env;
  DT_Status st;
  if (!connection)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL connection");
      return DT_ERR_INVALID_ARGUMENT;
    }
  m = dt_message_create_heartbeat (error);
  if (!m)
    return error ? error->code : DT_ERR_NO_MEMORY;
  env.id = message_id;
  env.msg = m;
  st = dt_localconn_send (connection, &env, error);
  dt_message_free (m);
  return st;
}

/* ---------------- remote connections ---------------- */

struct DT_RemoteConn
{
  DT_FrameReader reader;
  DT_FrameWriter writer;
  int fd;
  int rto_ms;
  int wto_ms;
  atomic_int cancelled;
  DT_ConnState state;
  char *host;
  uint16_t port;
};

void
dt_remoteconn_options_init (DT_RemoteConnOptions *options)
{
  if (!options)
    return;
  options->struct_size = (uint32_t)sizeof *options;
  options->host = NULL;
  options->port = 0;
  options->connect_timeout_ms = 5000;
  options->auth_token = NULL;
  options->read_timeout_ms = -1;
  options->write_timeout_ms = -1;
  options->keepalive_ms = 0;
  options->reserved = 0;
}

static int
dt_connect_poll (int fd, const struct sockaddr *addr, socklen_t addrlen,
                 unsigned int timeout_ms, const atomic_int *cancelled)
{
  int flags;
  int timeout = timeout_ms ? (int)timeout_ms : -1;
  int64_t deadline = dt_timeout_deadline (timeout);
  flags = fcntl (fd, F_GETFL);
  if (flags < 0)
    return -1;
  if (fcntl (fd, F_SETFL, flags | O_NONBLOCK) < 0)
    return -1;
  if (connect (fd, addr, addrlen) == 0)
    {
      return fcntl (fd, F_SETFL, flags);
    }
  if (errno != EINPROGRESS)
    return -1;
  for (;;)
    {
      struct pollfd pfd;
      int slice, r;
      socklen_t optlen;
      int soerr = 0;
      if (cancelled && atomic_load (cancelled))
        {
          errno = ECANCELED;
          return -1;
        }
      slice = dt_timeout_remaining (timeout, deadline);
      if (slice == 0)
        {
          errno = ETIMEDOUT;
          return -1;
        }
      if (slice < 0 || slice > 100)
        slice = 100;
      pfd.fd = fd;
      pfd.events = POLLOUT;
      pfd.revents = 0;
      r = poll (&pfd, 1, slice);
      if (r < 0)
        {
          if (errno == EINTR)
            continue;
          return -1;
        }
      if (r == 0)
        {
          continue;
        }
      optlen = sizeof soerr;
      if (getsockopt (fd, SOL_SOCKET, SO_ERROR, &soerr, &optlen) < 0)
        return -1;
      if (soerr)
        {
          errno = soerr;
          return -1;
        }
      return fcntl (fd, F_SETFL, flags);
    }
}

DT_RemoteConn *
dt_remoteconn_open (const DT_RemoteConnOptions *options, DT_Error *error)
{
  DT_RemoteConn *c = NULL;
  struct addrinfo hints, *list = NULL, *ai;
  char portbuf[16];
  int fd = -1;
  int wto;
  if (!options || !options->host || !options->port
      || options->connect_timeout_ms > INT_MAX)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0,
                  "host, port and a connect timeout <= INT_MAX are required");
      return NULL;
    }
  /* Tokens are one printable ASCII field, never additional HELLO lines or
     fields. Validate before connecting so invalid credentials leave no side
     effects and cannot leak into diagnostics. */
  if (options->auth_token)
    {
      const unsigned char *p = (const unsigned char *)options->auth_token;
      size_t len = 0;
      while (*p)
        {
          if (*p <= 0x20 || *p >= 0x7f
              || ++len > 1024 - sizeof "DT/1.0 HELLO proto=1 token=\n")
            {
              dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0,
                          "invalid authentication token");
              return NULL;
            }
          p++;
        }
    }
  c = calloc (1, sizeof *c);
  if (!c)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return NULL;
    }
  c->fd = -1;
  atomic_init (&c->cancelled, 0);
  c->state = DT_CONN_CONNECTING;
  c->rto_ms = options->read_timeout_ms;
  c->wto_ms = options->write_timeout_ms;
  c->host = strdup (options->host);
  c->port = options->port;
  if (!c->host)
    {
      free (c);
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return NULL;
    }
  memset (&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  snprintf (portbuf, sizeof portbuf, "%u", options->port);
  if (getaddrinfo (options->host, portbuf, &hints, &list) != 0 || !list)
    {
      dt_err_set (error, DT_ERR_SYSTEM, 0, 0, "cannot resolve '%s'",
                  options->host);
      goto fail;
    }
  for (ai = list; ai; ai = ai->ai_next)
    {
      fd = socket (ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd < 0)
        continue;
      {
        int one = 1;
        setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
      }
      if (dt_connect_poll (fd, ai->ai_addr, ai->ai_addrlen,
                           options->connect_timeout_ms, &c->cancelled)
          == 0)
        break;
      {
        int e = errno;
        close (fd);
        fd = -1;
        if (e == ECANCELED)
          {
            freeaddrinfo (list);
            dt_err_set (error, DT_ERR_CANCELLED, 0, 0, "connect cancelled");
            goto fail;
          }
        if (!ai->ai_next)
          {
            freeaddrinfo (list);
            dt_err_set (error, e == ETIMEDOUT ? DT_ERR_TIMEOUT : DT_ERR_SYSTEM,
                        e, 0, "connect to %s:%u failed", options->host,
                        options->port);
            goto fail;
          }
      }
    }
  freeaddrinfo (list);
  if (fd < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, 0, 0, "connect failed");
      goto fail;
    }
  c->fd = fd;
  /* Handshake: DT/1.0 HELLO proto=1 [token=...]. */
  {
    char hello[1024];
    const char *token = options->auth_token;
    int hlen;
    char reply[256];
    DT_Status st;
    if (token)
      hlen = snprintf (hello, sizeof hello, "DT/1.0 HELLO proto=1 token=%s\n",
                       token);
    else
      hlen = snprintf (hello, sizeof hello, "DT/1.0 HELLO proto=1\n");
    if (hlen < 0 || (size_t)hlen >= sizeof hello)
      {
        dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "token too long");
        goto fail;
      }
    wto = options->connect_timeout_ms ? (int)options->connect_timeout_ms : -1;
    {
      int errn = 0;
      int rc = dt_stream_write_full (c->fd, hello, (size_t)hlen, wto,
                                     &c->cancelled, &errn);
      if (rc != 0)
        {
          dt_err_set (error, (DT_Status)rc, errn, 0, "handshake write failed");
          goto fail;
        }
    }
    st = dt_stream_read_line (c->fd, reply, sizeof reply, wto, &c->cancelled,
                              error);
    if (st != DT_OK)
      goto fail;
    /* read_line requires a complete, bounded, NUL-free line. */
    {
      size_t len = strlen (reply);
      reply[--len] = '\0'; /* Final newline is guaranteed by read_line. */
      if (len && reply[len - 1] == '\r')
        reply[len - 1] = '\0';
    }
    if (strcmp (reply, "DT/1.0 OK") == 0)
      {
        c->state = DT_CONN_OPEN;
        return c;
      }
    if (strcmp (reply, "DT/1.0 DENIED") == 0)
      {
        dt_err_set (error, DT_ERR_AUTH, 0, 0, "remote rejected credentials");
        goto fail;
      }
    if (strcmp (reply, "DT/1.0 VERSION-MISMATCH") == 0)
      {
        dt_err_set (error, DT_ERR_PROTOCOL, 0, 0,
                    "remote protocol version mismatch");
        goto fail;
      }
    dt_err_set (error, DT_ERR_PROTOCOL, 0, 0, "bad handshake reply");
    goto fail;
  }

fail:
  if (c)
    {
      if (c->fd >= 0)
        close (c->fd);
      free (c->host);
      free (c);
    }
  return NULL;
}

void
dt_remoteconn_close (DT_RemoteConn *connection)
{
  if (!connection)
    return;
  connection->state = DT_CONN_CLOSED;
  dt_frame_reset (&connection->reader);
  dt_frame_writer_reset (&connection->writer);
  if (connection->fd >= 0)
    close (connection->fd);
  free (connection->host);
  free (connection);
}

int
dt_remoteconn_fd (const DT_RemoteConn *connection)
{
  return connection ? connection->fd : -1;
}

DT_ConnState
dt_remoteconn_state (const DT_RemoteConn *connection)
{
  return connection ? connection->state : DT_CONN_CLOSED;
}

void
dt_remoteconn_cancel (DT_RemoteConn *connection)
{
  if (!connection)
    return;
  atomic_store (&connection->cancelled, 1);
  if (connection->fd >= 0)
    shutdown (connection->fd, SHUT_RDWR);
}

DT_Status
dt_remoteconn_send (DT_RemoteConn *connection, const DT_Envelope *envelope,
                    DT_Error *error)
{
  DT_Status st;
  if (!connection || !envelope || !envelope->msg)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (connection->state != DT_CONN_OPEN)
    {
      dt_err_set (error, DT_ERR_IO, 0, 0, "connection is not open");
      return DT_ERR_IO;
    }
  st = dt_frame_send (connection->fd, &connection->writer, envelope->id,
                      envelope->msg, connection->wto_ms,
                      &connection->cancelled, error);
  if (st == DT_OK && envelope->msg->type == DT_MESSAGE_CLOSE)
    connection->state = DT_CONN_SHUTDOWN;
  if (st == DT_ERR_EOF)
    connection->state = DT_CONN_SHUTDOWN;
  return st;
}

DT_Status
dt_remoteconn_receive (DT_RemoteConn *connection, DT_Envelope **envelope,
                       DT_Error *error)
{
  DT_Status st;
  DT_Envelope *e;
  uint64_t id = 0;
  DT_Message *msg = NULL;
  if (!connection || !envelope)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  *envelope = NULL;
  if (connection->state != DT_CONN_OPEN)
    {
      dt_err_set (error, DT_ERR_IO, 0, 0, "connection is not open");
      return DT_ERR_IO;
    }
  e = malloc (sizeof *e);
  if (!e)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return DT_ERR_NO_MEMORY;
    }
  st = dt_frame_receive (connection->fd, &connection->reader, &id, &msg,
                         connection->rto_ms, &connection->cancelled, error);
  if (st != DT_OK)
    {
      free (e);
      if (st == DT_ERR_EOF || st == DT_ERR_LIMIT
          || (st == DT_ERR_PROTOCOL && connection->reader.header_got))
        {
          connection->state = DT_CONN_SHUTDOWN;
          dt_frame_reset (&connection->reader);
        }
      return st;
    }
  e->id = id;
  e->msg = msg;
  if (msg->type == DT_MESSAGE_CLOSE)
    connection->state = DT_CONN_SHUTDOWN;
  *envelope = e;
  return DT_OK;
}

DT_Status
dt_remoteconn_ping (DT_RemoteConn *connection, uint64_t message_id,
                    DT_Error *error)
{
  DT_Message *m;
  DT_Envelope env;
  DT_Status st;
  if (!connection)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL connection");
      return DT_ERR_INVALID_ARGUMENT;
    }
  m = dt_message_create_heartbeat (error);
  if (!m)
    return error ? error->code : DT_ERR_NO_MEMORY;
  env.id = message_id;
  env.msg = m;
  st = dt_remoteconn_send (connection, &env, error);
  dt_message_free (m);
  return st;
}
