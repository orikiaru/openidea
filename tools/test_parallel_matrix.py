"""parallel_matrix 的本地单测：同一路副本已有进程在用时拒绝启动。"""
import subprocess
import sys
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


if __name__ == "__main__":
    unittest.main()
