-- oi-mysql-history-v1 [{"columns":[{"name":"name","sql":"VARCHAR(64)","seq":0,"type":"std::string","json":false,"auto":false},{"name":"id","sql":"INT","seq":1,"type":"int32_t","json":false,"auto":false},{"name":"ip","sql":"VARCHAR(64)","seq":2,"type":"std::string","json":false,"auto":false},{"name":"port","sql":"VARCHAR(8)","seq":3,"type":"std::string","json":false,"auto":false},{"name":"zone","sql":"VARCHAR(32)","seq":4,"type":"std::string","json":false,"auto":false}],"primary":["name","id"],"indexes":{}}]
-- oi-mysql-sha256 df3f698060d04d70017ccc32a53ebe4a9718ba592ddb631a7643c16c62add997
-- Import into the target database with mysql. Keep this file under version control.
CREATE TABLE IF NOT EXISTS `servicelease` (
    `name` VARCHAR(64) NOT NULL,
    `id` INT NOT NULL,
    `ip` VARCHAR(64) NOT NULL,
    `port` VARCHAR(8) NOT NULL,
    `zone` VARCHAR(32) NOT NULL,
    PRIMARY KEY (`name`, `id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Validate existing definitions before executing any ALTER.
DROP TEMPORARY TABLE IF EXISTS _oi_expected_columns;
CREATE TEMPORARY TABLE _oi_expected_columns (name VARBINARY(64) PRIMARY KEY, sql_type VARCHAR(64), auto_id BOOL);
INSERT INTO _oi_expected_columns VALUES
    ('name', 'varchar(64)', 0),
    ('id', 'int', 0),
    ('ip', 'varchar(64)', 0),
    ('port', 'varchar(8)', 0),
    ('zone', 'varchar(32)', 0);
SET @oi_error = (SELECT CONCAT('column ', c.COLUMN_NAME, ' differs from protocol')
    FROM information_schema.COLUMNS c LEFT JOIN _oi_expected_columns e ON e.name = BINARY c.COLUMN_NAME
    WHERE c.TABLE_SCHEMA = DATABASE() AND c.TABLE_NAME = 'servicelease'
      AND (e.name IS NULL OR e.sql_type <> c.COLUMN_TYPE OR c.IS_NULLABLE <> 'NO'
           OR (e.auto_id AND c.EXTRA <> 'auto_increment')
           OR (NOT e.auto_id AND c.EXTRA NOT IN ('', 'DEFAULT_GENERATED'))) LIMIT 1);
DROP TEMPORARY TABLE _oi_expected_columns;
SET @oi_error = COALESCE(@oi_error, IF(EXISTS (SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'servicelease' AND (TABLE_TYPE <> 'BASE TABLE' OR ENGINE <> 'InnoDB')), 'expected an InnoDB table', NULL));
SET @oi_error = COALESCE(@oi_error, IF(NOT EXISTS (SELECT 1 FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'servicelease' AND INDEX_NAME = 'PRIMARY') OR EXISTS (SELECT 1 FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'servicelease' AND INDEX_NAME = 'PRIMARY' GROUP BY INDEX_NAME
        HAVING COUNT(*) <> 2 OR SUM(CASE WHEN ((SEQ_IN_INDEX = 1 AND BINARY COLUMN_NAME = 'name') OR (SEQ_IN_INDEX = 2 AND BINARY COLUMN_NAME = 'id')) AND NON_UNIQUE = 0
            AND SUB_PART IS NULL AND COLLATION = 'A' AND INDEX_TYPE = 'BTREE' AND IS_VISIBLE = 'YES'
            THEN 1 ELSE 0 END) <> 2), 'primary key differs from protocol', NULL));
SELECT CONCAT('servicelease: ', @oi_error, '. Back up and rebuild the table manually.') AS migration_error WHERE @oi_error IS NOT NULL;
DROP TEMPORARY TABLE IF EXISTS _oi_migration_guard;
CREATE TEMPORARY TABLE _oi_migration_guard (issue VARCHAR(255), CHECK (issue IS NULL));
INSERT INTO _oi_migration_guard VALUES (@oi_error);
DROP TEMPORARY TABLE _oi_migration_guard;

-- Accumulate missing columns and indexes across versions, then apply one atomic InnoDB ALTER.
SET @oi_changes = NULL;
-- v1: columns [name, id, ip, port, zone], indexes []
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'servicelease' AND COLUMN_NAME = 'ip'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `ip` VARCHAR(64) NOT NULL AFTER `id`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'servicelease' AND COLUMN_NAME = 'port'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `port` VARCHAR(8) NOT NULL AFTER `ip`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'servicelease' AND COLUMN_NAME = 'zone'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `zone` VARCHAR(32) NOT NULL AFTER `port`'));
SET @oi_sql = IF(@oi_error IS NULL AND @oi_changes IS NOT NULL,
    CONCAT('ALTER TABLE `servicelease` ', @oi_changes), 'DO 0');
PREPARE oi_migration FROM @oi_sql;
EXECUTE oi_migration;
DEALLOCATE PREPARE oi_migration;
