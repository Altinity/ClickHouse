-- Tags: no-parallel, no-fasttest

DROP TABLE IF EXISTS 03572_outdated_mt_table, 03572_invalid_schema_table, 03572_s3_outdated_table;

CREATE TABLE 03572_mt_table (id UInt64, year UInt16) ENGINE = MergeTree() PARTITION BY year ORDER BY tuple();

INSERT INTO 03572_mt_table VALUES (1, 2020);

-- Create a table with a different partition key and export a partition to it. It should throw
CREATE TABLE 03572_invalid_schema_table (id UInt64, x UInt16) ENGINE = S3(s3_conn, filename='03572_invalid_schema_table', format='Parquet', partition_strategy='hive') PARTITION BY x;

ALTER TABLE 03572_mt_table EXPORT PART '2020_1_1_0' TO TABLE 03572_invalid_schema_table
SETTINGS allow_experimental_export_merge_tree_part = 1; -- {serverError INCOMPATIBLE_COLUMNS}

DROP TABLE 03572_invalid_schema_table;

-- The only partition strategy that supports exports is hive. Wildcard should throw
CREATE TABLE 03572_invalid_schema_table (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='03572_invalid_schema_table/{_partition_id}', format='Parquet', partition_strategy='wildcard') PARTITION BY (id, year);

ALTER TABLE 03572_outdated_mt_table EXPORT PART '2020_1_1_0' TO TABLE 03572_invalid_schema_table SETTINGS allow_experimental_export_merge_tree_part = 1; -- {serverError NOT_IMPLEMENTED}

-- Test export_merge_tree_part_allow_outdated_parts setting
CREATE TABLE 03572_s3_outdated_table (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='03572_s3_outdated_table', format='Parquet', partition_strategy='hive') PARTITION BY year;

SYSTEM STOP MERGES 03572_outdated_mt_table;

INSERT INTO 03572_outdated_mt_table VALUES (2, 2020);
INSERT INTO 03572_outdated_mt_table VALUES (3, 2020);

-- Optimize to merge parts, making old parts outdated
OPTIMIZE TABLE 03572_outdated_mt_table FINAL;

ALTER TABLE 03572_outdated_mt_table EXPORT PART '2020_1_1_0' TO TABLE 03572_s3_outdated_table 
SETTINGS allow_experimental_export_merge_tree_part = 1, export_merge_tree_part_allow_outdated_parts = false; -- {serverError BAD_ARGUMENTS}

DROP TABLE IF EXISTS 03572_outdated_mt_table, 03572_invalid_schema_table, 03572_s3_outdated_table;
