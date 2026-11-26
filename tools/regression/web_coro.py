"""End-to-end HTTP handlers with real RPC and Redis dependencies."""
import http.client
import http.server
import json
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time
from regression_platform import SUFFIX, launch, WslRedis
from test_process_owner import record_process, write_owner
from . import ports
from .config import replace, shift_ports
from .probes import EnvironmentBlocked
from .procs import REPO, cleanup_owned, copy_runtime_libraries, regression_directory


def run_web_coro(args):
    # 独立后端和配置，禁止发现已有注册表；所有请求由真实网络路径完成。
    names = ("ipcserver", "redisserver", "webserver")
    missing = [str(args.bin_dir / (name + SUFFIX)) for name in names
               if not (args.bin_dir / (name + SUFFIX)).is_file()]
    if missing:
        raise EnvironmentBlocked("missing executables: " + ", ".join(missing))
    if args.redis_wsl:
        try:
            check = subprocess.run(["wsl", "-u", args.wsl_user, "--exec", "test", "-x", str(args.redis_binary)],
                                   capture_output=True, timeout=10)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise EnvironmentBlocked("WSL Redis preflight failed: " + str(error)) from error
        if check.returncode:
            raise EnvironmentBlocked("WSL Redis executable unavailable: " + str(args.redis_binary))
    elif not args.redis_binary.is_file():
        raise EnvironmentBlocked("missing Redis executable: " + str(args.redis_binary))
    with regression_directory(prefix="oi-regression-web-coro-p%d-" % ports.PORT_OFFSET) as tmp:
        root = Path(tmp)
        write_owner(root, REPO, ports.PORT_OFFSET)
        work, cfg = root / "bin", root / "config"
        work.mkdir()
        cfg.mkdir()
        copy_runtime_libraries(args.bin_dir, work)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            web_port = probe.getsockname()[1]
        for name in names:
            shutil.copy2(args.bin_dir / (name + SUFFIX), work / (name + SUFFIX))
            config = shift_ports((REPO / "deploy/config" / (name + ".toml")).read_text(encoding="utf-8-sig"))
            config = re.sub(r"(?ms)^\[Registry\]\s*\n.*?(?=^\[|\Z)", "", config)
            if name == "redisserver":
                config = config.replace('port = "6379"', 'port = "%d"' % ports.REDIS_PORT)
            if name == "webserver":
                config = replace(config, "port", str(web_port))
                config += ('\n[WebRpc]\nip = "127.0.0.1"\nport = "%d"\ntype = "tcp"\ndelay_ms = 1200\n'
                           '\n[RedisClient]\nip = "127.0.0.1"\nport = "%d"\ntype = "tcp"\ntimeout_ms = 5000\n' % (ports.IPC_PORT, ports.REDIS_SERVER_PORT))
            (cfg / (name + ".toml")).write_text(config, encoding="utf-8")
        redis_conf = root / "redis.conf"
        redis_conf.write_text('bind 127.0.0.1\nport %d\nsave ""\nappendonly no\nmaxmemory 1536mb\n'
                              'maxmemory-policy allkeys-lru\ndir %s\n' % (ports.REDIS_PORT, str(root) if args.redis_wsl else '..'))
        procs, handles = [], []
        db = None
        def start(name, command=None):
            path = root / (name + ".out")
            handle = path.open("w", encoding="utf-8")
            handles.append(handle)
            if name == "redis" and args.redis_wsl:
                proc = WslRedis(command[0], redis_conf, args.wsl_user, cwd=work, stdout=handle, stderr=subprocess.STDOUT)
            else:
                proc = launch(command or [str(work / (name + SUFFIX))], cwd=work, stdout=handle, stderr=subprocess.STDOUT)
            procs.append(proc)
            record_process(root, proc)
            return proc, path
        def wait_marker(proc, path, marker, count=1):
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                text = path.read_text(encoding="utf-8", errors="replace")
                if proc.poll() is not None:
                    raise RuntimeError("%s exited: %s" % (path.name, text))
                if text.count(marker) >= count:
                    return
                time.sleep(0.02)
            raise RuntimeError("missing %s count=%d: %s" % (marker, count, text))
        def request(text):
            return ("GET /query?text=%s HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n" % text).encode()
        def response(reader, expected):
            status = reader.readline().decode("ascii").strip()
            headers = {}
            while True:
                line = reader.readline()
                if line == b"\r\n":
                    break
                if not line:
                    raise RuntimeError("truncated HTTP headers")
                key, value = line.decode("ascii").split(":", 1)
                headers[key.lower()] = value.strip()
            body = reader.read(int(headers["content-length"]))
            assert status.startswith("HTTP/1.1 200 ") and json.loads(body)["text"] == expected, (status, body, expected)
        try:
            db, dbpath = start("redis", [str(args.redis_binary), str(redis_conf) if args.redis_wsl else "../redis.conf"])
            wait_marker(db, dbpath, "Ready to accept connections")
            redis, redispath = start("redisserver")
            wait_marker(redis, redispath, "redis server listen")
            ipc, ipcpath = start("ipcserver")
            wait_marker(ipc, ipcpath, "ipc server listen")
            web, webpath = start("webserver")
            deadline = time.monotonic() + 15
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", web_port), timeout=0.2):
                        break
                except OSError:
                    if web.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError(webpath.read_text(encoding="utf-8", errors="replace"))
                    time.sleep(0.05)
            with socket.create_connection(("127.0.0.1", web_port), timeout=5) as slow:
                slow.sendall(request("slow"))
                wait_marker(web, webpath, "web rpc begin")
                health = http.client.HTTPConnection("127.0.0.1", web_port, timeout=0.8)
                try:
                    health.request("GET", "/health")
                    res = health.getresponse()
                    assert res.status == 200 and res.read() == b"ok"
                    assert "web handler complete" not in webpath.read_text(encoding="utf-8", errors="replace")
                finally:
                    health.close()
                response(slow.makefile("rb"), "slow")
            with socket.create_connection(("127.0.0.1", web_port), timeout=8) as pipe:
                pipe.sendall(request("first") + request("second"))
                pipe.shutdown(socket.SHUT_WR)
                reader = pipe.makefile("rb")
                response(reader, "first")
                response(reader, "second")
                reader.close()
            wait_marker(web, webpath, "web handler complete", 3)
            with socket.create_connection(("127.0.0.1", web_port), timeout=5) as abandoned:
                abandoned.sendall(request("abandoned"))
                wait_marker(web, webpath, "web rpc begin", 4)
            with socket.create_connection(("127.0.0.1", web_port), timeout=8) as fresh:
                fresh.sendall(request("fresh"))
                response(fresh.makefile("rb"), "fresh")
            wait_marker(web, webpath, "web handler complete", 5)
            assert webpath.read_text(encoding="utf-8", errors="replace").count("web rpc begin") == 5
            print("web-coro PASS: health, RPC, Redis, pipeline, disconnected handler", flush=True)
        finally:
            cleanup_owned(procs, handles, root, args, "web-coro", db=db, primary=sys.exc_info()[1])
