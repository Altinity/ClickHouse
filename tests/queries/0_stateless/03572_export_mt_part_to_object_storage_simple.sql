-- Tags: no-parallel

DROP TABLE IF EXISTS 03572_mt_table, 03572_invalid_schema_table;

CREATE TABLE 03572_mt_table (id UInt64, year UInt16) ENGINE = MergeTree() PARTITION BY year ORDER BY tuple();

INSERT INTO 03572_mt_table VALUES (1, 2020);

-- Create a table with a different partition key and export a partition to it. It should throw
CREATE TABLE 03572_invalid_schema_table (id UInt64, x UInt16)
ENGINE = S3(
    s3_conn,
    filename='03572_invalid_schema_table',
    format=Parquet, partition_strategy='hive') PARTITION BY x;

ALTER TABLE $mt_table EXPORT PARTITION ID '2020' TO TABLE 03572_invalid_schema_table
SETTINGS allow_experimental_export_merge_tree_partition = 1 -- {serverError BAD_ARGUMENTS}


-- The only partition strategy that supports exports is hive. Wildcard should throw
CREATE TABLE 03572_invalid_schema_table (id UInt64, year UInt16)
ENGINE = S3(
    s3_conn,
    filename='03572_invalid_schema_table/{_partition_id}',
    format=Parquet,
    partition_strategy='wildcard') PARTITION BY (id, year);

ALTER TABLE $mt_table EXPORT PARTITION ID '2020' TO TABLE 03572_invalid_schema_table
SETTINGS allow_experimental_export_merge_tree_partition = 1 -- {serverError NOT_IMPLEMENTED}
