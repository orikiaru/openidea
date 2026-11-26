"""回归通用 driver：建环境、配置、起服务、跑 client、按用例断言和收尾。"""
import argparse
import json
import os
from pathlib import Path
import re
try:
    import resource
except ImportError:
    resource = None
import shutil
import socket
import sys
import threading
import time
import traceback

from regression_platform import WINDOWS, SUFFIX, pause, rss_kib
from registry_replacement_check import check as check_registry_replacement, check_reload_race, check_owner_loss_disconnect
from registry_fault_proxy import RegistryRedisProxy

from . import ports
from .cases import ALL_CASES, REGISTRY, TARGETED_CASES, get_case, normalize_case_name
from .config import replace, shift_ports
from .probes import CAP, EnvironmentBlocked, bytes_on_disk, log_payload_on_disk
from .procs import REPO, Context, cleanup_owned, regression_directory


def run_case(case, args):
    entry = get_case(case)
    if resource is not None:
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    # 前缀带偏移，并行时每路只按自己的临时目录清理进程。
    with regression_directory(prefix="oi-regression-p%d-" % ports.PORT_OFFSET) as tmp:
        root = Path(tmp)
        ctx = Context(args, entry, root)
        ctx.registry_proxy = None
        ctx.claim()
        ctx.prepare_paths()
        names = ("logd", "ipcclient") + (("ipcserver",) if ctx.scene not in ("timer", "redis") else ())
        names += (("redisserver",) if ctx.scene in (None, "redis") else ())
        names += (("registry",) if ctx.name in ("registry", "registry-static") or (ctx.mode in ("hold", "multi") and not ctx.scene) else ())
        for name in names:
            shutil.copy2(args.bin_dir / (name + SUFFIX), ctx.work / (name + SUFFIX))
            text = shift_ports((REPO / "deploy/config" / (name + ".toml")).read_text(encoding="utf-8-sig"))
            if ctx.scene:
                text = re.sub(r'(?m)^(\[Registry\]\s*\n)ip = "[^"\n]*"\s*\nport = "[^"\n]*"\s*\ntype = "[^"\n]*"\s*\n', r'\1', text, count=1)
            if name in ("redisserver", "registry"):
                assert 'port = "6379"' in text, name
                text = text.replace('port = "6379"', 'port = "%d"' % ports.REDIS_PORT)
            if name == "logd":
                text = replace(text, "root_path", '"' + ctx.logs.as_posix() + '"')
            text = entry.configure(ctx, name, text)
            (ctx.cfg / (name + ".toml")).write_text(text, encoding="utf-8")
        entry.prepare_directory(ctx)
        try:
            entry.prepare(ctx)
            if ctx.start_logd():
                return
            entry.after_logd(ctx)
            if (ctx.mode in ("redis", "hold", "multi") and ctx.scene not in ("blocking", "timer")) or ctx.name == "registry":
                redis_conf = ctx.root / "redis.conf"
                redis_conf.write_text('bind 127.0.0.1\nport %d\nsave ""\nappendonly no\n' % ports.REDIS_PORT +
                                      'maxmemory 1536mb\nmaxmemory-policy allkeys-lru\n'
                                      'dir ' + (str(ctx.root) if args.redis_wsl else '..') + '\n')
                ctx.redis_conf = redis_conf
                # msys2 版 redis-server.exe 不认盘符绝对路径，原生启动时按 cwd=bin 传相对路径。
                ctx.db, ctx.dbpath = ctx.start("redis", [str(args.redis_binary), str(redis_conf) if args.redis_wsl else "../redis.conf"])
                ctx.ready(ctx.db, ctx.dbpath, "Ready to accept connections")
                if ctx.fairness:
                    ctx.monitor = socket.create_connection(("127.0.0.1", ports.REDIS_PORT), timeout=2)
                    ctx.monitor.sendall(b"*1\r\n$7\r\nMONITOR\r\n")
                    assert ctx.monitor.recv(100) == b"+OK\r\n"
                elif ctx.name in ("hold", "hold-redis") and getattr(args, "redis_timeline", False):
                    ctx.monitor = socket.create_connection(("127.0.0.1", ports.REDIS_PORT), timeout=2)
                    ctx.monitor.sendall(b"*1\r\n$7\r\nMONITOR\r\n")
                    assert ctx.monitor.recv(100) == b"+OK\r\n"
                    ctx.monitor.settimeout(0.2)

                    def collect_timeline():
                        pending = b""
                        dropping = False
                        try:
                            while not ctx.timeline_stop.is_set():
                                try:
                                    part = ctx.monitor.recv(4096)
                                except socket.timeout:
                                    continue
                                if not part:
                                    if not ctx.timeline_stop.is_set():
                                        ctx.timeline_errors.append(RuntimeError("Redis MONITOR closed unexpectedly"))
                                    break
                                for fragment in part.splitlines(keepends=True):
                                    complete = fragment.endswith(b"\n")
                                    if not dropping:
                                        pending += fragment
                                        if len(pending) > 65536:
                                            pending = b""
                                            dropping = True
                                        elif complete:
                                            line = pending.rstrip(b"\r\n")
                                            if re.search(rb'"PING"\s*$', line) or re.search(rb'"BLPOP" "hold-(?:late|race)" ', line):
                                                if len(ctx.timeline_lines) < 1000:
                                                    ctx.timeline_lines.append(line)
                                                elif not ctx.timeline_errors:
                                                    ctx.timeline_errors.append(RuntimeError("Redis timeline exceeded 1000 entries"))
                                            pending = b""
                                    if complete:
                                        dropping = False
                        except Exception as error:
                            ctx.timeline_errors.append(error)

                    ctx.timeline_thread = threading.Thread(target=collect_timeline)
                    ctx.timeline_thread.start()
                if ctx.name == "registry":
                    ctx.registry_proxy = RegistryRedisProxy(ports.REDIS_PORT)
                    config_path = ctx.cfg / "registry.toml"
                    config = config_path.read_text(encoding="utf-8")
                    config = config.replace('port = "%d"' % ports.REDIS_PORT,
                                            'port = "%d"' % ctx.registry_proxy.server_address[1])
                    config = replace(config, "timeout_ms", "10000")
                    config_path.write_text(config, encoding="utf-8")
                if ctx.name == "registry" or (ctx.mode in ("hold", "multi") and not ctx.scene):
                    ctx.registry, ctx.registry_path = ctx.start("registry")
                    ctx.ready(ctx.registry, ctx.registry_path, "registry server listen")
                    if ctx.name == "registry":
                        check_registry_replacement(ports.REGISTRY_PORT, ports.REDIS_PORT)
                        check_reload_race(ports.REGISTRY_PORT, ports.REDIS_PORT, ctx.registry_proxy)
                        check_owner_loss_disconnect(ports.REGISTRY_PORT, ports.REDIS_PORT)
                if ctx.name not in ("registry", "multi", "local", "registry-static"):
                    ctx.rs, ctx.rsp = ctx.start("redisserver")
                    ctx.ready(ctx.rs, ctx.rsp, "redis server listen")
            entry.after_redis(ctx)
            if ctx.scene not in ("timer", "redis"):
                ctx.server, ctx.serverpath = ctx.start("ipcserver")
                ctx.ready(ctx.server, ctx.serverpath, "ipc server listen")
            entry.scenario(ctx)
            env = os.environ.copy()
            env = entry.client_env(ctx, env)
            if ctx.pressure:
                pause(ctx.db, True)
                ctx.db_paused = True
                ctx.rss_before = rss_kib(ctx.rs)
            started = time.monotonic()
            last_growth = started
            last_log_bytes = log_payload_on_disk(ctx.logs)
            ctx.started = started
            ctx.last_growth = last_growth
            ctx.last_log_bytes = last_log_bytes
            command = ([str(ctx.work / ("ipcclient" + SUFFIX)), "--regression-ready"]
                       if ctx.name == "disk-full" else None)
            ctx.client, ctx.clientpath = ctx.start("ipcclient", command, env=env)
            if ctx.name == "disk-full":
                ctx.ready(ctx.client, ctx.clientpath, "ipcclient framework ready")
            # 正常通过时 hold 约 50 秒，其余不超过 35 秒；留三四倍余量，失败时不用干等。
            ctx.deadline = started + entry.timeout
            while ctx.client.poll() is None:
                entry.on_poll(ctx)
                log_bytes = bytes_on_disk(ctx.logs)
                payload_bytes = log_payload_on_disk(ctx.logs) if ctx.mode == "log" else 0
                if payload_bytes > ctx.last_log_bytes:
                    ctx.last_growth, ctx.last_log_bytes = time.monotonic(), payload_bytes
                if log_bytes > CAP:
                    raise RuntimeError("test log exceeds 2GiB limit")
                logd_intentionally_stopped = ctx.name == "log-contract" and getattr(ctx, "contract_state", 0) in (1, 2)
                if not logd_intentionally_stopped and ctx.logd.poll() is not None and entry.logd_poll_failure(ctx, ctx.logd):
                    return
                for proc in ctx.procs:
                    if proc is not ctx.client and proc.poll() is not None:
                        raise RuntimeError("service exited: " + str(proc.args))
                if time.monotonic() > ctx.deadline:
                    raise RuntimeError("client timeout\n" + ctx.clientpath.read_text(encoding="utf-8", errors="replace"))
                time.sleep(0.1)
            text = ctx.clientpath.read_text(encoding="utf-8", errors="replace")
            print(text, flush=True)
            ctx.text = text
            entry.environment_blocked(ctx, text)
            assert ctx.client.returncode == 0, (ctx.name, ctx.client.returncode)
            if entry.check(ctx):
                return
            entry.diagnostics(ctx)
            print(ctx.name, "PASS seconds=", round(time.monotonic() - ctx.started, 2), flush=True)
        except BaseException:
            try:
                for output in ctx.root.glob("*.out"):
                    print(output.name, output.read_text(encoding="utf-8", errors="replace")[-6000:], flush=True)
            except BaseException as error:
                print("failure output unavailable:", repr(error), file=sys.stderr, flush=True)
            raise
        finally:
            primary = sys.exc_info()[1]
            try:
                if ctx.timeline_thread is not None:
                    ctx.timeline_stop.set()
                    ctx.timeline_thread.join(timeout=3)
                    if ctx.timeline_thread.is_alive():
                        raise RuntimeError("Redis timeline collector failed to stop")
                    if ctx.timeline_errors:
                        raise RuntimeError("Redis timeline collector failed: " + repr(ctx.timeline_errors[0]))
                    (ctx.root / "redis-timeline.out").write_bytes(b"\n".join(ctx.timeline_lines) + b"\n")
            except Exception as error:
                if primary is None:
                    raise
                print("Redis timeline capture failed:", repr(error), file=sys.stderr, flush=True)
            finally:
                try:
                    cleanup_owned(ctx.procs, ctx.handles, ctx.root, ctx.args, ctx.name,
                                  db=ctx.db, db_paused=ctx.db_paused,
                                  monitor=ctx.monitor, http_fixture=ctx.http_fixture, http_thread=ctx.http_thread,
                                  primary=sys.exc_info()[1] or primary,
                                  registry=ctx.registry, registry_paused=ctx.registry_paused)
                finally:
                    if getattr(ctx, "registry_proxy", None) is not None:
                        ctx.registry_proxy.close()


