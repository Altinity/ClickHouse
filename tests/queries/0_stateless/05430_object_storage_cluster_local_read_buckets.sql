-- Tags: no-fasttest
-- Tag no-fasttest: Depends on Minio
--
-- `object_storage_cluster_local_read_max_tasks` counts cluster tasks.
-- With `cluster_table_function_split_granularity = 'bucket'` one Parquet row group is one task,
-- so four row groups in one file stay on the cluster when the limit is 1 and run locally when the limit is 4.
-- With file granularity the same file is one task and runs locally. `Shards` is incremented only by a cluster read.

INSERT INTO FUNCTION s3('http://localhost:11111/test/05430_object_storage_cluster_local_read_buckets_a.parquet', 'Parquet', 'x UInt32')
SELECT number FROM numbers(4)
SETTINGS s3_truncate_on_insert = 1, output_format_parquet_row_group_size = 1;

SELECT 'four buckets, limit 1, cluster';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05430_object_storage_cluster_local_read_buckets_a.parquet', 'Parquet', 'x UInt32')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 1,
    object_storage_cluster_local_read_probe_ms = 5000,
    cluster_table_function_split_granularity = 'bucket',
    cluster_table_function_buckets_batch_size = 0,
    log_comment = '05430_buckets_over_limit';

SYSTEM FLUSH LOGS query_log;

SELECT ProfileEvents['Shards']
FROM system.query_log
WHERE current_database = currentDatabase()
    AND log_comment = '05430_buckets_over_limit'
    AND type = 'QueryFinish'
ORDER BY event_time_microseconds DESC
LIMIT 1;

SELECT 'four buckets, limit 4, local';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05430_object_storage_cluster_local_read_buckets_a.parquet', 'Parquet', 'x UInt32')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 4,
    object_storage_cluster_local_read_probe_ms = 5000,
    cluster_table_function_split_granularity = 'bucket',
    cluster_table_function_buckets_batch_size = 0,
    log_comment = '05430_buckets_at_limit';

SYSTEM FLUSH LOGS query_log;

SELECT ProfileEvents['Shards']
FROM system.query_log
WHERE current_database = currentDatabase()
    AND log_comment = '05430_buckets_at_limit'
    AND type = 'QueryFinish'
ORDER BY event_time_microseconds DESC
LIMIT 1;

SELECT 'same file, file granularity, local';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05430_object_storage_cluster_local_read_buckets_a.parquet', 'Parquet', 'x UInt32')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 1,
    object_storage_cluster_local_read_probe_ms = 5000,
    cluster_table_function_split_granularity = 'file',
    log_comment = '05430_file_granularity';

SYSTEM FLUSH LOGS query_log;

SELECT ProfileEvents['Shards']
FROM system.query_log
WHERE current_database = currentDatabase()
    AND log_comment = '05430_file_granularity'
    AND type = 'QueryFinish'
ORDER BY event_time_microseconds DESC
LIMIT 1;
