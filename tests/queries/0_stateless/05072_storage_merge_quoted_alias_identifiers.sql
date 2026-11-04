-- Qualified `ALIAS` names must use the planner's quoting convention in `Merge` header
-- reconciliation. A missing match used to fill both quoted outputs with zeros.
SET enable_analyzer = 1, enable_alias_marker = 1;

DROP TABLE IF EXISTS t_merge_dist_05072;
DROP TABLE IF EXISTS t_merge_local_05072;
DROP TABLE IF EXISTS t_dist_05072;
DROP TABLE IF EXISTS t_local_05072;

CREATE TABLE t_local_05072
(
    id UInt32,
    `a-b` UInt32 ALIAS id * 10,
    `a b` UInt32 ALIAS id * 100,
    chained UInt32 ALIAS `a-b` + 1,
    flag UInt8 ALIAS id = 2
) ENGINE = MergeTree ORDER BY id;
INSERT INTO t_local_05072 VALUES (1), (2);

CREATE TABLE t_dist_05072 AS t_local_05072
ENGINE = Distributed(test_cluster_two_shards, currentDatabase(), t_local_05072);
CREATE TABLE t_merge_local_05072
(id UInt32, `a-b` UInt32, `a b` UInt32, chained UInt32, flag UInt8)
ENGINE = Merge(currentDatabase(), '^t_local_05072$');
CREATE TABLE t_merge_dist_05072 AS t_merge_local_05072
ENGINE = Merge(currentDatabase(), '^t_dist_05072$');

SELECT 'local source';
SELECT id, `a-b`, `a b`, chained FROM t_local_05072 ORDER BY id;

SELECT 'local Merge';
SELECT id, `a-b`, `a b`, chained FROM t_merge_local_05072 ORDER BY id;

SELECT 'Distributed AST';
SELECT id, `a-b`, `a b` FROM t_merge_dist_05072
GROUP BY id, `a-b`, `a b` ORDER BY id
SETTINGS prefer_localhost_replica = 0, serialize_query_plan = 0;

SELECT 'Distributed plan';
SELECT id, `a-b`, `a b` FROM t_merge_dist_05072
GROUP BY id, `a-b`, `a b` ORDER BY id
SETTINGS prefer_localhost_replica = 0, serialize_query_plan = 1;

-- The `a-b` alias is used to calculate `chained`, but is not an output column.
SELECT 'intermediate alias';
SELECT id, chained FROM t_merge_dist_05072 GROUP BY id, chained ORDER BY id
SETTINGS prefer_localhost_replica = 0;

-- The row policy on the `Distributed` child runs after the qualified alias reconstruction.
CREATE ROW POLICY rp_05072 ON t_dist_05072 USING id >= 2 AND `a-b` >= 20 TO ALL;
SELECT 'row policy';
SELECT id, `a-b` FROM t_merge_dist_05072 GROUP BY id, `a-b` ORDER BY id
SETTINGS prefer_localhost_replica = 0;
SELECT 'row policy all aliases';
SELECT id, `a-b`, `a b` FROM t_merge_dist_05072
GROUP BY id, `a-b`, `a b` ORDER BY id
SETTINGS prefer_localhost_replica = 0, serialize_query_plan = 1;
SELECT 'policy-only alias';
SELECT id FROM t_merge_dist_05072 GROUP BY id ORDER BY id
SETTINGS prefer_localhost_replica = 0;
DROP ROW POLICY rp_05072 ON t_dist_05072;

-- A policy over a UInt8 `ALIAS` preserves the selected column and also works when
-- the alias is needed only for the filter.
CREATE ROW POLICY rp_05072 ON t_dist_05072 USING flag = 1 TO ALL;
SELECT 'flag alias policy';
SELECT id, flag FROM t_merge_dist_05072 GROUP BY id, flag ORDER BY id
SETTINGS prefer_localhost_replica = 0;
SELECT 'flag alias policy-only column';
SELECT id FROM t_merge_dist_05072 GROUP BY id ORDER BY id
SETTINGS prefer_localhost_replica = 0;
DROP ROW POLICY rp_05072 ON t_dist_05072;

-- A child without these columns still uses the ordinary missing-column defaults.
CREATE TABLE t_missing_05072 (id UInt32) ENGINE = MergeTree ORDER BY id;
INSERT INTO t_missing_05072 VALUES (3);
CREATE TABLE t_merge_mixed_05072 AS t_merge_local_05072
ENGINE = Merge(currentDatabase(), '^t_(local|missing)_05072$');
SELECT 'missing child aliases';
SELECT id, `a-b`, `a b`, chained FROM t_merge_mixed_05072 ORDER BY id;
DROP TABLE t_merge_mixed_05072;
DROP TABLE t_missing_05072;

DROP TABLE t_merge_dist_05072;
DROP TABLE t_merge_local_05072;
DROP TABLE t_dist_05072;
DROP TABLE t_local_05072;
