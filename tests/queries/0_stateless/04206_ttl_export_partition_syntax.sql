-- Tags: zookeeper, no-replicated-database
-- Parser and metadata round-trip for `TTL ... EXPORT TO TABLE db.table`, plus validation.

DROP TABLE IF EXISTS ttl_export_src SYNC;
DROP TABLE IF EXISTS ttl_export_dst SYNC;

CREATE TABLE ttl_export_dst (event_date Date, id UInt64)
ENGINE = MergeTree() PARTITION BY toYear(event_date) ORDER BY tuple();

-- 1. CREATE TABLE with an EXPORT TTL round-trips through system.tables.
CREATE TABLE ttl_export_src (event_date Date, id UInt64)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/ttl_export_src', 'r1')
PARTITION BY toYear(event_date)
ORDER BY id
TTL event_date + INTERVAL 7 DAY EXPORT TO TABLE ttl_export_dst;

SELECT replaceRegexpOne(extract(create_table_query, 'TTL [^\n]+'), ' SETTINGS .*$', '') FROM system.tables
WHERE database = currentDatabase() AND name = 'ttl_export_src';

DROP TABLE ttl_export_src SYNC;

-- 2. ALTER MODIFY TTL adds EXPORT alongside DELETE.
CREATE TABLE ttl_export_src (event_date Date, id UInt64)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/ttl_export_src', 'r1')
PARTITION BY toYear(event_date)
ORDER BY id;

ALTER TABLE ttl_export_src MODIFY TTL
    event_date + INTERVAL 7 DAY EXPORT TO TABLE ttl_export_dst,
    event_date + INTERVAL 30 DAY DELETE;

SELECT replaceRegexpOne(extract(create_table_query, 'TTL [^\n]+'), ' SETTINGS .*$', '') FROM system.tables
WHERE database = currentDatabase() AND name = 'ttl_export_src';

DROP TABLE ttl_export_src SYNC;

-- 3. Two EXPORT TTLs to the same destination must be rejected.
CREATE TABLE ttl_export_src (event_date Date, id UInt64)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/ttl_export_src', 'r1')
PARTITION BY toYear(event_date)
ORDER BY id
TTL
    event_date + INTERVAL 1 DAY EXPORT TO TABLE ttl_export_dst,
    event_date + INTERVAL 7 DAY EXPORT TO TABLE ttl_export_dst; -- { serverError BAD_ARGUMENTS }

-- 4. EXPORT TTL on a table without a partition key must be rejected.
CREATE TABLE ttl_export_src (event_date Date, id UInt64)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/ttl_export_src_nopk', 'r1')
ORDER BY id
TTL event_date + INTERVAL 7 DAY EXPORT TO TABLE ttl_export_dst; -- { serverError BAD_ARGUMENTS }

DROP TABLE IF EXISTS ttl_export_dst SYNC;
