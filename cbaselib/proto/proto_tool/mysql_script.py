"""Export one replayable MySQL 8 script, including its schema history, per DB row."""
from contextlib import contextmanager
from hashlib import sha256
import json
import os
from pathlib import Path
import tempfile

from proto_parse import ParseType, TypeNames
from serialize_data_parse import DbIndexDefinition, DbColumnDefault, DB_COLUMN_NAME, SerializeDataType


HISTORY = "-- oi-mysql-history-v1 "
CHECKSUM = "-- oi-mysql-sha256 "


def nested_types(field, types):
    """Store each reachable rpcdata once, including self-references through containers."""
    definitions = {}
    pending = list(TypeNames(ParseType(field.type)))
    while pending:
        name = pending.pop()
        if name not in types or name in definitions:
            continue
        fields = {}
        definitions[name] = fields
        for member in types[name].members:
            if field.db_json and not member.json_name:
                continue
            key = member.json_name if field.db_json else str(member.seq)
            fields[key] = member.type
            pending.extend(TypeNames(ParseType(member.type)))
    return definitions


def table_schema(data, types):
    columns = []
    primary = []
    for field in data.members:
        if not field.db_name:
            continue
        column = dict(name=field.db_name, sql=field.DbSqlType(), seq=field.seq,
                      type=field.GetType(), json=field.db_json, auto=field.auto_increment)
        nested = nested_types(field, types)
        if nested:
            column["nested"] = nested
        columns.append(column)
        if field.db_key:
            primary.append(field.db_name)
    return dict(columns=columns, primary=primary, indexes=data.DbIndexes())


def check_nested_column(old, new):
    for type_name, fields in old["nested"].items():
        current = new.get("nested", {}).get(type_name, {})
        for key, old_type in fields.items():
            field = "JSON key " + key if old["json"] else "PB field #" + key
            location = "column %s, %s %s" % (old["name"], type_name, field)
            if key not in current:
                raise ValueError(location + " removed or renamed (previously " + old_type + ")")
            if old_type != current[key]:
                raise ValueError("%s changed type: %s -> %s" % (location, old_type, current[key]))


def schema_changes(old, new):
    if old["primary"] != new["primary"]:
        raise ValueError("primary key changed: %s -> %s" % (old["primary"], new["primary"]))
    columns = {col["name"]: col for col in new["columns"]}
    old_names = [col["name"] for col in old["columns"]]
    for col in old["columns"]:
        name = col["name"]
        if name not in columns:
            raise ValueError("column removed or renamed: " + name)
        for key, value in col.items():
            if key == "nested":
                check_nested_column(col, columns[name])
            elif columns[name][key] != value:
                raise ValueError("column %s changed %s: %s -> %s" % (name, key, value, columns[name][key]))
    existing = set(old_names)
    if [name for name in columns if name in existing] != old_names:
        raise ValueError("existing columns reordered")
    for name, index in old["indexes"].items():
        if new["indexes"].get(name) != index:
            raise ValueError("index removed or changed: %s (%s -> %s)" % (name, index, new["indexes"].get(name)))
    return ([name for name in columns if name not in existing],
            [name for name in new["indexes"] if name not in old["indexes"]])


def changed_nested_columns(old, new):
    previous = {col["name"]: col for col in old["columns"]}
    return [col["name"] for col in new["columns"] if col["name"] in previous and
            col.get("nested") != previous[col["name"]].get("nested")]


def index_check(table, name, columns, unique):
    # Match every key part, including order, uniqueness and any prefix length.
    parts = ["(SEQ_IN_INDEX = %d AND BINARY COLUMN_NAME = '%s')" % (pos, col)
             for pos, col in enumerate(columns, 1)]
    where = "TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '%s' AND INDEX_NAME = '%s'" % (table, name)
    return ("EXISTS (SELECT 1 FROM information_schema.STATISTICS WHERE %s GROUP BY INDEX_NAME\n"
            "        HAVING COUNT(*) <> %d OR SUM(CASE WHEN (%s) AND NON_UNIQUE = %d\n"
            "            AND SUB_PART IS NULL AND COLLATION = 'A' AND INDEX_TYPE = 'BTREE' AND IS_VISIBLE = 'YES'\n"
            "            THEN 1 ELSE 0 END) <> %d)" %
            (where, len(columns), " OR ".join(parts), int(not unique), len(columns)))


