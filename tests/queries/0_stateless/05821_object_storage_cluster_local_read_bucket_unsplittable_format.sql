-- Tags: no-fasttest
-- Tag no-fasttest: Depends on Minio
--
-- With `cluster_table_function_split_granularity = 'bucket'`, a format that cannot be split into buckets
-- (here `TSV`) is read as one task per file. Such a file must not be dropped or make the query fail,
-- neither for the distributed read nor for the local read after the `object_storage_cluster_local_read_max_tasks` probe.

INSERT INTO FUNCTION s3('http://localhost:11111/test/05821_object_storage_cluster_local_read_bucket_unsplittable_a.tsv', 'TSV', 'x UInt32')
SELECT number FROM numbers(3)
SETTINGS s3_truncate_on_insert = 1;

INSERT INTO FUNCTION s3('http://localhost:11111/test/05821_object_storage_cluster_local_read_bucket_unsplittable_b.tsv', 'TSV', 'x UInt32')
SELECT number + 10 FROM numbers(2)
SETTINGS s3_truncate_on_insert = 1;

SELECT 'cluster read';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05821_object_storage_cluster_local_read_bucket_unsplittable_?.tsv', 'TSV', 'x UInt32')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    cluster_table_function_split_granularity = 'bucket';

SELECT 'local read after the probe, glob';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05821_object_storage_cluster_local_read_bucket_unsplittable_?.tsv', 'TSV', 'x UInt32')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 10,
    object_storage_cluster_local_read_probe_ms = 5000,
    cluster_table_function_split_granularity = 'bucket',
    log_comment = '05821_local_glob';

SELECT 'local read after the probe, braces';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05821_object_storage_cluster_local_read_bucket_unsplittable_{a,b}.tsv', 'TSV', 'x UInt32')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 10,
    object_storage_cluster_local_read_probe_ms = 5000,
    cluster_table_function_split_granularity = 'bucket',
    log_comment = '05821_local_braces';

SYSTEM FLUSH LOGS query_log;

SELECT log_comment, ProfileEvents['Shards']
FROM system.query_log
WHERE current_database = currentDatabase()
    AND log_comment IN ('05821_local_glob', '05821_local_braces')
    AND type = 'QueryFinish'
ORDER BY log_comment;
