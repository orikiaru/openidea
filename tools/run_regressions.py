"""WSL: python3 tools/run_regressions.py --case all [--clock-fault-so /tmp/oi-clock.so].

只启动仓库现有进程，临时配置和日志自动清理；不会终止不属于本次测试的进程。
先编出待测版本，单、多线程分别运行一次。Redis 默认使用已有的源码编译产物。
"""
from test_process_owner import write_owner, record_process
import argparse
import contextlib
import sys
import datetime
import os
from pathlib import Path
import re
try:
    import resource
except ImportError:
    resource = None
import json
import traceback
from regression_platform import WINDOWS, SUFFIX, launch, stop, pause, rss_kib, WslRedis
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
# Windows 动态端口段（本机 1024-15000）可能被 Hyper-V/WinNAT 保留，测试 Redis 不用 6379。
REDIS_PORT = 16380
LOGD_PORT = 19995
IPC_PORT = 17981
IPC_FALLBACK_PORT = 17982
REDIS_SERVER_PORT = 16379
REGISTRY_PORT = 17990
GAME_PORTS = (17992, 17993)
# --port-offset 平移的端口；多份工作目录并行跑时各用一段，互不抢占。
BASE_PORTS = (REDIS_PORT, LOGD_PORT, IPC_PORT, IPC_FALLBACK_PORT, REDIS_SERVER_PORT, REGISTRY_PORT) + GAME_PORTS
PORT_OFFSET = 0
CLIENT_SECONDS = {"hold": 120}


@contextlib.contextmanager
def regression_directory(**kwargs):
    directory = tempfile.TemporaryDirectory(**kwargs)
    try:
        yield directory.name
    finally:
        primary = sys.exc_info()[1]
        try:
            directory.cleanup()
        except BaseException as error:
            if primary is None:
                raise
            print("temporary directory cleanup failed:", repr(error), file=sys.stderr, flush=True)


def cleanup_owned(procs, handles, root, args, case, *, db=None, db_paused=False,
                  monitor=None, http_fixture=None, http_thread=None, primary=None,
                  registry=None, registry_paused=False):
    """Attempt every owned cleanup action, without masking the original test error."""
    errors = []

    def attempt(label, action):
        try:
            action()
            return True
        except BaseException as error:
            errors.append((label, error))
            return False

    if db_paused:
        attempt("resume Redis", lambda: pause(db, False) if db.poll() is None else None)
    if registry_paused:
        attempt("resume registry", lambda: pause(registry, False) if registry.poll() is None else None)
    for proc in reversed(procs):
        def graceful(proc=proc):
            if proc.poll() is not None:
                return
            if proc is db and WINDOWS and not args.redis_wsl:
                # Only the owned native Redis fixture uses protocol shutdown.
                try:
                    with socket.create_connection(("127.0.0.1", REDIS_PORT), timeout=2) as conn:
                        conn.sendall(b"*2\r\n$8\r\nSHUTDOWN\r\n$6\r\nNOSAVE\r\n")
                        conn.recv(64)
                except OSError:
                    pass
            else:
                stop(proc)
            proc.wait(timeout=3)

        if not attempt("graceful shutdown " + str(proc.args), graceful):
            # Windows Popen.kill uses the held process handle, not a fresh PID lookup.
            def force(proc=proc):
                if proc.poll() is None:
                    proc.kill()
            attempt("force shutdown " + str(proc.args), force)
            attempt("reap " + str(proc.args), lambda proc=proc: proc.wait(timeout=3))
    if monitor is not None:
        attempt("monitor close", monitor.close)
    if http_fixture is not None:
        attempt("HTTP shutdown", http_fixture.shutdown)
        attempt("HTTP close", http_fixture.server_close)
        def join_http():
            http_thread.join(timeout=3)
            if http_thread.is_alive():
                raise RuntimeError("HTTP fixture thread did not exit")
        attempt("HTTP join", join_http)
    for handle in handles:
        attempt("output close", handle.close)
    if args.artifacts:
        destination = args.artifacts / case
        attempt("artifact directory", lambda: destination.mkdir(parents=True, exist_ok=True))
        def copy_outputs():
            for output in root.glob("*.out"):
                attempt("artifact " + output.name, lambda output=output: shutil.copy2(output, destination / output.name))
        attempt("artifact enumeration", copy_outputs)
    if errors:
        message = "cleanup failed: " + "; ".join(label + ": " + repr(error) for label, error in errors)
        print(message, file=sys.stderr, flush=True)
        if primary is None:
            for _, error in errors:
                if not isinstance(error, Exception):
                    raise error
            raise RuntimeError(message) from errors[0][1]


