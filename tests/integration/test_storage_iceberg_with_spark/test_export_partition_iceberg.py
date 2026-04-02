"""
Tests for EXPORT PARTITION to an Iceberg table that was created by Apache Spark.

The destination Iceberg metadata — including field IDs and the partition spec —
is written by Spark, not by ClickHouse, which removes any bias from tests where
both source and destination are ClickHouse-created.

A separate module-level fixture is used because the package-level
started_cluster_iceberg_with_spark does not include ZooKeeper (which is
required for ReplicatedMergeTree / EXPORT PARTITION).

Transform coverage (ClickHouse → Iceberg):
    identity             → identity
    toYearNumSinceEpoch  → year
    toMonthNumSinceEpoch → month
    toRelativeDayNum     → day
    toRelativeHourNum    → hour
    icebergBucket(N)     → bucket(N)
    icebergTruncate(N)   → truncate(N)
    compound             → multiple fields
"""

import logging
import time
import uuid

import pytest
import pyspark

from helpers.cluster import ClickHouseCluster
from helpers.iceberg_utils import (
    create_iceberg_table,
    default_upload_directory,
)
from helpers.s3_tools import S3Uploader, prepare_s3_bucket


# ---------------------------------------------------------------------------
# Spark session
# ---------------------------------------------------------------------------

def _get_spark():
    builder = (
        pyspark.sql.SparkSession.builder
        .appName("test_export_partition_spark_iceberg")
        .config(
            "spark.sql.catalog.spark_catalog",
            "org.apache.iceberg.spark.SparkSessionCatalog",
        )
        .config("spark.sql.catalog.local", "org.apache.iceberg.spark.SparkCatalog")
        .config("spark.sql.catalog.spark_catalog.type", "hadoop")
        .config(
            "spark.sql.catalog.spark_catalog.warehouse",
            "/var/lib/clickhouse/user_files/iceberg_data",
        )
        .config(
            "spark.sql.extensions",
            "org.apache.iceberg.spark.extensions.IcebergSparkSessionExtensions",
        )
        .master("local")
    )
    return builder.getOrCreate()


# ---------------------------------------------------------------------------
# Cluster fixture
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def export_cluster():
    try:
        cluster = ClickHouseCluster(__file__, with_spark=True)
        cluster.add_instance(
            "node1",
            main_configs=[
                "configs/config.d/named_collections.xml",
                "configs/config.d/allow_export_partition.xml",
            ],
            with_minio=True,
            stay_alive=True,
            with_zookeeper=True,
            keeper_required_feature_flags=["multi_read"],
        )
        logging.info("Starting export_cluster...")
        cluster.start()
        prepare_s3_bucket(cluster)
        cluster.spark_session = _get_spark()
        cluster.default_s3_uploader = S3Uploader(cluster.minio_client, cluster.minio_bucket)
        yield cluster
    finally:
        cluster.shutdown()


@pytest.fixture(autouse=True)
def drop_tables(export_cluster):
    yield
    node = export_cluster.instances["node1"]
    try:
        tables = node.query(
            "SELECT name FROM system.tables WHERE database = 'default' FORMAT TabSeparated"
        ).strip()
        for table in tables.splitlines():
            table = table.strip()
            if table:
                node.query(f"DROP TABLE IF EXISTS default.`{table}` SYNC")
    except Exception as e:
        logging.warning(f"drop_tables cleanup failed: {e}")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _spark_iceberg(cluster, spark, iceberg_name: str, ddl: str):
    """Execute a Spark DDL and upload the resulting Iceberg files to MinIO."""
    spark.sql(ddl)
    default_upload_directory(
        cluster,
        "s3",
        f"/iceberg_data/default/{iceberg_name}/",
        f"/iceberg_data/default/{iceberg_name}/",
    )


def _attach_ch_iceberg(node, iceberg_name: str, schema: str, cluster):
    """
    Attach a ClickHouse IcebergS3 table to an existing Spark-written Iceberg path.
    No PARTITION BY is specified — the spec is read from Spark's metadata.
    """
    create_iceberg_table(
        "s3",
        node,
        iceberg_name,
        cluster,
        schema=f"({schema})",
        if_not_exists=True,
    )


