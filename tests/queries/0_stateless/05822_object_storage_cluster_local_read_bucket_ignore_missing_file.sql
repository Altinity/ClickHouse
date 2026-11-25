-- Tags: no-fasttest
-- Tag no-fasttest: Depends on Minio
--
-- With `cluster_table_function_split_granularity = 'bucket'` and `s3_ignore_file_doesnt_exist = 1`,
-- a key that does not exist is skipped, as it is without buckets. The query must not fail,
-- neither for the distributed read nor for the local read after the `object_storage_cluster_local_read_max_tasks` probe.
-- The brace list is read without object metadata, so the missing key is only found when the file is split.

INSERT INTO FUNCTION s3('http://localhost:11111/test/05822_object_storage_cluster_local_read_bucket_ignore_missing_file_a.parquet', 'Parquet', 'x UInt32')
SELECT number FROM numbers(4)
SETTINGS s3_truncate_on_insert = 1, output_format_parquet_row_group_size = 1;

SELECT 'cluster read';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05822_object_storage_cluster_local_read_bucket_ignore_missing_file_{missing,a}.parquet', 'Parquet', 'x UInt32')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    cluster_table_function_split_granularity = 'bucket',
    cluster_table_function_buckets_batch_size = 0,
    s3_ignore_file_doesnt_exist = 1;

SELECT 'local read after the probe';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05822_object_storage_cluster_local_read_bucket_ignore_missing_file_{missing,a}.parquet', 'Parquet', 'x UInt32')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 10,
    object_storage_cluster_local_read_probe_ms = 5000,
    cluster_table_function_split_granularity = 'bucket',
    cluster_table_function_buckets_batch_size = 0,
    s3_ignore_file_doesnt_exist = 1,
    log_comment = '05822_local';

SYSTEM FLUSH LOGS query_log;

SELECT ProfileEvents['Shards']
FROM system.query_log
WHERE current_database = currentDatabase()
    AND log_comment = '05822_local'
    AND type = 'QueryFinish';

SELECT 'missing file, ignore is off';
SELECT count()
FROM s3('http://localhost:11111/test/05822_object_storage_cluster_local_read_bucket_ignore_missing_file_{missing,a}.parquet', 'Parquet', 'x UInt32')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 10,
    object_storage_cluster_local_read_probe_ms = 5000,
    cluster_table_function_split_granularity = 'bucket',
    cluster_table_function_buckets_batch_size = 0,
    s3_ignore_file_doesnt_exist = 0; -- { serverError S3_ERROR }
