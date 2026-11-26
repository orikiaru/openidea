"""parallel_matrix 的本地单测：同一路副本已有进程在用时拒绝启动。"""
import os
import subprocess
import tempfile
import shutil
import sys
import time
import unittest
from pathlib import Path
from unittest import mock

import parallel_matrix as matrix


class LaneBusyTests(unittest.TestCase):
    def test_windows_match_is_case_and_separator_insensitive(self):
        processes = [
            {"ProcessId": 11, "CommandLine": r"python F:\OI-PAR\windows-mt\tools\test_matrix.py --mode mt"},
            {"ProcessId": 12, "CommandLine": r"python F:\oi-par\windows-mt-old\tools\test_matrix.py"},
            {"ProcessId": 13, "CommandLine": r"python F:\oi-par\windows-st\tools\test_matrix.py"},
            {"ProcessId": 14, "CommandLine": None},
        ]
        users = matrix.windows_lane_users(processes, Path("F:/oi-par/windows-mt"), 99)
        self.assertEqual(len(users), 1)
        self.assertTrue(users[0].startswith("11 "))

    def test_windows_skips_own_process(self):
        processes = [{"ProcessId": 7, "CommandLine": r"python F:\oi-par\windows-mt\tools\x.py"}]
        self.assertEqual(matrix.windows_lane_users(processes, Path("F:/oi-par/windows-mt"), 7), [])

    def test_linux_pgrep_codes(self):
        idle = subprocess.CompletedProcess([], 1, "", "")
        busy = subprocess.CompletedProcess([], 0, "42 python3 /root/oi-par/linux-mt/tools/test_matrix.py\n", "")
        broken = subprocess.CompletedProcess([], 2, "", "pgrep: bad option")
        with mock.patch.object(matrix, "wsl", return_value=idle) as call:
            self.assertEqual(matrix.lane_users("linux", "/root/oi-par/linux-mt", "root"), [])
            self.assertEqual(call.call_args[0][1][-1], "/root/oi-par/linux-mt/")
        with mock.patch.object(matrix, "wsl", return_value=busy):
            self.assertEqual(len(matrix.lane_users("linux", "/root/oi-par/linux-mt", "root")), 1)
        with mock.patch.object(matrix, "wsl", return_value=broken):
            with self.assertRaises(RuntimeError):
                matrix.lane_users("linux", "/root/oi-par/linux-mt", "root")

    def test_busy_lane_refuses_before_sync(self):
        def users(platform, copy, user):
            return ["5 python3 %s/tools/test_matrix.py" % copy] if str(copy).endswith("linux-st") else []

        argv = ["parallel_matrix.py", "--windows-redis-binary", "redis-server.exe", "--artifacts", "unused",
                "--lane", "linux-mt", "--lane", "linux-st"]
        with mock.patch.object(sys, "argv", argv), \
                mock.patch.object(matrix, "lane_users", side_effect=users), \
                mock.patch.object(matrix, "sync_linux") as sync_linux, \
                mock.patch.object(matrix, "sync_windows") as sync_windows, \
                mock.patch.object(matrix.subprocess, "Popen") as popen, \
                mock.patch.object(Path, "mkdir") as mkdir:
            code = matrix.main()
        self.assertEqual(code, 2)
        sync_linux.assert_not_called()
        sync_windows.assert_not_called()
        popen.assert_not_called()
        mkdir.assert_not_called()


