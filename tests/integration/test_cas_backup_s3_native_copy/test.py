"""BACKUP/RESTORE of a CAS table to an S3 destination sharing the pool's authority.

`sameKind` then matches and `BackupWriterS3` copies objects server-side instead of reading
through the CAS read path. That path must handle two shapes: a blob, whose object is
`[envelope][payload]` so the file starts at a non-zero offset, and an inline manifest entry,
which has no object at all. `getStorageObjects` reports neither -- it drops the offset and
returns an empty key.

Columns are chosen so one table yields both shapes: placement keys on the file name, so every
column makes blobs while per-part metadata stays inline. `n` and `arr` add the `.null.bin` and
`.size0.bin` substreams.
"""

import uuid

import pytest

from helpers.client import QueryRuntimeException
from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

STORAGE_POLICY = "cas_backup_s3"

S3_AUTHORITY = "http://rustfs1:11121"
S3_CREDENTIALS = "'clickhouse', 'clickhouse'"

NUM_ROWS = 100000

COLUMNS = ["k", "s", "n", "arr"]

RUN_TOKEN = uuid.uuid4().hex


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    cluster.add_instance(
        "node",
        main_configs=["configs/storage_conf.xml"],
        with_rustfs=True,
        with_remote_database_disk=False,
        stay_alive=True,
    )
    try:
        cluster.start()
        yield
    finally:
        cluster.shutdown()


def backup_s3_destination(name):
    return f"S3('{S3_AUTHORITY}/test/backups/{RUN_TOKEN}/{name}', {S3_CREDENTIALS})"


def backup_disk_destination(disk, name):
    return f"Disk('{disk}', '{RUN_TOKEN}/{name}')"


def create_and_fill(node, table, storage_policy=STORAGE_POLICY):
    node.query(f"DROP TABLE IF EXISTS {table} SYNC")
    node.query(
        f"""
        CREATE TABLE {table} (k UInt64, s String, n Nullable(Int64), arr Array(UInt32))
        ENGINE = MergeTree ORDER BY k
        SETTINGS storage_policy = '{storage_policy}', min_bytes_for_wide_part = 0
        """
    )
    node.query(
        f"""
        INSERT INTO {table}
        SELECT
            number,
            randomPrintableASCII(64),
            if(number % 7 = 0, NULL, toInt64(number)),
            [toUInt32(number), toUInt32(number + 1)]
        FROM numbers({NUM_ROWS})
        """
    )


def column_fingerprints(node, table):
    """Order-independent per-column hash; reading every column fetches every blob."""
    exprs = ", ".join(
        f"sum(cityHash64(ifNull(toString({column}), '<null>')))" for column in COLUMNS
    )
    row = node.query(f"SELECT count(), {exprs} FROM {table}").strip().split("\t")
    return dict(zip(["count"] + COLUMNS, row))


def transfer_events(node, query_id):
    """How many S3 operations of each mechanism the query issued.

    `server_side_part_copy` is `UploadPartCopy`, the only copy that can name a byte range.
    `server_side_object_copy` is `CopyObject`, which always takes the whole object.
    `buffered_write` and `buffered_read` are the puts and gets a copy through the server pays.
    """
    node.query("SYSTEM FLUSH LOGS query_log")
    row = node.query(
        f"""
        SELECT
            ProfileEvents['S3UploadPartCopy'],
            ProfileEvents['S3CopyObject'],
            ProfileEvents['S3PutObject'] + ProfileEvents['S3UploadPart'],
            ProfileEvents['S3GetObject']
        FROM system.query_log
        WHERE type = 'QueryFinish' AND query_id = '{query_id}'
        ORDER BY event_time DESC LIMIT 1
        """
    ).strip()
    assert row, f"no query_log row for {query_id}"
    names = ["server_side_part_copy", "server_side_object_copy", "buffered_write", "buffered_read"]
    return dict(zip(names, (int(value) for value in row.split("\t"))))


