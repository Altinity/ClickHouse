import logging
import time
import uuid

import pytest

from helpers.cluster import ClickHouseCluster
from helpers.network import PartitionManager


def wait_for_export_status(
    node,
    mt_table: str,
    iceberg_table: str,
    partition_id: str,
    expected_status: str = "COMPLETED",
    timeout: int = 60,
    poll_interval: float = 0.5,
):
    start_time = time.time()
    last_status = None
    while time.time() - start_time < timeout:
        status = node.query(
            f"""
            SELECT status FROM system.replicated_partition_exports
            WHERE source_table = '{mt_table}'
                AND destination_table = '{iceberg_table}'
                AND partition_id = '{partition_id}'
            """
        ).strip()
        last_status = status
        if status and status == expected_status:
            return status
        time.sleep(poll_interval)
    raise TimeoutError(
        f"Export status did not reach '{expected_status}' within {timeout}s. "
        f"Last status: '{last_status}'"
    )


def wait_for_export_to_start(
    node,
    mt_table: str,
    iceberg_table: str,
    partition_id: str,
    timeout: int = 10,
    poll_interval: float = 0.2,
):
    start_time = time.time()
    while time.time() - start_time < timeout:
        count = node.query(
            f"""
            SELECT count() FROM system.replicated_partition_exports
            WHERE source_table = '{mt_table}'
              AND destination_table = '{iceberg_table}'
              AND partition_id = '{partition_id}'
            """
        ).strip()
        if count != "0":
            return True
        time.sleep(poll_interval)
    raise TimeoutError(f"Export of partition {partition_id!r} did not start within {timeout}s.")


@pytest.fixture(scope="module")
def cluster():
    try:
        cluster = ClickHouseCluster(__file__)
        cluster.add_instance(
            "replica1",
            main_configs=["configs/allow_experimental_export_partition.xml"],
            user_configs=["configs/users.d/profile.xml"],
            with_minio=True,
            stay_alive=True,
            with_zookeeper=True,
            keeper_required_feature_flags=["multi_read"],
        )
        cluster.add_instance(
            "replica2",
            main_configs=["configs/allow_experimental_export_partition.xml"],
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
    """Drop all tables in the default database after every test.

    Without this, ReplicatedMergeTree tables from completed tests remain alive and keep
    running ZooKeeper background threads.  With many tables alive simultaneously the
    ZooKeeper session becomes overwhelmed and subsequent tests start seeing
    operation-timeout / session-expired errors.
    """
    yield
    for instance_name, instance in cluster.instances.items():
        try:
            tables_str = instance.query(
                "SELECT name FROM system.tables WHERE database = 'default' FORMAT TabSeparated"
            ).strip()
            if not tables_str:
                continue
            for table in tables_str.split("\n"):
                table = table.strip()
                if table:
                    instance.query(f"DROP TABLE IF EXISTS default.`{table}` SYNC")
        except Exception as e:
            logging.warning(
                f"drop_tables_after_test: cleanup failed on {instance_name}: {e}"
            )


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def create_replicated_mt(node, mt_table: str, replica_name: str):
    node.query(
        f"""
        CREATE TABLE {mt_table}
        (id Int64, year Int32)
        ENGINE = ReplicatedMergeTree('/clickhouse/tables/{mt_table}', '{replica_name}')
        PARTITION BY year
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
        """
    )


def create_iceberg_s3_table(node, iceberg_table: str, if_not_exists: bool = False):
    """Create (or attach to an existing) IcebergS3 table at a per-test MinIO prefix."""
    ine = "IF NOT EXISTS " if if_not_exists else ""
    node.query(
        f"""
        CREATE TABLE {ine}{iceberg_table}
        (id Int64, year Int32)
        ENGINE = IcebergS3(
            'http://minio1:9001/root/data/{iceberg_table}/',
            'minio',
            'ClickHouse_Minio_P@ssw0rd'
        )
        PARTITION BY year SETTINGS s3_retry_attempts = 1
        """
    )


def setup_tables(cluster, mt_table: str, iceberg_table: str, nodes: list | None = None):
    """
    Create the ReplicatedMergeTree table on the given nodes, insert data on the first
    node, wait for replication, then create the Iceberg destination table on each node.

    The Iceberg table is created on the first node (which initialises the S3 metadata).
    Subsequent nodes attach to the same path with IF NOT EXISTS.

    `nodes` defaults to ["replica1", "replica2"].
    """
    if nodes is None:
        nodes = ["replica1", "replica2"]

    instances = [cluster.instances[n] for n in nodes]
    primary = instances[0]

    for i, instance in enumerate(instances):
        create_replicated_mt(instance, mt_table, nodes[i])

    primary.query(f"INSERT INTO {mt_table} VALUES (1, 2020), (2, 2020), (3, 2020), (4, 2021)")
    for instance in instances[1:]:
        instance.query(f"SYSTEM SYNC REPLICA {mt_table}")

    create_iceberg_s3_table(primary, iceberg_table)
    for instance in instances[1:]:
        create_iceberg_s3_table(instance, iceberg_table, if_not_exists=True)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_export_partition_to_iceberg(cluster):
    """
    Basic happy path: export a single partition and verify row count and content.
    """
    node = cluster.instances["replica1"]

    uid = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"mt_{uid}"
    iceberg_table = f"iceberg_{uid}"

    setup_tables(cluster, mt_table, iceberg_table, nodes=["replica1"])

    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {iceberg_table}"
    )
    wait_for_export_status(node, mt_table, iceberg_table, "2020", "COMPLETED")

    count = int(node.query(f"SELECT count() FROM {iceberg_table}").strip())
    assert count == 3, f"Expected 3 rows in Iceberg table after export, got {count}"

    result = node.query(f"SELECT id, year FROM {iceberg_table} ORDER BY id").strip()
    assert result == "1\t2020\n2\t2020\n3\t2020", (
        f"Unexpected data in Iceberg table:\n{result}"
    )


