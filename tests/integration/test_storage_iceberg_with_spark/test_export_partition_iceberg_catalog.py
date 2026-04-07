"""
Tests for EXPORT PARTITION to a catalog-backed Iceberg table (Glue catalog via Moto).

These tests verify that the catalog commit path (catalog->updateMetadata) is
exercised correctly for EXPORT PARTITION. A dedicated module-level cluster fixture
combines ZooKeeper (for ReplicatedMergeTree) with the Glue docker-compose stack
(Moto mock + MinIO warehouse bucket).

Test coverage:
    test_catalog_basic_export      — single partition exported; catalog shows new snapshot
    test_catalog_concurrent_export — two partitions exported in parallel; both commits succeed
    test_catalog_idempotent_retry  — crash after catalog commit; restart; no data duplication
"""

import logging
import os
import threading
import time
import uuid

import pytest
from pyiceberg.catalog import load_catalog
from pyiceberg.partitioning import PartitionField, PartitionSpec
from pyiceberg.schema import Schema
from pyiceberg.transforms import IdentityTransform
from pyiceberg.types import LongType, NestedField, StringType

from helpers.cluster import ClickHouseCluster
from helpers.config_cluster import minio_access_key, minio_secret_key


GLUE_BASE_URL = "http://glue:3000"
GLUE_BASE_URL_LOCAL = "http://localhost:3000"
CH_CATALOG_DB = "glue_export_catalog"


# ---------------------------------------------------------------------------
# Cluster fixture
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def catalog_export_cluster():
    """
    Cluster with ZooKeeper (for ReplicatedMergeTree / EXPORT PARTITION) and the
    Glue docker-compose stack (Moto mock + MinIO warehouse bucket).
    Spark is not needed; pyiceberg handles table creation and catalog inspection.
    """
    try:
        os.environ["AWS_ACCESS_KEY_ID"] = "testing"
        os.environ["AWS_SECRET_ACCESS_KEY"] = "testing"
        cluster = ClickHouseCluster(__file__)
        cluster.add_instance(
            "node1",
            main_configs=[
                "configs/config.d/allow_export_partition.xml",
            ],
            stay_alive=True,
            with_zookeeper=True,
            keeper_required_feature_flags=["multi_read"],
            with_glue_catalog=True,
        )
        cluster.start()

        time.sleep(15)

        yield cluster
    finally:
        cluster.shutdown()


@pytest.fixture(autouse=True)
def cleanup_tables(catalog_export_cluster):
    """Drop all tables in the default database after each test."""
    yield
    node = catalog_export_cluster.instances["node1"]
    try:
        tables = node.query(
            "SELECT name FROM system.tables WHERE database = 'default' FORMAT TabSeparated"
        ).strip()
        for tbl in tables.splitlines():
            tbl = tbl.strip()
            if tbl:
                node.query(f"DROP TABLE IF EXISTS default.`{tbl}` SYNC")
    except Exception as exc:
        logging.warning("cleanup_tables: %s", exc)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _load_catalog(cluster):
    """
    Connect to the Moto Glue mock from the test host via localhost:3000.
    MinIO is accessed via the container IP for S3 operations.
    """
    minio_ip = cluster.get_instance_ip("minio")
    return load_catalog(
        "glue_test",
        **{
            "type": "glue",
            "glue.endpoint": GLUE_BASE_URL_LOCAL,
            "glue.region": "us-east-1",
            "s3.endpoint": f"http://{minio_ip}:9000",
            "s3.access-key-id": minio_access_key,
            "s3.secret-access-key": minio_secret_key,
        },
    )


def _setup_ch_catalog_db(node, db_name: str = CH_CATALOG_DB) -> None:
    """Drop-and-recreate the ClickHouse DataLakeCatalog database pointing at Glue (Moto)."""
    node.query(f"DROP DATABASE IF EXISTS {db_name}")
    node.query(
        f"""
        SET write_full_path_in_iceberg_metadata = 1;
        SET allow_database_glue_catalog = 1;
        CREATE DATABASE {db_name}
        ENGINE = DataLakeCatalog('{GLUE_BASE_URL}', '{minio_access_key}', '{minio_secret_key}')
        SETTINGS catalog_type = 'glue',
                 warehouse = 'test',
                 storage_endpoint = 'http://minio:9000/warehouse-glue',
                 region = 'us-east-1'
        """
    )


