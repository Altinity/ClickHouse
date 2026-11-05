-- Tags: no-fasttest
-- no-fasttest: uses the S3 mock server on localhost:11111

-- Old analyzer, `s3Cluster` joined to a table that exists only as far as the initiator is concerned.
-- `ORDER BY ALL` must not null-dereference `orderBy` after the join is stripped, and `GROUP BY ALL`
-- must count joined rows. Two identical left rows for key 1 make a shard-side `GROUP BY` visible:
-- it would report `x` once instead of twice.

DROP TABLE IF EXISTS t_dim;

CREATE TABLE t_dim (k UInt64, name String) ENGINE = Memory;
INSERT INTO t_dim VALUES (1, 'a'), (2, 'b');

INSERT INTO FUNCTION s3('http://localhost:11111/test/05027_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String')
SELECT k, v FROM
(
    SELECT 1 AS k, 'x' AS v
    UNION ALL SELECT 1, 'x'
    UNION ALL SELECT 2, 'y'
)
SETTINGS s3_truncate_on_insert = 1;

SELECT t1.k, t1.v, t2.name
FROM s3Cluster('test_shard_localhost', 'http://localhost:11111/test/05027_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
INNER JOIN t_dim AS t2 ON t1.k = t2.k
ORDER BY ALL
SETTINGS allow_experimental_analyzer = 0, object_storage_cluster_join_mode = 'allow';

SELECT t1.v, count()
FROM s3Cluster('test_shard_localhost', 'http://localhost:11111/test/05027_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
INNER JOIN t_dim AS t2 ON t1.k = t2.k
GROUP BY ALL
ORDER BY ALL
SETTINGS allow_experimental_analyzer = 0, object_storage_cluster_join_mode = 'allow';

SELECT t1.k, t1.v, t2.name
FROM s3Cluster('test_shard_localhost', 'http://localhost:11111/test/05027_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
INNER JOIN t_dim AS t2 ON t1.k = t2.k
ORDER BY ALL
SETTINGS allow_experimental_analyzer = 0, object_storage_cluster_join_mode = 'local';

SELECT t1.v, count()
FROM s3Cluster('test_shard_localhost', 'http://localhost:11111/test/05027_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
INNER JOIN t_dim AS t2 ON t1.k = t2.k
GROUP BY ALL
ORDER BY ALL
SETTINGS allow_experimental_analyzer = 0, object_storage_cluster_join_mode = 'local';

DROP TABLE t_dim;
