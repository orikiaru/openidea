"""Web 回归：HTTP fixture、传输/JSON/连接池/TLS/绑定重启和独立 web-coro 入口。"""
import contextlib
import email.policy
from email.parser import BytesParser
import http.client
import http.server
import json
from pathlib import Path
import re
import shutil
import socket
import socketserver
import ssl
import struct
import subprocess
import sys
import threading
import time
import urllib.parse

from regression_platform import WINDOWS, SUFFIX, launch, stop, WslRedis
from test_process_owner import record_process, write_owner

from . import ports
from .config import replace, shift_ports
from .probes import EnvironmentBlocked
from .procs import REPO, cleanup_owned, copy_runtime_dlls, regression_directory


class HostHttpRegression(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def respond(self):
        self.server.hosts.append(self.headers.get("Host"))
        self.server.connections.append(self.connection)
        self.server.connection_headers.append(self.headers.get("Connection"))
        path = urllib.parse.urlsplit(self.path)
        status = 200
        if path.path == "/health":
            body = b"ok"
        elif path.path == "/query":
            params = urllib.parse.parse_qs(path.query)
            if "text" not in params:
                self.send_response(400)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            data = {"text": params["text"][0]}
            if "existing" in params:
                data["existing"] = params["existing"][0]
            body = json.dumps(data).encode()
        else:
            body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            if path.path in ("/json", "/rpcjson"):
                data = json.loads(body)
                if path.path == "/json":
                    data["ok"] = True
                else:
                    data["gap"] += 1
                body = json.dumps(data).encode()
        self.send_response(status)
        if path.path in ("/query", "/json", "/rpcjson"):
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    do_GET = respond
    do_POST = respond

    def log_message(self, format, *args):
        pass


class IPv6HttpServer(http.server.ThreadingHTTPServer):
    address_family = socket.AF_INET6
    daemon_threads = True


def wait_web_marker(output_path, marker):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        output = output_path.read_text(encoding="utf-8", errors="replace")
        if marker in output:
            return
        time.sleep(0.02)
    raise RuntimeError("missing web marker %r: %s" % (marker, output))


def check_multipart_response(content_type, body):
    assert content_type and "\r" not in content_type and "\n" not in content_type, "invalid multipart content type"
    message = BytesParser(policy=email.policy.default).parsebytes(
        b"MIME-Version: 1.0\r\nContent-Type: " + content_type.encode("ascii") + b"\r\n\r\n" + body)
    assert message.get_content_type() == "multipart/form-data" and message.get_boundary(), "missing multipart boundary"
    assert message.is_multipart() and not message.defects, "invalid multipart body"
    parts = list(message.iter_parts())
    assert len(parts) == 2, "expected exactly two multipart parts"
    expected = (("field", None, None, b"value"),
                ("file", "a.bin", "application/octet-stream", b"\x00\xffxyz"))
    for part, (name, filename, part_type, payload) in zip(parts, expected):
        assert not part.defects and part.get_content_disposition() == "form-data", "invalid part disposition"
        assert part.get_param("name", header="content-disposition") == name, "wrong part name or order"
        assert part.get_filename() == filename, "wrong filename"
        assert (part.get_content_type() if part.get("Content-Type") else None) == part_type, "wrong part content type"
        assert part.get_payload(decode=True) == payload, "wrong part bytes"


def check_web_transport(port, output_path):
    def read_response(reader):
        status = reader.readline().decode("ascii").strip()
        headers = {}
        while True:
            line = reader.readline()
            if line == b"\r\n":
                break
            if not line:
                raise RuntimeError("truncated HTTP response headers")
            key, value = line.decode("ascii").split(":", 1)
            headers[key.lower()] = value.strip()
        body = reader.read(int(headers["content-length"]))
        return status, headers, body

    with socket.create_connection(("127.0.0.1", port), timeout=10) as conn:
        with conn.makefile("rb") as reader:
            conn.sendall(b"GET /health HTTP/1.1\r\nHo")
            time.sleep(0.05)
            conn.sendall(b"st: localhost\r\n\r\n")
            status, _, body = read_response(reader)
            assert status.startswith("HTTP/1.1 200 ") and body == b"ok", (status, body)
            conn.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
            status, _, body = read_response(reader)
            assert status.startswith("HTTP/1.1 200 ") and body == b"ok", (status, body)

    with socket.create_connection(("127.0.0.1", port), timeout=10) as conn:
        conn.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
        conn.shutdown(socket.SHUT_WR)
        with conn.makefile("rb") as reader:
            status, _, body = read_response(reader)
            assert status.startswith("HTTP/1.1 200 ") and body == b"ok", (status, body)
            assert reader.read(1) == b"", "half-closed connection did not reach EOF"

    payload = bytes(range(256)) * 2048
    with socket.create_connection(("127.0.0.1", port), timeout=10) as conn:
        with conn.makefile("rb") as reader:
            conn.sendall(b"POST /bin HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
            for start in range(0, len(payload), 32768):
                chunk = payload[start:start + 32768]
                conn.sendall(("%x\r\n" % len(chunk)).encode() + chunk + b"\r\n")
            conn.sendall(b"0\r\n\r\n")
            status, headers, body = read_response(reader)
            assert status.startswith("HTTP/1.1 200 ") and body == payload, (status, len(body))
            assert headers.get("connection", "").lower() == "close", headers
            assert reader.read(1) == b"", "connection did not close after response"

    multipart = (b"--boundary42\r\nContent-Disposition: form-data; name=\"field\"\r\n\r\nvalue\r\n"
                 b"--boundary42\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.bin\"\r\n"
                 b"Content-Type: application/octet-stream\r\n\r\n\x00\xffxyz\r\n--boundary42--\r\n")
    with socket.create_connection(("127.0.0.1", port), timeout=10) as conn:
        with conn.makefile("rb") as reader:
            conn.sendall(("POST /test-multipart HTTP/1.1\r\nHost: localhost\r\n"
                          "Content-Type: multipart/form-data; boundary=boundary42\r\nContent-Length: %d\r\n\r\n" % len(multipart)).encode())
            for offset in range(0, len(multipart), 3):
                conn.sendall(multipart[offset:offset + 3])
                time.sleep(0.001)
            status, headers, body = read_response(reader)
            assert status.startswith("HTTP/1.1 200 "), (status, headers, body)
            check_multipart_response(headers.get("content-type"), body)

    with socket.create_connection(("127.0.0.1", port), timeout=10) as abandoned:
        abandoned.sendall(b"POST /test-stream HTTP/1.1\r\nHost: localhost\r\nX-Disconnect-Check: 1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\n\x00\x01\x02\x03\r\n")
        wait_web_marker(output_path, "web disconnect sink created")
    wait_web_marker(output_path, "web disconnect sink released cancelled=1")

    with contextlib.closing(http.client.HTTPConnection("127.0.0.1", port, timeout=5)) as client:
        client.request("GET", "/query")
        result = client.getresponse()
        assert result.status == 400, result.status
        result.read()
    with contextlib.closing(http.client.HTTPConnection("127.0.0.1", port, timeout=5)) as client:
        client.request("GET", "/health")
        result = client.getresponse()
        assert result.status == 200 and result.read() == b"ok"
    print("web transport PASS: segmented headers, keep-alive, chunked binary, close, error isolation", flush=True)


def check_web_json(port):
    with contextlib.closing(http.client.HTTPConnection("127.0.0.1", port, timeout=5)) as client:
        for body, content_type in ((b'{"gap":"bad"}', "application/json"),
                                   (b'{', "application/json"),
                                   (b'{} trailing', "application/json"),
                                   (b'{}', "text/plain")):
            client.request("POST", "/rpcjson", body, {"Content-Type": content_type})
            response = client.getresponse()
            response.read()
            assert response.status == 400, (body, content_type, response.status)
        payload = json.dumps({"i64": 9007199254740993, "msg": "a\u0000雪", "nums": [0, 7],
                              "child": {"n": 8, "s": "nested"}, "gap": 2},
                             ensure_ascii=False).encode("utf-8")
        for content_type in ("application/json; charset=utf-8", "application/example+json"):
            client.request("POST", "/rpcjson", payload, {"Content-Type": content_type})
            response = client.getresponse()
            body = response.read()
            assert response.status == 200, (content_type, response.status, body)
            decoded = json.loads(body)
            assert decoded["i64"] == 9007199254740993 and decoded["msg"] == "a\u0000雪", decoded
            assert decoded["nums"] == [0, 7] and decoded["gap"] == 3, decoded
            assert decoded["child"]["n"] == 8 and decoded["child"]["s"] == "nested", decoded
    print("web JSON media and malformed input PASS", flush=True)


class StreamResponseFixture(socketserver.BaseRequestHandler):
    def handle(self):
        request = b""
        while b"\r\n\r\n" not in request:
            part = self.request.recv(4096)
            if not part:
                return
            request += part
        path = request.split(b" ", 2)[1]
        size = 9 * 1024 * 1024
        header = b"HTTP/1.1 200 OK\r\nConnection: close\r\n"
        if path == b"/length":
            header += ("Content-Length: %d\r\n" % size).encode()
        elif path != b"/eof":
            raise RuntimeError("unexpected stream fixture path")
        self.request.sendall(header + b"\r\n")
        pattern = bytes(range(251))
        payload = (pattern * (size // len(pattern) + 1))[:size]
        self.request.sendall(payload)
        self.request.shutdown(socket.SHUT_WR)


@contextlib.contextmanager
def stream_response_fixture():
    with socketserver.ThreadingTCPServer(("127.0.0.1", 0), StreamResponseFixture) as fixture:
        fixture.daemon_threads = True
        thread = threading.Thread(target=fixture.serve_forever, kwargs={"poll_interval": 0.05})
        thread.start()
        try:
            yield fixture.server_address[1]
        finally:
            fixture.shutdown()
            thread.join(timeout=3)
            if thread.is_alive():
                raise RuntimeError("stream response fixture failed to stop")


def check_web_stream_length(port):
    size = 9 * 1024 * 1024
    block = bytes(range(251))
    payload = (block * (size // len(block) + 1))[:size]
    with socket.create_connection(("127.0.0.1", port), timeout=180) as conn:
        conn.sendall(("POST /test-stream HTTP/1.0\r\nHost: localhost\r\nContent-Length: %d\r\n\r\n" % size).encode())
        conn.sendall(payload)
        conn.shutdown(socket.SHUT_WR)
        response = http.client.HTTPResponse(conn)
        response.begin()
        body = response.read()
        assert (response.version == 10 and response.status == 200 and not response.chunked and
                response.getheader("Transfer-Encoding") is None and response.getheader("Content-Length") is None and
                response.getheader("Connection", "").lower() == "close" and body == payload), (
                    response.version, response.status, response.getheaders(), len(body))
    print("web stream Content-Length upload HTTP/1.0 EOF response PASS", flush=True)


class UrlRegressionHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def respond(self, body):
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()
        self.server.responses.append((self.path, time.monotonic()))

    def do_GET(self):
        self.server.requests.append((self.path, self.connection))
        if self.path == "/slow":
            self.server.slow_started.set()
            assert self.server.fast_done.wait(3), "fast request did not run concurrently"
        elif self.path == "/fast":
            assert self.server.slow_started.wait(3), "slow request did not start"
            self.respond(self.path.encode())
            self.server.fast_done.set()
            return
        self.respond(self.path.encode())

    def do_POST(self):
        self.server.requests.append((self.path, self.connection))
        body = self.rfile.read(int(self.headers["Content-Length"]))
        self.respond(body)


def check_url_client(work, cfg):
    fixture_dir = REPO / "tools/fixtures/https"
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(str(fixture_dir / "server.pem"), str(fixture_dir / "server.key"))
    servers = []
    threads = []
    try:
        for secure in (False, True):
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), UrlRegressionHandler)
            server.daemon_threads = True
            server.requests = []
            server.responses = []
            server.slow_started = threading.Event()
            server.fast_done = threading.Event()
            if secure:
                server.socket = context.wrap_socket(server.socket, server_side=True)
            servers.append(server)
            thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05})
            thread.start()
            threads.append(thread)
        plain, secure = servers
        config = (REPO / "deploy/config/webclient.toml").read_text(encoding="utf-8-sig")
        config = config.replace('port = 18080', 'port = %d' % plain.server_address[1])
        config = config.replace('ca_file = ""', 'ca_file = ' + json.dumps(str(fixture_dir / "ca.pem")))
        config += '\nurl_case = true\nurl_tls_port = %d\n' % secure.server_address[1]
        (cfg / "webclient.toml").write_text(config, encoding="utf-8")
        client = subprocess.run([str(work / ("webclient" + SUFFIX))], cwd=work, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=20)
        assert client.returncode == 0 and "url regression bad=0" in client.stdout, (client.returncode, client.stdout, client.stderr)
        requests = dict(plain.requests)
        assert len(plain.requests) == 9 and set(requests) == {"/first", "/second?x=1", "/third", "/fresh", "/after", "/slow", "/fast", "/temporary", "/post"}, plain.requests
        assert requests["/first"] is requests["/second?x=1"] is requests["/third"] is requests["/after"], plain.requests
        assert requests["/fresh"] is not requests["/first"]
        assert requests["/slow"] is not requests["/fast"]
        response_order = [path for path, _ in plain.responses if path in ("/fast", "/slow")]
        assert response_order == ["/fast", "/slow"], (plain.responses, client.stdout, client.stderr)
        assert [path for path, _ in secure.requests] == ["/tls"], secure.requests
        print("URL client PASS: shared reuse, fresh, concurrent requests, TLS trust isolation", flush=True)
    finally:
        for server in servers:
            server.shutdown()
        for thread in threads:
            thread.join(timeout=5)
        for server in servers:
            server.server_close()
        if any(thread.is_alive() for thread in threads):
            raise RuntimeError("URL fixture failed to stop")


class DisconnectRegressionServer(socketserver.ThreadingTCPServer):
    def shutdown_request(self, request):
        # Preserve SO_LINGER's abortive close; shutdown(SHUT_WR) sends FIN first.
        if request in self.reset_connections:
            request.close()
            self.reset_closed.set()
        else:
            super().shutdown_request(request)


class DisconnectRegressionHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(4)
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.request.recv(4096)
            if not chunk:
                return
            data += chunk
        path = data.split(b" ", 2)[1]
        self.server.requests.append((path, self.request, time.monotonic()))
        if path == b"/reset":
            self.request.sendall(b"HTTP/1.1 200 OK\r\n\r\npartial")
            if not self.server.reset_release.wait(3):
                return
            self.request.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("HH" if WINDOWS else "ii", 1, 0))
            self.server.reset_connections.add(self.request)
        elif path == b"/clean":
            self.request.sendall(b"HTTP/1.1 200 OK\r\n\r\nclean")
        elif path == b"/fin":
            self.server.fin_ready.set()
            if not self.server.fin_start.wait(4):
                return
            self.request.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nf")
            if not self.server.fin_release.wait(3):
                return
            self.server.fin_sent = time.monotonic()
            self.request.sendall(b"irst")
            self.request.shutdown(socket.SHUT_WR)
            self.server.fin_closed.set()
        elif path == b"/queued":
            self.request.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nsecond")
        elif path == b"/partial-fin":
            self.request.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\npart")
            self.request.shutdown(socket.SHUT_WR)
        elif path == b"/informational-fin":
            self.request.sendall(b"HTTP/1.1 100 Continue\r\n\r\n")
            self.request.shutdown(socket.SHUT_WR)
        elif path == b"/late-first":
            self.request.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst")
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = self.request.recv(4096)
                if not chunk:
                    return
                data += chunk
            second_path = data.split(b" ", 2)[1]
            self.server.requests.append((second_path, self.request, time.monotonic()))
            self.request.shutdown(socket.SHUT_WR)


def check_http_disconnect(work, cfg):
    server = DisconnectRegressionServer(("127.0.0.1", 0), DisconnectRegressionHandler)
    server.daemon_threads = True
    server.requests, server.fin_sent, server.reset_connections = [], None, set()
    server.reset_release, server.reset_closed = threading.Event(), threading.Event()
    server.fin_release, server.fin_closed = threading.Event(), threading.Event()
    server.fin_ready, server.fin_start = threading.Event(), threading.Event()
    control = http.server.ThreadingHTTPServer(("127.0.0.1", 0), DisconnectControlHandler)
    control.daemon_threads = True
    control.target = server
    control_thread = threading.Thread(target=control.serve_forever, kwargs={"poll_interval": 0.05})
    control_thread.start()
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05})
    thread.start()
    try:
        config = (REPO / "deploy/config/webclient.toml").read_text(encoding="utf-8-sig")
        config = config.replace('port = 18080', 'port = %d' % server.server_address[1])
        config += '\ndisconnect_case = true\npool_control_port = %d\n[HttpClientPool]\nmax_connections_per_server = 1\n' % control.server_address[1]
        (cfg / "webclient.toml").write_text(config, encoding="utf-8")
        client = subprocess.run([str(work / ("webclient" + SUFFIX))], cwd=work, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=15)
        assert client.returncode == 0 and "disconnect regression bad=0" in client.stdout, (client.returncode, client.stdout, client.stderr)
        requests = server.requests
        assert [item[0] for item in requests] == [b"/clean", b"/reset", b"/fin", b"/queued", b"/late-first", b"/late-second", b"/partial-fin", b"/informational-fin"], requests
        assert requests[2][1] is not requests[3][1] and requests[3][2] >= server.fin_sent, requests
        assert requests[4][1] is requests[5][1], requests
        print("HTTP disconnect PASS: close-delimited RST and queued request after FIN", flush=True)
    finally:
        server.fin_start.set()
        server.fin_release.set()
        server.reset_release.set()
        server.shutdown()
        control.shutdown()
        thread.join(timeout=5)
        control_thread.join(timeout=5)
        server.server_close()
        control.server_close()
        if thread.is_alive() or control_thread.is_alive():
            raise RuntimeError("HTTP disconnect fixture failed to stop")


class DisconnectControlHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == "/ready":
            if not self.server.target.fin_ready.wait(4):
                self.send_error(503)
                return
        elif self.path == "/release":
            self.server.target.fin_start.set()
        elif self.path == "/signal-reset":
            self.server.target.reset_release.set()
            if not self.server.target.reset_closed.wait(3):
                self.send_error(503)
                return
        elif self.path == "/signal-fin":
            self.server.target.fin_release.set()
            if not self.server.target.fin_closed.wait(3):
                self.send_error(503)
                return
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()


class PoolRegressionHandler(UrlRegressionHandler):
    def finish(self):
        try:
            super().finish()
        finally:
            self.server.closed.append((self.connection, time.monotonic()))

    def do_GET(self):
        self.server.requests.append((self.path, self.connection, time.monotonic()))
        if self.path in ("/hold-one", "/hold-two"):
            self.server.holds[self.path].set()
            assert self.server.release.wait(4), "hold release not received"
        elif self.path == "/busy":
            self.server.busy_started.set()
            assert self.server.fresh_done.wait(3), "fresh request did not run beside busy request"
        elif self.path == "/fresh-full":
            assert self.server.busy_started.wait(3), "busy request did not start"
            self.respond(self.path.encode())
            self.server.fresh_done.set()
            return
        self.respond(self.path.encode())


