import io
import json
import importlib
import pkgutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

import run_regressions as regression
from regression import cases, ports, probes, web_server_checks as web
from textio import TEXT, child_env


MULTIPART_BODY = (b"--fixture\r\nContent-Disposition: form-data; name=\"field\"\r\n\r\nvalue\r\n"
                  b"--fixture\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.bin\"\r\n"
                  b"Content-Type: application/octet-stream\r\n\r\n\x00\xffxyz\r\n--fixture--\r\n")


class RegressionChecksTest(unittest.TestCase):
    def test_config_arguments_rejects_optimized_mode(self):
        script = Path(__file__).with_name("test_config_arguments.py")
        with tempfile.TemporaryDirectory() as temp:
            result = subprocess.run([sys.executable, "-O", str(script), "--bin-dir", temp],
                                    capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("assertions enabled", result.stdout + result.stderr)
        self.assertNotIn("config arguments PASS", result.stdout + result.stderr)

    def test_web_stop_probe_registration_and_optimized_mode(self):
        import web_stop_probe
        self.assertIn("web-stop", cases.TARGETED_CASES)
        self.assertNotIn("web-stop", cases.ALL_CASES)
        self.assertTrue(web_stop_probe.idle_closed(0, "eof"))
        self.assertTrue(web_stop_probe.idle_closed(0, "rst winerror=10054"))
        self.assertFalse(web_stop_probe.idle_closed(0, "b'x'"))
        self.assertFalse(web_stop_probe.idle_closed(1, "eof"))
        result = subprocess.run([sys.executable, "-O", str(Path(web_stop_probe.__file__))],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("assertions enabled", result.stderr)

    def test_preflight_failure_replaces_stale_results_and_keeps_full_plan(self):
        from regression import runner
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            (output / "results.json").write_text('[{"case":"old","status":"passed"}]', encoding="utf-8")
            (output / "plan.json").write_text('{"selected":["old"]}', encoding="utf-8")
            argv = ["run_regressions.py", "--case", "all", "--expect-thread", "mt", "--artifacts", temp]
            with patch.object(sys, "argv", argv), patch.object(runner.ports, "apply_port_offset"), \
                    patch.object(runner.ports, "preflight_ports", side_effect=RuntimeError("port busy")), \
                    patch.object(runner, "run_case") as run_case:
                with self.assertRaisesRegex(RuntimeError, "port busy"):
                    runner.main()
                run_case.assert_not_called()
            plan = json.loads((output / "plan.json").read_text(encoding="utf-8"))
            self.assertEqual(plan["selected"], list(cases.ALL_CASES))
            self.assertEqual(plan["targeted_only"], cases.TARGETED_CASES)
            self.assertEqual(json.loads((output / "results.json").read_text(encoding="utf-8")), [])

    def test_case_registry_covers_every_group_and_classifies_cases(self):
        modules = [importlib.import_module(cases.__name__ + "." + item.name)
                   for item in pkgutil.iter_modules(cases.__path__)]
        groups = [entry for module in modules for entry in module.CASES]
        names = [entry.name for entry in groups]
        self.assertEqual(len(names), len(set(names)))
        for entry in groups:
            self.assertIs(cases.REGISTRY[entry.name], entry)
        standalone = {name for name, entry in cases.REGISTRY.items() if entry.standalone}
        self.assertEqual(set(cases.REGISTRY), set(names) | standalone)
        full, targeted = set(cases.ALL_CASES), set(cases.TARGETED_CASES)
        self.assertFalse(full & targeted)
        self.assertEqual(full | targeted, set(cases.REGISTRY))
        self.assertTrue(all(reason.strip() for reason in cases.TARGETED_CASES.values()))
        self.assertTrue({"log-rotation", "log-contract"} <= full)

    def test_registry_rejects_duplicate_and_reserved_names(self):
        entry = cases.Case("fixture", "fixture")
        for groups in (((entry,), (entry,)), ((cases.Case("all", "fixture"),),)):
            with self.subTest(groups=groups), self.assertRaises(ValueError):
                cases.register_cases(groups)

    def test_canonical_and_legacy_case_names_select_same_case(self):
        for name, entry in cases.REGISTRY.items():
            with self.subTest(name=name):
                self.assertEqual(cases.normalize_case_name("test-" + name), name)
                self.assertEqual(cases.normalize_case_name(name), name)
                self.assertIs(cases.get_case("test-" + name), entry)
                self.assertIs(cases.get_case(name), entry)

    def test_blocking_wake_checks_actual_trace_capability(self):
        def row(name):
            return ("wake benchmark " + name + " n=100000 p50_us=1.000 p95_us=1.000 "
                    "p99_us=1.000 max_us=1.000 ge1000us=0 sum_us=100000.000")
        text = "\n".join(["wake benchmark default_empty_serial n=100000 total_us=100000.000 mean_us=1.000"] +
                         [row(name) for name in ("native_coordinator_to_worker", "native_worker_to_coordinator", "native_roundtrip")])
        ctx = SimpleNamespace(name="blocking-wake", text=text, work=Path("fixture"), args=SimpleNamespace(artifacts=None))
        case = cases.get_case("blocking-wake")
        with patch("regression.cases.db.trace_enabled", return_value=True) as query:
            with self.assertRaises(AssertionError):
                case.check(ctx)
            query.assert_called_once_with(ctx.work)
            ctx.text += "\n" + "\n".join(row(name) for name in
                ("trace_submit_to_worker", "trace_worker", "trace_done_to_post", "trace_post_to_poll",
                 "trace_poll_to_resume", "trace_finish_to_resume", "trace_total"))
            self.assertFalse(case.check(ctx))
        ctx.text = text + "\nwake benchmark trace unavailable hooks disabled"
        with patch("regression.cases.db.trace_enabled", return_value=False):
            self.assertFalse(case.check(ctx))
        with patch("regression.cases.db.trace_enabled", side_effect=RuntimeError("unknown binary")):
            with self.assertRaisesRegex(RuntimeError, "unknown binary"):
                case.check(ctx)

    def test_hook_query_rejects_unknown_and_failed_programs(self):
        from regression.procs import test_hook_enabled
        from regression_platform import SUFFIX
        work = Path("fixture")
        for code, expected in ((41, True), (40, False)):
            with patch("regression.procs.subprocess.run", return_value=SimpleNamespace(returncode=code, stdout="", stderr="")) as run:
                self.assertEqual(test_hook_enabled(work, "ipcclient", "blocking_wake_trace"), expected)
                self.assertEqual(run.call_args.args[0], [str(work / ("ipcclient" + SUFFIX)), "--test-hooks"])
        for code in (0, 1, 42, -1):
            with patch("regression.procs.subprocess.run", return_value=SimpleNamespace(returncode=code, stdout="", stderr="")):
                with self.assertRaises(RuntimeError):
                    test_hook_enabled(work, "ipcclient", "blocking_wake_trace")
        with patch("regression.procs.subprocess.run", side_effect=subprocess.TimeoutExpired("binary", 10)):
            with self.assertRaises(RuntimeError):
                test_hook_enabled(work, "ipcclient", "blocking_wake_trace")

    def test_windows_log_fault_checks_logd_capability(self):
        from regression.cases import log
        ctx = SimpleNamespace(name="disk-full", work=Path("fixture"))
        case = cases.get_case("disk-full")
        with patch.object(log, "WINDOWS", True), patch.object(log, "test_hook_enabled", return_value=False) as query:
            with self.assertRaises(RuntimeError):
                case.prepare(ctx)
            query.assert_called_once_with(ctx.work, "logd", "log_fault")
        with patch.object(log, "WINDOWS", True), patch.object(log, "test_hook_enabled", return_value=True):
            case.prepare(ctx)
            self.assertEqual(ctx.log_env["OI_TEST_LOG_FAULT"], "full")

    def test_disk_failure_requires_exit_and_write_error(self):
        for operation in ("fwrite", "fflush", "injected fwrite ENOSPC"):
            message = "logd %s failed: disk full\n" % operation
            self.assertTrue(probes.disk_failure(1, message))
            self.assertFalse(probes.disk_failure(None, message))
            self.assertFalse(probes.disk_failure(0, message))
        for message in ("logd listening", "logd open file failed: disk full", "other failed: disk full"):
            self.assertFalse(probes.disk_failure(1, message))

    def test_echo_total_is_scoped(self):
        config = "[Other]\ntotal = 1000000\n[TestEcho]\ntotal = 100 # short\ninflight = 4\n[After]\ntotal = 7\n"
        self.assertEqual(probes.echo_total(config), 100)
        updated = probes.set_echo_total(config, 200)
        self.assertIn("total = 200 # short", updated)
        self.assertIn("[After]\ntotal = 7", updated)
        with self.assertRaises(RuntimeError):
            probes.set_echo_total("[TestEcho]\ninflight = 4\n[After]\ntotal = 7\n", 100)
        with self.assertRaises(RuntimeError):
            probes.echo_total("[Other]\ntotal = 100\n")
        for total in ("0", "100\ntotal = 200"):
            with self.subTest(total=total), self.assertRaises(RuntimeError):
                probes.echo_total("[TestEcho]\ntotal = " + total + "\n")

    def test_echo_counts_and_connections(self):
        connection = "[2026-11-18 12:00:00] ipc client connect sid=1 mode=rpc total=100"
        result = "[2026-11-18 12:00:01] ipc mode=rpc total=100 ok=100 mismatch=0 rpc_fail=0 ms=1 rate=100/s"
        probes.check_echo(connection + "\n" + result, 100)
        for bad_result in (result.replace("total=100", "total=99"),
                           result.replace("ok=100", "ok=99"),
                           result.replace("mismatch=0", "mismatch=1"),
                           result.replace("rpc_fail=0", "rpc_fail=1")):
            with self.subTest(result=bad_result), self.assertRaises(AssertionError):
                probes.check_echo(connection + "\n" + bad_result, 100)
        for connections in ("", connection + "\n" + connection):
            with self.subTest(connections=connections), self.assertRaises(AssertionError):
                probes.check_echo(connections + "\n" + result, 100)

    def test_admission_requires_each_round_once(self):
        lines = ["[2026-11-18 12:00:00] hold rpc admission round=%d ok=2 busy=4 bad=0" % i for i in range(3)]
        probes.check_admission_rounds("\n".join(lines))
        with self.assertRaises(AssertionError):
            probes.check_admission_rounds("\n".join([lines[0]] * 3))
        with self.assertRaises(AssertionError):
            probes.check_admission_rounds("\n".join(lines + [lines[0]]))
        for invalid in (lines + ["hold rpc admission round=3 ok=0 busy=0 bad=1"],
                        [lines[0].replace("bad=0", "bad=01")] + lines[1:],
                        [lines[0].replace("ok=2", "ok=1")] + lines[1:],
                        [lines[0].replace("busy=4", "busy=3")] + lines[1:],
                        [lines[0].replace("bad=0", "bad=1")] + lines[1:]):
            with self.subTest(lines=invalid), self.assertRaises(AssertionError):
                probes.check_admission_rounds("\n".join(invalid))

    def test_web_skips_unrelated_port_preflight(self):
        with patch.object(ports.socket, "socket", side_effect=OSError("port occupied")) as probe:
            ports.preflight_ports("web")
            probe.assert_not_called()
            with self.assertRaisesRegex(OSError, "port occupied"):
                ports.preflight_ports("echo")
            probe.assert_called_once()

    def test_multipart_oracle_rejects_concatenated_values(self):
        with self.assertRaises(AssertionError):
            web.check_multipart_response("multipart/form-data; boundary=fixture", b"value\x00\xffxyz")

    def test_multipart_oracle_accepts_exact_parts(self):
        web.check_multipart_response("multipart/form-data; boundary=fixture", MULTIPART_BODY)
        for invalid in (MULTIPART_BODY.replace(b'name="field"', b'name="wrong"'),
                        MULTIPART_BODY.replace(b'filename="a.bin"', b'filename="other"'),
                        MULTIPART_BODY.replace(b'application/octet-stream', b'text/plain'),
                        MULTIPART_BODY.replace(b'\x00\xffxyz', b'\x00\xffxyZ'),
                        MULTIPART_BODY.replace(b'--fixture--\r\n', b'')):
            with self.subTest(invalid=invalid), self.assertRaises(AssertionError):
                web.check_multipart_response("multipart/form-data; boundary=fixture", invalid)

    def test_web_transport_runs_all_stages(self):
        payload = bytes(range(256)) * 2048
        ok = b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
        binary = (b"HTTP/1.1 200 OK\r\nContent-Length: 524288\r\n"
                  b"Connection: close\r\n\r\n" + payload)
        idle_first, idle_keep = MagicMock(), MagicMock()
        expect_continue, expect_unknown = MagicMock(), MagicMock()
        fixed_large, chunk_large, fixed_over, chunk_over = (MagicMock() for _ in range(4))
        keep, half, chunk, multipart, abandoned = MagicMock(), MagicMock(), MagicMock(), MagicMock(), MagicMock()
        for conn in (idle_first, idle_keep, expect_continue, expect_unknown, fixed_large, chunk_large, fixed_over, chunk_over):
            conn.__enter__.return_value = conn
        idle_first.recv.return_value = b""
        idle_keep.makefile.return_value = io.BytesIO(ok)
        expect_continue.makefile.return_value = io.BytesIO(b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ntest")
        expect_unknown.makefile.return_value = io.BytesIO(b"HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\n\r\n")
        fixed_large.makefile.return_value = io.BytesIO(binary)
        chunk_large.makefile.return_value = io.BytesIO(binary)
        for conn in (fixed_over, chunk_over):
            conn.makefile.return_value = io.BytesIO(b"HTTP/1.1 413 Content Too Large\r\nContent-Length: 0\r\n\r\n")
        keep.__enter__.return_value = keep
        half.__enter__.return_value = half
        chunk.__enter__.return_value = chunk
        multipart.__enter__.return_value = multipart
        abandoned.__enter__.return_value = abandoned
        keep.makefile.return_value = io.BytesIO(ok + ok)
        half.makefile.return_value = io.BytesIO(ok)
        chunk.makefile.return_value = io.BytesIO(binary)
        multipart.makefile.return_value = io.BytesIO(
            b"HTTP/1.1 200 OK\r\nContent-Type: multipart/form-data; boundary=fixture\r\n" +
            ("Content-Length: %d\r\n\r\n" % len(MULTIPART_BODY)).encode() + MULTIPART_BODY)
        boundary = [MagicMock() for _ in range(6)]
        closing = [MagicMock() for _ in range(2)]
        for index, conn in enumerate(boundary):
            conn.__enter__.return_value = conn
            stream_size = 9 * 1024 * 1024
            pattern = bytes(range(251))
            stream_body = (pattern * (stream_size // len(pattern) + 1))[:stream_size]
            response = ok if index < 3 else (b"HTTP/1.1 200 OK\r\nContent-Length: 9437184\r\n\r\n" + stream_body)
            conn.makefile.return_value = io.BytesIO(response if index % 3 != 2 else b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
        for conn in closing:
            conn.__enter__.return_value = conn
            conn.makefile.return_value = io.BytesIO(ok)
        bad, healthy = MagicMock(), MagicMock()
        bad.getresponse.return_value.status = 400
        healthy.getresponse.return_value.status = 200
        healthy.getresponse.return_value.read.return_value = b"ok"
        with patch.object(web.socket, "create_connection", side_effect=[idle_first, idle_keep, expect_continue, expect_unknown, keep, half,
                                                                          fixed_large, chunk_large, fixed_over,
                                                                          chunk_over, chunk] + boundary + closing + [multipart, abandoned]), \
             patch.object(web.http.client, "HTTPConnection", side_effect=[bad, healthy]), \
             patch.object(web.time, "sleep"), \
             patch.object(web.time, "monotonic", side_effect=[0, 3, 0, 3]), \
             patch.object(web, "wait_web_marker") as wait_marker:
            web.check_web_transport(18080, Path("server.out"))
        self.assertEqual(keep.sendall.call_count, 3)
        half.shutdown.assert_called_once_with(web.socket.SHUT_WR)
        self.assertEqual(chunk.sendall.call_count, 18)
        self.assertGreater(multipart.sendall.call_count, 2)
        abandoned.sendall.assert_called_once()
        self.assertEqual([call.args[1] for call in wait_marker.call_args_list],
                         ["web disconnect sink created", "web disconnect sink released cancelled=1"])
        bad.request.assert_called_once_with("GET", "/query")
        healthy.request.assert_called_once_with("GET", "/health")
        bad.close.assert_called_once_with()
        healthy.close.assert_called_once_with()

    def test_hold_scene_preflight_only_checks_required_ports(self):
        for case, expected_ports, udp in (
            ("web-coro", [ports.IPC_PORT, ports.REDIS_SERVER_PORT, ports.REDIS_PORT], False),
            ("hold-blocking", [ports.LOGD_PORT, ports.IPC_PORT], True),
            ("hold-timer", [ports.LOGD_PORT], False),
            ("hold-redis", [ports.LOGD_PORT, ports.REDIS_SERVER_PORT, ports.REDIS_PORT], False),
        ):
            with self.subTest(case=case), patch.object(ports.socket, "socket") as factory:
                ports.preflight_ports(case)
                calls = factory.return_value.__enter__.return_value.bind.call_args_list
                expected = expected_ports + ([ports.UDP_PORT] if udp else [])
                addresses = ("127.0.0.1", "0.0.0.0") if ports.WINDOWS else ("127.0.0.1",)
                self.assertEqual([call.args[0] for call in calls],
                                 [(address, port) for port in expected for address in addresses])

    def test_hold_scenes_are_accepted_and_all_retains_hold(self):
        script = Path(regression.__file__)
        result = subprocess.run([sys.executable, str(script), "--help"], capture_output=True, text=True,
                                check=True, env=child_env(), **TEXT)
        for case in ("hold-blocking", "hold-timer", "hold-redis"):
            self.assertIn(case, result.stdout)
        self.assertEqual(cases.ALL_CASES.count("web"), 1)
        self.assertEqual(cases.ALL_CASES.count("web-coro"), 1)
        self.assertEqual(cases.ALL_CASES.count("hold"), 1)
        for case in ("hold-blocking", "hold-timer", "hold-redis"):
            self.assertNotIn(case, cases.ALL_CASES)

    def test_optimized_python_is_rejected(self):
        script = Path(regression.__file__)
        result = subprocess.run([sys.executable, "-O", str(script), "--help"], capture_output=True, text=True,
                                env=child_env(), **TEXT)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires assertions", result.stderr)



class WakeMetricsChecks(unittest.TestCase):
    def test_blocking_metrics_and_negative_cases(self):
        from regression.cases.db import NATIVE, TRACE, parse_wake_metrics
        def row(name, count=100000):
            return ("wake benchmark %s n=%d p50_us=1.000 p95_us=2.000 p99_us=3.000 "
                    "max_us=4.000 ge1000us=0 sum_us=150000.000" % (name, count))
        serial = "wake benchmark default_empty_serial n=100000 total_us=100000.000 mean_us=1.000"
        rows = [row(name) for name in NATIVE + TRACE]
        # A zero-length substage is legitimate.
        rows[-1] = rows[-1].replace("p50_us=1.000 p95_us=2.000 p99_us=3.000 max_us=4.000 ge1000us=0 sum_us=150000.000",
                                  "p50_us=0.000 p95_us=0.000 p99_us=0.000 max_us=0.000 ge1000us=0 sum_us=0.000")
        valid = "\n".join([serial] + rows)
        self.assertEqual(parse_wake_metrics(valid, "blocking-wake", True)["trace_total"]["sum_us"], 0)
        for bad in (valid + "\n" + rows[0], valid.replace(rows[0], ""),
                    valid.replace("n=100000 p50", "n=99999 p50", 1),
                    valid.replace("p95_us=2.000", "p95_us=0.500", 1),
                    valid.replace("ge1000us=0", "ge1000us=100001", 1),
                    valid.replace("ge1000us=0", "ge1000us=1", 1),
                    valid.replace("sum_us=150000.000", "sum_us=0.000", 1),
                    valid.replace("mean_us=1.000", "mean_us=2.000", 1)):
            with self.subTest(bad=bad[:70]), self.assertRaises(AssertionError):
                parse_wake_metrics(bad, "blocking-wake", True)
        with self.assertRaises(AssertionError):
            parse_wake_metrics(valid, "blocking-wake", True, "native_roundtrip=2.9")
        self.assertTrue(parse_wake_metrics(valid, "blocking-wake", True, "native_roundtrip=3.0"))
        mt = "\n".join([serial] + [row(name) for name in NATIVE] + ["wake benchmark trace skipped multi-thread"])
        self.assertTrue(parse_wake_metrics(mt, "blocking-wake", False))
        with self.assertRaises(AssertionError):
            parse_wake_metrics(mt, "blocking-wake", True)
        unavailable = mt.replace("trace skipped multi-thread", "trace unavailable hooks disabled")
        self.assertTrue(parse_wake_metrics(unavailable, "blocking-wake", False))

    def test_trace_capability_uses_actual_trace_bit(self):
        from regression.cases.db import trace_enabled
        work = Path("fixture")
        with patch("regression.cases.db.test_hook_enabled", return_value=True), \
                patch("regression.cases.db.subprocess.run", return_value=SimpleNamespace(returncode=41, stdout="blocking_wake_trace=0\n")):
            self.assertFalse(trace_enabled(work))
        with patch("regression.cases.db.test_hook_enabled", return_value=True), \
                patch("regression.cases.db.subprocess.run", return_value=SimpleNamespace(returncode=41, stdout="blocking_wake_trace=1\n")):
            self.assertTrue(trace_enabled(work))
        with patch("regression.cases.db.test_hook_enabled", return_value=True), \
                patch("regression.cases.db.subprocess.run", return_value=SimpleNamespace(returncode=41, stdout="")):
            with self.assertRaises(RuntimeError):
                trace_enabled(work)

    def test_mysql_metrics_all_required(self):
        from regression.cases.db import MYSQL, parse_wake_metrics
        rows = ["wake benchmark %s n=1000 p50_us=1.000 p95_us=2.000 p99_us=3.000 max_us=4.000 ge1000us=0 sum_us=1500.000" % name for name in MYSQL]
        self.assertEqual(len(parse_wake_metrics("\n".join(rows), "mysql-wake", False)), len(MYSQL))
        with self.assertRaises(AssertionError):
            parse_wake_metrics("\n".join(rows[:-1]), "mysql-wake", False)

class CoroResultChecks(unittest.TestCase):
    def test_coro_result_counts_and_timing(self):
        from types import SimpleNamespace
        case = cases.get_case("coro")
        valid = ("coro live=10 churn=20 held=10 wake=10 done=20 workers=1 "
                 "live_ns=5 live_rate=1 wake_ns=6 wake_rate=1 churn_ns=7 churn_rate=1 bad=0")
        case.check(SimpleNamespace(mode="coro", text=valid))
        for invalid in (valid.replace("wake=10", "wake=9"),
                        valid.replace("live_ns=5", "live_ns=0"),
                        valid.replace("bad=0", "bad=1") + "\nother bad=0"):
            with self.subTest(invalid=invalid), self.assertRaises(AssertionError):
                case.check(SimpleNamespace(mode="coro", text=invalid))

class FakeGreenChecks(unittest.TestCase):
    def test_rpc_race_rejects_timeout_duplicate_and_wrong_content(self):
        from regression.cases.hold import check_rpc_race, require_thread_mode
        marks = ",".join("%d:%s" % (100 + index, "R" if index < 2 else "T") for index in range(8))
        valid = "hold rpc race marks=%s reply=2 timeout=6 controlled_reply=1 controlled_timeout=1 negatives=3 bad=0" % marks
        check_rpc_race(valid)
        timed_out = valid.replace(marks, ",".join("%d:T" % (100 + index) for index in range(8))).replace("reply=2 timeout=6", "reply=0 timeout=8")
        duplicated = valid.replace("100:R,101:R", "100:R,100:R", 1)
        wrong = valid.replace("100:R", "100:B", 1)
        for bad in (timed_out, duplicated, wrong, valid.replace("negatives=3", "negatives=0"), valid.replace("bad=0", "bad=1")):
            with self.subTest(bad=bad):
                with self.assertRaises(AssertionError):
                    check_rpc_race(bad)
        st = "hold foreign task st rejected=1 runs=0 bad=0\nwarning: coroutine start rejected: current thread is not the single-thread poll thread"
        mt = "hold foreign task mt skipped=1 bad=0"
        require_thread_mode(st, "st")
        require_thread_mode(mt, "mt")
        with self.assertRaises(AssertionError):
            require_thread_mode(mt, "st")
        with self.assertRaises(AssertionError):
            require_thread_mode(st, "mt")
        with self.assertRaises(AssertionError):
            require_thread_mode(st + "\n" + mt, "st")

    def test_hold_and_udp_require_expected_thread(self):
        script = Path(regression.__file__)
        result = subprocess.run([sys.executable, str(script), "--case", "hold"], capture_output=True, text=True,
                                env=child_env(), **TEXT)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expect-thread", result.stderr)


if __name__ == "__main__":
    unittest.main()