@pytest.mark.parametrize("allow_native_copy", [True, False])
def test_backup_to_s3_round_trip(allow_native_copy):
    """A blob is `[envelope][payload]`, so its copy must be ranged: `UploadPartCopy`, never
    `CopyObject`. Inline entries have no object and go through buffers either way, and a restore onto
    a CAS disk writes every file through the CAS write path.
    """
    node = cluster.instances["node"]
    suffix = "native" if allow_native_copy else "buffered"
    table = f"cas_backup_{suffix}"
    restored = f"{table}_restored"
    s3_destination = backup_s3_destination(suffix)
    backup_query_id = f"{table}_backup_{RUN_TOKEN}"
    restore_query_id = f"{table}_restore_{RUN_TOKEN}"

    create_and_fill(node, table)
    expected = column_fingerprints(node, table)

    node.query(
        f"BACKUP TABLE {table} TO {s3_destination} "
        f"SETTINGS allow_s3_native_copy = {int(allow_native_copy)}",
        query_id=backup_query_id,
    )

    backup = transfer_events(node, backup_query_id)
    assert backup["server_side_object_copy"] == 0, (
        "CopyObject cannot express a range, so the envelope would land in the backup"
    )
    assert backup["buffered_write"] > 0, "inline entries have no object and go through buffers"
    if allow_native_copy:
        assert backup["server_side_part_copy"] > 0, "blobs must be copied server-side with a range"
    else:
        assert backup["server_side_part_copy"] == 0, (
            "allow_s3_native_copy = 0 leaves no copy inside S3, so every file goes through buffers"
        )

    node.query(f"DROP TABLE IF EXISTS {restored} SYNC")
    node.query(
        f"RESTORE TABLE {table} AS {restored} FROM {s3_destination} "
        f"SETTINGS allow_s3_native_copy = {int(allow_native_copy)}",
        query_id=restore_query_id,
    )

    restore = transfer_events(node, restore_query_id)
    assert (restore["server_side_part_copy"], restore["server_side_object_copy"]) == (0, 0), (
        "a restore onto a CAS disk writes every file through the CAS write path"
    )
    assert restore["buffered_read"] > 0, "the backup must be read through buffers"
    assert restore["buffered_write"] > 0, "the restored part must be written through buffers"

    actual = column_fingerprints(node, restored)

    assert actual["count"] == expected["count"]
    differing = [c for c in COLUMNS if actual[c] != expected[c]]
    assert not differing, f"columns differ after restore: {differing}"

    assert (
        node.query(
            f"CHECK TABLE {restored} SETTINGS check_query_single_value_result = 1"
        ).strip()
        == "1"
    )

    node.query(f"DROP TABLE {table} SYNC")
    node.query(f"DROP TABLE {restored} SYNC")


