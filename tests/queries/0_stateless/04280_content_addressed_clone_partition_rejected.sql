-- Tags: no-fasttest
-- ^ content_addressed is an object-storage metadata type; keep it off the minimal fasttest image.

-- The part-cloning partition commands clone parts file-by-file via DiskObjectStorage::createHardLink
-- with no enclosing transaction, which on a content_addressed disk autocommits a one-file
-- manifest/ref per file and overwrites the destination ref — leaving the clone with only its last
-- file. This affects MOVE PARTITION, REPLACE PARTITION, ATTACH PARTITION ... FROM, and even plain
-- ATTACH PARTITION of the table's own detached parts (ATTACH re-clones the detached part). Until that
-- path is made transactional they are rejected with a clear SUPPORT_IS_DISABLED error rather than
-- silently corrupting. The pointer-unlink commands (DROP / DETACH / DROP DETACHED) stay allowed.

DROP TABLE IF EXISTS t_cas_clone_src;
DROP TABLE IF EXISTS t_cas_clone_dst;

CREATE TABLE t_cas_clone_src (a UInt64, p UInt8) ENGINE = MergeTree PARTITION BY p ORDER BY a
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    name = '04280_content_addressed_clone_src',
    path = '04280_content_addressed_clone_src_pool/');

CREATE TABLE t_cas_clone_dst (a UInt64, p UInt8) ENGINE = MergeTree PARTITION BY p ORDER BY a
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    name = '04280_content_addressed_clone_dst',
    path = '04280_content_addressed_clone_dst_pool/');

INSERT INTO t_cas_clone_src SELECT number, 1 FROM numbers(100);
INSERT INTO t_cas_clone_src SELECT number, 2 FROM numbers(50);

-- MOVE PARTITION ... TO TABLE clones parts → rejected with a clear error.
ALTER TABLE t_cas_clone_src MOVE PARTITION 1 TO TABLE t_cas_clone_dst; -- { serverError SUPPORT_IS_DISABLED }

-- REPLACE PARTITION clones parts from another table → rejected.
ALTER TABLE t_cas_clone_dst REPLACE PARTITION 1 FROM t_cas_clone_src; -- { serverError SUPPORT_IS_DISABLED }

-- ATTACH PARTITION ... FROM clones parts from another table → rejected.
ALTER TABLE t_cas_clone_dst ATTACH PARTITION 1 FROM t_cas_clone_src; -- { serverError SUPPORT_IS_DISABLED }

-- Plain ATTACH PARTITION of the table's own detached part re-clones it → rejected.
ALTER TABLE t_cas_clone_src DETACH PARTITION 1;
SELECT 'after_detach', count() FROM t_cas_clone_src;
ALTER TABLE t_cas_clone_src ATTACH PARTITION 1; -- { serverError SUPPORT_IS_DISABLED }

-- The pointer-unlink commands (DETACH above, DROP here) are still allowed and behave normally.
ALTER TABLE t_cas_clone_src DROP PARTITION 2;
SELECT 'after_drop', count() FROM t_cas_clone_src;

DROP TABLE t_cas_clone_src;
DROP TABLE t_cas_clone_dst;
SELECT 'dropped_ok';
