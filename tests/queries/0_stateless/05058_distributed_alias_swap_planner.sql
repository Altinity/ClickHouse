-- Nested `ALIAS` columns: `a2` contains `a1`'s subexpression, so planner CSE may reorder
-- the remote header. Check both alias-only and reordered/computed projections.
DROP TABLE IF EXISTS t_local_05058;
DROP TABLE IF EXISTS t_dist_05058;

CREATE TABLE t_local_05058 (x UInt32, a1 UInt32 ALIAS x + 1, a2 UInt32 ALIAS a1 + 1)
ENGINE = MergeTree ORDER BY x;
INSERT INTO t_local_05058 VALUES (10), (20);

CREATE TABLE t_dist_05058 AS t_local_05058
ENGINE = Distributed(test_shard_localhost, currentDatabase(), t_local_05058);

SELECT 'local';
SELECT a1, a2 FROM t_local_05058 ORDER BY a1;

SELECT 'dist_prefer0';
SELECT a1, a2 FROM t_dist_05058 ORDER BY a1
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 0;

SELECT 'dist_prefer1';
SELECT a1, a2 FROM t_dist_05058 ORDER BY a1
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 1;

SELECT 'dist_prefer0_plan';
SELECT a1, a2 FROM t_dist_05058 ORDER BY a1
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 0, serialize_query_plan = 1;

SELECT 'dist_prefer1_plan';
SELECT a1, a2 FROM t_dist_05058 ORDER BY a1
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 1, serialize_query_plan = 1;

SELECT 'reordered_local';
SELECT a2, a1, a1 + a2 AS s FROM t_local_05058 ORDER BY x;

SELECT 'reordered_dist';
SELECT a2, a1, a1 + a2 AS s FROM t_dist_05058 ORDER BY x
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 0, serialize_query_plan = 0;

SELECT 'reordered_dist_plan';
SELECT a2, a1, a1 + a2 AS s FROM t_dist_05058 ORDER BY x
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 0, serialize_query_plan = 1;

DROP TABLE t_dist_05058;
DROP TABLE t_local_05058;
