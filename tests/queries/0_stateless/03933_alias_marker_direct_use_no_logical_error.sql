-- __aliasMarker is an internal pass-through identity function. Direct use from SQL must not
-- raise a server-side LOGICAL_ERROR (which would abort under abort_on_logical_error / sanitizers),
-- in particular inside a lambda over a Distributed table where the marker's column argument
-- resolves to a lambda parameter with no table source.
DROP TABLE IF EXISTS t_local_03933;
DROP TABLE IF EXISTS t_dist_03933;

CREATE TABLE t_local_03933 (x UInt64) ENGINE = MergeTree ORDER BY x;
INSERT INTO t_local_03933 VALUES (1), (2), (3);

CREATE TABLE t_dist_03933 AS t_local_03933
ENGINE = Distributed(test_shard_localhost, currentDatabase(), t_local_03933);

SELECT '2arg_identity';
SELECT __aliasMarker(42, 'anything');

SELECT 'lambda_local';
SELECT arrayMap(lx -> __aliasMarker(lx, lx), [x]) AS arr FROM t_local_03933 ORDER BY x;

SELECT 'lambda_over_distributed';
SELECT arrayMap(lx -> __aliasMarker(lx, lx), [x]) AS arr
FROM t_dist_03933 ORDER BY x
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 0;

SELECT 'lambda_over_distributed_plan';
SELECT arrayMap(lx -> __aliasMarker(lx, lx), [x]) AS arr
FROM t_dist_03933 ORDER BY x
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 0, serialize_query_plan = 1;

DROP TABLE t_dist_03933;
DROP TABLE t_local_03933;
