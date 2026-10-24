-- A direct two-argument `__aliasMarker` must not acquire a transport ID from its column argument.
SET enable_analyzer = 1;

DROP TABLE IF EXISTS t_local_05067;
DROP TABLE IF EXISTS t_dist_05067;

CREATE TABLE t_local_05067 (x UInt64) ENGINE = MergeTree ORDER BY x;
INSERT INTO t_local_05067 VALUES (1), (2), (3), (4), (5);

CREATE TABLE t_dist_05067 AS t_local_05067
ENGINE = Distributed(test_shard_localhost, currentDatabase(), t_local_05067);

SELECT 'local';
SELECT count() FROM t_local_05067 WHERE __aliasMarker(x + 1, x) > 2;
SELECT __aliasMarker(x + 1, x) FROM t_local_05067 ORDER BY x;

SELECT 'distributed_marker_on';
SELECT count() FROM t_dist_05067 WHERE __aliasMarker(x + 1, x) > 2
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0;
SELECT __aliasMarker(x + 1, x) FROM t_dist_05067 ORDER BY x
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0;

SELECT 'distributed_marker_off';
SELECT count() FROM t_dist_05067 WHERE __aliasMarker(x + 1, x) > 2
SETTINGS enable_alias_marker = 0, prefer_localhost_replica = 0;
SELECT __aliasMarker(x + 1, x) FROM t_dist_05067 ORDER BY x
SETTINGS enable_alias_marker = 0, prefer_localhost_replica = 0;

SELECT 'distributed_serialized_plan';
SELECT count() FROM t_dist_05067 WHERE __aliasMarker(x + 1, x) > 2
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0, serialize_query_plan = 1;
SELECT __aliasMarker(x + 1, x) FROM t_dist_05067 ORDER BY x
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0, serialize_query_plan = 1;

DROP TABLE t_dist_05067;
DROP TABLE t_local_05067;
