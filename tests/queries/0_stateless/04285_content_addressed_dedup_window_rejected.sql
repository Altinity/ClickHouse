-- Tags: no-fasttest
-- ^ content_addressed is an object-storage metadata type; keep it off the minimal fasttest image.

-- B37: a non_replicated_deduplication_window > 0 on a plain MergeTree makes it write an APPEND-mode
-- on-disk deduplication log (deduplication_logs/) at the table root. The immutable content_addressed
-- disk cannot host append writes, so the log's writer would never be created and the first INSERT
-- used to dereference a null writer and crash the server. So the setting must be rejected at CREATE
-- with a clear SUPPORT_IS_DISABLED error. A plain MergeTree WITHOUT the setting on the same kind of
-- disk must still create / insert / select / drop normally.

DROP TABLE IF EXISTS t_cas_dedup;
DROP TABLE IF EXISTS t_cas_plain;

-- (1) A table with non_replicated_deduplication_window on a content_addressed disk is rejected at CREATE.
CREATE TABLE t_cas_dedup (a UInt64, b UInt64)
ENGINE = MergeTree ORDER BY a
SETTINGS non_replicated_deduplication_window = 100, disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    name = '04285_content_addressed_dedup',
    path = '04285_content_addressed_dedup_pool/'); -- { serverError SUPPORT_IS_DISABLED }

-- (2) A plain table (no dedup window) on a content_addressed disk still works end-to-end.
CREATE TABLE t_cas_plain (a UInt64, b UInt64)
ENGINE = MergeTree ORDER BY a
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    name = '04285_content_addressed_plain',
    path = '04285_content_addressed_plain_pool/');

INSERT INTO t_cas_plain SELECT number, number * 2 FROM numbers(100);
SELECT 'plain_count', count() FROM t_cas_plain;
SELECT 'plain_sum', sum(b) FROM t_cas_plain;

DROP TABLE t_cas_plain;
SELECT 'dropped_ok';
