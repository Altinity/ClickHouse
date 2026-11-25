-- Tags: no-fasttest
-- Tag no-fasttest: Depends on Minio
--
-- The `object_storage_cluster_local_read_max_tasks` probe lists objects while planning. `DESCRIBE` and `EXPLAIN`
-- never read the data, so they must not list: with `s3_throw_on_zero_files_match` a glob without files would
-- make them fail, as it does the query itself. The setting is given in the query, as a user does it.

SELECT 'describe';
DESCRIBE (
    SELECT count()
    FROM s3('http://localhost:11111/test/05817_object_storage_cluster_local_read_no_io_*.tsv', 'TSV', 'x UInt32')
    SETTINGS
        s3_throw_on_zero_files_match = 1,
        object_storage_cluster = 'test_shard_localhost',
        object_storage_cluster_local_read_max_tasks = 1,
        object_storage_cluster_local_read_probe_ms = 5000
) FORMAT Null;

SELECT 'explain plan';
SELECT count() > 0 FROM (
    EXPLAIN PLAN
    SELECT count()
    FROM s3('http://localhost:11111/test/05817_object_storage_cluster_local_read_no_io_*.tsv', 'TSV', 'x UInt32')
    SETTINGS
        s3_throw_on_zero_files_match = 1,
        object_storage_cluster = 'test_shard_localhost',
        object_storage_cluster_local_read_max_tasks = 1,
        object_storage_cluster_local_read_probe_ms = 5000
);

SELECT 'explain pipeline';
SELECT count() > 0 FROM (
    EXPLAIN PIPELINE
    SELECT count()
    FROM s3('http://localhost:11111/test/05817_object_storage_cluster_local_read_no_io_*.tsv', 'TSV', 'x UInt32')
    SETTINGS
        s3_throw_on_zero_files_match = 1,
        object_storage_cluster = 'test_shard_localhost',
        object_storage_cluster_local_read_max_tasks = 1,
        object_storage_cluster_local_read_probe_ms = 5000
);

-- The query itself still lists and fails on a glob without files.
SELECT 'select';
SELECT count()
FROM s3('http://localhost:11111/test/05817_object_storage_cluster_local_read_no_io_*.tsv', 'TSV', 'x UInt32')
SETTINGS
    s3_throw_on_zero_files_match = 1,
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 1,
    object_storage_cluster_local_read_probe_ms = 5000; -- { serverError FILE_DOESNT_EXIST }
