-- Tags: no-fasttest
-- Tag no-fasttest: Depends on Minio
--
-- A local read after the `object_storage_cluster_local_read_max_tasks` probe reuses the listed objects.
-- For a brace expansion and for keys taken from a `_path` filter the listing is a `KeysIterator`
-- without object metadata. The reader must fetch the real size and not take such a file for an empty one.

INSERT INTO FUNCTION s3('http://localhost:11111/test/05484_object_storage_cluster_local_read_keys_a.tsv', 'TSV', 'x UInt32')
SELECT 1
SETTINGS s3_truncate_on_insert = 1;

INSERT INTO FUNCTION s3('http://localhost:11111/test/05484_object_storage_cluster_local_read_keys_b.tsv', 'TSV', 'x UInt32')
SELECT 2
SETTINGS s3_truncate_on_insert = 1;

SELECT 'one key in braces';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05484_object_storage_cluster_local_read_keys_{a}.tsv', 'TSV', 'x UInt32')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 10,
    object_storage_cluster_local_read_probe_ms = 5000,
    log_comment = '05484_brace_one';

SELECT 'two keys in braces';
SELECT count(), sum(x), sum(_size)
FROM s3('http://localhost:11111/test/05484_object_storage_cluster_local_read_keys_{a,b}.tsv', 'TSV', 'x UInt32')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 10,
    object_storage_cluster_local_read_probe_ms = 5000,
    log_comment = '05484_brace_two';

SELECT 'glob with keys taken from the _path filter';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05484_object_storage_cluster_local_read_keys_*.tsv', 'TSV', 'x UInt32')
WHERE _path = 'test/05484_object_storage_cluster_local_read_keys_a.tsv'
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 10,
    object_storage_cluster_local_read_probe_ms = 5000,
    log_comment = '05484_glob_path_filter';

SYSTEM FLUSH LOGS query_log;

-- `Shards` is incremented only by the distributed read, so 0 means the queries above were read locally.
SELECT log_comment, ProfileEvents['Shards']
FROM system.query_log
WHERE current_database = currentDatabase()
    AND log_comment IN ('05484_brace_one', '05484_brace_two', '05484_glob_path_filter')
    AND type = 'QueryFinish'
ORDER BY log_comment;
