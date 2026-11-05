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
                self.assertEqual(expected, linux.matches("/usr/bin/redis-server", ["redis-server", "*:18380"],
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
