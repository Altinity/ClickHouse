DROP TABLE IF EXISTS test_dod_alias_swap_local;
DROP TABLE IF EXISTS test_dod_alias_swap_inner;

CREATE TABLE test_dod_alias_swap_local
(
    x UInt64
)
ENGINE = MergeTree()
ORDER BY x;

INSERT INTO test_dod_alias_swap_local VALUES (1), (2), (10);

CREATE TABLE test_dod_alias_swap_inner
(
    x UInt64,
    a UInt64 ALIAS 2,
    inner_c UInt64 ALIAS x + 1
)
ENGINE = Distributed(test_cluster_two_shards, currentDatabase(), test_dod_alias_swap_local);

SELECT 'prefer_localhost_replica_0_uint64';
SELECT
    __aliasMarker(_CAST(1, 'UInt64'), '__table1.a') AS a,
    __table1.inner_c AS inner_c
FROM test_dod_alias_swap_inner AS __table1
ORDER BY __table1.x
SETTINGS
    enable_analyzer = 1,
    enable_alias_marker = 1,
    prefer_localhost_replica = 0,
    enable_parallel_replicas = 0,
    max_parallel_replicas = 1,
    parallel_replicas_local_plan = 0
FORMAT TSVWithNames;

SELECT 'prefer_localhost_replica_0_string';
SELECT
    __aliasMarker(_CAST('aaaa', 'String'), '__table1.a') AS a,
    __table1.inner_c AS inner_c
FROM test_dod_alias_swap_inner AS __table1
ORDER BY __table1.x
SETTINGS
    enable_analyzer = 1,
    enable_alias_marker = 1,
    prefer_localhost_replica = 0,
    enable_parallel_replicas = 0,
    max_parallel_replicas = 1,
    parallel_replicas_local_plan = 0
FORMAT TSVWithNames;

SELECT 'prefer_localhost_replica_1_uint64';
SELECT
    __aliasMarker(_CAST(1, 'UInt64'), '__table1.a') AS a,
    __table1.inner_c AS inner_c
FROM test_dod_alias_swap_inner AS __table1
ORDER BY __table1.x
SETTINGS
    enable_analyzer = 1,
    enable_alias_marker = 1,
    prefer_localhost_replica = 1,
    enable_parallel_replicas = 0,
    max_parallel_replicas = 1,
    parallel_replicas_local_plan = 0
FORMAT TSVWithNames;

SELECT 'prefer_localhost_replica_1_string';
SELECT
    __aliasMarker(_CAST('aaaa', 'String'), '__table1.a') AS a,
    __table1.inner_c AS inner_c
FROM test_dod_alias_swap_inner AS __table1
ORDER BY __table1.x
SETTINGS
    enable_analyzer = 1,
    enable_alias_marker = 1,
    prefer_localhost_replica = 1,
    enable_parallel_replicas = 0,
    max_parallel_replicas = 1,
    parallel_replicas_local_plan = 0
FORMAT TSVWithNames;

DROP TABLE test_dod_alias_swap_inner;
DROP TABLE test_dod_alias_swap_local;
