-- Tags: no-fasttest
-- Tag no-fasttest: Depends on Minio
--
-- object_storage_cluster_fallback_to_local_if_empty applies only to non-cluster table functions / table engines.
-- Intended behavior changes (setting=1):
--   1) OSC non-empty but unknown/empty cluster, RI=0 -> local instead of error
--   2) OSC non-empty but unknown/empty cluster, RI=1, RI-cluster empty -> local instead of error
--   3) local OSC empty, RI=1, valid RI-cluster; remote OSC unknown/empty -> remote non-distributed instead of error
-- Other cases must keep pre-setting behavior.

SET enable_analyzer = 1;

INSERT INTO FUNCTION s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SELECT number FROM numbers(10)
SETTINGS s3_truncate_on_insert = 1;

SELECT 'pure';
SELECT count(), sum(x) FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32');

SELECT 'case1 unknown OSC without fallback still errors';
SELECT count() FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS object_storage_cluster = 'non_existent_cluster_04303'; -- { serverError CLUSTER_DOESNT_EXIST }

SELECT 'case1 unknown OSC with fallback runs locally';
SELECT count(), sum(x) FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS object_storage_cluster = 'non_existent_cluster_04303', object_storage_cluster_fallback_to_local_if_empty = 1;

SELECT 'valid OSC with fallback unchanged';
SELECT count(), sum(x) FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS object_storage_cluster = 'test_shard_localhost', object_storage_cluster_fallback_to_local_if_empty = 1;

SELECT 's3Cluster ignores fallback';
SELECT count() FROM s3Cluster('non_existent_cluster_04303', 'http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS object_storage_cluster_fallback_to_local_if_empty = 1; -- { serverError CLUSTER_DOESNT_EXIST }

SELECT 's3Cluster ignores fallback even with OSC setting';
SELECT count() FROM s3Cluster('non_existent_cluster_04303', 'http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS object_storage_cluster_fallback_to_local_if_empty = 1, object_storage_cluster = 'non_existent_cluster_04303_2'; -- { serverError CLUSTER_DOESNT_EXIST }

SELECT 's3Cluster with valid argument unchanged';
SELECT count(), sum(x) FROM s3Cluster('test_shard_localhost', 'http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS object_storage_cluster = 'non_existent_cluster_04303', object_storage_cluster_fallback_to_local_if_empty = 1;

SELECT 'case2 unknown OSC + RI without RI-cluster with fallback runs locally';
SELECT count(), sum(x) FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS
    object_storage_cluster = 'non_existent_cluster_04303',
    object_storage_cluster_fallback_to_local_if_empty = 1,
    object_storage_remote_initiator = 1;

SELECT 'empty OSC + RI without RI-cluster with fallback still BAD_ARGUMENTS';
SELECT count() FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS
    object_storage_remote_initiator = 1,
    object_storage_cluster_fallback_to_local_if_empty = 1; -- { serverError BAD_ARGUMENTS }

SELECT 'empty OSC + RI without RI-cluster without fallback still BAD_ARGUMENTS';
SELECT count() FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS object_storage_remote_initiator = 1; -- { serverError BAD_ARGUMENTS }

SELECT 'case3-like unknown OSC + RI-cluster with fallback (pure send, remote local)';
SELECT count(), sum(x) FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS
    object_storage_cluster = 'non_existent_cluster_04303',
    object_storage_cluster_fallback_to_local_if_empty = 1,
    object_storage_remote_initiator = 1,
    object_storage_remote_initiator_cluster = 'test_shard_localhost';

SELECT 'missing RI-cluster is not masked by OSC fallback';
SELECT count() FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_fallback_to_local_if_empty = 1,
    object_storage_remote_initiator = 1,
    object_storage_remote_initiator_cluster = 'non_existent_remote_initiator_04303'; -- { serverError CLUSTER_DOESNT_EXIST }

SELECT 'aggregate with fallback';
SELECT sum(x * x) FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32')
SETTINGS object_storage_cluster = 'non_existent_cluster_04303', object_storage_cluster_fallback_to_local_if_empty = 1;

SELECT 'pure aggregate';
SELECT sum(x * x) FROM s3('http://localhost:11111/test/04303_object_storage_cluster_fallback.tsv', 'TSV', 'x UInt32');
