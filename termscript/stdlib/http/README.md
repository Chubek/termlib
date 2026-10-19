# `std.http`: bounded fetch and URL resolution

`fetch` and `resolve` are new APIs. Historical `get`, `post`, `status`, `headers`
and `url_parse` retain their old plain HTTP behavior; they have **not** been
hardened by this change and should not be used for untrusted browsing.

## Contract

| Call | Owned result |
|---|---|
| `resolve(base, relative)` | Canonical HTTP(S) URL or nil for invalid/disallowed input; empty base permits absolute URLs |
| `fetch(url, timeout_ms=10000, max_bytes=1048576)` | `std.map` record: `ok`, `status`, `body`, `location`, `content_type`, `error` |

Read records with `reply["ok"]` or `reply->ok`. `ok` means the transport completed,
**not** HTTP success: a complete 404 has `ok=true,status=404`. Network, protocol,
certificate, limit and unsupported-input failures have `ok=false,status=0`, an
empty body, and a diagnostic string. Partial bodies are never published. Invalid
arity/types/options return `TS_ERR_INVAL`; allocation failures in adapter-owned
result construction return `TS_ERR_NOMEM`.

Inputs are borrowed only during the call. Results own copies and survive later
requests and VM destruction when retained by the caller. Returned maps follow
the existing shared-handle policy; serialize access if sharing them. There is no
module-global connection pool, credentials, cookie jar, or mutable configuration.
Independent VMs/requests may run independently. No close call is needed for a
response map: last-reference release frees it. Sockets, TLS contexts, temporary
strings and URL arenas are released by RAII on success/failure.

Limits: timeout 1..60000 milliseconds; body 0..1 MiB; URL 8192 bytes; headers use
cpp-httplib's default 8192-byte line/100-header bounds. The body limit is enforced
while receiving, including chunked responses. NUL-containing bodies and
non-identity content encodings are rejected; no decompression is enabled. Header
and URL strings remain untrusted text: filter before terminal display.

`fetch` performs one GET, with no redirect following. The application validates
statuses and redirect policy. Fragments are omitted from request targets.
`resolve` uses Lexbor's WHATWG parser, including dot segments, query/fragment
references and IDNA. Only HTTP(S) is allowed; embedded credentials, C0/DEL in
input and oversize URLs are rejected. Nil is also possible when Lexbor cannot
produce a URL; parse failure is not a promise of a particular syntax error code.

Requests are synchronous/blocking. Connection, read, write and overall transport
timeouts use the requested budget, but system DNS lookup is not cancellable or
guaranteed to meet it. No cross-thread cancellation API is provided. Raw HTTP is
unencrypted. HTTPS requires certificate **and hostname** verification using
OpenSSL defaults/system roots (`SSL_CERT_FILE`/`SSL_CERT_DIR` can affect trust).
There is no insecure override. HTTPS-disabled builds return `ok=false`.

## Dependencies and build

- Vendored **cpp-httplib 0.30.1**, isolated behind C native callbacks in
  `fetch.cpp`. Header-only client; no external executable is invoked.
- Vendored **Lexbor 3.1.0**, already linked by HTML/CSS modules, for URLs.
- Platform **OpenSSL >= 3.0**, enabled by default with `TERMSCRIPT_HTTP_TLS=ON`.
  Set the option OFF to omit TLS, without changing the legacy HTTP APIs.

CMake, the driver's external linker flags, pkg-config metadata and the per-module
Termak archive all include the dependency closure. Generated application code
remains C. Callback declarations use C linkage; no C++ types cross the API.

Regression fixtures: `tests/termscript_http_fetch.tsc` and `tests/test_xw3m.py`.
Only loopback services are used. The TLS fixture exercises a locally trusted
certificate, untrusted roots, and hostname mismatch without changing system roots.
