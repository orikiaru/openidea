import io
import subprocess
import sys
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

import run_regressions as regression


class RegressionChecksTest(unittest.TestCase):
    def test_disk_failure_requires_exit_and_write_error(self):
        for operation in ("fwrite", "fflush", "injected fwrite ENOSPC"):
            message = "logd %s failed: disk full\n" % operation
            self.assertTrue(regression.disk_failure(1, message))
            self.assertFalse(regression.disk_failure(None, message))
            self.assertFalse(regression.disk_failure(0, message))
        for message in ("logd listening", "logd open file failed: disk full", "other failed: disk full"):
            self.assertFalse(regression.disk_failure(1, message))

    def test_echo_total_is_scoped(self):
        config = "[Other]\ntotal = 1000000\n[TestEcho]\ntotal = 100 # short\ninflight = 4\n[After]\ntotal = 7\n"
        self.assertEqual(regression.echo_total(config), 100)
        updated = regression.set_echo_total(config, 200)
        self.assertIn("total = 200 # short", updated)
        self.assertIn("[After]\ntotal = 7", updated)
        with self.assertRaises(RuntimeError):
            regression.set_echo_total("[TestEcho]\ninflight = 4\n[After]\ntotal = 7\n", 100)
        with self.assertRaises(RuntimeError):
            regression.echo_total("[Other]\ntotal = 100\n")
        for total in ("0", "100\ntotal = 200"):
            with self.subTest(total=total), self.assertRaises(RuntimeError):
                regression.echo_total("[TestEcho]\ntotal = " + total + "\n")

    def test_echo_counts_and_connections(self):
        connection = "[2026-11-18 12:00:00] ipc client connect sid=1 mode=rpc total=100"
        result = "[2026-11-18 12:00:01] ipc mode=rpc total=100 ok=100 mismatch=0 rpc_fail=0 ms=1 rate=100/s"
        regression.check_echo(connection + "\n" + result, 100)
        for bad_result in (result.replace("total=100", "total=99"),
                           result.replace("ok=100", "ok=99"),
                           result.replace("mismatch=0", "mismatch=1"),
                           result.replace("rpc_fail=0", "rpc_fail=1")):
            with self.subTest(result=bad_result), self.assertRaises(AssertionError):
                regression.check_echo(connection + "\n" + bad_result, 100)
        for connections in ("", connection + "\n" + connection):
            with self.subTest(connections=connections), self.assertRaises(AssertionError):
                regression.check_echo(connections + "\n" + result, 100)

    def test_admission_requires_each_round_once(self):
        lines = ["[2026-11-18 12:00:00] hold rpc admission round=%d ok=2 busy=4 bad=0" % i for i in range(3)]
        regression.check_admission_rounds("\n".join(lines))
        with self.assertRaises(AssertionError):
            regression.check_admission_rounds("\n".join([lines[0]] * 3))
        with self.assertRaises(AssertionError):
            regression.check_admission_rounds("\n".join(lines + [lines[0]]))
        for invalid in (lines + ["hold rpc admission round=3 ok=0 busy=0 bad=1"],
                        [lines[0].replace("bad=0", "bad=01")] + lines[1:],
                        [lines[0].replace("ok=2", "ok=1")] + lines[1:],
                        [lines[0].replace("busy=4", "busy=3")] + lines[1:],
                        [lines[0].replace("bad=0", "bad=1")] + lines[1:]):
            with self.subTest(lines=invalid), self.assertRaises(AssertionError):
                regression.check_admission_rounds("\n".join(invalid))

    def test_web_skips_unrelated_port_preflight(self):
        with patch.object(regression.socket, "socket", side_effect=OSError("port occupied")) as probe:
            regression.preflight_ports("web")
            probe.assert_not_called()
            with self.assertRaisesRegex(OSError, "port occupied"):
                regression.preflight_ports("echo")
            probe.assert_called_once()

    def test_web_transport_runs_all_stages(self):
        payload = bytes(range(256)) * 2048
        ok = b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
        binary = (b"HTTP/1.1 200 OK\r\nContent-Length: 524288\r\n"
                  b"Connection: close\r\n\r\n" + payload)
        keep, chunk = MagicMock(), MagicMock()
        keep.__enter__.return_value = keep
        chunk.__enter__.return_value = chunk
        keep.makefile.return_value = io.BytesIO(ok + ok)
        chunk.makefile.return_value = io.BytesIO(binary)
        bad, healthy = MagicMock(), MagicMock()
        bad.getresponse.return_value.status = 400
        healthy.getresponse.return_value.status = 200
        healthy.getresponse.return_value.read.return_value = b"ok"
        with patch.object(regression.socket, "create_connection", side_effect=[keep, chunk]), \
             patch.object(regression.http.client, "HTTPConnection", side_effect=[bad, healthy]), \
             patch.object(regression.time, "sleep"):
            regression.check_web_transport(18080)
        self.assertEqual(keep.sendall.call_count, 3)
        self.assertEqual(chunk.sendall.call_count, 18)
        bad.request.assert_called_once_with("GET", "/query")
        healthy.request.assert_called_once_with("GET", "/health")
        bad.close.assert_called_once_with()
        healthy.close.assert_called_once_with()

    def test_hold_scene_preflight_only_checks_required_ports(self):
        for case, ports, udp in (
            ("web-coro", [regression.IPC_PORT, regression.REDIS_SERVER_PORT, regression.REDIS_PORT], False),
            ("hold-blocking", [regression.LOGD_PORT, regression.IPC_PORT], True),
            ("hold-timer", [regression.LOGD_PORT], False),
            ("hold-redis", [regression.LOGD_PORT, regression.REDIS_SERVER_PORT, regression.REDIS_PORT], False),
        ):
            with self.subTest(case=case), patch.object(regression.socket, "socket") as factory:
                regression.preflight_ports(case)
                calls = factory.return_value.__enter__.return_value.bind.call_args_list
                expected = ports + ([regression.UDP_PORT] if udp else [])
                self.assertEqual([call.args[0][1] for call in calls], expected)

    def test_hold_scenes_are_accepted_and_all_retains_hold(self):
        script = Path(regression.__file__)
        result = subprocess.run([sys.executable, str(script), "--help"], capture_output=True, text=True, check=True)
        for case in ("hold-blocking", "hold-timer", "hold-redis"):
            self.assertIn(case, result.stdout)
        self.assertEqual(regression.ALL_CASES.count("web"), 1)
        self.assertEqual(regression.ALL_CASES.count("web-coro"), 1)
        self.assertEqual(regression.ALL_CASES.count("hold"), 1)
        for case in ("hold-blocking", "hold-timer", "hold-redis"):
            self.assertNotIn(case, regression.ALL_CASES)

    def test_optimized_python_is_rejected(self):
        script = Path(regression.__file__)
        result = subprocess.run([sys.executable, "-O", str(script), "--help"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires assertions", result.stderr)


if __name__ == "__main__":
    unittest.main()
