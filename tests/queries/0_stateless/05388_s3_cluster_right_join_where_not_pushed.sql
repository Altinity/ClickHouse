-- Tags: no-fasttest
-- no-fasttest: uses the S3 mock server on localhost:11111

-- A left-only predicate must not run before a `RIGHT` join. With `join_use_nulls = 1`,
-- `WHERE isNull(t1.k)` is true for unmatched right rows. Filtering the matching left row
-- on the shard turns the matched right row into another unmatched row.

CREATE TEMPORARY TABLE t_dim_05388 (k Nullable(Int32), name String);
INSERT INTO t_dim_05388 VALUES (1, 'matched'), (2, 'unmatched');

INSERT INTO FUNCTION s3('http://localhost:11111/test/05388_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k Nullable(Int32), v String')
SELECT k, v FROM
(
    SELECT CAST(1, 'Nullable(Int32)') AS k, 'L' AS v
    UNION ALL SELECT CAST(1, 'Nullable(Int32)'), 'M'
    UNION ALL SELECT CAST(NULL, 'Nullable(Int32)'), 'N'
)
SETTINGS s3_truncate_on_insert = 1;

SELECT 'RIGHT';
SELECT t1.k, t1.v, t2.k, t2.name
FROM s3Cluster('test_shard_localhost', 'http://localhost:11111/test/05388_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k Nullable(Int32), v String') AS t1
RIGHT JOIN t_dim_05388 AS t2 ON t1.k = t2.k
WHERE isNull(t1.k)
ORDER BY t2.k, t1.v
SETTINGS allow_experimental_analyzer = 0, object_storage_cluster_join_mode = 'allow', join_use_nulls = 1;

SELECT 'INNER';
SELECT t1.k, t1.v, t2.k, t2.name
FROM s3Cluster('test_shard_localhost', 'http://localhost:11111/test/05388_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k Nullable(Int32), v String') AS t1
INNER JOIN t_dim_05388 AS t2 ON t1.k = t2.k
WHERE t1.v = 'L'
ORDER BY t1.v
SETTINGS allow_experimental_analyzer = 0, object_storage_cluster_join_mode = 'allow', join_use_nulls = 1;

DROP TEMPORARY TABLE t_dim_05388;
