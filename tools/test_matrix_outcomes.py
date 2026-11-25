"""Mock-only matrix outcome checks: no compiler or application is launched."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import test_matrix as matrix


class MatrixOutcomeTests(unittest.TestCase):
    def exercise(self, failures=None, raised=None, metadata_error=None, extra_args=(), windows=None):
        failures = failures or {}
        windows = matrix.WINDOWS if windows is None else windows
        self.commands = {}
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            (output / "summary.json").write_text('{"passed": true}')
            (output / "matrix.json").write_text('[{"step": "stale"}]')
            calls = []

            def run(command, **kwargs):
                step = Path(kwargs["stdout"].name).stem
                calls.append(step)
                self.commands[step] = [str(part) for part in command]
                result = failures.get(step, 0)
                if isinstance(result, BaseException):
                    raise result
                return subprocess.CompletedProcess(command, result)

            def metadata(command, **kwargs):
                if metadata_error:
                    raise metadata_error
                return "mock\n" if kwargs.get("text") else b"diff"

            with patch.object(sys, "argv", ["matrix", "--artifacts", str(output), *extra_args]), \
                 patch.object(matrix, "WINDOWS", windows), \
                 patch.object(matrix, "find_cmake", return_value="cmake"), \
                 patch.object(matrix, "missing_toolset", return_value=""), \
                 patch.object(matrix.subprocess, "check_output", side_effect=metadata), \
                 patch.object(matrix.subprocess, "run", side_effect=run), \
                 contextlib.redirect_stdout(io.StringIO()):
                if raised:
                    with self.assertRaises(raised):
                        matrix.main()
                    code = None
                else:
                    code = matrix.main()
            report = json.loads((output / "summary.json").read_text())
            self.assertEqual(not failures and not metadata_error, report["passed"])
            return code, report, calls

    def test_complete_uses_separate_build_dirs_and_does_not_restore(self):
        code, report, calls = self.exercise()
        self.assertEqual(0, code)
        self.assertTrue(report["completed"] and report["restore_skipped"])
        self.assertFalse(report["restored"])
        self.assertIn("mt-tests", calls)
        self.assertIn("st-tests", calls)
        self.assertNotIn("restore-configure", calls)
        self.assertNotIn("restore-build", calls)
        mt_dir = self.commands["mt-configure"][self.commands["mt-configure"].index("-B") + 1]
        st_dir = self.commands["st-configure"][self.commands["st-configure"].index("-B") + 1]
        self.assertTrue(st_dir.endswith("-st"))
        self.assertNotEqual(mt_dir, st_dir)
        self.assertEqual(st_dir, mt_dir + "-st")

    def test_single_thread_mode_keeps_its_own_directory(self):
        code, report, calls = self.exercise(extra_args=("--mode", "st", "--no-restore"), windows=False)
        self.assertEqual(0, code)
        self.assertTrue(report["completed"] and report["restore_skipped"])
        self.assertFalse(report["restored"])
        self.assertIn("st-tests", calls)
        self.assertNotIn("mt-tests", calls)
        self.assertNotIn("restore-configure", calls)
        st_dir = self.commands["st-configure"][self.commands["st-configure"].index("-B") + 1]
        self.assertTrue(st_dir.endswith("-st"))
        _, report, _ = self.exercise({"st-tests": 1}, extra_args=("--mode", "st"), windows=True)
        self.assertFalse(report["passed"])

    def test_no_restore_flag_is_ignored(self):
        for extra_args, windows in ((("--mode", "st", "--no-restore"), True),
                                    (("--mode", "mt", "--no-restore"), False),
                                    (("--no-restore",), False)):
            with self.subTest(extra_args=extra_args, windows=windows):
                code, report, calls = self.exercise(extra_args=extra_args, windows=windows)
                self.assertEqual(0, code)
                self.assertTrue(report["restore_skipped"])
                self.assertNotIn("restore-configure", calls)

    def test_review_first_test_interrupt_never_passes(self):
        _, report, calls = self.exercise({"mt-tests": KeyboardInterrupt()}, KeyboardInterrupt)
        self.assertFalse(report["completed"])
        self.assertFalse(report["restored"])
        self.assertNotIn("st-tests", calls)
        self.assertNotIn("restore-configure", calls)

    def test_interrupt_during_configure_does_not_restore(self):
        _, report, calls = self.exercise({"mt-configure": KeyboardInterrupt()}, KeyboardInterrupt)
        self.assertFalse(report["restored"])
        self.assertNotIn("mt-build", calls)
        self.assertNotIn("restore-configure", calls)

    def test_interrupt_before_configure_is_not_restored(self):
        _, report, calls = self.exercise({"field-names": KeyboardInterrupt()}, KeyboardInterrupt)
        self.assertFalse(report["restored"])
        self.assertNotIn("restore-configure", calls)

    def test_nonzero_steps_cannot_pass(self):
        for step in ("field-names", "inheritance-tests", "inheritance", "agent-route-tests", "agent-route", "process-fixtures", "test-safety", "matrix-outcomes",
                     "regression-checks", "build-tests", "linux-sync-tests", "config-macros",
                     "mt-configure", "mt-build", "mt-tests", "st-tests"):
            with self.subTest(step=step):
                code, _, _ = self.exercise({step: 1})
                self.assertEqual(1, code)

    def test_config_macro_probe_only_runs_for_all(self):
        _, _, calls = self.exercise(extra_args=("--case", "echo"))
        self.assertNotIn("config-macros", calls)
        self.assertIn("regression-checks", calls)

    def test_original_interrupt_is_not_swallowed(self):
        self.exercise({"mt-tests": KeyboardInterrupt()}, KeyboardInterrupt)

    def test_setup_interrupt_invalidates_old_summary(self):
        self.exercise(raised=KeyboardInterrupt, metadata_error=KeyboardInterrupt())

    def test_mysql_runs_after_build_before_tests(self):
        code, _, calls = self.exercise(extra_args=("--mode", "mt", "--mysqld", "/opt/m/mysqld"), windows=False)
        self.assertEqual(0, code)
        self.assertIn("-DOI_ENABLE_MYSQL=ON", self.commands["mt-configure"])
        self.assertLess(calls.index("mt-build"), calls.index("mt-mysql"))
        self.assertLess(calls.index("mt-mysql"), calls.index("mt-tests"))
        mysql = self.commands["mt-mysql"]
        self.assertTrue(any("run_mysql_regression.py" in part for part in mysql))
        self.assertEqual("/opt/m/mysqld", mysql[mysql.index("--mysqld") + 1])

    def test_mysql_failure_still_runs_tests(self):
        _, report, calls = self.exercise({"mt-mysql": 1}, extra_args=("--mode", "mt", "--mysqld", "/opt/m/mysqld"), windows=False)
        self.assertLess(calls.index("mt-mysql"), calls.index("mt-tests"))
        self.assertFalse(report["passed"])

    def test_define_reaches_each_mode_configure(self):
        _, _, calls = self.exercise(extra_args=("--define", "A=B"), windows=False)
        configure_steps = [step for step in calls if step.endswith("-configure")]
        self.assertEqual(["mt-configure", "st-configure"], configure_steps)
        for step in configure_steps:
            self.assertIn("-DA=B", self.commands[step])

    def test_webclient_thread_mode_tracks_matrix_on_both_platforms(self):
        for windows in (False, True):
            with self.subTest(windows=windows):
                code, _, _ = self.exercise(windows=windows)
                self.assertEqual(0, code)
                for mode, value in (("mt", "OFF"), ("st", "ON")):
                    command = self.commands[mode + "-configure"]
                    self.assertIn("-DOI_IPC_SINGLE_THREAD=" + value, command)
                    self.assertIn("-DOI_WEBCLIENT_SINGLE_THREAD=" + value, command)

    def test_missing_toolset_names_the_gap_and_the_install(self):
        with patch.object(matrix.shutil, "which", return_value=None):
            text = matrix.missing_toolset(False)
        self.assertIn("缺少编译工具集：gcc-13、g++-13", text)
        self.assertIn("sudo apt-get install -y gcc-13 g++-13", text)
        log = "CMake Error: No CMAKE_CXX_COMPILER could be found."
        self.assertIn("gcc-13", matrix.explain_configure_failure(log, False))
        self.assertEqual("", matrix.explain_configure_failure("error: unused variable", False))

    def test_bad_define_and_windows_mysqld_rejected(self):
        with self.subTest(case="define"):
            with self.assertRaises(SystemExit):
                self.exercise(extra_args=("--define", "AB"), windows=False)
        with self.subTest(case="mysqld"):
            with self.assertRaises(SystemExit):
                self.exercise(extra_args=("--platform", "native", "--mysqld", "/opt/m/mysqld"), windows=True)

    def test_asan_uses_selected_build_directory(self):
        for mode in ("mt", "st", "all"):
            with self.subTest(mode=mode):
                self.exercise(extra_args=("--mode", mode, "--asan"), windows=False)
                modes = ("mt", "st") if mode == "all" else (mode,)
                for selected in modes:
                    configured = self.commands[selected + "-configure"]
                    asan = self.commands[selected + "-asan"]
                    self.assertEqual(configured[configured.index("-B") + 1],
                                     asan[asan.index("--build-dir") + 1])
                    self.assertEqual(selected, asan[asan.index("--mode") + 1])

    def test_windows_native_rejects_linux_fault_options(self):
        for option in ("--asan", "--clock-faults"):
            with self.subTest(option=option), self.assertRaises(SystemExit):
                self.exercise(extra_args=(option,), windows=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
