-- Tags: no-fasttest, no-cas-storage
-- no-fasttest: the destination is an S3 table.
-- no-cas-storage: the EXPORT TTL of a plain MergeTree is not supported on a content-addressed disk.

SELECT formatQuery('CREATE TABLE t (d DateTime) ENGINE = MergeTree ORDER BY d TTL d + INTERVAL 1 DAY EXPORT TO TABLE dst, d + INTERVAL 2 DAY DELETE');
SELECT formatQuery('ALTER TABLE t MODIFY TTL d + INTERVAL 1 DAY EXPORT TO TABLE db.dst');
SELECT formatQuery('ALTER TABLE t MODIFY TTL d EXPORT TO TABLE `my db`.`my.table`');
SELECT formatQuery('ALTER TABLE t MODIFY TTL d EXPORT TO TABLE dst WHERE 1'); -- { serverError SYNTAX_ERROR }
SELECT formatQuery('ALTER TABLE t MODIFY TTL d EXPORT dst'); -- { serverError SYNTAX_ERROR }

DROP TABLE IF EXISTS export_ttl_source;
DROP TABLE IF EXISTS export_ttl_destination;
DROP TABLE IF EXISTS export_ttl_memory;
DROP TABLE IF EXISTS export_ttl_narrow;

CREATE TABLE export_ttl_destination (id UInt64, year UInt16, t DateTime)
ENGINE = S3(s3_conn, filename = 'export_ttl_syntax_destination', format = Parquet, partition_strategy = 'hive')
PARTITION BY year;

CREATE TABLE export_ttl_memory (id UInt64, year UInt16, t DateTime) ENGINE = Memory;

CREATE TABLE export_ttl_narrow (id UInt64, year UInt16)
ENGINE = S3(s3_conn, filename = 'export_ttl_syntax_narrow', format = Parquet, partition_strategy = 'hive')
PARTITION BY year;

SET allow_experimental_export_ttl = 0;
CREATE TABLE export_ttl_source (id UInt64, year UInt16, t DateTime) ENGINE = MergeTree PARTITION BY year ORDER BY id
TTL t + INTERVAL 10 YEAR EXPORT TO TABLE export_ttl_destination; -- { serverError SUPPORT_IS_DISABLED }

SET allow_experimental_export_ttl = 1;

CREATE TABLE export_ttl_source (id UInt64, year UInt16, t DateTime) ENGINE = MergeTree PARTITION BY year ORDER BY id
TTL t + INTERVAL 10 YEAR EXPORT TO TABLE export_ttl_missing; -- { serverError BAD_TTL_EXPRESSION }

CREATE TABLE export_ttl_source (id UInt64, year UInt16, t DateTime) ENGINE = MergeTree PARTITION BY year ORDER BY id
TTL t + INTERVAL 10 YEAR EXPORT TO TABLE export_ttl_memory; -- { serverError BAD_TTL_EXPRESSION }

CREATE TABLE export_ttl_source (id UInt64, year UInt16, t DateTime) ENGINE = MergeTree PARTITION BY year ORDER BY id
TTL t + INTERVAL 10 YEAR EXPORT TO TABLE export_ttl_narrow; -- { serverError NUMBER_OF_COLUMNS_DOESNT_MATCH }

CREATE TABLE export_ttl_source (id UInt64, year UInt16, t DateTime) ENGINE = MergeTree PARTITION BY year ORDER BY id
TTL id EXPORT TO TABLE export_ttl_destination; -- { serverError BAD_TTL_EXPRESSION }

CREATE TABLE export_ttl_source (id UInt64, year UInt16, t DateTime) ENGINE = MergeTree PARTITION BY year ORDER BY id
TTL t + INTERVAL 10 YEAR EXPORT TO TABLE export_ttl_destination, t + INTERVAL 20 YEAR EXPORT TO TABLE export_ttl_destination; -- { serverError BAD_TTL_EXPRESSION }

CREATE TABLE export_ttl_source (id UInt64, year UInt16, t DateTime) ENGINE = MergeTree PARTITION BY year ORDER BY id
TTL t + INTERVAL 10 YEAR EXPORT TO TABLE export_ttl_destination, t + INTERVAL 20 YEAR DELETE;

SELECT extract(create_table_query, 'TTL .*? SETTINGS') FROM system.tables WHERE database = currentDatabase() AND name = 'export_ttl_source';

