-- End-to-end guard for a `Merge` over `Distributed` whose schema declares both a plain column and a
-- dotted column whose tail is that plain name (`b` and `` `a.b` ``), read as `ALIAS` columns.
--
-- What this does NOT do: reproduce the first-match bug in the name mapping of
-- `ReadFromMerge::convertAndFilterSourceStream`. That mapping matches declared Merge column names
-- against the suffixes of the child stream's column names, and `__table1.a.b` ends with `.b`, so a
-- first match in schema order would hand `b` the wrong column. No input can put it in that state:
-- the Merge target header carries analyzer identifiers, which backquote a dotted name
-- (`__table1.`a.b``, `PlannerContext.cpp` `buildColumnIdentifier`), and the child stream carries the
-- alias values under the alias name or the expression name, never a dotted analyzer identifier,
-- because the query reaching the child has every Merge-level `ALIAS` reference already replaced by
-- its expression. Verified by reintroducing the first-match behaviour and observing this test still
-- pass.
--
-- So it asserts the values are right for a schema shape nothing else covers. 05059 covers dotted
-- names on their own.
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
