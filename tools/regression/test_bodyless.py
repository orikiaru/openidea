"""HTTP bodyless/interim responses on one real persistent connection."""
import socketserver
import subprocess
import threading

from regression_platform import SUFFIX
from .procs import REPO


class BodylessHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(5)
        stream = self.request.makefile("rb")
        try:
            while True:
                line = stream.readline()
                if not line:
                    return
                parts = line.split()
                if len(parts) != 3:
                    raise RuntimeError("invalid request line")
                while True:
                    header = stream.readline()
                    if header == b"\r\n":
                        break
                    if not header:
                        raise RuntimeError("truncated request")
                path = parts[1]
                self.server.requests.append((path, self.request))
                response = {
                    b"/no-content": b"HTTP/1.1 204 No Content\r\n\r\n",
                    b"/not-modified": b"HTTP/1.1 304 Not Modified\r\nContent-Length: 12\r\n\r\n",
                    b"/early-hints": b"HTTP/1.1 103 Early Hints\r\nLink: </x>; rel=preload\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nfirst-body",
                    b"/next": b"HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nnext-body",
                }.get(path)
                if response is None:
                    raise RuntimeError("unexpected path: " + repr(path))
                self.request.sendall(response)
        except Exception as error:
            self.server.errors.append(repr(error))
        finally:
            stream.close()


def check_http_bodyless(work, cfg):
    with socketserver.ThreadingTCPServer(("127.0.0.1", 0), BodylessHandler) as server:
        server.daemon_threads = True
        server.requests, server.errors = [], []
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05})
        thread.start()
        try:
            config = (REPO / "deploy/config/webclient.toml").read_text(encoding="utf-8-sig")
            config = config.replace("port = 18080", "port = %d" % server.server_address[1])
            config += "\nbodyless_case = true\n[HttpClientPool]\nmax_connections_per_server = 1\n"
            (cfg / "webclient.toml").write_text(config, encoding="utf-8")
            client = subprocess.run([str(work / ("webclient" + SUFFIX))], cwd=work, capture_output=True,
                                    text=True, encoding="utf-8", errors="replace", timeout=15)
            assert client.returncode == 0 and "bodyless regression codes=0/0/0/0 statuses=204/304/200/200 bodies=0/0/10/9 bad=0" in client.stdout, (client.returncode, client.stdout, client.stderr)
            assert not server.errors, server.errors
            assert [path for path, _ in server.requests] == [b"/no-content", b"/not-modified", b"/early-hints", b"/next"], server.requests
            assert len({id(connection) for _, connection in server.requests}) == 1, server.requests
            print("HTTP bodyless and interim persistent sequence PASS", flush=True)
        finally:
            server.shutdown()
            thread.join(timeout=5)
            if thread.is_alive():
                raise RuntimeError("bodyless HTTP fixture failed to stop")