-- The table cannot export to itself.
ALTER TABLE export_ttl_source MODIFY TTL t + INTERVAL 10 YEAR EXPORT TO TABLE export_ttl_source; -- { serverError BAD_TTL_EXPRESSION }
ALTER TABLE export_ttl_source MODIFY TTL t + INTERVAL 5 YEAR EXPORT TO TABLE export_ttl_destination;
SELECT extract(create_table_query, 'TTL .*? SETTINGS') FROM system.tables WHERE database = currentDatabase() AND name = 'export_ttl_source';

-- Nothing is due, so nothing is exported.
INSERT INTO export_ttl_source VALUES (1, 2020, now());
SELECT count() FROM system.distributed_exports WHERE source_database = currentDatabase() AND source_table = 'export_ttl_source';

ALTER TABLE export_ttl_source REMOVE TTL;
SELECT extract(create_table_query, 'TTL .*? SETTINGS') FROM system.tables WHERE database = currentDatabase() AND name = 'export_ttl_source';

DROP TABLE export_ttl_source;

-- A destination partition key that can never be proven to be single-valued over a group is refused.
DROP TABLE IF EXISTS export_ttl_monthly;
DROP TABLE IF EXISTS export_ttl_by_id;
DROP TABLE IF EXISTS export_ttl_by_year;
DROP TABLE IF EXISTS export_ttl_by_day_of_week;

CREATE TABLE export_ttl_by_id (id UInt64, year UInt16, t DateTime)
ENGINE = S3(s3_conn, filename = 'export_ttl_syntax_by_id', format = Parquet, partition_strategy = 'hive')
PARTITION BY id;

CREATE TABLE export_ttl_by_year (id UInt64, year UInt16, t DateTime)
ENGINE = S3(s3_conn, filename = 'export_ttl_syntax_by_year/{_partition_id}/{_file}.parquet', format = Parquet, partition_strategy = 'wildcard')
PARTITION BY toYear(t);

CREATE TABLE export_ttl_by_day_of_week (id UInt64, year UInt16, t DateTime)
ENGINE = S3(s3_conn, filename = 'export_ttl_syntax_by_day_of_week/{_partition_id}/{_file}.parquet', format = Parquet, partition_strategy = 'wildcard')
PARTITION BY toDayOfWeek(t);

-- `id` is not in the source partition key.
CREATE TABLE export_ttl_monthly (id UInt64, year UInt16, t DateTime) ENGINE = MergeTree PARTITION BY toYYYYMM(t) ORDER BY id
TTL t + INTERVAL 10 YEAR EXPORT TO TABLE export_ttl_by_id; -- { serverError BAD_ARGUMENTS }

-- `toDayOfWeek` is not monotonic.
CREATE TABLE export_ttl_monthly (id UInt64, year UInt16, t DateTime) ENGINE = MergeTree PARTITION BY toYYYYMM(t) ORDER BY id
TTL t + INTERVAL 10 YEAR EXPORT TO TABLE export_ttl_by_day_of_week; -- { serverError BAD_ARGUMENTS }

-- An unpartitioned source has no partition key to prove anything from.
CREATE TABLE export_ttl_monthly (id UInt64, year UInt16, t DateTime) ENGINE = MergeTree ORDER BY id
TTL t + INTERVAL 10 YEAR EXPORT TO TABLE export_ttl_destination; -- { serverError BAD_ARGUMENTS }

-- A month is within a year, which is proven from the parts when they are exported.
CREATE TABLE export_ttl_monthly (id UInt64, year UInt16, t DateTime) ENGINE = MergeTree PARTITION BY toYYYYMM(t) ORDER BY id
TTL t + INTERVAL 10 YEAR EXPORT TO TABLE export_ttl_by_year;
SELECT extract(create_table_query, 'TTL .*? SETTINGS') FROM system.tables WHERE database = currentDatabase() AND name = 'export_ttl_monthly';

ALTER TABLE export_ttl_monthly MODIFY TTL t + INTERVAL 10 YEAR EXPORT TO TABLE export_ttl_by_id; -- { serverError BAD_ARGUMENTS }
SELECT extract(create_table_query, 'TTL .*? SETTINGS') FROM system.tables WHERE database = currentDatabase() AND name = 'export_ttl_monthly';

DROP TABLE export_ttl_monthly;
DROP TABLE export_ttl_by_id;
DROP TABLE export_ttl_by_year;
DROP TABLE export_ttl_by_day_of_week;
DROP TABLE export_ttl_destination;
DROP TABLE export_ttl_memory;
DROP TABLE export_ttl_narrow;
