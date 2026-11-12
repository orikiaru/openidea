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

    def test_complete_requires_both_modes_and_restore(self):
        code, report, calls = self.exercise()
        self.assertEqual(0, code)
        self.assertTrue(report["completed"] and report["restored"])
        self.assertIn("mt-tests", calls)
        self.assertIn("st-tests", calls)
        self.assertEqual(["restore-configure", "restore-build"], calls[-2:])

    def test_dedicated_single_thread_host_skips_restore(self):
        code, report, calls = self.exercise(extra_args=("--mode", "st", "--no-restore"), windows=False)
        self.assertEqual(0, code)
        self.assertTrue(report["completed"] and report["restore_skipped"])
        self.assertFalse(report["restored"])
        self.assertIn("st-tests", calls)
        self.assertNotIn("mt-tests", calls)
        self.assertNotIn("restore-configure", calls)
        _, report, _ = self.exercise({"st-tests": 1}, extra_args=("--mode", "st", "--no-restore"), windows=False)
        self.assertFalse(report["passed"])

    def test_no_restore_rejected_outside_dedicated_single_thread_host(self):
        for extra_args, windows in ((("--mode", "st", "--no-restore"), True),
                                    (("--mode", "mt", "--no-restore"), False),
                                    (("--no-restore",), False)):
            with self.subTest(extra_args=extra_args, windows=windows):
                with self.assertRaises(SystemExit):
                    self.exercise(extra_args=extra_args, windows=windows)

    def test_review_first_test_interrupt_never_passes(self):
        _, report, calls = self.exercise({"mt-tests": KeyboardInterrupt()}, KeyboardInterrupt)
        self.assertFalse(report["completed"])
        self.assertTrue(report["restored"])
        self.assertNotIn("st-tests", calls)

    def test_interrupt_during_configure_still_restores(self):
        _, report, calls = self.exercise({"mt-configure": KeyboardInterrupt()}, KeyboardInterrupt)
        self.assertTrue(report["restored"])
        self.assertNotIn("mt-build", calls)

    def test_interrupt_before_configure_is_not_restored(self):
        _, report, calls = self.exercise({"field-names": KeyboardInterrupt()}, KeyboardInterrupt)
        self.assertFalse(report["restored"])
        self.assertNotIn("restore-configure", calls)

    def test_restore_exceptions_and_interrupts_invalidate_old_summary(self):
        for step in ("restore-configure", "restore-build"):
            for error in (RuntimeError("restore broken"), KeyboardInterrupt()):
                with self.subTest(step=step, error=type(error).__name__):
                    code, report, _ = self.exercise({step: error}, KeyboardInterrupt if isinstance(error, KeyboardInterrupt) else None)
                    self.assertTrue(report["completed"])
                    self.assertFalse(report["restored"])
                    self.assertIn("restore_error", report)
                    if code is not None:
                        self.assertEqual(1, code)

    def test_nonzero_steps_cannot_pass(self):
        for step in ("field-names", "inheritance-tests", "inheritance", "process-fixtures", "test-safety", "matrix-outcomes",
                     "mt-configure", "mt-build", "mt-tests", "st-tests", "restore-configure", "restore-build"):
            with self.subTest(step=step):
                code, _, _ = self.exercise({step: 1})
                self.assertEqual(1, code)

    def test_original_interrupt_survives_restore_failure(self):
        self.exercise({"mt-tests": KeyboardInterrupt(), "restore-build": RuntimeError("restore broken")}, KeyboardInterrupt)

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

    def test_define_reaches_every_configure_including_restore(self):
        _, _, calls = self.exercise(extra_args=("--define", "A=B"), windows=False)
        configure_steps = [step for step in calls if step.endswith("-configure")]
        self.assertEqual(["mt-configure", "st-configure", "restore-configure"], configure_steps)
        for step in configure_steps:
            self.assertIn("-DA=B", self.commands[step])

    def test_bad_define_and_windows_mysqld_rejected(self):
        with self.subTest(case="define"):
            with self.assertRaises(SystemExit):
                self.exercise(extra_args=("--define", "AB"), windows=False)
        with self.subTest(case="mysqld"):
            with self.assertRaises(SystemExit):
                self.exercise(extra_args=("--platform", "native", "--mysqld", "/opt/m/mysqld"), windows=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
