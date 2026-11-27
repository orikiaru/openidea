"""远程源码同步的打包、安装与回滚测试。"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import sync_linux_remote as sync


class SyncLinuxRemoteTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "posix" and shutil.which("bash") and shutil.which("mv"), "requires Linux shell")
    def test_apply_switch_and_recovery(self):
        for mode in ("success", "fail", "occupied"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                source = root / "source"
                source.mkdir()
                (source / "old.txt").write_text("old", encoding="utf-8")
                incoming = root / "incoming"
                (incoming / "tools").mkdir(parents=True)
                (incoming / ".git").mkdir()
                (incoming / "tools" / "test_matrix.py").write_text("marker", encoding="utf-8")
                (incoming / "tools" / "cleanup_linux_tests.py").write_text("cleanup", encoding="utf-8")
                (incoming / "new.txt").write_text("new", encoding="utf-8")
                archive = root / "source.tar"
                sync.build_archive(incoming, archive)
                bindir = root / "bin"
                bindir.mkdir()
                # 隔离目录没有正在运行的矩阵，宿主的其他回归与此测试无关。
                ps = bindir / "ps"
                ps.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
                ps.chmod(0o755)
                mv = bindir / "mv"
                real_mv = shutil.which("mv")
                mv.write_text(
                    "#!/bin/sh\n"
                    "for arg do\n"
                    "  case \"$arg\" in *.incoming.*) stage=$arg;; esac\n"
                    "done\n"
                    "if [ -n \"${stage:-}\" ] && [ ! -e \"$INJECTED\" ]; then\n"
                    "  : > \"$INJECTED\"\n"
                    "  if [ \"$MODE\" = fail ]; then exit 42; fi\n"
                    "  if [ \"$MODE\" = occupied ]; then mkdir -p \"$TARGET\"; printf concurrent > \"$TARGET/concurrent.txt\"; fi\n"
                    "fi\n"
                    f'exec "{real_mv}" "$@"\n', encoding="utf-8"
                )
                mv.chmod(0o755)
                env = dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}", MODE=mode,
                           TARGET=str(source), INJECTED=str(root / "injected"))
                result = subprocess.run(["bash", "-s"], input=sync.apply_script(str(source), str(archive),
                                        [("tools/test_matrix.py", "marker")]), text=True,
                                        capture_output=True, env=env)
                self.assertTrue((root / "injected").exists(), result.stdout + result.stderr)
                if mode == "success":
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("SYNC_OK=1", result.stdout)
                    self.assertEqual((source / "new.txt").read_text(), "new")
                    self.assertFalse((source / "old.txt").exists())
                    self.assertFalse(list(root.glob("source.backup.*")))
                elif mode == "fail":
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual((source / "old.txt").read_text(), "old")
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual((source / "concurrent.txt").read_text(), "concurrent")
                    backups = list(root.glob("source.backup.*"))
                    self.assertEqual(len(backups), 1, result.stdout + result.stderr)
                    self.assertEqual((backups[0] / "old.txt").read_text(), "old")
                    self.assertIn(str(backups[0]), result.stdout + result.stderr)

    def test_should_exclude_build_and_local(self):
        self.assertTrue(sync.should_exclude(Path(".local/linux-hosts.json")))
        self.assertTrue(sync.should_exclude(Path(".codex/session.json")))
        self.assertTrue(sync.should_exclude(Path("build-win/CMakeCache.txt")))
        self.assertTrue(sync.should_exclude(Path("build-clangd-linux/foo.o")))
        self.assertTrue(sync.should_exclude(Path("deploy/bin/ipcclient")))
        self.assertTrue(sync.should_exclude(Path("modules/__pycache__/x.pyc")))
        self.assertFalse(sync.should_exclude(Path("deploy/config/ipcclient.toml")))
        self.assertFalse(sync.should_exclude(Path("modules/ipcclient/src/test_coro.cpp")))

    def test_parse_require(self):
        self.assertEqual(
            sync.parse_require(["modules/ipcclient/src/test_coro.cpp:live_ns="]),
            [("modules/ipcclient/src/test_coro.cpp", "live_ns=")],
        )
        with self.assertRaises(SystemExit):
            sync.parse_require(["no-colon"])

    def test_apply_script_lf_and_markers(self):
        text = sync.apply_script(
            "/home/u/openidea",
            "/home/u/openidea-sync.tar",
            [("modules/ipcclient/src/test_coro.cpp", "live_ns=")],
        )
        self.assertNotIn("\r", text)
        self.assertIn("SYNC_OK=1", text)
        self.assertIn("live_ns=", text)
        self.assertIn("tools/test_matrix.py", text)

    def test_busy_script_uses_safe_set(self):
        text = sync.busy_script("/home/u/openidea")
        self.assertNotIn("\r", text)
        self.assertIn("BUSY=0", text)

    def test_build_archive_skips_excluded(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repo"
            (root / "tools").mkdir(parents=True)
            (root / "tools" / "test_matrix.py").write_text("ok\n", encoding="utf-8")
            (root / ".local").mkdir()
            (root / ".local" / "secret.json").write_text("{}\n", encoding="utf-8")
            (root / ".codex").mkdir()
            (root / ".codex" / "private.json").write_text("{}\n", encoding="utf-8")
            (root / "build-win").mkdir()
            (root / "build-win" / "a.obj").write_text("x", encoding="utf-8")
            (root / "deploy" / "bin").mkdir(parents=True)
            (root / "deploy" / "bin" / "ipcclient").write_text("bin", encoding="utf-8")
            (root / "deploy" / "config").mkdir(parents=True)
            (root / "deploy" / "config" / "ipcclient.toml").write_text("x=1\n", encoding="utf-8")
            archive = Path(tmp) / "out.tar"
            count = sync.build_archive(root, archive)
            self.assertGreater(count, 0)
            names = set()
            import tarfile

            with tarfile.open(archive, "r") as tar:
                names = {member.name for member in tar.getmembers()}
            self.assertIn("tools/test_matrix.py", names)
            self.assertIn("deploy/config/ipcclient.toml", names)
            self.assertTrue(all(not name.startswith(".local") for name in names))
            self.assertTrue(all(not name.startswith(".codex") for name in names))
            self.assertTrue(all(not name.startswith("build-win") for name in names))
            self.assertTrue(all(not name.startswith("deploy/bin") for name in names))


if __name__ == "__main__":
    unittest.main()