@pytest.mark.parametrize("backup_disk", ["backup_disk_s3_plain", "backup_disk_s3"])
def test_backup_to_disk_only_buffers(backup_disk):
    node = cluster.instances["node"]
    table = f"cas_backup_to_{backup_disk}"
    restored = f"{table}_restored"
    disk_destination = backup_disk_destination(backup_disk, table)
    backup_query_id = f"{table}_backup_{RUN_TOKEN}"
    restore_query_id = f"{table}_restore_{RUN_TOKEN}"

    create_and_fill(node, table)
    expected = column_fingerprints(node, table)

    node.query(f"BACKUP TABLE {table} TO {disk_destination}", query_id=backup_query_id)

    backup = transfer_events(node, backup_query_id)
    assert (backup["server_side_part_copy"], backup["server_side_object_copy"]) == (0, 0), (
        "a Disk(...) destination goes through IDisk::copyFile, which no longer copies CAS objects"
    )
    assert backup["buffered_write"] > 0, "every file must reach the destination through buffers"

    node.query(f"DROP TABLE IF EXISTS {restored} SYNC")
    node.query(
        f"RESTORE TABLE {table} AS {restored} FROM {disk_destination}",
        query_id=restore_query_id,
    )

    restore = transfer_events(node, restore_query_id)
    assert (restore["server_side_part_copy"], restore["server_side_object_copy"]) == (0, 0), (
        "RESTORE onto a CAS disk must write through the CAS path, not copy objects into the pool"
    )
    assert restore["buffered_write"] > 0, "the restored part must be written through buffers"

    assert (
        node.query(
            f"SELECT storage_policy FROM system.tables WHERE name = '{restored}'"
        ).strip()
        == STORAGE_POLICY
    )

    actual = column_fingerprints(node, restored)

    assert actual["count"] == expected["count"]
    differing = [c for c in COLUMNS if actual[c] != expected[c]]
    assert not differing, f"columns differ after restore: {differing}"

    assert (
        node.query(
            f"CHECK TABLE {restored} SETTINGS check_query_single_value_result = 1"
        ).strip()
        == "1"
    )

    node.query(f"DROP TABLE {table} SYNC")
    node.query(f"DROP TABLE {restored} SYNC")


def test_backup_to_s3_falls_back_without_multipart():
    """Only `UploadPartCopy` can express a range. With multipart copy off there is no server-side
    operation left, so the copy must go through buffers instead of taking the whole object.
    """
    node = cluster.instances["node"]
    table = "cas_backup_no_multipart"
    restored = f"{table}_restored"
    s3_destination = backup_s3_destination("no_multipart")
    query_id = f"cas_backup_no_multipart_{RUN_TOKEN}"
    no_multipart = {"s3_allow_multipart_copy": 0}

    create_and_fill(node, table)
    expected = column_fingerprints(node, table)

    node.query(
        f"BACKUP TABLE {table} TO {s3_destination} SETTINGS allow_s3_native_copy = 1",
        query_id=query_id,
        settings=no_multipart,
    )
    events = transfer_events(node, query_id)

    assert events["server_side_part_copy"] == 0, (
        "multipart copy was disabled but UploadPartCopy still ran"
    )
    assert events["server_side_object_copy"] == 0, (
        "a ranged copy fell back to CopyObject, which would take the envelope"
    )
    assert events["buffered_write"] > 0, (
        "nothing was uploaded through the server, so nothing was copied at all"
    )

    node.query(f"DROP TABLE IF EXISTS {restored} SYNC")
    node.query(
        f"RESTORE TABLE {table} AS {restored} FROM {s3_destination}", settings=no_multipart
    )

    actual = column_fingerprints(node, restored)
    assert actual["count"] == expected["count"]
    assert not [c for c in COLUMNS if actual[c] != expected[c]]

    node.query(f"DROP TABLE {table} SYNC")
    node.query(f"DROP TABLE {restored} SYNC")