class PoolRegressionServer(http.server.ThreadingHTTPServer):
    def get_request(self):
        conn, address = super().get_request()
        self.accepted.append((conn, time.monotonic()))
        return conn, address


class PoolControlHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        target = self.server.target
        if self.path == "/ready":
            ready = all(event.wait(4) for event in target.holds.values())
            if not ready:
                self.send_error(503)
                return
        elif self.path == "/release":
            target.released_at = time.monotonic()
            target.release.set()
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()


def check_http_pool(work, cfg):
    server = PoolRegressionServer(("127.0.0.1", 0), PoolRegressionHandler)
    server.daemon_threads = True
    server.requests, server.responses, server.closed = [], [], []
    server.busy_started, server.fresh_done = threading.Event(), threading.Event()
    server.holds = {"/hold-one": threading.Event(), "/hold-two": threading.Event()}
    server.release, server.accepted, server.released_at = threading.Event(), [], None
    control = http.server.ThreadingHTTPServer(("127.0.0.1", 0), PoolControlHandler)
    control.daemon_threads = True
    control.target = server
    control_thread = threading.Thread(target=control.serve_forever, kwargs={"poll_interval": 0.05})
    control_thread.start()
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05})
    thread.start()
    try:
        config = (REPO / "deploy/config/webclient.toml").read_text(encoding="utf-8-sig")
        config = config.replace('port = 18080', 'port = %d' % server.server_address[1])
        config += '\npool_case = true\npool_control_port = %d\n[HttpClientPool]\nmax_connections_per_server = 2\nidle_timeout_ms = 500\n' % control.server_address[1]
        (cfg / "webclient.toml").write_text(config, encoding="utf-8")
        client = subprocess.run([str(work / ("webclient" + SUFFIX))], cwd=work, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=20)
        assert client.returncode == 0 and "pool regression bad=0" in client.stdout, (client.returncode, client.stdout, client.stderr)
        requests = {path: (conn, at) for path, conn, at in server.requests}
        assert len(server.requests) == 7 and set(requests) == {"/hold-one", "/hold-two", "/queued-success", "/after-queue", "/busy", "/fresh-full", "/after-idle"}, server.requests
        first, second = requests["/hold-one"][0], requests["/hold-two"][0]
        assert first is not second
        assert server.released_at is not None and sum(at < server.released_at for _, at in server.accepted) == 2, server.accepted
        assert requests["/queued-success"][1] >= server.released_at
        assert requests["/queued-success"][0] in (first, second)
        assert requests["/after-queue"][0] in (first, second)
        assert requests["/busy"][0] in (first, second)
        assert requests["/fresh-full"][0] not in (first, second)
        response_order = [path for path, _ in server.responses if path in ("/fresh-full", "/busy")]
        assert response_order == ["/fresh-full", "/busy"], (server.responses, client.stdout, client.stderr)
        after = requests["/after-idle"]
        assert after[0] not in (first, second)
        assert all(any(conn is old and at <= after[1] - 1.0 for conn, at in server.closed) for old in (first, second)), server.closed
        print("HTTP pool PASS: limit, queue timeout, fresh isolation and idle eviction", flush=True)
    finally:
        server.release.set()
        server.fresh_done.set()
        server.busy_started.set()
        for event in server.holds.values():
            event.set()
        server.shutdown()
        control.shutdown()
        thread.join(timeout=5)
        control_thread.join(timeout=5)
        server.server_close()
        control.server_close()
        if thread.is_alive() or control_thread.is_alive():
            raise RuntimeError("HTTP pool fixture failed to stop")


class HttpsRegressionHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.server.wait_for_hello:
            assert self.server.hello.wait(1), "stalled ClientHello not received"
        self.server.requests.append((self.path, self.connection, time.monotonic()))
        if self.path in ("/clean-eof", "/bad-eof"):
            self.connection.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + (b"clean" if self.path == "/clean-eof" else b"bad"))
            self.close_connection = True
            if self.path == "/clean-eof":
                self.connection.settimeout(3)
                try:
                    self.connection.unwrap().close()
                except OSError:
                    pass
            else:
                socket.close(self.connection.detach())
            return
        if self.path == "/truncated":
            self.connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nshort")
            self.close_connection = True
            self.connection.shutdown(socket.SHUT_RDWR)
            return
        self.send_response(200)
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"ok")
        self.wfile.flush()
        self.server.responses.append(time.monotonic())

    def do_POST(self):
        assert self.path == "/stream"
        body = bytearray()
        while True:
            size = int(self.rfile.readline().strip(), 16)
            if not size:
                assert self.rfile.readline() == b"\r\n"
                break
            body.extend(self.rfile.read(size))
            assert self.rfile.read(2) == b"\r\n"
        assert bytes(body) == bytes(i % 251 for i in range(65536))
        self.server.requests.append((self.path, self.connection, time.monotonic()))
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()


