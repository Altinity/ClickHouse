-- Plain Distributed (no Hybrid). Reorders alias columns and mixes a computed expression over
-- them. With strict name-based header reconciliation (positional fallback disabled), the result
-- must equal the single-node ('local') result for both AST and serialized-plan transport, and no
-- LOGICAL_ERROR must be raised.
DROP TABLE IF EXISTS t_local_03932;
DROP TABLE IF EXISTS t_dist_03932;

CREATE TABLE t_local_03932 (x UInt32, a1 UInt32 ALIAS x + 1, a2 UInt32 ALIAS a1 + 1)
ENGINE = MergeTree ORDER BY x;
INSERT INTO t_local_03932 VALUES (10), (20);

CREATE TABLE t_dist_03932 AS t_local_03932
ENGINE = Distributed(test_shard_localhost, currentDatabase(), t_local_03932);

SELECT 'local';
SELECT a2, a1, a1 + a2 AS s FROM t_local_03932 ORDER BY x;

SELECT 'dist';
SELECT a2, a1, a1 + a2 AS s FROM t_dist_03932 ORDER BY x
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 0;

SELECT 'dist_plan';
SELECT a2, a1, a1 + a2 AS s FROM t_dist_03932 ORDER BY x
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 0, serialize_query_plan = 1;

DROP TABLE t_dist_03932;
DROP TABLE t_local_03932;
