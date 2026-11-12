"""Exercise owned process lifecycle on the current OS without application binaries."""
import os
import contextlib
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

from regression_platform import launch, pause, rss_kib, stop, WslRedis


class ProcessFixtureTests(unittest.TestCase):
    def test_pause_resume_and_graceful_stop(self):
        with tempfile.TemporaryDirectory(prefix="oi-process-test-") as directory:
            beat = Path(directory) / "beat"
            script = ("import signal,sys,time,pathlib\n"
                      "signal.signal(signal.SIGINT,lambda *args:sys.exit(0))\n"
                      "signal.signal(signal.SIGTERM,lambda *args:sys.exit(0))\n"
                      "p=pathlib.Path(sys.argv[1])\n"
                      "while True:\n p.write_text(str(time.monotonic_ns()))\n time.sleep(.03)\n")
            proc = launch([sys.executable, "-c", script, str(beat)], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL)
            suspended = False
            try:
                end = time.monotonic() + 5
                while not beat.exists() and time.monotonic() < end:
                    time.sleep(.02)
                self.assertTrue(beat.exists())
                self.assertGreater(rss_kib(proc), 0)
                pause(proc, True)
                suspended = True
                time.sleep(.1)
                before = beat.read_text()
                time.sleep(.1)
                self.assertEqual(before, beat.read_text())
                pause(proc, False)
                suspended = False
                end = time.monotonic() + 2
                while beat.read_text() == before and time.monotonic() < end:
                    time.sleep(.03)
                self.assertNotEqual(before, beat.read_text())
                stop(proc)
                self.assertEqual(proc.wait(timeout=5), 0)
            finally:
                if suspended:
                    pause(proc, False)
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()

    def test_wsl_redis_paths_are_not_shell_escaped(self):
        with mock.patch("regression_platform.subprocess.check_output", side_effect=lambda command, **kwargs: command[-1]), \
             mock.patch("regression_platform.launch") as start:
            WslRedis(Path("/tmp/redis-server"), Path("fixture dir/redis.conf"), "jxy")
            command = start.call_args.args[0]
            self.assertEqual(command[:5], ["wsl", "-u", "jxy", "--exec", "python3"])
            self.assertEqual(command[6], "/tmp/redis-server")
            self.assertTrue(all("\\" not in arg for arg in command[5:]))
            self.assertEqual(command[7], "fixture dir/redis.conf")

    def test_reject_invalid_redis_pid(self):
        with tempfile.TemporaryDirectory(prefix="oi-pid-test-") as directory:
            owned = WslRedis.__new__(WslRedis)
            owned.pidfile = Path(directory) / "redis.pid"
            owned.prefix = []
            for pid in ("0", "1", "-1"):
                owned.pidfile.write_text(pid)
                with self.assertRaises(RuntimeError):
                    owned.control("TERM")


class StopHelperTests(unittest.TestCase):
    def test_helper_failure_reports_captured_output(self):
        proc = mock.Mock(pid=123)
        proc.poll.return_value = None
        error = subprocess.CalledProcessError(120, "helper", output=b"out", stderr=b"failure detail")
        output = io.StringIO()
        with mock.patch("regression_platform.WINDOWS", True), \
             mock.patch("subprocess.CREATE_NO_WINDOW", 0, create=True), \
             mock.patch("regression_platform.subprocess.run", side_effect=error) as helper, \
             contextlib.redirect_stderr(output):
            with self.assertRaises(subprocess.CalledProcessError):
                stop(proc)
        self.assertIn("returncode=120", output.getvalue())
        self.assertIn("failure detail", output.getvalue())
        self.assertEqual(helper.call_args.kwargs["stdout"], subprocess.PIPE)
        self.assertEqual(helper.call_args.kwargs["stderr"], subprocess.PIPE)

    def test_helper_failures_remain_failures_for_live_process(self):
        for error in (subprocess.CalledProcessError(1, "helper"),
                      subprocess.TimeoutExpired("helper", 5), OSError("launch failed")):
            with self.subTest(error=type(error).__name__):
                proc = mock.Mock(pid=123)
                proc.poll.return_value = None
                with mock.patch("regression_platform.WINDOWS", True), \
                     mock.patch("subprocess.CREATE_NO_WINDOW", 0, create=True), \
                     mock.patch("regression_platform.subprocess.run", side_effect=error):
                    with self.assertRaises(type(error)):
                        stop(proc)
                proc.kill.assert_not_called()

    def test_exit_between_poll_and_attach_is_benign(self):
        proc = mock.Mock(pid=123)
        proc.poll.side_effect = [None, 0]
        with mock.patch("regression_platform.WINDOWS", True), \
             mock.patch("subprocess.CREATE_NO_WINDOW", 0, create=True), \
             mock.patch("regression_platform.subprocess.run", side_effect=subprocess.CalledProcessError(1, "helper")):
            stop(proc)
        proc.kill.assert_not_called()

    def test_already_exited_never_sends_control(self):
        proc = mock.Mock()
        proc.poll.return_value = 0
        with mock.patch("regression_platform.subprocess.run") as helper:
            stop(proc)
        helper.assert_not_called()


if __name__ == "__main__":
    unittest.main()
