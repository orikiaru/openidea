"""WSL: python3 tools/run_regressions.py --case all [--clock-fault-so /tmp/oi-clock.so].

只启动仓库现有进程，临时配置和日志自动清理；不会终止不属于本次测试的进程。
先编出待测版本，单、多线程分别运行一次。Redis 默认使用已有的源码编译产物。
"""
import argparse
import datetime
import os
from pathlib import Path
import re
import resource
import signal
import shutil
import socket
import socketserver
import threading
import struct
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parents[1]
CAP = 2 * 1024**3


def replace(text, key, value):
    return re.sub(r"(?m)^" + re.escape(key) + r"\s*=.*$", key + " = " + value, text)


def bytes_on_disk(root):
    return sum(p.stat().st_size for p in root.rglob("*") if p.is_file() and not p.is_symlink())


def log_payload_on_disk(root):
    return sum(p.stat().st_size for p in root.rglob("info") if p.is_file() and not p.is_symlink())


def reset_connections():
    # 故障注入：收到 RST 后不能在会话表留下已释放的指针。
    for _ in range(100):
        with socket.create_connection(("127.0.0.1", 17981), timeout=2) as s:
            s.sendall(b"\x00")
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))


class RawHttpRegression(socketserver.BaseRequestHandler):
    # 原始 TCP 回包夹具：库级 HTTP 服务通常不会生成这些畸形报文。
    def handle(self):
        self.request.settimeout(3)
        try:
            request = b""
            while b"\r\n\r\n" not in request and len(request) < 8192:
                part = self.request.recv(8192)
                if not part: return
                request += part
            path = request.split(b" ")[1].strip(b"/")
            ok = b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
            chunk = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
            wires = {
                b"good": ok,
                b"large": b"HTTP/1.1 200 OK\r\nContent-Length: 1048576\r\n\r\n" + b"x" * 1048576,
                b"interim": b"HTTP/1.1 100 Continue\r\n\r\n" + ok,
                b"204": b"HTTP/1.1 204 No Content\r\n\r\n",
                b"304": b"HTTP/1.1 304 Not Modified\r\nContent-Length: 123\r\n\r\n",
                b"chunk-good": chunk + b"2;name=value\r\nok\r\n0\r\nX-Test: done\r\n\r\n",
                b"length-junk": b"HTTP/1.1 200 OK\r\nContent-Length: 1x\r\n\r\na",
                b"length-overflow": b"HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551616\r\n\r\n",
                b"length-conflict": b"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nok",
                b"te-cl": b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
                b"chunk-separator": chunk + b"1\r\naXX0\r\n\r\n",
                b"chunk-junk": chunk + b"1x\r\na\r\n0\r\n\r\n",
                b"truncated": b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na",
                b"only-interim": b"HTTP/1.1 100 Continue\r\n\r\n",
            }
            if path == b"fragmented":
                self.request.sendall(b"HTTP/1.1 103 Early Hints\r\nLink: </test>\r\n\r\nHTTP/1.1 200")
                self.request.settimeout(0.02)
                try: self.request.recv(1)
                except socket.timeout: pass
                self.request.sendall(b" OK\r\nContent-Length: 2\r\n\r\nok")
            else:
                self.request.sendall(wires[path])
            if path in (b"204", b"304"):
                self.request.recv(1)  # 客户端必须在无正文响应完成后主动关闭，不依赖服务端 EOF。
        except (OSError, IndexError, KeyError):
            return


