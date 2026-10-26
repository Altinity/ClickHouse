-- Tags: no-fasttest
-- no-fasttest: uses the S3 mock server on localhost:11111

-- `PASTE` pairs rows by position. With `global`, a one-row right side paired on each
-- shard that has left rows would return one row per such shard. Several files make
-- both shards nonempty, so the initiator must return a single row.

CREATE TEMPORARY TABLE t_dim_05393 (name String);
INSERT INTO t_dim_05393 VALUES ('a');

INSERT INTO FUNCTION s3('http://localhost:11111/test/05393_' || currentDatabase() || '_{_partition_id}.tsv', 'test', 'testtest', 'TSV', 'k UInt64')
PARTITION BY k
SELECT number AS k FROM numbers(16)
SETTINGS s3_truncate_on_insert = 1;

SELECT 'old';
SELECT count()
FROM s3Cluster('test_cluster_two_shards', 'http://localhost:11111/test/05393_' || currentDatabase() || '_*.tsv', 'test', 'testtest', 'TSV', 'k UInt64') AS t1
PASTE JOIN t_dim_05393 AS t2
SETTINGS allow_experimental_analyzer = 0, object_storage_cluster_join_mode = 'global', lock_object_storage_task_distribution_ms = 600000;

SELECT 'analyzer';
SELECT count()
FROM s3Cluster('test_cluster_two_shards', 'http://localhost:11111/test/05393_' || currentDatabase() || '_*.tsv', 'test', 'testtest', 'TSV', 'k UInt64') AS t1
PASTE JOIN t_dim_05393 AS t2
SETTINGS allow_experimental_analyzer = 1, object_storage_cluster_join_mode = 'global', lock_object_storage_task_distribution_ms = 600000;

DROP TEMPORARY TABLE t_dim_05393;
