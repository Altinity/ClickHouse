-- Tags: no-fasttest
-- no-fasttest: uses the S3 mock server on localhost:11111

-- `PREWHERE` filters the left table before a `RIGHT` join. Dropping it from the shard
-- scan keeps the other left row, so the matched right row is emitted twice.

CREATE TEMPORARY TABLE t_dim_05391 (k UInt64, name String);
INSERT INTO t_dim_05391 VALUES (1, 'a'), (2, 'b');

INSERT INTO FUNCTION s3('http://localhost:11111/test/05391_' || currentDatabase() || '.parquet', 'test', 'testtest', 'Parquet', 'k UInt64, v String')
SELECT k, v FROM
(
    SELECT 1 AS k, 'L' AS v
    UNION ALL SELECT 1, 'M'
)
SETTINGS s3_truncate_on_insert = 1;

SELECT t1.k, t1.v, t2.name
FROM s3Cluster('test_shard_localhost', 'http://localhost:11111/test/05391_' || currentDatabase() || '.parquet', 'test', 'testtest', 'Parquet', 'k UInt64, v String') AS t1
RIGHT JOIN t_dim_05391 AS t2 ON t1.k = t2.k
PREWHERE t1.v = 'L'
ORDER BY t2.name, t1.v
SETTINGS allow_experimental_analyzer = 0, object_storage_cluster_join_mode = 'allow';

DROP TEMPORARY TABLE t_dim_05391;