def test_backup_to_s3_ranged_copy_spans_several_parts():
    """A payload larger than one upload part makes the ranged copy issue several `UploadPartCopy`
    requests. Every part but the first carries an offset of its own, so the payload window must be
    applied to each one and not only to the first.
    """
    node = cluster.instances["node"]
    table = "cas_backup_multipart_range"
    restored = f"{table}_restored"
    s3_destination = backup_s3_destination("multipart_range")
    query_id = f"{table}_backup_{RUN_TOKEN}"
    small_parts = {"s3_min_upload_part_size": 5 * 1024 * 1024}
    fingerprint = "SELECT count(), sum(cityHash64(s)) FROM {}"

    node.query(f"DROP TABLE IF EXISTS {table} SYNC")
    node.query(
        f"""
        CREATE TABLE {table} (k UInt64, s String)
        ENGINE = MergeTree ORDER BY k
        SETTINGS storage_policy = '{STORAGE_POLICY}', min_bytes_for_wide_part = 0
        """
    )
    node.query(
        f"""
        INSERT INTO {table}
        SELECT number, randomPrintableASCII(128) FROM numbers({NUM_ROWS})
        """
    )
    expected = node.query(fingerprint.format(table)).strip()

    node.query(
        f"BACKUP TABLE {table} TO {s3_destination}",
        query_id=query_id,
        settings=small_parts,
    )

    events = transfer_events(node, query_id)
    assert events["server_side_part_copy"] > 1, (
        "no blob was copied in more than one part, so the per-part offset stayed untested"
    )
    assert events["server_side_object_copy"] == 0, (
        "CopyObject has no range: the envelope would land in the backup"
    )

    node.query(f"DROP TABLE IF EXISTS {restored} SYNC")
    node.query(f"RESTORE TABLE {table} AS {restored} FROM {s3_destination}")

    assert node.query(fingerprint.format(restored)).strip() == expected
    assert (
        node.query(
            f"CHECK TABLE {restored} SETTINGS check_query_single_value_result = 1"
        ).strip()
        == "1"
    )

    node.query(f"DROP TABLE {table} SYNC")
    node.query(f"DROP TABLE {restored} SYNC")


def test_move_partition_from_cas_to_s3_disk_with_empty_arrays():
    """A zero-size `.bin` still becomes a blob: `partFileMustStayBlob` keys on the file name, not
    the size. Moving it off a CAS disk must copy it through buffers.
    """
    node = cluster.instances["node"]
    table = "cas_move_empty_arrays"
    query_id = f"{table}_move_{RUN_TOKEN}"

    node.query(f"DROP TABLE IF EXISTS {table} SYNC")
    node.query(
        f"""
        CREATE TABLE {table} (k UInt64, s String, empty Array(UInt32))
        ENGINE = MergeTree ORDER BY k
        SETTINGS storage_policy = 'cas_then_plain', min_bytes_for_wide_part = 0
        """
    )
    node.query(
        f"""
        INSERT INTO {table}
        SELECT number, randomPrintableASCII(64), []
        FROM numbers({NUM_ROWS})
        """
    )

    expected = node.query(
        f"SELECT count(), sum(cityHash64(s)), sum(length(empty)) FROM {table}"
    ).strip()

    node.query(
        f"ALTER TABLE {table} MOVE PARTITION tuple() TO DISK 'backup_disk_s3'",
        query_id=query_id,
    )

    events = transfer_events(node, query_id)
    assert (events["server_side_part_copy"], events["server_side_object_copy"]) == (0, 0), (
        "a CAS source is windowed, so no copy may run inside S3"
    )
    assert events["buffered_write"] > 0, "every file must be moved through buffers"

    assert (
        node.query(
            f"SELECT count(), sum(cityHash64(s)), sum(length(empty)) FROM {table}"
        ).strip()
        == expected
    )
    assert (
        node.query(
            f"CHECK TABLE {table} SETTINGS check_query_single_value_result = 1"
        ).strip()
        == "1"
    )

    node.query(f"DROP TABLE {table} SYNC")