def _wait_for_export(node, source: str, pid: str, timeout: int = 120) -> None:
    """Poll system.replicated_partition_exports until the task reaches COMPLETED."""
    start = time.time()
    last_status = None
    while time.time() - start < timeout:
        status = node.query(
            f"SELECT status FROM system.replicated_partition_exports"
            f" WHERE source_table = '{source}' AND partition_id = '{pid}'"
        ).strip()
        last_status = status
        if status == "COMPLETED":
            return
        time.sleep(0.5)
    raise TimeoutError(
        f"Export {source}/{pid} did not reach COMPLETED within {timeout}s "
        f"(last: {last_status!r})"
    )


def _make_rmt(node, name: str) -> None:
    """Create an identity(region)-partitioned ReplicatedMergeTree source table."""
    node.query(
        f"""
        CREATE TABLE {name} (id Int64, region String)
        ENGINE = ReplicatedMergeTree('/clickhouse/tables/{name}', 'r1')
        PARTITION BY region
        ORDER BY id
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
        """
    )


def _partition_id_for(node, table: str, region: str) -> str:
    return node.query(
        f"SELECT DISTINCT partition_id FROM system.parts"
        f" WHERE table = '{table}' AND active AND partition = '{region}'"
        f" FORMAT TabSeparated"
    ).strip()


def _create_iceberg_table(catalog, ns: str, tbl: str) -> None:
    """
    Create a simple identity(region)-partitioned Iceberg table in the catalog.
    Using format-version 2 and uncompressed metadata for test simplicity.
    """
    catalog.create_table(
        identifier=f"{ns}.{tbl}",
        schema=Schema(
            NestedField(field_id=1, name="id", field_type=LongType(), required=True),
            NestedField(field_id=2, name="region", field_type=StringType(), required=True),
        ),
        location=f"s3://warehouse-glue/data/{tbl}",
        partition_spec=PartitionSpec(
            PartitionField(
                source_id=2,
                field_id=1000,
                transform=IdentityTransform(),
                name="region",
            )
        ),
        properties={
            "write.metadata.compression-codec": "none",
            "write.format.default": "parquet",
            "format-version": "2",
        },
    )


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_catalog_basic_export(catalog_export_cluster):
    """
    Create a catalog-registered Iceberg table via pyiceberg, export one partition
    from a ReplicatedMergeTree, and verify:
    - The catalog (Glue) shows a new snapshot after the export.
    - SELECT via the DataLakeCatalog database returns the correct row count.

    This test exercises the catalog commit path:
      IcebergMetadata::commitImportPartitionTransactionImpl
        → catalog->updateMetadata(namespace, table, new_metadata_file, snapshot)
    """
    node = catalog_export_cluster.instances["node1"]
    catalog = _load_catalog(catalog_export_cluster)

    ns = f"ns_{uuid.uuid4().hex[:8]}"
    tbl = f"tbl_{uuid.uuid4().hex[:8]}"
    source = f"rmt_{uuid.uuid4().hex[:8]}"

    catalog.create_namespace((ns,))
    _create_iceberg_table(catalog, ns, tbl)
    _setup_ch_catalog_db(node)
    _make_rmt(node, source)

    node.query(f"INSERT INTO {source} VALUES (1, 'EU'), (2, 'EU'), (3, 'EU')")

    pid = _partition_id_for(node, source, "EU")
    dest_ch = f"`{CH_CATALOG_DB}`.`{ns}.{tbl}`"

    node.query(
        f"ALTER TABLE {source} EXPORT PARTITION ID '{pid}' TO TABLE {dest_ch}",
        settings={"write_full_path_in_iceberg_metadata": 1},
    )
    _wait_for_export(node, source, pid)

    count = int(node.query(f"SELECT count() FROM {dest_ch}").strip())
    assert count == 3, f"Expected 3 rows, got {count}"

    iceberg_tbl = catalog.load_table(f"{ns}.{tbl}")
    assert iceberg_tbl.current_snapshot() is not None, \
        "Expected at least one snapshot in Glue after the export"


