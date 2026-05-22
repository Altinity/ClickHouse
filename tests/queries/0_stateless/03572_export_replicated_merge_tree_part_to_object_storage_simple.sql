-- Tags: no-parallel, no-fasttest

DROP TABLE IF EXISTS 03572_rmt_table, 03572_invalid_schema_table, 03572_rmt_partition_type_mismatch_mt, 03572_rmt_partition_type_mismatch_s3;

CREATE TABLE 03572_rmt_table (id UInt64, year UInt16) ENGINE = ReplicatedMergeTree('/clickhouse/{database}/test_03572_rmt/03572_rmt_table', 'replica1') PARTITION BY year ORDER BY tuple();

INSERT INTO 03572_rmt_table VALUES (1, 2020);

-- Create a table with a different partition key and export a partition to it. It should throw
-- on the partition-key AST mismatch (schema compat now follows INSERT SELECT positional semantics,
-- so the column shape matches and the partition-key check is what fires).
CREATE TABLE 03572_invalid_schema_table (id UInt64, x UInt16) ENGINE = S3(s3_conn, filename='03572_invalid_schema_table', format='Parquet', partition_strategy='hive') PARTITION BY x;

ALTER TABLE 03572_rmt_table EXPORT PART '2020_0_0_0' TO TABLE 03572_invalid_schema_table
SETTINGS allow_experimental_export_merge_tree_part = 1; -- {serverError BAD_ARGUMENTS}

DROP TABLE 03572_invalid_schema_table;

-- The only partition strategy that supports exports is hive. Wildcard should throw
CREATE TABLE 03572_invalid_schema_table (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='03572_invalid_schema_table/{_partition_id}', format='Parquet', partition_strategy='wildcard') PARTITION BY (id, year);

ALTER TABLE 03572_rmt_table EXPORT PART '2020_0_0_0' TO TABLE 03572_invalid_schema_table SETTINGS allow_experimental_export_merge_tree_part = 1; -- {serverError NOT_IMPLEMENTED}

-- Castable but non-equal partition-column types must be rejected synchronously.
-- The partition path is computed from the source part's minmax block (SOURCE types)
-- BEFORE row data is CAST to destination types — letting String -> UInt16 through
-- would cause silent divergence between partition path and row values.
CREATE TABLE 03572_rmt_partition_type_mismatch_mt (id UInt64, year String) ENGINE = ReplicatedMergeTree('/clickhouse/{database}/test_03572_rmt_pcol_type/03572_rmt_partition_type_mismatch_mt', 'replica1') PARTITION BY year ORDER BY tuple();
CREATE TABLE 03572_rmt_partition_type_mismatch_s3 (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='03572_rmt_partition_type_mismatch_s3', format='Parquet', partition_strategy='hive') PARTITION BY year;

ALTER TABLE 03572_rmt_partition_type_mismatch_mt EXPORT PART 'any_part_name' TO TABLE 03572_rmt_partition_type_mismatch_s3
SETTINGS allow_experimental_export_merge_tree_part = 1; -- {serverError BAD_ARGUMENTS}

DROP TABLE IF EXISTS 03572_rmt_table, 03572_invalid_schema_table, 03572_rmt_partition_type_mismatch_mt, 03572_rmt_partition_type_mismatch_s3;
