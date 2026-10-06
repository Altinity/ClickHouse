-- Tags: no-fasttest, no-cas-storage
-- no-fasttest: the destinations are S3 tables.
-- no-cas-storage: the EXPORT TTL of a plain MergeTree is not supported on a content-addressed disk.

-- The partition key of the destination of an EXPORT TTL is checked when the expression is added:
-- a key that is a function of the source partition key (structural) or monotonic in a single column
-- of it (checked per group later) is accepted, any other is refused.

SET allow_experimental_export_ttl = 1;

DROP TABLE IF EXISTS src;
DROP TABLE IF EXISTS dst_year;
DROP TABLE IF EXISTS dst_region;
DROP TABLE IF EXISTS dst_ba;
DROP TABLE IF EXISTS dst_id;
DROP TABLE IF EXISTS dst_d;
DROP TABLE IF EXISTS dst_month;
DROP TABLE IF EXISTS dst_year_mod;
DROP TABLE IF EXISTS dst_hash;
DROP TABLE IF EXISTS dst_to_year;
DROP TABLE IF EXISTS dst_day_of_week;
DROP TABLE IF EXISTS dst_hundreds;
DROP TABLE IF EXISTS dst_nullable;

CREATE TABLE dst_year (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date)
ENGINE = S3(s3_conn, filename = 'export_ttl_pkey_check_year', format = Parquet, partition_strategy = 'hive') PARTITION BY year;
CREATE TABLE dst_region (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date)
ENGINE = S3(s3_conn, filename = 'export_ttl_pkey_check_region', format = Parquet, partition_strategy = 'hive') PARTITION BY region;
CREATE TABLE dst_ba (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date)
ENGINE = S3(s3_conn, filename = 'export_ttl_pkey_check_ba', format = Parquet, partition_strategy = 'hive') PARTITION BY (b, a);
CREATE TABLE dst_id (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date)
ENGINE = S3(s3_conn, filename = 'export_ttl_pkey_check_id', format = Parquet, partition_strategy = 'hive') PARTITION BY id;
CREATE TABLE dst_d (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date)
ENGINE = S3(s3_conn, filename = 'export_ttl_pkey_check_d', format = Parquet, partition_strategy = 'hive') PARTITION BY d;
CREATE TABLE dst_month (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date)
ENGINE = S3(s3_conn, filename = 'export_ttl_pkey_check_month/{_partition_id}/{_file}.parquet', format = Parquet, partition_strategy = 'wildcard') PARTITION BY toYYYYMM(t);
CREATE TABLE dst_year_mod (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date)
ENGINE = S3(s3_conn, filename = 'export_ttl_pkey_check_year_mod/{_partition_id}/{_file}.parquet', format = Parquet, partition_strategy = 'wildcard') PARTITION BY year % 10;
CREATE TABLE dst_hash (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date)
ENGINE = S3(s3_conn, filename = 'export_ttl_pkey_check_hash/{_partition_id}/{_file}.parquet', format = Parquet, partition_strategy = 'wildcard') PARTITION BY cityHash64(year) % 4;
CREATE TABLE dst_to_year (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date)
ENGINE = S3(s3_conn, filename = 'export_ttl_pkey_check_to_year/{_partition_id}/{_file}.parquet', format = Parquet, partition_strategy = 'wildcard') PARTITION BY toYear(t);
CREATE TABLE dst_day_of_week (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date)
ENGINE = S3(s3_conn, filename = 'export_ttl_pkey_check_day_of_week/{_partition_id}/{_file}.parquet', format = Parquet, partition_strategy = 'wildcard') PARTITION BY toDayOfWeek(t);
CREATE TABLE dst_hundreds (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date)
ENGINE = S3(s3_conn, filename = 'export_ttl_pkey_check_hundreds/{_partition_id}/{_file}.parquet', format = Parquet, partition_strategy = 'wildcard') PARTITION BY intDiv(id, 100);

SELECT 'structural';

CREATE TABLE src (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date) ENGINE = MergeTree PARTITION BY year ORDER BY id
TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_year;
SELECT extract(create_table_query, 'EXPORT TO TABLE (\\w+)') FROM system.tables WHERE database = currentDatabase() AND name = 'src';
DROP TABLE src;

CREATE TABLE src (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date) ENGINE = MergeTree PARTITION BY toYYYYMM(t) ORDER BY id
TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_month;
SELECT extract(create_table_query, 'EXPORT TO TABLE (\\w+)') FROM system.tables WHERE database = currentDatabase() AND name = 'src';
DROP TABLE src;

CREATE TABLE src (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date) ENGINE = MergeTree PARTITION BY (region, toYYYYMM(t)) ORDER BY id
TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_region;
SELECT extract(create_table_query, 'EXPORT TO TABLE (\\w+)') FROM system.tables WHERE database = currentDatabase() AND name = 'src';
DROP TABLE src;

CREATE TABLE src (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date) ENGINE = MergeTree PARTITION BY (a, b) ORDER BY id
TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_ba;
SELECT extract(create_table_query, 'EXPORT TO TABLE (\\w+)') FROM system.tables WHERE database = currentDatabase() AND name = 'src';
DROP TABLE src;

