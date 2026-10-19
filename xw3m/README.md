# xw3m — Termscript terminal browser prototype

**Runnable:** browser policy, navigation, rendering orchestration and the
command interface are entirely in [`xw3m.tsc`](xw3m.tsc). Native standard-library
adapters provide HTTP, HTML parsing, Unicode and I/O; this does not invoke w3m,
curl, a shell, or an external rendering process.

This is a first implementation, **not yet better than w3m**. The illustrative
applications in `termscript/examples` still require substantial language and
library work. Their source has not been weakened to pretend they run.

## Run

From the repository root, after the ordinary CMake build:

```sh
build/bin/termscript xw3m/xw3m.tsc xw3m/fixtures/welcome.html
build/bin/termscript xw3m/xw3m.tsc https://example.org/
build/bin/termscript xw3m/xw3m.tsc --dump --width 60 xw3m/fixtures/welcome.html
```

No address opens the built-in start page. `--help` lists commands. Commands are
line-oriented: type a command and press Enter. No raw mode or alternate screen
is used, so EOF and runtime errors do not leave terminal modes changed.

Compile the same application to a native executable:

```sh
cmake --build build --target xw3m
build/bin/xw3m --dump xw3m/fixtures/welcome.html
# Or select QBE explicitly:
build/bin/termscript --backend qbe -o build/bin/xw3m-qbe xw3m/xw3m.tsc
```

The script and fixtures are installed under `share/termscript/xw3m`; the optional
compiled executable is a build-tree target, not an installed binary.

## Working features

- HTTP and verified HTTPS; relative URLs and HTML `<base>` through Lexbor's
  WHATWG URL parser; chunked responses; five redirects maximum; HTTPS-to-HTTP
  redirects refused.
- Local HTML files, plain-text HTTP responses, Unicode, width-limited text,
  numbered link lists, and no JavaScript/subresource execution.
- Up to 16 tabs with independent back/forward histories (128 addresses each).
- Page movement (`j`/`k`), case-folded literal search (`/TEXT`, then `n`), reload,
  source view, and explicit source saving (`s PATH`, **overwrites that path**).
- `--dump` without terminal interaction; VM/C/QBE/Jiterati conformance fixtures.
- C0/C1 and directional-control filtering of all displayed untrusted text.

## Boundaries and security

- This is a paginated, line-command interface, not a full-screen browser. It
  does not implement forms, JavaScript, images, downloads, cookies, authentication,
  bookmarks, automatic resize, advanced CSS layout, tables, or charset conversion.
  Link numbers appear in the link list, not inline. Search finds matching lines.
- Rendering uses the existing `std.html.render` subset, not a new browser engine.
  Scripts/styles are not displayed; stylesheet fetching is disabled. Hidden
  elements may still contribute links to the separate link list.
- Network bodies are capped at 1 MiB, URLs at 8192 bytes, links at 1000, and each
  request uses a 10-second transport budget. System DNS resolution can exceed
  that budget. There is no asynchronous networking or in-flight cancellation.
- HTTPS needs OpenSSL 3 and the system trust store. OpenSSL trust environment
  variables (including `SSL_CERT_FILE`) apply; do not take these from untrusted
  callers. Builds with `TERMSCRIPT_HTTP_TLS=OFF` refuse HTTPS, never downgrade it.
  Explicit HTTP navigation is allowed and **unencrypted**.
- Only HTTP(S) URLs without credentials are accepted by the network adapter.
  Local pages may follow local paths; remote pages cannot navigate to local
  files. Local reads/saves are not sandboxed. Do not expose this application as
  a server for untrusted users: it can access the user's filesystem and network.
- Compression and binary HTTP responses are rejected. UTF-8 is assumed;
  malformed UTF-8 is replaced for display. Local file reads inherit `std.io`'s
  text-file semantics. Saving stores loaded source; plain-text HTTP is saved as
  its generated HTML wrapper. Errors from parsers/size/step limits may terminate
  the application; ordinary fetch failures preserve the current tab.
- Browser output filtering is control hygiene, not HTML sanitization or a
  general Unicode spoofing defense.

## Tests

```sh
ctest --test-dir build -R 'browser_core|xw3m_offline' --output-on-failure
```

The offline suite needs Python 3 and (when TLS is enabled) the `openssl` CLI.
It creates loopback HTTP/TLS services and a temporary test certificate. It tests
trusted TLS, untrusted certificates, hostname mismatch, redirect downgrade,
limits, truncation, timeout, terminal injection, navigation and all configured
backends. It does not connect to public websites or change system trust.

## Toward the example language and a w3m successor

This milestone adds raw strings and list/map read indexing to Termscript, shared
by all backends. Remaining example-language work includes structs/enums/impl,
match, ranges/for, closures, interpolation, destructuring, methods, optional/result
propagation, defer and scheduling. See
[`IMPLEMENTATION.md`](../termscript/IMPLEMENTATION.md).

Before calling xw3m a successor to w3m, implement/test full-screen keyboard and
resize handling, accessible forms, robust charset/layout support, asynchronous
cancellable navigation, persistent bookmarks/session storage, and quantitative
compatibility/usability comparisons. No superiority claim is made here.