def test_backup_to_s3_with_empty_arrays():
    """A zero-size `.bin` is a blob with an empty payload. A ranged copy of zero bytes reaches
    `calculatePartSize(0)`, which throws, so it must be copied through buffers. Only a backup with
    `deduplicate_files = 0` passes empty files to the writer; a deduplicated one drops them earlier.
    """
    node = cluster.instances["node"]
    table = "cas_backup_empty_arrays"
    restored = f"{table}_restored"
    s3_destination = backup_s3_destination("empty_arrays")
    query_id = f"{table}_backup_{RUN_TOKEN}"
    fingerprint = "SELECT count(), sum(cityHash64(s)), sum(length(empty)) FROM {}"

    node.query(f"DROP TABLE IF EXISTS {table} SYNC")
    node.query(
        f"""
        CREATE TABLE {table} (k UInt64, s String, empty Array(UInt32))
        ENGINE = MergeTree ORDER BY k
        SETTINGS storage_policy = '{STORAGE_POLICY}', min_bytes_for_wide_part = 0
        """
    )
    node.query(
        f"""
        INSERT INTO {table}
        SELECT number, randomPrintableASCII(64), []
        FROM numbers({NUM_ROWS})
        """
    )
    expected = node.query(fingerprint.format(table)).strip()

    node.query(
        f"BACKUP TABLE {table} TO {s3_destination} SETTINGS deduplicate_files = 0",
        query_id=query_id,
    )

    events = transfer_events(node, query_id)
    assert events["server_side_part_copy"] > 0, (
        "a zero-size blob must not cost the other blobs their ranged copy"
    )
    assert events["server_side_object_copy"] == 0, (
        "CopyObject has no range: the envelope would land in the backup"
    )

    empty_files_in_backup = int(
        node.query(
            f"""
            SELECT count()
            FROM s3('{S3_AUTHORITY}/test/backups/{RUN_TOKEN}/empty_arrays/**', {S3_CREDENTIALS}, 'One')
            WHERE _size = 0
            SETTINGS s3_skip_empty_files = 0
            """
        ).strip()
    )
    assert empty_files_in_backup > 0, "no zero-size file reached the backup writer"

    node.query(f"DROP TABLE IF EXISTS {restored} SYNC")
    node.query(f"RESTORE TABLE {table} AS {restored} FROM {s3_destination}")

    assert node.query(fingerprint.format(restored)).strip() == expected
    assert (
        node.query(
            f"CHECK TABLE {restored} SETTINGS check_query_single_value_result = 1"
        ).strip()
        == "1"
    )

    node.query(f"DROP TABLE {table} SYNC")
    node.query(f"DROP TABLE {restored} SYNC")


def test_incremental_backup_to_s3():
    """An incremental backup copies only the files that changed since the base backup, so the
    ranged copy runs against a subset of the part files and the restore reads from both backups.
    """
    node = cluster.instances["node"]
    table = "cas_backup_incremental"
    restored = f"{table}_restored"
    base = backup_s3_destination("incremental_base")
    incremental = backup_s3_destination("incremental")
    query_id = f"{table}_backup_{RUN_TOKEN}"

    create_and_fill(node, table)
    node.query(f"BACKUP TABLE {table} TO {base}")

    node.query(
        f"""
        INSERT INTO {table}
        SELECT
            number,
            randomPrintableASCII(64),
            if(number % 5 = 0, NULL, toInt64(number)),
            [toUInt32(number)]
        FROM numbers({NUM_ROWS}, {NUM_ROWS})
        """
    )
    expected = column_fingerprints(node, table)

    node.query(
        f"BACKUP TABLE {table} TO {incremental} SETTINGS base_backup = {base}",
        query_id=query_id,
    )

    events = transfer_events(node, query_id)
    assert events["server_side_part_copy"] > 0, (
        "the files of the new part must still be copied inside S3 with a range"
    )
    assert events["server_side_object_copy"] == 0, (
        "CopyObject has no range: the envelope would land in the backup"
    )
    assert events["buffered_write"] > 0, "inline entries have no object and go through buffers"

    node.query(f"DROP TABLE IF EXISTS {restored} SYNC")
    node.query(f"RESTORE TABLE {table} AS {restored} FROM {incremental}")

    actual = column_fingerprints(node, restored)
    assert actual["count"] == expected["count"]
    differing = [c for c in COLUMNS if actual[c] != expected[c]]
    assert not differing, f"columns differ after restore: {differing}"

    node.query(f"DROP TABLE {table} SYNC")
    node.query(f"DROP TABLE {restored} SYNC")


