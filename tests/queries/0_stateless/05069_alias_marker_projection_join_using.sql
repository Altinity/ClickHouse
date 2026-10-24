-- `JOIN USING` can resolve a projection alias to an expression-bearing `ColumnNode`.
-- The matching query-tree test checks the injected side marker: `USING (k)` renders only the key name.
SET enable_analyzer = 1;

DROP TABLE IF EXISTS t_left_05069;
DROP TABLE IF EXISTS t_dist_05069;
DROP TABLE IF EXISTS t_right_05069;

CREATE TABLE t_left_05069 (x UInt64) ENGINE = MergeTree ORDER BY x;
INSERT INTO t_left_05069 VALUES (1), (2), (3);
CREATE TABLE t_dist_05069 AS t_left_05069
ENGINE = Distributed(test_shard_localhost, currentDatabase(), t_left_05069);
CREATE TABLE t_right_05069 (k UInt64, v UInt64) ENGINE = MergeTree ORDER BY k;
INSERT INTO t_right_05069 VALUES (2, 20), (3, 30), (4, 40);

SELECT 'local';
SELECT l.x + 1 AS k, r.v FROM t_left_05069 AS l INNER JOIN t_right_05069 AS r USING (k) ORDER BY k
SETTINGS analyzer_compatibility_join_using_top_level_identifier = 1;

SELECT 'distributed_marker_on';
SELECT l.x + 1 AS k, r.v FROM t_dist_05069 AS l INNER JOIN t_right_05069 AS r USING (k) ORDER BY k
SETTINGS analyzer_compatibility_join_using_top_level_identifier = 1, enable_alias_marker = 1,
    prefer_global_in_and_join = 1, prefer_localhost_replica = 0;

SELECT 'distributed_marker_off';
SELECT l.x + 1 AS k, r.v FROM t_dist_05069 AS l INNER JOIN t_right_05069 AS r USING (k) ORDER BY k
SETTINGS analyzer_compatibility_join_using_top_level_identifier = 1, enable_alias_marker = 0,
    prefer_global_in_and_join = 1, prefer_localhost_replica = 0;

DROP TABLE t_right_05069;
DROP TABLE t_dist_05069;
DROP TABLE t_left_05069;
