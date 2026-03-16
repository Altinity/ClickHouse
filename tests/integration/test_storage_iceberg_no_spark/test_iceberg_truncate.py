#!/usr/bin/env python3

from pyiceberg.catalog import load_catalog
from helpers.config_cluster import minio_secret_key, minio_access_key
import uuid
import pyarrow as pa
from pyiceberg.schema import Schema, NestedField
from pyiceberg.types import LongType, StringType
from pyiceberg.partitioning import PartitionSpec

BASE_URL_LOCAL_RAW = "http://localhost:8182"
CATALOG_NAME = "demo"

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

def test_iceberg_truncate(started_cluster_iceberg_no_spark):
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    catalog = load_catalog_impl(started_cluster_iceberg_no_spark)

    namespace = f"clickhouse_truncate_{uuid.uuid4().hex}"
    catalog.create_namespace(namespace)

    schema = Schema(
        NestedField(field_id=1, name="id", field_type=LongType(), required=False),
        NestedField(field_id=2, name="val", field_type=StringType(), required=False),
    )

    table_name = "test_truncate"
    
    table = catalog.create_table(
        identifier=f"{namespace}.{table_name}",
        schema=schema,
        location=f"s3://warehouse-rest/{namespace}.{table_name}",
        partition_spec=PartitionSpec(),
    )

    df = pa.Table.from_pylist([
        {"id": 1, "val": "A"},
        {"id": 2, "val": "B"},
        {"id": 3, "val": "C"},
    ])
    table.append(df)

    # Validate data is in iceberg
    assert len(table.scan().to_arrow()) == 3

    # Setup ClickHouse Database
    instance.query(
        f"""
        DROP DATABASE IF EXISTS {namespace};
        SET allow_database_iceberg=true;
        CREATE DATABASE {namespace} ENGINE = DataLakeCatalog('http://rest:8181/v1', 'minio', '{minio_secret_key}')
        SETTINGS catalog_type='rest', warehouse='demo', storage_endpoint='http://minio:9000/warehouse-rest';
        """
    )
    
    # Assert data from ClickHouse
    assert int(instance.query(f"SELECT count() FROM {namespace}.{table_name}").strip()) == 3

    # Truncate Table via ClickHouse
    instance.query(f"SET allow_experimental_insert_into_iceberg=1; TRUNCATE TABLE {namespace}.{table_name};")

    # Assert truncated from ClickHouse
    assert int(instance.query(f"SELECT count() FROM {namespace}.{table_name}").strip()) == 0

    # Cross-Engine Validation using PyIceberg
    # Refresh table state
    table.refresh()

    # Assert PyIceberg reads the empty snapshot successfully
    assert len(table.scan().to_arrow()) == 0

    # Cleanup
    instance.query(f"DROP DATABASE {namespace}")
