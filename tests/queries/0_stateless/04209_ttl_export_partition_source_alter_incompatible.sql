-- Tags: zookeeper, no-replicated-database
-- `ALTER TABLE source` that breaks compatibility with an existing `TTL ... EXPORT TO TABLE`
-- destination must fail at DDL time. `AlterCommands::apply` re-parses the table TTL against
-- the post-ALTER columns and reruns `verifyExportDestinationCompatibility` for every EXPORT
-- clause, which rejects the column mismatch up front.

DROP TABLE IF EXISTS ttl_export_src SYNC;
DROP TABLE IF EXISTS ttl_export_dst SYNC;

CREATE TABLE ttl_export_dst (event_date Date, id UInt64)
ENGINE = MergeTree() PARTITION BY toYear(event_date) ORDER BY tuple();

CREATE TABLE ttl_export_src (event_date Date, id UInt64)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/ttl_export_src', 'r1')
PARTITION BY toYear(event_date)
ORDER BY id
TTL event_date + INTERVAL 7 DAY EXPORT TO TABLE ttl_export_dst;

-- ADD COLUMN to the source: post-ALTER source columns no longer match destination's
-- insertable columns, so the TTL re-parse rejects the ALTER.
ALTER TABLE ttl_export_src ADD COLUMN extra String; -- { serverError INCOMPATIBLE_COLUMNS }

-- The ALTER failed; source schema is unchanged. Subsequent inserts still match destination.
INSERT INTO ttl_export_src VALUES (today(), 1);
SELECT count() FROM ttl_export_src;

DROP TABLE ttl_export_src SYNC;
DROP TABLE ttl_export_dst SYNC;
