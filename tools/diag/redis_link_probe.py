"""纯 Python 直连 Redis，验证链路本身能否承载大块双向流量和阻塞命令，排除被测代码。

--native 起 Windows 原生 redis-server.exe；--wsl 在 WSL 里起 redis-server，从宿主经 localhost 转发或
NAT 地址（--host）连接。只操作自己启动且通过随机密码认证的实例；端口被占用即失败。
"""
import argparse
from pathlib import Path
import secrets
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from regression_platform import launch, WslRedis
from test_process_owner import write_owner, record_process
import socket
import subprocess
import threading
import time

ROUNDS = 64
PAYLOAD_BYTES = 256 * 1024


def resp(*args):
    out = b"*%d\r\n" % len(args)
    for arg in args:
        data = arg if isinstance(arg, bytes) else str(arg).encode()
        out += b"$%d\r\n%s\r\n" % (len(data), data)
    return out


def connect(host, port, password):
    sock = socket.create_connection((host, port), timeout=10)
    try:
        sock.sendall(resp("AUTH", password))
        answer = b""
        while not answer.endswith(b"\r\n") and len(answer) < 1024:
            chunk = sock.recv(1024)
            if not chunk:
                break
            answer += chunk
        if answer != b"+OK\r\n":
            raise RuntimeError("probe instance authentication failed")
        return sock
    except BaseException:
        sock.close()
        raise


def wait_ready(server, output, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = output.read_text(encoding="utf-8", errors="replace")
        if server.poll() is not None:
            raise RuntimeError("owned Redis startup failed: " + text)
        if "Ready to accept connections" in text:
            return
        time.sleep(.05)
    raise RuntimeError("owned Redis startup timed out")


def stop_owned(server):
    if server.poll() is not None:
        return
    try:
        server.terminate()
    except FileNotFoundError:
        # WSL wrapper may already have reaped Redis and removed its PID file.
        server.wait(timeout=5)
        return
    try:
        server.wait(timeout=5)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait(timeout=5)


def echo_round(host, port, label, password):
    sock = connect(host, port, password)
    expected = ROUNDS * (PAYLOAD_BYTES + len("$%d\r\n\r\n" % PAYLOAD_BYTES))
    received = [0]

    def reader():
        sock.settimeout(15)
        while received[0] < expected:
            try:
                data = sock.recv(1 << 20)
            except (socket.timeout, OSError):
                return
            if not data:
                return
            received[0] += len(data)

    thread = threading.Thread(target=reader)
    thread.start()
    started = time.monotonic()
    for index in range(ROUNDS):
        sock.sendall(resp("ECHO", b"%02d" % index + b"x" * (PAYLOAD_BYTES - 2)))
    thread.join()
    sock.close()
    complete = received[0] >= expected
    print("%s echo reply_MB=%.1f/%.1f seconds=%.2f %s" % (label, received[0] / 1048576, expected / 1048576,
                                                         time.monotonic() - started, "ok" if complete else "STALL"),
          flush=True)
    return complete


def blpop_then_ping(host, port, block_seconds, password):
    sock = connect(host, port, password)
    sock.settimeout(0.05)
    started = time.monotonic()
    sock.sendall(resp("BLPOP", "probe-late", block_seconds))
    time.sleep(min(2.0, block_seconds))
    sock.sendall(resp("PING"))
    buffer = b""
    while time.monotonic() - started < block_seconds + 5 and b"+PONG" not in buffer:
        try:
            buffer += sock.recv(4096)
        except socket.timeout:
            pass
    sock.close()
    pong = b"+PONG" in buffer
    print("blpop %ds then ping: pong=%s at=%.3f" % (block_seconds, pong, time.monotonic() - started), flush=True)
    return pong


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--native", metavar="REDIS_SERVER_EXE")
    target.add_argument("--wsl", action="store_true")
    parser.add_argument("--wsl-user", default="root")
    parser.add_argument("--wsl-binary", default="redis-server")
    parser.add_argument("--host", default="127.0.0.1", help="WSL NAT address to bypass localhost forwarding")
    parser.add_argument("--port", type=int, default=16392)
    args = parser.parse_args()

    # A unique password also prevents a wrong --host from silently probing another database.
    password = secrets.token_hex(24)
    host = "127.0.0.1" if args.native else args.host
    bind = "127.0.0.1" if host == "127.0.0.1" else "0.0.0.0"
    with tempfile.TemporaryDirectory(prefix="oi-regression-probe-") as directory:
        root = Path(directory)
        write_owner(root, Path(__file__).resolve().parents[2], None)
        config = root / "redis.conf"
        config.write_text('bind %s\nport %d\nsave ""\nappendonly no\ndir .\n'
                          'maxmemory 1536mb\nrequirepass %s\n' % (bind, args.port, password), encoding="utf-8")
        output = root / "redis.out"
        with output.open("w", encoding="utf-8") as handle:
            if args.native:
                # MSYS2 Redis expects a relative config path; cwd is owned and temporary.
                server = launch([args.native, "redis.conf"], cwd=root, stdout=handle, stderr=subprocess.STDOUT)
            else:
                server = WslRedis(args.wsl_binary, config, args.wsl_user, cwd=root,
                                  stdout=handle, stderr=subprocess.STDOUT)
            try:
                record_process(root, server)
                wait_ready(server, output)
                ok = True
                for _ in range(3):
                    ok = echo_round(host, args.port, "native" if args.native else "wsl", password) and ok
                ok = blpop_then_ping(host, args.port, 3, password) and ok
                return 0 if ok else 1
            finally:
                stop_owned(server)


if __name__ == "__main__":
    raise SystemExit(main())