def check_https(work, cfg):
    fixture_dir = REPO / "tools/fixtures/https"
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(str(fixture_dir / "server.pem"), str(fixture_dir / "server.key"))
    names = []
    context.set_servername_callback(lambda sock, name, ctx: names.append(name))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), HttpsRegressionHandler)
    server.daemon_threads = True
    server.socket = context.wrap_socket(server.socket, server_side=True)
    server.requests = []
    server.responses = []
    server.hello = threading.Event()
    server.wait_for_hello = False
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05})
    thread.start()
    stall = socket.socket()
    stall.bind(("127.0.0.1", 0))
    stall.listen()
    stall.settimeout(5)
    accepted = []
    closed = []
    def stall_handshake():
        try:
            conn, _ = stall.accept()
            accepted.append(time.monotonic())
            with conn:
                conn.settimeout(4)
                data = conn.recv(4096)
                assert data and data[0] == 0x16, "missing TLS ClientHello"
                server.hello.set()
                while conn.recv(4096):
                    pass
                closed.append(time.monotonic())
        except OSError:
            pass
    stalled_thread = threading.Thread(target=stall_handshake)
    try:
        port = server.server_address[1]
        base = (REPO / "deploy/config/webclient.toml").read_text(encoding="utf-8-sig")
        def run(host, ca_file, case):
            config = base.replace('ip = "127.0.0.1"', 'ip = ' + json.dumps(host))
            config = config.replace('port = 18080', 'port = %d' % port)
            config = config.replace('https = false', 'https = true')
            config = config.replace('ca_file = ""', 'ca_file = ' + json.dumps(ca_file))
            config += '\ntls_case = %s\nstall_port = %d\n' % (json.dumps(case), stall.getsockname()[1])
            (cfg / "webclient.toml").write_text(config, encoding="utf-8")
            client = subprocess.run([str(work / ("webclient" + SUFFIX))], cwd=work, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=15)
            assert client.returncode == 0 and "https regression case=%s bad=0" % case in client.stdout, (case, client.returncode, client.stdout, client.stderr)
        ca = str(fixture_dir / "ca.pem")
        normal = http.server.ThreadingHTTPServer(("127.0.0.1", 0), HostHttpRegression)
        normal.daemon_threads = True
        normal.socket = context.wrap_socket(normal.socket, server_side=True)
        normal.hosts, normal.connections, normal.connection_headers = [], [], []
        normal_thread = threading.Thread(target=normal.serve_forever, kwargs={"poll_interval": 0.05})
        normal_thread.start()
        try:
            config = base.replace('ip = "127.0.0.1"', 'ip = "localhost"')
            config = config.replace('port = 18080', 'port = %d' % normal.server_address[1])
            config = config.replace('https = false', 'https = true')
            config = config.replace('ca_file = ""', 'ca_file = ' + json.dumps(ca))
            (cfg / "webclient.toml").write_text(config, encoding="utf-8")
            client = subprocess.run([str(work / ("webclient" + SUFFIX))], cwd=work, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
            assert client.returncode == 0 and "rpc=1 bin=1 bad=0" in client.stdout, (client.stdout, client.stderr)
            assert len(normal.hosts) == 9 and len({id(conn) for conn in normal.connections}) == 1
        finally:
            normal.shutdown()
            normal_thread.join(timeout=3)
            normal.server_close()
            assert not normal_thread.is_alive()
        names.clear()
        run("localhost", ca, "success")
        assert [item[0] for item in server.requests] == ["/health", "/health", "/truncated", "/health", "/stream", "/clean-eof", "/bad-eof"], server.requests
        assert server.requests[0][1] is server.requests[1][1] is server.requests[2][1]
        assert server.requests[3][1] is not server.requests[2][1]
        assert names and all(name == "localhost" for name in names), names
        request_count = len(server.requests)
        run("127.0.0.1", ca, "hostname")
        assert len(server.requests) == request_count
        run("localhost", "", "untrusted")
        assert len(server.requests) == request_count
        server.wait_for_hello = True
        stalled_thread.start()
        run("localhost", ca, "stall")
        stalled_thread.join(timeout=3)
        assert accepted and closed and server.responses[-1] < closed[0], (accepted, closed, server.responses)
        print("https PASS: trusted CA, hostname, untrusted CA, SNI, reuse, truncation, reconnect, nonblocking handshake", flush=True)
    finally:
        stall.close()
        if stalled_thread.ident is not None:
            stalled_thread.join(timeout=5)
        server.shutdown()
        thread.join(timeout=5)
        server.server_close()
        if stalled_thread.is_alive() or thread.is_alive():
            raise RuntimeError("HTTPS fixture failed to stop")


def check_web_bind_restart(args):
    # Real listeners exercise overlapping addresses and server-side connection closure.
    with regression_directory(prefix="oi-regression-web-bind-p%d-" % ports.PORT_OFFSET) as tmp:
        root = Path(tmp)
        write_owner(root, REPO, ports.PORT_OFFSET)
        procs, handles = [], []
        runtime = root / "runtime"
        runtime.mkdir()
        copy_runtime_dlls(args.bin_dir, runtime)
        executable = runtime / ("webserver" + SUFFIX)
        shutil.copy2(args.bin_dir / executable.name, executable)
        expected_failures = []
        try:
            for index, (first_ip, second_ip) in enumerate((
                    ("127.0.0.1", "127.0.0.1"), ("127.0.0.1", "0.0.0.0"),
                    ("0.0.0.0", "127.0.0.1"), ("0.0.0.0", "0.0.0.0"))):
                with socket.socket() as probe:
                    probe.bind(("127.0.0.1", 0))
                    port = probe.getsockname()[1]

                def prepare(name, ip):
                    instance = root / (str(index) + name)
                    work, cfg = instance / "bin", instance / "config"
                    work.mkdir(parents=True)
                    cfg.mkdir()
                    config = (REPO / "deploy/config/webserver.toml").read_text(encoding="utf-8-sig")
                    config = config.replace('ip = "127.0.0.1"', 'ip = "' + ip + '"')
                    config = config.replace("port = 18080", "port = " + str(port))
                    (cfg / "webserver.toml").write_text(config, encoding="utf-8")
                    path = root / (str(index) + name + ".out")
                    output = path.open("w", encoding="utf-8")
                    handles.append(output)
                    return work, path, output

                def start(prepared):
                    work, path, output = prepared
                    proc = launch([str(executable)], cwd=work,
                                  stdout=output, stderr=subprocess.STDOUT)
                    procs.append(proc)
                    record_process(root, proc)
                    return proc, path

                def ready(proc, path, ip):
                    deadline = time.monotonic() + 10
                    while True:
                        output = path.read_text(encoding="utf-8", errors="replace")
                        if "web server listen %s:%d" % (ip, port) in output:
                            return
                        if proc.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError("web listener failed: " + output)
                        time.sleep(0.05)

                def health(conn):
                    conn.request("GET", "/health")
                    response = conn.getresponse()
                    body = response.read()
                    assert response.status == 200 and body == b"ok", (response.status, body)

                first_prepared = prepare("first", first_ip)
                second_prepared = prepare("second", second_ip)
                restart_prepared = prepare("restart", first_ip)
                first, first_path = start(first_prepared)
                ready(first, first_path, first_ip)
                second, second_path = start(second_prepared)
                second.wait(timeout=10)
                expected_failures.append(second)
                output = second_path.read_text(encoding="utf-8", errors="replace")
                assert second.returncode != 0 and "web server startup failed code=-15" in output, (first_ip, second_ip, second.returncode, output)
                assert first.poll() is None, (first_ip, second_ip, first.returncode)
                # Request an orderly server-side close before stopping the listener.
                with socket.create_connection(("127.0.0.1", port), timeout=10) as closing:
                    closing.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
                    response = http.client.HTTPResponse(closing)
                    response.begin()
                    body = response.read()
                    assert response.status == 200 and body == b"ok", (response.status, body)
                    assert closing.recv(1) == b"", "server did not close Connection: close request"
                    response.close()
                conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
                try:
                    health(conn)
                    # Also exercise shutdown with a previously served keep-alive connection.
                    sock = conn.sock
                    assert sock is not None
                    stop(first)
                    first.wait(timeout=10)
                    assert first.returncode == 0, ("first shutdown", first.returncode)
                    try:
                        assert sock.recv(1) == b"", "server did not close the existing connection"
                        close_result = "EOF"
                    except ConnectionResetError:
                        close_result = "reset"
                    print("web shutdown active connection: " + close_result, flush=True)
                    conn.close()
                    restarted, restarted_path = start(restart_prepared)
                    ready(restarted, restarted_path, first_ip)
                    fresh = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
                    try:
                        health(fresh)
                    finally:
                        fresh.close()
                    stop(restarted)
                    restarted.wait(timeout=10)
                    assert restarted.returncode == 0, ("restart shutdown", restarted.returncode)
                finally:
                    conn.close()
                print("web bind %s / %s restart PASS" % (first_ip, second_ip), flush=True)
        finally:
            cleanup_owned(procs, handles, root, args, "web-bind", primary=sys.exc_info()[1],
                          allow_nonzero=expected_failures)


def run_web(args):
    check_web_bind_restart(args)
    # 仍使用模块自带的 WebClient 用例，验证别名键和未改名键的 HTTP/JSON/二进制收发。
    with regression_directory(prefix="oi-regression-web-p%d-" % ports.PORT_OFFSET) as tmp:
        root = Path(tmp)
        write_owner(root, REPO, ports.PORT_OFFSET)
        work, cfg = root / "bin", root / "config"
        work.mkdir()
        cfg.mkdir()
        copy_runtime_dlls(args.bin_dir, work)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        for name in ("webserver", "webclient"):
            shutil.copy2(args.bin_dir / (name + SUFFIX), work / (name + SUFFIX))
            text = shift_ports((REPO / "deploy/config" / (name + ".toml")).read_text(encoding="utf-8-sig"))
            text = text.replace("port = 18080", "port = " + str(port))
            if name == "webserver":
                text += "\necho_cookies = true\nform_regression = true\nmax_receive_buffer_bytes = 131072\nmax_frame_bytes = 131064\n"
            if name == "webclient":
                text = replace(text, "timeout_ms", "120000")
                text += "\ncheck_echo_header = true\n"
            (cfg / (name + ".toml")).write_text(text, encoding="utf-8")
        with (root / "server.out").open("w", encoding="utf-8") as output:
            server = launch([str(work / ("webserver" + SUFFIX))], cwd=work, stdout=output, stderr=subprocess.STDOUT)
            try:
                record_process(root, server)
                deadline = time.monotonic() + 10
                while True:
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2): break
                    except OSError:
                        if server.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError((root / "server.out").read_text())
                        time.sleep(0.05)
                with stream_response_fixture() as fixture_port:
                    with (cfg / "webclient.toml").open("a", encoding="utf-8") as config:
                        config.write("stream_fixture_port = %d\n" % fixture_port)
                    client = subprocess.run([str(work / ("webclient" + SUFFIX))], cwd=work, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=180)
                print(client.stdout, flush=True)
                assert client.returncode == 0 and "rpc=1 bin=1 bad=0" in client.stdout, client.stderr
                with IPv6HttpServer(("::1", 0), HostHttpRegression) as fixture:
                    fixture.hosts = []
                    fixture.connections = []
                    fixture.connection_headers = []
                    thread = threading.Thread(target=fixture.serve_forever, kwargs={"poll_interval": 0.05})
                    thread.start()
                    try:
                        fixture_port = fixture.server_address[1]
                        for host, reuse in (("::1", True), ("[::1]", False)):
                            config = (REPO / "deploy/config/webclient.toml").read_text(encoding="utf-8-sig")
                            config = shift_ports(config).replace('[WebClient]\nip = "127.0.0.1"', '[WebClient]\nip = "' + host + '"')
                            config = config.replace("port = 18080", "port = " + str(fixture_port))
                            config = config.replace("reuse = true", "reuse = " + str(reuse).lower())
                            (cfg / "webclient.toml").write_text(config, encoding="utf-8")
                            fixture.hosts.clear()
                            fixture.connections.clear()
                            fixture.connection_headers.clear()
                            client = subprocess.run([str(work / ("webclient" + SUFFIX))], cwd=work, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
                            assert client.returncode == 0 and "rpc=1 bin=1 bad=0" in client.stdout, (host, client.stdout, client.stderr)
                            assert fixture.hosts == ["[::1]:%d" % fixture_port] * 9, (host, fixture.hosts)
                            assert fixture.connection_headers == [("keep-alive" if reuse else "close")] * 9, (host, fixture.connection_headers)
                            assert len({id(connection) for connection in fixture.connections}) == (1 if reuse else 9), (host, fixture.connections)
                            print("web ipv6 host %s reuse=%s PASS" % (host, reuse), flush=True)
                    finally:
                        fixture.shutdown()
                        thread.join(timeout=3)
                        if thread.is_alive():
                            raise RuntimeError("IPv6 HTTP fixture failed to stop")
                check_http_pool(work, cfg)
                check_http_disconnect(work, cfg)
                check_url_client(work, cfg)
                check_https(work, cfg)
                check_web_transport(port, root / "server.out")
                check_web_json(port)
                check_web_stream_length(port)
                print("web PASS", flush=True)
            finally:
                cleanup_owned([server], [output], root, args, "web", primary=sys.exc_info()[1])


def run_web_coro(args):
    # 独立后端和配置，禁止发现已有注册表；所有请求由真实网络路径完成。
    names = ("ipcserver", "redisserver", "webserver")
    missing = [str(args.bin_dir / (name + SUFFIX)) for name in names
               if not (args.bin_dir / (name + SUFFIX)).is_file()]
    if missing:
        raise EnvironmentBlocked("missing executables: " + ", ".join(missing))
    if args.redis_wsl:
        try:
            check = subprocess.run(["wsl", "-u", args.wsl_user, "--exec", "test", "-x", str(args.redis_binary)],
                                   capture_output=True, timeout=10)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise EnvironmentBlocked("WSL Redis preflight failed: " + str(error)) from error
        if check.returncode:
            raise EnvironmentBlocked("WSL Redis executable unavailable: " + str(args.redis_binary))
    elif not args.redis_binary.is_file():
        raise EnvironmentBlocked("missing Redis executable: " + str(args.redis_binary))
    with regression_directory(prefix="oi-regression-web-coro-p%d-" % ports.PORT_OFFSET) as tmp:
        root = Path(tmp)
        write_owner(root, REPO, ports.PORT_OFFSET)
        work, cfg = root / "bin", root / "config"
        work.mkdir()
        cfg.mkdir()
        copy_runtime_dlls(args.bin_dir, work)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            web_port = probe.getsockname()[1]
        for name in names:
            shutil.copy2(args.bin_dir / (name + SUFFIX), work / (name + SUFFIX))
            config = shift_ports((REPO / "deploy/config" / (name + ".toml")).read_text(encoding="utf-8-sig"))
            config = re.sub(r"(?ms)^\[Registry\]\s*\n.*?(?=^\[|\Z)", "", config)
            if name == "redisserver":
                config = config.replace('port = "6379"', 'port = "%d"' % ports.REDIS_PORT)
            if name == "webserver":
                config = replace(config, "port", str(web_port))
                config += ('\n[WebRpc]\nip = "127.0.0.1"\nport = "%d"\ntype = "tcp"\ndelay_ms = 1200\n'
                           '\n[RedisClient]\nip = "127.0.0.1"\nport = "%d"\ntype = "tcp"\ntimeout_ms = 5000\n' % (ports.IPC_PORT, ports.REDIS_SERVER_PORT))
            (cfg / (name + ".toml")).write_text(config, encoding="utf-8")
        redis_conf = root / "redis.conf"
        redis_conf.write_text('bind 127.0.0.1\nport %d\nsave ""\nappendonly no\nmaxmemory 1536mb\n'
                              'maxmemory-policy allkeys-lru\ndir %s\n' % (ports.REDIS_PORT, str(root) if args.redis_wsl else '..'))
        procs, handles = [], []
        db = None
        def start(name, command=None):
            path = root / (name + ".out")
            handle = path.open("w", encoding="utf-8")
            handles.append(handle)
            if name == "redis" and args.redis_wsl:
                proc = WslRedis(command[0], redis_conf, args.wsl_user, cwd=work, stdout=handle, stderr=subprocess.STDOUT)
            else:
                proc = launch(command or [str(work / (name + SUFFIX))], cwd=work, stdout=handle, stderr=subprocess.STDOUT)
            procs.append(proc)
            record_process(root, proc)
            return proc, path
        def wait_marker(proc, path, marker, count=1):
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                text = path.read_text(encoding="utf-8", errors="replace")
                if proc.poll() is not None:
                    raise RuntimeError("%s exited: %s" % (path.name, text))
                if text.count(marker) >= count:
                    return
                time.sleep(0.02)
            raise RuntimeError("missing %s count=%d: %s" % (marker, count, text))
        def request(text):
            return ("GET /query?text=%s HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n" % text).encode()
        def response(reader, expected):
            status = reader.readline().decode("ascii").strip()
            headers = {}
            while True:
                line = reader.readline()
                if line == b"\r\n":
                    break
                if not line:
                    raise RuntimeError("truncated HTTP headers")
                key, value = line.decode("ascii").split(":", 1)
                headers[key.lower()] = value.strip()
            body = reader.read(int(headers["content-length"]))
            assert status.startswith("HTTP/1.1 200 ") and json.loads(body)["text"] == expected, (status, body, expected)
        try:
            db, dbpath = start("redis", [str(args.redis_binary), str(redis_conf) if args.redis_wsl else "../redis.conf"])
            wait_marker(db, dbpath, "Ready to accept connections")
            redis, redispath = start("redisserver")
            wait_marker(redis, redispath, "redis server listen")
            ipc, ipcpath = start("ipcserver")
            wait_marker(ipc, ipcpath, "ipc server listen")
            web, webpath = start("webserver")
            deadline = time.monotonic() + 15
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", web_port), timeout=0.2):
                        break
                except OSError:
                    if web.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError(webpath.read_text(encoding="utf-8", errors="replace"))
                    time.sleep(0.05)
            with socket.create_connection(("127.0.0.1", web_port), timeout=5) as slow:
                slow.sendall(request("slow"))
                wait_marker(web, webpath, "web rpc begin")
                health = http.client.HTTPConnection("127.0.0.1", web_port, timeout=0.8)
                try:
                    health.request("GET", "/health")
                    res = health.getresponse()
                    assert res.status == 200 and res.read() == b"ok"
                    assert "web handler complete" not in webpath.read_text(encoding="utf-8", errors="replace")
                finally:
                    health.close()
                response(slow.makefile("rb"), "slow")
            with socket.create_connection(("127.0.0.1", web_port), timeout=8) as pipe:
                pipe.sendall(request("first") + request("second"))
                pipe.shutdown(socket.SHUT_WR)
                reader = pipe.makefile("rb")
                response(reader, "first")
                response(reader, "second")
                reader.close()
            wait_marker(web, webpath, "web handler complete", 3)
            with socket.create_connection(("127.0.0.1", web_port), timeout=5) as abandoned:
                abandoned.sendall(request("abandoned"))
                wait_marker(web, webpath, "web rpc begin", 4)
            with socket.create_connection(("127.0.0.1", web_port), timeout=8) as fresh:
                fresh.sendall(request("fresh"))
                response(fresh.makefile("rb"), "fresh")
            wait_marker(web, webpath, "web handler complete", 5)
            assert webpath.read_text(encoding="utf-8", errors="replace").count("web rpc begin") == 5
            print("web-coro PASS: health, RPC, Redis, pipeline, disconnected handler", flush=True)
        finally:
            cleanup_owned(procs, handles, root, args, "web-coro", db=db, primary=sys.exc_info()[1])
