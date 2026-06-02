-- Tags: no-fasttest
-- ^ content_addressed is an object-storage metadata type; keep it off the minimal fasttest image.

-- The M1 content-addressed manifest models a part as a flat set of top-level files (one manifest,
-- one ref) and cannot represent nested projection parts (backlog B5/B31). So a CREATE of a table
-- that has a projection on a content_addressed disk must fail closed at CREATE with a clear
-- SUPPORT_IS_DISABLED error, rather than being brought up and silently misbehaving. A table WITHOUT
-- a projection on the same kind of disk must still create / insert / select / drop normally.

DROP TABLE IF EXISTS t_cas_proj;
DROP TABLE IF EXISTS t_cas_plain;

-- (1) A table WITH a projection on a content_addressed disk is rejected at CREATE.
CREATE TABLE t_cas_proj
(
    a UInt64,
    b UInt64,
    PROJECTION p_by_b (SELECT a, b ORDER BY b)
)
ENGINE = MergeTree ORDER BY a
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    name = '04281_content_addressed_proj',
    path = '04281_content_addressed_proj_pool/'); -- { serverError SUPPORT_IS_DISABLED }

-- (2) A plain table (no projection) on a content_addressed disk still works end-to-end.
CREATE TABLE t_cas_plain (a UInt64, b UInt64)
ENGINE = MergeTree ORDER BY a
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    name = '04281_content_addressed_plain',
    path = '04281_content_addressed_plain_pool/');

INSERT INTO t_cas_plain SELECT number, number * 2 FROM numbers(100);
SELECT 'plain_count', count() FROM t_cas_plain;
SELECT 'plain_sum', sum(b) FROM t_cas_plain;

-- (3) Adding a projection to an existing content_addressed table via ALTER is also rejected
-- (ADD PROJECTION is an ALTER that needs hardlinks, which the disk no longer advertises).
ALTER TABLE t_cas_plain ADD PROJECTION p_late (SELECT a, b ORDER BY b); -- { serverError SUPPORT_IS_DISABLED }

DROP TABLE t_cas_plain;
SELECT 'dropped_ok';