def main(doc=None):
    parser = argparse.ArgumentParser(description=__doc__ if doc is None else doc)
    parser.add_argument("--case", default="all", type=normalize_case_name, choices=("all", *REGISTRY),
                        metavar="all|test-NAME", help="available cases: " + ", ".join("test-" + name for name in REGISTRY))
    parser.add_argument("--list-cases", action="store_true", help="list full and targeted cases without starting services")
    parser.add_argument("--bin-dir", type=Path, default=REPO / "deploy/bin")
    parser.add_argument("--redis-binary", type=Path, help="default: redis-server on PATH")
    parser.add_argument("--clock-fault-so", type=Path)
    parser.add_argument("--redis-wsl", action="store_true", help="Windows: run only the owned Redis fixture in WSL")
    parser.add_argument("--wsl-user", default="root")
    parser.add_argument("--artifacts", type=Path, help="retain process output and results.json, not bulk log payload")
    parser.add_argument("--redis-timeline", action="store_true", help="capture hold PING and BLPOP execution timestamps from the owned Redis fixture")
    parser.add_argument("--port-offset", type=int, default=0, help="shift every test port; parallel work copies need distinct offsets")
    parser.add_argument("--expect-thread", choices=("st", "mt"), help="thread mode the hold and udp binaries must match")
    options = parser.parse_args()
    if options.list_cases:
        for name in REGISTRY:
            print("test-" + name, TARGETED_CASES.get(name, "all"), sep=": ")
        return
    if options.case in ("all", "hold", "udp", "web-stop") and options.expect_thread is None:
        parser.error("--expect-thread st or mt is required for hold, udp and web-stop")
    if options.redis_timeline and (options.case not in ("hold", "hold-redis") or not options.artifacts):
        parser.error("--redis-timeline requires --case hold or hold-redis and --artifacts")
    options.redis_binary = options.redis_binary or Path(shutil.which("redis-server") or "/usr/local/bin/redis-server")
    if options.redis_wsl and not WINDOWS:
        parser.error("--redis-wsl is only for native Windows tests")
    if WINDOWS and options.case.startswith("clock-"):
        parser.error("clock fault injection requires Linux LD_PRELOAD; never silently skip it")
    cases = ALL_CASES if options.case == "all" else (options.case,)
    if options.artifacts:
        options.artifacts = options.artifacts.resolve()
        options.artifacts.mkdir(parents=True, exist_ok=True)
        plan = {"selected": list(cases)}
        if options.case == "all":
            plan["targeted_only"] = TARGETED_CASES
        (options.artifacts / "plan.json").write_text(json.dumps(plan, indent=2), encoding="utf-8")
        (options.artifacts / "results.json").write_text("[]", encoding="utf-8")
    ports.apply_port_offset(options.port_offset)
    ports.preflight_ports(options.case)
    results = []
    for item in cases:
        started = time.monotonic()
        result = {"case": item, "platform": "windows" if WINDOWS else "linux"}
        try:
            if WINDOWS and item == "tcp-error-rearm":
                result.update(status="skipped", reason="Unix libuv POLLERR only")
                print(item, "SKIP:", result["reason"], flush=True)
                continue
            if item in ("mysql", "mysql-wake"):
                required = ("OI_TEST_MYSQL_HOST", "OI_TEST_MYSQL_PORT", "OI_TEST_MYSQL_USER", "OI_TEST_MYSQL_PASSWORD", "OI_TEST_MYSQL_DATABASE")
                missing = [name for name in required if name not in os.environ or (name != "OI_TEST_MYSQL_PASSWORD" and not os.environ[name])]
                if missing:
                    raise EnvironmentBlocked("missing environment variables: " + ", ".join(missing))
            entry = get_case(item)
            if entry.standalone is not None:
                entry.standalone(options)
            else:
                run_case(item, options)
            result["status"] = "passed"
        except EnvironmentBlocked as error:
            result.update(status="blocked", error=str(error))
            print(item, "environment blocked:", error, flush=True)
        except Exception as error:
            result.update(status="failed", error=str(error))
            traceback.print_exc()
        finally:
            result["seconds"] = round(time.monotonic() - started, 3)
            results.append(result)
            if options.artifacts:
                (options.artifacts / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    raise SystemExit(1 if any(item["status"] not in ("passed", "skipped") for item in results) else 0)


run = run_case
