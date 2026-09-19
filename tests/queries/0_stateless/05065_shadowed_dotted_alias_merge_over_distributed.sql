-- Values guard for a `Merge` over `Distributed` whose schema declares both a plain column and a
-- dotted column whose tail is that plain name (`b` and `` `a.b` ``), both read as `ALIAS` columns.
-- 05059 covers dotted names on their own; this shape is covered nowhere else.
--
-- `test_cluster_two_shards` reads the local table twice, so every query groups to dedup.

DROP TABLE IF EXISTS t_local_05065;
DROP TABLE IF EXISTS t_dist_05065;
DROP TABLE IF EXISTS t_merge_05065;

CREATE TABLE t_local_05065
(
    id UInt32,
    b UInt32 ALIAS id * 10,
    `a.b` UInt32 ALIAS id * 100
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
    `a.b` UInt32
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

DROP TABLE t_merge_05065;
DROP TABLE t_dist_05065;
DROP TABLE t_local_05065;
