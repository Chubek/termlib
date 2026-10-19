"""Offline browser and HTTP conformance; no production network connections."""
import argparse
import contextlib
import http.server
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import threading
import time


class Handler(http.server.BaseHTTPRequestHandler):
    http_origin = ""
    def log_message(self, *_):
        pass

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/slow":
            time.sleep(0.2)
        if path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self.wfile.write(b"5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n")
            return
        body = b"<h1>local HTTP works</h1>"
        status = 200
        if path in ("/redirect", "/loop", "/downgrade"):
            status = 302
        elif path == "/missing":
            status = 404
        elif path == "/binary":
            body = b"before\0after"
        elif path == "/large":
            body = b"x" * 20000
        elif path == "/hostile":
            body = ('<title>title\x1b]52;c;bad\x07</title>'
                    '<h1>Hello\x1b[31m</h1><p>\u009b31m\u202eevil</p>'
                    '<script>HIDDEN_SCRIPT</script>'
                    '<a href="javascript:alert(1)">script link</a>'
                    '<a href="/ok">safe link</a>').encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        if path == "/encoded":
            self.send_header("Content-Encoding", "gzip")
        if status == 302:
            target = "/loop" if path == "/loop" else "/ok"
            if path == "/downgrade":
                target = self.http_origin + "/ok"
            self.send_header("Location", target)
        self.send_header("Content-Length", str(len(body) + (100 if path == "/truncated" else 0)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


@contextlib.contextmanager
def server(tls_context=None):
    service = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    service.daemon_threads = True
    if tls_context:
        service.socket = tls_context.wrap_socket(service.socket, server_side=True)
    thread = threading.Thread(target=service.serve_forever, daemon=True)
    thread.start()
    try:
        yield f'{"https" if tls_context else "http"}://127.0.0.1:{service.server_port}'
    finally:
        service.shutdown()
        service.server_close()
        thread.join()


def run(command, *, code=0, stdin=None, env=None):
    process = subprocess.run(command, input=stdin, text=True, capture_output=True,
                             timeout=45, env=env)
    assert process.returncode == code, (command, process.returncode, process.stdout, process.stderr)
    return process.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--native", action="store_true")
    parser.add_argument("--jit", action="store_true")
    parser.add_argument("--tls", action="store_true")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    app = args.root / "xw3m/xw3m.tsc"
    fixture = args.root / "xw3m/fixtures/welcome.html"
    fetch = args.root / "tests/termscript_http_fetch.tsc"
    environment = dict(os.environ, TMPDIR=str(args.work))
    for key in ("XW3M_TEST_TLS", "XW3M_TEST_TRUSTED_TLS", "XW3M_TEST_BAD_HOST_TLS", "XW3M_TEST_NO_TLS"):
        environment.pop(key, None)
    if not args.tls:
        environment["XW3M_TEST_NO_TLS"] = "1"
    app_runners = [[args.driver, str(app)]]
    fetch_runners = [[args.driver, str(fetch)]]
    for backend in ["c"] + (["qbe"] if args.native else []):
        for source, runners in [(app, app_runners), (fetch, fetch_runners)]:
            binary = args.work / f"{source.stem}-{backend}"
            run([args.driver, "--backend", backend, "-o", str(binary), str(source)], env=environment)
            runners.append([str(binary)])
    if args.jit:
        app_runners.append([args.driver, "--backend", "jiterati", str(app)])
        fetch_runners.append([args.driver, "--backend", "jiterati", str(fetch)])

    with contextlib.ExitStack() as stack:
        base = stack.enter_context(server())
        Handler.http_origin = base
        environment["XW3M_TEST_HTTP"] = base
        if args.tls:
            openssl = shutil.which("openssl")
            assert openssl, "TLS rejection test requires openssl CLI"
            cert, key = args.work / "cert.pem", args.work / "key.pem"
            run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                 "-days", "1", "-subj", "/CN=127.0.0.1",
                 "-addext", "subjectAltName=IP:127.0.0.1", "-keyout", str(key), "-out", str(cert)])
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(cert, key)
            environment["XW3M_TEST_TLS"] = stack.enter_context(server(context))
        for runner in fetch_runners:
            assert "HTTP fetch OK" in run(runner, env=environment)
        if args.tls:
            trusted_env = dict(environment, SSL_CERT_FILE=str(cert))
            tls_url = trusted_env.pop("XW3M_TEST_TLS")
            trusted_env["XW3M_TEST_TRUSTED_TLS"] = tls_url
            trusted_env["XW3M_TEST_BAD_HOST_TLS"] = tls_url.replace("127.0.0.1", "localhost")
            for runner in fetch_runners:
                assert "HTTP fetch OK" in run(runner, env=trusted_env)
            for runner in app_runners:
                assert "local HTTP works" in run(runner + ["--dump", tls_url + "/ok"], env=trusted_env)
                assert "Refusing HTTPS to HTTP downgrade" in run(
                    runner + ["--dump", tls_url + "/downgrade"], code=1, env=trusted_env)
        reference = None
        for runner in app_runners:
            dump = run(runner + ["--dump", str(fixture)], env=environment)
            assert "café, 日本語, مرحبا" in dump
            assert "[1] Next local page" in dump and "[2] Example HTTPS site" in dump
            assert "SHOULD_NOT_BE_VISIBLE" not in dump and "javascript:" not in dump
            if reference is None:
                reference = dump
            assert dump == reference, "browser backend output mismatch"
            assert "local HTTP works" in run(runner + ["--dump", base + "/redirect"], env=environment)
            hostile = run(runner + ["--dump", base + "/hostile"], env=environment)
            assert "\x1b" not in hostile and "\x07" not in hostile and "\u009b" not in hostile and "\u202e" not in hostile
            assert "HIDDEN_SCRIPT" not in hostile and "javascript:" not in hostile
            assert "Redirect limit exceeded" in run(runner + ["--dump", base + "/loop"], code=1, env=environment)
            run(runner + ["--width", "bad"], code=2, env=environment)
            run(runner + ["--unknown"], code=2, env=environment)
            run(runner + ["--dump", "file:///etc/passwd"], code=1, env=environment)
            assert "xw3m [--dump]" in run(runner + ["--help"], env=environment)
            saved = args.work / "saved.html"
            commands = f"g 1\nb\nf\nb\nt about:home\n[\n/Unicode\nn\nv\nv\ns {saved}\no {base}/missing\ng 9999\nj\nk\nq\n"
            output = run(runner + [str(fixture)], stdin=commands, env=environment)
            assert "Navigation works" in output and "tab 2/2" in output
            assert "HTTP status 404" in output and "No such link" in output
            assert saved.read_text() == fixture.read_text()
            # EOF exits cleanly with the current page still valid.
            run(runner + [str(fixture)], stdin="", env=environment)
    print("xw3m offline conformance OK")


if __name__ == "__main__":
    main()
