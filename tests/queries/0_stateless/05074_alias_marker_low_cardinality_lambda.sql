-- The planner removes `__aliasMarker` without converting its payload, so type inference must preserve `LowCardinality`.
SET enable_analyzer = 1;
SET enable_alias_marker = 1;
SET allow_suspicious_low_cardinality_types = 1;

DROP TABLE IF EXISTS alias_marker_lc_dist;
DROP TABLE IF EXISTS alias_marker_lc_local;

CREATE TABLE alias_marker_lc_local (x LowCardinality(Nullable(Int64))) ENGINE = MergeTree ORDER BY tuple();
INSERT INTO alias_marker_lc_local VALUES (1), (2), (NULL);

CREATE TABLE alias_marker_lc_dist AS alias_marker_lc_local
ENGINE = Distributed(test_shard_localhost, currentDatabase(), alias_marker_lc_local);

SELECT 'payload_type';
SELECT toTypeName(x), toTypeName(__aliasMarker(x, x)) FROM alias_marker_lc_local ORDER BY x;

SELECT 'lambda_local';
SELECT arrayMap(lx -> __aliasMarker(lx, lx), [x]) FROM alias_marker_lc_local ORDER BY x;

-- The fuzzer's asterisk expands to the table column captured by the lambda, not its parameter.
SELECT 'lambda_capture_local';
SELECT arrayMap(lx -> __aliasMarker(*, lx), [x]) FROM alias_marker_lc_local ORDER BY x;

SELECT 'lambda_distributed_ast';
SELECT arrayMap(lx -> __aliasMarker(lx, lx), [x]) FROM alias_marker_lc_dist ORDER BY x
SETTINGS prefer_localhost_replica = 0, serialize_query_plan = 0;

SELECT 'lambda_capture_distributed_ast';
SELECT arrayMap(lx -> __aliasMarker(*, lx), [x]) FROM alias_marker_lc_dist ORDER BY x
SETTINGS prefer_localhost_replica = 0, serialize_query_plan = 0;

SELECT 'lambda_distributed_plan';
SELECT arrayMap(lx -> __aliasMarker(lx, lx), [x]) FROM alias_marker_lc_dist ORDER BY x
SETTINGS prefer_localhost_replica = 0, serialize_query_plan = 1;

SELECT 'lambda_capture_distributed_plan';
SELECT arrayMap(lx -> __aliasMarker(*, lx), [x]) FROM alias_marker_lc_dist ORDER BY x
SETTINGS prefer_localhost_replica = 0, serialize_query_plan = 1;

DROP TABLE alias_marker_lc_dist;
DROP TABLE alias_marker_lc_local;