def _make_rmt(node, name: str, columns: str, partition_by: str):
    node.query(
        f"""
        CREATE TABLE {name} ({columns})
        ENGINE = ReplicatedMergeTree('/clickhouse/tables/{name}', 'r1')
        PARTITION BY {partition_by}
        ORDER BY id
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
        """
    )


def _first_partition_id(node, table: str) -> str:
    return node.query(
        f"SELECT partition_id FROM system.parts"
        f" WHERE table = '{table}' AND database = currentDatabase() AND active"
        f" LIMIT 1"
    ).strip()


def _wait_for_export(node, source: str, dest: str, pid: str, timeout: int = 60):
    start = time.time()
    last_status = None
    while time.time() - start < timeout:
        status = node.query(
            f"SELECT status FROM system.replicated_partition_exports"
            f" WHERE source_table = '{source}'"
            f"   AND destination_table = '{dest}'"
            f"   AND partition_id = '{pid}'"
        ).strip()
        last_status = status
        if status == "COMPLETED":
            return
        time.sleep(0.5)
    raise TimeoutError(
        f"Export did not reach COMPLETED within {timeout}s (last: {last_status!r})"
    )


def _run_accepted(export_cluster, spark_ddl, ch_schema, rmt_columns, rmt_partition_by, insert_values):
    """
    Create a Spark-created Iceberg table, attach ClickHouse to it, create the
    source RMT, export, wait, and return (node, source, iceberg, partition_id)
    so the caller can do additional assertions.
    """
    node = export_cluster.instances["node1"]
    spark = export_cluster.spark_session

    uid = str(uuid.uuid4()).replace("-", "_")
    source = f"rmt_{uid}"
    iceberg = f"spark_{uid}"

    _spark_iceberg(export_cluster, spark, iceberg, spark_ddl.format(TABLE=iceberg))
    _attach_ch_iceberg(node, iceberg, ch_schema, export_cluster)
    _make_rmt(node, source, rmt_columns, rmt_partition_by)
    node.query(f"INSERT INTO {source} VALUES {insert_values}")

    pid = _first_partition_id(node, source)
    node.query(f"ALTER TABLE {source} EXPORT PARTITION ID '{pid}' TO TABLE {iceberg}")
    _wait_for_export(node, source, iceberg, pid)

    return node, source, iceberg, pid


def _run_rejected(export_cluster, spark_ddl, ch_schema, rmt_columns, rmt_partition_by, insert_values):
    """
    Create a mismatched pair and assert that EXPORT PARTITION fails with BAD_ARGUMENTS.
    The check fires synchronously before any task is enqueued.
    """
    node = export_cluster.instances["node1"]
    spark = export_cluster.spark_session

    uid = str(uuid.uuid4()).replace("-", "_")
    source = f"rmt_{uid}"
    iceberg = f"spark_{uid}"

    _spark_iceberg(export_cluster, spark, iceberg, spark_ddl.format(TABLE=iceberg))
    _attach_ch_iceberg(node, iceberg, ch_schema, export_cluster)
    _make_rmt(node, source, rmt_columns, rmt_partition_by)
    node.query(f"INSERT INTO {source} VALUES {insert_values}")

    pid = _first_partition_id(node, source)
    error = node.query_and_get_error(
        f"ALTER TABLE {source} EXPORT PARTITION ID '{pid}' TO TABLE {iceberg}"
    )
    return error


# ---------------------------------------------------------------------------
# Happy-path tests — one per transform
# ---------------------------------------------------------------------------

def test_identity_transform(export_cluster):
    """Spark identity(year)  <->  PARTITION BY year."""
    node, _, iceberg, _ = _run_accepted(
        export_cluster,
        spark_ddl="CREATE TABLE {TABLE} (id BIGINT, year INT)"
                  " USING iceberg PARTITIONED BY (identity(year)) OPTIONS('format-version'='2')",
        ch_schema="id Int64, year Int32",
        rmt_columns="id Int64, year Int32",
        rmt_partition_by="year",
        insert_values="(1, 2024), (2, 2024), (3, 2024)",
    )
    assert int(node.query(f"SELECT count() FROM {iceberg}").strip()) == 3


