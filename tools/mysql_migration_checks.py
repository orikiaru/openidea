"""Exercise the distributed SQL with the mysql client in the regression runner's private server."""
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def check_mysql_migrations(client, env, directory):
    def sql(script, *, error=None, database="oi_schema_migration"):
        args = client + ["--batch", "--raw", "--skip-column-names", "--database=" + database]
        result = subprocess.run(args, input=script, env=env, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=30)
        if error:
            if not result.returncode or error not in result.stdout + result.stderr:
                raise RuntimeError(f"expected mysql error {error}: {result.stdout} {result.stderr}")
        elif result.returncode:
            raise RuntimeError("mysql migration failed: " + result.stdout + result.stderr)
        return result.stdout.strip()

    def equal(actual, expected):
        if actual != expected:
            raise RuntimeError(f"mysql migration: expected {expected!r}, got {actual!r}")

    def export(schema):
        source = directory / "players.proto"
        source.write_text(schema, encoding="utf-8")
        subprocess.run([sys.executable, str(ROOT / "cbaselib/proto/proto_tool/main.py"),
                        "--mysql-script-dir=" + str(directory / "sql"), "--data-only", str(directory / "headers"),
                        "--", str(source)], env=env, check=True, timeout=30)
        return (directory / "sql/player.sql").read_text(encoding="utf-8")

    sql("CREATE DATABASE oi_schema_migration", database="oi_regression")
    original = """rpcdata Player db {
        0 [db zone, dbkey] uint32_t iZone;
        1 [db role, dbkey, autoincrement] uint64_t iRole;
        uniqueindex uq_role(iRole);
    }"""
    sql(export(original))
    equal(sql("INSERT INTO player(zone) VALUES (7); SELECT LAST_INSERT_ID();"), "1")
    sql("INSERT INTO player(zone, role) VALUES (8, 1)", error="uq_role")
    second = original.replace("uniqueindex", "2 [db coins] int32_t iCoins; index lookup(iZone,iCoins); uniqueindex")
    sql(export(second))
    sql("UPDATE player SET coins=42 WHERE role=1")
    third = second.replace("2 [db coins]", "3 [dbjson] vector<int32_t> vJson; 4 vector<uint32_t> vBlob; 2 [db coins]")
    latest = export(third)
    sql(latest)
    sql(latest)
    equal(sql("SELECT zone,role,vJson,HEX(vBlob),coins FROM player WHERE role=1"),
          "7\t1\t[]\t000000000400000000000000\t42")
    fields = "SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='player' ORDER BY ORDINAL_POSITION"
    equal(sql(fields), "zone\nrole\nvJson\nvBlob\ncoins")
    equal(sql("SELECT COLUMN_NAME FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='player' AND INDEX_NAME='lookup' ORDER BY SEQ_IN_INDEX"), "zone\ncoins")
    upgraded = sql("SHOW CREATE TABLE player")
    # A fresh install from the latest script produces the same structure as the upgraded table.
    sql("CREATE DATABASE oi_schema_fresh")
    sql(latest, database="oi_schema_fresh")
    equal(re.sub(r" AUTO_INCREMENT=\d+", "", upgraded), sql("SHOW CREATE TABLE player", database="oi_schema_fresh"))
    # Missing additions stay absent after a schema mismatch. Verify the original row as well.
    sql("ALTER TABLE player DROP INDEX lookup, DROP COLUMN vJson, MODIFY coins BIGINT NOT NULL")
    sql(latest, error="column coins differs from protocol")
    equal(sql(fields), "zone\nrole\nvBlob\ncoins")
    sql("ALTER TABLE player MODIFY coins INT NOT NULL, DROP PRIMARY KEY, ADD PRIMARY KEY(role,zone)")
    sql(latest, error="primary key differs from protocol")
    equal(sql("SELECT zone,role,coins FROM player"), "7\t1\t42")
    sql("ALTER TABLE player DROP PRIMARY KEY, ADD PRIMARY KEY(zone,role), ADD UNIQUE INDEX lookup(coins,zone)")
    sql(latest, error="index lookup differs from protocol")
    equal(sql(fields), "zone\nrole\nvBlob\ncoins")
    # A new unique index must reject existing duplicate data without partially adding columns.
    sql("ALTER TABLE player DROP INDEX lookup")
    sql("INSERT INTO player(zone,coins) VALUES (7,42)")
    next_version = third.replace("uniqueindex uq_role", "uniqueindex uq_coins(iCoins); uniqueindex uq_role")
    sql(export(next_version), error="Duplicate entry")
    equal(sql(fields), "zone\nrole\nvBlob\ncoins")
    equal(sql("SELECT COUNT(*) FROM player WHERE coins=42"), "2")

    # Import the actual application and framework deliverables, including their complete type mappings.
    for file in ("dbsample.sql", "framework/servicelease.sql"):
        script = (ROOT / "module/mysql_script" / file).read_text(encoding="utf-8")
        sql(script)
        sql(script)
    print("mysql migration PASS: versions, fresh install, replay, indexes, primary key, auto increment, drift and atomic ALTER", flush=True)
