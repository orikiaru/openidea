"""不经 Redis 的纯 TCP 大块收发，区分链路问题和 Redis 问题。

一端在 WSL 跑 server，另一端在宿主跑 client，依次测下行、上行、双向各 64MiB：
  wsl -u root --exec python3 tools/diag/tcp_bulk.py server 16395
  python tools/diag/tcp_bulk.py client 127.0.0.1 16395
"""
import argparse
import socket
import threading
import time

TOTAL_BYTES = 64 << 20
MODES = ("down", "up", "both")


def drain(conn, limit):
    received = 0
    while received < limit:
        try:
            data = conn.recv(1 << 20)
        except (socket.timeout, OSError):
            break
        if not data:
            break
        received += len(data)
    return received


def serve(port):
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", port))
    listener.listen(4)
    listener.settimeout(60)
    for mode in MODES:
        conn, _ = listener.accept()
        if mode == "down":
            conn.sendall(b"x" * TOTAL_BYTES)
        elif mode == "up":
            conn.sendall(b"%d\n" % drain(conn, TOTAL_BYTES))
        else:
            sender = threading.Thread(target=lambda: conn.sendall(b"y" * TOTAL_BYTES))
            sender.start()
            drain(conn, TOTAL_BYTES)
            sender.join()
        conn.close()


def run_client(host, port):
    ok = True
    for mode in MODES:
        conn = socket.create_connection((host, port), timeout=20)
        started = time.monotonic()
        try:
            if mode == "down":
                received = drain(conn, TOTAL_BYTES)
            elif mode == "up":
                conn.sendall(b"x" * TOTAL_BYTES)
                received = int(conn.recv(64).strip() or 0)
            else:
                result = []
                reader = threading.Thread(target=lambda: result.append(drain(conn, TOTAL_BYTES)))
                reader.start()
                conn.sendall(b"x" * TOTAL_BYTES)
                reader.join()
                received = result[0]
            complete = received >= TOTAL_BYTES
            ok = ok and complete
            print("%s MB=%.1f seconds=%.2f %s" % (mode, received / 1048576, time.monotonic() - started,
                                                  "ok" if complete else "STALL"), flush=True)
        except OSError as error:
            ok = False
            print(mode, "error", error, "seconds=%.2f" % (time.monotonic() - started), flush=True)
        conn.close()
    return 0 if ok else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="role", required=True)
    server = sub.add_parser("server")
    server.add_argument("port", type=int)
    client = sub.add_parser("client")
    client.add_argument("host")
    client.add_argument("port", type=int)
    args = parser.parse_args()
    if args.role == "server":
        serve(args.port)
        return 0
    return run_client(args.host, args.port)


if __name__ == "__main__":
    raise SystemExit(main())
