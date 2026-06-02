-- Tags: no-fasttest, no-shared-merge-tree
-- ^ content_addressed is an object-storage metadata type; keep it off the minimal fasttest image.
--   no-shared-merge-tree: this checks the open-source ReplicatedMergeTree rejection.

-- B33: replication-internal clones (fetch-fallback, queue-driven REPLACE/MOVE) reach
-- cloneAndLoadDataPart -> freeze -> Backup -> per-file createHardLink autocommit, which bypasses
-- the ALTER-partition gate and would corrupt a content-addressed part (the B21 mode). So creating a
-- ReplicatedMergeTree table on a content_addressed disk must fail closed at CREATE with a clear
-- SUPPORT_IS_DISABLED error (until B1 replication support lands). A plain (non-replicated) MergeTree
-- on the same kind of disk must still work.

DROP TABLE IF EXISTS t_cas_repl;
DROP TABLE IF EXISTS t_cas_plain;

-- (1) A ReplicatedMergeTree table on a content_addressed disk is rejected at CREATE.
CREATE TABLE t_cas_repl (a UInt64, b UInt64)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/t_cas_repl', 'r1')
ORDER BY a
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    name = '04283_content_addressed_repl',
    path = '04283_content_addressed_repl_pool/'); -- { serverError SUPPORT_IS_DISABLED }

-- (2) A plain (non-replicated) MergeTree on a content_addressed disk still works end-to-end.
CREATE TABLE t_cas_plain (a UInt64, b UInt64)
ENGINE = MergeTree ORDER BY a
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    name = '04283_content_addressed_plain',
    path = '04283_content_addressed_plain_pool/');

INSERT INTO t_cas_plain SELECT number, number * 2 FROM numbers(100);
SELECT 'plain_count', count() FROM t_cas_plain;
SELECT 'plain_sum', sum(b) FROM t_cas_plain;

DROP TABLE t_cas_plain;
SELECT 'dropped_ok';