def run(case, args):
    mode = "hold" if case.startswith("clock-") else ("echo" if case in ("reset", "registry") else case)
    mode = "log" if case in ("disk-full", "log-backpressure") else mode
    mode = "redis" if case.startswith("redis-") else mode
    fairness = case == "redis-fairness"
    pressure = case.startswith("redis-backpressure-") or fairness
    db_paused = False
    pause_at = None
    shutdown_sent = False
    procs = []
    handles = []
    http_fixture = None
    http_thread = None
    monitor = None
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix="oi-regression-") as tmp:
        root = Path(tmp)
        work, cfg, logs = root / "bin", root / "config", root / "logs"
        for p in (work, cfg, logs):
            p.mkdir()
        for name in ("logd", "ipcclient", "ipcserver", "redisserver") + (("registry",) if case == "registry" or mode == "hold" else ()):
            shutil.copy2(args.bin_dir / name, work / name)
            text = (REPO / "deploy/config" / (name + ".toml")).read_text(encoding="utf-8-sig")
            if name == "logd":
                text = replace(text, "root_path", '"' + str(logs) + '"')
                if case == "log-backpressure":
                    text = replace(text, "max_pending_bytes", str(8 * 1024**2 + 8))
            if name == "redisserver" and case == "redis":
                text = replace(text, "connections", "1")
            if name == "redisserver" and pressure:
                text = replace(text, "connections", "1")
                maximum_bytes = 1024**2 if case.endswith("bytes") else 8 * 1024**2
                maximum_count = 1 if fairness else (128 if case.endswith("bytes") else 4)
                text = replace(text, "max_pending_bytes", str(maximum_bytes))
                text = replace(text, "max_pending_commands", str(maximum_count))
                if fairness: text = replace(text, "offset_ms", "1000")
            if name == "ipcclient":
                if case == "registry":
                    # 错误的直连后备端口，只有通过注册发现才能完成回声。
                    text = text.replace('port = "17981"', 'port = "17982"')
                if case == "redis-shutdown":
                    text = replace(text, "timeout_ms", "300")
                    text += "\n[Regression]\nredis_shutdown = true\n"
                if fairness:
                    text += "\n[Regression]\nredis_fairness = true\n"
                elif pressure:
                    text = replace(text, "timeout_ms", "10000")
                    text += "\n[Regression]\nredis_pressure = true\nredis_oversized = %s\n" % ("true" if case.endswith("bytes") else "false")
                if case == "redis-limit":
                    text = text.replace("[RedisClient]", "[RedisClient]\nmax_pending = 1")
                    text = replace(text, "tasks", "0")
                for flag in ("echo", "log", "redis", "hold"):
                    text = replace(text, flag, "true" if mode == flag else "false")
                if mode == "hold":
                    text = replace(text, "max_task_count", "1")
                    # Hold 的迟到回包晚 5 秒；后续 PING 的预算必须覆盖这段排队。
                    text = replace(text, "timeout_ms", "8000")
                if mode == "log":
                    text = text.replace("[LogClient]", "[LogClient]\nmax_send_queue_bytes = 262144")
                if case == "disk-full":
                    text = replace(text, "bytes", "131072")
                if case == "log-backpressure":
                    text = replace(text, "line", "131072")
                    text = replace(text, "bytes", str(64 * 1024**2))
                    text = replace(text, "drain_sec", "2")
            (cfg / (name + ".toml")).write_text(text, encoding="utf-8")
        if case == "disk-full":
            dated = logs / datetime.datetime.now().strftime("%y_%m_%d")
            dated.mkdir()
            for level in ("trace", "debug", "info", "warning", "error"):
                (dated / level).symlink_to("/dev/full")

        def start(name, command=None, env=None):
            output = root / (name + ".out")
            handle = output.open("w")
            handles.append(handle)
            proc = subprocess.Popen(command or [str(work / name)], cwd=work,
                                    stdout=handle, stderr=subprocess.STDOUT, env=env,
                                    start_new_session=True)
            procs.append(proc)
            return proc, output

        def ready(proc, path, marker):
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                text = path.read_text(errors="replace")
                if proc.poll() is not None:
                    raise RuntimeError(text)
                if marker in text:
                    return
                time.sleep(0.05)
            raise RuntimeError("start timeout: " + str(path))

        try:
            if mode == "hold":
                http_fixture = socketserver.ThreadingTCPServer(("127.0.0.1", 0), RawHttpRegression)
                http_fixture.daemon_threads = True
                http_thread = threading.Thread(target=http_fixture.serve_forever, kwargs={"poll_interval": 0.05})
                http_thread.start()
                for name in ("ipcclient", "ipcserver"):
                    with (cfg / (name + ".toml")).open("a", encoding="utf-8") as config:
                        config.write("\n[Regression]\nencoding_failure = true\nregistry_checks = true\nhttp_port = %d\n" % http_fixture.server_address[1])
            log_env = None
            if case == "log-backpressure":
                slow_write = root / "slow-write.so"
                subprocess.run(["gcc", "-shared", "-fPIC", "-O2", "-Wall", "-Werror",
                                str(REPO / "tools/log_slow_write.c"), "-o", str(slow_write), "-ldl", "-pthread"], check=True)
                log_env = dict(os.environ, LD_PRELOAD=str(slow_write))
            logd, logpath = start("logd", env=log_env)
            # 日志服没有 ready 文本，端口探测只用来确认监听，不发送日志。
            deadline = time.monotonic() + 10
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", 19995), timeout=0.1):
                        break
                except OSError:
                    if logd.poll() is not None or time.monotonic() > deadline:
                        if case == "disk-full" and "logd " in logpath.read_text(errors="replace"):
                            print(case, "PASS: disk error detected", flush=True)
                            return
                        raise RuntimeError(logpath.read_text(errors="replace"))
                    time.sleep(0.05)
            if mode in ("redis", "hold") or case == "registry":
                redis_conf = root / "redis.conf"
                redis_conf.write_text('bind 127.0.0.1\nport 6379\nsave ""\nappendonly no\n'
                                      'maxmemory 1536mb\nmaxmemory-policy allkeys-lru\n'
                                      'dir ' + str(root) + '\n')
                db, dbpath = start("redis", [str(args.redis_binary), str(redis_conf)])
                ready(db, dbpath, "Ready to accept connections")
                if fairness:
                    monitor = socket.create_connection(("127.0.0.1", 6379), timeout=2)
                    monitor.sendall(b"*1\r\n$7\r\nMONITOR\r\n")
                    assert monitor.recv(100) == b"+OK\r\n"
                if case == "registry" or mode == "hold":
                    registry, registry_path = start("registry")
                    ready(registry, registry_path, "registry server listen")
                if case != "registry":
                    rs, rsp = start("redisserver")
                    ready(rs, rsp, "redis server listen")
            server, serverpath = start("ipcserver")
            ready(server, serverpath, "ipc server listen")
            if case == "reset":
                reset_connections()
            env = os.environ.copy()
            if case.startswith("clock-"):
                if not args.clock_fault_so:
                    raise RuntimeError("clock test requires --clock-fault-so")
                env["LD_PRELOAD"] = str(args.clock_fault_so)
                env["OI_TEST_CLOCK_SHIFT"] = "86400" if case == "clock-forward" else "-3600"
            if pressure:
                db.send_signal(signal.SIGSTOP)
                db_paused = True
                rss_before = int(re.search(r"VmRSS:\s+(\d+)", Path("/proc/%d/status" % rs.pid).read_text()).group(1))
            started = time.monotonic()
            last_growth = started
            last_log_bytes = log_payload_on_disk(logs)
            client, clientpath = start("ipcclient", env=env)
            deadline = started + (20 if pressure or case == "redis-shutdown" else 210)
            while client.poll() is None:
                if case == "redis-shutdown" and not shutdown_sent and "redis shutdown pending" in clientpath.read_text(errors="replace"):
                    client.send_signal(signal.SIGTERM)
                    shutdown_sent = True
                    deadline = time.monotonic() + 3
                if pressure and db_paused:
                    if pause_at is None and "redis pressure pause" in rsp.read_text(errors="replace"):
                        pause_at = time.monotonic()
                    if pause_at is not None and time.monotonic() - pause_at > 0.5 and (not fairness or "redis fairness ready" in clientpath.read_text(errors="replace")):
                        rss_paused = int(re.search(r"VmRSS:\s+(\d+)", Path("/proc/%d/status" % rs.pid).read_text()).group(1))
                        assert rss_paused < rss_before + 32 * 1024, (rss_before, rss_paused)
                        print(case, "paused RSS KiB before=", rss_before, "after=", rss_paused, flush=True)
                        db.send_signal(signal.SIGCONT)
                        db_paused = False
                log_bytes = bytes_on_disk(logs)
                payload_bytes = log_payload_on_disk(logs) if mode == "log" else 0
                if payload_bytes > last_log_bytes:
                    last_growth, last_log_bytes = time.monotonic(), payload_bytes
                if log_bytes > CAP:
                    raise RuntimeError("test log exceeds 2GiB limit")
                if case == "disk-full" and logd.poll() is not None:
                    message = logpath.read_text(errors="replace")
                    if logd.returncode != 0 and "failed:" in message:
                        print(case, "PASS: disk error detected", flush=True)
                        return
                    raise RuntimeError(message)
                for proc in procs:
                    if proc is not client and proc.poll() is not None:
                        raise RuntimeError("service exited: " + str(proc.args))
                if time.monotonic() > deadline:
                    raise RuntimeError("client timeout\n" + clientpath.read_text(errors="replace"))
                time.sleep(0.1)
            text = clientpath.read_text(errors="replace")
            print(text, flush=True)
            assert client.returncode == 0, (case, client.returncode)
            if mode == "echo":
                assert "ok=1000000 mismatch=0 rpc_fail=0" in text
                assert text.count("ipc client connect sid=") == 1, "echo test started more than once"
            elif case == "redis-shutdown":
                assert shutdown_sent and "redis shutdown result=2 late=2 bad=0" in text
            elif pressure:
                assert pause_at is not None and not db_paused
                if fairness:
                    assert re.search(r"redis fairness done first=64 second=1 .*bad=0", text)
                    monitor.settimeout(0.2)
                    wire = b""
                    while True:
                        try:
                            part = monitor.recv(65536)
                            if not part: break
                            wire += part
                        except socket.timeout: break
                    order = re.findall(rb'"ECHO" "([AB])"', wire)
                    assert order.count(b"A") == 64 and order.count(b"B") == 1, order
                    assert order.index(b"B") <= 2, order
                    timestamps = [float(value) for value in re.findall(rb'\+(\d+\.\d+) .*"ECHO" "[AB]"', wire)]
                    duration_ms = (timestamps[-1] - timestamps[0]) * 1000
                    assert duration_ms < 1500, duration_ms
                    print("redis fairness upstream B position=", order.index(b"B") + 1, "duration_ms=", round(duration_ms, 3), flush=True)
                else: assert "redis pressure done total=64 bad=0" in text
                samples = re.findall(r"redis pressure pause link=\d+ pending=(\d+) bytes=(\d+)", rsp.read_text(errors="replace"))
                assert samples and all(int(count) <= maximum_count and int(size) <= maximum_bytes for count, size in samples)
                if case.endswith("count"): assert any(int(count) == maximum_count for count, size in samples)
            elif mode == "redis":
                assert "redis integrity ok" in text
                if case == "redis-limit":
                    assert "redis pending limit bad=0" in text
                else:
                    assert "redis script isolation bad=0" in text
                    assert "redis single use bad=0" in text
                    assert re.search(r"redis stress .*bad=0", text)
            elif mode == "hold":
                assert "hold timer exceptions executed=5 destroyed=5 bad=0" in text
                assert "hold nested results completed=32768 caught=16384 bad=0" in text
                assert "hold deferred rpc value=0 output=0 pinned=-1 bad=0" in text
                assert "hold binary bool bad=0" in text
                assert "hold connection isolation bad=0" in text
                assert "hold coroutine connect bad=0" in text
                assert "hold reentrant connect first=1 retry=0 connections=1 bad=0" in text
                assert "hold registry pairing bad=0" in text
                assert re.search(r"hold http release before=\d+ after=\d+ bad=0", text)
                assert "hold json validation bad=0" in text
                assert "hold task exceptions bad=0" in text
                assert "hold server encoding bad=0" in text
                assert "hold http framing bad=0" in text
                assert "hold rpc standardized reply bad=0" in text
                assert "hold done bad=0" in text
            elif mode == "log":
                # Stop drains accepted batches; inspect actual files and the final backlog accounting.
                logd.terminate()
                logd.wait(timeout=10)
                assert logd.returncode == 0, logpath.read_text(errors="replace")
                size = bytes_on_disk(logs)
                line_bytes, target_bytes = (131072, 64 * 1024**2) if case == "log-backpressure" else (65536, 1024**3)
                assert target_bytes <= size < CAP, size
                rows = 0
                for info in logs.rglob("info"):
                    with info.open("rb") as stream:
                        for line in stream:
                            assert re.match(rb"\d{2}:\d{2}:\d{2} ", line), line[:40]
                            assert len(line) == line_bytes + 9 and line[9:-1] == b"L" * (line_bytes - 1)
                            rows += 1
                assert rows == target_bytes // line_bytes, rows
                written_payload = rows * (line_bytes + 9)
                if written_payload > last_log_bytes:
                    last_growth = time.monotonic()
                duration = max(last_growth - started, 0.001)
                print("log written bytes=", size, "payload_bytes=", written_payload,
                      "end_to_end_MBps=", round(written_payload / duration / 1024**2, 2), flush=True)
                if case == "log-backpressure":
                    backlog = re.search(r"logd backlog peak=(\d+) limit=(\d+) pauses=(\d+) pending=(\d+)", logpath.read_text(errors="replace"))
                    assert backlog, logpath.read_text(errors="replace")
                    peak, limit, pauses, pending = map(int, backlog.groups())
                    assert peak <= limit == 8 * 1024**2 + 8 and pauses > 0 and pending == 0, backlog.group(0)
                    print(backlog.group(0), flush=True)
            print(case, "PASS seconds=", round(time.monotonic() - started, 2), flush=True)
        except BaseException:
            for output in root.glob("*.out"):
                print(output.name, output.read_text(errors="replace")[-6000:], flush=True)
            raise
        finally:
            if db_paused and db.poll() is None:
                db.send_signal(signal.SIGCONT)
            stuck = []
            for proc in reversed(procs):
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        stuck.append(str(proc.args))
                        proc.kill()
                        proc.wait()
            if monitor is not None: monitor.close()
            if http_fixture is not None:
                http_fixture.shutdown()
                http_fixture.server_close()
                http_thread.join()
            for handle in handles:
                handle.close()
            if stuck:
                raise RuntimeError("graceful shutdown timed out: " + ", ".join(stuck))


