import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

import run_regressions as regression
from regression import cases, ports, probes, web
from textio import TEXT, child_env


MULTIPART_BODY = (b"--fixture\r\nContent-Disposition: form-data; name=\"field\"\r\n\r\nvalue\r\n"
                  b"--fixture\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.bin\"\r\n"
                  b"Content-Type: application/octet-stream\r\n\r\n\x00\xffxyz\r\n--fixture--\r\n")


class RegressionChecksTest(unittest.TestCase):
    def test_blocking_wake_checks_actual_trace_capability(self):
        text = "\n".join("wake benchmark " + name + " n=100000" for name in
                         ("default_empty_serial", "native_coordinator_to_worker",
                          "native_worker_to_coordinator", "native_roundtrip"))
        ctx = SimpleNamespace(name="blocking-wake", text=text, work=Path("fixture"))
        case = cases.get_case("blocking-wake")
        with patch("regression.cases.db.test_hook_enabled", return_value=True) as query:
            with self.assertRaises(AssertionError):
                case.check(ctx)
            query.assert_called_once_with(ctx.work, "ipcclient", "blocking_wake_trace")
            ctx.text += "\n" + "\n".join("wake benchmark " + name + " n=100000" for name in
                ("trace_submit_to_worker", "trace_worker", "trace_done_to_post", "trace_post_to_poll",
                 "trace_poll_to_resume", "trace_finish_to_resume", "trace_total"))
            self.assertFalse(case.check(ctx))
        ctx.text = text
        with patch("regression.cases.db.test_hook_enabled", return_value=False):
            self.assertFalse(case.check(ctx))
        with patch("regression.cases.db.test_hook_enabled", side_effect=RuntimeError("unknown binary")):
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

if __name__ == "__main__":
    unittest.main()
