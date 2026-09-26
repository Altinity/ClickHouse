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
        stay_alive=True,
    )
    try:
        cluster.start()
        yield
    finally:
        cluster.shutdown()


def backup_destination(name):
    return f"S3('{S3_AUTHORITY}/test/backups/{RUN_TOKEN}/{name}', {S3_CREDENTIALS})"


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


@pytest.mark.parametrize("allow_native_copy", [True, False])
def test_native_copy_round_trip(allow_native_copy):
    node = cluster.instances["node"]
    suffix = "native" if allow_native_copy else "buffered"
    table = f"cas_backup_{suffix}"
    restored = f"{table}_restored"
    destination = backup_destination(suffix)

    create_and_fill(node, table)
    expected = column_fingerprints(node, table)

    node.query(
        f"BACKUP TABLE {table} TO {destination} "
        f"SETTINGS allow_s3_native_copy = {int(allow_native_copy)}"
    )

    node.query(f"DROP TABLE IF EXISTS {restored} SYNC")
    node.query(
        f"RESTORE TABLE {table} AS {restored} FROM {destination} "
        f"SETTINGS allow_s3_native_copy = {int(allow_native_copy)}"
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


def copy_events(node, query_id):
    node.query("SYSTEM FLUSH LOGS query_log")
    events = node.query(
        f"""
        SELECT ProfileEvents['S3UploadPartCopy'], ProfileEvents['S3CopyObject']
        FROM system.query_log
        WHERE type = 'QueryFinish' AND query_id = '{query_id}'
        ORDER BY event_time DESC LIMIT 1
        """
    ).strip()
    assert events, f"no query_log row for {query_id}"
    upload_part_copy, copy_object = (int(value) for value in events.split("\t"))
    return upload_part_copy, copy_object


@pytest.mark.parametrize(
    "storage_policy, multipart_copy",
    [
        pytest.param(STORAGE_POLICY, True, id="multipart_copy"),
        pytest.param("cas_backup_s3_no_multipart", False, id="no_multipart_copy"),
    ],
)
@pytest.mark.parametrize("backup_disk", ["backup_disk_s3_plain", "backup_disk_s3"])
def test_backup_to_disk_on_same_authority(backup_disk, storage_policy, multipart_copy):
    node = cluster.instances["node"]
    table = f"cas_backup_to_{backup_disk}_{storage_policy}"
    restored = f"{table}_restored"
    destination = f"Disk('{backup_disk}', '{RUN_TOKEN}/{table}')"
    backup_query_id = f"{table}_backup_{RUN_TOKEN}"
    restore_query_id = f"{table}_restore_{RUN_TOKEN}"

    create_and_fill(node, table, storage_policy)
    expected = column_fingerprints(node, table)

    node.query(f"BACKUP TABLE {table} TO {destination}", query_id=backup_query_id)

    upload_part_copy, copy_object = copy_events(node, backup_query_id)
    assert copy_object == 0, "CopyObject has no range: the envelope would land in the backup"
    if multipart_copy:
        assert upload_part_copy > 0, "blobs were not copied with a ranged server-side copy"
    else:
        assert upload_part_copy == 0, "multipart copy is disabled but UploadPartCopy still ran"

    node.query(f"DROP TABLE IF EXISTS {restored} SYNC")
    node.query(
        f"RESTORE TABLE {table} AS {restored} FROM {destination}",
        query_id=restore_query_id,
    )

    upload_part_copy, copy_object = copy_events(node, restore_query_id)
    assert (upload_part_copy, copy_object) == (
        0,
        0,
    ), "RESTORE onto a CAS disk must write through the CAS path, not copy objects into the pool"

    assert (
        node.query(
            f"SELECT storage_policy FROM system.tables WHERE name = '{restored}'"
        ).strip()
        == storage_policy
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


def test_blobs_use_ranged_copy_and_inline_falls_back():
    """A blob is `[envelope][payload]`, so its copy must be ranged: `UploadPartCopy`, never
    `CopyObject`. Inline entries have no object and must go through buffers.
    """
    node = cluster.instances["node"]
    table = "cas_backup_mechanism"
    destination = backup_destination("mechanism")
    query_id = f"cas_backup_mechanism_{RUN_TOKEN}"

    create_and_fill(node, table)
    node.query(
        f"BACKUP TABLE {table} TO {destination} SETTINGS allow_s3_native_copy = 1",
        query_id=query_id,
    )
    node.query("SYSTEM FLUSH LOGS query_log")

    events = node.query(
        f"""
        SELECT ProfileEvents['S3UploadPartCopy'], ProfileEvents['S3CopyObject']
        FROM system.query_log
        WHERE type = 'QueryFinish' AND query_id = '{query_id}'
        ORDER BY event_time DESC LIMIT 1
        """
    ).strip()
    assert events, "no query_log row for the backup query"
    upload_part_copy, copy_object = (int(value) for value in events.split("\t"))

    assert upload_part_copy > 0, "no ranged server-side copy happened"
    assert copy_object == 0, "CopyObject has no range: the envelope would land in the backup"
    assert node.contains_in_log(
        "has no object of its own, copying through buffers"
    ), "inline entries did not fall back"

    node.query(f"DROP TABLE {table} SYNC")


def test_ranged_copy_falls_back_without_multipart():
    """Only `UploadPartCopy` can express a range. With multipart copy off there is no server-side
    operation left, so the copy must go through buffers instead of taking the whole object.
    """
    node = cluster.instances["node"]
    table = "cas_backup_no_multipart"
    restored = f"{table}_restored"
    destination = backup_destination("no_multipart")
    query_id = f"cas_backup_no_multipart_{RUN_TOKEN}"
    no_multipart = {"s3_allow_multipart_copy": 0}

    create_and_fill(node, table)
    expected = column_fingerprints(node, table)

    node.query(
        f"BACKUP TABLE {table} TO {destination} SETTINGS allow_s3_native_copy = 1",
        query_id=query_id,
        settings=no_multipart,
    )
    node.query("SYSTEM FLUSH LOGS query_log")

    events = node.query(
        f"""
        SELECT
            ProfileEvents['S3UploadPartCopy'],
            ProfileEvents['S3CopyObject'],
            ProfileEvents['S3PutObject'] + ProfileEvents['S3UploadPart']
        FROM system.query_log
        WHERE type = 'QueryFinish' AND query_id = '{query_id}'
        ORDER BY event_time DESC LIMIT 1
        """
    ).strip()
    assert events, "no query_log row for the backup query"
    upload_part_copy, copy_object, uploaded = (int(value) for value in events.split("\t"))

    assert upload_part_copy == 0, "multipart copy was disabled but UploadPartCopy still ran"
    assert copy_object == 0, "a ranged copy fell back to CopyObject, which would take the envelope"
    assert uploaded > 0, "nothing was uploaded through the server, so nothing was copied at all"
    assert node.contains_in_log(
        "Ranged native copy needs multipart copy"
    ), "the copy did not reach the ranged-copy fallback"

    node.query(f"DROP TABLE IF EXISTS {restored} SYNC")
    node.query(
        f"RESTORE TABLE {table} AS {restored} FROM {destination}", settings=no_multipart
    )

    actual = column_fingerprints(node, restored)
    assert actual["count"] == expected["count"]
    assert not [c for c in COLUMNS if actual[c] != expected[c]]

    node.query(f"DROP TABLE {table} SYNC")
    node.query(f"DROP TABLE {restored} SYNC")
