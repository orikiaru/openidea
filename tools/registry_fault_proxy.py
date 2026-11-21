"""Test-only RESP relay for deterministic registry reload/withdraw races."""
import socket
import socketserver
import threading


def read_resp(stream):
    line = stream.readline()
    if not line:
        raise EOFError
    if not line.endswith(b"\r\n"):
        raise ValueError("invalid RESP line")
    kind, payload = line[:1], line[1:-2]
    if kind in (b"+", b"-", b":"):
        return line, payload
    if kind == b"$":
        length = int(payload)
        if length < 0:
            return line, None
        data = stream.read(length + 2)
        if len(data) != length + 2 or not data.endswith(b"\r\n"):
            raise EOFError
        return line + data, data[:-2]
    if kind == b"*":
        count = int(payload)
        if count < 0:
            return line, None
        raw, values = [line], []
        for _ in range(count):
            item, value = read_resp(stream)
            raw.append(item)
            values.append(value)
        return b"".join(raw), values
    raise ValueError("unsupported RESP prefix %r" % kind)


class RegistryRedisProxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, redis_port):
        self.redis_port = redis_port
        self.reload_armed = threading.Event()
        self.reload_target = b""
        self.withdraw_key = b""
        self.reload_captured = threading.Event()
        self.reload_release = threading.Event()
        self.reload_forwarded = threading.Event()
        self.withdraw_armed = threading.Event()
        self.withdraw_failed = threading.Event()
        self.errors = []
        super().__init__(("127.0.0.1", 0), Relay)
        self.thread = threading.Thread(target=self.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        self.thread.start()

    def close(self):
        self.reload_release.set()
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=3)
        assert not self.thread.is_alive(), "registry Redis proxy failed to stop"
        assert not self.errors, self.errors


class Relay(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            with socket.create_connection(("127.0.0.1", self.server.redis_port), timeout=5) as upstream:
                upstream.settimeout(None)
                with upstream.makefile("rb") as source, self.request.makefile("rb") as client:
                    while True:
                        raw, command = read_resp(client)
                        script = command[1] if isinstance(command, list) and len(command) > 1 and command[0].upper() == b"EVAL" else b""
                        if (self.server.withdraw_armed.is_set() and b"redis.call('GET', KEYS[3])" in script
                                and len(command) > 3 and command[3] == self.server.withdraw_key):
                            self.server.withdraw_armed.clear()
                            self.request.sendall(b"-ERR injected withdraw failure\r\n")
                            self.server.withdraw_failed.set()
                            continue
                        upstream.sendall(raw)
                        reply, _ = read_resp(source)
                        if (self.server.reload_armed.is_set() and b"local out = {}" in script
                                and self.server.reload_target in reply):
                            self.server.reload_armed.clear()
                            self.server.reload_captured.set()
                            if not self.server.reload_release.wait(10):
                                raise TimeoutError("reload release was not signalled")
                        self.request.sendall(reply)
                        if b"local out = {}" in script:
                            self.server.reload_forwarded.set()
        except (EOFError, ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
            pass
        except Exception as exc:
            self.server.errors.append(repr(exc))
