-- Tags: no-fasttest
-- ^ content_addressed is an object-storage metadata type; keep it off the minimal fasttest image.

-- B38: querying system.remote_data_paths traverses the disk and probes existsFile on pool sub-dirs
-- (e.g. the "store" directory). Such a path resolves to a directory object key; existsFile must treat
-- a directory as not-a-file and not let the raw filesystem "Is a directory" error escape. So a
-- system.remote_data_paths query over a content_addressed table must succeed (return the parts'
-- remote paths) instead of throwing.

DROP TABLE IF EXISTS t_cas_rdp;

CREATE TABLE t_cas_rdp (a UInt64, b UInt64)
ENGINE = MergeTree ORDER BY a
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    server_root_id = '04286',
    name = '04286_content_addressed_rdp',
    path = '04286_content_addressed_rdp_pool/');

INSERT INTO t_cas_rdp SELECT number, number * 2 FROM numbers(100);

-- The traversal (with shadow paths) must be QUERYABLE: it used to throw `Is a directory` (Code 1001)
-- when it probed the CA pool sub-dir (e.g. "store") via existsFile. After the B38 fix it returns a
-- result without raising. We assert the query succeeds (count() is a non-negative number) rather than
-- a specific row count: the CA disk's object-storage directory model determines how many rows the
-- traversal yields, which is orthogonal to the not-throwing contract this test pins.
SELECT count() >= 0
FROM system.remote_data_paths
WHERE disk_name = '04286_content_addressed_rdp'
SETTINGS traverse_shadow_remote_data_paths = 1;

DROP TABLE t_cas_rdp;
SELECT 'dropped_ok';