def test_move_partition_between_plain_and_encrypted_s3_disks():
    """No CAS here. The capability predicate must only narrow: `sameKind` ignores `is_encrypted`, and
    `DiskEncrypted` reports its delegate's description, so a predicate that replaced `operator==`
    would let this pair through and the cast to `DiskObjectStorage` would throw.
    """
    node = cluster.instances["node"]
    table = "plain_to_encrypted"
    to_encrypted_query_id = f"{table}_to_encrypted_{RUN_TOKEN}"
    from_encrypted_query_id = f"{table}_from_encrypted_{RUN_TOKEN}"

    node.query(f"DROP TABLE IF EXISTS {table} SYNC")
    node.query(
        f"""
        CREATE TABLE {table} (k UInt64, s String)
        ENGINE = MergeTree ORDER BY k
        SETTINGS storage_policy = 'plain_then_encrypted', min_bytes_for_wide_part = 0
        """
    )
    node.query(
        f"INSERT INTO {table} SELECT number, randomPrintableASCII(64) FROM numbers({NUM_ROWS})"
    )

    expected = node.query(f"SELECT count(), sum(cityHash64(s)) FROM {table}").strip()

    node.query(
        f"ALTER TABLE {table} MOVE PARTITION tuple() TO DISK 'disk_plain_s3_encrypted'",
        query_id=to_encrypted_query_id,
    )
    to_encrypted = transfer_events(node, to_encrypted_query_id)
    assert to_encrypted["buffered_write"] > 0, (
        "one side encrypts and the other does not, so the bytes must pass through the server"
    )
    assert (
        node.query(f"SELECT count(), sum(cityHash64(s)) FROM {table}").strip() == expected
    )

    node.query(
        f"ALTER TABLE {table} MOVE PARTITION tuple() TO DISK 'backup_disk_s3'",
        query_id=from_encrypted_query_id,
    )
    from_encrypted = transfer_events(node, from_encrypted_query_id)
    assert from_encrypted["buffered_write"] > 0, (
        "the way back decrypts, so the bytes must pass through the server again"
    )
    assert (
        node.query(f"SELECT count(), sum(cityHash64(s)) FROM {table}").strip() == expected
    )

    node.query(f"DROP TABLE {table} SYNC")


def test_move_partition_between_plain_s3_disks_copies_server_side():
    """The capability defaults to false, so a description that forgets to claim it silently loses the
    server-side copy. Two ordinary s3 disks must keep it.
    """
    node = cluster.instances["node"]
    table = "plain_to_plain"
    query_id = f"plain_to_plain_move_{RUN_TOKEN}"

    node.query(f"DROP TABLE IF EXISTS {table} SYNC")
    node.query(
        f"""
        CREATE TABLE {table} (k UInt64, s String)
        ENGINE = MergeTree ORDER BY k
        SETTINGS storage_policy = 'plain_then_plain', min_bytes_for_wide_part = 0
        """
    )
    node.query(
        f"INSERT INTO {table} SELECT number, randomPrintableASCII(64) FROM numbers({NUM_ROWS})"
    )
    expected = node.query(f"SELECT count(), sum(cityHash64(s)) FROM {table}").strip()

    node.query(
        f"ALTER TABLE {table} MOVE PARTITION tuple() TO DISK 'disk_plain_s3_second'",
        query_id=query_id,
    )

    events = transfer_events(node, query_id)
    assert events["server_side_object_copy"] > 0, (
        "a non-CAS disk pair lost its server-side copy: a whole file is a whole object here, so "
        "CopyObject is the expected operation"
    )
    assert (
        node.query(f"SELECT count(), sum(cityHash64(s)) FROM {table}").strip() == expected
    )

    node.query(f"DROP TABLE {table} SYNC")


