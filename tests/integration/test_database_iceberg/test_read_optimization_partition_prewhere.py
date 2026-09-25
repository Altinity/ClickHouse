import logging
import time
import uuid
from datetime import date

import pyarrow as pa
import pytest
from pyiceberg.catalog import load_catalog
from pyiceberg.partitioning import PartitionField, PartitionSpec
from pyiceberg.schema import Schema
from pyiceberg.transforms import BucketTransform, IdentityTransform
from pyiceberg.types import DateType, LongType, NestedField

from helpers.cluster import ClickHouseCluster
from helpers.config_cluster import minio_access_key, minio_secret_key

ICEBERG_PORT = 8184

BASE_URL = "http://rest:8181/v1"
BASE_URL_LOCAL_RAW = f"http://localhost:{ICEBERG_PORT}"

CATALOG_NAME = "demo"

SCHEMA = Schema(
    NestedField(field_id=1, name="id", field_type=LongType(), required=False),
    NestedField(field_id=2, name="date", field_type=DateType(), required=False),
    NestedField(field_id=3, name="value", field_type=LongType(), required=False),
)

IDENTITY_ON_DATE = PartitionSpec(
    PartitionField(source_id=2, field_id=1000, transform=IdentityTransform(), name="date")
)


@pytest.fixture(scope="module")
def started_cluster():
    try:
        cluster = ClickHouseCluster(__file__)
        cluster.iceberg_rest_external_port = ICEBERG_PORT
        cluster.add_instance(
            "node1",
            main_configs=["configs/cluster.xml"],
            user_configs=[],
            stay_alive=True,
            with_iceberg_catalog=True,
        )

        logging.info("Starting cluster...")
        cluster.start()

        time.sleep(10)

        yield cluster

    finally:
        cluster.shutdown()


def load_catalog_impl(started_cluster):
    return load_catalog(
        CATALOG_NAME,
        **{
            "uri": BASE_URL_LOCAL_RAW,
            "type": "rest",
            "s3.endpoint": f"http://{started_cluster.get_instance_ip('minio')}:9000",
            "s3.access-key-id": minio_access_key,
            "s3.secret-access-key": minio_secret_key,
        },
    )


def create_table(catalog, namespace, table, schema, partition_spec):
    return catalog.create_table(
        identifier=f"{namespace}.{table}",
        schema=schema,
        location="s3://warehouse-rest/data",
        partition_spec=partition_spec,
    )


def create_clickhouse_iceberg_database(node, name):
    settings = {
        "catalog_type": "rest",
        "warehouse": "demo",
        "storage_endpoint": "http://minio:9000/warehouse-rest",
    }
    node.query(
        f"""
DROP DATABASE IF EXISTS {name};
SET allow_database_iceberg=true;
SET write_full_path_in_iceberg_metadata=1;
CREATE DATABASE {name} ENGINE = DataLakeCatalog('{BASE_URL}', 'minio', '{minio_secret_key}')
SETTINGS {",".join((k + "=" + repr(v) for k, v in settings.items()))}
"""
    )


def append_row(table, id_, date_, value_):
    append_rows(table, [(id_, date_, value_)])


def append_rows(table, rows):
    """Writes all `rows` in a single `.append()` call, i.e. into the same
    physical data file when they share a partition value."""
    table.append(
        pa.Table.from_pylist(
            [{"id": id_, "date": date_, "value": value_} for id_, date_, value_ in rows],
            schema=table.schema().as_arrow(),
        )
    )


def _run_and_get_profile_events(instance, select_expression, settings, event_names):
    query_id = f"read-opt-{uuid.uuid4()}"
    result = instance.query(select_expression, query_id=query_id, settings=settings)
    instance.query("SYSTEM FLUSH LOGS")
    columns = ", ".join(f"ProfileEvents['{name}']" for name in event_names)
    events_row = instance.query(
        f"""
        SELECT {columns}
        FROM system.query_log
        WHERE query_id = '{query_id}' AND type = 'QueryFinish'
        """
    ).strip()
    events = tuple(int(x) for x in events_row.split("\t"))
    return result.strip(), events