def render_sql(table, history):
    schema = history[-1]
    columns = schema["columns"]
    primary = schema["primary"]
    indexes = schema["indexes"]
    definitions = ["`%s` %s NOT NULL%s%s" % (col["name"], col["sql"], " AUTO_INCREMENT" if col["auto"] else "",
                                               DbColumnDefault(col["type"], col["json"]))
                   for col in columns]
    if primary:
        definitions.append("PRIMARY KEY (%s)" % ", ".join("`%s`" % col for col in primary))
    definitions.extend(DbIndexDefinition(name, index) for name, index in indexes.items())
    lines = [
        "-- Import into the target database with mysql. Keep this file under version control.",
        "CREATE TABLE IF NOT EXISTS `%s` (\n    %s\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;" %
        (table, ",\n    ".join(definitions)), "",
        "-- Validate existing definitions before executing any ALTER.",
        "DROP TEMPORARY TABLE IF EXISTS _oi_expected_columns;",
        "CREATE TEMPORARY TABLE _oi_expected_columns (name VARBINARY(64) PRIMARY KEY, sql_type VARCHAR(64), auto_id BOOL);",
        "INSERT INTO _oi_expected_columns VALUES\n    " +
        ",\n    ".join("('%s', '%s', %d)" % (col["name"], col["sql"].lower(), col["auto"]) for col in columns) + ";",
        "SET @oi_error = (SELECT CONCAT('column ', c.COLUMN_NAME, ' differs from protocol')",
        "    FROM information_schema.COLUMNS c LEFT JOIN _oi_expected_columns e ON e.name = BINARY c.COLUMN_NAME",
        "    WHERE c.TABLE_SCHEMA = DATABASE() AND c.TABLE_NAME = '%s'" % table,
        "      AND (e.name IS NULL OR e.sql_type <> c.COLUMN_TYPE OR c.IS_NULLABLE <> 'NO'",
        "           OR (e.auto_id AND c.EXTRA <> 'auto_increment')",
        "           OR (NOT e.auto_id AND c.EXTRA NOT IN ('', 'DEFAULT_GENERATED'))) LIMIT 1);",
        "DROP TEMPORARY TABLE _oi_expected_columns;",
    ]
    actual = "TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '%s'" % table
    lines.append("SET @oi_error = COALESCE(@oi_error, IF(EXISTS (SELECT 1 FROM information_schema.TABLES WHERE %s"
                 " AND (TABLE_TYPE <> 'BASE TABLE' OR ENGINE <> 'InnoDB')), 'expected an InnoDB table', NULL));" % actual)
    has_pk = "EXISTS (SELECT 1 FROM information_schema.STATISTICS WHERE %s AND INDEX_NAME = 'PRIMARY')" % actual
    pk_changed = has_pk
    if primary:
        pk_changed = "NOT " + has_pk + " OR " + index_check(table, "PRIMARY", primary, True)
    lines.append("SET @oi_error = COALESCE(@oi_error, IF(%s, 'primary key differs from protocol', NULL));" % pk_changed)
    for name, index in indexes.items():
        condition = index_check(table, name, index["columns"], index["unique"])
        lines.append("SET @oi_error = COALESCE(@oi_error, IF(%s, 'index %s differs from protocol', NULL));" % (condition, name))
    lines += [
        "SELECT CONCAT('%s: ', @oi_error, '. Back up and rebuild the table manually.') AS migration_error WHERE @oi_error IS NOT NULL;" % table,
        "DROP TEMPORARY TABLE IF EXISTS _oi_migration_guard;",
        "CREATE TEMPORARY TABLE _oi_migration_guard (issue VARCHAR(255), CHECK (issue IS NULL));",
        "INSERT INTO _oi_migration_guard VALUES (@oi_error);",
        "DROP TEMPORARY TABLE _oi_migration_guard;", "",
        "-- Accumulate missing columns and indexes across versions, then apply one atomic InnoDB ALTER.",
        "SET @oi_changes = NULL;",
    ]
    previous = {"columns": [], "indexes": {}, "primary": primary}
    for version, snapshot in enumerate(history, 1):
        added, indexed = schema_changes(previous, snapshot)
        nested = changed_nested_columns(previous, snapshot)
        previous = snapshot
        lines.append("-- v%d: columns [%s], indexes [%s]" % (version, ", ".join(added), ", ".join(indexed)))
        if nested:
            lines.append("-- Nested encoding history updated for: " + ", ".join(nested))
        for col_pos, col in enumerate(snapshot["columns"]):
            name = col["name"]
            if name in primary or name not in added:
                continue
            position = "AFTER `%s`" % snapshot["columns"][col_pos - 1]["name"] if col_pos else "FIRST"
            default = DbColumnDefault(col["type"], col["json"])
            clause = "ADD COLUMN `%s` %s NOT NULL%s %s" % (name, col["sql"], default, position)
            lines.append(missing_clause(actual, "COLUMNS", "COLUMN_NAME", name, clause))
        for name in indexed:
            clause = "ADD " + DbIndexDefinition(name, snapshot["indexes"][name])
            lines.append(missing_clause(actual, "STATISTICS", "INDEX_NAME", name, clause))
    lines += [
        "SET @oi_sql = IF(@oi_error IS NULL AND @oi_changes IS NOT NULL,",
        "    CONCAT('ALTER TABLE `%s` ', @oi_changes), 'DO 0');" % table,
        "PREPARE oi_migration FROM @oi_sql;", "EXECUTE oi_migration;", "DEALLOCATE PREPARE oi_migration;", ""
    ]
    return "\n".join(lines)


