-- Tags: no-fasttest
-- ^ content_addressed is an object-storage metadata type; keep it off the minimal fasttest image.

-- Introspection coverage for the content-addressed (CA) garbage collector: the
-- `SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION <disk>` command runs one GC round synchronously and
-- the round is recorded in `system.content_addressed_garbage_collection_log` (a Start + Finish row
-- per round, like `part_log`). We build a CA disk inline (named, so the SYSTEM command can target it),
-- create garbage by inserting then truncating, run the round a few times, flush the log, and assert
-- the rows are there with the right shape — including a non-empty per-round `ProfileEvents` delta
-- (the Manual round runs on the query thread, which always has an attached ThreadStatus that captures
-- ProfileEvents).

DROP TABLE IF EXISTS t_cas_gc_introspection;

-- A named inline CA disk: the `name` is what `SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION <name>`
-- targets and what lands in the log's `disk_name` column.
CREATE TABLE t_cas_gc_introspection (a UInt64, s String)
ENGINE = MergeTree ORDER BY a
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    name = '05007_content_addressed_gc_introspection',
    path = '05007_content_addressed_gc_introspection_pool/',
    gc_enabled = 1,
    gc_interval_sec = 1),
    old_parts_lifetime = 1;

-- Two distinct inserts => distinct blobs (not deduped away), then TRUNCATE drops every ref so the
-- blobs/trees become unreferenced GC fodder.
INSERT INTO t_cas_gc_introspection SELECT number, toString(number) FROM numbers(1000);
INSERT INTO t_cas_gc_introspection SELECT number, toString(number) FROM numbers(1000, 1000);
TRUNCATE TABLE t_cas_gc_introspection;

-- Run several synchronous rounds: the first rounds mark the retired candidates, later rounds delete
-- them once the durable watermark floor advances past the builds (the background renewer does this).
SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION '05007_content_addressed_gc_introspection';
SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION '05007_content_addressed_gc_introspection';
SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION '05007_content_addressed_gc_introspection';

SYSTEM FLUSH LOGS content_addressed_garbage_collection_log;

-- A Start, a Finish, and a Manual-triggered row were all recorded for this disk.
SELECT
    countIf(event_type = 'Start') > 0,
    countIf(event_type = 'Finish') > 0,
    countIf(trigger = 'Manual') > 0
FROM system.content_addressed_garbage_collection_log
WHERE disk_name LIKE '%05007_content_addressed_gc_introspection%';

-- A synchronous Manual Finish captured a non-empty per-round ProfileEvents delta (the round touches
-- the object storage, so Cas*/Disk*/S3* counters are non-zero). The query thread is always attached,
-- so capture is active for the Manual path.
SELECT any(length(ProfileEvents)) > 0
FROM system.content_addressed_garbage_collection_log
WHERE disk_name LIKE '%05007_content_addressed_gc_introspection%'
  AND event_type = 'Finish'
  AND trigger = 'Manual';

-- The error path: a non-CA disk (the always-present local `default`) is rejected.
SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION 'default'; -- { serverError BAD_ARGUMENTS }

DROP TABLE t_cas_gc_introspection;
SELECT 'ok';
