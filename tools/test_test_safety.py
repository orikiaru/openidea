"""Targeted safety checks; never enumerate/kill real processes or run global cleanup.

Run with Python on Windows and WSL. Process control in unit tests is mocked.
Set OI_SAFETY_REDIS_BINARY to additionally run a real Redis bind collision;
that test owns its socket and child, and forbids all outgoing connections.
"""
import contextlib
import io
import json
import os
from pathlib import Path
import runpy
import shutil
import socket
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import cleanup_linux_tests as linux
import kill_test_procs as windows
import test_matrix as matrix
import test_process_owner as owner
from diag import redis_link_probe as probe


class SafetyTests(unittest.TestCase):
    def enterContext(self, manager):
        value = manager.__enter__()
        self.addCleanup(manager.__exit__, None, None, None)
        return value

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="oi-safety-unit-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.lanes = self.root / "lanes"
        self.marked = self.root / "oi-owned"
        self.marked.mkdir()
        owner.write_owner(self.marked, self.repo, 2000)
        # Belt and braces: every test forbids actual termination and external tools.
        self.kill = self.enterContext(patch("os.kill", side_effect=AssertionError("real kill forbidden")))
        self.enterContext(patch("subprocess.run", side_effect=AssertionError("external command forbidden")))
        self.enterContext(patch("subprocess.Popen", side_effect=AssertionError("process launch forbidden")))
        self.enterContext(patch("subprocess.check_output", side_effect=AssertionError("external command forbidden")))

    def process(self, pid, path, command=""):
        return dict(ProcessId=pid, ParentProcessId=0, ExecutablePath=str(path), CommandLine=command,
                    CreationFileTimeUtc=100)

    def select(self, processes, records=None, lane="all", birth=100):
        with patch.object(windows, "ROOT", self.repo), \
             patch.object(windows, "windows_processes", return_value=processes), \
             patch.object(windows, "windows_records", return_value=records or {}), \
             patch.object(windows, "windows_birth", return_value=birth):
            return windows.windows_targets(lane, 2000, self.lanes)

    def test_other_repository_python_is_preserved(self):
        for name in ("redis_fixture.py", "run_regressions.py", "test_matrix.py"):
            script = self.root / "other" / name
            with self.subTest(script=name):
                self.assertFalse(linux.matches("/usr/bin/python3", ["python3", str(script)],
                                               str(self.repo), [self.repo], None))
                self.assertEqual([], self.select([self.process(901, "/usr/bin/python3",
                                                               'python3 "' + str(script) + '"')]))

    def test_foreign_redis_ports_are_preserved(self):
        for port in (6379, 6380, 16380, 18380):
            with self.subTest(port=port):
                self.assertFalse(linux.matches("/usr/bin/redis-server", ["redis-server", "*:%d" % port],
                                               str(self.root / "foreign"), [self.repo], None))
                self.assertEqual([], self.select([self.process(901, "/usr/bin/redis-server",
                                                               "redis-server *:%d" % port)]))

    def test_owner_marker_and_lane_offset(self):
        for offset, expected in ((2000, True), (3000, False), (None, True)):
            with self.subTest(offset=offset):
                self.assertEqual(expected, bool(owner.read_owner(self.marked, [self.repo], offset)))
                self.assertFalse(linux.matches("/usr/bin/redis-server", ["redis-server", "*:18380"],
                                               str(self.marked), [self.repo], offset))
        self.assertFalse(owner.owned_path(self.marked / "bin/server", [self.root / "other"], 2000))

    def test_repository_prefix_boundary(self):
        self.assertFalse(owner.within(str(self.repo) + "-other/tools/x.py", self.repo))

    def test_parent_traversal_cannot_claim_other_repository(self):
        escaped = str(self.repo / ".." / "foreign" / "worker.py")
        self.assertFalse(owner.within(escaped, self.repo))
        self.assertFalse(owner.owned_path(escaped, [self.repo]))
        self.assertFalse(owner.within("E:/repo/../foreign/x.py", "E:/repo"))
        self.assertFalse(owner.within("/mnt/e/repo/../foreign/x.py", "E:/repo"))
        self.assertTrue(owner.within(str(self.repo / "tools" / ".." / "worker.py"), self.repo))

    def test_empty_relative_paths_and_malformed_process_records(self):
        for path in ("", "redis_fixture.py", "../repo/worker.py"):
            self.assertFalse(owner.owned_path(path, [self.repo]))
        for records in (None, {}, "bad", [None, 1, {}, {"pid": True, "birth": 1},
                                         {"pid": -1, "birth": 2}, {"pid": 901, "birth": "100"}]):
            with self.subTest(records=records):
                data = {"repository": str(self.repo), "offset": 2000, "processes": records}
                (self.marked / owner.MARKER).write_text(json.dumps(data), encoding="utf-8")
                with patch.object(owner.tempfile, "gettempdir", return_value=str(self.root)):
                    self.assertEqual({}, owner.windows_records([self.repo]))

    def test_malformed_owner_marker_fails_closed(self):
        for value in ([], [1], None, True, 5, "owner", {}, {"repository": None, "offset": 2000}):
            with self.subTest(value=value):
                (self.marked / owner.MARKER).write_text(json.dumps(value), encoding="utf-8")
                self.assertIsNone(owner.read_owner(self.marked, [self.repo], 2000))

    def test_windows_records_non_mapping_marker_fails_closed(self):
        (self.marked / owner.MARKER).write_text('[]', encoding="utf-8")
        with patch.object(owner.tempfile, "gettempdir", return_value=str(self.root)):
            self.assertEqual({}, owner.windows_records([self.repo]))

    def test_windows_owned_pid_birth_and_lane(self):
        data = owner.read_owner(self.marked, [self.repo])
        data["processes"] = [{"pid": 901, "birth": 100}]
        (self.marked / owner.MARKER).write_text(json.dumps(data), encoding="utf-8")
        with patch.object(owner.tempfile, "gettempdir", return_value=str(self.root)):
            self.assertEqual({901: 100}, owner.windows_records([self.repo], 2000))
            self.assertEqual({}, owner.windows_records([self.repo], 3000))
        process = self.process(901, "/usr/bin/redis-server")
        self.assertEqual([(901, process["ExecutablePath"], 100)], self.select([process], {901: 100}))
        self.assertEqual([], self.select([process], {901: 99}))

    def test_windows_reused_marker_pid_does_not_open_foreign_process(self):
        process = self.process(901, "")
        process["CreationFileTimeUtc"] = 200
        with patch.object(windows, "windows_processes", return_value=[process]), \
             patch.object(windows, "windows_records", return_value={901: 100}), \
             patch.object(windows, "windows_birth", side_effect=AssertionError("foreign PID opened")) as birth:
            self.assertEqual([], windows.windows_targets("windows-st", 1000, self.lanes))
            birth.assert_not_called()

    def test_windows_lane_excludes_other_lane(self):
        own = self.process(901, self.lanes / "linux-mt" / "bin/ipcclient")
        other = self.process(902, self.lanes / "linux-st" / "bin/ipcclient")
        self.assertEqual([(901, own["ExecutablePath"], 100)], self.select([own, other], lane="linux-mt"))

    def test_windows_birth_exit_skipped_permission_failure_reported(self):
        for code in (87, 5):
            error = OSError("identity unavailable")
            error.winerror = code
            with self.subTest(error=type(error).__name__), \
                 patch.object(windows, "windows_processes", return_value=[self.process(901, "/usr/bin/redis-server")]), \
                 patch.object(windows, "windows_records", return_value={901: 100}), \
                 patch.object(windows, "windows_birth", side_effect=error):
                if code == 87:
                    self.assertEqual([], windows.windows_targets("all", 0, self.lanes))
                else:
                    with self.assertRaises(OSError):
                        windows.windows_targets("all", 0, self.lanes)

    def test_windows_unclosed_command_quote_is_skipped(self):
        self.assertEqual([], self.select([self.process(901, "/usr/bin/python3", 'python3 "unterminated')]))

    def test_windows_stale_cim_path_does_not_claim_reused_pid(self):
        process = self.process(901, self.repo / "bin/ipcclient.exe")
        self.assertEqual([], self.select([process], birth=200))
        for value in (None, "100", 0):
            with self.subTest(snapshot=value):
                process["CreationFileTimeUtc"] = value
                self.assertEqual([], self.select([process]))
                self.assertEqual([(901, "owner marker PID 901", 100)],
                                 self.select([process], records={901: 100}))

    def test_windows_cim_microsecond_precision(self):
        process = self.process(901, self.repo / "bin/ipcclient.exe")
        self.assertEqual([(901, process["ExecutablePath"], 109)], self.select([process], birth=109))
        self.assertEqual([], self.select([process], birth=110))

    def test_linux_records_persist_start_and_isolate_lane(self):
        with patch.object(owner, "os", SimpleNamespace(name="posix")), \
             patch.object(owner, "linux_start", return_value=12345):
            owner.record_process(self.marked, Mock(pid=901))
        with patch.object(owner.tempfile, "gettempdir", return_value=str(self.root)):
            self.assertEqual({901: 12345}, owner.linux_records([self.repo], 2000))
            self.assertEqual({}, owner.linux_records([self.repo], 3000))
            self.assertEqual({}, owner.windows_records([self.repo], 2000))

    def test_linux_redis_requires_recorded_identity_and_confirmed_nondefault_port(self):
        for arguments, recorded, start, expected in (
            (["redis-server", "*:18380"], {}, 12345, False),
            (["redis-server", "*:18380"], {901: 99999}, 12345, False),
            (["redis-server", "*:18380"], {901: 12345}, 12345, True),
            (["redis-server", "--port", "18380"], {901: 12345}, 12345, True),
            (["redis-server", "*:6379"], {901: 12345}, 12345, False),
            (["redis-server", "--port", "6379"], {901: 12345}, 12345, False),
            (["redis-server", "redis.conf"], {901: 12345}, 12345, False),
            (["redis-server", "--port", "invalid"], {901: 12345}, 12345, False),
        ):
            with self.subTest(arguments=arguments, recorded=recorded, start=start), \
                 patch.object(Path, "iterdir", return_value=iter([Path("/proc/901")])), \
                 patch.object(linux, "linux_records", return_value=recorded), \
                 patch.object(linux, "snapshot", return_value=("/usr/bin/redis-server", arguments,
                                                               str(self.marked), start)):
                self.assertEqual(expected, 901 in linux.targets([self.repo], 2000))

    def test_linux_redis_uses_persisted_record(self):
        with patch.object(owner, "os", SimpleNamespace(name="posix")), \
             patch.object(owner, "linux_start", return_value=12345):
            owner.record_process(self.marked, Mock(pid=901))
        with patch.object(owner.tempfile, "gettempdir", return_value=str(self.root)), \
             patch.object(Path, "iterdir", return_value=iter([Path("/proc/901")])), \
             patch.object(linux, "snapshot", return_value=("/usr/bin/redis-server", ["redis-server", "*:18380"],
                                                           str(self.marked), 12345)):
            self.assertIn(901, linux.targets([self.repo], 2000))

    def test_linux_registered_permission_failure_is_reported(self):
        for start in (12345, PermissionError("stat denied")):
            with self.subTest(start=start), \
                 patch.object(Path, "iterdir", return_value=iter([Path("/proc/901")])), \
                 patch.object(linux, "linux_records", return_value={901: 12345}), \
                 patch.object(linux, "snapshot", side_effect=PermissionError("exe denied")), \
                 patch.object(linux, "linux_start", **({"side_effect": start} if isinstance(start, Exception)
                                                       else {"return_value": start})):
                with self.assertRaisesRegex(RuntimeError, "WSL test PID 901"):
                    linux.targets([self.repo], 2000)
        self.kill.assert_not_called()

    def test_linux_registered_reused_pid_permission_failure_is_preserved(self):
        with patch.object(Path, "iterdir", return_value=iter([Path("/proc/901")])), \
             patch.object(linux, "linux_records", return_value={901: 12345}), \
             patch.object(linux, "snapshot", side_effect=PermissionError("exe denied")), \
             patch.object(linux, "linux_start", return_value=99999):
            self.assertEqual({}, linux.targets([self.repo], 2000))

    def test_linux_unregistered_permission_failure_uses_command_ownership(self):
        commands = [(str(self.repo / "bin/ipcclient"), True),
                    ("/usr/bin/python3\0" + str(self.repo / "tools/redis_fixture.py"), True),
                    ("/usr/bin/python3\0/other/repo/redis_fixture.py", False)]
        for command, owned in commands:
            with self.subTest(command=command), \
                 patch.object(Path, "iterdir", return_value=iter([Path("/proc/901")])), \
                 patch.object(linux, "linux_records", return_value={}), \
                 patch.object(linux, "snapshot", side_effect=PermissionError("exe denied")), \
                 patch.object(Path, "read_bytes", return_value=(command + "\0").encode()):
                if owned:
                    with self.assertRaisesRegex(RuntimeError, "repository WSL test PID 901"):
                        linux.targets([self.repo], 2000)
                else:
                    self.assertEqual({}, linux.targets([self.repo], 2000))

    def test_linux_pid_reuse_before_kill_is_preserved(self):
        original = ("/owned/ipcclient", [], "/owned", "100")
        reused = ("/foreign/app", [], "/foreign", "200")
        with patch.object(sys, "argv", ["cleanup", "--root", str(self.repo)]), \
             patch.object(linux, "targets", side_effect=[{901: original}, {}]), \
             patch.object(linux, "snapshot", return_value=reused), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, linux.main())
        self.kill.assert_not_called()

    def test_windows_pid_reuse_between_selection_and_kill_is_preserved(self):
        # The selection observes birth 100; the process is replaced before kill.
        process = self.process(901, "/usr/bin/redis-server")
        with patch.object(sys, "argv", ["cleanup", "--lane", "windows-mt", "--windows-root", str(self.lanes)]), \
             patch.object(windows, "windows_processes", side_effect=[[process], []]), \
             patch.object(windows, "windows_records", return_value={901: 100}), \
             patch.object(windows, "windows_birth", return_value=100), \
             patch.object(windows, "terminate_verified", return_value=False) as terminate, \
             patch("os.kill") as kill, contextlib.redirect_stdout(io.StringIO()):
            windows.main()
        kill.assert_not_called()
        terminate.assert_called_once_with(901, 100)

    def test_windows_termination_uses_verified_handle(self):
        for actual_birth in (100, 200):
            with self.subTest(birth=actual_birth):
                kernel = Mock()
                kernel.OpenProcess.return_value = 123

                def times(handle, creation, *unused):
                    creation._obj.value = actual_birth
                    return 1

                kernel.GetProcessTimes.side_effect = times
                kernel.TerminateProcess.return_value = 1
                with patch.object(owner.ctypes, "WinDLL", return_value=kernel, create=True):
                    self.assertEqual(actual_birth == 100, owner.terminate_verified(901, 100))
                kernel.OpenProcess.assert_called_once_with(0x1001, False, 901)
                kernel.CloseHandle.assert_called_once_with(123)
                if actual_birth == 100:
                    kernel.TerminateProcess.assert_called_once_with(123, 1)
                else:
                    kernel.TerminateProcess.assert_not_called()

    def test_wsl_nonzero_is_not_success(self):
        with patch.object(windows.subprocess, "check_output", return_value="/mnt/e/repo\n"), \
             patch.object(windows.subprocess, "run", return_value=subprocess.CompletedProcess([], 2, "", "WSL broken")):
            with self.assertRaisesRegex(RuntimeError, "WSL broken"):
                windows.wsl_cleanup("linux-mt", 2000, "root", "/tmp/lanes", False)

    def test_wsl_failure_propagates_to_exit_status(self):
        with patch.object(sys, "argv", ["cleanup", "--lane", "linux-mt", "--dry-run"]), \
             patch.object(windows, "windows_targets", return_value=[]), \
             patch.object(windows, "wsl_cleanup", side_effect=RuntimeError("WSL broken")), \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(1, windows.main())

    def test_both_platforms_forward_offset(self):
        with patch.object(sys, "argv", ["matrix", "--platform", "both", "--port-offset", "2345",
                                       "--artifacts", str(self.root / "results")]), \
             patch.object(matrix, "WINDOWS", True), \
             patch.object(matrix.subprocess, "check_output", return_value="/tmp/translated\n"), \
             patch.object(matrix.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
            self.assertEqual(0, matrix.main())
        self.assertEqual(2, run.call_count)
        for call in run.call_args_list:
            command = call.args[0]
            self.assertEqual("2345", command[command.index("--port-offset") + 1])

    def test_fixture_child_starts_in_marked_directory(self):
        source = self.marked / "redis.conf"
        source.write_text('dir .\nport 18380\n', encoding="utf-8")
        proc = Mock(pid=901)
        proc.wait.return_value = 0
        proc.poll.return_value = 0
        observed = []

        def launch(command, **kwargs):
            cwd = Path(kwargs.get("cwd", Path.cwd()))
            observed.append(bool(owner.read_owner(cwd, [self.repo], 2000)))
            return proc

        # This helper is WSL-only; supply POSIX spelling on Windows too.
        temporary_directory = tempfile.TemporaryDirectory

        @contextlib.contextmanager
        def posix_temp(**kwargs):
            with temporary_directory(**kwargs) as directory:
                yield Path(directory).as_posix()

        with patch.object(sys, "argv", ["fixture", "fake-redis", str(source)]), \
             patch("tempfile.TemporaryDirectory", side_effect=posix_temp), \
             patch.object(owner, "record_process"), \
             patch("subprocess.Popen", side_effect=launch):
            with self.assertRaises(SystemExit) as result:
                runpy.run_path(str(Path(__file__).with_name("redis_fixture.py")), run_name="__main__")
        self.assertEqual(0, result.exception.code)
        self.assertEqual([True], observed)
        self.assertFalse(Path(str(source) + ".pid").exists())

    def test_fixture_pidfile_write_failure_reaps_child(self):
        source = self.marked / "redis.conf"
        source.write_text('dir .\nport 18380\n', encoding="utf-8")
        proc = Mock(pid=901)
        proc.poll.return_value = None
        original_write = Path.write_text
        temporary_directory = tempfile.TemporaryDirectory

        @contextlib.contextmanager
        def posix_temp(**kwargs):
            with temporary_directory(**kwargs) as directory:
                yield Path(directory).as_posix()

        def fail_pidfile(path, *args, **kwargs):
            if str(path).endswith(".pid"):
                raise PermissionError("pidfile denied")
            return original_write(path, *args, **kwargs)

        with patch.object(sys, "argv", ["fixture", "fake-redis", str(source)]), \
             patch("tempfile.TemporaryDirectory", side_effect=posix_temp), \
             patch.object(owner, "record_process"), \
             patch("subprocess.Popen", return_value=proc), \
             patch.object(Path, "write_text", fail_pidfile):
            with self.assertRaisesRegex(PermissionError, "pidfile denied"):
                runpy.run_path(str(Path(__file__).with_name("redis_fixture.py")), run_name="__main__")
        proc.terminate.assert_called_once_with()
        proc.wait.assert_called_once_with(timeout=3)
        self.assertFalse(Path(str(source) + ".pid").exists())

    def test_probe_occupied_port_startup_failure_never_connects(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            port = listener.getsockname()[1]
            # Real bind collision, with a fake child returned only after it fails.
            proc = Mock(pid=901)
            proc.poll.return_value = 1

            def failed_launch(command, **kwargs):
                with socket.socket() as collision:
                    with self.assertRaises(OSError):
                        collision.bind(("127.0.0.1", port))
                kwargs["stdout"].write("Address already in use\n")
                kwargs["stdout"].flush()
                return proc

            with patch.object(sys, "argv", ["probe", "--native", "fake-redis", "--port", str(port)]), \
                 patch.object(probe, "launch", side_effect=failed_launch), \
                 patch.object(probe, "record_process"), \
                 patch.object(socket, "create_connection", side_effect=AssertionError("must not connect")) as connect:
                with self.assertRaisesRegex(RuntimeError, "startup failed"):
                    probe.main()
            connect.assert_not_called()
            proc.terminate.assert_not_called()
            proc.kill.assert_not_called()
            listener.settimeout(.05)
            with self.assertRaises(socket.timeout):
                listener.accept()


class RealProbeSafetyTests(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("OI_SAFETY_REDIS_BINARY"), "set OI_SAFETY_REDIS_BINARY for real collision test")
    def test_real_redis_occupied_port_no_connection_or_shutdown(self):
        binary = os.environ["OI_SAFETY_REDIS_BINARY"]
        self.assertTrue(Path(binary).is_file(), binary)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            port = listener.getsockname()[1]
            with patch.object(sys, "argv", ["probe", "--native", binary, "--port", str(port)]), \
                 patch.object(socket, "create_connection", side_effect=AssertionError("must not connect")) as connect:
                with self.assertRaisesRegex(RuntimeError, "startup failed"):
                    probe.main()
            connect.assert_not_called()
            listener.settimeout(.05)
            with self.assertRaises(socket.timeout):
                listener.accept()


class RegressionCleanupTests(unittest.TestCase):
    def setUp(self):
        from regression import procs, runner, web
        self.procs = procs
        self.runner = runner
        self.web = web
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.args = SimpleNamespace(artifacts=self.root / "saved", redis_wsl=False)
        self.old = Mock(args=["old-owned"])
        self.first = Mock(args=["first-owned"])
        for proc in (self.old, self.first):
            proc.poll.return_value = None
            proc.returncode = 0
        self.handles = [Mock(), Mock()]
        (self.root / "process.out").write_text("test evidence")
        self.stop = patch.object(self.procs, "stop", side_effect=[subprocess.CalledProcessError(1, "helper"), None])
        self.stopped = self.stop.start()
        self.addCleanup(self.stop.stop)
        self.stderr = contextlib.redirect_stderr(io.StringIO())
        self.stderr.__enter__()
        self.addCleanup(self.stderr.__exit__, None, None, None)

    def cleanup(self, **kwargs):
        self.procs.cleanup_owned([self.old, self.first], self.handles, self.root, self.args, "echo", **kwargs)

    def assert_all_attempted(self):
        self.assertEqual([self.first, self.old], [c.args[0] for c in self.stopped.call_args_list])
        self.first.kill.assert_called_once_with()
        self.old.wait.assert_called_once_with(timeout=3)
        self.old.kill.assert_not_called()
        for handle in self.handles:
            handle.close.assert_called_once_with()
        self.assertEqual("test evidence", (self.args.artifacts / "echo/process.out").read_text())

    def test_review_first_helper_failure_cleans_remaining_processes(self):
        with self.assertRaisesRegex(RuntimeError, "graceful shutdown"):
            self.cleanup()
        self.assert_all_attempted()

    def test_service_nonzero_exit_is_reported(self):
        self.stopped.side_effect = None
        self.old.returncode = 7
        with self.assertRaisesRegex(RuntimeError, "exited code=7"):
            self.cleanup()
        self.first.kill.assert_not_called()

    def test_kill_failure_still_reaps_and_cleans_resources(self):
        self.first.kill.side_effect = OSError("kill denied")
        with self.assertRaisesRegex(RuntimeError, "kill denied"):
            self.cleanup()
        self.first.wait.assert_called_once_with(timeout=3)
        self.assert_all_attempted()

    def test_interrupt_is_reraised_after_remaining_cleanup(self):
        self.stopped.side_effect = [KeyboardInterrupt(), None]
        with self.assertRaises(KeyboardInterrupt):
            self.cleanup()
        self.assert_all_attempted()

    def test_timeout_force_kill_is_not_graceful_success(self):
        self.stopped.side_effect = None
        self.first.wait.side_effect = [subprocess.TimeoutExpired("owned", 3), 0]
        with self.assertRaisesRegex(RuntimeError, "TimeoutExpired"):
            self.cleanup()
        self.assert_all_attempted()

    def test_all_resource_errors_are_attempted_and_original_is_preserved(self):
        monitor, fixture, thread = Mock(), Mock(), Mock()
        monitor.close.side_effect = OSError("monitor")
        fixture.shutdown.side_effect = OSError("HTTP shutdown")
        fixture.server_close.side_effect = OSError("HTTP close")
        thread.is_alive.return_value = False
        self.handles[0].close.side_effect = OSError("handle")
        primary = ValueError("original test error")
        with self.assertRaises(ValueError) as caught:
            try:
                raise primary
            finally:
                self.cleanup(monitor=monitor, http_fixture=fixture, http_thread=thread, primary=sys.exc_info()[1])
        self.assertIs(primary, caught.exception)
        fixture.server_close.assert_called_once_with()
        thread.join.assert_called_once_with(timeout=3)
        self.assert_all_attempted()

    def test_artifact_copy_failure_does_not_skip_other_outputs(self):
        (self.root / "second.out").write_text("second")
        self.stopped.side_effect = None
        with patch.object(self.procs.shutil, "copy2", side_effect=[OSError("copy denied"), None]) as copy:
            with self.assertRaisesRegex(RuntimeError, "copy denied"):
                self.cleanup()
        self.assertEqual(2, copy.call_count)

    def test_resume_failure_does_not_skip_processes(self):
        with patch.object(self.procs, "pause", side_effect=OSError("resume failed")):
            with self.assertRaisesRegex(RuntimeError, "resume failed"):
                self.cleanup(db=Mock(poll=Mock(return_value=None)), db_paused=True)
        # Exercise the ordinary stop branch for the fixture on either host.
        self.first.kill.assert_called_once_with()
        self.handles[1].close.assert_called_once_with()

    def test_registry_resume_failure_cleans_other_processes_and_reports_error(self):
        # The paused registry is an owned child, not an unrelated process.
        output = io.StringIO()
        with patch.object(self.procs, "pause", side_effect=OSError("registry resume denied")) as resume, \
             contextlib.redirect_stderr(output):
            with self.assertRaisesRegex(RuntimeError, "resume registry.*registry resume denied") as caught:
                self.cleanup(registry=self.first, registry_paused=True)
        resume.assert_called_once_with(self.first, False)
        self.assertIn("graceful shutdown", str(caught.exception))
        self.assertIn("resume registry", output.getvalue())
        self.assertIn("registry resume denied", output.getvalue())
        self.first.wait.assert_called_once_with(timeout=3)
        self.assert_all_attempted()

    def test_normal_finally_calls_resilient_cleanup(self):
        # Execute the actual finally AST, as in the review reproduction, with
        # two owned processes and no application launch or network activity.
        import ast
        import inspect
        tree = ast.parse(inspect.getsource(self.runner.run))
        final = next(node.finalbody for node in ast.walk(tree) if isinstance(node, ast.Try) and
                     any(isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id == "cleanup_owned"
                         for statement in node.finalbody for n in ast.walk(statement)))
        ctx = SimpleNamespace(procs=[self.old, self.first], handles=self.handles,
                              root=self.root, args=self.args, name="echo", db=None, db_paused=False,
                              monitor=None, http_fixture=None, http_thread=None,
                              timeline_thread=None, timeline_stop=None,
                              timeline_lines=[], timeline_errors=[],
                              registry=None, registry_paused=False)
        scope = dict(self.runner.__dict__, ctx=ctx)
        with self.assertRaisesRegex(RuntimeError, "graceful shutdown"):
            exec(compile(ast.Module(body=final, type_ignores=[]), "normal-finally", "exec"), scope)
        self.assert_all_attempted()

    def test_web_helper_failure_preserves_client_error_and_artifacts(self):
        self.args.bin_dir = self.root
        self.stopped.side_effect = subprocess.CalledProcessError(1, "helper")
        server = self.first
        original_copy = self.procs.shutil.copy2
        def copy(source, destination):
            if Path(source).suffix == ".out":
                return original_copy(source, destination)
        client = subprocess.CompletedProcess([], 1, "bad=1", "original client failure")
        with patch.object(self.web, "check_web_bind_restart"), \
             patch.object(self.web, "check_web_small_send_queue"), \
             patch.object(self.procs.shutil, "copy2", side_effect=copy), \
             patch.object(self.web, "launch", return_value=server), \
             patch.object(self.web, "record_process"), \
             patch.object(self.web.socket, "create_connection"), \
             patch.object(self.web.subprocess, "run", return_value=client), \
             contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(AssertionError, "original client failure"):
                self.web.run_web(self.args)
        server.kill.assert_called_once_with()
        self.assertTrue((self.args.artifacts / "web/server.out").exists())

    def test_web_success_still_fails_if_stop_helper_fails(self):
        self.args.bin_dir = self.root
        self.stopped.side_effect = subprocess.TimeoutExpired("helper", 5)
        client = subprocess.CompletedProcess([], 0, "rpc=1 bin=1 bad=0", "")
        fixture = Mock()
        fixture.server_address = ("::1", 12345)
        fixture.calls = 0
        fixture.__enter__ = Mock(return_value=fixture)
        fixture.__exit__ = Mock(return_value=False)
        def run_client(*args, **kwargs):
            if hasattr(fixture, "hosts") and isinstance(fixture.hosts, list):
                if fixture.calls >= 3:
                    fixture.first_request_at = self.web.time.monotonic()
                fixture.hosts.extend([("localhost" if fixture.calls >= 2 else "[::1]") + ":12345"] * 9)
                reuse = fixture.calls != 1
                fixture.connection_headers.extend(["keep-alive" if reuse else "close"] * 9)
                fixture.connections.extend(([object()] * 9) if reuse else [object() for _ in range(9)])
                fixture.calls += 1
            return client
        with patch.multiple(self.web, check_web_bind_restart=Mock(), check_web_small_send_queue=Mock()), \
             patch.object(self.procs.shutil, "copy2"), \
             patch.object(self.web, "launch", return_value=self.first), \
             patch.object(self.web, "record_process"), \
             patch.object(self.web.socket, "create_connection"), \
             patch.object(self.web.socket, "getaddrinfo", return_value=[(self.web.socket.AF_INET, None, None, None, None), (self.web.socket.AF_INET6, None, None, None, None)]), \
             patch.object(self.web, "IPv6HttpServer", return_value=fixture), \
             patch.object(self.web.http.server, "ThreadingHTTPServer", return_value=fixture), \
             patch.object(self.web, "stream_response_fixture", return_value=contextlib.nullcontext(12345)), \
             patch.multiple(self.web, check_http_pool=Mock(), check_http_disconnect=Mock(),
                            check_http_bodyless=Mock(), check_url_client=Mock(), check_https=Mock(),
                            check_web_transport=Mock(), check_web_json=Mock(), check_web_stream_length=Mock()), \
             patch.object(self.web.threading, "Thread") as thread, \
             patch.object(self.web.subprocess, "run", side_effect=run_client), \
             contextlib.redirect_stdout(io.StringIO()):
            thread.return_value.is_alive.return_value = False
            with self.assertRaisesRegex(RuntimeError, "TimeoutExpired"):
                self.web.run_web(self.args)
        self.first.kill.assert_called_once_with()
        self.first.wait.assert_called_once_with(timeout=3)

    def test_failure_diagnostic_read_does_not_mask_original(self):
        import ast
        import inspect
        tree = ast.parse(inspect.getsource(self.runner.run))
        handler = next(node for node in ast.walk(tree) if isinstance(node, ast.ExceptHandler) and
                       isinstance(node.type, ast.Name) and node.type.id == "BaseException")
        ctx = SimpleNamespace(root=self.root)
        scope = dict(self.runner.__dict__, ctx=ctx)
        original = ValueError("original failure")
        with patch.object(Path, "read_text", side_effect=OSError("cannot read output")):
            with self.assertRaises(ValueError) as caught:
                try:
                    raise original
                except ValueError:
                    exec(compile(ast.Module(body=handler.body, type_ignores=[]), "failure-output", "exec"), scope)
        self.assertIs(original, caught.exception)

    def test_temporary_directory_failure_preserves_original(self):
        directory = Mock(name="directory")
        directory.cleanup.side_effect = OSError("directory busy")
        with patch.object(self.procs.tempfile, "TemporaryDirectory", return_value=directory):
            with self.assertRaisesRegex(ValueError, "test failure"):
                with self.procs.regression_directory():
                    raise ValueError("test failure")
        directory.cleanup.assert_called_once_with()

    def test_cleanup_order_is_recorded_and_recorder_failure_is_not_the_result(self):
        owner.write_owner(self.root, self.root, 0)
        with self.assertRaisesRegex(RuntimeError, "graceful shutdown"):
            self.cleanup()
        data = json.loads((self.root / owner.MARKER).read_text(encoding="utf-8"))
        self.assertEqual([(step["label"], step["ok"]) for step in data["cleanup"]], [
            ("graceful shutdown ['first-owned']", False),
            ("force shutdown ['first-owned']", True),
            ("reap ['first-owned']", True),
            ("graceful shutdown ['old-owned']", True),
            ("output close", True),
            ("output close", True),
            ("artifact directory", True),
            ("artifact process.out", True),
            ("artifact enumeration", True),
        ])
        self.assertIn("CalledProcessError", data["cleanup"][0]["error"])
        self.first.kill.assert_called_once_with()
        self.stopped.side_effect = [subprocess.CalledProcessError(1, "helper"), None]
        with patch.object(self.procs, "note_cleanup_step", side_effect=RuntimeError("marker")):
            with self.assertRaisesRegex(RuntimeError, "graceful shutdown"):
                self.cleanup()
        self.assertEqual(self.first.kill.call_count, 2)


@unittest.skipUnless(os.name == "nt", "delete denial snapshot uses Windows file locks")
class DeleteDenialTests(unittest.TestCase):
    def test_locked_file_records_holder_and_still_raises(self):
        import ctypes
        from ctypes import wintypes
        from regression import procs

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        held = {}

        def release():
            handle = held.get("handle")
            if handle is not None and handle != wintypes.HANDLE(-1).value:
                kernel.CloseHandle(handle)
            root = held.get("root")
            if root is not None:
                shutil.rmtree(root, ignore_errors=True)
            record = held.get("record")
            if record is not None and record.is_file():
                record.unlink()

        self.addCleanup(release)
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            with self.assertRaises(PermissionError) as caught:
                with procs.regression_directory(prefix="oi-denial-") as name:
                    root = Path(name)
                    held["root"] = root
                    held["record"] = root.parent / (root.name + ".delete-denial.json")
                    owner.write_owner(root, root, 0)
                    owner.note_cleanup_step(root, "graceful shutdown", None)
                    target = root / "bin" / "ipcclient.exe"
                    target.parent.mkdir()
                    target.write_bytes(b"exe")
                    handle = kernel.CreateFileW(str(target), 0x80000000, 0, None, 3, 0, None)
                    self.assertNotEqual(handle, wintypes.HANDLE(-1).value)
                    held["handle"] = handle
        self.assertIn(caught.exception.winerror, (5, 32))
        text = stderr.getvalue()
        self.assertIn("delete denial ", text)
        records = json.loads(held["record"].read_text(encoding="utf-8"))
        self.assertGreaterEqual(len(records), 1)
        matched = [item for item in records if item["winerror"] in (5, 32) and item["path"].endswith("ipcclient.exe")]
        self.assertTrue(matched, records)
        record = matched[0]
        self.assertEqual(record["cleanup"], [{"label": "graceful shutdown", "ok": True, "error": None}])
        holders = [item for item in record["holders"]["holders"] if item["pid"] == os.getpid()]
        self.assertEqual(len(holders), 1, record["holders"])
        self.assertEqual(holders[0]["birth"], owner.windows_birth(os.getpid()))
        self.assertFalse(record["holders"]["exclusive_open"]["opened"])
        self.assertEqual(record["owner_processes"], [])

    def test_capture_failure_does_not_replace_delete_error(self):
        import ctypes
        from ctypes import wintypes
        from regression import procs

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        held = {}

        def release():
            handle = held.get("handle")
            if handle is not None and handle != wintypes.HANDLE(-1).value:
                kernel.CloseHandle(handle)
            root = held.get("root")
            if root is not None:
                shutil.rmtree(root, ignore_errors=True)

        self.addCleanup(release)
        stderr = io.StringIO()
        with patch.object(procs, "delete_denial_record", side_effect=RuntimeError("capture boom")):
            with contextlib.redirect_stderr(stderr):
                with self.assertRaises(PermissionError) as caught:
                    with procs.regression_directory(prefix="oi-denial-") as name:
                        root = Path(name)
                        held["root"] = root
                        target = root / "ipcclient.exe"
                        target.write_bytes(b"exe")
                        handle = kernel.CreateFileW(str(target), 0x80000000, 0, None, 3, 0, None)
                        self.assertNotEqual(handle, wintypes.HANDLE(-1).value)
                        held["handle"] = handle
        self.assertIn(caught.exception.winerror, (5, 32))
        self.assertIn("capture boom", stderr.getvalue())
        self.assertNotIn("capture boom", str(caught.exception))


if __name__ == "__main__":
    unittest.main(verbosity=2)
