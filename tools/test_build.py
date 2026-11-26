"""build.py 只检查命令，不真的调用 cmake。"""
import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import build


def ipc_test_source_errors(root):
    """Check test source registration and reject reintroduced test Mgr classes."""
    root = Path(root)
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"(?is)\bset\s*\(\s*MODULE_SRCS\b(.*?)\)", cmake)
    if not match:
        return ["missing MODULE_SRCS"]
    listed = re.findall(r"(?m)^\s*(src/test_[A-Za-z0-9_]+\.cpp)\s*$", match.group(1))
    actual = {"src/" + path.name for path in (root / "src").glob("test_*.cpp")}
    errors = ["test sources missing from MODULE_SRCS: " + ", ".join(sorted(actual - set(listed)))] if actual - set(listed) else []
    if set(listed) - actual:
        errors.append("registered test sources absent: " + ", ".join(sorted(set(listed) - actual)))
    if len(listed) != len(set(listed)):
        errors.append("duplicate registered test source")
    for directory in ("inc", "src"):
        for path in (root / directory).glob("*"):
            if path.suffix not in (".h", ".cpp"):
                continue
            if re.search(r"(?m)^\s*(?:class|struct)\s+\w+Mgr\b", path.read_text(encoding="utf-8")):
                errors.append("legacy test Mgr declaration: " + str(path))
            if path.stem.endswith("_mgr"):
                errors.append("legacy test Mgr file: " + str(path))
    return errors


class IpcTestSourceTests(unittest.TestCase):
    def test_repository_sources(self):
        self.assertEqual(ipc_test_source_errors(build.ROOT / "modules/ipcclient"), [])

    def test_missing_and_duplicate_sources_and_old_mgr_fail(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "src").mkdir()
            (root / "inc").mkdir()
            (root / "src/test_example.cpp").write_text("class TestExample {};", encoding="utf-8")
            (root / "inc/example_mgr.h").write_text("class ExampleMgr {};", encoding="utf-8")
            (root / "CMakeLists.txt").write_text("set(MODULE_SRCS\n src/test_board.cpp\n src/test_board.cpp\n)\n", encoding="utf-8")
            errors = "\n".join(ipc_test_source_errors(root))
            for expected in ("missing from MODULE_SRCS", "registered test sources absent", "duplicate", "Mgr declaration", "Mgr file"):
                self.assertIn(expected, errors)



class BuildCommandTests(unittest.TestCase):
    def test_windows_command_uses_all_cores_and_caller_mode(self):
        with mock.patch.object(build, "WINDOWS", True), mock.patch.object(build.sys, "executable", "python.exe"):
            configure = build.configure_command("cmake", Path("build-win"), "mt", ["FOO=1"], None)
            compile_cmd = build.build_command("cmake", Path("build-win"), 16)
        self.assertIn("-G", configure)
        self.assertIn("Visual Studio 17 2022", configure)
        self.assertIn("-DOI_IPC_SINGLE_THREAD=OFF", configure)
        self.assertIn("-DFOO=1", configure)
        self.assertIn("-j", compile_cmd)
        self.assertIn("/m:16", compile_cmd)
        self.assertIn("/p:CL_MPCount=16", compile_cmd)

    def test_linux_command_uses_gcc13(self):
        with mock.patch.object(build, "WINDOWS", False):
            configure = build.configure_command("cmake", Path("build"), "st", [], None)
            compile_cmd = build.build_command("cmake", Path("build"), 4)
        self.assertIn("-DCMAKE_CXX_COMPILER=g++-13", configure)
        self.assertIn("-DOI_IPC_SINGLE_THREAD=ON", configure)
        self.assertNotIn("--", compile_cmd)
        self.assertEqual(compile_cmd[compile_cmd.index("-j") + 1], "4")

    def test_main_build_directories_per_platform_and_mode(self):
        for windows in (True, False):
            for mode in ("mt", "st"):
                with self.subTest(windows=windows, mode=mode):
                    expected = (build.ROOT / "build-win" if windows else Path.home() / "openidea-build-gcc13")
                    if mode == "st":
                        expected = expected.with_name(expected.name + "-st")
                    with mock.patch.object(build, "WINDOWS", windows), \
                         mock.patch.object(build.test_matrix, "missing_toolset", return_value=""), \
                         mock.patch.object(build.test_matrix, "find_cmake", return_value="cmake"), \
                         mock.patch.object(build.subprocess, "call", return_value=0) as call:
                        self.assertEqual(build.main(["--mode", mode]), 0)
                    configure, compile_cmd = (item.args[0] for item in call.call_args_list)
                    self.assertEqual(Path(configure[configure.index("-B") + 1]), expected.resolve())
                    self.assertEqual(Path(compile_cmd[compile_cmd.index("--build") + 1]), expected.resolve())

    def test_explicit_build_directory_is_used_for_all_modes(self):
        for windows in (True, False):
            for mode in ("mt", "st"):
                with self.subTest(windows=windows, mode=mode):
                    expected = Path("custom-build").resolve()
                    with mock.patch.object(build, "WINDOWS", windows), \
                         mock.patch.object(build.test_matrix, "missing_toolset", return_value=""), \
                         mock.patch.object(build.test_matrix, "find_cmake", return_value="cmake"), \
                         mock.patch.object(build.subprocess, "call", return_value=0) as call:
                        self.assertEqual(build.main(["--mode", mode, "--build-dir", str(expected)]), 0)
                    configure, compile_cmd = (item.args[0] for item in call.call_args_list)
                    self.assertEqual(Path(configure[configure.index("-B") + 1]), expected)
                    self.assertEqual(Path(compile_cmd[compile_cmd.index("--build") + 1]), expected)

    def test_default_jobs_follows_cpu(self):
        with mock.patch.object(build.os, "cpu_count", return_value=16):
            self.assertEqual(build.default_jobs(), 16)
        self.assertGreaterEqual(build.default_jobs(), 1)
        self.assertTrue(build.default_build_dir())


if __name__ == "__main__":
    unittest.main()
