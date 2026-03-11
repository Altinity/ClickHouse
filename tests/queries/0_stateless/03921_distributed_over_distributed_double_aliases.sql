DROP TABLE IF EXISTS test_dod_double_alias_outer;
DROP TABLE IF EXISTS test_dod_double_alias_inner;
DROP TABLE IF EXISTS test_dod_double_alias_local;

CREATE TABLE test_dod_double_alias_local
(
    x UInt64
)
ENGINE = MergeTree()
ORDER BY x;

INSERT INTO test_dod_double_alias_local VALUES (1), (2), (10);

CREATE TABLE test_dod_double_alias_inner
(
    x UInt64,
    a UInt64 ALIAS 2,
    b UInt64 ALIAS 2,
    inner_c UInt64 ALIAS x + 1,
    inner_d UInt64 ALIAS x + 1
)
ENGINE = Distributed(test_cluster_two_shards, currentDatabase(), test_dod_double_alias_local);

CREATE TABLE test_dod_double_alias_outer
(
    x UInt64,
    inner_c UInt64,
    a UInt64 ALIAS 1,
    b UInt64 ALIAS 1,
    c UInt64 ALIAS inner_c,
    d UInt64 ALIAS inner_c,
    inner_d UInt64
)
ENGINE = Distributed(test_cluster_two_shards, currentDatabase(), test_dod_double_alias_inner);

SELECT 'prefer_localhost_replica_0';
SELECT x, a, b, c, d, inner_c, inner_d
FROM test_dod_double_alias_outer
ORDER BY x
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 0
FORMAT TSVWithNames;

SELECT 'prefer_localhost_replica_1';
SELECT x, a, b, c, d, inner_c, inner_d
FROM test_dod_double_alias_outer
ORDER BY x
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 1
FORMAT TSVWithNames;

SELECT 'prefer_localhost_replica_0_serialize_query_plan_1';
SELECT x, a, b, c, d, inner_c, inner_d
FROM test_dod_double_alias_outer
ORDER BY x
SETTINGS
    enable_analyzer = 1,
    enable_alias_marker = 1,
    prefer_localhost_replica = 0,
    serialize_query_plan = 1
FORMAT TSVWithNames;

SELECT 'prefer_localhost_replica_1_serialize_query_plan_1';
SELECT x, a, b, c, d, inner_c, inner_d
FROM test_dod_double_alias_outer
ORDER BY x
SETTINGS
    enable_analyzer = 1,
    enable_alias_marker = 1,
    prefer_localhost_replica = 1,
    serialize_query_plan = 1
FORMAT TSVWithNames;

DROP TABLE test_dod_double_alias_outer;
DROP TABLE test_dod_double_alias_inner;
DROP TABLE test_dod_double_alias_local;
