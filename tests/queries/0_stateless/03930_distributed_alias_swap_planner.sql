-- Plain Distributed (no Hybrid). Two nested ALIAS columns: a2 contains a1's subexpression,
-- so planner CSE may reorder the remote header. Correct result must equal the single-node
-- ('local') result across every transport variant.
DROP TABLE IF EXISTS t_local_03930;
DROP TABLE IF EXISTS t_dist_03930;

CREATE TABLE t_local_03930 (x UInt32, a1 UInt32 ALIAS x + 1, a2 UInt32 ALIAS a1 + 1)
ENGINE = MergeTree ORDER BY x;
INSERT INTO t_local_03930 VALUES (10), (20);

CREATE TABLE t_dist_03930 AS t_local_03930
ENGINE = Distributed(test_shard_localhost, currentDatabase(), t_local_03930);

SELECT 'local';
SELECT a1, a2 FROM t_local_03930 ORDER BY a1;

SELECT 'dist_prefer0';
SELECT a1, a2 FROM t_dist_03930 ORDER BY a1
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 0;

SELECT 'dist_prefer1';
SELECT a1, a2 FROM t_dist_03930 ORDER BY a1
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 1;

SELECT 'dist_prefer0_plan';
SELECT a1, a2 FROM t_dist_03930 ORDER BY a1
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 0, serialize_query_plan = 1;

SELECT 'dist_prefer1_plan';
SELECT a1, a2 FROM t_dist_03930 ORDER BY a1
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 1, serialize_query_plan = 1;

DROP TABLE t_dist_03930;
DROP TABLE t_local_03930;
