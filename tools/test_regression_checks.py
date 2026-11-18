import subprocess
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

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

    def test_admission_requires_each_round_once(self):
        lines = ["hold rpc admission round=%d ok=2 busy=4 bad=0" % i for i in range(3)]
        regression.check_admission_rounds("\n".join(lines))
        with self.assertRaises(AssertionError):
            regression.check_admission_rounds("\n".join([lines[0]] * 3))
        with self.assertRaises(AssertionError):
            regression.check_admission_rounds("\n".join(lines + [lines[0]]))

    def test_web_skips_unrelated_port_preflight(self):
        with patch.object(regression.socket, "socket", side_effect=OSError("port occupied")) as probe:
            regression.preflight_ports("web")
            probe.assert_not_called()
            with self.assertRaisesRegex(OSError, "port occupied"):
                regression.preflight_ports("echo")
            probe.assert_called_once()

    def test_optimized_python_is_rejected(self):
        script = Path(regression.__file__)
        result = subprocess.run([sys.executable, "-O", str(script), "--help"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires assertions", result.stderr)


if __name__ == "__main__":
    unittest.main()