CREATE TABLE src (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date) ENGINE = MergeTree PARTITION BY year ORDER BY id
TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_year_mod;
SELECT extract(create_table_query, 'EXPORT TO TABLE (\\w+)') FROM system.tables WHERE database = currentDatabase() AND name = 'src';
DROP TABLE src;

CREATE TABLE src (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date) ENGINE = MergeTree PARTITION BY year ORDER BY id
TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_hash;
SELECT extract(create_table_query, 'EXPORT TO TABLE (\\w+)') FROM system.tables WHERE database = currentDatabase() AND name = 'src';
DROP TABLE src;

CREATE TABLE src (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date) ENGINE = MergeTree PARTITION BY toString(year) ORDER BY id
TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_year;
SELECT extract(create_table_query, 'EXPORT TO TABLE (\\w+)') FROM system.tables WHERE database = currentDatabase() AND name = 'src';
DROP TABLE src;

SELECT 'dynamic';

CREATE TABLE src (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date) ENGINE = MergeTree PARTITION BY toYYYYMM(t) ORDER BY id
TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_to_year;
SELECT extract(create_table_query, 'EXPORT TO TABLE (\\w+)') FROM system.tables WHERE database = currentDatabase() AND name = 'src';
DROP TABLE src;

CREATE TABLE src (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date) ENGINE = MergeTree PARTITION BY toYYYYMM(d) ORDER BY id
TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_d;
SELECT extract(create_table_query, 'EXPORT TO TABLE (\\w+)') FROM system.tables WHERE database = currentDatabase() AND name = 'src';
DROP TABLE src;

CREATE TABLE src (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date) ENGINE = MergeTree PARTITION BY intDiv(id, 1000) ORDER BY id
TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_hundreds;
SELECT extract(create_table_query, 'EXPORT TO TABLE (\\w+)') FROM system.tables WHERE database = currentDatabase() AND name = 'src';
DROP TABLE src;

SELECT 'refused';

-- `id` is not in the source partition key.
CREATE TABLE src (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date) ENGINE = MergeTree PARTITION BY year ORDER BY id
TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_id; -- { serverError BAD_ARGUMENTS }

-- `toDayOfWeek` is not monotonic.
CREATE TABLE src (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date) ENGINE = MergeTree PARTITION BY toYYYYMM(t) ORDER BY id
TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_day_of_week; -- { serverError BAD_ARGUMENTS }

-- A source without a partition key into a partitioned destination.
CREATE TABLE src (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date) ENGINE = MergeTree ORDER BY id
TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_year; -- { serverError BAD_ARGUMENTS }

-- A `Nullable` column that has to be checked per group: a NULL forms its own destination partition.
CREATE TABLE dst_nullable (id UInt64, k Nullable(UInt32), t DateTime)
ENGINE = S3(s3_conn, filename = 'export_ttl_pkey_check_nullable/{_partition_id}/{_file}.parquet', format = Parquet, partition_strategy = 'wildcard') PARTITION BY intDiv(k, 10);
CREATE TABLE src (id UInt64, k Nullable(UInt32), t DateTime) ENGINE = MergeTree PARTITION BY intDiv(k, 100) ORDER BY id
TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_nullable SETTINGS allow_nullable_key = 1; -- { serverError BAD_ARGUMENTS }

-- The same destination is a function of a `Nullable` source key, which holds for every group.
CREATE TABLE src (id UInt64, k Nullable(UInt32), t DateTime) ENGINE = MergeTree PARTITION BY k ORDER BY id
TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_nullable SETTINGS allow_nullable_key = 1;
SELECT extract(create_table_query, 'EXPORT TO TABLE (\\w+)') FROM system.tables WHERE database = currentDatabase() AND name = 'src';
DROP TABLE src;

SELECT 'alter';

CREATE TABLE src (id UInt64, year UInt16, region String, a UInt8, b UInt8, t DateTime, d Date) ENGINE = MergeTree PARTITION BY year ORDER BY id
TTL t + INTERVAL 30 DAY DELETE;
ALTER TABLE src MODIFY TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_id; -- { serverError BAD_ARGUMENTS }
ALTER TABLE src MODIFY TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_day_of_week; -- { serverError BAD_ARGUMENTS }
SELECT extract(create_table_query, 'TTL .*? SETTINGS') FROM system.tables WHERE database = currentDatabase() AND name = 'src';
ALTER TABLE src MODIFY TTL t + INTERVAL 1 DAY EXPORT TO TABLE dst_year_mod;
SELECT extract(create_table_query, 'TTL .*? SETTINGS') FROM system.tables WHERE database = currentDatabase() AND name = 'src';
DROP TABLE src;

DROP TABLE dst_year;
DROP TABLE dst_region;
DROP TABLE dst_ba;
DROP TABLE dst_id;
DROP TABLE dst_d;
DROP TABLE dst_month;
DROP TABLE dst_year_mod;
DROP TABLE dst_hash;
DROP TABLE dst_to_year;
DROP TABLE dst_day_of_week;
DROP TABLE dst_hundreds;
DROP TABLE dst_nullable;
