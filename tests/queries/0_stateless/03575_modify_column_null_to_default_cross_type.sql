-- Tags: no-random-settings, no-random-merge-tree-settings

DROP TABLE IF EXISTS t_cross_scalar;
CREATE TABLE t_cross_scalar (x Nullable(UInt8), y String) ENGINE = MergeTree ORDER BY tuple();
INSERT INTO t_cross_scalar VALUES (NULL, 'a'), (42, 'b');
ALTER TABLE t_cross_scalar MODIFY COLUMN x String DEFAULT '';
OPTIMIZE TABLE t_cross_scalar FINAL;
SELECT x, y FROM t_cross_scalar ORDER BY y;
DROP TABLE t_cross_scalar;

DROP TABLE IF EXISTS t_cross_lc;
CREATE TABLE t_cross_lc (x Nullable(UInt8), y String) ENGINE = MergeTree ORDER BY tuple();
INSERT INTO t_cross_lc VALUES (NULL, 'a'), (7, 'b');
ALTER TABLE t_cross_lc MODIFY COLUMN x LowCardinality(String) DEFAULT 'def';
OPTIMIZE TABLE t_cross_lc FINAL;
SELECT toTypeName(x), x, y FROM t_cross_lc ORDER BY y;
DROP TABLE t_cross_lc;

DROP TABLE IF EXISTS t_cross_array;
CREATE TABLE t_cross_array (x Nullable(String), y String) ENGINE = MergeTree ORDER BY tuple();
INSERT INTO t_cross_array VALUES (NULL, 'a'), ('[1,2,3]', 'b');
ALTER TABLE t_cross_array MODIFY COLUMN x Array(UInt8) DEFAULT [];
OPTIMIZE TABLE t_cross_array FINAL;
SELECT x, y FROM t_cross_array ORDER BY y;
DROP TABLE t_cross_array;

DROP TABLE IF EXISTS t_cross_tuple;
CREATE TABLE t_cross_tuple (x Nullable(String), y String) ENGINE = MergeTree ORDER BY tuple();
INSERT INTO t_cross_tuple VALUES (NULL, 'a'), ('(1,2)', 'b');
ALTER TABLE t_cross_tuple MODIFY COLUMN x Tuple(UInt8, UInt8) DEFAULT (0, 0);
OPTIMIZE TABLE t_cross_tuple FINAL;
SELECT x, y FROM t_cross_tuple ORDER BY y;
DROP TABLE t_cross_tuple;
