-- An `ALIAS` column referenced from inside a lambda body is inlined and marked like any other, but
-- `finalizeAliasMarkersForDistributedSerialization` used to refuse to descend into a lambda. The marker's id then
-- stayed a live `ColumnNode` and was rendered back into the shipped SQL as `__table1.computed`, so the shard had to
-- resolve the very `ALIAS` column the inlining exists to remove. Here the column is declared on the `Distributed`
-- table and not on the local one, so the shard cannot resolve it and the query fails.
--
-- The finalize pass still leaves a hand-written marker inside a lambda alone, because its id resolves to the lambda
-- parameter rather than to a column of a table -- that is what 03933 covers.

DROP TABLE IF EXISTS t_local_05064;
DROP TABLE IF EXISTS t_dist_05064;

CREATE TABLE t_local_05064 (value UInt64, arr Array(UInt64)) ENGINE = MergeTree ORDER BY value;
INSERT INTO t_local_05064 VALUES (1, [10, 20]), (3, [5]);

CREATE TABLE t_dist_05064
(
    value UInt64,
    arr Array(UInt64),
    computed UInt64 ALIAS value * 2
)
ENGINE = Distributed(test_shard_localhost, currentDatabase(), t_local_05064);

SELECT 'control';
SELECT computed
FROM t_dist_05064
ORDER BY value
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0;

SELECT 'lambda';
SELECT arrayMap(x -> x + computed, arr)
FROM t_dist_05064
ORDER BY value
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0;

SELECT 'lambda_no_marker';
SELECT arrayMap(x -> x + computed, arr)
FROM t_dist_05064
ORDER BY value
SETTINGS enable_alias_marker = 0, prefer_localhost_replica = 0;

DROP TABLE t_dist_05064;
DROP TABLE t_local_05064;
