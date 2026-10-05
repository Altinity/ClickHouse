-- The SQL attached to each remote query keeps the finalized two-argument marker, even when
-- the server also sends a serialized plan. The plan packet itself contains actions, not this SQL call.
SET enable_analyzer = 1;

DROP TABLE IF EXISTS t_local_05071;
DROP TABLE IF EXISTS t_dist_05071;

CREATE TABLE t_local_05071 (x UInt64) ENGINE = MergeTree ORDER BY x;
INSERT INTO t_local_05071 VALUES (1), (2), (3);
CREATE TABLE t_dist_05071 (x UInt64, computed UInt64 ALIAS x * 10)
ENGINE = Distributed(test_shard_localhost, currentDatabase(), t_local_05071);

SELECT computed FROM t_dist_05071 ORDER BY x
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0, serialize_query_plan = 0,
    log_queries = 1, log_comment = '05071_alias_marker_sql' FORMAT Null;

SELECT computed FROM t_dist_05071 ORDER BY x
SETTINGS enable_alias_marker = 1, prefer_localhost_replica = 0, serialize_query_plan = 1,
    distributed_group_by_no_merge = 0, log_queries = 1, log_comment = '05071_alias_marker_plan' FORMAT Null;

SYSTEM FLUSH LOGS query_log;

SELECT 'sql_transport';
SELECT count() > 0 AS shipped, countIf(
    position(query, '__aliasMarker(') > 0
    AND match(query, '__aliasMarker[(].+, ''__table[0-9]+[.]computed''[)]')
    AND position(query, '__aliasMarker_pending_v1') = 0) = count() AS finalized
FROM system.query_log
WHERE type = 'QueryFinish' AND is_initial_query = 0
    AND initial_query_id IN (
        SELECT query_id FROM system.query_log
        WHERE type = 'QueryFinish' AND is_initial_query = 1
            AND current_database = currentDatabase() AND log_comment = '05071_alias_marker_sql');

SELECT 'plan_attached_sql';
SELECT count() > 0 AS shipped, countIf(
    position(query, '__aliasMarker(') > 0
    AND match(query, '__aliasMarker[(].+, ''__table[0-9]+[.]computed''[)]')
    AND position(query, '__aliasMarker_pending_v1') = 0) = count() AS finalized
FROM system.query_log
WHERE type = 'QueryFinish' AND is_initial_query = 0
    AND initial_query_id IN (
        SELECT query_id FROM system.query_log
        WHERE type = 'QueryFinish' AND is_initial_query = 1
            AND current_database = currentDatabase() AND log_comment = '05071_alias_marker_plan');

DROP TABLE t_dist_05071;
DROP TABLE t_local_05071;
