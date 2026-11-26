-- Tags: no-fasttest
-- no-fasttest: uses the S3 mock server on localhost:11111

-- `allow_object_storage_cluster_distributed_join` is decided on the initiator and is not sent to the nodes, which
-- may not know it. A query whose only setting is this one, as `name = value` or `name = DEFAULT`, must not reach the
-- nodes with a bare `SETTINGS`, which does not parse.

INSERT INTO FUNCTION s3('http://localhost:11111/test/05485_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String')
SELECT number, toString(number) FROM numbers(3)
SETTINGS s3_truncate_on_insert = 1;

SELECT 'value';
SELECT k, v
FROM s3Cluster('test_cluster_two_shards', 'http://localhost:11111/test/05485_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String')
ORDER BY k
SETTINGS allow_object_storage_cluster_distributed_join = 0;

SELECT 'default';
SELECT k, v
FROM s3Cluster('test_cluster_two_shards', 'http://localhost:11111/test/05485_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String')
ORDER BY k
SETTINGS allow_object_storage_cluster_distributed_join = DEFAULT;

SELECT 'enabled, not dispatched';
SELECT k, v
FROM s3Cluster('test_cluster_two_shards', 'http://localhost:11111/test/05485_' || currentDatabase() || '.tsv', 'test', 'testtest', 'TSV', 'k UInt64, v String')
ORDER BY k
SETTINGS allow_object_storage_cluster_distributed_join = 1;

-- No node received the setting.
SYSTEM FLUSH LOGS query_log;
SELECT count()
FROM system.query_log
WHERE type = 'QueryFinish' AND NOT is_initial_query AND current_database = currentDatabase()
    AND positionCaseInsensitive(query, 'allow_object_storage_cluster_distributed_join') > 0;
