"""Peer fixtures for exercising the framework HTTP client."""
import contextlib
import http.client
import http.server
import json
import socket
import socketserver
import ssl
import struct
import subprocess
import threading
import time
import urllib.parse
from regression_platform import WINDOWS, SUFFIX
from .procs import REPO


class HostHttpRegression(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def respond(self):
        if hasattr(self.server, "first_request_at") and self.server.first_request_at is None:
            self.server.first_request_at = time.monotonic()
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


class StreamResponseFixture(socketserver.BaseRequestHandler):
    def handle(self):
        request = b""
        while b"\r\n\r\n" not in request:
            part = self.request.recv(4096)
            if not part:
                return
            request += part
        path = request.split(b" ", 2)[1]
        if path in (b"/buffered-eof-exact", b"/buffered-eof-over"):
            over = path.endswith(b"-over")
            header = b"HTTP/1.1 200 OK\r\nConnection: close\r\n"
            if over:
                header += b"X-Fixture: " + b"x" * 1024 + b"\r\n"
            self.request.sendall(header + b"\r\n")
            block = b"e" * 65536
            for _ in range(128):
                self.request.sendall(block)
            if over:
                self.request.sendall(b"!")
            self.request.shutdown(socket.SHUT_WR)
            return
        if path.startswith(b"/size-"):
            size = int(path[6:])
            self.request.sendall(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n" +
                                 b"2;" + b"x" * (size - 2) + b"\r")
            time.sleep(0.05)
            self.request.sendall(b"\n\x00\x01\r\n0\r\n\r\n")
            return
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
        elif path == b"/bad-header":
            self.request.sendall(b"HTTP/1.1 200 OK\r\nnot-a-header\r\nContent-Length: 2\r\n\r\nok")
        elif path in (b"/trailer-limit", b"/big-trailer"):
            size = 65529 if path == b"/trailer-limit" else 65530
            self.request.sendall(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\nX: " + b"a" * size + b"\r\n\r\n")
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
        assert [item[0] for item in requests] == [b"/clean", b"/reset", b"/fin", b"/queued", b"/late-first", b"/late-second", b"/partial-fin", b"/informational-fin", b"/bad-header", b"/trailer-limit", b"/big-trailer", b"/clean"], requests
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
        config += '\npool_case = true\npool_control_port = %d\n[HttpClientPool]\nmax_connections_per_server = 2\nmax_waiting_per_server = 2\nmax_waiting_bytes_per_server = 1024\nidle_timeout_ms = 500\n' % control.server_address[1]
        (cfg / "webclient.toml").write_text(config, encoding="utf-8")
        client = subprocess.run([str(work / ("webclient" + SUFFIX))], cwd=work, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=20)
        assert client.returncode == 0 and "pool regression bad=0" in client.stdout, (client.returncode, client.stdout, client.stderr)
        requests = {path: (conn, at) for path, conn, at in server.requests}
        waiting = set(requests) & {"/queued-success", "/count-busy", "/count-other"}
        assert len(waiting) == 2 and len(server.requests) == 8 and set(requests) == {"/hold-one", "/hold-two", "/after-queue", "/busy", "/fresh-full", "/after-idle"} | waiting, server.requests
        first, second = requests["/hold-one"][0], requests["/hold-two"][0]
        assert first is not second
        assert server.released_at is not None and sum(at < server.released_at for _, at in server.accepted) == 2, server.accepted
        assert all(requests[path][1] >= server.released_at and requests[path][0] in (first, second) for path in waiting), server.requests
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
        def run(host, ca_file, case, send_limit=65546):
            config = base.replace('ip = "127.0.0.1"', 'ip = ' + json.dumps(host))
            config = config.replace('port = 18080', 'port = %d' % port)
            config = config.replace('https = false', 'https = true')
            config = config.replace('ca_file = ""', 'ca_file = ' + json.dumps(ca_file))
            config += '\ntls_case = %s\nstall_port = %d\n' % (json.dumps(case), stall.getsockname()[1])
            if case == "success":
                config += '\n[HttpClientPool]\nmax_send_queue_bytes = %d\n' % send_limit
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
        assert [item[0] for item in server.requests] == ["/health", "/health", "/truncated", "/health", "/stream", "/health", "/clean-eof", "/bad-eof"], server.requests
        assert server.requests[0][1] is server.requests[1][1] is server.requests[2][1]
        assert server.requests[3][1] is not server.requests[2][1]
        assert names and all(name == "localhost" for name in names), names
        server.requests.clear()
        run("localhost", ca, "success", 8192)
        assert [item[0] for item in server.requests] == ["/health", "/health", "/truncated", "/health", "/stream", "/health", "/clean-eof", "/bad-eof"], server.requests
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
