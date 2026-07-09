-- Tags: no-fasttest
-- Tag no-fasttest: Depends on Minio

SET enable_analyzer = 1;

INSERT INTO FUNCTION s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SELECT number FROM numbers(10)
SETTINGS s3_truncate_on_insert = 1;

SELECT 'pure';
SELECT count(), sum(x) FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32');

SELECT 'unknown cluster without fallback';
SELECT count() FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS object_storage_cluster = 'non_existent_cluster_04303'; -- { serverError CLUSTER_DOESNT_EXIST }

SELECT 'unknown cluster with fallback';
SELECT count(), sum(x) FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS object_storage_cluster = 'non_existent_cluster_04303', object_storage_cluster_fallback_if_empty = 1;

SELECT 'valid cluster with fallback';
SELECT count(), sum(x) FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS object_storage_cluster = 'test_shard_localhost', object_storage_cluster_fallback_if_empty = 1;

SELECT 'explicit cluster function with fallback';
SELECT count() FROM s3Cluster('non_existent_cluster_04303', 'http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS object_storage_cluster_fallback_if_empty = 1; -- { serverError CLUSTER_DOESNT_EXIST }

SELECT 'explicit cluster function overrides setting';
SELECT count() FROM s3Cluster('non_existent_cluster_04303', 'http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS object_storage_cluster_fallback_if_empty = 1, object_storage_cluster = 'non_existent_cluster_04303_2'; -- { serverError CLUSTER_DOESNT_EXIST }

SELECT 'explicit cluster function overrides setting with valid cluster';
SELECT count(), sum(x) FROM s3Cluster('test_shard_localhost', 'http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS object_storage_cluster = 'non_existent_cluster_04303', object_storage_cluster_fallback_if_empty = 1;

SELECT 'remote initiator unresolved with fallback';
SELECT count(), sum(x) FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS
    object_storage_cluster = 'non_existent_cluster_04303',
    object_storage_cluster_fallback_if_empty = 1,
    object_storage_remote_initiator = 1;

SELECT 'remote initiator with non-existent cluster';
SELECT count(), sum(x) FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS
    object_storage_cluster = 'non_existent_cluster_04303',
    object_storage_cluster_fallback_if_empty = 1,
    object_storage_remote_initiator = 1,
    object_storage_remote_initiator_cluster = 'test_shard_localhost';

SELECT 'aggregate with fallback';
SELECT sum(x * x) FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS object_storage_cluster = 'non_existent_cluster_04303', object_storage_cluster_fallback_if_empty = 1;

SELECT 'pure aggregate';
SELECT sum(x * x) FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32');