def missing_clause(table_filter, view, attr, name, ddl):
    return ("SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.%s WHERE %s AND %s = '%s'),\n"
            "    @oi_changes, CONCAT_WS(', ', @oi_changes, '%s'));" % (view, table_filter, attr, name, ddl))


def read_history(path):
    header, checksum, body = path.read_text(encoding="utf-8").split("\n", 2)
    if not header.startswith(HISTORY) or checksum != CHECKSUM + sha256((header + "\n" + body).encode()).hexdigest():
        raise ValueError("unrecognized or damaged script; restore the file from version control")
    history = json.loads(header[len(HISTORY):])
    if not isinstance(history, list) or not history:
        raise ValueError("missing schema history")
    return history


@contextmanager
def export_lock(directory):
    # CMake configurations can share the same protocol directory. OS releases this lock on exit.
    identity = os.path.normcase(str(directory.resolve())).encode()
    path = Path(tempfile.gettempdir()) / ("oi-mysql-export-" + sha256(identity).hexdigest() + ".lock")
    with path.open("a+b") as handle:
        handle.seek(0)
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(handle.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(handle, fcntl.LOCK_EX)
        yield


def export_mysql_scripts(datas, directory, baseline_only=False):
    directory = Path(directory)
    rows = {data.DbTableName(): data for data in datas if data.has_db}
    types = {data.name: data for data in datas if data.GetType() == SerializeDataType.RPC_DATA}
    if not rows and not directory.exists():
        return True
    try:
        directory.mkdir(parents=True, exist_ok=True)
        with export_lock(directory):
            pending = []
            for path in directory.glob("*.sql"):
                if path.stem not in rows:
                    with path.open(encoding="utf-8") as stream:
                        if stream.readline().startswith(HISTORY):
                            raise ValueError("table removed or renamed")
            for table, data in rows.items():
                path = directory / (table + ".sql")
                if not DB_COLUMN_NAME.fullmatch(table) or len(table) > 64:
                    raise ValueError("invalid MySQL table name")
                schema = table_schema(data, types)
                history = read_history(path) if path.exists() else []
                if history and history[-1] == schema:
                    continue
                old = history[-1] if history else {"columns": [], "primary": schema["primary"], "indexes": {}}
                added, indexed = schema_changes(old, schema)
                nested = changed_nested_columns(old, schema)
                missing = [col["name"] for col in old["columns"] if col["name"] in nested and "nested" not in col]
                if baseline_only:
                    if not history or added or indexed or nested != missing:
                        raise ValueError("baseline must use the previously deployed protocol, without schema changes")
                elif missing:
                    print("MySQL export refused:", path, "- missing nested encoding history for", ", ".join(missing),
                          "\nUse the previously deployed protocol with --baseline-mysql-nested <sql-dir> -- <proto-files>,"
                          " then export the new protocol.")
                    return False
                history.append(schema)
                body = render_sql(table, history)
                headers = HISTORY + json.dumps(history, separators=(",", ":")) + "\n"
                script = headers + CHECKSUM + sha256((headers + body).encode()).hexdigest() + "\n" + body
                note = "v%d: added columns [%s], indexes [%s]" % (len(history), ", ".join(added), ", ".join(indexed))
                if nested:
                    note += ", nested encoding [%s]" % ", ".join(nested)
                pending.append((path, script, note))
            # Validate every table before publishing the first file. Replace each file atomically.
            for path, script, note in pending:
                with tempfile.NamedTemporaryFile(dir=directory, delete=False) as output:
                    temp = Path(output.name)
                    try:
                        output.write(script.encode("utf-8"))
                        output.close()
                        os.replace(temp, path)
                    finally:
                        temp.unlink(missing_ok=True)
                print("MySQL export", path, note)
        return True
    except (ValueError, KeyError, TypeError, IndexError) as error:
        print("MySQL export refused:", path, "-", error,
              "\nRestore the previous definition. For an intentional incompatible change, back up your data,",
              "manually delete this table's SQL history file and rebuild the table.")
    except OSError as error:
        print("MySQL export failed:", error)
    return False
