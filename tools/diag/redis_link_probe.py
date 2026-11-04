"""纯 Python 直连 Redis，验证链路本身能否承载大块双向流量和阻塞命令，排除被测代码。

--native 起 Windows 原生 redis-server.exe；--wsl 在 WSL 里起 redis-server，从宿主经 localhost 转发或
NAT 地址（--host）连接。WSL 转发卡死时这里的 echo 收不全，Redis 侧 client list 显示输出缓冲为空。
"""
import argparse
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


def wsl(user, command):
    result = subprocess.run(["wsl", "-u", user, "--exec", "bash", "-c", command], capture_output=True,
                            encoding="utf-8", errors="replace")
    return (result.stdout or "").strip()


def echo_round(host, port, label):
    sock = socket.create_connection((host, port), timeout=10)
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


def blpop_then_ping(host, port, block_seconds):
    sock = socket.create_connection((host, port), timeout=1)
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
    parser.add_argument("--host", default="127.0.0.1", help="WSL NAT address to bypass localhost forwarding")
    parser.add_argument("--port", type=int, default=16392)
    args = parser.parse_args()

    server = None
    if args.native:
        server = subprocess.Popen([args.native, "--port", str(args.port), "--bind", "127.0.0.1", "--save", "",
                                   "--appendonly", "no"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        host, label = "127.0.0.1", "native"
    else:
        bind = "127.0.0.1" if args.host == "127.0.0.1" else "0.0.0.0 --protected-mode no"
        wsl(args.wsl_user, "redis-server --port %d --bind %s --save '' --appendonly no --daemonize yes"
            % (args.port, bind))
        host, label = args.host, "wsl"
    time.sleep(1)
    ok = True
    try:
        for _ in range(3):
            if not echo_round(host, args.port, label):
                ok = False
                if args.wsl:
                    print("  redis view:", wsl(args.wsl_user, "redis-cli -p %d client list" % args.port), flush=True)
        ok = blpop_then_ping(host, args.port, 3) and ok
    finally:
        if server is not None:
            server.kill()
            server.wait()
        else:
            wsl(args.wsl_user, "redis-cli -p %d shutdown nosave" % args.port)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
