-- Tags: no-fasttest
-- Tag no-fasttest: Depends on Minio
--
-- A brace glob lists a file that does not exist. With `s3_ignore_file_doesnt_exist` the existing file must be read
-- whichever way the query is planned, also when the missing file comes first. The local read after the
-- `object_storage_cluster_local_read_max_tasks` probe used to end its stream at the missing file and return no rows.

INSERT INTO FUNCTION s3('http://localhost:11111/test/05820_object_storage_cluster_local_read_ignore_missing_file_a.tsv', 'TSV', 'x UInt32')
SELECT 1
SETTINGS s3_truncate_on_insert = 1;

SELECT 'no cluster';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05820_object_storage_cluster_local_read_ignore_missing_file_{missing,a}.tsv', 'TSV', 'x UInt32')
SETTINGS
    s3_ignore_file_doesnt_exist = 1,
    max_threads = 1;

SELECT 'local read after the probe';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05820_object_storage_cluster_local_read_ignore_missing_file_{missing,a}.tsv', 'TSV', 'x UInt32')
SETTINGS
    s3_ignore_file_doesnt_exist = 1,
    max_threads = 1,
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 10,
    object_storage_cluster_local_read_probe_ms = 5000;