def test_backup_to_file_keeps_fs_copy():
    """Both sides take their description from `DiskLocal::getLocalDataSourceDescription`. A forgotten
    claim there gives `false && false`, and `BackupWriterFile` silently stops using `fs::copy`.
    """
    node = cluster.instances["node"]
    table = "plain_local_to_file"
    query_id = f"plain_local_backup_{RUN_TOKEN}"

    node.query(f"DROP TABLE IF EXISTS {table} SYNC")
    node.query(
        f"""
        CREATE TABLE {table} (k UInt64, s String)
        ENGINE = MergeTree ORDER BY k
        SETTINGS min_bytes_for_wide_part = 0
        """
    )
    node.query(
        f"INSERT INTO {table} SELECT number, randomPrintableASCII(64) FROM numbers({NUM_ROWS})"
    )

    node.query(
        f"BACKUP TABLE {table} TO File('{RUN_TOKEN}/file_backup')", query_id=query_id
    )
    node.query("SYSTEM FLUSH LOGS query_log")

    read_bytes = int(
        node.query(
            f"""
            SELECT ProfileEvents['ReadBufferFromFileDescriptorReadBytes']
            FROM system.query_log
            WHERE type = 'QueryFinish' AND query_id = '{query_id}'
            ORDER BY event_time DESC LIMIT 1
            """
        ).strip()
    )
    bytes_on_disk = int(
        node.query(
            f"""
            SELECT sum(bytes_on_disk) FROM system.parts
            WHERE table = '{table}' AND active
            """
        ).strip()
    )

    assert bytes_on_disk > 0, "the table has no active parts, so the assertion below proves nothing"
    assert read_bytes < bytes_on_disk * 3 // 2, (
        f"BACKUP TO File(...) read {read_bytes} bytes with {bytes_on_disk} on disk. One pass is the "
        "checksum pass every entry pays; a second pass means fs::copy was replaced by a buffered copy"
    )

    node.query(f"DROP TABLE {table} SYNC")


def test_move_partition_from_cached_cas_to_s3_disk_only_buffers():
    """`wrapWithCache` reuses the CAS metadata storage for a CAS disk, so the cache disk must inherit
    the same answer and stay out of the server-side copy.
    """
    node = cluster.instances["node"]
    table = "cas_cached_move"
    query_id = f"cas_cached_move_{RUN_TOKEN}"

    create_and_fill(node, table, "cas_cached_then_plain")
    expected = column_fingerprints(node, table)

    node.query(
        f"ALTER TABLE {table} MOVE PARTITION tuple() TO DISK 'backup_disk_s3'",
        query_id=query_id,
    )

    events = transfer_events(node, query_id)
    assert (events["server_side_part_copy"], events["server_side_object_copy"]) == (0, 0), (
        "a cache disk over CAS reported itself as whole-object"
    )
    assert events["buffered_write"] > 0, "the moved part must be written through buffers"
    assert column_fingerprints(node, table) == expected

    node.query(f"DROP TABLE {table} SYNC")


def test_backup_to_disk_cas_is_rejected():
    """A `CAS` disk takes a part only as a whole part in one transaction. A backup's own layout mirrors
    the table's data directory, so its files sit under a part directory too and `isPartFilePath` matches
    them - which is why writing a backup into a `CAS` disk is refused rather than silently accepted.
    """
    node = cluster.instances["node"]
    table = "backup_into_cas"
    disk_destination = backup_disk_destination("disk_cas_backup_s3", table)

    node.query(f"DROP TABLE IF EXISTS {table} SYNC")
    node.query(
        f"""
        CREATE TABLE {table} (k UInt64, s String)
        ENGINE = MergeTree ORDER BY k
        SETTINGS min_bytes_for_wide_part = 0
        """
    )
    node.query(
        f"INSERT INTO {table} SELECT number, randomPrintableASCII(64) FROM numbers({NUM_ROWS})"
    )

    with pytest.raises(QueryRuntimeException) as raised:
        node.query(f"BACKUP TABLE {table} TO {disk_destination}")
    assert "Autocommit writes are not supported for content part files" in str(raised.value)

    node.query(f"DROP TABLE {table} SYNC")
