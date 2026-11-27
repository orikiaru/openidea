"""Startup cleanup and upstream failure checks using isolated real sockets."""
import http.client
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

from regression_platform import SUFFIX, WINDOWS, launch
from test_process_owner import record_process, write_owner
from . import ports
from .config import shift_ports
from .procs import REPO, cleanup_owned, copy_runtime_libraries, regression_directory


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_listener(proc, path, port):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise AssertionError(path.read_text(encoding="utf-8", errors="replace"))
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise AssertionError(path.read_text(encoding="utf-8", errors="replace"))


def check_ipc_second_listener(args):
    with regression_directory(prefix="oi-regression-startup-p%d-" % ports.PORT_OFFSET) as tmp:
        root = Path(tmp)
        write_owner(root, REPO, ports.PORT_OFFSET)
        work, cfg = root / "bin", root / "config"
        work.mkdir()
        cfg.mkdir()
        copy_runtime_libraries(args.bin_dir, work)
        shutil.copy2(args.bin_dir / ("ipcserver" + SUFFIX), work / ("ipcserver" + SUFFIX))
        tcp_port = free_port(socket.SOCK_STREAM)
        config = shift_ports((REPO / "deploy/config/ipcserver.toml").read_text(encoding="utf-8-sig"))
        config = re.sub(r'(?ms)^\[Registry\]\s*\n.*?(?=^\[|\Z)', '', config)
        config = config.replace('port = "%d"' % ports.IPC_PORT, 'port = "%d"' % tcp_port)
        procs, handles = [], []
        expected_failures = []
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as occupied:
                if WINDOWS:
                    occupied.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                occupied.bind(("127.0.0.1", 0))
                udp_port = occupied.getsockname()[1]
                config = config.replace('port = "%d"' % ports.UDP_PORT, 'port = "%d"' % udp_port)
                (cfg / "ipcserver.toml").write_text(config, encoding="utf-8")
                path = root / "failed.out"
                handle = path.open("w", encoding="utf-8")
                handles.append(handle)
                proc = launch([str(work / ("ipcserver" + SUFFIX))], cwd=work, stdout=handle, stderr=subprocess.STDOUT)
                procs.append(proc)
                record_process(root, proc)
                code = proc.wait(timeout=15)
                handle.flush()
                output = path.read_text(encoding="utf-8", errors="replace")
                assert "IpcUdp StartServer failed: code=-15" in output, output
                assert code == (0xfffffff1 if WINDOWS else 241), (code, output)
                expected_failures.append(proc)
                with socket.socket() as probe:
                    if WINDOWS:
                        probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                    else:
                        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    probe.bind(("127.0.0.1", tcp_port))
            path = root / "restarted.out"
            handle = path.open("w", encoding="utf-8")
            handles.append(handle)
            proc = launch([str(work / ("ipcserver" + SUFFIX))], cwd=work, stdout=handle, stderr=subprocess.STDOUT)
            procs.append(proc)
            record_process(root, proc)
            wait_listener(proc, path, tcp_port)
            deadline = time.monotonic() + 10
            while "ipc server listen" not in path.read_text(encoding="utf-8", errors="replace"):
                assert proc.poll() is None and time.monotonic() < deadline, path.read_text(encoding="utf-8", errors="replace")
                time.sleep(0.05)
            print("ipc second listener bind failure, TCP release and restart PASS", flush=True)
        finally:
            cleanup_owned(procs, handles, root, args, "startup", allow_nonzero=expected_failures, primary=sys.exc_info()[1])


def check_web_upstream_failures(args):
    with regression_directory(prefix="oi-regression-upstream-p%d-" % ports.PORT_OFFSET) as tmp:
        root = Path(tmp)
        write_owner(root, REPO, ports.PORT_OFFSET)
        work, cfg = root / "bin", root / "config"
        work.mkdir()
        cfg.mkdir()
        copy_runtime_libraries(args.bin_dir, work)
        shutil.copy2(args.bin_dir / ("webserver" + SUFFIX), work / ("webserver" + SUFFIX))
        procs, handles = [], []
        errors = []
        connected = threading.Event()
        try:
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                listener.listen(4)
                listener.settimeout(15)
                config = (REPO / "deploy/config/webserver.toml").read_text(encoding="utf-8-sig")
                web_port = free_port(socket.SOCK_STREAM)
                config = config.replace("port = 18080", "port = %d" % web_port)
                config += '\n[WebRpc]\nip = "127.0.0.1"\nport = "%d"\ntype = "tcp"\n' % listener.getsockname()[1]
                (cfg / "webserver.toml").write_text(config, encoding="utf-8")
                def fixture():
                    try:
                        with listener.accept()[0] as conn:
                            connected.set()
                            conn.settimeout(15)
                            def receive(size):
                                data = b""
                                while len(data) < size:
                                    part = conn.recv(size - len(data))
                                    assert part, "RPC request truncated"
                                    data += part
                                return data
                            proto, size = struct.unpack("<II", receive(8))
                            body = receive(size)
                            call_id = struct.unpack_from("<Q", body)[0] & ~(1 << 63)
                            assert proto == 3002 and call_id, (proto, call_id)
                            reply = struct.pack("<QiIIi", call_id, 0, 0, 4, 2)
                            conn.sendall(struct.pack("<II", proto, len(reply)) + reply)
                            proto, size = struct.unpack("<II", receive(8))
                            body = receive(size)
                            assert proto == 3002 and len(body) >= 8
                            conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("HH" if WINDOWS else "ii", 1, 0))
                    except BaseException as error:
                        errors.append(error)
                thread = threading.Thread(target=fixture, daemon=True)
                thread.start()
                path = root / "web.out"
                handle = path.open("w", encoding="utf-8")
                handles.append(handle)
                proc = launch([str(work / ("webserver" + SUFFIX))], cwd=work, stdout=handle, stderr=subprocess.STDOUT)
                procs.append(proc)
                record_process(root, proc)
                wait_listener(proc, path, web_port)
                assert connected.wait(10), ("RPC fixture was not connected", errors)
                for marker in ("web upstream RPC reply mismatch expected=1 received=2",
                               "web upstream RPC call failed code=-3"):
                    client = http.client.HTTPConnection("127.0.0.1", web_port, timeout=12)
                    try:
                        client.request("GET", "/query?text=probe")
                        response = client.getresponse()
                        assert response.status == 502, (marker, response.status, response.read())
                        response.read()
                    finally:
                        client.close()
                    output = path.read_text(encoding="utf-8", errors="replace")
                    assert marker in output, output
                    if "mismatch" in marker:
                        assert "web upstream RPC call failed" not in output, output
                thread.join(timeout=5)
                assert not thread.is_alive() and not errors, errors
                print("web upstream mismatch and transport failures PASS", flush=True)
        finally:
            cleanup_owned(procs, handles, root, args, "upstream", primary=sys.exc_info()[1])
