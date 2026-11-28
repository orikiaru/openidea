"""Protocol schema history, source comments and MySQL index declarations."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

GENERATOR = Path(__file__).resolve().parents[1] / "cbaselib/proto/proto_tool/main.py"
BASE = """rpcdata Player db {
    0 [db zone, dbkey] uint32_t iZone;
    1 [db role, dbkey, autoincrement] uint64_t iRole;
    uniqueindex uq_role(iRole);
}"""


def generate(root, spec):
    source = root / "schema.proto"
    source.write_text(spec, encoding="utf-8")
    return subprocess.run([sys.executable, str(GENERATOR), "--mysql-script-dir=" + str(root / "sql"),
                           "--data-only", str(root / "include"), "--", str(source)],
                          capture_output=True, text=True, encoding="utf-8", errors="replace")


class MysqlExportTest(unittest.TestCase):
    def test_version_history_and_refusal(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            script = root / "sql/player.sql"
            self.assertEqual(generate(root, BASE).returncode, 0)
            original = script.read_bytes()
            os.utime(script, (1000000000, 1000000000))
            timestamp = script.stat().st_mtime_ns
            comment = "only_in_protocol"
            marked = ("/* " + comment + "\n block */\n" + BASE).replace("iRole;", "iRole; // " + comment)
            same = generate(root, marked)
            self.assertEqual(same.returncode, 0, same.stdout)
            self.assertNotIn("MySQL export ", same.stdout)
            self.assertEqual((script.read_bytes(), script.stat().st_mtime_ns), (original, timestamp))
            self.assertNotIn(comment, (root / "include/oi_rpcdata_player.h").read_text(encoding="utf-8"))
            add = BASE.replace("uniqueindex", "2 [db coins] uint32_t iCoins; index idx_zone_coins(iZone,iCoins); uniqueindex")
            changed = generate(root, add)
            self.assertEqual(changed.returncode, 0, changed.stdout)
            self.assertIn("v2: added columns [coins], indexes [idx_zone_coins]", changed.stdout)
            saved = script.read_bytes()
            self.assertIn(b"-- v1:", saved)
            self.assertIn(b"-- v2:", saved)
            for invalid, reason in [
                (BASE, "column removed"), (add.replace("[db coins]", "[db balance]"), "column removed"),
                (add.replace("uint32_t iCoins", "uint64_t iCoins"), "column coins changed"),
                (add.replace("db zone, dbkey", "db zone"), "primary key changed"),
                (add.replace(", autoincrement", ""), "column role changed"),
                (add.replace("uniqueindex", "index"), "index removed or changed"),
                (add.replace("iZone,iCoins", "iCoins,iZone"), "index removed or changed"),
                (add.replace("Player", "Other"), "table removed or renamed"),
            ]:
                with self.subTest(reason=reason):
                    rejected = generate(root, "rpcdata NewTable db { 0 [dbkey] int32_t iId; }\n" + invalid)
                    self.assertNotEqual(rejected.returncode, 0)
                    self.assertIn(reason, rejected.stdout)
                    self.assertIn("rebuild the table", rejected.stdout)
                    self.assertEqual(script.read_bytes(), saved)
                    self.assertFalse((root / "sql/newtable.sql").exists())
            script.unlink()  # Explicit operator reset after backing up the old table.
            reset = generate(root, add.replace("uint32_t iCoins", "uint64_t iCoins"))
            self.assertEqual(reset.returncode, 0, reset.stdout)
            self.assertNotIn(b"-- v2:", script.read_bytes())

    def test_rejects_invalid_definitions(self):
        specs = [
            BASE + " /* missing end", BASE.replace("uq_role(iRole)", "uq_role(iMissing)"),
            BASE.replace("uq_role(iRole)", "uq_role(iRole,iRole)"),
            BASE.replace("uniqueindex uq_role(iRole);", "uniqueindex uq_role(iRole); index UQ_ROLE(iZone);"),
            BASE.replace("uniqueindex uq_role(iRole);", ""),  # AUTO_INCREMENT no longer leads any key.
            BASE.replace("db zone, dbkey", "db zone, dbkey, autoincrement"),
            BASE.replace("uint64_t iRole", "bool bRole").replace("(iRole)", "(bRole)"),
            BASE.replace("db role, dbkey,", "db role,"),
            BASE.replace("uniqueindex", "2 string sName; index idx_text(sName); uniqueindex"),
            BASE.replace("uniqueindex", "2 [nodb] int32_t iSkip; index idx_skip(iSkip); uniqueindex"),
        ]
        with tempfile.TemporaryDirectory() as temp:
            for spec in specs:
                with self.subTest(spec=spec):
                    self.assertNotEqual(generate(Path(temp), spec).returncode, 0)
                    self.assertFalse((Path(temp) / "sql/player.sql").exists())


if __name__ == "__main__":
    unittest.main()
