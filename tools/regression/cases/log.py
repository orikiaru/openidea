"""log 组：日志吞吐、写回压和磁盘写满。"""
import datetime
import os
import re
import socket
import struct
import subprocess
import time

from regression_platform import WINDOWS

from .. import ports
from ..config import replace, set_test_flags
from ..probes import CAP, bytes_on_disk, disk_failure
from ..procs import REPO, test_hook_enabled
from . import Case


class LogCase(Case):
    def configure(self, ctx, name, text):
        case = ctx.name
        if name == "logd" and case == "log-backpressure":
            text = replace(text, "max_pending_bytes", str(8 * 1024**2 + 8))
        if name == "ipcclient":
            text = set_test_flags(text, ctx.mode)
            if ctx.mode == "log":
                limit = 4096 if case == "log-contract" else 262144
                text = text.replace("[LogClient]", "[LogClient]\nmax_send_queue_bytes = %d" % limit)
            if case == "disk-full":
                text = replace(text, "bytes", "131072")
            if case == "log-backpressure":
                text = replace(text, "line", "131072")
                text = replace(text, "bytes", str(64 * 1024**2))
                text = replace(text, "drain_sec", "2")
        return text

    def client_env(self, ctx, env):
        if ctx.name == "log-contract":
            ctx.contract_state = 0
            ctx.contract_gate = ctx.root / "log-contract"
            env["OI_TEST_LOG_CONTRACT_GATE"] = str(ctx.contract_gate)
        return env

    def on_poll(self, ctx):
        if ctx.name != "log-contract":
            return
        output = ctx.clientpath.read_text(encoding="utf-8", errors="replace")
        if ctx.contract_state == 0 and "LogClient::OnClientConnect" in output:
            ctx.stop(ctx.logd)
            ctx.logd.wait(timeout=10)
            assert ctx.logd.returncode == 0
            ctx.procs.remove(ctx.logd)
            ctx.contract_baseline = sum(path.stat().st_size for path in ctx.logs.rglob("info"))
            ctx.contract_state = 1
        if ctx.contract_state == 1 and "LogClient::OnClientDisconnect" in output:
            ctx.contract_gate.with_suffix(".disconnected").touch()
            ctx.contract_state = 2
        if ctx.contract_state == 2 and "log-contract pending complete" in output:
            ctx.start_logd()
            ctx.contract_state = 3
        if ctx.contract_state == 3 and output.count("LogClient::OnClientConnect") >= 2:
            ctx.contract_gate.with_suffix(".connected").touch()
            ctx.contract_state = 4
        if ctx.contract_state == 4 and "log-contract final code=0" in output:
            match = re.search(r"log-contract pending complete busy=(\d+)", output)
            assert match
            expected = sum(len("00:00:00 ipcclient-1:log-contract seq=%d\n" % i) for i in range(int(match.group(1))))
            expected += len("00:00:00 ipcclient-1:log-contract final\n")
            if sum(path.stat().st_size for path in ctx.logs.rglob("info")) >= ctx.contract_baseline + expected:
                ctx.contract_gate.with_suffix(".drained").touch()
                ctx.contract_state = 5

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
            clock_source = ctx.root / "log-clock.c"
            clock_source.write_text(r"""
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static struct timespec started;
__attribute__((constructor)) static void init_clock(void)
{
    clock_gettime(CLOCK_MONOTONIC, &started);
}
time_t time(time_t *out)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed = now.tv_sec - started.tv_sec;
    time_t value = 1704110400 + elapsed + (elapsed >= 2 ? 90000 : 0);
    if (out) *out = value;
    return value;
}
FILE *fopen(const char *path, const char *mode)
{
    FILE *(*next_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
    const char *marker = getenv("OI_TEST_ROTATION_OPEN_MARKER");
    const char *release = getenv("OI_TEST_ROTATION_OPEN_RELEASE");
    if (marker && release && strstr(path, "/24_01_02/"))
    {
        int fd = open(marker, O_WRONLY | O_CREAT, 0600);
        if (fd >= 0) close(fd);
        for (int i = 0; i < 1000 && access(release, F_OK) != 0; ++i)
        {
            struct timespec delay = {0, 10000000};
            nanosleep(&delay, NULL);
        }
    }
    return next_fopen(path, mode);
}
int gettimeofday(struct timeval *tv, void *tz)
{
    int rc = (int)syscall(SYS_gettimeofday, tv, tz);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed = now.tv_sec - started.tv_sec;
    if (rc == 0) tv->tv_sec = 1704110400 + elapsed + (elapsed >= 2 ? 90000 : 0);
    return rc;
}
""", encoding="utf-8")
            clock_library = ctx.root / "log-clock.so"
            subprocess.run(["gcc", "-shared", "-fPIC", "-O2", "-Wall", "-Werror",
                            str(clock_source), "-o", str(clock_library), "-ldl"], check=True)
            ctx.log_env = dict(os.environ, LD_PRELOAD=f"{slow_write}:{clock_library}", TZ="UTC")
        if WINDOWS and case in ("disk-full", "log-backpressure"):
            if not test_hook_enabled(ctx.work, "logd", "log_fault"):
                raise RuntimeError("Windows fault cases require logd built with -DOI_ENABLE_TEST_HOOKS=ON")
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
        if case == "log-contract":
            assert ctx.contract_state == 5, ctx.contract_state
            stop(logd)
            logd.wait(timeout=10)
            assert logd.returncode == 0
            results = [(int(seq), int(code)) for seq, code in re.findall(r"log-contract result seq=(\d+) code=(-?\d+)", text)]
            assert len(results) > 1 and results[-1][1] == -5, results
            assert results == [(i, 0) for i in range(len(results) - 1)] + [(len(results) - 1, -5)], results
            stdout_rows = [int(seq) for seq in re.findall(r"(?m)^ipcclient-1:log-contract seq=(\d+)$", text)]
            assert stdout_rows == list(range(len(results))), stdout_rows
            final_codes = [int(code) for code in re.findall(r"log-contract final code=(-?\d+)", text)]
            assert final_codes and final_codes[-1] == 0 and all(code == -5 for code in final_codes[:-1]), final_codes
            disk = "".join(path.read_text(encoding="utf-8", errors="replace") for path in sorted(logs.rglob("info")))
            rows = re.findall(r"(?m)^\d\d:\d\d:\d\d ipcclient-1:log-contract (seq=\d+|final)$", disk)
            assert rows == ["seq=%d" % i for i in range(len(results) - 1)] + ["final"], rows
            print("log contract accepted=%d busy=1 stdout=all disk=fifo final=1 PASS" % (len(results) - 1), flush=True)
            return False
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
            day_rows = {}
            for info in logs.rglob("info"):
                with info.open("rb") as stream:
                    for line in stream:
                        assert re.match(rb"\d{2}:\d{2}:\d{2} ", line), line[:40]
                        assert len(line) == line_bytes + 9 and line[9:-1] == b"L" * (line_bytes - 1)
                        rows += 1
                        if case == "log-backpressure" and not WINDOWS:
                            day = info.parent.name
                            assert day in ("24_01_01", "24_01_02"), day
                            assert line.startswith(b"12:" if day == "24_01_01" else b"13:"), (day, line[:9])
                            day_rows[day] = day_rows.get(day, 0) + 1
            if case == "log-backpressure" and not WINDOWS:
                assert all(day_rows.get(day, 0) > 0 for day in ("24_01_01", "24_01_02")), day_rows
                print("log rotation ordered", day_rows, "PASS", flush=True)
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
                if not WINDOWS:
                    # A regular file blocks the next day's directory. The same
                    # accelerated clock forces a real rotation in a new logd.
                    failure_root = ctx.root / "rotation-failure"
                    failure_root.mkdir()
                    (failure_root / "24_01_02").write_text("block", encoding="utf-8")
                    config = ctx.cfg / "logd.toml"
                    config.write_text(replace(config.read_text(encoding="utf-8"), "root_path",
                                              '"' + failure_root.as_posix() + '"'), encoding="utf-8")
                    failed_logd, failed_path = ctx.start("logd", env=ctx.log_env)
                    deadline = time.monotonic() + 10
                    failure = ""
                    while time.monotonic() < deadline:
                        failure = failed_path.read_text(encoding="utf-8", errors="replace")
                        if "logd mkdir failed:" in failure and "logd rotate open failed:" in failure:
                            break
                        if failed_logd.poll() is not None:
                            break
                        time.sleep(0.05)
                    assert failed_logd.poll() is None, failure
                    assert "logd mkdir failed:" in failure and "logd rotate open failed:" in failure, failure
                    ctx.stop(failed_logd)
                    failed_logd.wait(timeout=10)
                    ctx.procs.remove(failed_logd)
                    assert failed_logd.returncode == 0, failure
                    print("log rotation mkdir failure kept process PASS", flush=True)

                    # Pause only the writer's new-day fopen, then make the
                    # log server accept and reject a malformed frame.
                    gate_root = ctx.root / "rotation-gate"
                    gate_root.mkdir()
                    config.write_text(replace(config.read_text(encoding="utf-8"), "root_path",
                                              '"' + gate_root.as_posix() + '"'), encoding="utf-8")
                    marker = ctx.root / "rotation-open-waiting"
                    release = ctx.root / "rotation-open-release"
                    gate_env = dict(ctx.log_env, OI_TEST_ROTATION_OPEN_MARKER=str(marker),
                                    OI_TEST_ROTATION_OPEN_RELEASE=str(release))
                    gate_logd, gate_path = ctx.start("logd", env=gate_env)
                    try:
                        deadline = time.monotonic() + 8
                        while not marker.exists() and gate_logd.poll() is None and time.monotonic() < deadline:
                            time.sleep(0.01)
                        assert marker.exists(), gate_path.read_text(encoding="utf-8", errors="replace")
                        with socket.create_connection(("127.0.0.1", ports.LOGD_PORT), timeout=1) as probe:
                            probe.settimeout(1)
                            probe.sendall(struct.pack("<II", 0, 0xffffffff))
                            assert probe.recv(1) == b"", "logd did not reject malformed frame while open was blocked"
                    finally:
                        release.touch()
                    ctx.stop(gate_logd)
                    gate_logd.wait(timeout=10)
                    assert gate_logd.returncode == 0, gate_path.read_text(encoding="utf-8", errors="replace")
                    trace = (gate_root / "24_01_02" / "trace").read_text(encoding="utf-8", errors="replace")
                    assert "LogServer::OnAddSession" in trace, trace
                    print("log rotation blocked open poll responsive persisted PASS", flush=True)
        return False

CASES = (LogCase("log", "log"), LogCase("log-backpressure", "log"), LogCase("log-contract", "log"), LogCase("disk-full", "log"))
