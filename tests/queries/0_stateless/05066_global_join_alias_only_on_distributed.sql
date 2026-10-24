-- A `GLOBAL JOIN` ships its right side as a temporary table, whose columns are the ones
-- `CollectColumnSourceToColumnsVisitor` gathered from the query tree. The `__aliasMarker` id is a
-- `ColumnNode` naming the `ALIAS` column an inlined expression came from, so it used to be gathered
-- as if it were a column the query reads. The temporary table's subquery is rebuilt from names and
-- types alone, which drops the alias body, and the shard is then asked for a column its local table
-- does not declare.
--
-- `foo` exists only on the `Distributed` table here, so nothing can resolve it on the shard.

DROP TABLE IF EXISTS t_left_local_05066;
DROP TABLE IF EXISTS t_left_dist_05066;
DROP TABLE IF EXISTS t_right_local_05066;
DROP TABLE IF EXISTS t_right_dist_05066;

CREATE TABLE t_left_local_05066 (id UInt64) ENGINE = MergeTree ORDER BY id;
INSERT INTO t_left_local_05066 VALUES (1), (2), (3);

CREATE TABLE t_left_dist_05066 AS t_left_local_05066
ENGINE = Distributed(test_shard_localhost, currentDatabase(), t_left_local_05066);

CREATE TABLE t_right_local_05066 (id UInt64, x UInt64) ENGINE = MergeTree ORDER BY id;
INSERT INTO t_right_local_05066 VALUES (1, 10), (2, 20), (3, 30);

CREATE TABLE t_right_dist_05066
(
    id UInt64,
    x UInt64,
    foo UInt64 ALIAS x * 2
)
ENGINE = Distributed(test_shard_localhost, currentDatabase(), t_right_local_05066);

SELECT 'projected_marker_on';
SELECT m.id, r.foo
FROM t_left_dist_05066 AS m
GLOBAL INNER JOIN t_right_dist_05066 AS r ON m.id = r.id
ORDER BY m.id
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0;

-- The projection path also sets an alias on the marker. Referencing the column only in a clause
-- exercises the path where no alias is set at all.
SELECT 'clause_only_marker_on';
SELECT m.id
FROM t_left_dist_05066 AS m
GLOBAL INNER JOIN t_right_dist_05066 AS r ON m.id = r.id
WHERE r.foo > 30
ORDER BY m.id
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0;

SELECT 'projected_marker_off';
SELECT m.id, r.foo
FROM t_left_dist_05066 AS m
GLOBAL INNER JOIN t_right_dist_05066 AS r ON m.id = r.id
ORDER BY m.id
SETTINGS enable_alias_marker = 0, prefer_localhost_replica = 0;

SELECT 'clause_only_marker_off';
SELECT m.id
FROM t_left_dist_05066 AS m
GLOBAL INNER JOIN t_right_dist_05066 AS r ON m.id = r.id
WHERE r.foo > 30
ORDER BY m.id
SETTINGS enable_alias_marker = 0, prefer_localhost_replica = 0;

-- The same rewrite reaches a plain `JOIN` through `prefer_global_in_and_join`.
SELECT 'implicit_global_join';
SELECT m.id, r.foo
FROM t_left_dist_05066 AS m
INNER JOIN t_right_dist_05066 AS r ON m.id = r.id
ORDER BY m.id
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0, prefer_global_in_and_join = 1;

DROP TABLE t_right_dist_05066;
DROP TABLE t_right_local_05066;
DROP TABLE t_left_dist_05066;
DROP TABLE t_left_local_05066;
