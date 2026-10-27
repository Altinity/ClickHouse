-- Tags: zookeeper, no-fasttest, no-parallel, no-random-merge-tree-settings
-- no-parallel: enables a server-global fail point on the part disk-transaction commit.
-- The Packed twin of 05014: the fail point on the part's disk commit fires for a Packed part too, so a
-- failed commit never leaves a phantom dedup block id behind, and the byte-identical retry really inserts.

DROP TABLE IF EXISTS t_packed_commit_failpoint SYNC;

CREATE TABLE t_packed_commit_failpoint (k UInt64, v String)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/t_packed_commit_failpoint', 'r1')
ORDER BY k
SETTINGS min_bytes_for_full_part_storage = 1000000000;

SYSTEM ENABLE FAILPOINT part_storage_fail_commit_transaction;
INSERT INTO t_packed_commit_failpoint SETTINGS insert_deduplicate = 1, insert_keeper_fault_injection_probability = 0 VALUES (1, 'x'); -- { serverError FAULT_INJECTED }
SYSTEM DISABLE FAILPOINT part_storage_fail_commit_transaction;

INSERT INTO t_packed_commit_failpoint SETTINGS insert_deduplicate = 1, insert_keeper_fault_injection_probability = 0 VALUES (1, 'x');

SELECT count() FROM t_packed_commit_failpoint;
SELECT DISTINCT part_storage_type FROM system.parts WHERE database = currentDatabase() AND table = 't_packed_commit_failpoint' AND active;

DROP TABLE t_packed_commit_failpoint SYNC;
