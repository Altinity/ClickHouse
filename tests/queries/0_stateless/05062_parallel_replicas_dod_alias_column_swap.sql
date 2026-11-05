DROP TABLE IF EXISTS test_pr_dod_alias_swap_outer;
DROP TABLE IF EXISTS test_pr_dod_alias_swap_inner;
DROP TABLE IF EXISTS test_pr_dod_alias_swap_local;

CREATE TABLE test_pr_dod_alias_swap_local
(
    x UInt64
)
ENGINE = MergeTree()
ORDER BY x;

INSERT INTO test_pr_dod_alias_swap_local VALUES (1), (2), (10);

CREATE TABLE test_pr_dod_alias_swap_inner
(
    x UInt64,
    inner_c UInt64 ALIAS x + 1
)
ENGINE = Distributed(test_cluster_one_shard_three_replicas_localhost, currentDatabase(), test_pr_dod_alias_swap_local);

CREATE TABLE test_pr_dod_alias_swap_outer
(
    x UInt64,
    inner_c UInt64,
    a_num UInt64 ALIAS 1,
    a_str String ALIAS 'aaaa'
)
ENGINE = Distributed(test_cluster_one_shard_three_replicas_localhost, currentDatabase(), test_pr_dod_alias_swap_inner);

SELECT 'no_pr_uint64';
SELECT x, a_num, inner_c
FROM test_pr_dod_alias_swap_outer
ORDER BY x
SETTINGS
    enable_analyzer = 1,
    enable_alias_marker = 0,
    enable_parallel_replicas = 0,
    max_parallel_replicas = 1,
    parallel_replicas_local_plan = 0,
    parallel_replicas_for_non_replicated_merge_tree = 1,
    cluster_for_parallel_replicas = 'test_cluster_one_shard_three_replicas_localhost'
FORMAT TSVWithNames;

SELECT 'no_pr_string';
SELECT x, a_str, inner_c
FROM test_pr_dod_alias_swap_outer
ORDER BY x
SETTINGS
    enable_analyzer = 1,
    enable_alias_marker = 0,
    enable_parallel_replicas = 0,
    max_parallel_replicas = 1,
    parallel_replicas_local_plan = 0,
    parallel_replicas_for_non_replicated_merge_tree = 1,
    cluster_for_parallel_replicas = 'test_cluster_one_shard_three_replicas_localhost'
FORMAT TSVWithNames;

-- Serialized `Distributed` plans bypass parallel replicas; automatic mode 2 collects statistics only.
-- Pin both for queries whose replica participation is checked below.
SELECT 'pr_uint64';
SELECT x, a_num, inner_c
FROM test_pr_dod_alias_swap_outer
ORDER BY x
SETTINGS
    enable_analyzer = 1,
    enable_alias_marker = 0,
    enable_parallel_replicas = 2,
    max_parallel_replicas = 3,
    parallel_replicas_local_plan = 1,
    parallel_replicas_for_non_replicated_merge_tree = 1,
    parallel_replicas_min_number_of_rows_per_replica = 0,
    automatic_parallel_replicas_mode = 0,
    cluster_for_parallel_replicas = 'test_cluster_one_shard_three_replicas_localhost',
    serialize_query_plan = 0,
    log_queries = 1,
    log_comment = '05062_pr_uint64'
FORMAT TSVWithNames;

SELECT 'pr_string';
SELECT x, a_str, inner_c
FROM test_pr_dod_alias_swap_outer
ORDER BY x
SETTINGS
    enable_analyzer = 1,
    enable_alias_marker = 0,
    enable_parallel_replicas = 2,
    max_parallel_replicas = 3,
    parallel_replicas_local_plan = 1,
    parallel_replicas_for_non_replicated_merge_tree = 1,
    parallel_replicas_min_number_of_rows_per_replica = 0,
    automatic_parallel_replicas_mode = 0,
    cluster_for_parallel_replicas = 'test_cluster_one_shard_three_replicas_localhost',
    serialize_query_plan = 0,
    log_queries = 1,
    log_comment = '05062_pr_string'
FORMAT TSVWithNames;

SYSTEM FLUSH LOGS query_log;
-- Check the remote legs as well as the initiator: a correct result alone does not prove parallel reading.
SELECT 'parallel_replicas_used';
SELECT log_comment,
       countIf(ProfileEvents['ParallelReplicasQueryCount'] > 0) > 0,
       countIf(ProfileEvents['ParallelReplicasUsedCount'] > 0) > 0
FROM system.query_log
WHERE type = 'QueryFinish' AND is_initial_query = 0
  AND initial_query_id IN
  (
      SELECT query_id FROM system.query_log
      WHERE type = 'QueryFinish' AND is_initial_query
        AND current_database = currentDatabase()
        AND log_comment IN ('05062_pr_uint64', '05062_pr_string')
  )
GROUP BY log_comment ORDER BY log_comment;

DROP TABLE test_pr_dod_alias_swap_outer;
DROP TABLE test_pr_dod_alias_swap_inner;
DROP TABLE test_pr_dod_alias_swap_local;
