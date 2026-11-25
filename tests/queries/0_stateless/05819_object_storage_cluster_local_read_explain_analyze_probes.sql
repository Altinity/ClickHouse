-- Tags: no-fasttest
-- Tag no-fasttest: Depends on Minio
--
-- `EXPLAIN` without `ANALYZE` reads no data, so it shows the cluster read and does not run the
-- `object_storage_cluster_local_read_max_tasks` probe. `EXPLAIN ANALYZE` executes the query, so it runs the probe
-- like the query does: a small read is local, and a missing glob fails the same way the query fails.

INSERT INTO FUNCTION s3('http://localhost:11111/test/05819_object_storage_cluster_local_read_explain_analyze_one.tsv', 'TSV', 'x UInt32')
SELECT 1
SETTINGS s3_truncate_on_insert = 1;

SELECT 'explain plan keeps the cluster read';
SELECT countIf(explain LIKE '%ReadFromCluster%')
FROM
(
    EXPLAIN PLAN
    SELECT count()
    FROM s3('http://localhost:11111/test/05819_object_storage_cluster_local_read_explain_analyze_*.tsv', 'TSV', 'x UInt32')
    SETTINGS
        object_storage_cluster = 'test_shard_localhost',
        object_storage_cluster_local_read_max_tasks = 1,
        object_storage_cluster_local_read_probe_ms = 5000
);

SELECT 'explain analyze runs the probe and reads locally';
SELECT countIf(explain LIKE '%ReadFromCluster%')
FROM
(
    EXPLAIN ANALYZE
    SELECT count()
    FROM s3('http://localhost:11111/test/05819_object_storage_cluster_local_read_explain_analyze_*.tsv', 'TSV', 'x UInt32')
    SETTINGS
        object_storage_cluster = 'test_shard_localhost',
        object_storage_cluster_local_read_max_tasks = 1,
        object_storage_cluster_local_read_probe_ms = 5000
);

SELECT 'explain analyze fails on a missing glob like the query';
EXPLAIN ANALYZE
SELECT count()
FROM s3('http://localhost:11111/test/05819_object_storage_cluster_local_read_explain_analyze_missing_*.tsv', 'TSV', 'x UInt32')
SETTINGS
    s3_throw_on_zero_files_match = 1,
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 1,
    object_storage_cluster_local_read_probe_ms = 5000; -- { serverError FILE_DOESNT_EXIST }
