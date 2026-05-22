-- Tags: no-parallel, no-fasttest

DROP TABLE IF EXISTS 03572_mt_table, 03572_invalid_schema_table, 03572_ephemeral_mt_table, 03572_matching_ephemeral_s3_table, 03572_partition_type_mismatch_mt, 03572_partition_type_mismatch_s3;

SET allow_experimental_export_merge_tree_part=1;

CREATE TABLE 03572_mt_table (id UInt64, year UInt16) ENGINE = MergeTree() PARTITION BY year ORDER BY tuple();

INSERT INTO 03572_mt_table VALUES (1, 2020);

-- Create a table with a different partition key and export a partition to it. It should throw
-- on the partition-key AST mismatch (schema compat now follows INSERT SELECT positional semantics,
-- so the column shape matches and the partition-key check is what fires).
CREATE TABLE 03572_invalid_schema_table (id UInt64, x UInt16) ENGINE = S3(s3_conn, filename='03572_invalid_schema_table', format='Parquet', partition_strategy='hive') PARTITION BY x;

ALTER TABLE 03572_mt_table EXPORT PART '2020_1_1_0' TO TABLE 03572_invalid_schema_table
SETTINGS allow_experimental_export_merge_tree_part = 1; -- {serverError BAD_ARGUMENTS}

DROP TABLE 03572_invalid_schema_table;

-- The only partition strategy that supports exports is hive. Wildcard should throw
CREATE TABLE 03572_invalid_schema_table (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='03572_invalid_schema_table/{_partition_id}', format='Parquet', partition_strategy='wildcard') PARTITION BY (id, year);

ALTER TABLE 03572_mt_table EXPORT PART '2020_1_1_0' TO TABLE 03572_invalid_schema_table; -- {serverError NOT_IMPLEMENTED}

-- Not a table function, should throw
ALTER TABLE 03572_mt_table EXPORT PART '2020_1_1_0' TO TABLE FUNCTION extractKeyValuePairs('name:ronaldo'); -- {serverError UNKNOWN_FUNCTION}

-- It is a table function, but the engine does not support exports/imports, should throw
ALTER TABLE 03572_mt_table EXPORT PART '2020_1_1_0' TO TABLE FUNCTION url('a.parquet'); -- {serverError NOT_IMPLEMENTED}

-- Source-side ephemeral columns are not readable, so the destination must not declare a matching
-- ordinary column or the column count will not align under positional matching.
CREATE TABLE 03572_ephemeral_mt_table (id UInt64, year UInt16, name String EPHEMERAL) ENGINE = MergeTree() PARTITION BY year ORDER BY tuple();

CREATE TABLE 03572_matching_ephemeral_s3_table (id UInt64, year UInt16, name String) ENGINE = S3(s3_conn, filename='03572_matching_ephemeral_s3_table', format='Parquet', partition_strategy='hive') PARTITION BY year;

INSERT INTO 03572_ephemeral_mt_table (id, year, name) VALUES (1, 2020, 'alice');

ALTER TABLE 03572_ephemeral_mt_table EXPORT PART '2020_1_1_0' TO TABLE 03572_matching_ephemeral_s3_table; -- {serverError NUMBER_OF_COLUMNS_DOESNT_MATCH}

-- Castable but non-equal partition-column types must be rejected synchronously.
-- The partition path is computed from the source part's minmax block (SOURCE types)
-- BEFORE row data is CAST to destination types — letting String -> UInt16 through
-- would cause silent divergence between partition path and row values.
CREATE TABLE 03572_partition_type_mismatch_mt (id UInt64, year String) ENGINE = MergeTree() PARTITION BY year ORDER BY tuple();
CREATE TABLE 03572_partition_type_mismatch_s3 (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='03572_partition_type_mismatch_s3', format='Parquet', partition_strategy='hive') PARTITION BY year;

-- Partition-column type mismatch is checked synchronously BEFORE the part lookup,
-- so we don't need to insert data nor use a real part name to provoke it.
ALTER TABLE 03572_partition_type_mismatch_mt EXPORT PART 'any_part_name' TO TABLE 03572_partition_type_mismatch_s3
SETTINGS allow_experimental_export_merge_tree_part = 1; -- {serverError BAD_ARGUMENTS}

DROP TABLE IF EXISTS 03572_mt_table, 03572_invalid_schema_table, 03572_ephemeral_mt_table, 03572_matching_ephemeral_s3_table, 03572_partition_type_mismatch_mt, 03572_partition_type_mismatch_s3;