def test_year_transform(export_cluster):
    """Spark years(dt)  <->  PARTITION BY toYearNumSinceEpoch(dt)."""
    node, _, iceberg, _ = _run_accepted(
        export_cluster,
        spark_ddl="CREATE TABLE {TABLE} (id BIGINT, dt DATE)"
                  " USING iceberg PARTITIONED BY (years(dt)) OPTIONS('format-version'='2')",
        ch_schema="id Int64, dt Date",
        rmt_columns="id Int64, dt Date",
        rmt_partition_by="toYearNumSinceEpoch(dt)",
        insert_values="(1, '2021-03-01'), (2, '2021-07-15'), (3, '2021-12-31')",
    )
    assert int(node.query(f"SELECT count() FROM {iceberg}").strip()) == 3


def test_month_transform(export_cluster):
    """Spark months(dt)  <->  PARTITION BY toMonthNumSinceEpoch(dt)."""
    node, _, iceberg, _ = _run_accepted(
        export_cluster,
        spark_ddl="CREATE TABLE {TABLE} (id BIGINT, dt DATE)"
                  " USING iceberg PARTITIONED BY (months(dt)) OPTIONS('format-version'='2')",
        ch_schema="id Int64, dt Date",
        rmt_columns="id Int64, dt Date",
        rmt_partition_by="toMonthNumSinceEpoch(dt)",
        insert_values="(1, '2020-06-01'), (2, '2020-06-15'), (3, '2020-06-30')",
    )
    assert int(node.query(f"SELECT count() FROM {iceberg}").strip()) == 3


def test_day_transform(export_cluster):
    """Spark days(dt)  <->  PARTITION BY toRelativeDayNum(dt)."""
    node, _, iceberg, _ = _run_accepted(
        export_cluster,
        spark_ddl="CREATE TABLE {TABLE} (id BIGINT, dt DATE)"
                  " USING iceberg PARTITIONED BY (days(dt)) OPTIONS('format-version'='2')",
        ch_schema="id Int64, dt Date",
        rmt_columns="id Int64, dt Date",
        rmt_partition_by="toRelativeDayNum(dt)",
        insert_values="(1, '2023-03-15'), (2, '2023-03-15'), (3, '2023-03-15')",
    )
    assert int(node.query(f"SELECT count() FROM {iceberg}").strip()) == 3


def test_hour_transform(export_cluster):
    """Spark hours(ts)  <->  PARTITION BY toRelativeHourNum(ts).

    Spark TIMESTAMP maps to Iceberg 'timestamp' which ClickHouse reads as DateTime64(6).
    All three rows fall within the same hour so a single partition is exported.
    """
    node, _, iceberg, _ = _run_accepted(
        export_cluster,
        spark_ddl="CREATE TABLE {TABLE} (id BIGINT, ts TIMESTAMP)"
                  " USING iceberg PARTITIONED BY (hours(ts)) OPTIONS('format-version'='2')",
        ch_schema="id Int64, ts DateTime64(6)",
        rmt_columns="id Int64, ts DateTime64(6)",
        rmt_partition_by="toRelativeHourNum(ts)",
        insert_values=(
            "(1, '2023-03-15 10:00:00'), "
            "(2, '2023-03-15 10:30:00'), "
            "(3, '2023-03-15 10:59:00')"
        ),
    )
    assert int(node.query(f"SELECT count() FROM {iceberg}").strip()) == 3


def test_bucket_transform(export_cluster):
    """Spark bucket(8, user_id)  <->  PARTITION BY icebergBucket(8, user_id)."""
    node, _, iceberg, _ = _run_accepted(
        export_cluster,
        spark_ddl="CREATE TABLE {TABLE} (id BIGINT, user_id BIGINT)"
                  " USING iceberg PARTITIONED BY (bucket(8, user_id)) OPTIONS('format-version'='2')",
        ch_schema="id Int64, user_id Int64",
        rmt_columns="id Int64, user_id Int64",
        rmt_partition_by="icebergBucket(8, user_id)",
        # All rows share the same user_id → same bucket → single partition.
        insert_values="(1, 42), (2, 42), (3, 42)",
    )
    assert int(node.query(f"SELECT count() FROM {iceberg}").strip()) == 3


