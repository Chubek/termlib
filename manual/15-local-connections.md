# Chapter 15: Local Connections

Cancellation is thread-safe and unblocks a matching operation. A closed peer is reported as EOF.

## Framing and retrying

Receive retains partial length headers, payloads and checksums across
`DT_ERR_WOULD_BLOCK` and `DT_ERR_TIMEOUT`. Retry on the same connection;
`*envelope` stays `NULL` until the complete frame passes validation.
Embedded NUL bytes in DATA payloads are preserved.

Send copies a pending frame and retains its write position under backpressure.
After a retryable result, retry the same message ID, type and payload until
`DT_OK`. A different envelope is rejected with `DT_ERR_INVALID_ARGUMENT` without
altering the pending send. Do not use raw descriptor reads/writes alongside
these buffered operations. Closing a connection releases both pending buffers.

Positive read/write timeouts use monotonic per-call deadlines, including
interrupted waits and partial socket transfers. Repeated retries start new
deadlines. Socket writes on systems with `MSG_NOSIGNAL` suppress `SIGPIPE`;
applications using pipe descriptors or platforms without that facility still
need an appropriate signal policy. Pipe writers should use nonblocking
descriptors: a blocking pipe write itself is not interrupted by cancellation.

EOF between frames means `DT_ERR_EOF`; EOF partway through a frame means
`DT_ERR_PROTOCOL`. Both shut down the connection. Invalid frame lengths also
shut it down rather than interpreting leftover bytes as a new frame. A fully
consumed frame with a bad checksum, unsupported type or malformed resize payload
reports `DT_ERR_PROTOCOL` while leaving the next frame readable.

The runnable regression fixture is `tests/test_connections.c`: it exercises
every split position, timeout/retry, backpressure, binary payloads, cancellation,
closed sockets and abandoned pending buffers without an external service.

## Scope

This chapter documents the stable Termlib contract and the decisions applications must make when using it.

## Recommended pattern

Initialize option structures with their `*_options_init` function, check every returned status, and release each owned handle with its matching cleanup function. Keep borrowed pointers and file descriptors valid for the lifetime promised by the API.

## Reference

See `termlib.h` for exact signatures and `termlib.hpp` for the header-only C++ facade.
