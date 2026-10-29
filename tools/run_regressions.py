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
import shutil
import socket
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


def reset_connections():
    # 故障注入：收到 RST 后不能在会话表留下已释放的指针。
    for _ in range(100):
        with socket.create_connection(("127.0.0.1", 17981), timeout=2) as s:
            s.sendall(b"\x00")
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))


def run(case, args):
    mode = "hold" if case.startswith("clock-") else ("echo" if case == "reset" else case)
    mode = "log" if case == "disk-full" else mode
    procs = []
    handles = []
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix="oi-regression-") as tmp:
        root = Path(tmp)
        work, cfg, logs = root / "bin", root / "config", root / "logs"
        for p in (work, cfg, logs):
            p.mkdir()
        for name in ("logd", "ipcclient", "ipcserver", "redisserver"):
            shutil.copy2(args.bin_dir / name, work / name)
            text = (REPO / "deploy/config" / (name + ".toml")).read_text(encoding="utf-8-sig")
            if name == "logd":
                text = replace(text, "root_path", '"' + str(logs) + '"')
            if name == "ipcclient":
                for flag in ("echo", "log", "redis", "hold"):
                    text = replace(text, flag, "true" if mode == flag else "false")
                if mode == "hold":
                    # Hold 的迟到回包晚 5 秒；后续 PING 的预算必须覆盖这段排队。
                    text = replace(text, "timeout_ms", "8000")
                if case == "disk-full":
                    text = replace(text, "bytes", "131072")
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
            logd, logpath = start("logd")
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
            if mode in ("redis", "hold"):
                redis_conf = root / "redis.conf"
                redis_conf.write_text('bind 127.0.0.1\nport 6379\nsave ""\nappendonly no\n'
                                      'maxmemory 1536mb\nmaxmemory-policy allkeys-lru\n'
                                      'dir ' + str(root) + '\n')
                db, dbpath = start("redis", [str(args.redis_binary), str(redis_conf)])
                ready(db, dbpath, "Ready to accept connections")
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
            started = time.monotonic()
            client, clientpath = start("ipcclient", env=env)
            deadline = started + 210
            while client.poll() is None:
                if bytes_on_disk(logs) > CAP:
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
            elif mode == "redis":
                assert "redis integrity ok" in text and re.search(r"redis stress .*bad=0", text)
            elif mode == "hold":
                assert "hold done bad=0" in text
            elif mode == "log":
                size = bytes_on_disk(logs)
                assert 1024**3 <= size < CAP, size
                rows = 0
                for info in logs.rglob("info"):
                    with info.open("rb") as stream:
                        for line in stream:
                            assert re.match(rb"\d{2}:\d{2}:\d{2} ", line), line[:40]
                            assert len(line) == 65536 + 9 and line[9:-1] == b"L" * 65535
                            rows += 1
                assert rows == 16384, rows
                print("log written bytes=", size, flush=True)
            print(case, "PASS seconds=", round(time.monotonic() - started, 2), flush=True)
        except BaseException:
            for output in root.glob("*.out"):
                print(output.name, output.read_text(errors="replace")[-6000:], flush=True)
            raise
        finally:
            for proc in reversed(procs):
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
            for handle in handles:
                handle.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", default="all", choices=("all", "echo", "log", "redis", "hold", "reset", "disk-full", "clock-forward", "clock-back"))
    parser.add_argument("--bin-dir", type=Path, default=REPO / "deploy/bin")
    parser.add_argument("--redis-binary", type=Path, default=Path("/tmp/redis-7.4.12/src/redis-server"))
    parser.add_argument("--clock-fault-so", type=Path)
    options = parser.parse_args()
    for port in (19995, 17981, 16379, 6379):
        with socket.socket() as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind(("127.0.0.1", port))
    cases = ("echo", "log", "redis", "hold", "reset", "disk-full") if options.case == "all" else (options.case,)
    for item in cases:
        run(item, options)
