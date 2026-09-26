-- The three-argument form is an internal pending marker. A local query evaluates its payload.
-- The distributed cases fail in the finalizer, since `1` is not a column id; `gtest_alias_marker_pending` covers the shipping check.
SET enable_analyzer = 1;

DROP TABLE IF EXISTS t_local_05070;
DROP TABLE IF EXISTS t_dist_05070;
CREATE TABLE t_local_05070 (x UInt64) ENGINE = MergeTree ORDER BY x;
INSERT INTO t_local_05070 VALUES (1), (2);
CREATE TABLE t_dist_05070 AS t_local_05070
ENGINE = Distributed(test_shard_localhost, currentDatabase(), t_local_05070);

SELECT 'local_pending';
SELECT __aliasMarker(1, 1, '__aliasMarker_pending_v1') FROM t_local_05070 ORDER BY x;

SELECT 'bad_token';
SELECT __aliasMarker(1, 1, 'wrong_token') FROM t_local_05070; -- { serverError BAD_ARGUMENTS }
SELECT __aliasMarker(NULL, 1, 'wrong_token') FROM t_local_05070; -- { serverError BAD_ARGUMENTS }

SELECT 'bad_arity';
SELECT __aliasMarker(1, 1, 'wrong_token', 1) FROM t_local_05070; -- { serverError BAD_ARGUMENTS }

SELECT 'distributed_sql';
SELECT count() FROM t_dist_05070 WHERE __aliasMarker(x + 1, 1, '__aliasMarker_pending_v1') > 2
SETTINGS prefer_localhost_replica = 0, serialize_query_plan = 0; -- { serverError BAD_ARGUMENTS }

SELECT 'distributed_plan';
SELECT count() FROM t_dist_05070 WHERE __aliasMarker(x + 1, 1, '__aliasMarker_pending_v1') > 2
SETTINGS prefer_localhost_replica = 0, serialize_query_plan = 1; -- { serverError BAD_ARGUMENTS }

SELECT 'global_in_subquery';
SELECT count() FROM t_dist_05070 WHERE x GLOBAL IN
    (SELECT __aliasMarker(number + 1, 1, '__aliasMarker_pending_v1') FROM numbers(1))
SETTINGS prefer_localhost_replica = 0; -- { serverError BAD_ARGUMENTS }

DROP TABLE t_dist_05070;
DROP TABLE t_local_05070;
