-- Tags: no-fasttest
-- no-fasttest: uses the S3 mock server on localhost:11111

-- `FROM t ARRAY JOIN arr INNER JOIN dim` is one join. The shard scan must drop the join
-- instead of throwing because the second table element is the `ARRAY JOIN`.

CREATE TEMPORARY TABLE t_dim_05391 (k UInt64, name String);
INSERT INTO t_dim_05391 VALUES (1, 'a');

INSERT INTO FUNCTION s3('http://localhost:11111/test/05391_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, arr Array(String)')
SELECT 1, ['x', 'y']
SETTINGS s3_truncate_on_insert = 1;

SELECT elem, t2.name
FROM s3Cluster('test_shard_localhost', 'http://localhost:11111/test/05391_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, arr Array(String)') AS t1
ARRAY JOIN arr AS elem
INNER JOIN t_dim_05391 AS t2 ON t1.k = t2.k
ORDER BY elem
SETTINGS allow_experimental_analyzer = 0, object_storage_cluster_join_mode = 'allow';

DROP TEMPORARY TABLE t_dim_05391;
