"""Protocol schema history, source comments and MySQL index declarations."""
import os
import hashlib
import json
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
    def test_nested_storage_compatibility(self):
        child = "rpcdata Child json { 7 [json count] int32_t iNumber; }"
        for mode, column, identity in [
            ("", "Child stValue", "column stValue, Child PB field #7"),
            ("", "map<string, vector<Child>> mValues", "column mValues, Child PB field #7"),
            ("[dbjson]", "vector<Child> vValues", "column vValues, Child JSON key count"),
        ]:
            with self.subTest(column=column), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                spec = child + " rpcdata Parent db { 0 [dbkey] int32_t iId; 1 %s %s; }" % (mode, column)
                self.assertEqual(generate(root, spec).returncode, 0)
                script = root / "sql/parent.sql"
                header = root / "include/oi_rpcdata_child.h"
                original = script.read_bytes()
                # Field identity is the stored key. A C++ rename leaves the old value readable.
                renamed = spec.replace("iNumber", "iCount")
                if mode:
                    renamed = renamed.replace("7 [json", "11 [json")
                same = generate(root, renamed)
                self.assertEqual(same.returncode, 0, same.stdout)
                self.assertEqual(script.read_bytes(), original)
                old_header = header.read_bytes()
                for changed, reason in [
                    (renamed.replace("int32_t iCount", "int64_t iCount"), "changed type: int32_t -> int64_t"),
                    (renamed.replace("json count", "json total") if mode else renamed.replace("7 [json", "8 [json"),
                     "removed or renamed"),
                ]:
                    rejected = generate(root, changed)
                    self.assertNotEqual(rejected.returncode, 0)
                    self.assertIn("parent.sql", rejected.stdout)
                    self.assertIn(identity + " " + reason, rejected.stdout)
                    self.assertEqual((script.read_bytes(), header.read_bytes()), (original, old_header))
                extended = renamed.replace("iCount;", "iCount; 12 string sLabel;")
                added = generate(root, extended)
                self.assertEqual(added.returncode, 0, added.stdout)
                self.assertIn("v2:", added.stdout)
                version = script.read_bytes()
                self.assertIn(b"Nested encoding history updated", version)
                self.assertEqual(generate(root, extended).returncode, 0)
                self.assertEqual(script.read_bytes(), version)

    def test_deep_and_recursive_pb_types(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            spec = ("rpcdata Key key { 1 int32_t iId; } "
                    "rpcdata Node { 0 vector<Node> vChildren; 1 map<Key, string> mValues; } "
                    "rpcdata Tree db { 0 [dbkey] int32_t iId; 1 Node stRoot; }")
            saved = generate(root, spec)
            self.assertEqual(saved.returncode, 0, saved.stdout)
            rejected = generate(root, spec.replace("1 int32_t iId", "1 int64_t iId"))
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("column stRoot, Key PB field #1 changed type: int32_t -> int64_t", rejected.stdout)

    def test_legacy_nested_history_requires_deployed_baseline(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            spec = ("rpcdata Child { 7 int32_t iCount; } "
                    "rpcdata Parent db { 0 [dbkey] int32_t iId; 1 Child stChild; }")
            self.assertEqual(generate(root, spec).returncode, 0)
            script = root / "sql/parent.sql"
            header, _, body = script.read_text(encoding="utf-8").split("\n", 2)
            prefix, history = header.split(" ", 2)[1:]
            legacy = json.loads(history)
            del legacy[0]["columns"][1]["nested"]
            header = "-- " + prefix + " " + json.dumps(legacy, separators=(",", ":")) + "\n"
            checksum = hashlib.sha256((header + body).encode()).hexdigest()
            original = (header + "-- oi-mysql-sha256 " + checksum + "\n" + body).encode()
            script.write_bytes(original)
            changed = spec.replace("7 int32_t", "7 int64_t")
            attempted = generate(root, changed)
            self.assertNotEqual(attempted.returncode, 0)
            self.assertIn("--baseline-mysql-nested", attempted.stdout)
            self.assertEqual(script.read_bytes(), original)
            source = root / "schema.proto"
            args = [sys.executable, str(GENERATOR), "--baseline-mysql-nested", str(root / "sql"), "--", str(source)]
            source.write_text(spec.replace("0 [dbkey]", "2 uint32_t iOther; 0 [dbkey]"), encoding="utf-8")
            bad = subprocess.run(args, capture_output=True, text=True)
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn("previously deployed protocol", bad.stdout)
            self.assertEqual(script.read_bytes(), original)
            source.write_text(spec, encoding="utf-8")
            baseline = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(baseline.returncode, 0, baseline.stdout)
            saved = script.read_bytes()
            self.assertIn(b"-- v2:", saved)
            self.assertIn(b"-- v1:", saved)
            refused = generate(root, changed)
            self.assertNotEqual(refused.returncode, 0)
            self.assertIn("column stChild, Child PB field #7 changed type", refused.stdout)
            self.assertEqual(script.read_bytes(), saved)

    def test_physical_table_name(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            spec = BASE.replace("Player db", "Player db(game_player)")
            saved = generate(root, spec)
            self.assertEqual(saved.returncode, 0, saved.stdout)
            script = root / "sql/game_player.sql"
            self.assertIn(b"CREATE TABLE IF NOT EXISTS `game_player`", script.read_bytes())
            self.assertFalse((root / "sql/player.sql").exists())
            self.assertIn('return "game_player";', (root / "include/oi_rpcdata_player.h").read_text(encoding="utf-8"))
            for name in ["Game_Player", "Other db(GAME_PLAYER)"]:
                extra = " rpcdata " + name + (" db" if "(" not in name else "") + " { 0 int32_t iId; }"
                refused = generate(root, spec + extra)
                self.assertNotEqual(refused.returncode, 0)
                self.assertIn("table name collision", refused.stdout)
            for name in ["", "bad-name", "a" * 65]:
                self.assertNotEqual(generate(root, spec.replace("game_player", name)).returncode, 0)

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
