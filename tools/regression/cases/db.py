"""db 组：MySQL 集成和阻塞唤醒基准。"""
import json
import math
import os
import re
import subprocess

from ..config import set_test_flags
from ..probes import EnvironmentBlocked
from regression_platform import SUFFIX
from ..procs import test_hook_enabled
from . import Case


def trace_enabled(work):
    test_hook_enabled(work, "ipcclient", "blocking_wake_trace")
    result = subprocess.run([str(work / ("ipcclient" + SUFFIX)), "--test-hooks"], cwd=work,
                            capture_output=True, text=True, timeout=10)
    values = re.findall(r"(?m)^blocking_wake_trace=([01])\s*$", result.stdout)
    if result.returncode not in (40, 41) or len(values) != 1:
        raise RuntimeError("cannot determine blocking wake trace capability")
    return values[0] == "1"


NATIVE = ("native_coordinator_to_worker", "native_worker_to_coordinator", "native_roundtrip")
TRACE = ("trace_submit_to_worker", "trace_worker", "trace_done_to_post", "trace_post_to_poll",
         "trace_poll_to_resume", "trace_finish_to_resume", "trace_total")
MYSQL = ("blocking_finish_to_resume", "mysql_direct_submit_to_worker_start",
         "mysql_direct_worker_start_to_finish", "mysql_direct_finish_to_resume",
         "mysql_execute_finish_to_resume_including_cleanup", "mysql_reset_finish_to_resume_including_cleanup",
         "mysql_call_total")
FLOAT = r"(?:[0-9]+(?:\.[0-9]+)?)"
REPORT = re.compile(r"^wake benchmark (\w+) n=(\d+) p50_us=(" + FLOAT + r") p95_us=(" + FLOAT +
                    r") p99_us=(" + FLOAT + r") max_us=(" + FLOAT + r") ge1000us=(\d+) sum_us=(" + FLOAT + r")$")
SERIAL = re.compile(r"^wake benchmark default_empty_serial n=(\d+) total_us=(" + FLOAT + r") mean_us=(" + FLOAT + r")$")


def parse_wake_metrics(text, case, hooks, budget=""):
    lines = [line.strip() for line in text.splitlines() if line.startswith("wake benchmark ")]
    metrics = {}
    serial = []
    for line in lines:
        report = REPORT.fullmatch(line)
        if report:
            name, count, p50, p95, p99, maximum, slow, total = report.groups()
            assert name not in metrics, "duplicate wake metric: " + name
            count, slow = int(count), int(slow)
            p50, p95, p99, maximum, total = map(float, (p50, p95, p99, maximum, total))
            assert all(math.isfinite(value) for value in (p50, p95, p99, maximum, total)), line
            assert p50 <= p95 <= p99 <= maximum and 0 <= slow <= count, line
            # Printed microseconds are rounded to 0.001; preserve the rounding margin.
            margin = (count + 1) * 0.0005 + 1e-9
            assert total + margin >= maximum and total <= count * maximum + margin, line
            for fraction, percentile in ((50, p50), (95, p95), (99, p99)):
                rank = (count * fraction + 99) // 100
                assert total + margin >= (count - rank + 1) * percentile, line
                if slow > count - rank:
                    assert percentile >= 999.999, line
            assert total + margin >= slow * 1000, line
            assert not (slow and maximum < 999.999), line
            assert not (slow == 0 and maximum > 1000.0), line
            metrics[name] = dict(n=count, p50_us=p50, p95_us=p95, p99_us=p99,
                                 max_us=maximum, ge1000us=slow, sum_us=total)
        elif line.startswith("wake benchmark default_empty_serial"):
            match = SERIAL.fullmatch(line)
            assert match, line
            serial.append(match.groups())
        elif re.match(r"^wake benchmark (?:native_|trace_|blocking_finish|mysql_)", line):
            raise AssertionError("invalid wake metric: " + line)
    expected = set(MYSQL if case == "mysql-wake" else NATIVE)
    count = 1000 if case == "mysql-wake" else 100000
    if case == "blocking-wake":
        skipped = lines.count("wake benchmark trace skipped multi-thread")
        unavailable = lines.count("wake benchmark trace unavailable hooks disabled")
        assert skipped + unavailable <= 1
        if skipped + unavailable == 0:
            assert hooks, "trace reported when hooks disabled"
            expected.update(TRACE)
        else:
            assert not hooks, "trace unexpectedly skipped or unavailable"
        assert len(serial) == 1, serial
        n, total, mean = serial[0]
        n, total, mean = int(n), float(total), float(mean)
        assert n == count and math.isfinite(total) and math.isfinite(mean) and total > 0 and mean >= 0
        assert abs(total / n - mean) <= 0.0005 + 0.0005 / n + 1e-9
        metrics["default_empty_serial"] = dict(n=n, total_us=total, mean_us=mean)
    assert set(metrics) == expected | ({"default_empty_serial"} if case == "blocking-wake" else set()), (set(metrics), expected)
    assert all(metrics[name]["n"] == count for name in expected), metrics
    if budget:
        for item in budget.split(","):
            name, separator, value = item.partition("=")
            assert separator and name in expected, "invalid p99 budget: " + item
            limit = float(value)
            assert math.isfinite(limit) and limit >= 0, item
            assert metrics[name]["p99_us"] <= limit, (name, metrics[name]["p99_us"], limit)
    return metrics


