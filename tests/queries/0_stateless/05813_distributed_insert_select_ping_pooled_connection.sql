-- Tags: no-fasttest, no-parallel
-- no-fasttest: the Fast test build has no libfiu, so SYSTEM ENABLE FAILPOINT is unsupported.
-- no-parallel: the fail points below fail every Ping in the server process.

-- `INSERT INTO <Distributed> SELECT ... FROM <cluster storage>` takes pooled connections in
-- `StorageDistributed::distributedWriteFromClusterStorage`. Those connections are pinged before use,
-- the same as the cluster read in 05812. The fail points make the Ping fail; the insert must still
-- finish and write rows. `DistributedConnectionFailTry` shows that the Ping ran.

INSERT INTO FUNCTION file(currentDatabase() || '_05813_data.tsv', 'TSV', 'x UInt8')
SELECT number FROM numbers(3)
SETTINGS engine_file_truncate_on_insert = 1;

CREATE TABLE dst_05813_local (x UInt8) ENGINE = Memory;
CREATE TABLE dst_05813 (x UInt8) ENGINE = Distributed('test_cluster_two_shards', currentDatabase(), dst_05813_local);

-- Fill the connection pool, so the next insert takes pooled connections.
INSERT INTO dst_05813
SELECT * FROM fileCluster('test_cluster_two_shards', currentDatabase() || '_05813_data.tsv', 'TSV', 'x UInt8')
SETTINGS parallel_distributed_insert_select = 2;
TRUNCATE TABLE dst_05813_local;

CREATE TEMPORARY TABLE ping_fail_try_before (value UInt64);
INSERT INTO ping_fail_try_before
SELECT ifNull((SELECT value FROM system.events WHERE event = 'DistributedConnectionFailTry'), 0);

SYSTEM ENABLE FAILPOINT unresponsive_pooled_connection;

INSERT INTO dst_05813
SELECT * FROM fileCluster('test_cluster_two_shards', currentDatabase() || '_05813_data.tsv', 'TSV', 'x UInt8')
SETTINGS parallel_distributed_insert_select = 2;

SYSTEM DISABLE FAILPOINT unresponsive_pooled_connection;

SELECT throwIf(
    ifNull((SELECT value FROM system.events WHERE event = 'DistributedConnectionFailTry'), 0)
        <= (SELECT value FROM ping_fail_try_before),
    'Distributed insert did not ping the pooled connection');

SELECT throwIf(count() = 0, 'Distributed insert wrote no rows') FROM dst_05813_local;

-- A peer that keeps the socket open and sends no Pong reconnects once, as in the cluster read.
-- `DistributedConnectionReconnectCount` moves when that reconnect is decided.

TRUNCATE TABLE dst_05813_local;

CREATE TEMPORARY TABLE ping_reconnect_before (value UInt64);
INSERT INTO ping_reconnect_before
SELECT ifNull((SELECT value FROM system.events WHERE event = 'DistributedConnectionReconnectCount'), 0);

SYSTEM ENABLE FAILPOINT pooled_connection_ping_timeout;

INSERT INTO dst_05813
SELECT * FROM fileCluster('test_cluster_two_shards', currentDatabase() || '_05813_data.tsv', 'TSV', 'x UInt8')
SETTINGS parallel_distributed_insert_select = 2;

SYSTEM DISABLE FAILPOINT pooled_connection_ping_timeout;

SELECT throwIf(
    ifNull((SELECT value FROM system.events WHERE event = 'DistributedConnectionReconnectCount'), 0)
        <= (SELECT value FROM ping_reconnect_before),
    'Distributed insert ping timeout did not reconnect');

SELECT throwIf(count() = 0, 'Distributed insert wrote no rows after ping timeout') FROM dst_05813_local;

DROP TABLE dst_05813;
DROP TABLE dst_05813_local;
