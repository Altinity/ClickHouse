-- A hand-written marker's second argument is still a SQL column reference until the shard analyzes it.
-- It must be inlined when it is an `ALIAS` and collected when it is needed by a `GLOBAL JOIN` temporary table.
SET enable_analyzer = 1;

DROP TABLE IF EXISTS t_local_05068;
DROP TABLE IF EXISTS t_dist_05068;
DROP TABLE IF EXISTS t_dist_only_alias_05068;
DROP TABLE IF EXISTS t_left_05068;
DROP TABLE IF EXISTS t_right_05068;

CREATE TABLE t_local_05068 (x UInt64, computed UInt64 ALIAS x * 10) ENGINE = MergeTree ORDER BY x;
INSERT INTO t_local_05068 (x) VALUES (1), (2), (3), (4), (5);
CREATE TABLE t_dist_05068 AS t_local_05068
ENGINE = Distributed(test_shard_localhost, currentDatabase(), t_local_05068);

CREATE TABLE t_left_05068 (x UInt64) ENGINE = MergeTree ORDER BY x;
INSERT INTO t_left_05068 VALUES (1), (2), (3), (4), (5);
-- This alias exists only on the initiator's `Distributed` schema, not on the local storage.
CREATE TABLE t_dist_only_alias_05068 (x UInt64, computed UInt64 ALIAS x * 10)
ENGINE = Distributed(test_shard_localhost, currentDatabase(), t_left_05068);

SELECT 'alias_on_both_marker_on';
SELECT count() FROM t_dist_05068 WHERE __aliasMarker(x + 1, computed) > 2
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0;
SELECT __aliasMarker(x + 1, computed) FROM t_dist_05068 ORDER BY x
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0;
SELECT 'alias_on_both_marker_off';
SELECT count() FROM t_dist_05068 WHERE __aliasMarker(x + 1, computed) > 2
SETTINGS enable_alias_marker = 0, prefer_localhost_replica = 0;
SELECT __aliasMarker(x + 1, computed) FROM t_dist_05068 ORDER BY x
SETTINGS enable_alias_marker = 0, prefer_localhost_replica = 0;

SELECT 'alias_only_on_distributed_marker_on';
SELECT count() FROM t_dist_only_alias_05068 WHERE __aliasMarker(x + 1, computed) > 2
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0;
SELECT __aliasMarker(x + 1, computed) FROM t_dist_only_alias_05068 ORDER BY x
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0;
SELECT 'alias_only_on_distributed_marker_off';
SELECT count() FROM t_dist_only_alias_05068 WHERE __aliasMarker(x + 1, computed) > 2
SETTINGS enable_alias_marker = 0, prefer_localhost_replica = 0;
SELECT __aliasMarker(x + 1, computed) FROM t_dist_only_alias_05068 ORDER BY x
SETTINGS enable_alias_marker = 0, prefer_localhost_replica = 0;

SELECT 'pending_token_survives_scalar_rewrite';
SELECT computed FROM t_dist_only_alias_05068 ORDER BY x
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0, optimize_const_name_size = 0;

CREATE TABLE t_right_05068 (x UInt64, extra UInt64) ENGINE = MergeTree ORDER BY x;
INSERT INTO t_right_05068 VALUES (1, 10), (2, 20), (3, 30), (4, 40), (5, 50);

-- `extra` is mentioned only in the marker's ID. It must still appear in the materialized right side.
SELECT 'global_join_id_only';
SELECT l.x, __aliasMarker(1, r.extra) AS marker_value
FROM t_dist_only_alias_05068 AS l
GLOBAL INNER JOIN t_right_05068 AS r ON l.x = r.x
ORDER BY l.x
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0;

DROP TABLE t_right_05068;
DROP TABLE t_dist_only_alias_05068;
DROP TABLE t_left_05068;
DROP TABLE t_dist_05068;
DROP TABLE t_local_05068;
