"""Run the HTTP client/server integration suite."""
import http.client
import http.server
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import threading
import time
from regression_platform import SUFFIX, launch
from test_process_owner import record_process, write_owner
from . import ports
from .config import replace, shift_ports
from .probes import EnvironmentBlocked
from .procs import REPO, cleanup_owned, copy_runtime_libraries, regression_directory
from .test_bodyless import check_http_bodyless
from .startup_checks import check_ipc_second_listener, check_web_upstream_failures
from .web_server_checks import (
    check_web_bind_restart,
    check_web_json,
    check_web_small_send_queue,
    check_web_stream_length,
    check_web_transport,
)
from .web_client_fixtures import (
    HostHttpRegression,
    IPv6HttpServer,
    check_http_disconnect,
    check_http_pool,
    check_https,
    check_url_client,
    stream_response_fixture,
)


def run_web(args):
    check_ipc_second_listener(args)
    check_web_upstream_failures(args)
    check_web_bind_restart(args)
    check_web_small_send_queue(args)
    # 仍使用模块自带的 WebClient 用例，验证别名键和未改名键的 HTTP/JSON/二进制收发。
    with regression_directory(prefix="oi-regression-web-p%d-" % ports.PORT_OFFSET) as tmp:
        root = Path(tmp)
        write_owner(root, REPO, ports.PORT_OFFSET)
        work, cfg = root / "bin", root / "config"
        work.mkdir()
        cfg.mkdir()
        copy_runtime_libraries(args.bin_dir, work)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        for name in ("webserver", "webclient"):
            shutil.copy2(args.bin_dir / (name + SUFFIX), work / (name + SUFFIX))
            text = shift_ports((REPO / "deploy/config" / (name + ".toml")).read_text(encoding="utf-8-sig"))
            text = text.replace("port = 18080", "port = " + str(port))
            if name == "webserver":
                text += "\necho_cookies = true\nform_regression = true\nmax_receive_buffer_bytes = 131072\nmax_frame_bytes = 131064\nincomplete_frame_timeout_ms = 500\nidle_timeout_ms = 2500\n"
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
                resolved = socket.getaddrinfo("localhost", None, socket.AF_UNSPEC, socket.SOCK_STREAM)
                families = list(dict.fromkeys(item[0] for item in resolved if item[0] in (socket.AF_INET, socket.AF_INET6)))
                if not families:
                    raise EnvironmentBlocked("localhost has no IPv4 or IPv6 stream address")
                print("web localhost resolver order:", ["IPv4" if family == socket.AF_INET else "IPv6" for family in families], flush=True)
                if len(families) < 2:
                    print("web localhost dual-stack fallback ENVIRONMENT BLOCKED: only one address family", flush=True)
                with IPv6HttpServer(("::1", 0), HostHttpRegression) as fixture:
                    fixture.hosts = []
                    fixture.connections = []
                    fixture.connection_headers = []
                    thread = threading.Thread(target=fixture.serve_forever, kwargs={"poll_interval": 0.05})
                    thread.start()
                    try:
                        fixture_port = fixture.server_address[1]
                        cases = [("::1", True), ("[::1]", False)]
                        if socket.AF_INET6 in families:
                            cases.append(("localhost", True))
                        for host, reuse in cases:
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
                            expected_host = "localhost" if host == "localhost" else "[::1]"
                            assert fixture.hosts == ["%s:%d" % (expected_host, fixture_port)] * 9, (host, fixture.hosts)
                            assert fixture.connection_headers == [("keep-alive" if reuse else "close")] * 9, (host, fixture.connection_headers)
                            assert len({id(connection) for connection in fixture.connections}) == (1 if reuse else 9), (host, fixture.connections)
                            print("web ipv6 host %s reuse=%s PASS" % (host, reuse), flush=True)
                    finally:
                        fixture.shutdown()
                        thread.join(timeout=3)
                        if thread.is_alive():
                            raise RuntimeError("IPv6 HTTP fixture failed to stop")
                if socket.AF_INET in families:
                    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), HostHttpRegression) as fixture:
                        fixture.daemon_threads = True
                        fixture.hosts, fixture.connections, fixture.connection_headers = [], [], []
                        fixture.first_request_at = None
                        thread = threading.Thread(target=fixture.serve_forever, kwargs={"poll_interval": 0.05})
                        thread.start()
                        try:
                            fixture_port = fixture.server_address[1]
                            config = (REPO / "deploy/config/webclient.toml").read_text(encoding="utf-8-sig")
                            config = shift_ports(config).replace('[WebClient]\nip = "127.0.0.1"', '[WebClient]\nip = "localhost"')
                            config = config.replace("port = 18080", "port = " + str(fixture_port))
                            config = config.replace("timeout_ms = 5000", "timeout_ms = 1500")
                            (cfg / "webclient.toml").write_text(config, encoding="utf-8")
                            started = time.monotonic()
                            client = subprocess.run([str(work / ("webclient" + SUFFIX))], cwd=work, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
                            assert client.returncode == 0 and "rpc=1 bin=1 bad=0" in client.stdout, (client.stdout, client.stderr)
                            assert fixture.hosts == ["localhost:%d" % fixture_port] * 9, fixture.hosts
                            assert fixture.first_request_at is not None, "IPv4 fixture received no request"
                            print("web localhost IPv4-only first request ms=%.1f PASS" % ((fixture.first_request_at - started) * 1000), flush=True)
                        finally:
                            fixture.shutdown()
                            thread.join(timeout=3)
                            if thread.is_alive():
                                raise RuntimeError("IPv4 HTTP fixture failed to stop")
                check_http_pool(work, cfg)
                check_http_disconnect(work, cfg)
                check_http_bodyless(work, cfg)
                check_url_client(work, cfg)
                check_https(work, cfg)
                check_web_transport(port, root / "server.out")
                check_web_json(port)
                check_web_stream_length(port)
                print("web PASS", flush=True)
            finally:
                cleanup_owned([server], [output], root, args, "web", primary=sys.exc_info()[1])