def apply_port_offset(offset):
    global REDIS_PORT, LOGD_PORT, IPC_PORT, IPC_FALLBACK_PORT, REDIS_SERVER_PORT, REGISTRY_PORT, GAME_PORTS, PORT_OFFSET
    PORT_OFFSET = offset
    REDIS_PORT += offset
    LOGD_PORT += offset
    IPC_PORT += offset
    IPC_FALLBACK_PORT += offset
    REDIS_SERVER_PORT += offset
    REGISTRY_PORT += offset
    GAME_PORTS = tuple(port + offset for port in GAME_PORTS)


def shift_ports(text):
    def shifted(match):
        port = int(match.group(2))
        return match.group(1) + str(port + PORT_OFFSET if port in BASE_PORTS else port) + match.group(3)
    return re.sub(r'(?m)^(port\s*=\s*")(\d+)(")', shifted, text)


def replace(text, key, value):
    return re.sub(r"(?m)^" + re.escape(key) + r"\s*=.*$", key + " = " + value, text)


def bytes_on_disk(root):
    return sum(p.stat().st_size for p in root.rglob("*") if p.is_file() and not p.is_symlink())


def log_payload_on_disk(root):
    return sum(p.stat().st_size for p in root.rglob("info") if p.is_file() and not p.is_symlink())


def reset_connections():
    # 故障注入：收到 RST 后不能在会话表留下已释放的指针。
    for _ in range(100):
        with socket.create_connection(("127.0.0.1", IPC_PORT), timeout=2) as s:
            s.sendall(b"\x00")
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("HH" if WINDOWS else "ii", 1, 0))


