-- The `Distributed` queries check nested `ALIAS` columns in AST and serialized-plan transport.
-- The nested queries over `MergeTree` exercise `buildQueryPlanForParallelReplicas` and its
-- `Convert distributed names` header reconciliation in both transports. A serialized
-- `Distributed` plan is built before the shard can enable parallel replicas.
--
-- Parallel replicas over a small non-replicated table can read the same rows on several replicas.
-- `GROUP BY x, a1, a2` deduplicates them and keeps x required for the `ALIAS` expansion;
-- `ORDER BY x` over distinct values gives a total order. Swapped or wrong aliases still fail.
DROP TABLE IF EXISTS t_local_03931;
DROP TABLE IF EXISTS t_dist_03931;

CREATE TABLE t_local_03931 (x UInt32, a1 UInt32 ALIAS x + 1, a2 UInt32 ALIAS a1 + 1)
ENGINE = MergeTree ORDER BY x;
INSERT INTO t_local_03931 VALUES (10), (20);

CREATE TABLE t_dist_03931 AS t_local_03931
ENGINE = Distributed(test_cluster_one_shard_three_replicas_localhost, currentDatabase(), t_local_03931);

SELECT 'local';
SELECT x, a1, a2 FROM t_local_03931 GROUP BY x, a1, a2 ORDER BY x;

SET enable_analyzer = 1, enable_alias_marker = 1;
SELECT 'dist_plan';
SELECT x, a1, a2 FROM t_dist_03931 GROUP BY x, a1, a2 ORDER BY x
SETTINGS serialize_query_plan = 1;

SET allow_experimental_parallel_reading_from_replicas = 2, max_parallel_replicas = 3,
    parallel_replicas_for_non_replicated_merge_tree = 1,
    parallel_replicas_min_number_of_rows_per_replica = 0,
    automatic_parallel_replicas_mode = 0,
    cluster_for_parallel_replicas = 'test_cluster_one_shard_three_replicas_localhost',
    serialize_query_plan = 0;

SELECT 'pr_ast';
SELECT x, a1, a2 FROM t_dist_03931 GROUP BY x, a1, a2 ORDER BY x
SETTINGS log_comment = '03931_dist_ast';

-- The subquery makes the query-level parallel-replica candidate eligible. Check the
-- distinctive reconciliation and remote-reading steps, not just the resulting values.
SELECT 'pr_query_ast';
SELECT countIf(explain LIKE '%Convert distributed names%') > 0,
       countIf(explain LIKE '%ReadFromRemoteParallelReplicas%') > 0
FROM (EXPLAIN PLAN SELECT x, a1, a2 FROM (SELECT x, a1, a2 FROM t_local_03931)
      GROUP BY x, a1, a2 ORDER BY x);
SELECT x, a1, a2 FROM (SELECT x, a1, a2 FROM t_local_03931)
GROUP BY x, a1, a2 ORDER BY x
SETTINGS log_comment = '03931_query_ast';

SET serialize_query_plan = 1;
SELECT 'pr_query_plan';
SELECT countIf(explain LIKE '%Convert distributed names%') > 0,
       countIf(explain LIKE '%ReadFromRemoteParallelReplicas%') > 0
FROM (EXPLAIN PLAN SELECT x, a1, a2 FROM (SELECT x, a1, a2 FROM t_local_03931)
      GROUP BY x, a1, a2 ORDER BY x);
SELECT x, a1, a2 FROM (SELECT x, a1, a2 FROM t_local_03931)
GROUP BY x, a1, a2 ORDER BY x
SETTINGS log_comment = '03931_query_plan';

SET allow_experimental_parallel_reading_from_replicas = 0, serialize_query_plan = 0;
SYSTEM FLUSH LOGS query_log;
-- The `Distributed` initiator does not itself count the replica reads. Correlate its
-- `initial_query_id` with secondary queries instead of checking only the initial row.
SELECT '03931_dist_ast', countIf(ProfileEvents['ParallelReplicasQueryCount'] > 0) > 0,
       countIf(ProfileEvents['ParallelReplicasUsedCount'] > 0) > 0
FROM system.query_log
WHERE event_date >= yesterday() AND type = 'QueryFinish'
  AND initial_query_id IN
  (
      SELECT query_id FROM system.query_log
      WHERE event_date >= yesterday() AND type = 'QueryFinish' AND is_initial_query
        AND current_database = currentDatabase() AND log_comment = '03931_dist_ast'
  );
SELECT log_comment, ProfileEvents['ParallelReplicasQueryCount'] > 0,
       ProfileEvents['ParallelReplicasUsedCount'] > 0
FROM system.query_log
WHERE event_date >= yesterday() AND type = 'QueryFinish'
  AND is_initial_query AND current_database = currentDatabase()
  AND log_comment IN ('03931_query_ast', '03931_query_plan')
ORDER BY log_comment;

DROP TABLE t_dist_03931;
DROP TABLE t_local_03931;
