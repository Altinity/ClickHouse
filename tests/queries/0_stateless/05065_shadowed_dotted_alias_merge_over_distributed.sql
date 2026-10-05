-- A `Merge` over `Distributed` must preserve multiple dotted `ALIAS` names, including
-- a dotted name whose tail collides with a plain `ALIAS` name (`b` and `` `a.b` ``).
--
-- `test_cluster_two_shards` reads the local table twice, so every query groups to dedup.

DROP TABLE IF EXISTS t_local_05065;
DROP TABLE IF EXISTS t_dist_05065;
DROP TABLE IF EXISTS t_merge_05065;

CREATE TABLE t_local_05065
(
    id UInt32,
    b UInt32 ALIAS id * 10,
    `a.b` UInt32 ALIAS id * 100,
    `n.a` UInt32 ALIAS id * 1000,
    `m.b` UInt32 ALIAS id * 10000
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO t_local_05065 VALUES (1), (2);

CREATE TABLE t_dist_05065 AS t_local_05065
ENGINE = Distributed(test_cluster_two_shards, currentDatabase(), t_local_05065);

CREATE TABLE t_merge_05065
(
    id UInt32,
    b UInt32,
    `a.b` UInt32,
    `n.a` UInt32,
    `m.b` UInt32
)
ENGINE = Merge(currentDatabase(), '^t_dist_05065$');

SELECT 'local';
SELECT id, b, `a.b`
FROM t_local_05065
GROUP BY id, b, `a.b`
ORDER BY id;

SELECT 'merge_prefer0';
SELECT id, b, `a.b`
FROM t_merge_05065
GROUP BY id, b, `a.b`
ORDER BY id
SETTINGS prefer_localhost_replica = 0;

SELECT 'merge_prefer1';
SELECT id, b, `a.b`
FROM t_merge_05065
GROUP BY id, b, `a.b`
ORDER BY id
SETTINGS prefer_localhost_replica = 1;

SELECT 'multiple dotted local';
SELECT id, `n.a`, `m.b` FROM t_local_05065
GROUP BY id, `n.a`, `m.b` ORDER BY id;

SELECT 'multiple dotted merge_prefer0';
SELECT id, `n.a`, `m.b` FROM t_merge_05065
GROUP BY id, `n.a`, `m.b` ORDER BY id
SETTINGS prefer_localhost_replica = 0;

SELECT 'multiple dotted merge_prefer1';
SELECT id, `n.a`, `m.b` FROM t_merge_05065
GROUP BY id, `n.a`, `m.b` ORDER BY id
SETTINGS prefer_localhost_replica = 1;

DROP TABLE t_merge_05065;
DROP TABLE t_dist_05065;
DROP TABLE t_local_05065;