class SyncAndBuildTests(unittest.TestCase):
    def test_private_directories_are_excluded_and_windows_overlap_rejected(self):
        self.assertIn(".local", matrix.EXCLUDED_DIRS)
        self.assertIn(".codex", matrix.EXCLUDED_DIRS)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "repo"
            root.mkdir()
            with mock.patch.object(matrix, "ROOT", root), mock.patch.object(matrix.subprocess, "run") as run:
                for target in (root, root / "lane", root.parent):
                    with self.subTest(target=target), self.assertRaises(ValueError):
                        matrix.sync_windows(target)
                run.assert_not_called()

    def test_linux_overlap_rejected_before_sync(self):
        for destination in ("/source", "/source/child", "/"):
            response = subprocess.CompletedProcess([], 0, "/source\n" + destination + "\n")
            with self.subTest(destination=destination), mock.patch.object(matrix, "wsl", return_value=response) as wsl, \
                    mock.patch.object(matrix, "linux_path", return_value="/source"):
                with self.assertRaises(ValueError):
                    matrix.sync_linux("root", destination)
                self.assertEqual(wsl.call_count, 1)

    def test_linux_uses_checksum_and_delete(self):
        with mock.patch.object(matrix, "wsl", return_value=subprocess.CompletedProcess([], 0, "/source\n/tmp/linux-mt\n")), \
                mock.patch.object(matrix, "linux_path", return_value="/source"), \
                mock.patch.object(matrix, "git_dir", return_value=None):
            matrix.sync_linux("root", "/tmp/linux-mt")
            commands = [call.args[1] for call in matrix.wsl.call_args_list]
        rsync = next(command for command in commands if command[0] == "rsync")
        self.assertIn("--checksum", rsync)
        self.assertIn("--delete", rsync)

    @unittest.skipUnless(shutil.which("robocopy"), "requires Windows robocopy")
    def test_windows_sync_replaces_equal_and_older_timestamps_and_deletes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "source"
            dest = Path(temp) / "windows-mt"
            root.mkdir()
            dest.mkdir()
            (root / "same.cpp").write_bytes(b"new")
            time.sleep(1.1)  # Give NTFS change times distinct seconds.
            (dest / "same.cpp").write_bytes(b"old")
            timestamp = 1600000000
            os.utime(root / "same.cpp", (timestamp, timestamp))
            time.sleep(1.1)
            os.utime(dest / "same.cpp", (timestamp, timestamp))
            (root / "older.cpp").write_bytes(b"new")
            (dest / "older.cpp").write_bytes(b"old")
            os.utime(root / "older.cpp", (timestamp, timestamp))
            os.utime(dest / "older.cpp", (timestamp + 100, timestamp + 100))
            (root / "added.cpp").write_bytes(b"added")
            (dest / "removed.cpp").write_bytes(b"removed")
            with mock.patch.object(matrix, "ROOT", root), mock.patch.object(matrix, "git_dir", return_value=None):
                matrix.sync_windows(dest)
            self.assertEqual((dest / "same.cpp").read_bytes(), b"new")
            self.assertEqual((dest / "older.cpp").read_bytes(), b"new")
            self.assertEqual((dest / "added.cpp").read_bytes(), b"added")
            self.assertFalse((dest / "removed.cpp").exists())

    @unittest.skipUnless(shutil.which("wsl"), "requires WSL")
    def test_linux_sync_replaces_equal_and_older_timestamps_and_deletes(self):
        available = subprocess.run(["wsl", "-u", "root", "--exec", "sh", "-c", "command -v rsync"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if available.returncode:
            self.skipTest("WSL root and rsync are required")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "source"
            dest = Path(temp) / "linux-mt"
            root.mkdir()
            dest.mkdir()
            for name, source_time, dest_time in (("same.cpp", 1600000000, 1600000000),
                                                  ("older.cpp", 1600000000, 1600000100)):
                (root / name).write_bytes(b"new")
                (dest / name).write_bytes(b"old")
                os.utime(root / name, (source_time, source_time))
                os.utime(dest / name, (dest_time, dest_time))
            (root / "added.cpp").write_bytes(b"added")
            (dest / "removed.cpp").write_bytes(b"removed")
            with mock.patch.object(matrix, "ROOT", root), mock.patch.object(matrix, "git_dir", return_value=None):
                matrix.sync_linux("root", matrix.linux_path("root", dest))
            self.assertEqual((dest / "same.cpp").read_bytes(), b"new")
            self.assertEqual((dest / "older.cpp").read_bytes(), b"new")
            self.assertEqual((dest / "added.cpp").read_bytes(), b"added")
            self.assertFalse((dest / "removed.cpp").exists())

    def test_each_lane_requests_clean_build_and_forwards_define(self):
        with tempfile.TemporaryDirectory() as temp:
            argv = ["parallel_matrix.py", "--windows-redis-binary", "redis.exe", "--artifacts", temp,
                    "--lane", "windows-mt", "--lane", "linux-st", "--define", "OI_ENABLE_TEST_HOOKS=ON"]
            process = mock.Mock()
            process.wait.return_value = 0
            process.poll.return_value = 0
            with mock.patch.object(sys, "argv", argv), mock.patch.object(matrix, "lane_users", return_value=[]), \
                    mock.patch.object(matrix, "sync_windows"), mock.patch.object(matrix, "sync_linux"), \
                    mock.patch.object(matrix, "linux_path", return_value="/artifacts"), \
                    mock.patch.object(matrix.subprocess, "Popen", return_value=process) as popen:
                self.assertEqual(matrix.main(), 0)
            self.assertEqual(popen.call_count, 2)
            for call in popen.call_args_list:
                command = call.args[0]
                self.assertIn("--clean-build", command)
                self.assertEqual(command[command.index("--define") + 1], "OI_ENABLE_TEST_HOOKS=ON")


if __name__ == "__main__":
    unittest.main()
