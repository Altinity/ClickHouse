-- Tags: no-fasttest
-- no-fasttest: uses the S3 mock server on localhost:11111

-- Analyzer. `SELECT 1 FROM t_dim` references a temporary table and no column of it.
-- That still has to stay on the initiator; workers cannot resolve the temporary table.

CREATE TEMPORARY TABLE t_dim_05390 (k UInt64, name String);
INSERT INTO t_dim_05390 VALUES (1, 'a');

INSERT INTO FUNCTION s3('http://localhost:11111/test/05390_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String')
SELECT k, v FROM
(
    SELECT 1 AS k, 'x' AS v
    UNION ALL SELECT 1, 'x'
    UNION ALL SELECT 2, 'y'
)
SETTINGS s3_truncate_on_insert = 1;

SELECT 'no-column';
SELECT t1.k, t1.v
FROM s3Cluster('test_shard_localhost', 'http://localhost:11111/test/05390_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
WHERE t1.k IN (SELECT 1 FROM t_dim_05390 LIMIT 1)
ORDER BY t1.k, t1.v
SETTINGS allow_experimental_analyzer = 1, object_storage_cluster_join_mode = 'allow';

SELECT 'column';
SELECT t1.k, t1.v
FROM s3Cluster('test_shard_localhost', 'http://localhost:11111/test/05390_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
WHERE t1.k IN (SELECT k FROM t_dim_05390)
ORDER BY t1.k, t1.v
SETTINGS allow_experimental_analyzer = 1, object_storage_cluster_join_mode = 'allow';

DROP TEMPORARY TABLE t_dim_05390;
