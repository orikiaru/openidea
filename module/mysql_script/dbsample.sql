-- oi-mysql-history-v1 [{"columns":[{"name":"id","sql":"BIGINT","seq":0,"type":"int64_t","json":false,"auto":true},{"name":"sName","sql":"VARBINARY(64)","seq":1,"type":"std::string","json":false,"auto":false},{"name":"iSecret","sql":"INT","seq":2,"type":"int32_t","json":false,"auto":false},{"name":"iToken","sql":"BIGINT UNSIGNED","seq":4,"type":"uint64_t","json":false,"auto":false},{"name":"dScore","sql":"DOUBLE","seq":5,"type":"double","json":false,"auto":false},{"name":"fRatio","sql":"DOUBLE","seq":6,"type":"float","json":false,"auto":false},{"name":"stPayload","sql":"LONGBLOB","seq":7,"type":"Bytes","json":false,"auto":false},{"name":"vNumbers","sql":"LONGBLOB","seq":8,"type":"std::vector<int32_t>","json":false,"auto":false},{"name":"mFlags","sql":"LONGBLOB","seq":9,"type":"std::map<std::string, int32_t>","json":false,"auto":false},{"name":"stChild","sql":"LONGBLOB","seq":10,"type":"DbChild","json":false,"auto":false},{"name":"iFlag","sql":"TINYINT","seq":11,"type":"int8_t","json":false,"auto":false},{"name":"bActive","sql":"TINYINT","seq":12,"type":"bool","json":false,"auto":false},{"name":"vJsonNumbers","sql":"LONGTEXT","seq":13,"type":"std::vector<int32_t>","json":true,"auto":false},{"name":"notes","sql":"TEXT","seq":14,"type":"std::map<std::string, int32_t>","json":true,"auto":false},{"name":"stJsonChild","sql":"LONGTEXT","seq":15,"type":"DbChild","json":true,"auto":false},{"name":"iMask","sql":"TINYINT UNSIGNED","seq":16,"type":"uint8_t","json":false,"auto":false},{"name":"iOffset","sql":"SMALLINT","seq":17,"type":"int16_t","json":false,"auto":false},{"name":"iPort","sql":"SMALLINT UNSIGNED","seq":18,"type":"uint16_t","json":false,"auto":false},{"name":"iCount","sql":"INT UNSIGNED","seq":19,"type":"uint32_t","json":false,"auto":false},{"name":"iBalance","sql":"BIGINT","seq":20,"type":"int64_t","json":false,"auto":false},{"name":"sCode","sql":"CHAR(4)","seq":21,"type":"std::string","json":false,"auto":false},{"name":"sLabel","sql":"VARCHAR(16)","seq":22,"type":"std::string","json":false,"auto":false},{"name":"sFixed","sql":"BINARY(3)","seq":23,"type":"std::string","json":false,"auto":false},{"name":"sDescription","sql":"MEDIUMTEXT","seq":24,"type":"std::string","json":false,"auto":false},{"name":"sArticle","sql":"LONGTEXT","seq":25,"type":"std::string","json":false,"auto":false},{"name":"stData","sql":"BLOB","seq":26,"type":"Bytes","json":false,"auto":false},{"name":"stArchive","sql":"MEDIUMBLOB","seq":27,"type":"Bytes","json":false,"auto":false},{"name":"mGroups","sql":"LONGBLOB","seq":28,"type":"std::map<std::string, std::vector<int32_t>>","json":false,"auto":false},{"name":"vJsonGroups","sql":"LONGTEXT","seq":29,"type":"std::vector<std::map<std::string, int32_t>>","json":true,"auto":false}],"primary":["id"],"indexes":{"idx_name":{"unique":false,"columns":["sName"]},"idx_code_id":{"unique":false,"columns":["sCode","id"]}}}]
-- oi-mysql-sha256 3748b08bb53bfaec7fd0a426f96bf2d81de16610b5bd8caa6c5ffb76e7538271
-- Import into the target database with mysql. Keep this file under version control.
CREATE TABLE IF NOT EXISTS `dbsample` (
    `id` BIGINT NOT NULL AUTO_INCREMENT,
    `sName` VARBINARY(64) NOT NULL,
    `iSecret` INT NOT NULL,
    `iToken` BIGINT UNSIGNED NOT NULL,
    `dScore` DOUBLE NOT NULL,
    `fRatio` DOUBLE NOT NULL,
    `stPayload` LONGBLOB NOT NULL,
    `vNumbers` LONGBLOB NOT NULL DEFAULT (0x000000000400000000000000),
    `mFlags` LONGBLOB NOT NULL DEFAULT (0x000000000400000000000000),
    `stChild` LONGBLOB NOT NULL DEFAULT (0x0000000000000000),
    `iFlag` TINYINT NOT NULL,
    `bActive` TINYINT NOT NULL,
    `vJsonNumbers` LONGTEXT NOT NULL DEFAULT (0x5B5D),
    `notes` TEXT NOT NULL DEFAULT (0x7B7D),
    `stJsonChild` LONGTEXT NOT NULL DEFAULT (0x7B7D),
    `iMask` TINYINT UNSIGNED NOT NULL,
    `iOffset` SMALLINT NOT NULL,
    `iPort` SMALLINT UNSIGNED NOT NULL,
    `iCount` INT UNSIGNED NOT NULL,
    `iBalance` BIGINT NOT NULL,
    `sCode` CHAR(4) NOT NULL,
    `sLabel` VARCHAR(16) NOT NULL,
    `sFixed` BINARY(3) NOT NULL,
    `sDescription` MEDIUMTEXT NOT NULL,
    `sArticle` LONGTEXT NOT NULL,
    `stData` BLOB NOT NULL,
    `stArchive` MEDIUMBLOB NOT NULL,
    `mGroups` LONGBLOB NOT NULL DEFAULT (0x000000000400000000000000),
    `vJsonGroups` LONGTEXT NOT NULL DEFAULT (0x5B5D),
    PRIMARY KEY (`id`),
    INDEX `idx_name` (`sName`),
    INDEX `idx_code_id` (`sCode`, `id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Validate existing definitions before executing any ALTER.
DROP TEMPORARY TABLE IF EXISTS _oi_expected_columns;
CREATE TEMPORARY TABLE _oi_expected_columns (name VARBINARY(64) PRIMARY KEY, sql_type VARCHAR(64), auto_id BOOL);
INSERT INTO _oi_expected_columns VALUES
    ('id', 'bigint', 1),
    ('sName', 'varbinary(64)', 0),
    ('iSecret', 'int', 0),
    ('iToken', 'bigint unsigned', 0),
    ('dScore', 'double', 0),
    ('fRatio', 'double', 0),
    ('stPayload', 'longblob', 0),
    ('vNumbers', 'longblob', 0),
    ('mFlags', 'longblob', 0),
    ('stChild', 'longblob', 0),
    ('iFlag', 'tinyint', 0),
    ('bActive', 'tinyint', 0),
    ('vJsonNumbers', 'longtext', 0),
    ('notes', 'text', 0),
    ('stJsonChild', 'longtext', 0),
    ('iMask', 'tinyint unsigned', 0),
    ('iOffset', 'smallint', 0),
    ('iPort', 'smallint unsigned', 0),
    ('iCount', 'int unsigned', 0),
    ('iBalance', 'bigint', 0),
    ('sCode', 'char(4)', 0),
    ('sLabel', 'varchar(16)', 0),
    ('sFixed', 'binary(3)', 0),
    ('sDescription', 'mediumtext', 0),
    ('sArticle', 'longtext', 0),
    ('stData', 'blob', 0),
    ('stArchive', 'mediumblob', 0),
    ('mGroups', 'longblob', 0),
    ('vJsonGroups', 'longtext', 0);
SET @oi_error = (SELECT CONCAT('column ', c.COLUMN_NAME, ' differs from protocol')
    FROM information_schema.COLUMNS c LEFT JOIN _oi_expected_columns e ON e.name = BINARY c.COLUMN_NAME
    WHERE c.TABLE_SCHEMA = DATABASE() AND c.TABLE_NAME = 'dbsample'
      AND (e.name IS NULL OR e.sql_type <> c.COLUMN_TYPE OR c.IS_NULLABLE <> 'NO'
           OR (e.auto_id AND c.EXTRA <> 'auto_increment')
           OR (NOT e.auto_id AND c.EXTRA NOT IN ('', 'DEFAULT_GENERATED'))) LIMIT 1);
DROP TEMPORARY TABLE _oi_expected_columns;
SET @oi_error = COALESCE(@oi_error, IF(EXISTS (SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND (TABLE_TYPE <> 'BASE TABLE' OR ENGINE <> 'InnoDB')), 'expected an InnoDB table', NULL));
SET @oi_error = COALESCE(@oi_error, IF(NOT EXISTS (SELECT 1 FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND INDEX_NAME = 'PRIMARY') OR EXISTS (SELECT 1 FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND INDEX_NAME = 'PRIMARY' GROUP BY INDEX_NAME
        HAVING COUNT(*) <> 1 OR SUM(CASE WHEN ((SEQ_IN_INDEX = 1 AND BINARY COLUMN_NAME = 'id')) AND NON_UNIQUE = 0
            AND SUB_PART IS NULL AND COLLATION = 'A' AND INDEX_TYPE = 'BTREE' AND IS_VISIBLE = 'YES'
            THEN 1 ELSE 0 END) <> 1), 'primary key differs from protocol', NULL));
SET @oi_error = COALESCE(@oi_error, IF(EXISTS (SELECT 1 FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND INDEX_NAME = 'idx_name' GROUP BY INDEX_NAME
        HAVING COUNT(*) <> 1 OR SUM(CASE WHEN ((SEQ_IN_INDEX = 1 AND BINARY COLUMN_NAME = 'sName')) AND NON_UNIQUE = 1
            AND SUB_PART IS NULL AND COLLATION = 'A' AND INDEX_TYPE = 'BTREE' AND IS_VISIBLE = 'YES'
            THEN 1 ELSE 0 END) <> 1), 'index idx_name differs from protocol', NULL));
SET @oi_error = COALESCE(@oi_error, IF(EXISTS (SELECT 1 FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND INDEX_NAME = 'idx_code_id' GROUP BY INDEX_NAME
        HAVING COUNT(*) <> 2 OR SUM(CASE WHEN ((SEQ_IN_INDEX = 1 AND BINARY COLUMN_NAME = 'sCode') OR (SEQ_IN_INDEX = 2 AND BINARY COLUMN_NAME = 'id')) AND NON_UNIQUE = 1
            AND SUB_PART IS NULL AND COLLATION = 'A' AND INDEX_TYPE = 'BTREE' AND IS_VISIBLE = 'YES'
            THEN 1 ELSE 0 END) <> 2), 'index idx_code_id differs from protocol', NULL));
SELECT CONCAT('dbsample: ', @oi_error, '. Back up and rebuild the table manually.') AS migration_error WHERE @oi_error IS NOT NULL;
DROP TEMPORARY TABLE IF EXISTS _oi_migration_guard;
CREATE TEMPORARY TABLE _oi_migration_guard (issue VARCHAR(255), CHECK (issue IS NULL));
INSERT INTO _oi_migration_guard VALUES (@oi_error);
DROP TEMPORARY TABLE _oi_migration_guard;

-- Accumulate missing columns and indexes across versions, then apply one atomic InnoDB ALTER.
SET @oi_changes = NULL;
-- v1: columns [id, sName, iSecret, iToken, dScore, fRatio, stPayload, vNumbers, mFlags, stChild, iFlag, bActive, vJsonNumbers, notes, stJsonChild, iMask, iOffset, iPort, iCount, iBalance, sCode, sLabel, sFixed, sDescription, sArticle, stData, stArchive, mGroups, vJsonGroups], indexes [idx_name, idx_code_id]
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'sName'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `sName` VARBINARY(64) NOT NULL AFTER `id`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'iSecret'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `iSecret` INT NOT NULL AFTER `sName`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'iToken'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `iToken` BIGINT UNSIGNED NOT NULL AFTER `iSecret`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'dScore'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `dScore` DOUBLE NOT NULL AFTER `iToken`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'fRatio'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `fRatio` DOUBLE NOT NULL AFTER `dScore`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'stPayload'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `stPayload` LONGBLOB NOT NULL AFTER `fRatio`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'vNumbers'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `vNumbers` LONGBLOB NOT NULL DEFAULT (0x000000000400000000000000) AFTER `stPayload`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'mFlags'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `mFlags` LONGBLOB NOT NULL DEFAULT (0x000000000400000000000000) AFTER `vNumbers`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'stChild'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `stChild` LONGBLOB NOT NULL DEFAULT (0x0000000000000000) AFTER `mFlags`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'iFlag'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `iFlag` TINYINT NOT NULL AFTER `stChild`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'bActive'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `bActive` TINYINT NOT NULL AFTER `iFlag`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'vJsonNumbers'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `vJsonNumbers` LONGTEXT NOT NULL DEFAULT (0x5B5D) AFTER `bActive`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'notes'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `notes` TEXT NOT NULL DEFAULT (0x7B7D) AFTER `vJsonNumbers`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'stJsonChild'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `stJsonChild` LONGTEXT NOT NULL DEFAULT (0x7B7D) AFTER `notes`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'iMask'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `iMask` TINYINT UNSIGNED NOT NULL AFTER `stJsonChild`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'iOffset'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `iOffset` SMALLINT NOT NULL AFTER `iMask`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'iPort'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `iPort` SMALLINT UNSIGNED NOT NULL AFTER `iOffset`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'iCount'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `iCount` INT UNSIGNED NOT NULL AFTER `iPort`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'iBalance'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `iBalance` BIGINT NOT NULL AFTER `iCount`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'sCode'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `sCode` CHAR(4) NOT NULL AFTER `iBalance`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'sLabel'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `sLabel` VARCHAR(16) NOT NULL AFTER `sCode`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'sFixed'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `sFixed` BINARY(3) NOT NULL AFTER `sLabel`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'sDescription'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `sDescription` MEDIUMTEXT NOT NULL AFTER `sFixed`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'sArticle'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `sArticle` LONGTEXT NOT NULL AFTER `sDescription`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'stData'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `stData` BLOB NOT NULL AFTER `sArticle`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'stArchive'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `stArchive` MEDIUMBLOB NOT NULL AFTER `stData`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'mGroups'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `mGroups` LONGBLOB NOT NULL DEFAULT (0x000000000400000000000000) AFTER `stArchive`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND COLUMN_NAME = 'vJsonGroups'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD COLUMN `vJsonGroups` LONGTEXT NOT NULL DEFAULT (0x5B5D) AFTER `mGroups`'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND INDEX_NAME = 'idx_name'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD INDEX `idx_name` (`sName`)'));
SET @oi_changes = IF(EXISTS (SELECT 1 FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dbsample' AND INDEX_NAME = 'idx_code_id'),
    @oi_changes, CONCAT_WS(', ', @oi_changes, 'ADD INDEX `idx_code_id` (`sCode`, `id`)'));
SET @oi_sql = IF(@oi_error IS NULL AND @oi_changes IS NOT NULL,
    CONCAT('ALTER TABLE `dbsample` ', @oi_changes), 'DO 0');
PREPARE oi_migration FROM @oi_sql;
EXECUTE oi_migration;
DEALLOCATE PREPARE oi_migration;