def test_catalog_concurrent_export(catalog_export_cluster):
    """
    Export two partitions concurrently to the same catalog-backed Iceberg table.

    Both commits go through catalog->updateMetadata (Glue). Both commits must
    ultimately succeed.

    Verifies:
    - Total row count equals total inserted (no rows lost).
    - The catalog history contains at least two snapshots (one per partition).
    """
    node = catalog_export_cluster.instances["node1"]
    catalog = _load_catalog(catalog_export_cluster)

    ns = f"ns_{uuid.uuid4().hex[:8]}"
    tbl = f"tbl_{uuid.uuid4().hex[:8]}"
    source = f"rmt_{uuid.uuid4().hex[:8]}"

    catalog.create_namespace((ns,))
    _create_iceberg_table(catalog, ns, tbl)
    _setup_ch_catalog_db(node)
    _make_rmt(node, source)

    node.query(f"INSERT INTO {source} VALUES (1, 'EU'), (2, 'EU'), (3, 'EU')")
    node.query(f"INSERT INTO {source} VALUES (4, 'US'), (5, 'US'), (6, 'US')")

    pid_eu = _partition_id_for(node, source, "EU")
    pid_us = _partition_id_for(node, source, "US")
    dest_ch = f"`{CH_CATALOG_DB}`.`{ns}.{tbl}`"

    errors: list = []

    def export_partition(pid: str) -> None:
        try:
            node.query(
                f"ALTER TABLE {source} EXPORT PARTITION ID '{pid}' TO TABLE {dest_ch}",
                settings={"write_full_path_in_iceberg_metadata": 1},
            )
            _wait_for_export(node, source, pid, timeout=120)
        except Exception as exc:
            errors.append(exc)

    t1 = threading.Thread(target=export_partition, args=(pid_eu,))
    t2 = threading.Thread(target=export_partition, args=(pid_us,))
    t1.start()
    t2.start()
    t1.join()
    t2.join()

    assert not errors, f"Export threads raised errors: {errors}"

    count = int(node.query(f"SELECT count() FROM {dest_ch}").strip())
    assert count == 6, f"Expected 6 rows (3 EU + 3 US), got {count}"

    iceberg_tbl = catalog.load_table(f"{ns}.{tbl}")
    history = iceberg_tbl.history()
    assert len(history) >= 2, (
        f"Expected ≥2 snapshots (one per concurrent partition commit), got {len(history)}"
    )


def test_catalog_idempotent_retry(catalog_export_cluster):
    """
    Simulate a crash after the catalog commit but before ZooKeeper is updated to
    COMPLETED (via the iceberg_export_after_commit_before_zk_completed failpoint).

    After restart the scheduler retries the PENDING task.
    IcebergMetadata::commitExportPartitionTransaction finds the transaction_id already
    embedded in a snapshot summary field (clickhouse.export-partition-transaction-id)
    and returns without re-committing.

    Verifies:
    - Exactly 3 rows in the Iceberg table (no duplicates from the re-commit).
    - Exactly 1 snapshot in the Glue catalog (the idempotent retry was a no-op).
    """
    node = catalog_export_cluster.instances["node1"]
    catalog = _load_catalog(catalog_export_cluster)

    ns = f"ns_{uuid.uuid4().hex[:8]}"
    tbl = f"tbl_{uuid.uuid4().hex[:8]}"
    source = f"rmt_{uuid.uuid4().hex[:8]}"

    catalog.create_namespace((ns,))
    _create_iceberg_table(catalog, ns, tbl)
    _setup_ch_catalog_db(node)
    _make_rmt(node, source)

    node.query(f"INSERT INTO {source} VALUES (1, 'EU'), (2, 'EU'), (3, 'EU')")

    pid = _partition_id_for(node, source, "EU")
    dest_ch = f"`{CH_CATALOG_DB}`.`{ns}.{tbl}`"

    # Enable the ONCE failpoint: after a successful catalog commit the process
    # calls std::terminate() before writing ZK COMPLETED — simulating a hard crash.
    node.query("SYSTEM ENABLE FAILPOINT iceberg_export_after_commit_before_zk_completed")
    node.query(
        f"ALTER TABLE {source} EXPORT PARTITION ID '{pid}' TO TABLE {dest_ch}",
        settings={"write_full_path_in_iceberg_metadata": 1},
    )

    # Give the background scheduler time to export the data files and reach the
    # failpoint.  The crash is immediate (std::terminate), so 10 s is generous.
    time.sleep(10)
    node.restart_clickhouse()

    # ClickHouse persists database metadata to disk so the DataLakeCatalog database
    # survives the crash.  Recreate it anyway to make the test self-contained.
    _setup_ch_catalog_db(node)

    # The scheduler picks up the PENDING task and retries. commitExportPartitionTransaction
    # detects the transaction_id in the existing snapshot summary and skips the
    # re-commit, then marks the task COMPLETED in ZooKeeper.
    _wait_for_export(node, source, pid, timeout=120)

    count = int(node.query(f"SELECT count() FROM {dest_ch}").strip())
    assert count == 3, f"Expected 3 rows (no duplicates from idempotent retry), got {count}"

    iceberg_tbl = catalog.load_table(f"{ns}.{tbl}")
    history = iceberg_tbl.history()
    assert len(history) == 1, (
        f"Expected exactly 1 snapshot (idempotent re-commit was a no-op), "
        f"got {len(history)}"
    )