def test_truncate_transform(export_cluster):
    """Spark truncate(4, category)  <->  PARTITION BY icebergTruncate(4, category)."""
    node, _, iceberg, _ = _run_accepted(
        export_cluster,
        spark_ddl="CREATE TABLE {TABLE} (id BIGINT, category STRING)"
                  " USING iceberg PARTITIONED BY (truncate(4, category)) OPTIONS('format-version'='2')",
        ch_schema="id Int64, category String",
        rmt_columns="id Int64, category String",
        rmt_partition_by="icebergTruncate(4, category)",
        # All share the 4-char prefix 'clic' → same truncate bucket.
        insert_values="(1, 'clickhouse'), (2, 'click'), (3, 'clickstream')",
    )
    assert int(node.query(f"SELECT count() FROM {iceberg}").strip()) == 3


def test_compound_transform(export_cluster):
    """Spark (identity(year), identity(region))  <->  PARTITION BY (year, region)."""
    node, _, iceberg, _ = _run_accepted(
        export_cluster,
        spark_ddl="CREATE TABLE {TABLE} (id BIGINT, year INT, region STRING)"
                  " USING iceberg PARTITIONED BY (identity(year), identity(region))"
                  " OPTIONS('format-version'='2')",
        ch_schema="id Int64, year Int32, region String",
        rmt_columns="id Int64, year Int32, region String",
        rmt_partition_by="(year, region)",
        insert_values="(1, 2022, 'EU'), (2, 2022, 'EU'), (3, 2022, 'EU')",
    )
    assert int(node.query(f"SELECT count() FROM {iceberg}").strip()) == 3


# ---------------------------------------------------------------------------
# Unhappy-path tests — BAD_ARGUMENTS must be raised synchronously
# ---------------------------------------------------------------------------

def test_rejected_column_mismatch(export_cluster):
    """Spark identity(year) — RMT PARTITION BY id: different column."""
    error = _run_rejected(
        export_cluster,
        spark_ddl="CREATE TABLE {TABLE} (id BIGINT, year INT)"
                  " USING iceberg PARTITIONED BY (identity(year)) OPTIONS('format-version'='2')",
        ch_schema="id Int64, year Int32",
        rmt_columns="id Int64, year Int32",
        rmt_partition_by="id",
        insert_values="(1, 2024)",
    )
    assert "BAD_ARGUMENTS" in error, f"Expected BAD_ARGUMENTS, got: {error!r}"


def test_rejected_transform_mismatch(export_cluster):
    """Spark years(dt) — RMT PARTITION BY dt (identity, not year-transform)."""
    error = _run_rejected(
        export_cluster,
        spark_ddl="CREATE TABLE {TABLE} (id BIGINT, dt DATE)"
                  " USING iceberg PARTITIONED BY (years(dt)) OPTIONS('format-version'='2')",
        ch_schema="id Int64, dt Date",
        rmt_columns="id Int64, dt Date",
        rmt_partition_by="dt",
        insert_values="(1, '2021-06-01')",
    )
    assert "BAD_ARGUMENTS" in error, f"Expected BAD_ARGUMENTS, got: {error!r}"


def test_rejected_bucket_count_mismatch(export_cluster):
    """Spark bucket(8, user_id) — RMT icebergBucket(16, user_id): wrong N."""
    error = _run_rejected(
        export_cluster,
        spark_ddl="CREATE TABLE {TABLE} (id BIGINT, user_id BIGINT)"
                  " USING iceberg PARTITIONED BY (bucket(8, user_id)) OPTIONS('format-version'='2')",
        ch_schema="id Int64, user_id Int64",
        rmt_columns="id Int64, user_id Int64",
        rmt_partition_by="icebergBucket(16, user_id)",
        insert_values="(1, 42)",
    )
    assert "BAD_ARGUMENTS" in error, f"Expected BAD_ARGUMENTS, got: {error!r}"


def test_rejected_truncate_width_mismatch(export_cluster):
    """Spark truncate(4, category) — RMT icebergTruncate(8, category): wrong width."""
    error = _run_rejected(
        export_cluster,
        spark_ddl="CREATE TABLE {TABLE} (id BIGINT, category STRING)"
                  " USING iceberg PARTITIONED BY (truncate(4, category)) OPTIONS('format-version'='2')",
        ch_schema="id Int64, category String",
        rmt_columns="id Int64, category String",
        rmt_partition_by="icebergTruncate(8, category)",
        insert_values="(1, 'clickhouse')",
    )
    assert "BAD_ARGUMENTS" in error, f"Expected BAD_ARGUMENTS, got: {error!r}"


