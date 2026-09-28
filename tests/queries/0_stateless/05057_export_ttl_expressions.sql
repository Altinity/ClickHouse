-- Tags: no-fasttest
-- no-fasttest: the destinations are S3 tables.

-- The expressions an `EXPORT` TTL accepts, and the date and time columns it may widen when exporting.

DROP TABLE IF EXISTS export_ttl_expressions_source;
DROP TABLE IF EXISTS export_ttl_expressions_random;
DROP TABLE IF EXISTS export_ttl_expressions_destination;

SET allow_experimental_export_ttl = 1;

CREATE TABLE export_ttl_expressions_destination (id UInt64, d Date, retention UInt16, t DateTime, nd Nullable(Date))
ENGINE = S3(s3_conn, filename = 'export_ttl_expressions_destination', format = Parquet, partition_strategy = 'hive')
PARTITION BY d;

-- An interval read from a column.
CREATE TABLE export_ttl_expressions_source (id UInt64, d Date, retention UInt16, t DateTime, nd Nullable(Date))
ENGINE = MergeTree PARTITION BY d ORDER BY id
TTL d + toIntervalDay(retention) EXPORT TO TABLE export_ttl_expressions_destination;
SELECT extract(create_table_query, 'TTL .*? SETTINGS') FROM system.tables WHERE database = currentDatabase() AND name = 'export_ttl_expressions_source';

-- Functions of one or several columns.
ALTER TABLE export_ttl_expressions_source MODIFY TTL toStartOfMonth(d) + INTERVAL 1 MONTH EXPORT TO TABLE export_ttl_expressions_destination;
SELECT extract(create_table_query, 'TTL .*? SETTINGS') FROM system.tables WHERE database = currentDatabase() AND name = 'export_ttl_expressions_source';

ALTER TABLE export_ttl_expressions_source MODIFY TTL greatest(t, toDateTime(d)) + INTERVAL 1 DAY EXPORT TO TABLE export_ttl_expressions_destination;
SELECT extract(create_table_query, 'TTL .*? SETTINGS') FROM system.tables WHERE database = currentDatabase() AND name = 'export_ttl_expressions_source';

ALTER TABLE export_ttl_expressions_source MODIFY TTL ifNull(nd, d) + INTERVAL 1 DAY EXPORT TO TABLE export_ttl_expressions_destination;
SELECT extract(create_table_query, 'TTL .*? SETTINGS') FROM system.tables WHERE database = currentDatabase() AND name = 'export_ttl_expressions_source';

-- The result must be a date or a time, not a `Nullable` one.
ALTER TABLE export_ttl_expressions_source MODIFY TTL nd + INTERVAL 1 DAY EXPORT TO TABLE export_ttl_expressions_destination; -- { serverError BAD_TTL_EXPRESSION }

-- The expression must be a deterministic function of the columns.
CREATE TABLE export_ttl_expressions_random (id UInt64, d Date, retention UInt16, t DateTime, nd Nullable(Date))
ENGINE = MergeTree PARTITION BY d ORDER BY id
TTL t + toIntervalSecond(rand() % 10) EXPORT TO TABLE export_ttl_expressions_destination; -- { serverError BAD_ARGUMENTS }
ALTER TABLE export_ttl_expressions_source MODIFY TTL t + toIntervalSecond(rand() % 10) EXPORT TO TABLE export_ttl_expressions_destination; -- { serverError BAD_ARGUMENTS }
ALTER TABLE export_ttl_expressions_source MODIFY TTL now() + INTERVAL 1 DAY EXPORT TO TABLE export_ttl_expressions_destination; -- { serverError BAD_ARGUMENTS }

