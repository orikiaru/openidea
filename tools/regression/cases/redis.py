"""redis 组：Redis 客户端完整性、限制、停服、回压和公平性。"""
import re
import socket
import time

from regression_platform import pause, rss_kib

from .. import ports
from ..config import replace, set_test_flags
from . import Case


class RedisCase(Case):
    def configure(self, ctx, name, text):
        case = ctx.name
        if name == "redisserver" and case == "redis":
            text = replace(text, "connections", "1")
        if name == "redisserver" and ctx.pressure:
            text = replace(text, "connections", "1")
            maximum_bytes = 1024**2 if case.endswith("bytes") else 8 * 1024**2
            maximum_count = 1 if ctx.fairness else (128 if case.endswith("bytes") else 4)
            text = replace(text, "max_pending_bytes", str(maximum_bytes))
            text = replace(text, "max_pending_commands", str(maximum_count))
            if ctx.fairness: text = replace(text, "offset_ms", "1000")
            ctx.maximum_bytes, ctx.maximum_count = maximum_bytes, maximum_count
        if name != "ipcclient":
            return text
        if case == "redis":
            text = text.replace("[RedisClient]", "[RedisClient]\nmax_send_queue_bytes = 1048576", 1)
            text += "\n[Regression]\nredis_upstream_disconnect = true\nredis_send_budget = true\n"
        if case == "redis-queue-timeout":
            text = replace(text, "timeout_ms", "100")
            text += "\n[Regression]\nredis_queue_timeout = true\n"
        if case == "redis-shutdown":
            # 停服测试必须由关闭唤醒，不能与 Windows 控制台通知启动耗时竞争。
            # 独立 redis-queue-timeout 用例仍验证短超时；停服后的退出时限仍为 3 秒。
            text = replace(text, "timeout_ms", "10000")
            text += "\n[Regression]\nredis_shutdown = true\n"
        if ctx.fairness:
            text += "\n[Regression]\nredis_fairness = true\n"
        elif ctx.pressure:
            text = replace(text, "timeout_ms", "10000")
            text += "\n[Regression]\nredis_pressure = true\nredis_oversized = %s\n" % ("true" if case.endswith("bytes") else "false")
        if case == "redis-limit":
            text = text.replace("[RedisClient]", "[RedisClient]\nmax_pending = 1")
            text = replace(text, "tasks", "0")
        text = set_test_flags(text, ctx.mode)
        return text

    def on_poll(self, ctx):
        if ctx.name == "redis" and not ctx.redis_restarted and (ctx.root / "redis-disconnect.request").exists():
            # 故障注入仅终止本次持有的 Redis 进程句柄，不按端口/进程名杀服务。
            assert ctx.db.poll() is None, "owned Redis exited before injection"
            ctx.db.kill()
            ctx.db.wait(timeout=5)
            # 已确认退出的故障注入目标不再参与存活检查；新实例仍照常检查和清理。
            ctx.procs.remove(ctx.db)
            ctx.db, ctx.dbpath = ctx.start("redis", [str(ctx.args.redis_binary), str(ctx.redis_conf) if ctx.args.redis_wsl else "../redis.conf"])
            ctx.ready(ctx.db, ctx.dbpath, "Ready to accept connections")
            ctx.redis_restarted = True
            (ctx.root / "redis-disconnect.ack").write_text("restarted", encoding="utf-8")
        if ctx.name == "redis-shutdown" and not ctx.shutdown_sent and "redis shutdown pending" in ctx.clientpath.read_text(encoding="utf-8", errors="replace"):
            ctx.stop(ctx.client)
            ctx.shutdown_sent = True
            ctx.deadline = time.monotonic() + 3
        if ctx.pressure and ctx.db_paused:
            if ctx.pause_at is None and "redis pressure pause" in ctx.rsp.read_text(encoding="utf-8", errors="replace"):
                ctx.pause_at = time.monotonic()
            if ctx.pause_at is not None and time.monotonic() - ctx.pause_at > 0.5 and (not ctx.fairness or "redis fairness ready" in ctx.clientpath.read_text(encoding="utf-8", errors="replace")):
                rss_paused = rss_kib(ctx.rs)
                assert rss_paused < ctx.rss_before + 32 * 1024, (ctx.rss_before, rss_paused)
                print(ctx.name, "paused RSS KiB before=", ctx.rss_before, "after=", rss_paused, flush=True)
                pause(ctx.db, False)
                ctx.db_paused = False

    def check(self, ctx):
        case, text = ctx.name, ctx.text
        shutdown_sent, redis_restarted = ctx.shutdown_sent, ctx.redis_restarted
        pause_at, db_paused, fairness = ctx.pause_at, ctx.db_paused, ctx.fairness
        monitor, rsp = ctx.monitor, ctx.rsp
        maximum_count, maximum_bytes = ctx.maximum_count, ctx.maximum_bytes
        if ctx.name == "redis-shutdown":
            assert shutdown_sent and "redis shutdown result=2 late=2 bad=0" in text
        elif ctx.name == "redis-queue-timeout":
            assert re.search(r"redis queue timeout elapsed=\d+ bad=0", text)
        elif ctx.pressure:
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
        elif ctx.mode == "redis":
            assert "redis integrity ok" in text
            if case == "redis-limit":
                assert "redis pending limit bad=0" in text
            else:
                assert redis_restarted and "redis upstream disconnect recovery bad=0" in text
                assert "redis send budget rejected=16 recovered=16 pending=0 bad=0" in text
                assert "redis millisecond expiry bad=0" in text
                assert "redis typed commands string rpcdata zset bad=0" in text
                assert "redis typed collections string rpcdata bad=0" in text
                assert "redis typed native ping lua scalar array rpcdata bad=0" in text
                assert "redis expanded five types bad=0" in text
                assert "redis typed sync id=0 bad=0" in text
                assert "redis typed sync id=1 bad=0" in text
                assert re.search(r"redis failure stage=server_reply request=[1-9]\d* sid=\d+ command=EVAL reason=.*test error.*truncated=0 late=0", text), "missing original Lua error"
                assert re.search(r"redis failure .*command=GET reason=WRONGTYPE .*truncated=0 late=0", text), "missing original Redis WRONGTYPE"
                assert "redis script isolation bad=0" in text
                assert "redis single use bad=0" in text
                assert re.search(r"redis stress .*bad=0", text)
        return False

    def diagnostics(self, ctx):
        if ctx.name == "redis-backpressure-bytes":
            # 普通接收额度默认 64 MiB，2 MiB 的 ECHO 必须由 max_pending_bytes 的解析拒绝断开。
            # 先正常停 redisserver 刷新 stdout，再排空日志服核对落盘。
            ctx.stop(ctx.rs)
            ctx.rs.wait(timeout=10)
            assert ctx.rs.returncode == 0, "redisserver stop exit=%s" % ctx.rs.returncode
            ctx.procs.remove(ctx.rs)
            rejected = r"redisserver rejects malformed or oversized command sid=\d+ limit=%d\b" % ctx.maximum_bytes
            assert ctx.maximum_bytes == 1048576, ctx.maximum_bytes
            assert re.search(rejected, ctx.rsp.read_text(encoding="utf-8", errors="replace")), "oversized command rejection missing from redisserver output"
            assert re.search(rejected, ctx.stop_backends()), "oversized command rejection missing from logd disk"
            print("redis oversized command rejected limit=%d PASS" % ctx.maximum_bytes, flush=True)
            return
        if ctx.name != "redis":
            return
        diagnostics = ctx.stop_backends()
        assert re.search(r"redis failure stage=send request=[1-9]\d* sid=[1-9]\d* command=ECHO reason=ret=-5 bytes=\d+ cause=send queue capacity exceeded", diagnostics), "Redis send budget failure missing from logd disk"
        assert re.search(r"redis failure stage=server_reply request=[1-9]\d* sid=\d+ command=EVAL reason=.*test error", diagnostics), "Lua error missing from logd disk"
        assert re.search(r"redis upstream disconnect index=0 host=127\.0\.0\.1 port=" + str(ports.REDIS_PORT) + r" status=-1 error=[1-9]\d* reason=.+ truncated=0", diagnostics), "original hiredis disconnect error missing from logd disk"
        print("diagnostic original errors persisted", ctx.name, "PASS", flush=True)

CASES = (
    RedisCase("redis", "redis"),
    RedisCase("redis-limit", "redis"),
    RedisCase("redis-shutdown", "redis"),
    RedisCase("redis-queue-timeout", "redis"),
    RedisCase("redis-backpressure-bytes", "redis"),
    RedisCase("redis-backpressure-count", "redis"),
    RedisCase("redis-fairness", "redis"),
)