def rpc_ingress_faults():
    # 只注入畸形/分片网络数据；正常负载仍由 ipcclient 的 EchoMgr 发出。
    def closed(stream):
        try:
            assert stream.recv(1) == b"", "invalid frame accepted"
        except ConnectionResetError:
            pass

    def receive(stream, count):
        result = b""
        while len(result) < count:
            part = stream.recv(count - len(result))
            assert part, "valid fragmented frame disconnected"
            result += part
        return result

    with socket.create_connection(("127.0.0.1", IPC_PORT), timeout=1) as stream:
        stream.sendall(struct.pack("<II", 3002, 65537))
        closed(stream)  # 只发头，不应等正文或残包超时。
    for partial in (b"\x01", struct.pack("<IIQ", 3002, 65536, 1 << 63)):
        with socket.create_connection(("127.0.0.1", IPC_PORT), timeout=1) as stream:
            started = time.monotonic()
            stream.sendall(partial)
            time.sleep(0.15)
            stream.sendall(b"\x00")  # 滴流不刷新 250ms 的绝对期限。
            closed(stream)
            assert time.monotonic() - started < 0.39, "partial deadline extended"
    with socket.create_connection(("127.0.0.1", IPC_PORT), timeout=2) as stream:
        for call_id in range(1, 4):
            body = struct.pack("<QIIiIIi", (1 << 63) | call_id, 0, 4, 0, 1, 4, 42)
            body += struct.pack("<II", 99, 65536 - len(body) - 8) + b"x" * (65536 - len(body) - 8)
            frame = struct.pack("<II", 3002, len(body)) + body
            stream.sendall(frame[:5])
            time.sleep(0.03)
            stream.sendall(frame[5:])
            proto, size = struct.unpack("<II", receive(stream, 8))
            response = receive(stream, size)
            assert proto == 3002 and struct.unpack_from("<Q", response)[0] == call_id
            assert struct.unpack_from("<i", response, 8)[0] == 0, response
            time.sleep(0.3)  # 完整包必须取消旧计时器，下一包重新计时。
    print("rpc ingress oversized/partial/drip/boundary/rearm PASS", flush=True)


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
    mode = "hold" if case.startswith(("clock-", "rpc-admission-")) else ("echo" if case in ("reset", "registry", "rpc-ingress") else case)
    mode = "log" if case in ("disk-full", "log-backpressure") else mode
    mode = "redis" if case.startswith("redis-") else mode
    fairness = case == "redis-fairness"
    pressure = case.startswith("redis-backpressure-") or fairness
    db = None
    db_paused = False
    registry = None
    registry_paused = False
    registry_fault_command = ""
    registry_resume_at = None
    pause_at = None
    shutdown_sent = False
    procs = []
    handles = []
    http_fixture = None
    http_thread = None
    monitor = None
    if resource is not None:
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    # 前缀带偏移，并行时每路只按自己的临时目录清理进程。
    with regression_directory(prefix="oi-regression-p%d-" % PORT_OFFSET) as tmp:
        root = Path(tmp)
        write_owner(root, REPO, PORT_OFFSET)
        work, cfg, logs = root / "bin", root / "config", root / "logs"
        for p in (work, cfg, logs):
            p.mkdir()
        for name in ("logd", "ipcclient", "ipcserver", "redisserver") + (("registry",) if case == "registry" or mode == "hold" else ()):
            shutil.copy2(args.bin_dir / (name + SUFFIX), work / (name + SUFFIX))
            text = shift_ports((REPO / "deploy/config" / (name + ".toml")).read_text(encoding="utf-8-sig"))
            if name in ("redisserver", "registry"):
                assert 'port = "6379"' in text, name
                text = text.replace('port = "6379"', 'port = "%d"' % REDIS_PORT)
            if name == "logd":
                text = replace(text, "root_path", '"' + logs.as_posix() + '"')
                if case == "log-backpressure":
                    text = replace(text, "max_pending_bytes", str(8 * 1024**2 + 8))
            if name == "ipcserver" and case.startswith("rpc-admission-"):
                # 只匹配真正的段头，配置注释也可能提到 [Rpc]。
                text = re.sub(r"(?ms)^\[Rpc\]\s*\n.*?(?=^\[|\Z)", "", text)
            if name == "ipcserver" and case == "rpc-ingress":
                text = replace(text, "max_frame_bytes", "65536")
                text = replace(text, "max_receive_buffer_bytes", "65544")
                text = replace(text, "incomplete_frame_timeout_ms", "250")
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
                if case == "redis-queue-timeout":
                    text = replace(text, "timeout_ms", "100")
                    text += "\n[Regression]\nredis_queue_timeout = true\n"
                if case == "registry":
                    # 错误的直连后备端口，只有通过注册发现才能完成回声。
                    text = text.replace('port = "%d"' % IPC_PORT, 'port = "%d"' % IPC_FALLBACK_PORT)
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
                    text = replace(text, "max_pending_pulls", "16")
                    text = replace(text, "pull_ms", "60000")
                    text = replace(text, "max_clients", "8")
                    text = replace(text, "idle_ms", "200")
                    # Hold 的 Redis 迟到回包比超时晚 1 秒；后续 PING 的预算必须覆盖这段排队。
                    text = replace(text, "timeout_ms", "2000")
                if mode == "log":
                    text = text.replace("[LogClient]", "[LogClient]\nmax_send_queue_bytes = 262144")
                if case == "disk-full":
                    text = replace(text, "bytes", "131072")
                if case == "log-backpressure":
                    text = replace(text, "line", "131072")
                    text = replace(text, "bytes", str(64 * 1024**2))
                    text = replace(text, "drain_sec", "2")
            (cfg / (name + ".toml")).write_text(text, encoding="utf-8")
        if case == "disk-full" and not WINDOWS:
            dated = logs / datetime.datetime.now().strftime("%y_%m_%d")
            dated.mkdir()
            for level in ("trace", "debug", "info", "warning", "error"):
                (dated / level).symlink_to("/dev/full")

        def start(name, command=None, env=None):
            output = root / (name + ".out")
            handle = output.open("w", encoding="utf-8")
            handles.append(handle)
            if name == "redis" and args.redis_wsl:
                proc = WslRedis(command[0], Path(command[1]), args.wsl_user, cwd=work,
                                stdout=handle, stderr=subprocess.STDOUT, env=env)
            else:
                proc = launch(command or [str(work / (name + SUFFIX))], cwd=work,
                              stdout=handle, stderr=subprocess.STDOUT, env=env)
            procs.append(proc)
            record_process(root, proc)
            return proc, output

        def ready(proc, path, marker):
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                text = path.read_text(encoding="utf-8", errors="replace")
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
                        if name == "ipcclient":
                            config.write('registry_fault_path = "%s"\n' % (root / "registry-fault").as_posix())
            if case.startswith("rpc-admission-"):
                with (cfg / "ipcclient.toml").open("a") as config:
                    config.write("rpc_admission = true\nrpc_admission_bytes = %s\n" % ("true" if case.endswith("bytes") else "false"))
                with (cfg / "ipcserver.toml").open("a") as config:
                    config.write("\n[Rpc]\nmax_inflight_count = %d\nsession_max_inflight_count = %d\nmax_inflight_bytes = %d\nsession_max_inflight_bytes = %d\n" % ((100, 100, 64, 32) if case.endswith("bytes") else (2, 1, 1048576, 1048576)))
            log_env = None
            if case == "log-backpressure" and not WINDOWS:
                slow_write = root / "slow-write.so"
                subprocess.run(["gcc", "-shared", "-fPIC", "-O2", "-Wall", "-Werror",
                                str(REPO / "tools/log_slow_write.c"), "-o", str(slow_write), "-ldl", "-pthread"], check=True)
                log_env = dict(os.environ, LD_PRELOAD=str(slow_write))
            if WINDOWS and case in ("disk-full", "log-backpressure"):
                stamp = args.bin_dir / "oi_test_hooks.txt"
                if not stamp.exists() or stamp.read_text().strip() != "ON":
                    raise RuntimeError("Windows fault cases require -DOI_ENABLE_TEST_HOOKS=ON")
                log_env = dict(os.environ, OI_TEST_LOG_FAULT="full" if case == "disk-full" else "slow")
            logd, logpath = start("logd", env=log_env)
            # 日志服没有 ready 文本，端口探测只用来确认监听，不发送日志。
            deadline = time.monotonic() + 10
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", LOGD_PORT), timeout=0.1):
                        break
                except OSError:
                    if logd.poll() is not None or time.monotonic() > deadline:
                        if case == "disk-full" and "logd " in logpath.read_text(encoding="utf-8", errors="replace"):
                            print(case, "PASS: disk error detected", flush=True)
                            return
                        raise RuntimeError(logpath.read_text(encoding="utf-8", errors="replace"))
                    time.sleep(0.05)
            if mode in ("redis", "hold") or case == "registry":
                redis_conf = root / "redis.conf"
                redis_conf.write_text('bind 127.0.0.1\nport %d\nsave ""\nappendonly no\n' % REDIS_PORT +
                                      'maxmemory 1536mb\nmaxmemory-policy allkeys-lru\n'
                                      'dir ' + (str(root) if args.redis_wsl else '..') + '\n')
                # msys2 版 redis-server.exe 不认盘符绝对路径，原生启动时按 cwd=bin 传相对路径。
                db, dbpath = start("redis", [str(args.redis_binary), str(redis_conf) if args.redis_wsl else "../redis.conf"])
                ready(db, dbpath, "Ready to accept connections")
                if fairness:
                    monitor = socket.create_connection(("127.0.0.1", REDIS_PORT), timeout=2)
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
            if mode == "hold":
                for zone, port in zip(("1001", "1002"), GAME_PORTS):
                    text = (cfg / "ipcserver.toml").read_text(encoding="utf-8")
                    text = text.replace('name = "ipcserver"', 'name = "gameserver"', 1)
                    text = re.sub(r"(?m)^id = 1$", 'id = %s\nzone = "%s"' % (zone, zone), text, count=1)
                    text = text.replace('port = "%d"' % IPC_PORT, 'port = "%d"' % port, 1)
                    path = cfg / ("gameserver-%s.toml" % zone)
                    path.write_text(text, encoding="utf-8")
                    proc, output = start("gameserver-" + zone, [str(work / ("ipcserver" + SUFFIX)), str(path)])
                    ready(proc, output, "ipc server listen")
            if case == "reset":
                reset_connections()
            if case == "rpc-ingress":
                rpc_ingress_faults()
            env = os.environ.copy()
            if case.startswith("clock-"):
                if not args.clock_fault_so:
                    raise RuntimeError("clock test requires --clock-fault-so")
                env["LD_PRELOAD"] = str(args.clock_fault_so)
                env["OI_TEST_CLOCK_SHIFT"] = "86400" if case == "clock-forward" else "-3600"
            if pressure:
                pause(db, True)
                db_paused = True
                rss_before = rss_kib(rs)
            started = time.monotonic()
            last_growth = started
            last_log_bytes = log_payload_on_disk(logs)
            client, clientpath = start("ipcclient", env=env)
            # 正常通过时 hold 约 50 秒，其余不超过 35 秒；留三四倍余量，失败时不用干等。
            deadline = started + (20 if pressure or case == "redis-shutdown" else CLIENT_SECONDS.get(mode, 60))
            while client.poll() is None:
                if mode == "hold":
                    request_path = root / "registry-fault.request"
                    command = request_path.read_text(encoding="utf-8").strip() if request_path.exists() else ""
                    if command in ("pause", "resume", "budget") and command != registry_fault_command:
                        if command in ("pause", "budget"):
                            registry_paused = True  # finally also resumes if suspension partly fails.
                            pause(registry, True)
                            registry_resume_at = time.monotonic() + 0.6 if command == "budget" else None
                        else:
                            pause(registry, False)
                            registry_paused = False
                            registry_resume_at = None
                        registry_fault_command = command
                        (root / "registry-fault.ack").write_text(command, encoding="utf-8")
                    if registry_paused and registry_resume_at is not None and time.monotonic() >= registry_resume_at:
                        pause(registry, False)
                        registry_paused = False
                        registry_resume_at = None
                if case == "redis-shutdown" and not shutdown_sent and "redis shutdown pending" in clientpath.read_text(encoding="utf-8", errors="replace"):
                    stop(client)
                    shutdown_sent = True
                    deadline = time.monotonic() + 3
                if pressure and db_paused:
                    if pause_at is None and "redis pressure pause" in rsp.read_text(encoding="utf-8", errors="replace"):
                        pause_at = time.monotonic()
                    if pause_at is not None and time.monotonic() - pause_at > 0.5 and (not fairness or "redis fairness ready" in clientpath.read_text(encoding="utf-8", errors="replace")):
                        rss_paused = rss_kib(rs)
                        assert rss_paused < rss_before + 32 * 1024, (rss_before, rss_paused)
                        print(case, "paused RSS KiB before=", rss_before, "after=", rss_paused, flush=True)
                        pause(db, False)
                        db_paused = False
                log_bytes = bytes_on_disk(logs)
                payload_bytes = log_payload_on_disk(logs) if mode == "log" else 0
                if payload_bytes > last_log_bytes:
                    last_growth, last_log_bytes = time.monotonic(), payload_bytes
                if log_bytes > CAP:
                    raise RuntimeError("test log exceeds 2GiB limit")
                if case == "disk-full" and logd.poll() is not None:
                    message = logpath.read_text(encoding="utf-8", errors="replace")
                    if logd.returncode != 0 and "failed:" in message:
                        print(case, "PASS: disk error detected", flush=True)
                        return
                    raise RuntimeError(message)
                for proc in procs:
                    if proc is not client and proc.poll() is not None:
                        raise RuntimeError("service exited: " + str(proc.args))
                if time.monotonic() > deadline:
                    raise RuntimeError("client timeout\n" + clientpath.read_text(encoding="utf-8", errors="replace"))
                time.sleep(0.1)
            text = clientpath.read_text(encoding="utf-8", errors="replace")
            print(text, flush=True)
            assert client.returncode == 0, (case, client.returncode)
            if mode == "echo":
                assert "ok=1000000 mismatch=0 rpc_fail=0" in text
                assert text.count("ipc client connect sid=") == 1, "echo test started more than once"
            elif case == "redis-shutdown":
                assert shutdown_sent and "redis shutdown result=2 late=2 bad=0" in text
            elif case == "redis-queue-timeout":
                assert re.search(r"redis queue timeout elapsed=\d+ bad=0", text)
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
                samples = re.findall(r"redis pressure pause link=\d+ pending=(\d+) bytes=(\d+)", rsp.read_text(encoding="utf-8", errors="replace"))
                assert samples and all(int(count) <= maximum_count and int(size) <= maximum_bytes for count, size in samples)
                if case.endswith("count"): assert any(int(count) == maximum_count for count, size in samples)
            elif mode == "redis":
                assert "redis integrity ok" in text
                if case == "redis-limit":
                    assert "redis pending limit bad=0" in text
                else:
                    assert "redis millisecond expiry bad=0" in text
                    assert "redis script isolation bad=0" in text
                    assert "redis single use bad=0" in text
                    assert re.search(r"redis stress .*bad=0", text)
            elif case.startswith("rpc-admission-"):
                assert "hold rpc admission done bad=0" in text
                assert text.count("ok=2 busy=4 bad=0") == 3
            elif mode == "hold":
                assert "hold rpc session index calls=4096 bad=0" in text
                assert "hold timer exceptions executed=5 destroyed=5 bad=0" in text
                assert "hold nested results completed=32768 caught=16384 bad=0" in text
                assert "hold hot start inline=1 queued=1 bad=0" in text
                assert "hold shared tasks waiters=64 bad=0" in text
                for waiter_count in (2, 8, 64, 256):
                    for shared_mode in range(3):
                        assert f"hold started task mode={shared_mode} waiters={waiter_count} runs=1 awakened={waiter_count} bad=0" in text
                assert "hold deferred rpc value=0 output=0 pinned=-1 bad=0" in text
                assert "hold await ownership shared=32 bad=0" in text
                assert "hold output rpc shared waiters=16 submissions=1 bad=0" in text
                assert "hold output rpc direct pinned error late bad=0" in text
                assert "hold output rpc named any zone shared=8 bad=0" in text
                assert "hold completed rpc requests retained=20 destroyed=20 bad=0" in text
                assert "hold route policy isolation idle reconnect bad=0" in text
                assert "hold pooled reconnect bad=0" in text
                assert "hold inbound route lifetime bad=0" in text
                assert "hold reverse reply bad=0" in (root / "gameserver-1001.out").read_text(encoding="utf-8", errors="replace")
                assert "hold batch failure slots bad=0" in text
                assert re.search(r"hold absolute deadline buckets calls=4096 buckets=\d+ fired=32 bad=0", text)
                assert "hold route pool capacity busy=1 bad=0" in text
                assert re.search(r"hold registry bounded recovery busy=[1-9]\d* timeout=[1-9]\d* bad=0", text)
                budget = re.search(r"hold named rpc end to end budget elapsed_ms=(\d+) bad=0", text)
                assert budget and int(budget.group(1)) < 1450, budget.group(0) if budget else "missing budget marker"
                assert "hold binary bool bad=0" in text
                assert "hold connection isolation bad=0" in text
                assert "hold coroutine connect bad=0" in text
                assert "hold reentrant connect first=1 retry=0 connections=1 bad=0" in text
                assert "hold registry pairing bad=0" in text
                assert "hold route target stability bad=0" in text
                assert re.search(r"hold service rpc global=1 any=1 zone=1 batch=2 many=2 slot=1 elapsed_ms=\d+ wrong=1 miss=1 bad=0", text)
                assert "hold named rpc ownership success=8 rejected=0 bad=0" in text
                assert re.search(r"hold http release before=\d+ after=\d+ bad=0", text)
                assert "hold json validation bad=0" in text
                assert "hold task exceptions bad=0" in text
                assert "hold server encoding bad=0" in text
                assert "hold http framing bad=0" in text
                assert "hold rpc standardized reply bad=0" in text
                assert "hold done bad=0" in text
            elif mode == "log":
                # Stop drains accepted batches; inspect actual files and the final backlog accounting.
                stop(logd)
                logd.wait(timeout=10)
                assert logd.returncode == 0, logpath.read_text(encoding="utf-8", errors="replace")
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
                    backlog = re.search(r"logd backlog peak=(\d+) limit=(\d+) pauses=(\d+) pending=(\d+)", logpath.read_text(encoding="utf-8", errors="replace"))
                    assert backlog, logpath.read_text(encoding="utf-8", errors="replace")
                    peak, limit, pauses, pending = map(int, backlog.groups())
                    assert peak <= limit == 8 * 1024**2 + 8 and pauses > 0 and pending == 0, backlog.group(0)
                    print(backlog.group(0), flush=True)
            print(case, "PASS seconds=", round(time.monotonic() - started, 2), flush=True)
        except BaseException:
            try:
                for output in root.glob("*.out"):
                    print(output.name, output.read_text(encoding="utf-8", errors="replace")[-6000:], flush=True)
            except BaseException as error:
                print("failure output unavailable:", repr(error), file=sys.stderr, flush=True)
            raise
        finally:
            cleanup_owned(procs, handles, root, args, case, db=db, db_paused=db_paused,
                          monitor=monitor, http_fixture=http_fixture, http_thread=http_thread,
                          primary=sys.exc_info()[1], registry=registry, registry_paused=registry_paused)