-- Even when suspicious TTL expressions are allowed, which still applies to the TTL that does not export.
SET allow_suspicious_ttl_expressions = 1;
CREATE TABLE export_ttl_expressions_random (id UInt64, d Date, retention UInt16, t DateTime, nd Nullable(Date))
ENGINE = MergeTree PARTITION BY d ORDER BY id
TTL t + toIntervalSecond(rand() % 10) EXPORT TO TABLE export_ttl_expressions_destination; -- { serverError BAD_ARGUMENTS }
ALTER TABLE export_ttl_expressions_source MODIFY TTL t + toIntervalSecond(rand() % 10) EXPORT TO TABLE export_ttl_expressions_destination; -- { serverError BAD_ARGUMENTS }
ALTER TABLE export_ttl_expressions_source MODIFY TTL now() + INTERVAL 1 DAY EXPORT TO TABLE export_ttl_expressions_destination; -- { serverError BAD_ARGUMENTS }
ALTER TABLE export_ttl_expressions_source MODIFY TTL ifNull(nd, d) + INTERVAL 1 DAY EXPORT TO TABLE export_ttl_expressions_destination, t + toIntervalSecond(rand() % 10) DELETE;
SELECT extract(create_table_query, 'TTL .*? SETTINGS') FROM system.tables WHERE database = currentDatabase() AND name = 'export_ttl_expressions_source';
SET allow_suspicious_ttl_expressions = 0;

DROP TABLE export_ttl_expressions_source;
DROP TABLE export_ttl_expressions_destination;

-- The `EXPORT` TTL allows lossy casts, whatever `export_merge_tree_part_allow_lossy_cast` is, e.g.
-- nanoseconds to the microseconds of an Iceberg `timestamp`, or an unsigned column to a signed one.
DROP TABLE IF EXISTS export_ttl_expressions_micros;
DROP TABLE IF EXISTS export_ttl_expressions_nanos;
DROP TABLE IF EXISTS export_ttl_expressions_signed;
DROP TABLE IF EXISTS export_ttl_expressions_millis_to_micros;
DROP TABLE IF EXISTS export_ttl_expressions_nanos_to_micros;
DROP TABLE IF EXISTS export_ttl_expressions_millis_to_nanos;
DROP TABLE IF EXISTS export_ttl_expressions_unsigned_to_signed;

SET export_merge_tree_part_allow_lossy_cast = 0;

CREATE TABLE export_ttl_expressions_micros (id UInt64, d Date, ts DateTime64(6))
ENGINE = S3(s3_conn, filename = 'export_ttl_expressions_micros', format = Parquet, partition_strategy = 'hive')
PARTITION BY d;

CREATE TABLE export_ttl_expressions_nanos (id UInt64, d Date, ts DateTime64(9))
ENGINE = S3(s3_conn, filename = 'export_ttl_expressions_nanos', format = Parquet, partition_strategy = 'hive')
PARTITION BY d;

CREATE TABLE export_ttl_expressions_signed (id Int32, d Date, ts DateTime)
ENGINE = S3(s3_conn, filename = 'export_ttl_expressions_signed', format = Parquet, partition_strategy = 'hive')
PARTITION BY d;

CREATE TABLE export_ttl_expressions_millis_to_micros (id UInt64, d Date, ts DateTime64(3)) ENGINE = MergeTree PARTITION BY d ORDER BY id
TTL ts + INTERVAL 1 DAY EXPORT TO TABLE export_ttl_expressions_micros;

CREATE TABLE export_ttl_expressions_nanos_to_micros (id UInt64, d Date, ts DateTime64(9)) ENGINE = MergeTree PARTITION BY d ORDER BY id
TTL ts + INTERVAL 1 DAY EXPORT TO TABLE export_ttl_expressions_micros;

CREATE TABLE export_ttl_expressions_millis_to_nanos (id UInt64, d Date, ts DateTime64(3)) ENGINE = MergeTree PARTITION BY d ORDER BY id
TTL ts + INTERVAL 1 DAY EXPORT TO TABLE export_ttl_expressions_nanos;

CREATE TABLE export_ttl_expressions_unsigned_to_signed (id UInt32, d Date, ts DateTime) ENGINE = MergeTree PARTITION BY d ORDER BY id
TTL ts + INTERVAL 1 DAY EXPORT TO TABLE export_ttl_expressions_signed;

SELECT name FROM system.tables WHERE database = currentDatabase() AND match(name, '_to_') ORDER BY name;

DROP TABLE export_ttl_expressions_millis_to_micros;
DROP TABLE export_ttl_expressions_nanos_to_micros;
DROP TABLE export_ttl_expressions_millis_to_nanos;
DROP TABLE export_ttl_expressions_unsigned_to_signed;
DROP TABLE export_ttl_expressions_micros;
DROP TABLE export_ttl_expressions_nanos;
DROP TABLE export_ttl_expressions_signed;
