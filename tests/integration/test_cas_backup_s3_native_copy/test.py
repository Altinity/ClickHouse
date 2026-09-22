"""BACKUP/RESTORE of a CAS table to an S3 destination on the same object store.

When the backup destination and the CAS pool share an S3 authority,
`DataSourceDescription::sameKind` matches and `BackupWriterS3::copyFileFromDisk` copies
the source object server-side instead of reading it through the CAS read path.

A CAS blob object is `[blob_header_len bytes of envelope][payload]`, and the payload is
what the MergeTree file actually is. `BlobLocation` carries that `offset`, but
`ContentAddressedMetadataStorage::getStorageObjects` returns only `key` and `length` --
the offset is dropped. Small per-part files are inline manifest entries and get a
placeholder with an empty remote key. Neither shape survives a server-side copy that
addresses the object from byte 0.

`test_native_copy_round_trip` is the oracle: the restored table must equal the source.
`test_native_copy_branch_is_reached` guards it against going vacuous -- if the native
branch is never entered, the oracle proves nothing about this path.
"""

import uuid

import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

STORAGE_POLICY = "cas_backup_s3"

S3_AUTHORITY = "http://rustfs1:11121"
S3_CREDENTIALS = "'clickhouse', 'clickhouse'"

NUM_ROWS = 200000

RUN_TOKEN = uuid.uuid4().hex

FORCE_SINGLE_OPERATION_COPY = {"s3_max_single_operation_copy_size": 5 * 1024**3}


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


def create_and_fill(node, table):
    node.query(f"DROP TABLE IF EXISTS {table} SYNC")
    node.query(
        f"""
        CREATE TABLE {table} (k UInt64, v String)
        ENGINE = MergeTree ORDER BY k
        SETTINGS storage_policy = '{STORAGE_POLICY}', min_bytes_for_wide_part = 0
        """
    )
    node.query(
        f"""
        INSERT INTO {table}
        SELECT number, randomPrintableASCII(64) FROM numbers({NUM_ROWS})
        """
    )


def fingerprint(node, table):
    return node.query(
        f"SELECT count(), sum(k), sum(cityHash64(v)) FROM {table}"
    ).strip()


@pytest.mark.parametrize("allow_native_copy", [True, False])
def test_native_copy_round_trip(allow_native_copy):
    node = cluster.instances["node"]
    suffix = "native" if allow_native_copy else "buffered"
    table = f"cas_backup_{suffix}"
    restored = f"{table}_restored"
    destination = backup_destination(suffix)

    create_and_fill(node, table)
    expected = fingerprint(node, table)

    node.query(
        f"BACKUP TABLE {table} TO {destination} "
        f"SETTINGS allow_s3_native_copy = {int(allow_native_copy)}",
        settings=FORCE_SINGLE_OPERATION_COPY,
    )

    node.query(f"DROP TABLE IF EXISTS {restored} SYNC")
    node.query(
        f"RESTORE TABLE {table} AS {restored} FROM {destination} "
        f"SETTINGS allow_s3_native_copy = {int(allow_native_copy)}",
        settings=FORCE_SINGLE_OPERATION_COPY,
    )

    assert fingerprint(node, restored) == expected
    assert (
        node.query(
            f"CHECK TABLE {restored} SETTINGS check_query_single_value_result = 1"
        ).strip()
        == "1"
    )

    node.query(f"DROP TABLE {table} SYNC")
    node.query(f"DROP TABLE {restored} SYNC")


def test_native_copy_branch_is_reached():
    node = cluster.instances["node"]
    table = "cas_backup_probe"
    destination = backup_destination("probe")

    create_and_fill(node, table)

    query_id = f"cas_backup_native_copy_{RUN_TOKEN}"
    node.query(
        f"BACKUP TABLE {table} TO {destination} SETTINGS allow_s3_native_copy = 1",
        query_id=query_id,
        settings=FORCE_SINGLE_OPERATION_COPY,
    )
    node.query("SYSTEM FLUSH LOGS query_log")

    copy_object_events = node.query(
        f"""
        SELECT ProfileEvents['S3CopyObject'] FROM system.query_log
        WHERE type = 'QueryFinish' AND query_id = '{query_id}'
        ORDER BY event_time DESC LIMIT 1
        """
    ).strip()

    assert copy_object_events != "", "no query_log row for the backup query"
    assert int(copy_object_events) > 0, (
        "BackupWriterS3 never took its native-copy branch, so "
        "test_native_copy_round_trip does not cover it; check whether "
        "DataSourceDescription::sameKind still matches for a CAS disk"
    )

    node.query(f"DROP TABLE {table} SYNC")
