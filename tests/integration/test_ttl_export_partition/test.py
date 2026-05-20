"""Integration tests for `TTL ... EXPORT TO TABLE db.table`.

The stateless suite covers parser, single-replica happy path and skip-already-exported.
This suite covers what can only be observed on a multi-replica cluster: ZK race,
failure injection with backoff, serial behaviour, restart recovery, ALTER pickup,
the disabled-replica case, and dedup across a high-water-mark lookup.
"""

import logging
import time

import pytest

from helpers.cluster import ClickHouseCluster
from helpers.export_partition_helpers import (
    make_iceberg_s3,
    make_rmt,
    unique_suffix,
    wait_for_exception_count,
    wait_for_export_status,
    wait_for_export_to_start,
)
from helpers.network import PartitionManager


@pytest.fixture(scope="module")
def cluster():
    try:
        cluster = ClickHouseCluster(__file__)
        common = dict(
            main_configs=[
                "configs/named_collections.xml",
                "configs/allow_experimental_export_partition.xml",
            ],
            user_configs=["configs/users.d/profile.xml"],
            with_minio=True,
            stay_alive=True,
            with_zookeeper=True,
            keeper_required_feature_flags=["multi_read"],
        )
        cluster.add_instance("replica1", **common)
        cluster.add_instance("replica2", **common)
        cluster.add_instance(
            "replica_disabled",
            main_configs=[
                "configs/named_collections.xml",
                "configs/disable_experimental_export_partition.xml",
            ],
            user_configs=["configs/users.d/profile.xml"],
            with_minio=True,
            stay_alive=True,
            with_zookeeper=True,
            keeper_required_feature_flags=["multi_read"],
        )
        logging.info("Starting cluster...")
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


@pytest.fixture(autouse=True)
def drop_tables_after_test(cluster):
    yield
    for name, instance in cluster.instances.items():
        try:
            tables = instance.query(
                "SELECT name FROM system.tables WHERE database = 'default' FORMAT TabSeparated"
            ).strip()
            for table in tables.split("\n"):
                table = table.strip()
                if table:
                    instance.query(f"DROP TABLE IF EXISTS default.`{table}` SYNC")
        except Exception as exc:
            logging.warning("cleanup on %s failed: %s", name, exc)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def create_rmt_with_export_ttl(node, name, dst, interval="INTERVAL 1 DAY"):
    """ReplicatedMergeTree with EXPORT TTL set at create time so parts get export_ttl info on write."""
    node.query(
        f"""
        CREATE TABLE {name} (event_date Date, id UInt64, year UInt16)
        ENGINE = ReplicatedMergeTree('/clickhouse/tables/{{database}}/{name}', '{node.name}')
        PARTITION BY year ORDER BY id
        TTL event_date + {interval} EXPORT TO TABLE {dst}
        """
    )


def create_s3_dst(node, name):
    node.query(
        f"""
        CREATE TABLE {name} (event_date Date, id UInt64, year UInt16)
        ENGINE = S3(s3_conn, filename='{name}', format=Parquet, partition_strategy='hive')
        PARTITION BY year
        """
    )


def insert_expired_partition(node, name, year, ids):
    values = ", ".join(f"(toDate('{year}-01-01'), {i}, {year})" for i in ids)
    node.query(f"INSERT INTO {name} VALUES {values}")