class DbCase(Case):
    def configure(self, ctx, name, text):
        case = ctx.name
        if name == "ipcclient":
            if case in ("blocking-wake", "mysql-wake"):
                text = text.replace("[Test]", "[Test]\nwake_case = \"" + ("blocking" if case == "blocking-wake" else "mysql") + "\"", 1)
            text = set_test_flags(text, ctx.mode)
        return text

    def environment_blocked(self, ctx, text):
        if ctx.name == "mysql" and "mysql environment blocked:" in text:
            raise EnvironmentBlocked("MySQL driver disabled in ipcclient build")

    def check(self, ctx):
        case, text = ctx.name, ctx.text
        if case in ("blocking-wake", "mysql-wake"):
            hooks = trace_enabled(ctx.work) if case == "blocking-wake" else False
            metrics = parse_wake_metrics(text, case, hooks)
            if ctx.args.artifacts:
                destination = ctx.args.artifacts / case
                destination.mkdir(parents=True, exist_ok=True)
                (destination / "wake-metrics.json").write_text(json.dumps(metrics, indent=2), encoding="utf-8")
            if os.environ.get("OI_WAKE_P99_LIMIT_US"):
                parse_wake_metrics(text, case, hooks, os.environ["OI_WAKE_P99_LIMIT_US"])
        elif case == "mysql":
            assert "mysql row mapping bad=0" in text
            assert "mysql transaction affected rows bad=0" in text
            assert "mysql native FLOAT roundtrip bad=0" in text
            assert "mysql cell predicates bad=0" in text
            if os.environ.get("OI_TEST_MYSQL_ISOLATED") == "1":
                assert "mysql default_table bad=0" in text
            assert "mysql all field types batch bad=0" in text
            assert "mysql error handling bad=0" in text
            assert re.search(r"database failure code=-11 database=\S+ table=<unknown> column=required_id missing", text)
            assert re.search(r"database failure code=-18 database=\S+ stage=prepare native=1054 sqlstate=42S22", text)
            assert re.search(r"database failure code=-3 database=\S+ stage=connect native=1045 sqlstate=28000", text)
            if os.environ.get("OI_TEST_DB_REFUSED_PORT"):
                assert re.search(r"database failure code=-3 database=\S+ stage=connect native=200[23] sqlstate=\S+", text)
            assert text.count("database failure code=-1 unexpected exception") == 1
            assert "oi_test_credential" not in text and "oi_private_exception_probe" not in text
            assert "mysql rows bad=0" in text
            assert "mysql done bad=0" in text
        return False

CASES = (
    DbCase("mysql", "db", targeted_reason="requires a real database; run the matrix with --mysqld"),
    DbCase("mysql-wake", "db", targeted_reason="database wake latency benchmark"),
    DbCase("blocking-wake", "db", targeted_reason="blocking wake latency benchmark"),
)
