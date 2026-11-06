-- Tags: no-fasttest
-- Tag no-fasttest: Depends on Minio
--
-- A Merge passes one query info to every child. Each object-storage cluster child must probe
-- itself: one small child is read locally and one large child stays on the cluster.
-- `Shards` counts cluster reads. Both local would be 0, both cluster would be 2.

INSERT INTO FUNCTION s3('http://localhost:11111/test/05429_object_storage_cluster_local_read_merge_small.tsv', 'TSV', 'x UInt32')
SELECT 1
SETTINGS s3_truncate_on_insert = 1;

INSERT INTO FUNCTION s3('http://localhost:11111/test/05429_object_storage_cluster_local_read_merge_large_1.tsv', 'TSV', 'x UInt32')
SELECT 2
SETTINGS s3_truncate_on_insert = 1;

INSERT INTO FUNCTION s3('http://localhost:11111/test/05429_object_storage_cluster_local_read_merge_large_2.tsv', 'TSV', 'x UInt32')
SELECT 3
SETTINGS s3_truncate_on_insert = 1;

INSERT INTO FUNCTION s3('http://localhost:11111/test/05429_object_storage_cluster_local_read_merge_large_3.tsv', 'TSV', 'x UInt32')
SELECT 4
SETTINGS s3_truncate_on_insert = 1;

DROP TABLE IF EXISTS local_read_merge_05429_all;
DROP TABLE IF EXISTS local_read_merge_05429_small;
DROP TABLE IF EXISTS local_read_merge_05429_large;

CREATE TABLE local_read_merge_05429_small (x UInt32)
ENGINE = S3('http://localhost:11111/test/05429_object_storage_cluster_local_read_merge_small.tsv', 'TSV')
SETTINGS object_storage_cluster = 'test_shard_localhost';

CREATE TABLE local_read_merge_05429_large (x UInt32)
ENGINE = S3('http://localhost:11111/test/05429_object_storage_cluster_local_read_merge_large_*.tsv', 'TSV')
SETTINGS object_storage_cluster = 'test_shard_localhost';

CREATE TABLE local_read_merge_05429_all (x UInt32)
ENGINE = Merge(currentDatabase(), '^local_read_merge_05429_(small|large)$');

SELECT 'small child local, large child cluster';
SELECT count(), sum(x)
FROM local_read_merge_05429_all
SETTINGS
    object_storage_cluster_local_read_max_files = 1,
    object_storage_cluster_local_read_probe_ms = 5000,
    log_comment = '05429_merge_mixed';

SYSTEM FLUSH LOGS query_log;

SELECT ProfileEvents['Shards']
FROM system.query_log
WHERE current_database = currentDatabase()
    AND log_comment = '05429_merge_mixed'
    AND type = 'QueryFinish'
ORDER BY event_time_microseconds DESC
LIMIT 1;

DROP TABLE local_read_merge_05429_all;
DROP TABLE local_read_merge_05429_small;
DROP TABLE local_read_merge_05429_large;