def test_iceberg_read_optimization_count_with_partition_filter(started_cluster):
    node = started_cluster.instances["node1"]
    catalog = load_catalog_impl(started_cluster)

    namespace = f"read_opt_count_ns_{uuid.uuid4().hex}"
    table_name = "t"
    catalog.create_namespace(namespace)
    table = create_table(catalog, namespace, table_name, SCHEMA, IDENTITY_ON_DATE)

    rows = [
        (1, date(2024, 1, 10), 100),
        (2, date(2024, 1, 20), 200),
        (3, date(2024, 2, 5), 300),
        (4, date(2024, 2, 15), 400),
        (5, date(2024, 3, 1), 500),
    ]
    for id_, date_, value_ in rows:
        append_row(table, id_, date_, value_)

    create_clickhouse_iceberg_database(node, CATALOG_NAME)
    full_name = f"{CATALOG_NAME}.`{namespace}.{table_name}`"

    select_expression = f"SELECT count() FROM {full_name} WHERE date > '2024-01-15'"
    events = ("ObjectStorageReadObjects", "ParquetReadRowGroups", "IcebergPartitionPrunedFiles")

    baseline_result, (baseline_reads, baseline_row_groups, baseline_pruned) = _run_and_get_profile_events(
        node, select_expression, {"allow_experimental_iceberg_read_optimization": 0}, events
    )
    optimized_result, (optimized_reads, optimized_row_groups, optimized_pruned) = _run_and_get_profile_events(
        node, select_expression, {"allow_experimental_iceberg_read_optimization": 1}, events
    )

    assert baseline_result == optimized_result == "4"

    assert baseline_pruned == optimized_pruned == 1

    assert baseline_reads > 0
    assert baseline_row_groups > 0
    assert optimized_reads == 0
    assert optimized_row_groups == 0


def test_iceberg_read_optimization_partition_filter_excludes_all_files(started_cluster):
    node = started_cluster.instances["node1"]
    catalog = load_catalog_impl(started_cluster)

    namespace = f"read_opt_excl_ns_{uuid.uuid4().hex}"
    table_name = "t"
    catalog.create_namespace(namespace)
    table = create_table(catalog, namespace, table_name, SCHEMA, IDENTITY_ON_DATE)

    rows = [
        (1, date(2024, 1, 10), 100),
        (2, date(2024, 1, 20), 200),
        (3, date(2024, 2, 5), 300),
    ]
    for id_, date_, value_ in rows:
        append_row(table, id_, date_, value_)

    create_clickhouse_iceberg_database(node, CATALOG_NAME)
    full_name = f"{CATALOG_NAME}.`{namespace}.{table_name}`"

    select_expression = f"SELECT count() FROM {full_name} WHERE date > '2099-01-01'"
    events = ("ObjectStorageReadObjects", "IcebergPartitionPrunedFiles")

    result, (reads, pruned) = _run_and_get_profile_events(
        node, select_expression, {"allow_experimental_iceberg_read_optimization": 1}, events
    )

    assert result == "0"
    assert pruned == 3
    assert reads == 0


def test_iceberg_read_optimization_real_column_query_still_reads_data(started_cluster):
    node = started_cluster.instances["node1"]
    catalog = load_catalog_impl(started_cluster)

    namespace = f"read_opt_realcol_ns_{uuid.uuid4().hex}"
    table_name = "t"
    catalog.create_namespace(namespace)
    table = create_table(catalog, namespace, table_name, SCHEMA, IDENTITY_ON_DATE)

    append_row(table, 1, date(2024, 1, 10), 100)
    append_rows(table, [(2, date(2024, 1, 20), 200), (3, date(2024, 1, 20), 250)])
    append_row(table, 4, date(2024, 2, 15), 400)

    create_clickhouse_iceberg_database(node, CATALOG_NAME)
    full_name = f"{CATALOG_NAME}.`{namespace}.{table_name}`"

    select_expression = f"SELECT count(), sum(value) FROM {full_name} WHERE date > '2024-01-15'"
    events = ("ObjectStorageReadObjects",)

    baseline_result, (baseline_reads,) = _run_and_get_profile_events(
        node, select_expression, {"allow_experimental_iceberg_read_optimization": 0}, events
    )
    optimized_result, (optimized_reads,) = _run_and_get_profile_events(
        node, select_expression, {"allow_experimental_iceberg_read_optimization": 1}, events
    )

    assert baseline_result == optimized_result == "3\t850"
    assert baseline_reads > 0
    assert optimized_reads > 0


def test_iceberg_read_optimization_non_identity_transform_keeps_prewhere_eligible(started_cluster):
    node = started_cluster.instances["node1"]
    catalog = load_catalog_impl(started_cluster)

    namespace = f"read_opt_nonident_ns_{uuid.uuid4().hex}"
    table_name = "t"
    catalog.create_namespace(namespace)

    bucket_on_id = PartitionSpec(
        PartitionField(source_id=1, field_id=1000, transform=BucketTransform(4), name="id_bucket")
    )
    table = create_table(catalog, namespace, table_name, SCHEMA, bucket_on_id)

    for id_ in range(1, 9):
        append_row(table, id_, date(2024, 1, 1), id_ * 100)

    create_clickhouse_iceberg_database(node, CATALOG_NAME)
    full_name = f"{CATALOG_NAME}.`{namespace}.{table_name}`"

    select_expression = f"SELECT id, value FROM {full_name} WHERE id = 5 ORDER BY ALL"

    result_baseline = node.query(
        select_expression, settings={"allow_experimental_iceberg_read_optimization": 0}
    ).strip()
    result_optimized = node.query(
        select_expression, settings={"allow_experimental_iceberg_read_optimization": 1}
    ).strip()

    assert result_baseline == result_optimized == "5\t500"
