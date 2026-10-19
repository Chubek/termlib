# Chapter 16: Remote Connections

The bearer token handshake is not encryption. Use TLS, SSH forwarding, or a VPN on untrusted networks.

## Handshake validation

Replies must be complete newline-terminated ASCII lines matching `DT/1.0 OK`,
`DT/1.0 DENIED` or `DT/1.0 VERSION-MISMATCH` exactly. CRLF is also accepted.
Embedded NULs, truncated lines and success-prefix lookalikes are protocol errors;
overlong replies report `DT_ERR_LIMIT`. Failed opens close their socket.

Tokens are validated before connecting: only printable ASCII without spaces is
accepted, bounded to fit the 1024-byte HELLO buffer. Control characters cannot
inject another handshake line, and diagnostics never echo the token.
`connect_timeout_ms` must be at most `INT_MAX`; zero means unlimited. DNS lookup
is synchronous and is not bounded by this timeout. Connect, HELLO write and reply
read each have their own budget; partial bytes and `EINTR` do not restart a budget.

Remote framed I/O shares the retry/ownership rules in
[local connections](15-local-connections.md). The handshake authenticates neither
the server nor its success reply; strict parsing does **not** make raw TCP secure.

## Scope

This chapter documents the stable Termlib contract and the decisions applications must make when using it.

## Recommended pattern

Initialize option structures with their `*_options_init` function, check every returned status, and release each owned handle with its matching cleanup function. Keep borrowed pointers and file descriptors valid for the lifetime promised by the API.

## Reference

See `termlib.h` for exact signatures and `termlib.hpp` for the header-only C++ facade.
