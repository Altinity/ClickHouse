-- Tags: no-fasttest
-- no-fasttest: uses the S3 mock server on localhost:11111

-- `object_storage_remote_initiator` analyzes the query on another host.
-- `allow` strips the JOIN into `query_to_send`; that stripped query must be what the remote host
-- runs. The right table is temporary, so it exists only in this session.

CREATE TEMPORARY TABLE t_dim_05387 (k UInt64, name String);
INSERT INTO t_dim_05387 VALUES (1, 'a'), (2, 'b');

INSERT INTO FUNCTION s3('http://localhost:11111/test/05387_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String')
SELECT k, v FROM
(
    SELECT 1 AS k, 'x' AS v
    UNION ALL SELECT 1, 'x'
    UNION ALL SELECT 2, 'y'
)
SETTINGS s3_truncate_on_insert = 1;

SELECT 'old';
SELECT t1.k, t1.v, t2.name
FROM s3Cluster('test_shard_localhost', 'http://localhost:11111/test/05387_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
INNER JOIN t_dim_05387 AS t2 ON t1.k = t2.k
ORDER BY t1.k, t1.v
SETTINGS
    allow_experimental_analyzer = 0,
    object_storage_cluster_join_mode = 'allow',
    object_storage_remote_initiator = 1,
    object_storage_remote_initiator_cluster = 'test_shard_localhost';

SELECT 'analyzer';
SELECT t1.k, t1.v, t2.name
FROM s3Cluster('test_shard_localhost', 'http://localhost:11111/test/05387_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
INNER JOIN t_dim_05387 AS t2 ON t1.k = t2.k
ORDER BY t1.k, t1.v
SETTINGS
    allow_experimental_analyzer = 1,
    object_storage_cluster_join_mode = 'allow',
    object_storage_remote_initiator = 1,
    object_storage_remote_initiator_cluster = 'test_shard_localhost',
    -- A serialized plan carries the `s3Cluster` arguments with the secret hidden.
    serialize_query_plan = 0;

DROP TEMPORARY TABLE t_dim_05387;