def count_pending(node, src):
    return int(node.query(
        f"SELECT count() FROM system.replicated_partition_exports"
        f" WHERE source_table = '{src}' AND status = 'PENDING'"
    ).strip())


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_basic_to_iceberg(cluster):
    """Iceberg destination: expired partition exports end to end, single replica."""
    node = cluster.instances["replica1"]
    uid = unique_suffix()
    src = f"src_{uid}"
    dst = f"iceberg_{uid}"

    # Iceberg only accepts its own partition transforms (`toYearNumSinceEpoch`, not `toYear`)
    # and only signed integer types, so source and destination must be created inline to match.
    make_iceberg_s3(node, dst, "event_date Date, id Int64", partition_by="toYearNumSinceEpoch(event_date)")
    node.query(
        f"""
        CREATE TABLE {src} (event_date Date, id Int64)
        ENGINE = ReplicatedMergeTree('/clickhouse/tables/{{database}}/{src}', '{node.name}')
        PARTITION BY toYearNumSinceEpoch(event_date) ORDER BY id
        TTL event_date + INTERVAL 1 DAY EXPORT TO TABLE {dst}
        """
    )
    insert_expired_partition(node, src, 2000, [1, 2, 3])

    # `toYearNumSinceEpoch('2000-01-01')` = 2000 - 1970 = 30, so the partition_id is "30".
    wait_for_export_status(node, src, dst, "30", "COMPLETED")

    assert int(node.query(f"SELECT count() FROM {dst}").strip()) == 3
    assert int(node.query(f"SELECT count() FROM {src}").strip()) == 3  # not dropped locally


def test_only_one_replica_submits(cluster):
    """Both replicas have the TTL; only one ZK manifest is created."""
    r1 = cluster.instances["replica1"]
    r2 = cluster.instances["replica2"]
    uid = unique_suffix()
    src = f"src_{uid}"
    dst = f"dst_{uid}"

    # S3-engine tables are not replicated, so the destination must exist on every replica
    # that creates the source — the DDL-time schema check resolves it locally.
    create_s3_dst(r1, dst)
    create_s3_dst(r2, dst)
    create_rmt_with_export_ttl(r1, src, dst)
    create_rmt_with_export_ttl(r2, src, dst)

    insert_expired_partition(r1, src, 2000, [1, 2])
    r2.query(f"SYSTEM SYNC REPLICA {src}")

    wait_for_export_status(r1, src, dst, "2000", "COMPLETED")

    # Each replica sees the manifest via ZK, but only one row exists in the history
    # (keyed by partition_id, dest_db, dest_table). Both replicas report the same row.
    for node in (r1, r2):
        count = int(node.query(
            f"SELECT count() FROM system.replicated_partition_exports"
            f" WHERE source_table = '{src}' AND destination_table = '{dst}' AND partition_id = '2000'"
        ).strip())
        assert count == 1, f"{node.name}: expected exactly one entry, got {count}"


def test_failure_and_backoff(cluster):
    """Block S3, watch retries accumulate, unblock, expect eventual COMPLETED."""
    node = cluster.instances["replica1"]
    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    uid = unique_suffix()
    src = f"src_{uid}"
    dst = f"dst_{uid}"

    create_s3_dst(node, dst)
    create_rmt_with_export_ttl(node, src, dst)

    with PartitionManager() as pm:
        pm.add_rule({
            "instance": node, "destination": minio_ip, "protocol": "tcp",
            "destination_port": minio_port, "action": "REJECT --reject-with tcp-reset",
        })
        insert_expired_partition(node, src, 2000, [1, 2])

        # Wait until the scheduler has attempted submission and recorded at least one failure.
        # The TTL scheduler's own backoff is in-memory; we observe its retries through
        # exception_count growing on the manifest.
        wait_for_exception_count(node, src, dst, "2000", min_exception_count=2, timeout=60)

    # MinIO is reachable again; the next retry of the scheduler must succeed.
    wait_for_export_status(node, src, dst, "2000", "COMPLETED", timeout=120)

    rows = int(node.query(f"SELECT count() FROM {dst}").strip())
    assert rows == 2