def run_web(args):
    # 仍使用模块自带的 WebClient 用例，验证字段改名后的 HTTP/JSON/二进制收发。
    with tempfile.TemporaryDirectory(prefix="oi-regression-web-") as tmp:
        root = Path(tmp)
        work, cfg = root / "bin", root / "config"
        work.mkdir()
        cfg.mkdir()
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        for name in ("webserver", "webclient"):
            shutil.copy2(args.bin_dir / name, work / name)
            text = (REPO / "deploy/config" / (name + ".toml")).read_text(encoding="utf-8-sig")
            text = text.replace("port = 18080", "port = " + str(port))
            (cfg / (name + ".toml")).write_text(text, encoding="utf-8")
        with (root / "server.out").open("w") as output:
            server = subprocess.Popen([str(work / "webserver")], cwd=work, stdout=output, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 10
                while True:
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2): break
                    except OSError:
                        if server.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError((root / "server.out").read_text())
                        time.sleep(0.05)
                client = subprocess.run([str(work / "webclient")], cwd=work, capture_output=True, text=True, timeout=30)
                print(client.stdout, flush=True)
                assert client.returncode == 0 and "rpc=1 bin=1 bad=0" in client.stdout, client.stderr
                print("web PASS", flush=True)
            finally:
                if server.poll() is None:
                    server.terminate()
                    try: server.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait()
                        raise RuntimeError("webserver shutdown timed out")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", default="all", choices=("all", "echo", "log", "log-backpressure", "redis", "redis-limit", "redis-shutdown", "redis-backpressure-bytes", "redis-backpressure-count", "redis-fairness", "hold", "reset", "disk-full", "clock-forward", "clock-back", "registry", "web"))
    parser.add_argument("--bin-dir", type=Path, default=REPO / "deploy/bin")
    parser.add_argument("--redis-binary", type=Path, default=Path("/tmp/redis-7.4.12/src/redis-server"))
    parser.add_argument("--clock-fault-so", type=Path)
    options = parser.parse_args()
    for port in (19995, 17981, 16379, 6379, 17990):
        with socket.socket() as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind(("127.0.0.1", port))
    cases = ("echo", "log", "log-backpressure", "redis", "redis-limit", "redis-shutdown", "redis-backpressure-bytes", "redis-backpressure-count", "redis-fairness", "hold", "reset", "disk-full", "registry", "web") if options.case == "all" else (options.case,)
    for item in cases:
        if item == "web":
            run_web(options)
        else:
            run(item, options)
