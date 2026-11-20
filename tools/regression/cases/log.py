"""log 组：日志吞吐、写回压和磁盘写满。"""
import datetime
import os
import re
import subprocess
import time

from regression_platform import WINDOWS

from ..config import replace, set_test_flags
from ..probes import CAP, bytes_on_disk, disk_failure
from ..procs import REPO
from . import Case


class LogCase(Case):
    def configure(self, ctx, name, text):
        case = ctx.name
        if name == "logd" and case == "log-backpressure":
            text = replace(text, "max_pending_bytes", str(8 * 1024**2 + 8))
        if name == "ipcclient":
            text = set_test_flags(text, ctx.mode)
            if ctx.mode == "log":
                text = text.replace("[LogClient]", "[LogClient]\nmax_send_queue_bytes = 262144")
            if case == "disk-full":
                text = replace(text, "bytes", "131072")
            if case == "log-backpressure":
                text = replace(text, "line", "131072")
                text = replace(text, "bytes", str(64 * 1024**2))
                text = replace(text, "drain_sec", "2")
        return text

    def prepare_directory(self, ctx):
        if ctx.name == "disk-full" and not WINDOWS:
            dated = ctx.logs / datetime.datetime.now().strftime("%y_%m_%d")
            dated.mkdir()
            for level in ("trace", "debug", "info", "warning", "error"):
                (dated / level).symlink_to("/dev/full")

    def prepare(self, ctx):
        case = ctx.name
        if case == "log-backpressure" and not WINDOWS:
            slow_write = ctx.root / "slow-write.so"
            subprocess.run(["gcc", "-shared", "-fPIC", "-O2", "-Wall", "-Werror",
                            str(REPO / "tools/log_slow_write.c"), "-o", str(slow_write), "-ldl", "-pthread"], check=True)
            ctx.log_env = dict(os.environ, LD_PRELOAD=str(slow_write))
        if WINDOWS and case in ("disk-full", "log-backpressure"):
            stamp = ctx.args.bin_dir / "oi_test_hooks.txt"
            if not stamp.exists() or stamp.read_text().strip() != "ON":
                raise RuntimeError("Windows fault cases require -DOI_ENABLE_TEST_HOOKS=ON")
            ctx.log_env = dict(os.environ, OI_TEST_LOG_FAULT="full" if case == "disk-full" else "slow")

    def logd_start_failure(self, ctx, logd):
        if ctx.name != "disk-full":
            return False
        message = ctx.logpath.read_text(encoding="utf-8", errors="replace")
        if disk_failure(logd.poll(), message):
            print(ctx.name, "PASS: disk error detected", flush=True)
            return True
        return False

    def logd_poll_failure(self, ctx, logd):
        if ctx.name != "disk-full":
            return False
        message = ctx.logpath.read_text(encoding="utf-8", errors="replace")
        if disk_failure(logd.returncode, message):
            print(ctx.name, "PASS: disk error detected", flush=True)
            return True
        raise RuntimeError(message)

    def check(self, ctx):
        case, text = ctx.name, ctx.text
        logd, logpath, logs = ctx.logd, ctx.logpath, ctx.logs
        stop = ctx.stop
        last_log_bytes, last_growth, started = ctx.last_log_bytes, ctx.last_growth, ctx.started
        if ctx.mode == "log":
            # Stop drains accepted batches; inspect actual files and the final backlog accounting.
            stop(logd)
            logd.wait(timeout=10)
            if case == "disk-full":
                message = logpath.read_text(encoding="utf-8", errors="replace")
                if disk_failure(logd.returncode, message):
                    print(case, "PASS: disk error detected", flush=True)
                    return True
                raise RuntimeError("disk-full did not report a write failure\n" + message)
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
        return False

CASES = (LogCase("log", "log"), LogCase("log-backpressure", "log"), LogCase("disk-full", "log"))
