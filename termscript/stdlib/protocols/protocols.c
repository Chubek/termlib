#include "protocols/protocols.h"
#include "common/ts_std_common.h"
#include <stdlib.h>
#include <string.h>

struct TS_ProtocolDecoder
{
  uint8_t header[TS_PROTOCOL_HEADER_SIZE];
  uint8_t *payload;
  size_t header_used, used, length;
  uint32_t limit;
  TS_Status fault;
  size_t fault_offset;
  bool ready, ended, closed;
};
static TS_Status fail (TS_Error *e, TS_Status status, size_t offset, const char *message)
{
  ts_error_set (e, status, 0, offset, "%s", message); return status;
}
static uint32_t get32 (const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put32 (uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t crc (uint32_t c, const uint8_t *p, size_t n)
{
  for (size_t i = 0; i < n; i++)
    {
      c ^= p[i];
      for (unsigned j = 0; j < 8; j++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
  return c;
}
void ts_protocol_options_init (TS_ProtocolOptions *o)
{
  if (o) { memset (o, 0, sizeof *o); o->struct_size = sizeof *o; o->version = TS_PROTOCOL_VERSION; o->max_payload = 65536; }
}
TS_Status ts_protocol_encode (uint16_t type, const void *payload, size_t size,
                              void *output, size_t capacity, size_t *written, TS_Error *e)
{
  if (!written) return fail (e, TS_ERR_INVAL, 0, "protocol: written pointer required");
  *written = 0;
  if (!type || (!payload && size) || (!output && capacity))
    return fail (e, TS_ERR_INVAL, 0, "protocol: invalid encode arguments");
  if (size > TS_PROTOCOL_MAX_PAYLOAD) return fail (e, TS_ERR_LIMIT, 8, "protocol payload exceeds 1 MiB");
  *written = size + TS_PROTOCOL_HEADER_SIZE;
  if (capacity < *written) return fail (e, TS_ERR_LIMIT, 0, "protocol output capacity too small");
  uint8_t *p = output;
  if (size) memmove (p + TS_PROTOCOL_HEADER_SIZE, payload, size);
  memcpy (p, "TSPF", 4); p[4] = TS_PROTOCOL_VERSION; p[5] = 0;
  p[6] = (uint8_t)(type >> 8); p[7] = (uint8_t)type; put32 (p + 8, (uint32_t)size);
  put32 (p + 12, ~crc (crc (~0u, p, 12), p + TS_PROTOCOL_HEADER_SIZE, size));
  return TS_OK;
}
TS_Status ts_protocol_decoder_create (const TS_ProtocolOptions *o,
                                       TS_ProtocolDecoder **out, TS_Error *e)
{
  if (!out) return fail (e, TS_ERR_INVAL, 0, "protocol: decoder output required");
  *out = NULL;
  TS_ProtocolOptions defaults;
  if (!o) { ts_protocol_options_init (&defaults); o = &defaults; }
  if (o->struct_size != sizeof *o || o->version != TS_PROTOCOL_VERSION
      || o->max_payload > TS_PROTOCOL_MAX_PAYLOAD)
    return fail (e, TS_ERR_INVAL, 0, "protocol: invalid options size/version/limit");
  for (size_t i = 0; i < 4; i++)
    if (o->reserved[i]) return fail (e, TS_ERR_INVAL, 0, "protocol: nonzero reserved option");
  TS_ProtocolDecoder *d = calloc (1, sizeof *d);
  if (!d) return ts_std_oom (e);
  d->limit = o->max_payload; *out = d; return TS_OK;
}
void ts_protocol_decoder_reset (TS_ProtocolDecoder *d)
{
  if (d)
    { uint32_t limit = d->limit; free (d->payload); memset (d, 0, sizeof *d); d->limit = limit; }
}
void ts_protocol_decoder_free (TS_ProtocolDecoder *d)
{ if (d) { free (d->payload); free (d); } }
static TS_Status poison (TS_ProtocolDecoder *d, TS_Status status, size_t at, TS_Error *e)
{
  d->fault = status; d->fault_offset = at;
  return fail (e, status, at, status == TS_ERR_LIMIT ? "protocol payload exceeds decoder limit"
                   : status == TS_ERR_UNSUPPORTED ? "unsupported protocol version or flags"
                   : status == TS_ERR_NOMEM ? "protocol result allocation failed; reset required"
                   : "malformed, truncated or corrupt protocol frame");
}
TS_Status ts_protocol_decoder_feed (TS_ProtocolDecoder *d, const void *input, size_t size,
                                     size_t *consumed, TS_ProtocolFrame *frame, TS_Error *e)
{
  if (consumed) *consumed = 0;
  if (frame) memset (frame, 0, sizeof *frame);
  if (!d || !consumed || !frame || (!input && size))
    return fail (e, TS_ERR_INVAL, 0, "protocol: invalid feed arguments");
  if (d->fault) return poison (d, d->fault, d->fault_offset, e);
  if (d->ended) return fail (e, TS_ERR_EOF, 0, "protocol input already ended");
  if (d->ready) ts_protocol_decoder_reset (d);
  const uint8_t *p = input;
  size_t take = TS_PROTOCOL_HEADER_SIZE - d->header_used;
  if (take > size) take = size;
  if (take) memcpy (d->header + d->header_used, p, take);
  d->header_used += take; *consumed = take;
  if (d->header_used < TS_PROTOCOL_HEADER_SIZE)
    return fail (e, TS_ERR_WOULD_BLOCK, d->header_used, "protocol needs more header bytes");
  if (memcmp (d->header, "TSPF", 4)) return poison (d, TS_ERR_PARSE, 0, e);
  if (d->header[4] != TS_PROTOCOL_VERSION) return poison (d, TS_ERR_UNSUPPORTED, 4, e);
  if (d->header[5]) return poison (d, TS_ERR_UNSUPPORTED, 5, e);
  if (!(d->header[6] | d->header[7])) return poison (d, TS_ERR_PARSE, 6, e);
  d->length = get32 (d->header + 8);
  if (d->length > d->limit) return poison (d, TS_ERR_LIMIT, 8, e);
  if (d->length && !d->payload)
    {
      d->payload = malloc (d->length);
      if (!d->payload) return ts_std_oom (e);
    }
  size_t remaining = size - *consumed;
  take = d->length - d->used;
  if (take > remaining) take = remaining;
  if (take) memcpy (d->payload + d->used, p + *consumed, take);
  d->used += take; *consumed += take;
  if (d->used < d->length)
    return fail (e, TS_ERR_WOULD_BLOCK, TS_PROTOCOL_HEADER_SIZE + d->used, "protocol needs more payload bytes");
  if (get32 (d->header + 12) != ~crc (crc (~0u, d->header, 12), d->payload, d->length))
    return poison (d, TS_ERR_PARSE, 12, e);
  d->ready = true;
  frame->type = (uint16_t)(((unsigned)d->header[6] << 8) | d->header[7]);
  frame->payload = d->payload; frame->size = d->length; return TS_OK;
}
TS_Status ts_protocol_decoder_finish (TS_ProtocolDecoder *d, TS_Error *e)
{
  if (!d) return fail (e, TS_ERR_INVAL, 0, "protocol: decoder required");
  if (d->fault) return poison (d, d->fault, d->fault_offset, e);
  if (d->header_used && !d->ready)
    return poison (d, TS_ERR_PARSE, d->header_used + d->used, e);
  d->ended = true;
  return fail (e, TS_ERR_EOF, 0, "protocol end of input");
}

/* Termscript binary boundary: strict hex input/output, never C-string truncation. */
static const char tag;
static void destroy (void *p) { ts_protocol_decoder_free (p); }
static int digit (char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static TS_Status unhex (const TS_Value *v, uint8_t **out, size_t *size, TS_Error *e)
{
  const char *text;
  *out = NULL;
  *size = 0;
  if (ts_std_str (v, &text, e, "protocol hex")) return TS_ERR_INVAL;
  size_t n = strnlen (text, 2 * (TS_PROTOCOL_MAX_PAYLOAD + TS_PROTOCOL_HEADER_SIZE) + 1);
  if (n > 2 * (TS_PROTOCOL_MAX_PAYLOAD + TS_PROTOCOL_HEADER_SIZE))
    return fail (e, TS_ERR_LIMIT, 0, "protocol hex chunk too large");
  if (n % 2) return fail (e, TS_ERR_PARSE, n - 1, "protocol hex requires complete byte pairs");
  for (size_t i = 0; i < n; i++)
    if (digit (text[i]) < 0) return fail (e, TS_ERR_PARSE, i, "invalid protocol hex digit");
  uint8_t *bytes = malloc (n / 2 + 1);
  if (!bytes) return ts_std_oom (e);
  for (size_t i = 0; i < n / 2; i++) bytes[i] = (uint8_t)((digit (text[2*i]) << 4) | digit (text[2*i+1]));
  *out = bytes; *size = n / 2; return TS_OK;
}
static char *hex (const uint8_t *p, size_t n)
{
  const char *digits = "0123456789abcdef";
  char *s = malloc (2*n + 1);
  if (!s) return NULL;
  for (size_t i = 0; i < n; i++) { s[2*i] = digits[p[i] >> 4]; s[2*i+1] = digits[p[i] & 15]; }
  s[2*n] = 0; return s;
}
static TS_Status create (TS_VM *vm, void *u, const TS_Value *a, size_t n,
                         TS_Value *r, TS_Error *e)
{
  (void)u;
  if (ts_std_argc (vm, n, 0, 1, e, "protocols.new")) return TS_ERR_INVAL;
  TS_ProtocolOptions o; ts_protocol_options_init (&o);
  if (n)
    {
      int64_t limit;
      if (ts_std_int (a, &limit, e, "protocol limit") || limit < 0 || limit > TS_PROTOCOL_MAX_PAYLOAD)
        return ts_std_inval (e, "protocol limit must be 0..1 MiB");
      o.max_payload = (uint32_t)limit;
    }
  TS_ProtocolDecoder *d;
  TS_Status st = ts_protocol_decoder_create (&o, &d, e);
  if (st) return st;
  if (ts_value_make_handle (r, d, destroy, "protocol decoder", &tag))
    { destroy (d); return ts_std_oom (e); }
  return TS_OK;
}
static TS_Status encode (TS_VM *vm, void *u, const TS_Value *a, size_t n,
                         TS_Value *r, TS_Error *e)
{
  (void)u;
  int64_t type;
  if (ts_std_argc (vm, n, 2, 2, e, "protocols.encode_hex") || ts_std_int (a, &type, e, "protocol type")) return TS_ERR_INVAL;
  if (type < 1 || type > 65535) return ts_std_inval (e, "protocol type must be 1..65535");
  uint8_t *payload; size_t size;
  TS_Status st = unhex (a + 1, &payload, &size, e);
  if (st) return st;
  size_t capacity = size + TS_PROTOCOL_HEADER_SIZE, written;
  uint8_t *wire = malloc (capacity);
  if (!wire) { free (payload); return ts_std_oom (e); }
  st = ts_protocol_encode ((uint16_t)type, payload, size, wire, capacity, &written, e);
  free (payload);
  char *text = st ? NULL : hex (wire, written); free (wire);
  if (st) return st;
  if (!text) return ts_std_oom (e);
  ts_value_free (r); r->type = TS_STRING; r->as.string = text; return TS_OK;
}
static TS_Status operation (TS_VM *vm, void *u, const TS_Value *a, size_t n,
                            TS_Value *r, TS_Error *e)
{
  intptr_t op = (intptr_t)u;
  if (ts_std_argc (vm, n, op == 0 ? 2 : 1, op == 0 ? 2 : 1, e, "protocols")) return TS_ERR_INVAL;
  TS_ProtocolDecoder *d = ts_std_handle (a, &tag, e, "protocols");
  if (!d) return TS_ERR_INVAL;
  /* Close leaves the shared wrapper allocated but permanently invalidates it. */
  if (op == 3) { ts_protocol_decoder_reset (d); d->closed = true; ts_std_ret_nil (r); return TS_OK; }
  if (d->closed) return ts_std_inval (e, "protocol decoder is closed");
  if (op == 1) { ts_protocol_decoder_reset (d); ts_std_ret_nil (r); return TS_OK; }
  if (op == 2) return ts_protocol_decoder_finish (d, e);
  uint8_t *bytes; size_t size;
  TS_Status st = unhex (a + 1, &bytes, &size, e);
  if (st) return st;
  /* Allocate large temporary storage before advancing decoder state. */
  ts_dom_t *result = ts_dom_new_obj ();
  char *text = malloc (2 * (size_t)d->limit + 1);
  if (!result || !text)
    { free (bytes); free (text); ts_dom_free (result); return ts_std_oom (e); }
  /* DOM/list allocation can still fail after feed. Mark this explicitly as a
   * terminal decoder failure, rather than permit unsafe replay of the chunk. */
  size_t consumed; TS_ProtocolFrame frame;
  TS_Error feed_error = {0};
  st = ts_protocol_decoder_feed (d, bytes, size, &consumed, &frame, &feed_error); free (bytes);
  if (st != TS_OK && st != TS_ERR_WOULD_BLOCK)
    {
      if (st == TS_ERR_NOMEM) d->fault = TS_ERR_NOMEM;
      if (e) *e = feed_error;
      free (text); ts_dom_free (result); return st;
    }
  const char *digits = "0123456789abcdef";
  for (size_t i = 0; i < frame.size; i++) { text[2*i] = digits[frame.payload[i] >> 4]; text[2*i+1] = digits[frame.payload[i] & 15]; }
  text[2*frame.size] = 0;
  TS_Status build = ts_dom_obj_set (result, "consumed", ts_dom_new_int ((int64_t)consumed));
  if (!build) build = ts_dom_obj_set (result, "ready", ts_dom_new_bool (st == TS_OK));
  if (!build) build = ts_dom_obj_set (result, "type", ts_dom_new_int (frame.type));
  if (!build) build = ts_dom_obj_set (result, "payload_hex", ts_dom_new_str (text));
  free (text);
  if (build) { ts_dom_free (result); d->fault = TS_ERR_NOMEM; return ts_std_oom (e); }
  st = ts_dom_wrap (r, result, e);
  if (st) d->fault = TS_ERR_NOMEM;
  return st;
}
static const TS_FuncDef functions[] = {
  { "new", create, NULL }, { "encode_hex", encode, NULL },
  { "feed_hex", operation, (void *)0 }, { "reset", operation, (void *)1 },
  { "finish", operation, (void *)2 }, { "close", operation, (void *)3 }
};
const TS_Module ts_std_protocols_module = TS_MODULE_INIT ("std.protocols", functions);
