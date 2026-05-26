-- Plain Distributed + parallel replicas (no Hybrid). Exercises the findParallelReplicasQuery
-- header reconciliation path with nested ALIAS columns. Correct result equals the single-node
-- ('local') result for both AST and serialized-plan transport.
DROP TABLE IF EXISTS t_local_03931;
DROP TABLE IF EXISTS t_dist_03931;

CREATE TABLE t_local_03931 (x UInt32, a1 UInt32 ALIAS x + 1, a2 UInt32 ALIAS a1 + 1)
ENGINE = MergeTree ORDER BY x;
INSERT INTO t_local_03931 VALUES (10), (20);

CREATE TABLE t_dist_03931 AS t_local_03931
ENGINE = Distributed(test_cluster_one_shard_three_replicas_localhost, currentDatabase(), t_local_03931);

SELECT 'local';
SELECT a1, a2 FROM t_local_03931 ORDER BY a1;

SELECT 'pr_ast';
SELECT a1, a2 FROM t_dist_03931 ORDER BY a1
SETTINGS enable_analyzer = 1, enable_alias_marker = 1,
         allow_experimental_parallel_reading_from_replicas = 1, max_parallel_replicas = 3,
         cluster_for_parallel_replicas = 'test_cluster_one_shard_three_replicas_localhost',
         serialize_query_plan = 0;

SELECT 'pr_plan';
SELECT a1, a2 FROM t_dist_03931 ORDER BY a1
SETTINGS enable_analyzer = 1, enable_alias_marker = 1,
         allow_experimental_parallel_reading_from_replicas = 1, max_parallel_replicas = 3,
         cluster_for_parallel_replicas = 'test_cluster_one_shard_three_replicas_localhost',
         serialize_query_plan = 1;

DROP TABLE t_dist_03931;
DROP TABLE t_local_03931;