def test_export_two_partitions_to_iceberg(cluster):
    """
    Export two partitions in a single ALTER TABLE statement and verify that both
    land in the Iceberg table with correct row counts.
    """
    node = cluster.instances["replica1"]

    uid = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"mt_{uid}"
    iceberg_table = f"iceberg_{uid}"

    setup_tables(cluster, mt_table, iceberg_table, nodes=["replica1"])

    node.query(
        f"""
        ALTER TABLE {mt_table}
            EXPORT PARTITION ID '2020' TO TABLE {iceberg_table},
            EXPORT PARTITION ID '2021' TO TABLE {iceberg_table}
        """
    )

    wait_for_export_status(node, mt_table, iceberg_table, "2020", "COMPLETED")
    wait_for_export_status(node, mt_table, iceberg_table, "2021", "COMPLETED")

    count_2020 = int(node.query(f"SELECT count() FROM {iceberg_table} WHERE year = 2020").strip())
    count_2021 = int(node.query(f"SELECT count() FROM {iceberg_table} WHERE year = 2021").strip())

    assert count_2020 == 3, f"Expected 3 rows for year=2020, got {count_2020}"
    assert count_2021 == 1, f"Expected 1 row for year=2021, got {count_2021}"


def test_failure_is_logged_in_system_table(cluster):
    """
    When S3 is unreachable the export must be marked FAILED in
    system.replicated_partition_exports with a non-zero exception_count.
    """
    node = cluster.instances["replica1"]
    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    uid = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"mt_{uid}"
    iceberg_table = f"iceberg_{uid}"

    setup_tables(cluster, mt_table, iceberg_table, nodes=["replica1"])

    node.query(f"SYSTEM STOP MOVES {mt_table}")

    node.query(f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {iceberg_table} SETTINGS export_merge_tree_partition_max_retries = 1")

    with PartitionManager() as pm:
        pm.add_rule({
            "instance": node,
            "destination": node.ip_address,
            "protocol": "tcp",
            "source_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        })
        pm.add_rule({
            "instance": node,
            "destination": minio_ip,
            "protocol": "tcp",
            "destination_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        })

        node.query(f"SYSTEM START MOVES {mt_table}")

        wait_for_export_status(node, mt_table, iceberg_table, "2020", "FAILED", timeout=60)

    status = node.query(
        f"""
        SELECT status FROM system.replicated_partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{iceberg_table}'
          AND partition_id = '2020'
          SETTINGS export_merge_tree_partition_system_table_prefer_remote_information = 1
        """
    ).strip()
    assert status == "FAILED", f"Expected FAILED status, got: {status!r}"

    exception_count = int(node.query(
        f"""
        SELECT any(exception_count) FROM system.replicated_partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{iceberg_table}'
          AND partition_id = '2020'
          SETTINGS export_merge_tree_partition_system_table_prefer_remote_information = 1
        """
    ).strip())
    assert exception_count > 0, "Expected non-zero exception_count in system.replicated_partition_exports"


def test_inject_short_living_failures(cluster):
    """
    Transient S3 failures must not prevent the export from completing: after the
    network is restored the export should retry and eventually land COMPLETED.
    """
    node = cluster.instances["replica1"]
    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    uid = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"mt_{uid}"
    iceberg_table = f"iceberg_{uid}"

    setup_tables(cluster, mt_table, iceberg_table, nodes=["replica1"])

    node.query(f"SYSTEM STOP MOVES {mt_table}")

    node.query(f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {iceberg_table} SETTINGS export_merge_tree_partition_max_retries = 100")

    with PartitionManager() as pm:
        pm.add_rule({
            "instance": node,
            "destination": node.ip_address,
            "protocol": "tcp",
            "source_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        })
        pm.add_rule({
            "instance": node,
            "destination": minio_ip,
            "protocol": "tcp",
            "destination_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        })

        node.query(f"SYSTEM START MOVES {mt_table}")

        # Let at least one retry happen before restoring the network.
        time.sleep(15)

    wait_for_export_status(node, mt_table, iceberg_table, "2020", "COMPLETED")

    count = int(node.query(f"SELECT count() FROM {iceberg_table} WHERE year = 2020").strip())
    assert count == 3, f"Expected 3 rows after retry, got {count}"

    status = node.query(
        f"""
        SELECT status FROM system.replicated_partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{iceberg_table}'
          AND partition_id = '2020'
        """
    ).strip()
    assert status == "COMPLETED", f"Expected COMPLETED in system table, got: {status!r}"

    exception_count = int(node.query(
        f"""
        SELECT exception_count FROM system.replicated_partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{iceberg_table}'
          AND partition_id = '2020'
          SETTINGS export_merge_tree_partition_system_table_prefer_remote_information = 1
        """
    ).strip())
    assert exception_count >= 1, "Expected at least one transient exception to be recorded"


def test_export_partition_scheduler_skipped_when_moves_stopped(cluster):
    """
    Verify that selectPartsToExport() skips the scheduler entirely when moves
    are stopped (moves_blocker guard at the top of the function).

    No ZK locks are acquired and no background tasks are submitted, so the
    Iceberg table must remain empty across multiple scheduler cycles.  Once moves
    are re-enabled the export completes and rows appear in the Iceberg table.
    """
    node = cluster.instances["replica1"]

    uid = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"mt_{uid}"
    iceberg_table = f"iceberg_{uid}"

    setup_tables(cluster, mt_table, iceberg_table, nodes=["replica1"])

    node.query(f"SYSTEM STOP MOVES {mt_table}")

    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {iceberg_table}"
    )

    wait_for_export_to_start(node, mt_table, iceberg_table, "2020")

    # Wait for several scheduler cycles (each fires every 5 s).
    # If the guard is absent the scheduler would run and rows would appear in the Iceberg table.
    time.sleep(12)

    status = node.query(
        f"SELECT status FROM system.replicated_partition_exports"
        f" WHERE source_table = '{mt_table}' AND destination_table = '{iceberg_table}'"
        f" AND partition_id = '2020'"
    ).strip()

    assert status == "PENDING", f"Expected PENDING while moves are stopped, got '{status}'"

    count = int(node.query(f"SELECT count() FROM {iceberg_table} WHERE year = 2020").strip())
    assert count == 0, f"Expected 0 rows in Iceberg table while scheduler is skipped, got {count}"

    node.query(f"SYSTEM START MOVES {mt_table}")

    wait_for_export_status(node, mt_table, iceberg_table, "2020", "COMPLETED")

    count = int(node.query(f"SELECT count() FROM {iceberg_table} WHERE year = 2020").strip())
    assert count == 3, f"Expected 3 rows in Iceberg table after export completed, got {count}"


def test_export_partition_resumes_after_stop_moves(cluster):
    """
    Verify that SYSTEM STOP MOVES before EXPORT PARTITION does not permanently
    orphan the ZooKeeper part lock for Iceberg destinations.

    When moves are stopped the scheduler still picks parts up and submits them to
    the background executor, but ExportPartTask::isCancelled() returns true (via
    moves_blocker), causing QUERY_WAS_CANCELLED before any data is written.  The
    fix in handlePartExportFailure must release the ZK lock so the part is retried
    once moves are restarted.
    """
    node = cluster.instances["replica1"]

    uid = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"mt_{uid}"
    iceberg_table = f"iceberg_{uid}"

    setup_tables(cluster, mt_table, iceberg_table, nodes=["replica1"])

    node.query(f"SYSTEM STOP MOVES {mt_table}")

    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {iceberg_table}"
        f" SETTINGS export_merge_tree_partition_max_retries = 50"
    )

    wait_for_export_to_start(node, mt_table, iceberg_table, "2020")

    # Give the scheduler enough time to attempt (and cancel) the part task at least once.
    time.sleep(5)

    status = node.query(
        f"SELECT status FROM system.replicated_partition_exports"
        f" WHERE source_table = '{mt_table}' AND destination_table = '{iceberg_table}'"
        f" AND partition_id = '2020'"
    ).strip()
    assert status == "PENDING", f"Expected PENDING while moves are stopped, got '{status}'"

    count = int(node.query(f"SELECT count() FROM {iceberg_table} WHERE year = 2020").strip())
    assert count == 0, f"Expected 0 rows in Iceberg table while moves are stopped, got {count}"

    node.query(f"SYSTEM START MOVES {mt_table}")

    wait_for_export_status(node, mt_table, iceberg_table, "2020", "COMPLETED")

    count = int(node.query(f"SELECT count() FROM {iceberg_table} WHERE year = 2020").strip())
    assert count == 3, f"Expected 3 rows in Iceberg table after export completed, got {count}"


def test_export_partition_resumes_after_stop_moves_during_export(cluster):
    """
    Verify that SYSTEM STOP MOVES issued while an Iceberg export is actively
    retrying (S3 blocked) does not permanently orphan the ZooKeeper part lock.
    """
    node = cluster.instances["replica1"]
    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    uid = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"mt_{uid}"
    iceberg_table = f"iceberg_{uid}"

    setup_tables(cluster, mt_table, iceberg_table, nodes=["replica1"])

    node.query(f"SYSTEM STOP MOVES {mt_table}")

    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {iceberg_table}"
        f" SETTINGS export_merge_tree_partition_max_retries = 50")

    wait_for_export_to_start(node, mt_table, iceberg_table, "2020")

    with PartitionManager() as pm:
        pm.add_rule({
            "instance": node,
            "destination": node.ip_address,
            "protocol": "tcp",
            "source_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        })
        pm.add_rule({
            "instance": node,
            "destination": minio_ip,
            "protocol": "tcp",
            "destination_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        })

        node.query(f"SYSTEM STOP MOVES {mt_table}")

        time.sleep(3)

        status = node.query(
            f"SELECT status FROM system.replicated_partition_exports"
            f" WHERE source_table = '{mt_table}' AND destination_table = '{iceberg_table}'"
            f" AND partition_id = '2020'"
        ).strip()
        assert status == "PENDING", (
            f"Expected PENDING while moves are stopped and S3 is blocked, got '{status}'"
        )

        node.query(f"SYSTEM START MOVES {mt_table}")

    # MinIO is now unblocked; the next scheduler cycle should succeed.
    wait_for_export_status(node, mt_table, iceberg_table, "2020", "COMPLETED")

    count = int(node.query(f"SELECT count() FROM {iceberg_table} WHERE year = 2020").strip())
    assert count == 3, f"Expected 3 rows in Iceberg table after export completed, got {count}"


def _make_iceberg_s3(node, name: str, columns: str, partition_by: str = "") -> None:
    """Create an IcebergS3 table at a per-test MinIO prefix."""
    pclause = f"PARTITION BY {partition_by}" if partition_by else ""
    node.query(
        f"""
        CREATE TABLE {name} ({columns})
        ENGINE = IcebergS3(
            'http://minio1:9001/root/data/{name}/',
            'minio',
            'ClickHouse_Minio_P@ssw0rd'
        )
        {pclause} SETTINGS s3_retry_attempts = 1
        """
    )


def _make_rmt(node, name: str, columns: str, partition_by: str) -> None:
    """Create a single-replica ReplicatedMergeTree with the given partition key."""
    node.query(
        f"""
        CREATE TABLE {name} ({columns})
        ENGINE = ReplicatedMergeTree('/clickhouse/tables/{name}', 'replica1')
        PARTITION BY {partition_by} ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
        """
    )


def _first_partition_id(node, table: str) -> str:
    """Return the partition_id of the first active part of *table*."""
    return node.query(
        f"SELECT partition_id FROM system.parts"
        f" WHERE table = '{table}' AND database = currentDatabase()"
        f" AND active LIMIT 1"
    ).strip()


def test_partition_transform_compatibility_accepted(cluster):
    """
    Verify that EXPORT PARTITION is accepted (no BAD_ARGUMENTS) for every
    supported transform when the MergeTree and Iceberg partition specs match.

    Cases covered:
    1. Compound identity (year, region)
    2. Year transform  – toYearNumSinceEpoch(event_date)
    3. Month transform – toMonthNumSinceEpoch(event_date)
    4. truncate[4]     – icebergTruncate(4, category)
    5. bucket[8]       – icebergBucket(8, user_id)
    6. Compound mixed  – (toYearNumSinceEpoch(event_date), icebergBucket(16, user_id))
    """
    node = cluster.instances["replica1"]
    uid = str(uuid.uuid4()).replace("-", "_")

    def check_accepted(mt, iceberg, description):
        pid = _first_partition_id(node, mt)
        node.query(
            f"ALTER TABLE {mt} EXPORT PARTITION ID '{pid}' TO TABLE {iceberg}"
        )

    # 1. Compound identity: (year, region)
    cols = "id Int64, year Int32, region String"
    t = f"mt_acc_1_{uid}"; i = f"iceberg_acc_1_{uid}"
    _make_rmt(node, t, cols, "(year, region)")
    node.query(f"INSERT INTO {t} VALUES (1, 2023, 'EU')")
    _make_iceberg_s3(node, i, cols, "(year, region)")
    check_accepted(t, i, "compound identity (year, region)")

    # 2. Year transform
    cols = "id Int64, event_date Date"
    t = f"mt_acc_2_{uid}"; i = f"iceberg_acc_2_{uid}"
    _make_rmt(node, t, cols, "toYearNumSinceEpoch(event_date)")
    node.query(f"INSERT INTO {t} VALUES (1, '2020-06-15')")
    _make_iceberg_s3(node, i, cols, "toYearNumSinceEpoch(event_date)")
    check_accepted(t, i, "year transform")

    # 3. Month transform
    cols = "id Int64, event_date Date"
    t = f"mt_acc_3_{uid}"; i = f"iceberg_acc_3_{uid}"
    _make_rmt(node, t, cols, "toMonthNumSinceEpoch(event_date)")
    node.query(f"INSERT INTO {t} VALUES (1, '2020-06-15')")
    _make_iceberg_s3(node, i, cols, "toMonthNumSinceEpoch(event_date)")
    check_accepted(t, i, "month transform")

    # 4. truncate[4]
    cols = "id Int64, category String"
    t = f"mt_acc_4_{uid}"; i = f"iceberg_acc_4_{uid}"
    _make_rmt(node, t, cols, "icebergTruncate(4, category)")
    node.query(f"INSERT INTO {t} VALUES (1, 'clickhouse')")
    _make_iceberg_s3(node, i, cols, "icebergTruncate(4, category)")
    check_accepted(t, i, "truncate[4]")

    # 5. bucket[8]
    cols = "id Int64, user_id Int64"
    t = f"mt_acc_5_{uid}"; i = f"iceberg_acc_5_{uid}"
    _make_rmt(node, t, cols, "icebergBucket(8, user_id)")
    node.query(f"INSERT INTO {t} VALUES (1, 42)")
    _make_iceberg_s3(node, i, cols, "icebergBucket(8, user_id)")
    check_accepted(t, i, "bucket[8]")

    # 6. Compound mixed: year(event_date) + bucket[16](user_id)
    cols = "id Int64, event_date Date, user_id Int64"
    t = f"mt_acc_6_{uid}"; i = f"iceberg_acc_6_{uid}"
    _make_rmt(node, t, cols, "(toYearNumSinceEpoch(event_date), icebergBucket(16, user_id))")
    node.query(f"INSERT INTO {t} VALUES (1, '2021-03-01', 99)")
    _make_iceberg_s3(node, i, cols, "(toYearNumSinceEpoch(event_date), icebergBucket(16, user_id))")
    check_accepted(t, i, "compound year+bucket[16]")


def test_partition_transform_compatibility_rejected(cluster):
    """
    Verify that mismatched partition specs are rejected with BAD_ARGUMENTS.

    Cases covered:
    1. Compound field order reversed: MergeTree (year, region) vs Iceberg (region, year)
    2. Transform mismatch on same column: year-transform vs identity
    3. Bucket count mismatch: bucket[8] vs bucket[16]
    4. Truncate width mismatch: truncate[4] vs truncate[8]
    5. Field-count mismatch: 2-field MergeTree vs 1-field Iceberg
    6. Unsupported MergeTree expression (intDiv — not an Iceberg transform)
    """
    node = cluster.instances["replica1"]
    uid = str(uuid.uuid4()).replace("-", "_")

    def assert_rejected(mt, iceberg, description):
        # The compatibility check fires synchronously; any partition ID works here.
        pid = _first_partition_id(node, mt)
        error = node.query_and_get_error(
            f"ALTER TABLE {mt} EXPORT PARTITION ID '{pid}' TO TABLE {iceberg}"
        )
        assert "BAD_ARGUMENTS" in error, (
            f"[{description}] Expected BAD_ARGUMENTS, got: {error!r}"
        )

    # 1. Compound field order reversed
    cols = "id Int64, year Int32, region String"
    t = f"mt_rej_1_{uid}"; i = f"iceberg_rej_1_{uid}"
    _make_rmt(node, t, cols, "(year, region)")
    node.query(f"INSERT INTO {t} VALUES (1, 2020, 'EU')")
    _make_iceberg_s3(node, i, cols, "(region, year)")
    assert_rejected(t, i, "compound field order reversed")

    # 2. Transform mismatch: MergeTree year-transform, Iceberg identity on same Date col
    cols = "id Int64, event_date Date"
    t = f"mt_rej_2_{uid}"; i = f"iceberg_rej_2_{uid}"
    _make_rmt(node, t, cols, "toYearNumSinceEpoch(event_date)")
    node.query(f"INSERT INTO {t} VALUES (1, '2020-01-01')")
    _make_iceberg_s3(node, i, cols, "event_date")   # identity, not year-transform
    assert_rejected(t, i, "year-transform vs identity on same column")

    # 3. Bucket count mismatch: bucket[8] vs bucket[16]
    cols = "id Int64, user_id Int64"
    t = f"mt_rej_3_{uid}"; i = f"iceberg_rej_3_{uid}"
    _make_rmt(node, t, cols, "icebergBucket(8, user_id)")
    node.query(f"INSERT INTO {t} VALUES (1, 42)")
    _make_iceberg_s3(node, i, cols, "icebergBucket(16, user_id)")
    assert_rejected(t, i, "bucket[8] vs bucket[16]")

    # 4. Truncate width mismatch: truncate[4] vs truncate[8]
    cols = "id Int64, category String"
    t = f"mt_rej_4_{uid}"; i = f"iceberg_rej_4_{uid}"
    _make_rmt(node, t, cols, "icebergTruncate(4, category)")
    node.query(f"INSERT INTO {t} VALUES (1, 'clickhouse')")
    _make_iceberg_s3(node, i, cols, "icebergTruncate(8, category)")
    assert_rejected(t, i, "truncate[4] vs truncate[8]")

    # 5. Field-count mismatch: MergeTree has 2 fields, Iceberg has 1
    cols = "id Int64, year Int32, region String"
    t = f"mt_rej_5_{uid}"; i = f"iceberg_rej_5_{uid}"
    _make_rmt(node, t, cols, "(year, region)")
    node.query(f"INSERT INTO {t} VALUES (1, 2020, 'EU')")
    _make_iceberg_s3(node, i, cols, "year")
    assert_rejected(t, i, "2-field MergeTree vs 1-field Iceberg")

    # 6. Unsupported MergeTree expression: intDiv(year, 100) is not an Iceberg transform
    cols = "id Int64, year Int32"
    t = f"mt_rej_6_{uid}"; i = f"iceberg_rej_6_{uid}"
    _make_rmt(node, t, cols, "intDiv(year, 100)")
    node.query(f"INSERT INTO {t} VALUES (1, 2020)")
    _make_iceberg_s3(node, i, cols, "year")
    assert_rejected(t, i, "unsupported MergeTree expression intDiv")


def test_partition_key_compatibility_check(cluster):
    """
    Verify that EXPORT PARTITION throws BAD_ARGUMENTS synchronously when the
    MergeTree partition key does not match the Iceberg table's partition spec,
    and is accepted without error when the keys match.

    Three cases:
    1. Column mismatch   – MergeTree PARTITION BY year, Iceberg PARTITION BY id
    2. Count mismatch    – MergeTree PARTITION BY year, Iceberg unpartitioned
    3. Matching keys     – both PARTITION BY year (must be accepted)
    """
    node = cluster.instances["replica1"]

    uid = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"mt_{uid}"

    create_replicated_mt(node, mt_table, "replica1")
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020), (2, 2020), (3, 2021)")
    node.query(f"SYSTEM SYNC REPLICA {mt_table}")

    # --- Case 1: Iceberg partitioned by 'id' but MergeTree by 'year' ---
    iceberg_col_mismatch = f"iceberg_col_mismatch_{uid}"
    node.query(
        f"""
        CREATE TABLE {iceberg_col_mismatch}
        (id Int64, year Int32)
        ENGINE = IcebergS3(
            'http://minio1:9001/root/data/{iceberg_col_mismatch}/',
            'minio',
            'ClickHouse_Minio_P@ssw0rd'
        )
        PARTITION BY id SETTINGS s3_retry_attempts = 1
        """
    )
    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {iceberg_col_mismatch}"
    )
    assert "BAD_ARGUMENTS" in error, (
        f"Expected BAD_ARGUMENTS for partition column mismatch, got: {error!r}"
    )

    # --- Case 2: Iceberg unpartitioned but MergeTree PARTITION BY year ---
    iceberg_count_mismatch = f"iceberg_count_mismatch_{uid}"
    node.query(
        f"""
        CREATE TABLE {iceberg_count_mismatch}
        (id Int64, year Int32)
        ENGINE = IcebergS3(
            'http://minio1:9001/root/data/{iceberg_count_mismatch}/',
            'minio',
            'ClickHouse_Minio_P@ssw0rd'
        )
        SETTINGS s3_retry_attempts = 1
        """
    )
    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {iceberg_count_mismatch}"
    )
    assert "BAD_ARGUMENTS" in error, (
        f"Expected BAD_ARGUMENTS for partition count mismatch, got: {error!r}"
    )

    # --- Case 3: Matching partition keys (both PARTITION BY year) ---
    iceberg_match = f"iceberg_match_{uid}"
    node.query(
        f"""
        CREATE TABLE {iceberg_match}
        (id Int64, year Int32)
        ENGINE = IcebergS3(
            'http://minio1:9001/root/data/{iceberg_match}/',
            'minio',
            'ClickHouse_Minio_P@ssw0rd'
        )
        PARTITION BY year SETTINGS s3_retry_attempts = 1
        """
    )
    # Should not raise — the check passes so the export is accepted synchronously
    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {iceberg_match}"
    )


def test_export_ttl(cluster):
    """
    After a manifest TTL expires the same partition can be re-exported, and the
    new data is appended to (or replaces) what is in the Iceberg table.
    """
    node = cluster.instances["replica1"]
    ttl_seconds = 3

    uid = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"mt_{uid}"
    iceberg_table = f"iceberg_{uid}"

    setup_tables(cluster, mt_table, iceberg_table, nodes=["replica1"])

    # First export.
    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {iceberg_table} "
        f"SETTINGS export_merge_tree_partition_manifest_ttl = {ttl_seconds}"
    )

    # A second export before the TTL expires must be rejected.
    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {iceberg_table}"
    )
    assert "Export with key" in error, f"Expected duplicate-export error before TTL, got: {error}"

    wait_for_export_status(node, mt_table, iceberg_table, "2020", "COMPLETED")

    count_after_first = int(node.query(f"SELECT count() FROM {iceberg_table} WHERE year = 2020").strip())
    assert count_after_first == 3, f"Expected 3 rows after first export, got {count_after_first}"

    # Wait for the manifest TTL to expire.
    time.sleep(ttl_seconds * 2)

    # Second export must be accepted now.
    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {iceberg_table}"
    )
    wait_for_export_status(node, mt_table, iceberg_table, "2020", "COMPLETED")


def test_export_data_files_are_not_cleaned_up_on_commit_failure(cluster):
    """
    Verify that the data files are not cleaned up on commit failure and the export is retried.
    This is to avoid data loss.

    If the data files were cleaned up, a retry would commit a new snapshot that points to dangling references.
    """
    node = cluster.instances["replica1"]
    uid = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"mt_{uid}"
    iceberg_table = f"iceberg_{uid}"
    setup_tables(cluster, mt_table, iceberg_table, nodes=["replica1"])

    node.query("SYSTEM ENABLE FAILPOINT iceberg_writes_non_retry_cleanup")

    node.query(f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {iceberg_table}")
    wait_for_export_status(node, mt_table, iceberg_table, "2020", "COMPLETED")

    count = int(node.query(f"SELECT count() FROM {iceberg_table} WHERE year = 2020").strip())
    assert count == 3, f"Expected 3 rows after first export, got {count}"
