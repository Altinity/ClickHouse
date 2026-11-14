-- Tags: no-fasttest
-- no-fasttest: uses the S3 mock server on localhost:11111

-- `object_storage_cluster_join_mode = 'distributed'` sends a whole `JOIN` only when its tables come from a `DataLake`
-- catalog. Any other query, such as this `s3Cluster` one, runs in the mode set by
-- `object_storage_cluster_join_mode_fallback`: `allow` keeps the `JOIN` on the initiator, `global` sends it to the
-- nodes together with the right table. Every mode returns the same rows; only the query the nodes receive differs.

DROP TABLE IF EXISTS t_dim_05484;
CREATE TABLE t_dim_05484 (k UInt64, name String) ENGINE = Memory;
INSERT INTO t_dim_05484 VALUES (1, 'a'), (2, 'b'), (5, 'c');

INSERT INTO FUNCTION s3('http://localhost:11111/test/05484_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String')
SELECT number, toString(number) FROM numbers(4)
SETTINGS s3_truncate_on_insert = 1;

SELECT 'allow';
SELECT t1.k, t1.v, t2.name
FROM s3Cluster('test_cluster_two_shards', 'http://localhost:11111/test/05484_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
INNER JOIN t_dim_05484 AS t2 ON t1.k = t2.k
ORDER BY t1.k
SETTINGS object_storage_cluster_join_mode = 'allow', log_comment = '05484_allow';

SELECT 'global';
SELECT t1.k, t1.v, t2.name
FROM s3Cluster('test_cluster_two_shards', 'http://localhost:11111/test/05484_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
INNER JOIN t_dim_05484 AS t2 ON t1.k = t2.k
ORDER BY t1.k
SETTINGS object_storage_cluster_join_mode = 'global', log_comment = '05484_global';

SELECT 'distributed, default fallback';
SELECT t1.k, t1.v, t2.name
FROM s3Cluster('test_cluster_two_shards', 'http://localhost:11111/test/05484_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
INNER JOIN t_dim_05484 AS t2 ON t1.k = t2.k
ORDER BY t1.k
SETTINGS object_storage_cluster_join_mode = 'distributed', log_comment = '05484_distributed_default';

SELECT 'distributed, fallback allow';
SELECT t1.k, t1.v, t2.name
FROM s3Cluster('test_cluster_two_shards', 'http://localhost:11111/test/05484_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
INNER JOIN t_dim_05484 AS t2 ON t1.k = t2.k
ORDER BY t1.k
SETTINGS object_storage_cluster_join_mode = 'distributed', object_storage_cluster_join_mode_fallback = 'allow', log_comment = '05484_distributed_allow';

SELECT 'distributed, fallback global';
SELECT t1.k, t1.v, t2.name
FROM s3Cluster('test_cluster_two_shards', 'http://localhost:11111/test/05484_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
INNER JOIN t_dim_05484 AS t2 ON t1.k = t2.k
ORDER BY t1.k
SETTINGS object_storage_cluster_join_mode = 'distributed', object_storage_cluster_join_mode_fallback = 'global', log_comment = '05484_distributed_global';

SELECT 'distributed, fallback global, old analyzer';
SELECT t1.k, t1.v, t2.name
FROM s3Cluster('test_cluster_two_shards', 'http://localhost:11111/test/05484_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
INNER JOIN t_dim_05484 AS t2 ON t1.k = t2.k
ORDER BY t1.k
SETTINGS allow_experimental_analyzer = 0, object_storage_cluster_join_mode = 'distributed', object_storage_cluster_join_mode_fallback = 'global', log_comment = '05484_distributed_global_old_analyzer';

-- `distributed` is not a mode to fall back to.
SELECT t1.k
FROM s3Cluster('test_cluster_two_shards', 'http://localhost:11111/test/05484_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String') AS t1
INNER JOIN t_dim_05484 AS t2 ON t1.k = t2.k
SETTINGS object_storage_cluster_join_mode = 'distributed', object_storage_cluster_join_mode_fallback = 'distributed'; -- { serverError BAD_ARGUMENTS }

-- Whether the query each node received contains the `JOIN`.
SYSTEM FLUSH LOGS query_log;
SELECT initial.log_comment, max(positionCaseInsensitive(secondary.query, 'JOIN') > 0) AS join_sent_to_nodes
FROM system.query_log AS secondary
INNER JOIN
(
    SELECT query_id, log_comment
    FROM system.query_log
    WHERE type = 'QueryFinish' AND is_initial_query AND current_database = currentDatabase() AND log_comment LIKE '05484_%'
) AS initial ON secondary.initial_query_id = initial.query_id
WHERE secondary.type = 'QueryFinish' AND NOT secondary.is_initial_query
GROUP BY initial.log_comment
ORDER BY initial.log_comment;

DROP TABLE t_dim_05484;
