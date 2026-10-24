DROP TABLE IF EXISTS test_dod_alias_swap_no_marker_outer;
DROP TABLE IF EXISTS test_dod_alias_swap_no_marker_inner;
DROP TABLE IF EXISTS test_dod_alias_swap_no_marker_local;

CREATE TABLE test_dod_alias_swap_no_marker_local
(
    x UInt64
)
ENGINE = MergeTree()
ORDER BY x;

INSERT INTO test_dod_alias_swap_no_marker_local VALUES (1), (2), (10);

CREATE TABLE test_dod_alias_swap_no_marker_inner
(
    x UInt64,
    inner_c UInt64 ALIAS x + 1
)
ENGINE = Distributed(test_cluster_two_shards, currentDatabase(), test_dod_alias_swap_no_marker_local);

CREATE TABLE test_dod_alias_swap_no_marker_outer
(
    x UInt64,
    inner_c UInt64,
    a_num UInt64 ALIAS 1,
    a_str String ALIAS 'aaaa'
)
ENGINE = Distributed(test_cluster_two_shards, currentDatabase(), test_dod_alias_swap_no_marker_inner);

SELECT 'prefer_localhost_replica_0_uint64';
SELECT
    x,
    a_num,
    inner_c
FROM test_dod_alias_swap_no_marker_outer
ORDER BY x
SETTINGS
    allow_experimental_analyzer = 1,
    enable_alias_marker = 0,
    prefer_localhost_replica = 0,
    enable_parallel_replicas = 0,
    max_parallel_replicas = 1,
    parallel_replicas_local_plan = 0
FORMAT TSVWithNames;

SELECT 'prefer_localhost_replica_0_string';
SELECT
    x,
    a_str,
    inner_c
FROM test_dod_alias_swap_no_marker_outer
ORDER BY x
SETTINGS
    allow_experimental_analyzer = 1,
    enable_alias_marker = 0,
    prefer_localhost_replica = 0,
    enable_parallel_replicas = 0,
    max_parallel_replicas = 1,
    parallel_replicas_local_plan = 0
FORMAT TSVWithNames;

SELECT 'prefer_localhost_replica_1_uint64';
SELECT
    x,
    a_num,
    inner_c
FROM test_dod_alias_swap_no_marker_outer
ORDER BY x
SETTINGS
    allow_experimental_analyzer = 1,
    enable_alias_marker = 0,
    prefer_localhost_replica = 1,
    enable_parallel_replicas = 0,
    max_parallel_replicas = 1,
    parallel_replicas_local_plan = 0
FORMAT TSVWithNames;

SELECT 'prefer_localhost_replica_1_string';
SELECT
    x,
    a_str,
    inner_c
FROM test_dod_alias_swap_no_marker_outer
ORDER BY x
SETTINGS
    allow_experimental_analyzer = 1,
    enable_alias_marker = 0,
    prefer_localhost_replica = 1,
    enable_parallel_replicas = 0,
    max_parallel_replicas = 1,
    parallel_replicas_local_plan = 0
FORMAT TSVWithNames;

DROP TABLE test_dod_alias_swap_no_marker_outer;
DROP TABLE test_dod_alias_swap_no_marker_inner;
DROP TABLE test_dod_alias_swap_no_marker_local;
