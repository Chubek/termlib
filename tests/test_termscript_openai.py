"""Offline HTTP fixture and shared VM/C/QBE OpenAI module checks."""
import http.server
import json
import os
import socket
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

requests = []


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def handle_request(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        requests.append((self.command, self.path, self.headers.get("Authorization"), body))
        status = 429 if self.path.endswith("/limited") else 200
        response = json.dumps({"id": "resp_test", "output": [{"type": "function_call",
                              "call_id": "call_1", "name": "lookup", "arguments": "{}"}]})
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response.encode())

    do_POST = do_GET = do_DELETE = handle_request


driver = sys.argv[1]
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = f"http://127.0.0.1:{server.server_port}/v1"
env = dict(os.environ, OPENAI_API_KEY="fixture-key")
env.pop("OPENAI_BASE", None)
env.pop("OPENAI_USER_AGENT", None)


def run(args, success=True):
    p = subprocess.run(args, env=env, capture_output=True, text=True, timeout=60)
    assert (p.returncode == 0) == success, (args, p.stdout, p.stderr)
    return p


try:
    with tempfile.TemporaryDirectory() as tmp:
        script = Path(tmp) / "openai.tsc"
        script.write_text('''use openai, json;
const c = openai.new("fixture-key", "BASE", 2);
const r = openai.prompt(c, "test-model", "quoted \\\"input\\\"\\n");
if not json.get(r, "ok") then G:die("request failed"); end
const body = json.parse(json.get(r, "body"));
if json.get(body, "output.0.call_id") != "call_1" then G:die("tool call missing"); end
openai.responses(c, r#"{"model":"test-model","previous_response_id":"resp_test","input":[{"type":"function_call_output","call_id":"call_1","output":"done"}]}"#);
openai.chat(c, r#"{"model":"test-model","messages":[]}"#);
openai.models(c);
openai.retrieve(c, "a/b");
openai.cancel(c, "resp_test");
openai.delete(c, "resp_test");
const err = openai.retrieve(c, "limited");
if json.get(err, "ok") then G:die("expected failure"); end
if json.get(err, "status") != 429 then G:die("missing HTTP status"); end
const alias = c;
openai.close(alias);
openai.close(c);
const automatic_cleanup = openai.new("", "BASE");
G:puts("ok");
'''.replace("BASE", base))
        for backend in sys.argv[2:] or ["vm", "c"]:
            before = len(requests)
            if backend in ("vm", "jiterati"):
                p = run([driver, "--backend", backend, str(script)])
            else:
                executable = str(Path(tmp) / ("test-" + backend))
                run([driver, "--backend", backend, "-o", executable, str(script)])
                p = run([executable])
            assert p.stdout == "ok\n", p
            batch = requests[before:]
            assert len(batch) == 8, batch
            assert all(x[2] == "Bearer fixture-key" for x in batch)
            assert json.loads(batch[0][3]) == {"model": "test-model", "input": 'quoted "input"\n', "store": False}
            assert batch[4][1] == "/v1/responses/a%2Fb", batch[4]
            assert batch[5][:2] == ("POST", "/v1/responses/resp_test/cancel")
            assert batch[6][:2] == ("DELETE", "/v1/responses/resp_test")
        prefix = f'use openai; const c = openai.new("", "{base}", 1);\n'
        for bad in ['openai.responses(c, "[]");',
                    'openai.responses(c, "{");',
                    'openai.responses(c, r#"{"stream":true}"#);',
                    'const alias = c; openai.close(alias); openai.models(c);',
                    'openai.models(1);', 'openai.new("x", "http://example.com");',
                    'openai.new("x", "https://example.com", 0);',
                    'openai.new("x\\nInjected: yes");', 'openai.retrieve(c, "");',
                    'openai.new("x", "http://127.0.0.1:80junk");']:
            before = len(requests)
            script.write_text(prefix + bad)
            run([driver, str(script)], success=False)
            assert len(requests) == before
        # A bound but non-listening socket deterministically refuses connections.
        with socket.socket() as refused:
            refused.bind(("127.0.0.1", 0))
            script.write_text(f'''use openai, json;
const c = openai.new("", "http://127.0.0.1:{refused.getsockname()[1]}", 1);
const r = openai.models(c);
if json.get(r, "ok") then G:die("expected transport failure"); end
if json.get(r, "status") != 0 then G:die("unexpected status"); end
openai.close(c);
''')
            run([driver, str(script)])
finally:
    server.shutdown()
    server.server_close()
    thread.join()