def test_serial_across_partitions(cluster):
    """While any partition is in flight (PENDING / FAILED), no other partition for the same
    (source, dest) reaches PENDING. We force the in-flight to linger by blocking MinIO,
    insert three eligible partitions, and assert at-most-one in flight throughout.
    """
    node = cluster.instances["replica1"]
    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    uid = unique_suffix()
    src = f"src_{uid}"
    dst = f"dst_{uid}"

    create_s3_dst(node, dst)
    create_rmt_with_export_ttl(node, src, dst)

    with PartitionManager() as pm:
        pm.add_rule({
            "instance": node, "destination": minio_ip, "protocol": "tcp",
            "destination_port": minio_port, "action": "REJECT --reject-with tcp-reset",
        })

        for year, ids in [(2000, [1, 2]), (2001, [3]), (2002, [4])]:
            insert_expired_partition(node, src, year, ids)

        wait_for_export_to_start(node, src, dst, "2000", timeout=30)

        # Sample at 0.5s for 15s. With serial scheduling there must never be more than one
        # in-flight entry for this (source, dest). Track the maximum observed instead of
        # asserting per-sample so a single transient blip isn't the cause of a flake report.
        observations = []
        deadline = time.time() + 15
        while time.time() < deadline:
            observations.append(count_pending(node, src))
            time.sleep(0.5)

        assert observations, "no observations collected"
        assert max(observations) <= 1, (
            f"observed multiple in-flight partitions: {observations}"
        )

    # Drain everything once MinIO returns. Only the trailing partition is guaranteed to be
    # observable as COMPLETED — earlier ttl-origin markers are removed when the next one is
    # submitted. The destination row count below verifies that all three actually exported.
    wait_for_export_status(node, src, dst, "2002", "COMPLETED", timeout=120)

    assert int(node.query(f"SELECT count() FROM {dst}").strip()) == 4


def test_replica_restart_mid_export(cluster):
    """If the replica restarts mid-flight, the scheduler recovers current_export from ZK
    and does not queue a second partition before the first completes.
    """
    node = cluster.instances["replica1"]
    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    uid = unique_suffix()
    src = f"src_{uid}"
    dst = f"dst_{uid}"

    create_s3_dst(node, dst)
    create_rmt_with_export_ttl(node, src, dst)

    with PartitionManager() as pm:
        pm.add_rule({
            "instance": node, "destination": minio_ip, "protocol": "tcp",
            "destination_port": minio_port, "action": "REJECT --reject-with tcp-reset",
        })

        insert_expired_partition(node, src, 2000, [1])
        insert_expired_partition(node, src, 2001, [2])
        wait_for_export_to_start(node, src, dst, "2000", timeout=30)

    node.restart_clickhouse()

    # Both partitions complete after MinIO is back and the scheduler has re-attached
    # to the in-flight 2000 manifest from ZK.
    wait_for_export_status(node, src, dst, "2000", "COMPLETED", timeout=120)
    wait_for_export_status(node, src, dst, "2001", "COMPLETED", timeout=120)

    # No duplicate manifest for the trailing partition. The 2000 ttl-marker is removed
    # when 2001 is submitted (the "at most one ttl-origin manifest per (src, dest)" invariant),
    # so we check 2001, which is the latest and therefore not pruned.
    n_2001 = int(node.query(
        f"SELECT count() FROM system.replicated_partition_exports"
        f" WHERE source_table = '{src}' AND destination_table = '{dst}' AND partition_id = '2001'"
    ).strip())
    assert n_2001 == 1


