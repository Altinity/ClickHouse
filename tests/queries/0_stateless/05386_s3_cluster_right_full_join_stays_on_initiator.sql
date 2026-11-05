-- Tags: no-fasttest
-- no-fasttest: uses the S3 mock server on localhost:11111

-- Old analyzer, `object_storage_cluster_join_mode = 'global'`.
-- `RIGHT` and `FULL` preserve the right side, so they must run on the initiator.
-- Two shards and one file: a shard that reads no files would still emit every preserved
-- right row if the join were broadcast. The right table is temporary, so only the
-- initiator session can see it.

DROP TABLE IF EXISTS t_dim_05386;
CREATE TEMPORARY TABLE t_dim_05386 (k UInt64, name String);
INSERT INTO t_dim_05386 VALUES (1, 'a'), (2, 'b');

INSERT INTO FUNCTION s3('http://localhost:11111/test/05386_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String')
SELECT k, v FROM
(
    SELECT 1 AS k, 'x' AS v
    UNION ALL SELECT 1, 'x'
)
SETTINGS s3_truncate_on_insert = 1;

SELECT 'RIGHT';
SELECT t1.k, t1.v, t2.name
FROM s3Cluster('test_cluster_two_shards', 'http://localhost:11111/test/05386_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
RIGHT JOIN t_dim_05386 AS t2 ON t1.k = t2.k
ORDER BY t2.name, t1.v
SETTINGS allow_experimental_analyzer = 0, object_storage_cluster_join_mode = 'global';

SELECT 'FULL';
SELECT t1.k, t1.v, t2.name
FROM s3Cluster('test_cluster_two_shards', 'http://localhost:11111/test/05386_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
FULL JOIN t_dim_05386 AS t2 ON t1.k = t2.k
ORDER BY t2.name, t1.v
SETTINGS allow_experimental_analyzer = 0, object_storage_cluster_join_mode = 'global';

SELECT 'INNER';
SELECT t1.k, t1.v, t2.name
FROM s3Cluster('test_cluster_two_shards', 'http://localhost:11111/test/05386_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
INNER JOIN t_dim_05386 AS t2 ON t1.k = t2.k
ORDER BY t1.v
SETTINGS allow_experimental_analyzer = 0, object_storage_cluster_join_mode = 'global';

DROP TEMPORARY TABLE t_dim_05386;
