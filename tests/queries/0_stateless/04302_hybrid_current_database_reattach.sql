-- Reproduce the Hybrid currentDatabase() metadata bug without a server restart.
-- Detaching the table and re-attaching it while a different database is current
-- mimics the missing session-database context that startup ATTACH has. Before the
-- fix the stored metadata keeps currentDatabase(), which then resolves to the wrong
-- database and the segment fails to attach with UNKNOWN_TABLE.

SET allow_experimental_hybrid_table = 1;

CREATE TABLE {CLICKHOUSE_DATABASE:Identifier}.local_hot (ts DateTime, value UInt64) ENGINE = MergeTree ORDER BY ts;
CREATE TABLE {CLICKHOUSE_DATABASE:Identifier}.local_cold (ts DateTime, value UInt64) ENGINE = MergeTree ORDER BY ts;
INSERT INTO {CLICKHOUSE_DATABASE:Identifier}.local_hot VALUES ('2025-10-15', 1), ('2025-11-01', 2);
INSERT INTO {CLICKHOUSE_DATABASE:Identifier}.local_cold VALUES ('2025-08-01', 3), ('2025-06-15', 4);

-- Create with the test database as current so currentDatabase() resolves to it.
USE {CLICKHOUSE_DATABASE:Identifier};
CREATE TABLE {CLICKHOUSE_DATABASE:Identifier}.hybrid_t (ts DateTime, value UInt64)
ENGINE = Hybrid(
    remote('localhost:9000', {CLICKHOUSE_DATABASE:String}, 'local_hot'), ts > '2025-09-01',
    remote('localhost:9000', currentDatabase(), 'local_cold'), ts <= '2025-09-01'
);

SELECT count() FROM {CLICKHOUSE_DATABASE:Identifier}.hybrid_t;

-- Same table through the DATABASE() alias of currentDatabase(): the metadata rewrite
-- has to canonicalize the function name instead of comparing it literally.
CREATE TABLE {CLICKHOUSE_DATABASE:Identifier}.hybrid_alias_t (ts DateTime, value UInt64)
ENGINE = Hybrid(
    remote('localhost:9000', {CLICKHOUSE_DATABASE:String}, 'local_hot'), ts > '2025-09-01',
    remote('localhost:9000', DATABASE(), 'local_cold'), ts <= '2025-09-01'
);

SELECT count() FROM {CLICKHOUSE_DATABASE:Identifier}.hybrid_alias_t;

-- Detach, switch the current database, then re-attach. currentDatabase() in the
-- stored metadata now resolves against the other database, whose local_cold does
-- not exist. With the fix the metadata holds the resolved name, so this succeeds.
DETACH TABLE {CLICKHOUSE_DATABASE:Identifier}.hybrid_t;
DETACH TABLE {CLICKHOUSE_DATABASE:Identifier}.hybrid_alias_t;
CREATE DATABASE IF NOT EXISTS {CLICKHOUSE_DATABASE_1:Identifier};
USE {CLICKHOUSE_DATABASE_1:Identifier};
ATTACH TABLE {CLICKHOUSE_DATABASE:Identifier}.hybrid_t;
ATTACH TABLE {CLICKHOUSE_DATABASE:Identifier}.hybrid_alias_t;

SELECT count() FROM {CLICKHOUSE_DATABASE:Identifier}.hybrid_t;
SELECT count() FROM {CLICKHOUSE_DATABASE:Identifier}.hybrid_alias_t;

USE {CLICKHOUSE_DATABASE:Identifier};
DROP DATABASE {CLICKHOUSE_DATABASE_1:Identifier};
