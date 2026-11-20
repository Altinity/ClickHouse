-- Tags: no-fasttest
-- Tag no-fasttest: Depends on Minio
--
-- object_storage_cluster_local_read_max_tasks reads on the initiator when planning can list
-- at most that many files inside the probe budget. More files keep the distributed read.
-- `Shards` is incremented only by the distributed read.

INSERT INTO FUNCTION s3('http://localhost:11111/test/05428_object_storage_cluster_local_read_a.tsv', 'TSV', 'x UInt32')
SELECT 1
SETTINGS s3_truncate_on_insert = 1;

INSERT INTO FUNCTION s3('http://localhost:11111/test/05428_object_storage_cluster_local_read_b.tsv', 'TSV', 'x UInt32')
SELECT 2
SETTINGS s3_truncate_on_insert = 1;

INSERT INTO FUNCTION s3('http://localhost:11111/test/05428_object_storage_cluster_local_read_c.tsv', 'TSV', 'x UInt32')
SELECT 3
SETTINGS s3_truncate_on_insert = 1;

SELECT 'one file, under the limit, local';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05428_object_storage_cluster_local_read_a.tsv', 'TSV', 'x UInt32')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 10,
    object_storage_cluster_local_read_probe_ms = 5000,
    log_comment = '05428_local_read_small';

SYSTEM FLUSH LOGS query_log;

SELECT ProfileEvents['Shards']
FROM system.query_log
WHERE current_database = currentDatabase()
    AND log_comment = '05428_local_read_small'
    AND type = 'QueryFinish'
ORDER BY event_time_microseconds DESC
LIMIT 1;

SELECT 'three files, over the limit, cluster';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05428_object_storage_cluster_local_read_*.tsv', 'TSV', 'x UInt32')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 1,
    object_storage_cluster_local_read_probe_ms = 5000,
    log_comment = '05428_local_read_cluster';

SYSTEM FLUSH LOGS query_log;

SELECT ProfileEvents['Shards']
FROM system.query_log
WHERE current_database = currentDatabase()
    AND log_comment = '05428_local_read_cluster'
    AND type = 'QueryFinish'
ORDER BY event_time_microseconds DESC
LIMIT 1;

SELECT 'limit 0 keeps the cluster read';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05428_object_storage_cluster_local_read_a.tsv', 'TSV', 'x UInt32')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 0,
    log_comment = '05428_local_read_disabled';

SYSTEM FLUSH LOGS query_log;

SELECT ProfileEvents['Shards']
FROM system.query_log
WHERE current_database = currentDatabase()
    AND log_comment = '05428_local_read_disabled'
    AND type = 'QueryFinish'
ORDER BY event_time_microseconds DESC
LIMIT 1;

SELECT 's3Cluster under the limit, local';
SELECT count(), sum(x)
FROM s3Cluster('test_shard_localhost', 'http://localhost:11111/test/05428_object_storage_cluster_local_read_a.tsv', 'TSV', 'x UInt32')
SETTINGS
    object_storage_cluster_local_read_max_tasks = 10,
    object_storage_cluster_local_read_probe_ms = 5000,
    log_comment = '05428_local_read_cluster_function';

SYSTEM FLUSH LOGS query_log;

SELECT ProfileEvents['Shards']
FROM system.query_log
WHERE current_database = currentDatabase()
    AND log_comment = '05428_local_read_cluster_function'
    AND type = 'QueryFinish'
ORDER BY event_time_microseconds DESC
LIMIT 1;
