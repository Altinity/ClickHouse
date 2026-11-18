-- Tags: no-fasttest, no-parallel
-- no-fasttest: the Fast test build has no libfiu, so SYSTEM ENABLE FAILPOINT is unsupported.
-- no-parallel: the fail points below fail every Ping in the server process.

-- `test_cluster_two_shards` has one local address (127.0.0.1) and one that is not local
-- (127.0.0.2). A cluster read pings a pooled connection only for the remote shard. The fail
-- point makes that Ping fail. The read must still return every row: the remote shard is
-- reconnected, and the local shard is not pinged. `DistributedConnectionFailTry` is what shows
-- the Ping ran. The zero-timeout poll used for every other pooled connection does not call it.

SELECT host_name, is_local
FROM system.clusters
WHERE cluster = 'test_cluster_two_shards'
ORDER BY host_name;

INSERT INTO FUNCTION file(currentDatabase() || '_05812_data.tsv', 'TSV', 'x UInt8')
SELECT number FROM numbers(3)
SETTINGS engine_file_truncate_on_insert = 1;

-- Fill the connection pool. The next read is the one that takes a pooled connection.
SELECT count()
FROM fileCluster('test_cluster_two_shards', currentDatabase() || '_05812_data.tsv', 'TSV', 'x UInt8')
FORMAT Null;

CREATE TEMPORARY TABLE ping_fail_try_before (value UInt64);
INSERT INTO ping_fail_try_before
SELECT ifNull((SELECT value FROM system.events WHERE event = 'DistributedConnectionFailTry'), 0);

SYSTEM ENABLE FAILPOINT unresponsive_pooled_connection;

SELECT count()
FROM fileCluster('test_cluster_two_shards', currentDatabase() || '_05812_data.tsv', 'TSV', 'x UInt8');

SYSTEM DISABLE FAILPOINT unresponsive_pooled_connection;

SELECT throwIf(
    ifNull((SELECT value FROM system.events WHERE event = 'DistributedConnectionFailTry'), 0)
        <= (SELECT value FROM ping_fail_try_before),
    'Remote shard was not pinged');

-- A peer that keeps the socket open and sends no Pong is skipped after the ping. Opening another
-- connection would wait for handshake_timeout_ms and get no Hello either. `ReconnectCount` stays
-- put; `FailTry` still moves. The local shard is not pinged, so it still reads the file.

CREATE TEMPORARY TABLE ping_reconnect_before (value UInt64);
INSERT INTO ping_reconnect_before
SELECT ifNull((SELECT value FROM system.events WHERE event = 'DistributedConnectionReconnectCount'), 0);

CREATE TEMPORARY TABLE ping_fail_try_before_timeout (value UInt64);
INSERT INTO ping_fail_try_before_timeout
SELECT ifNull((SELECT value FROM system.events WHERE event = 'DistributedConnectionFailTry'), 0);

SYSTEM ENABLE FAILPOINT pooled_connection_ping_timeout;

SELECT count()
FROM fileCluster('test_cluster_two_shards', currentDatabase() || '_05812_data.tsv', 'TSV', 'x UInt8');

SYSTEM DISABLE FAILPOINT pooled_connection_ping_timeout;

SELECT throwIf(
    ifNull((SELECT value FROM system.events WHERE event = 'DistributedConnectionReconnectCount'), 0)
        != (SELECT value FROM ping_reconnect_before),
    'Ping timeout reconnected');

SELECT throwIf(
    ifNull((SELECT value FROM system.events WHERE event = 'DistributedConnectionFailTry'), 0)
        <= (SELECT value FROM ping_fail_try_before_timeout),
    'Ping timeout was not applied');