def run_web(args):
    # 仍使用模块自带的 WebClient 用例，验证字段改名后的 HTTP/JSON/二进制收发。
    with regression_directory(prefix="oi-regression-web-p%d-" % PORT_OFFSET) as tmp:
        root = Path(tmp)
        write_owner(root, REPO, PORT_OFFSET)
        work, cfg = root / "bin", root / "config"
        work.mkdir()
        cfg.mkdir()
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        for name in ("webserver", "webclient"):
            shutil.copy2(args.bin_dir / (name + SUFFIX), work / (name + SUFFIX))
            text = shift_ports((REPO / "deploy/config" / (name + ".toml")).read_text(encoding="utf-8-sig"))
            text = text.replace("port = 18080", "port = " + str(port))
            (cfg / (name + ".toml")).write_text(text, encoding="utf-8")
        with (root / "server.out").open("w") as output:
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
                client = subprocess.run([str(work / ("webclient" + SUFFIX))], cwd=work, capture_output=True, text=True, timeout=30)
                print(client.stdout, flush=True)
                assert client.returncode == 0 and "rpc=1 bin=1 bad=0" in client.stdout, client.stderr
                print("web PASS", flush=True)
            finally:
                cleanup_owned([server], [output], root, args, "web", primary=sys.exc_info()[1])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", default="all", choices=("all", "echo", "log", "log-backpressure", "redis", "redis-limit", "redis-shutdown", "redis-queue-timeout", "rpc-ingress", "redis-backpressure-bytes", "redis-backpressure-count", "redis-fairness", "hold", "rpc-admission-count", "rpc-admission-bytes", "reset", "disk-full", "clock-forward", "clock-back", "registry", "web"))
    parser.add_argument("--bin-dir", type=Path, default=REPO / "deploy/bin")
    parser.add_argument("--redis-binary", type=Path, default=Path("/tmp/redis-7.4.12/src/redis-server"))
    parser.add_argument("--clock-fault-so", type=Path)
    parser.add_argument("--redis-wsl", action="store_true", help="Windows: run only the owned Redis fixture in WSL")
    parser.add_argument("--wsl-user", default="jxy")
    parser.add_argument("--artifacts", type=Path, help="retain process output and results.json, not bulk log payload")
    parser.add_argument("--port-offset", type=int, default=0, help="shift every test port; parallel work copies need distinct offsets")
    options = parser.parse_args()
    if options.redis_wsl and not WINDOWS:
        parser.error("--redis-wsl is only for native Windows tests")
    if WINDOWS and options.case.startswith("clock-"):
        parser.error("clock fault injection requires Linux LD_PRELOAD; never silently skip it")
    if options.artifacts:
        options.artifacts = options.artifacts.resolve()
        options.artifacts.mkdir(parents=True, exist_ok=True)
    apply_port_offset(options.port_offset)
    for port in (LOGD_PORT, IPC_PORT, REDIS_SERVER_PORT, REDIS_PORT, REGISTRY_PORT) + GAME_PORTS:
        with socket.socket() as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind(("127.0.0.1", port))
    cases = ("echo", "log", "log-backpressure", "redis", "redis-limit", "redis-shutdown", "redis-queue-timeout", "rpc-ingress", "redis-backpressure-bytes", "redis-backpressure-count", "redis-fairness", "hold", "rpc-admission-count", "rpc-admission-bytes", "reset", "disk-full", "registry", "web") if options.case == "all" else (options.case,)
    results = []
    for item in cases:
        started = time.monotonic()
        result = {"case": item, "platform": "windows" if WINDOWS else "linux"}
        try:
            if item == "web":
                run_web(options)
            else:
                run(item, options)
            result["status"] = "passed"
        except Exception as error:
            result.update(status="failed", error=str(error))
            traceback.print_exc()
        finally:
            result["seconds"] = round(time.monotonic() - started, 3)
            results.append(result)
            if options.artifacts:
                (options.artifacts / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    raise SystemExit(1 if any(item["status"] != "passed" for item in results) else 0)
