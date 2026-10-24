# std.openai

Synchronous OpenAI API bindings using the vendored **openaipp 0.1.0**
(`third_party/openaipp`, MIT, Chubak Bidpaa). The checkout is the dependency
pin; its C++ namespace is `openapipp`. This adapter uses its low-level endpoint
functions, whose actual return type is a JSON **string**. It shares the root
vendored cpp-httplib with `std.http` (openaipp's own copy is absent).

## API

Load with `use openai, json;` or `G:load("std.openai")`.

| Function | Arguments |
| --- | --- |
| `new` | optional API key, base URL, timeout seconds (1–600, default 60) |
| `close` | client; idempotent, invalidates all aliases |
| `prompt` | client, model, text; sends a Responses request with `store:false` |
| `responses` | client, JSON object string; POST `/responses` |
| `chat` | client, JSON object string; POST `/chat/completions` |
| `models` | client; GET `/models` |
| `retrieve` | client, response ID |
| `cancel` | client, response ID; cancels a server-side background response |
| `delete` | client, response ID |

`new()` reads only `OPENAI_API_KEY` when the key argument is omitted. The base
defaults to `https://api.openai.com/v1`. Explicit empty keys support local test
servers. Other environment variables, including `OPENAI_BASE`, are ignored.
IDs are escaped as path segments. Requests accept at most 1 MiB per string
argument. JSON request bodies must be objects; `stream` must be absent or false.

Network calls return a `std.json` document with `ok` (boolean), `status`
(integer), `body` (raw response string), and `error` (diagnostic string).
Read fields using `json.get(result, "ok")`. HTTP failures preserve status and
response body. Transport failures have status zero. Successful calls also have
status zero because this upstream API discards successful HTTP status codes.
Bodies are not automatically parsed, so malformed server payloads remain
inspectable. Argument/closed-handle errors raise `TS_ERR_INVAL`, malformed input
JSON raises its parser error, oversized input raises `TS_ERR_LIMIT`, and allocation
failure raises `TS_ERR_NOMEM`. No C++ exceptions cross the native ABI.

## Agents and tool calls

Pass `tools`, `instructions`, `previous_response_id`, and structured `input`
through `responses`. Parse `body` using `json.parse`, inspect each output's
`type`, `name`, `arguments`, and `call_id`, dispatch your application function,
then send a `function_call_output` with the same `call_id`. The wrapper does not
execute model-provided commands. Applications control tool dispatch and iteration
limits. Chat Completions tools and messages similarly pass through `chat`.

Illustrative live-service example (not tested against a paid API):

```termscript
use openai, json;
const client = openai.new();
const result = openai.prompt(client, "gpt-4.1-mini", "Say hello.");
if json.get(result, "ok") then
  G:puts(json.get(result, "body"));
else
  G:puts(json.get(result, "error"));
end
openai.close(client);
```

Runnable offline coverage: `tests/test_termscript_openai.py DRIVER vm c qbe
jiterati` exercises a local HTTP fixture, tool output continuation, escaped IDs,
HTTP errors, input validation, aliases/close and automatic cleanup. CTest runs
VM and C coverage by default. Loading the companion makes no network requests.

## Ownership, threading and limitations

Clients are reference-counted Termscript handles; the final release destroys the
C++ client. `close` releases it early and may be repeated. Arguments are borrowed
for the call; configuration and results are owned copies. No client pointer or
descriptor is exposed. Use the VM's single-threaded policy; concurrent operations
or close on the same handle require external synchronization.

HTTPS uses verified TLS when `TERMSCRIPT_HTTP_TLS=ON` (the default). When disabled,
HTTPS client construction fails rather than downgrading. Plain HTTP is restricted
to `localhost` and `127.0.0.1`. Credentials and prompts are transmitted to the
explicitly selected endpoint. No credentials are logged by this adapter; response
bodies may contain sensitive data. Raw requests retain the service's storage
defaults unless callers explicitly set `store:false`.

The vendored client buffers full response bodies and exposes no response-size
limit, total deadline, or in-flight cancellation hook. Its timeout covers connect,
read and write separately. There are no automatic retries, streaming callbacks,
async operations, local conversation history, or automatic agent loop. `cancel`
is a separate API call, not interruption of an in-flight local call. Do not use
this blocking interface on a latency-sensitive terminal event-loop thread.
IPv6 base URLs are not supported by the upstream URL parser.
