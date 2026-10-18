/* Versioned byte framing, implemented in libtermscript_stdlib (not a
 * transport). No authentication, encryption, replay protection or protocol
 * negotiation. All inputs are borrowed for the call; objects require external
 * serialization. Functions set error only on failure, replacing its previous
 * diagnostic. No operation blocks, owns fds, or performs I/O. See
 * infrastructure-modules.md.
 */
#ifndef TS_PROTOCOL_H
#define TS_PROTOCOL_H
#include "TSC-API.h"
#ifdef __cplusplus
extern "C"
{
#endif
#define TS_PROTOCOL_VERSION 1u
#define TS_PROTOCOL_HEADER_SIZE 16u
#define TS_PROTOCOL_MAX_PAYLOAD (1024u * 1024u)

  typedef struct TS_ProtocolDecoder TS_ProtocolDecoder;
  typedef struct
  {
    uint32_t struct_size; /* Initialized by ts_protocol_options_init. */
    uint32_t version;     /* Must equal TS_PROTOCOL_VERSION. */
    uint32_t max_payload; /* 0..TS_PROTOCOL_MAX_PAYLOAD; default 65536. */
    uint32_t reserved[4]; /* Must be zero. */
  } TS_ProtocolOptions;
  typedef struct
  {
    uint16_t type; /* 1..65535; unknown types are preserved for the caller. */
    const uint8_t *payload; /* Borrowed, valid until next feed/reset/free. */
    size_t size;
  } TS_ProtocolFrame;

  void ts_protocol_options_init (TS_ProtocolOptions *options); /* NULL-safe. */
  /* On success, *out is caller-owned; on failure *out is NULL. NULL options
   * select defaults. Allocation is bounded by the configured payload limit. */
  TS_Status ts_protocol_decoder_create (const TS_ProtocolOptions *options,
                                        TS_ProtocolDecoder **out, TS_Error *e);
  void ts_protocol_decoder_free (TS_ProtocolDecoder *decoder); /* NULL-safe. */
  void
  ts_protocol_decoder_reset (TS_ProtocolDecoder *decoder); /* NULL-safe. */
  /* Consumes at most one frame, reporting bytes copied in *consumed even on
   * failure. TS_OK means a complete validated *frame; WOULD_BLOCK means more
   * input, not an OS error. Extra bytes remain caller-owned and unconsumed.
   * NULL input is valid only for size 0. Malformed/unsupported/oversized input
   * poisons the decoder until reset. NOMEM is retryable with unconsumed input.
   * Input must not alias the decoder's borrowed payload. Output structs must
   * not overlap input or each other. */
  TS_Status ts_protocol_decoder_feed (TS_ProtocolDecoder *decoder,
                                      const void *input, size_t size,
                                      size_t *consumed,
                                      TS_ProtocolFrame *frame, TS_Error *e);
  /* Marks end of input. EOF at a frame boundary; PARSE for truncated data.
   * Subsequent feed returns EOF until reset. Existing frame storage survives.
   */
  TS_Status ts_protocol_decoder_finish (TS_ProtocolDecoder *decoder,
                                        TS_Error *e);
  /* Copies a complete frame to caller-owned output. *written is required size
   * even on insufficient capacity (LIMIT, no write). NULL output is allowed
   * only with capacity 0. Payload and output may overlap. No heap allocation.
   * written must not overlap input/output. Hard maximum is 1 MiB. */
  TS_Status ts_protocol_encode (uint16_t type, const void *payload,
                                size_t size, void *output, size_t capacity,
                                size_t *written, TS_Error *e);
#ifdef __cplusplus
}
#endif
#endif
