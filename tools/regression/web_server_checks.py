"""HTTP server transport, request parsing, and listener regression checks."""
import contextlib
import email.policy
from email.parser import BytesParser
import http.client
import http.server
import json
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time
from regression_platform import SUFFIX, launch, stop
from test_process_owner import record_process, write_owner
from . import ports
from .procs import REPO, cleanup_owned, copy_runtime_libraries, regression_directory


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
        if headers.get("transfer-encoding", "").lower() == "chunked":
            chunks = []
            while True:
                chunk_size = int(reader.readline().split(b";", 1)[0], 16)
                if chunk_size == 0:
                    assert reader.readline() == b"\r\n"
                    break
                chunks.append(reader.read(chunk_size))
                assert reader.read(2) == b"\r\n"
            body = b"".join(chunks)
        else:
            body = reader.read(int(headers["content-length"]))
        return status, headers, body

    for path in (b"/test-head", b"/test-head-stream"):
        with socket.create_connection(("127.0.0.1", port), timeout=10) as conn:
            conn.settimeout(8)
            with conn.makefile("rb") as reader:
                conn.sendall(b"HEAD " + path + b" HTTP/1.1\r\nHost: localhost\r\n\r\n"
                             b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
                status = reader.readline()
                assert status.startswith(b"HTTP/1.1 200 "), (path, status)
                headers = {}
                while True:
                    line = reader.readline()
                    assert line, "truncated HEAD headers"
                    if line == b"\r\n":
                        break
                    key, value = line.decode("ascii").split(":", 1)
                    headers[key.lower()] = value.strip()
                if path == b"/test-head":
                    assert headers.get("content-length") == "2", headers
                else:
                    assert "content-length" not in headers and "transfer-encoding" not in headers, headers
                status, _, body = read_response(reader)
                assert status.startswith("HTTP/1.1 200 ") and body == b"ok", (path, status, body)
    wait_web_marker(output_path, "web head source released cancelled=1 read=0")
    print("web HEAD PASS: no body before next response, source cancelled and released", flush=True)

    with socket.create_connection(("127.0.0.1", port), timeout=10) as conn:
        conn.settimeout(8)
        started = time.monotonic()
        assert conn.recv(1) == b"", "first-byte idle connection stayed open"
        assert time.monotonic() - started >= 2.0, "first-byte idle connection closed early"
    with socket.create_connection(("127.0.0.1", port), timeout=10) as conn:
        conn.settimeout(8)
        with conn.makefile("rb") as reader:
            conn.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
            status, _, body = read_response(reader)
            assert status.startswith("HTTP/1.1 200 ") and body == b"ok", (status, body)
            started = time.monotonic()
            assert reader.read(1) == b"", "keep-alive idle connection stayed open"
            assert time.monotonic() - started >= 2.0, "keep-alive idle connection closed early"
    print("web idle PASS: first byte and keep-alive", flush=True)

    with socket.create_connection(("127.0.0.1", port), timeout=10) as conn:
        with conn.makefile("rb") as reader:
            conn.sendall(b"POST /bin HTTP/1.1\r\nHost: localhost\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n")
            assert reader.readline() == b"HTTP/1.1 100 Continue\r\n"
            assert reader.readline() == b"\r\n"
            conn.sendall(b"test")
            status, _, body = read_response(reader)
            assert status.startswith("HTTP/1.1 200 ") and body == b"test", (status, body)
    with socket.create_connection(("127.0.0.1", port), timeout=10) as conn:
        with conn.makefile("rb") as reader:
            conn.sendall(b"POST /bin HTTP/1.1\r\nHost: localhost\r\nExpect: unsupported\r\nContent-Length: 4\r\n\r\n")
            status, _, _ = read_response(reader)
            assert status.startswith("HTTP/1.1 417 "), status
    print("web Expect PASS: 100 before body and unsupported 417", flush=True)

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
    for chunked in (False, True):
        with socket.create_connection(("127.0.0.1", port), timeout=10) as conn:
            with conn.makefile("rb") as reader:
                framing = b"Transfer-Encoding: chunked\r\n" if chunked else ("Content-Length: %d\r\n" % len(payload)).encode()
                conn.sendall(b"POST /bin HTTP/1.1\r\nHost: localhost\r\n" + framing + b"Connection: close\r\n\r\n")
                if chunked:
                    conn.sendall(("%x\r\n" % len(payload)).encode())
                conn.sendall(payload)
                if chunked:
                    conn.sendall(b"\r\n0\r\n\r\n")
                status, _, body = read_response(reader)
                assert status.startswith("HTTP/1.1 200 ") and body == payload, (chunked, status, len(body))
    for framing in (b"Content-Length: 8388609\r\n", b"Transfer-Encoding: chunked\r\n"):
        with socket.create_connection(("127.0.0.1", port), timeout=10) as conn:
            with conn.makefile("rb") as reader:
                conn.sendall(b"POST /bin HTTP/1.1\r\nHost: localhost\r\n" + framing + b"\r\n" +
                             (b"800001\r\n" if framing.startswith(b"Transfer") else b""))
                status, _, _ = read_response(reader)
                assert status.startswith("HTTP/1.1 413 "), (framing, status)
    print("web body receive limit PASS: fixed, single chunk and oversize 413", flush=True)
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

    stream_size = 9 * 1024 * 1024
    stream_pattern = bytes(range(251))
    stream_body = (stream_pattern * (stream_size // len(stream_pattern) + 1))[:stream_size]
    for path in (b"/bin", b"/test-stream"):
        for size in (1023, 1024, 1025):
            with socket.create_connection(("127.0.0.1", port), timeout=10) as conn:
                conn.settimeout(120)
                with conn.makefile("rb") as reader:
                    header = b"POST " + path + b" HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                    line = b"2;" + b"x" * (size - 2)
                    if size == 1025:
                        conn.sendall(header + line + b"\r\n")
                    else:
                        conn.sendall(header + line + b"\r")
                        time.sleep(0.05)
                        if path == b"/bin":
                            conn.sendall(b"\nok\r\n0\r\n\r\n")
                        else:
                            conn.sendall(b"\n" + stream_body[:2] + b"\r\n" + ("%x\r\n" % (stream_size - 2)).encode())
                            conn.sendall(stream_body[2:] + b"\r\n0\r\n\r\n")
                    status, _, body = read_response(reader)
                    if size <= 1024:
                        expected = b"ok" if path == b"/bin" else stream_body
                        assert status.startswith("HTTP/1.1 200 ") and body == expected, (path, size, status, len(body))
                    else:
                        assert status.startswith("HTTP/1.1 400 "), (path, size, status)
    for headers in (b"Connection: close\r\nConnection: keep-alive\r\n", b"Connection: keep-alive\r\nConnection: close\r\n"):
        with socket.create_connection(("127.0.0.1", port), timeout=10) as conn:
            conn.settimeout(8)
            with conn.makefile("rb") as reader:
                conn.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n" + headers + b"\r\n")
                status, _, body = read_response(reader)
                assert status.startswith("HTTP/1.1 200 ") and body == b"ok", (headers, status, body)
                assert reader.read(1) == b"", (headers, "close ignored")
    print("web chunk size boundaries and Connection close PASS", flush=True)

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


def check_web_stream_length(port):
    size = 9 * 1024 * 1024
    block = bytes(range(251))
    payload = (block * (size // len(block) + 1))[:size]
    with socket.create_connection(("127.0.0.1", port), timeout=180) as conn:
        started = time.monotonic()
        conn.sendall(("POST /test-stream HTTP/1.0\r\nHost: localhost\r\nX-Busy-Check: 1\r\nContent-Length: %d\r\n\r\n" % size).encode())
        conn.sendall(payload)
        conn.shutdown(socket.SHUT_WR)
        response = http.client.HTTPResponse(conn)
        response.begin()
        body = response.read()
        assert (response.version == 10 and response.status == 200 and not response.chunked and
                response.getheader("Transfer-Encoding") is None and response.getheader("Content-Length") is None and
                response.getheader("Connection", "").lower() == "close" and body == payload and time.monotonic() - started >= 0.7), (
                    response.version, response.status, response.getheaders(), len(body))
    print("web stream Content-Length upload HTTP/1.0 EOF response PASS", flush=True)


def check_web_small_send_queue(args):
    with regression_directory(prefix="oi-regression-web-small-queue-p%d-" % ports.PORT_OFFSET) as tmp:
        root = Path(tmp)
        write_owner(root, REPO, ports.PORT_OFFSET)
        work, cfg = root / "bin", root / "config"
        work.mkdir()
        cfg.mkdir()
        copy_runtime_libraries(args.bin_dir, work)
        shutil.copy2(args.bin_dir / ("webserver" + SUFFIX), work / ("webserver" + SUFFIX))
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        config = (REPO / "deploy/config/webserver.toml").read_text(encoding="utf-8-sig")
        config = config.replace("port = 18080", "port = %d" % port)
        config += "\nform_regression = true\nmax_send_queue_bytes = 8192\n"
        (cfg / "webserver.toml").write_text(config, encoding="utf-8")
        with (root / "server.out").open("w", encoding="utf-8") as output:
            server = launch([str(work / ("webserver" + SUFFIX))], cwd=work, stdout=output, stderr=subprocess.STDOUT)
            try:
                record_process(root, server)
                deadline = time.monotonic() + 10
                while True:
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                            break
                    except OSError:
                        if server.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError((root / "server.out").read_text())
                        time.sleep(0.05)
                size = 9 * 1024 * 1024
                block = bytes(range(251))
                payload = (block * (size // len(block) + 1))[:size]
                conn = http.client.HTTPConnection("127.0.0.1", port, timeout=180)
                try:
                    conn.request("POST", "/test-stream", body=payload)
                    response = conn.getresponse()
                    body = response.read()
                    assert response.status == 200 and response.chunked and body == payload, (response.status, response.getheaders(), len(body))
                finally:
                    conn.close()
                print("web stream small server send queue PASS", flush=True)
            finally:
                cleanup_owned([server], [output], root, args, "web-small-queue", primary=sys.exc_info()[1])


def check_web_bind_restart(args):
    # Real listeners exercise overlapping addresses and server-side connection closure.
    with regression_directory(prefix="oi-regression-web-bind-p%d-" % ports.PORT_OFFSET) as tmp:
        root = Path(tmp)
        write_owner(root, REPO, ports.PORT_OFFSET)
        procs, handles = [], []
        runtime = root / "runtime"
        runtime.mkdir()
        copy_runtime_libraries(args.bin_dir, runtime)
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