def test_rejected_field_count_mismatch(export_cluster):
    """Spark 1-field identity(year) — RMT 2-field (year, region)."""
    error = _run_rejected(
        export_cluster,
        spark_ddl="CREATE TABLE {TABLE} (id BIGINT, year INT, region STRING)"
                  " USING iceberg PARTITIONED BY (identity(year)) OPTIONS('format-version'='2')",
        ch_schema="id Int64, year Int32, region String",
        rmt_columns="id Int64, year Int32, region String",
        rmt_partition_by="(year, region)",
        insert_values="(1, 2024, 'EU')",
    )
    assert "BAD_ARGUMENTS" in error, f"Expected BAD_ARGUMENTS, got: {error!r}"


def test_rejected_compound_order_reversed(export_cluster):
    """Spark (identity(year), identity(region)) — RMT (region, year): reversed order."""
    error = _run_rejected(
        export_cluster,
        spark_ddl="CREATE TABLE {TABLE} (id BIGINT, year INT, region STRING)"
                  " USING iceberg PARTITIONED BY (identity(year), identity(region))"
                  " OPTIONS('format-version'='2')",
        ch_schema="id Int64, year Int32, region String",
        rmt_columns="id Int64, year Int32, region String",
        rmt_partition_by="(region, year)",
        insert_values="(1, 2024, 'EU')",
    )
    assert "BAD_ARGUMENTS" in error, f"Expected BAD_ARGUMENTS, got: {error!r}"


def test_idempotency_after_commit_crash(export_cluster):
    """
    Verify that an Iceberg export commit is idempotent when ClickHouse crashes (via
    std::terminate() in a failpoint) after the Iceberg metadata is written but before
    ZooKeeper is updated to COMPLETED.

    Expected behaviour:
    - The failpoint fires once: std::terminate() kills the process immediately after the
      Iceberg commit; ZK task remains PENDING.
    - ClickHouse is restarted.  The scheduler picks up the PENDING task and retries the
      commit.  commitExportPartitionTransaction finds the transaction_id already present in
      the Iceberg snapshot summary and skips re-committing.
    - The task eventually reaches COMPLETED.
    - The row count in the Iceberg table is exactly the number inserted (no duplicates).
    """
    node = export_cluster.instances["node1"]
    spark = export_cluster.spark_session

    uid = str(uuid.uuid4()).replace("-", "_")
    source = f"rmt_{uid}"
    iceberg = f"spark_{uid}"

    _spark_iceberg(
        export_cluster,
        spark,
        iceberg,
        f"CREATE TABLE {iceberg} (id BIGINT, year INT)"
        f" USING iceberg PARTITIONED BY (identity(year)) OPTIONS('format-version'='2')",
    )
    _attach_ch_iceberg(node, iceberg, "id Int64, year Int32", export_cluster)
    _make_rmt(node, source, "id Int64, year Int32", "year")
    node.query(f"INSERT INTO {source} VALUES (1, 2024), (2, 2024), (3, 2024)")

    pid = _first_partition_id(node, source)

    # Enable the ONCE failpoint. When the background scheduler thread reaches the
    # injection point (after a successful Iceberg commit), std::terminate() is called
    # and the process exits immediately without setting ZK COMPLETED.
    node.query("SYSTEM ENABLE FAILPOINT iceberg_export_after_commit_before_zk_completed")
    node.query(f"ALTER TABLE {source} EXPORT PARTITION ID '{pid}' TO TABLE {iceberg}")

    # the fail point will sleep for 10 seconds. Wait for 5 and then re-start clickhouse.
    time.sleep(5)

    # Restart ClickHouse. The ZK task is still PENDING; the scheduler will pick it up.
    node.restart_clickhouse()

    # On restart the scheduler retries the commit. commitExportPartitionTransaction
    # detects the transaction_id in the existing Iceberg snapshot summary and returns
    # without re-writing any data, then sets ZK COMPLETED.
    _wait_for_export(node, source, iceberg, pid, timeout=60)

    # Exactly 3 rows — no duplicates from the idempotent re-commit.
    count = int(node.query(f"SELECT count() FROM {iceberg}").strip())
    assert count == 3, f"Expected 3 rows (no duplicates), got {count}"
