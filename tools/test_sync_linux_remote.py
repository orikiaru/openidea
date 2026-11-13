"""sync_linux_remote 的本地单测：排除规则、标记参数、远端脚本不含 CR。"""
import tempfile
import unittest
from pathlib import Path

import sync_linux_remote as sync


class SyncLinuxRemoteTests(unittest.TestCase):
    def test_should_exclude_build_and_local(self):
        self.assertTrue(sync.should_exclude(Path(".local/linux-hosts.json")))
        self.assertTrue(sync.should_exclude(Path("build-win/CMakeCache.txt")))
        self.assertTrue(sync.should_exclude(Path("build-clangd-linux/foo.o")))
        self.assertTrue(sync.should_exclude(Path("deploy/bin/ipcclient")))
        self.assertTrue(sync.should_exclude(Path("modules/__pycache__/x.pyc")))
        self.assertFalse(sync.should_exclude(Path("deploy/config/ipcclient.toml")))
        self.assertFalse(sync.should_exclude(Path("modules/ipcclient/src/coro_mgr.cpp")))

    def test_parse_require(self):
        self.assertEqual(
            sync.parse_require(["modules/ipcclient/src/coro_mgr.cpp:live_ns="]),
            [("modules/ipcclient/src/coro_mgr.cpp", "live_ns=")],
        )
        with self.assertRaises(SystemExit):
            sync.parse_require(["no-colon"])

    def test_apply_script_lf_and_markers(self):
        text = sync.apply_script(
            "/home/u/openidea",
            "/home/u/openidea-sync.tar",
            [("modules/ipcclient/src/coro_mgr.cpp", "live_ns=")],
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
            self.assertTrue(all(not name.startswith("build-win") for name in names))
            self.assertTrue(all(not name.startswith("deploy/bin") for name in names))


if __name__ == "__main__":
    unittest.main()