def test_modify_ttl_picks_up_with_materialize(cluster):
    """ALTER MODIFY TTL adding an EXPORT TTL must take effect.
    Parts that pre-date the TTL require MATERIALIZE TTL to populate per-part export info.
    """
    node = cluster.instances["replica1"]
    uid = unique_suffix()
    src = f"src_{uid}"
    dst = f"dst_{uid}"

    create_s3_dst(node, dst)
    # No EXPORT TTL at create time.
    node.query(
        f"""
        CREATE TABLE {src} (event_date Date, id UInt64, year UInt16)
        ENGINE = ReplicatedMergeTree('/clickhouse/tables/{{database}}/{src}', 'r1')
        PARTITION BY year ORDER BY id
        """
    )
    insert_expired_partition(node, src, 2000, [1, 2])

    # Add the TTL; old parts lack export_ttl[result_column] so the scheduler skips them.
    # Disable the implicit materialise so we can demonstrate the explicit `MATERIALIZE TTL`
    # back-fill below — otherwise `MODIFY TTL` populates the per-part info itself.
    node.query(
        f"ALTER TABLE {src} MODIFY TTL event_date + INTERVAL 1 DAY EXPORT TO TABLE {dst}",
        settings={"materialize_ttl_after_modify": 0},
    )

    # Confirm no export happens within one tick.
    time.sleep(8)
    assert int(node.query(
        f"SELECT count() FROM system.replicated_partition_exports WHERE source_table = '{src}'"
    ).strip()) == 0

    # MATERIALIZE TTL back-fills per-part info; export becomes possible.
    node.query(f"ALTER TABLE {src} MATERIALIZE TTL", settings={"mutations_sync": 2})
    wait_for_export_status(node, src, dst, "2000", "COMPLETED", timeout=60)

    assert int(node.query(f"SELECT count() FROM {dst}").strip()) == 2


def test_disabled_replica(cluster):
    """A replica with allow_experimental_export_merge_tree_partition=0 must not submit."""
    r_on = cluster.instances["replica1"]
    r_off = cluster.instances["replica_disabled"]
    uid = unique_suffix()
    src = f"src_{uid}"
    dst = f"dst_{uid}"

    create_s3_dst(r_on, dst)
    create_s3_dst(r_off, dst)
    create_rmt_with_export_ttl(r_on, src, dst)
    create_rmt_with_export_ttl(r_off, src, dst)

    insert_expired_partition(r_on, src, 2000, [1])
    r_off.query(f"SYSTEM SYNC REPLICA {src}")

    wait_for_export_status(r_on, src, dst, "2000", "COMPLETED", timeout=60)

    # The disabled replica must not appear as the submitter (source_replica column).
    source_replica = r_on.query(
        f"SELECT source_replica FROM system.replicated_partition_exports"
        f" WHERE source_table = '{src}' AND destination_table = '{dst}' AND partition_id = '2000'"
    ).strip()
    assert source_replica != r_off.name, f"disabled replica submitted: {source_replica}"


def test_dedup_after_restart(cluster):
    """Restart wipes in-memory scheduler state. The scheduler reads the latest ttl-origin
    manifest from ZK on each tick and walks forward by `partition_id`, so already-exported
    partitions are not re-submitted.
    """
    node = cluster.instances["replica1"]
    uid = unique_suffix()
    src = f"src_{uid}"
    dst = f"dst_{uid}"

    create_s3_dst(node, dst)
    create_rmt_with_export_ttl(node, src, dst)

    insert_expired_partition(node, src, 2000, [1])
    insert_expired_partition(node, src, 2001, [2])
    # Only the trailing partition is guaranteed to be observable; earlier markers are pruned
    # when the next one is submitted.
    wait_for_export_status(node, src, dst, "2001", "COMPLETED", timeout=60)

    node.restart_clickhouse()

    # A fresh partition with a strictly newer expiration must still export, and 2000/2001
    # must not be re-submitted — the destination row count is the witness.
    insert_expired_partition(node, src, 2002, [3])
    wait_for_export_status(node, src, dst, "2002", "COMPLETED", timeout=60)

    n_2002 = int(node.query(
        f"SELECT count() FROM system.replicated_partition_exports"
        f" WHERE source_table = '{src}' AND destination_table = '{dst}' AND partition_id = '2002'"
    ).strip())
    assert n_2002 == 1, f"partition 2002 appears {n_2002} times (expected 1)"

    # Destination has rows from all three partitions, no duplicates.
    rows = int(node.query(f"SELECT count() FROM {dst}").strip())
    assert rows == 3
