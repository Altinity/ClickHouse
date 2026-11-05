-- Tags: no-fasttest
-- no-fasttest: uses the S3 mock server on localhost:11111

-- No explicit JOIN. The `IN` subquery references a temporary table, so the shard query drops
-- `GROUP BY`. The `WITH TOTALS` flag has to be dropped with it; otherwise the shard query is
-- rejected and the initiator never sees the rows.

CREATE TEMPORARY TABLE t_dim_05389 (k UInt64);
INSERT INTO t_dim_05389 VALUES (1);

INSERT INTO FUNCTION s3('http://localhost:11111/test/05389_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String')
SELECT k, v FROM
(
    SELECT 1 AS k, 'x' AS v
    UNION ALL SELECT 1, 'x'
    UNION ALL SELECT 2, 'y'
)
SETTINGS s3_truncate_on_insert = 1;

SELECT t1.k, count()
FROM s3Cluster('test_shard_localhost', 'http://localhost:11111/test/05389_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
WHERE t1.k IN (SELECT k FROM t_dim_05389)
GROUP BY t1.k WITH TOTALS
ORDER BY t1.k
SETTINGS allow_experimental_analyzer = 0, object_storage_cluster_join_mode = 'allow';

DROP TEMPORARY TABLE t_dim_05389;
