#!/usr/bin/env python3

"""
Integration test for Iceberg v3 `unknown` primitive type.

Creates an Iceberg v3 table with an `unknown`-typed column via PyIceberg,
writes some rows, then reads the table from ClickHouse and asserts:
- The unknown column is mapped to Nullable(Nothing)
- All values in the unknown column are NULL
- Non-unknown columns read correctly
"""

from pyiceberg.catalog import load_catalog
from pyiceberg.schema import Schema
from pyiceberg.types import NestedField, LongType, StringType, UnknownType
from pyiceberg.partitioning import PartitionSpec
from pyiceberg.table.sorting import SortOrder
from helpers.config_cluster import minio_secret_key, minio_access_key
import pyarrow as pa
import uuid

BASE_URL = "http://rest:8181/v1"
CATALOG_NAME = "demo"


def load_catalog_impl(started_cluster):
    base_url_local_raw = f"http://localhost:{started_cluster.iceberg_rest_catalog_port}"
    return load_catalog(
        CATALOG_NAME,
        **{
            "uri": base_url_local_raw,
            "type": "rest",
            "s3.endpoint": f"http://{started_cluster.minio_ip}:{started_cluster.minio_port}",
            "s3.access-key-id": minio_access_key,
            "s3.secret-access-key": minio_secret_key,
        },
    )


def create_clickhouse_iceberg_database(started_cluster, node, name):
    settings = {
        "catalog_type": "rest",
        "warehouse": "demo",
        "storage_endpoint": "http://minio1:9001/warehouse-rest",
    }

    node.query(
        f"""
DROP DATABASE IF EXISTS {name};
SET allow_database_iceberg=true;
SET write_full_path_in_iceberg_metadata=1;
CREATE DATABASE {name} ENGINE = DataLakeCatalog('{BASE_URL}', 'minio', '{minio_secret_key}')
SETTINGS {",".join((k+"="+repr(v) for k, v in settings.items()))}
    """
    )


def test_unknown_type_read(started_cluster_iceberg_no_spark):
    """Create an Iceberg v3 table with an unknown-typed column and verify ClickHouse reads it."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    root_namespace = f"clickhouse_{uuid.uuid4()}"
    catalog = load_catalog_impl(started_cluster_iceberg_no_spark)

    schema = Schema(
        NestedField(field_id=1, name="id", field_type=LongType(), required=True),
        NestedField(field_id=2, name="name", field_type=StringType(), required=False),
        NestedField(
            field_id=3, name="placeholder", field_type=UnknownType(), required=False
        ),
    )

    table = catalog.create_table(
        identifier=f"{root_namespace}.test_unknown",
        schema=schema,
        location="s3://warehouse-rest/data",
        partition_spec=PartitionSpec(),
        sort_order=SortOrder(),
        properties={"format-version": "3"},
    )

    # Write data; the unknown column is all NULLs in the Arrow table
    data = pa.table(
        {
            "id": pa.array([1, 2, 3], type=pa.int64()),
            "name": pa.array(["alice", "bob", "charlie"], type=pa.large_string()),
            "placeholder": pa.nulls(3),
        }
    )
    table.append(data)

    create_clickhouse_iceberg_database(
        started_cluster_iceberg_no_spark, instance, CATALOG_NAME
    )

    fqtn = f"{CATALOG_NAME}.`{root_namespace}.test_unknown`"

    # Verify row count
    assert instance.query(f"SELECT count() FROM {fqtn}").strip() == "3"

    # Verify the unknown column type is Nullable(Nothing)
    describe = instance.query(f"DESCRIBE TABLE {fqtn}")
    lines = [line.split("\t") for line in describe.strip().split("\n")]
    placeholder_row = [row for row in lines if row[0] == "placeholder"]
    assert len(placeholder_row) == 1, f"Expected one 'placeholder' column, got: {lines}"
    assert placeholder_row[0][1] == "Nullable(Nothing)"

    # Verify all values in the unknown column are NULL
    result = instance.query(f"SELECT placeholder FROM {fqtn}").strip()
    assert result == "\\N\n\\N\n\\N"

    # Verify non-unknown columns read correctly alongside the unknown column
    result = instance.query(
        f"SELECT id, name, placeholder FROM {fqtn} ORDER BY id"
    ).strip()
    expected = "1\talice\t\\N\n2\tbob\t\\N\n3\tcharlie\t\\N"
    assert result == expected
