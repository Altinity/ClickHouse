import glob
import json
import logging
import os
import random
import time
import uuid
from datetime import datetime, timedelta, time as dtime

import pyarrow as pa
import pytest
import requests
import urllib3
import pytz
from minio import Minio
from pyiceberg.catalog import load_catalog
from pyiceberg.partitioning import PartitionField, PartitionSpec, UNPARTITIONED_PARTITION_SPEC
from pyiceberg.schema import Schema
from pyiceberg.table.sorting import SortField, SortOrder
from pyiceberg.transforms import DayTransform, IdentityTransform
from pyiceberg.types import (
    DoubleType,
    LongType,
    FloatType,
    NestedField,
    StringType,
    StructType,
    TimestampType,
    TimestamptzType,
    TimeType,
)
from pyiceberg.table.sorting import UNSORTED_SORT_ORDER

from helpers.cluster import ClickHouseCluster, ClickHouseInstance, is_arm
from helpers.config_cluster import minio_secret_key, minio_access_key
from helpers.s3_tools import get_file_contents, list_s3_objects, prepare_s3_bucket
from helpers.test_tools import TSV, csv_compare
from helpers.config_cluster import minio_secret_key
from helpers.network import PartitionManager
from helpers.client import QueryRuntimeException

BASE_URL = "http://rest:8181/v1"
BASE_URL_LOCAL = "http://localhost:8182/v1"
BASE_URL_LOCAL_RAW = "http://localhost:8182"

CATALOG_NAME = "demo"

DEFAULT_SCHEMA = Schema(
    NestedField(
        field_id=1, name="datetime", field_type=TimestampType(), required=False
    ),
    NestedField(field_id=2, name="symbol", field_type=StringType(), required=False),
    NestedField(field_id=3, name="bid", field_type=DoubleType(), required=False),
    NestedField(field_id=4, name="ask", field_type=DoubleType(), required=False),
    NestedField(
        field_id=5,
        name="details",
        field_type=StructType(
            NestedField(
                field_id=4,
                name="created_by",
                field_type=StringType(),
                required=False,
            ),
        ),
        required=False,
    ),
)

DEFAULT_CREATE_TABLE = "CREATE TABLE {}.`{}.{}`\\n(\\n    `datetime` Nullable(DateTime64(6)),\\n    `symbol` Nullable(String),\\n    `bid` Nullable(Float64),\\n    `ask` Nullable(Float64),\\n    `details` Tuple(created_by Nullable(String))\\n)\\nENGINE = Iceberg(\\'http://minio:9000/warehouse-rest/data/\\', \\'minio\\', \\'[HIDDEN]\\')\n"

DEFAULT_PARTITION_SPEC = PartitionSpec(
    PartitionField(
        source_id=1, field_id=1000, transform=DayTransform(), name="datetime_day"
    )
)

DEFAULT_SORT_ORDER = SortOrder(SortField(source_id=2, transform=IdentityTransform()))

AVAILABLE_ENGINES = ["DataLakeCatalog", "Iceberg"]


def list_namespaces():
    response = requests.get(f"{BASE_URL_LOCAL}/namespaces")
    if response.status_code == 200:
        return response.json()
    else:
        raise Exception(f"Failed to list namespaces: {response.status_code}")


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


def create_table(
    catalog,
    namespace,
    table,
    schema=DEFAULT_SCHEMA,
    partition_spec=DEFAULT_PARTITION_SPEC,
    sort_order=DEFAULT_SORT_ORDER,
):
    return catalog.create_table(
        identifier=f"{namespace}.{table}",
        schema=schema,
        location=f"s3://warehouse-rest/data",
        partition_spec=partition_spec,
        sort_order=sort_order,
    )


def generate_record():
    return {
        "datetime": datetime.now(),
        "symbol": str("kek"),
        "bid": round(random.uniform(100, 200), 2),
        "ask": round(random.uniform(200, 300), 2),
        "details": {"created_by": "Alice Smith"},
    }


def create_clickhouse_iceberg_database(
    started_cluster, node, name, additional_settings={}, engine='DataLakeCatalog'
):
    settings = {
        "catalog_type": "rest",
        "warehouse": "demo",
        "storage_endpoint": "http://minio:9000/warehouse-rest",
    }

    settings.update(additional_settings)

    node.query(
        f"""
DROP DATABASE IF EXISTS {name};
CREATE DATABASE {name} ENGINE = {engine}('{BASE_URL}', 'minio', '{minio_secret_key}')
SETTINGS {",".join((k+"="+repr(v) for k, v in settings.items()))}
    """,
        settings={
            "allow_database_iceberg": 1,
            "write_full_path_in_iceberg_metadata": 1,
        },
    )
    show_result = node.query(f"SHOW DATABASE {name}")
    assert minio_secret_key not in show_result
    assert "HIDDEN" in show_result

def create_clickhouse_iceberg_table(
    started_cluster, node, database_name, table_name, schema, additional_settings={}
):
    settings_suffix = "" if len(additional_settings) == 0 else f"SETTINGS {",".join((k+"="+repr(v) for k, v in additional_settings.items()))}"
    node.query(
        f"""
CREATE TABLE {CATALOG_NAME}.`{database_name}.{table_name}` {schema} ENGINE = IcebergS3('http://minio:9000/warehouse-rest/{table_name}/', '{minio_access_key}', '{minio_secret_key}')
{settings_suffix}
    """,
        settings={
            "allow_experimental_database_iceberg": 1,
            "write_full_path_in_iceberg_metadata": 1,
        },
    )

def drop_clickhouse_iceberg_table(
    node, database_name, table_name, if_exists=False
):
    if if_exists:
        node.query(
            f"""
    DROP TABLE IF EXISTS {CATALOG_NAME}.`{database_name}.{table_name}`
        """
        )
    else:
        node.query(
            f"""
    DROP TABLE {CATALOG_NAME}.`{database_name}.{table_name}`
        """
        )


@pytest.fixture(scope="module")
def started_cluster():
    try:
        cluster = ClickHouseCluster(__file__)
        cluster.add_instance(
            "node1",
            main_configs=["configs/backups.xml","configs/cluster.xml"],
            user_configs=[],
            stay_alive=True,
            with_iceberg_catalog=True,
        )

        cluster.add_instance(
            "node2",
            main_configs=["configs/backups.xml","configs/cluster.xml"],
            user_configs=[],
            stay_alive=True,
            with_iceberg_catalog=True,
            with_zookeeper=True,
        )

        logging.info("Starting cluster...")
        cluster.start()

        # TODO: properly wait for container
        time.sleep(10)

        yield cluster

    finally:
        cluster.shutdown()


@pytest.mark.parametrize("engine", AVAILABLE_ENGINES)
def test_list_tables(started_cluster, engine):
    node = started_cluster.instances["node1"]

    root_namespace = f"clickhouse_{uuid.uuid4()}"
    namespace_1 = f"{root_namespace}.testA.A"
    namespace_2 = f"{root_namespace}.testB.B"
    namespace_1_tables = ["tableA", "tableB"]
    namespace_2_tables = ["tableC", "tableD"]

    catalog = load_catalog_impl(started_cluster)

    for namespace in [namespace_1, namespace_2]:
        catalog.create_namespace(namespace)

    found = False
    for namespace_list in list_namespaces()["namespaces"]:
        if root_namespace == namespace_list[0]:
            found = True
            break
    assert found

    found = False
    for namespace_list in catalog.list_namespaces():
        if root_namespace == namespace_list[0]:
            found = True
            break
    assert found

    for namespace in [namespace_1, namespace_2]:
        assert len(catalog.list_tables(namespace)) == 0

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME, engine=engine)

    tables_list = ""
    for table in namespace_1_tables:
        create_table(catalog, namespace_1, table)
        if len(tables_list) > 0:
            tables_list += "\n"
        tables_list += f"{namespace_1}.{table}"

    for table in namespace_2_tables:
        create_table(catalog, namespace_2, table)
        if len(tables_list) > 0:
            tables_list += "\n"
        tables_list += f"{namespace_2}.{table}"

    assert (
        tables_list
        == node.query(
            f"SELECT name FROM system.tables WHERE database = '{CATALOG_NAME}' and name ILIKE '{root_namespace}%' ORDER BY name SETTINGS show_data_lake_catalogs_in_system_tables = true"
        ).strip()
    )
    node.restart_clickhouse()
    assert (
        tables_list
        == node.query(
            f"SELECT name FROM system.tables WHERE database = '{CATALOG_NAME}' and name ILIKE '{root_namespace}%' ORDER BY name SETTINGS show_data_lake_catalogs_in_system_tables = true"
        ).strip()
    )

    expected = DEFAULT_CREATE_TABLE.format(CATALOG_NAME, namespace_2, "tableC")
    assert expected == node.query(
        f"SHOW CREATE TABLE {CATALOG_NAME}.`{namespace_2}.tableC`"
    )


@pytest.mark.parametrize("engine", AVAILABLE_ENGINES)
def test_many_namespaces(started_cluster, engine):
    node = started_cluster.instances["node1"]
    root_namespace_1 = f"A_{uuid.uuid4()}"
    root_namespace_2 = f"B_{uuid.uuid4()}"
    namespaces = [
        f"{root_namespace_1}",
        f"{root_namespace_1}.B.C",
        f"{root_namespace_1}.B.C.D",
        f"{root_namespace_1}.B.C.D.E",
        f"{root_namespace_2}",
        f"{root_namespace_2}.C",
        f"{root_namespace_2}.CC",
    ]
    tables = ["A", "B", "C"]
    catalog = load_catalog_impl(started_cluster)

    for namespace in namespaces:
        catalog.create_namespace(namespace)
        for table in tables:
            create_table(catalog, namespace, table)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME, engine=engine)

    for namespace in namespaces:
        for table in tables:
            table_name = f"{namespace}.{table}"
            assert int(
                node.query(
                    f"SELECT count() FROM system.tables WHERE database = '{CATALOG_NAME}' and name = '{table_name}' SETTINGS show_data_lake_catalogs_in_system_tables = true"
                )
            )


@pytest.mark.parametrize("engine", AVAILABLE_ENGINES)
def test_select(started_cluster, engine):
    node = started_cluster.instances["node1"]

    test_ref = f"test_list_tables_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    namespace = f"{root_namespace}.A.B.C"
    namespaces_to_create = [
        root_namespace,
        f"{root_namespace}.A",
        f"{root_namespace}.A.B",
        f"{root_namespace}.A.B.C",
    ]

    catalog = load_catalog_impl(started_cluster)

    for namespace in namespaces_to_create:
        catalog.create_namespace(namespace)
        assert len(catalog.list_tables(namespace)) == 0

    table = create_table(catalog, namespace, table_name)

    num_rows = 10
    data = [generate_record() for _ in range(num_rows)]
    df = pa.Table.from_pylist(data)
    table.append(df)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME, engine=engine)

    expected = DEFAULT_CREATE_TABLE.format(CATALOG_NAME, namespace, table_name)
    assert expected == node.query(
        f"SHOW CREATE TABLE {CATALOG_NAME}.`{namespace}.{table_name}`"
    )

    assert num_rows == int(
        node.query(f"SELECT count() FROM {CATALOG_NAME}.`{namespace}.{table_name}`")
    )

    assert int(node.query(f"SELECT count() FROM system.iceberg_history WHERE table = '{namespace}.{table_name}' and database = '{CATALOG_NAME}'").strip()) == 1


@pytest.mark.parametrize("engine", AVAILABLE_ENGINES)
def test_hide_sensitive_info(started_cluster, engine):
    node = started_cluster.instances["node1"]

    test_ref = f"test_hide_sensitive_info_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    namespace = f"{root_namespace}.A"
    catalog = load_catalog_impl(started_cluster)
    catalog.create_namespace(namespace)

    table = create_table(catalog, namespace, table_name)

    create_clickhouse_iceberg_database(
        started_cluster,
        node,
        CATALOG_NAME,
        additional_settings={"catalog_credential": "SECRET_1"},
        engine=engine,
    )
    assert "SECRET_1" not in node.query(f"SHOW CREATE DATABASE {CATALOG_NAME}")

    create_clickhouse_iceberg_database(
        started_cluster,
        node,
        CATALOG_NAME,
        additional_settings={"auth_header": "Authorization: SECRET_2"},
        engine=engine,
    )
    assert "SECRET_2" not in node.query(f"SHOW CREATE DATABASE {CATALOG_NAME}")


@pytest.mark.parametrize("engine", AVAILABLE_ENGINES)
def test_tables_with_same_location(started_cluster, engine):
    node = started_cluster.instances["node1"]

    test_ref = f"test_tables_with_same_location_{uuid.uuid4()}"
    namespace = f"{test_ref}_namespace"
    catalog = load_catalog_impl(started_cluster)

    table_name = f"{test_ref}_table"
    table_name_2 = f"{test_ref}_table_2"

    catalog.create_namespace(namespace)
    table = create_table(catalog, namespace, table_name)
    table_2 = create_table(catalog, namespace, table_name_2)

    def record(key):
        return {
            "datetime": datetime.now(),
            "symbol": str(key),
            "bid": round(random.uniform(100, 200), 2),
            "ask": round(random.uniform(200, 300), 2),
            "details": {"created_by": "Alice Smith"},
        }

    data = [record('aaa') for _ in range(3)]
    df = pa.Table.from_pylist(data)
    table.append(df)

    data = [record('bbb') for _ in range(3)]
    df = pa.Table.from_pylist(data)
    table_2.append(df)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME, engine=engine)

    assert 'aaa\naaa\naaa' == node.query(f"SELECT symbol FROM {CATALOG_NAME}.`{namespace}.{table_name}`").strip()
    assert 'bbb\nbbb\nbbb' == node.query(f"SELECT symbol FROM {CATALOG_NAME}.`{namespace}.{table_name_2}`").strip()


def test_backup_database(started_cluster):
    node = started_cluster.instances["node1"]
    create_clickhouse_iceberg_database(started_cluster, node, "backup_database")

    backup_id = uuid.uuid4().hex
    backup_name = f"File('/backups/test_backup_{backup_id}/')"

    node.query(f"BACKUP DATABASE backup_database TO {backup_name}")
    node.query("DROP DATABASE backup_database SYNC")
    assert "backup_database" not in node.query("SHOW DATABASES")

    node.query(f"RESTORE DATABASE backup_database FROM {backup_name}", settings={"allow_database_iceberg": 1})
    assert (
        node.query("SHOW CREATE DATABASE backup_database")
        == "CREATE DATABASE backup_database\\nENGINE = DataLakeCatalog(\\'http://rest:8181/v1\\', \\'minio\\', \\'[HIDDEN]\\')\\nSETTINGS catalog_type = \\'rest\\', warehouse = \\'demo\\', storage_endpoint = \\'http://minio:9000/warehouse-rest\\'\n"
    )


def test_non_existing_tables(started_cluster):
    node = started_cluster.instances["node1"]

    test_ref = f"test_list_tables_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    namespace = f"{root_namespace}.A.B.C"
    namespaces_to_create = [
        root_namespace,
        f"{root_namespace}.A",
        f"{root_namespace}.A.B",
        f"{root_namespace}.A.B.C",
    ]

    catalog = load_catalog_impl(started_cluster)

    for namespace in namespaces_to_create:
        catalog.create_namespace(namespace)
        assert len(catalog.list_tables(namespace)) == 0

    table = create_table(catalog, namespace, table_name)

    num_rows = 10
    data = [generate_record() for _ in range(num_rows)]
    df = pa.Table.from_pylist(data)
    table.append(df)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

    expected = DEFAULT_CREATE_TABLE.format(CATALOG_NAME, namespace, table_name)
    assert expected == node.query(
        f"SHOW CREATE TABLE {CATALOG_NAME}.`{namespace}.{table_name}`"
    )

    try:
        node.query(
            f"SHOW CREATE TABLE {CATALOG_NAME}.`{namespace}.qweqwe`"
        )
    except Exception as e:
        assert "DB::Exception: Table" in str(e)
        assert "doesn't exist" in str(e)

    try:
        node.query(
            f"SHOW CREATE TABLE {CATALOG_NAME}.`qweqwe.qweqwe`"
        )
    except Exception as e:
        assert "DB::Exception: Table" in str(e)
        assert "doesn't exist" in str(e)


def test_timestamps(started_cluster):
    node = started_cluster.instances["node1"]

    test_ref = f"test_list_tables_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    catalog = load_catalog_impl(started_cluster)
    catalog.create_namespace(root_namespace)

    schema = Schema(
        NestedField(
            field_id=1, name="timestamp", field_type=TimestampType(), required=False
        ),
        NestedField(
            field_id=2,
            name="timestamptz",
            field_type=TimestamptzType(),
            required=False,
        ),
    )
    table = create_table(catalog, root_namespace, table_name, schema)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

    data = [
        {
            "timestamp": datetime(2024, 1, 1, hour=12, minute=0, second=0, microsecond=0),
            "timestamptz": datetime(
                2024,
                1,
                1,
                hour=12,
                minute=0,
                second=0,
                microsecond=0,
                tzinfo=pytz.timezone("UTC"),
            )
        }
    ]
    df = pa.Table.from_pylist(data)
    table.append(df)

    assert node.query(f"SHOW CREATE TABLE {CATALOG_NAME}.`{root_namespace}.{table_name}`") == f"CREATE TABLE {CATALOG_NAME}.`{root_namespace}.{table_name}`\\n(\\n    `timestamp` Nullable(DateTime64(6)),\\n    `timestamptz` Nullable(DateTime64(6, \\'UTC\\'))\\n)\\nENGINE = Iceberg(\\'http://minio:9000/warehouse-rest/data/\\', \\'minio\\', \\'[HIDDEN]\\')\n"
    assert node.query(f"SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`") == "2024-01-01 12:00:00.000000\t2024-01-01 12:00:00.000000\n"

    # Berlin - UTC+1 at winter
    # Istanbul - UTC+3 at winter

    # 'UTC' is default value, responce is equal to query above
    assert node.query(f"""
                      SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`
                      SETTINGS iceberg_timezone_for_timestamptz='UTC'
                      """) == "2024-01-01 12:00:00.000000\t2024-01-01 12:00:00.000000\n"
    # Timezone from setting
    assert node.query(f"""
                      SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`
                      SETTINGS iceberg_timezone_for_timestamptz='Europe/Berlin'
                      """) == "2024-01-01 12:00:00.000000\t2024-01-01 13:00:00.000000\n"
    # Empty value means session timezone, by default it is 'UTC' too
    assert node.query(f"""
                      SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`
                      SETTINGS iceberg_timezone_for_timestamptz=''
                      """) == "2024-01-01 12:00:00.000000\t2024-01-01 12:00:00.000000\n"
    # If session timezone is used, `timestamptz` does not changed, 'UTC' by default
    assert node.query(f"""
                      SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`
                      SETTINGS session_timezone='Asia/Istanbul'
                      """) == "2024-01-01 15:00:00.000000\t2024-01-01 12:00:00.000000\n"
    # Setiing `iceberg_timezone_for_timestamptz` does not affect `timestamp` column
    assert node.query(f"""
                      SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`
                      SETTINGS session_timezone='Asia/Istanbul', iceberg_timezone_for_timestamptz='Europe/Berlin'
                      """) == "2024-01-01 15:00:00.000000\t2024-01-01 13:00:00.000000\n"
    # Empty value, used non-default session timezone
    assert node.query(f"""
                      SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`
                      SETTINGS session_timezone='Asia/Istanbul', iceberg_timezone_for_timestamptz=''
                      """) == "2024-01-01 15:00:00.000000\t2024-01-01 15:00:00.000000\n"
    # Invalid timezone
    assert "Invalid time zone: Foo/Bar" in node.query_and_get_error(f"""
                      SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`
                      SETTINGS iceberg_timezone_for_timestamptz='Foo/Bar'
                      """)

    assert node.query(f"SHOW CREATE TABLE {CATALOG_NAME}.`{root_namespace}.{table_name}` SETTINGS iceberg_timezone_for_timestamptz='UTC'") == f"CREATE TABLE {CATALOG_NAME}.`{root_namespace}.{table_name}`\\n(\\n    `timestamp` Nullable(DateTime64(6)),\\n    `timestamptz` Nullable(DateTime64(6, \\'UTC\\'))\\n)\\nENGINE = Iceberg(\\'http://minio:9000/warehouse-rest/data/\\', \\'minio\\', \\'[HIDDEN]\\')\n"
    assert node.query(f"SHOW CREATE TABLE {CATALOG_NAME}.`{root_namespace}.{table_name}` SETTINGS iceberg_timezone_for_timestamptz='Europe/Berlin'") == f"CREATE TABLE {CATALOG_NAME}.`{root_namespace}.{table_name}`\\n(\\n    `timestamp` Nullable(DateTime64(6)),\\n    `timestamptz` Nullable(DateTime64(6, \\'Europe/Berlin\\'))\\n)\\nENGINE = Iceberg(\\'http://minio:9000/warehouse-rest/data/\\', \\'minio\\', \\'[HIDDEN]\\')\n"

    assert node.query(f"SELECT timezoneOf(timestamptz) FROM {CATALOG_NAME}.`{root_namespace}.{table_name}` LIMIT 1") == "UTC\n"
    assert node.query(f"SELECT timezoneOf(timestamptz) FROM {CATALOG_NAME}.`{root_namespace}.{table_name}` LIMIT 1 SETTINGS iceberg_timezone_for_timestamptz='UTC'") == "UTC\n"
    assert node.query(f"SELECT timezoneOf(timestamptz) FROM {CATALOG_NAME}.`{root_namespace}.{table_name}` LIMIT 1 SETTINGS iceberg_timezone_for_timestamptz='Europe/Berlin'") == "Europe/Berlin\n"


def test_insert(started_cluster):
    node = started_cluster.instances["node1"]

    test_ref = f"test_list_tables_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    catalog = load_catalog_impl(started_cluster)
    catalog.create_namespace(root_namespace)

    create_table(catalog, root_namespace, table_name, DEFAULT_SCHEMA, PartitionSpec(), DEFAULT_SORT_ORDER)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)
    node.query(f"INSERT INTO {CATALOG_NAME}.`{root_namespace}.{table_name}` VALUES (NULL, 'AAPL', 193.24, 193.31, tuple('bot'));", settings={"allow_insert_into_iceberg": 1, 'write_full_path_in_iceberg_metadata': 1})
    catalog.load_table(f"{root_namespace}.{table_name}")
    assert node.query(f"SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`") == "\\N\tAAPL\t193.24\t193.31\t('bot')\n"

    node.query(f"INSERT INTO {CATALOG_NAME}.`{root_namespace}.{table_name}` VALUES (NULL, 'Pavel Ivanov (pudge1000-7) pereezhai v amsterdam', 193.24, 193.31, tuple('bot'));", settings={"allow_insert_into_iceberg": 1, 'write_full_path_in_iceberg_metadata': 1})
    assert node.query(f"SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}` ORDER BY ALL") == "\\N\tAAPL\t193.24\t193.31\t('bot')\n\\N\tPavel Ivanov (pudge1000-7) pereezhai v amsterdam\t193.24\t193.31\t('bot')\n"


def test_create(started_cluster):
    node = started_cluster.instances["node1"]

    test_ref = f"test_list_tables_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)
    create_clickhouse_iceberg_table(started_cluster, node, root_namespace, table_name, "(x String)")
    node.query(f"INSERT INTO {CATALOG_NAME}.`{root_namespace}.{table_name}` VALUES ('AAPL');", settings={"allow_insert_into_iceberg": 1, 'write_full_path_in_iceberg_metadata': 1})
    assert node.query(f"SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`") == "AAPL\n"


def test_drop_table(started_cluster):
    node = started_cluster.instances["node1"]

    test_ref = f"test_list_tables_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    catalog = load_catalog_impl(started_cluster)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)
    create_clickhouse_iceberg_table(started_cluster, node, root_namespace, table_name, "(x String)")
    assert len(catalog.list_tables(root_namespace)) == 1

    drop_clickhouse_iceberg_table(node, root_namespace, table_name + "some_strange_non_exists_suffix", True)
    assert len(catalog.list_tables(root_namespace)) == 1

    drop_clickhouse_iceberg_table(node, root_namespace, table_name)
    assert len(catalog.list_tables(root_namespace)) == 0


def test_table_with_slash(started_cluster):
    node = started_cluster.instances["node1"]

    # pyiceberg at current moment (version 0.9.1) has a bug with table names with slashes
    # see https://github.com/apache/iceberg-python/issues/2462
    # so we need to encode it manually
    table_raw_suffix = "table/foo"
    table_encoded_suffix = "table%2Ffoo"

    test_ref = f"test_list_tables_{uuid.uuid4()}"
    table_name = f"{test_ref}_{table_raw_suffix}"
    table_encoded_name = f"{test_ref}_{table_encoded_suffix}"
    root_namespace = f"{test_ref}_namespace"

    catalog = load_catalog_impl(started_cluster)
    catalog.create_namespace(root_namespace)

    create_table(catalog, root_namespace, table_name, DEFAULT_SCHEMA, PartitionSpec(), DEFAULT_SORT_ORDER)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)
    node.query(f"INSERT INTO {CATALOG_NAME}.`{root_namespace}.{table_encoded_name}` VALUES (NULL, 'AAPL', 193.24, 193.31, tuple('bot'));", settings={"allow_insert_into_iceberg": 1, 'write_full_path_in_iceberg_metadata': 1})
    assert node.query(f"SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_encoded_name}`") == "\\N\tAAPL\t193.24\t193.31\t('bot')\n"


def test_partition_value_with_slash(started_cluster):
    """Partition value containing '/' produces object keys with %2F; reading must preserve encoding."""
    node = started_cluster.instances["node1"]

    test_ref = f"test_partition_slash_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    partition_spec = PartitionSpec(
        PartitionField(
            source_id=2, field_id=1000, transform=IdentityTransform(), name="symbol"
        )
    )
    schema = DEFAULT_SCHEMA

    catalog = load_catalog_impl(started_cluster)
    catalog.create_namespace(root_namespace)

    table = create_table(
        catalog,
        root_namespace,
        table_name,
        schema,
        partition_spec=partition_spec,
        sort_order=DEFAULT_SORT_ORDER,
    )

    data = [
        {
            "datetime": datetime.now(),
            "symbol": "us/west",
            "bid": 100.0,
            "ask": 101.0,
            "details": {"created_by": "test"},
        }
    ]
    df = pa.Table.from_pylist(data)
    table.append(df)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)
    assert 1 == int(node.query(f"SELECT count() FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`"))
    assert "us/west" in node.query(f"SELECT symbol FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`")


def test_cluster_select(started_cluster):
    node1 = started_cluster.instances["node1"]
    node2 = started_cluster.instances["node2"]

    test_ref = f"test_list_tables_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    catalog = load_catalog_impl(started_cluster)
    create_clickhouse_iceberg_database(started_cluster, node1, CATALOG_NAME)
    create_clickhouse_iceberg_database(started_cluster, node2, CATALOG_NAME)
    create_clickhouse_iceberg_table(started_cluster, node1, root_namespace, table_name, "(x String)")
    node1.query(f"INSERT INTO {CATALOG_NAME}.`{root_namespace}.{table_name}` VALUES ('pablo');", settings={"allow_insert_into_iceberg": 1, 'write_full_path_in_iceberg_metadata': 1})

    query_id = uuid.uuid4().hex
    assert node1.query(f"SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}` SETTINGS parallel_replicas_for_cluster_engines=1, enable_parallel_replicas=2, cluster_for_parallel_replicas='cluster_simple'", query_id=query_id) == 'pablo\n'

    node1.query("SYSTEM FLUSH LOGS system.query_log")
    node2.query("SYSTEM FLUSH LOGS system.query_log")

    assert node1.query(f"SELECT Settings['parallel_replicas_for_cluster_engines'] AS parallel_replicas_for_cluster_engines FROM system.query_log WHERE query_id = '{query_id}' LIMIT 1;") == '1\n'

    for replica in [node1, node2]:
        cluster_secondary_queries = (
            replica.query(
                f"""
                SELECT query, type, is_initial_query, read_rows, read_bytes FROM system.query_log
                WHERE
                    type = 'QueryStart' AND
                    positionCaseInsensitive(query, 's3Cluster') != 0 AND
                    position(query, 'system.query_log') = 0 AND
                    NOT is_initial_query
            """
            )
            .strip()
            .split("\n")
        )
        assert len(cluster_secondary_queries) == 1

    assert node2.query(f"SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`", settings={"parallel_replicas_for_cluster_engines":1, 'enable_parallel_replicas': 2, 'cluster_for_parallel_replicas': 'cluster_simple', 'parallel_replicas_for_cluster_engines' : 1}) == 'pablo\n'
    
def test_not_specified_catalog_type(started_cluster):
    node = started_cluster.instances["node1"]
    settings = {
        "warehouse": "demo",
        "storage_endpoint": "http://minio:9000/warehouse-rest",
    }

    node.query(
        f"""
    DROP DATABASE IF EXISTS {CATALOG_NAME};
    CREATE DATABASE {CATALOG_NAME} ENGINE = DataLakeCatalog('{BASE_URL}', 'minio', '{minio_secret_key}')
    SETTINGS {",".join((k+"="+repr(v) for k, v in settings.items()))}
    """,
        settings={
            "allow_database_iceberg": 1,
            "write_full_path_in_iceberg_metadata": 1,
        },
    )
    assert "" == node.query(f"SHOW TABLES FROM {CATALOG_NAME}")


def test_system_tables_with_nullptr_table(started_cluster):
    """
    Test that querying system.tables does not crash when DataLake database
    returns nullptr for some tables (e.g. when table metadata fetch fails).
    Reproduces: https://github.com/ClickHouse/clickhouse-core-incidents/issues/1434
    """
    node = started_cluster.instances["node1"]

    root_namespace = f"clickhouse_{uuid.uuid4()}"
    namespace = f"{root_namespace}_test_nullptr"

    catalog = load_catalog_impl(started_cluster)
    catalog.create_namespace(namespace)

    table_name = "test_table"
    create_table(catalog, namespace, table_name)

    num_rows = 5
    arrow_data = pa.table(
        {
            "datetime": [datetime.now() for _ in range(num_rows)],
            "symbol": [f"sym_{i}" for i in range(num_rows)],
            "bid": [float(i) for i in range(num_rows)],
            "ask": [float(i + 1) for i in range(num_rows)],
            "details": [{"created_by": f"user_{i}"} for i in range(num_rows)],
        },
        schema=pa.schema(
            [
                pa.field("datetime", pa.timestamp("us")),
                pa.field("symbol", pa.string()),
                pa.field("bid", pa.float64()),
                pa.field("ask", pa.float64()),
                pa.field(
                    "details", pa.struct([pa.field("created_by", pa.string())])
                ),
            ]
        ),
    )
    iceberg_table = catalog.load_table(f"{namespace}.{table_name}")
    iceberg_table.append(arrow_data)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

    ## Enable the failpoint so that tryGetTableImpl returns nullptr for all tables.
    node.query("SYSTEM ENABLE FAILPOINT datalake_try_get_table_return_nullptr")

    try:
        ## This triggers getFilteredTables with engine_column populated (the crash site).
        result = node.query(
            f"SELECT engine FROM system.tables WHERE database = '{CATALOG_NAME}' "
            f"SETTINGS show_data_lake_catalogs_in_system_tables = 1"
        )
        ## With the failpoint, all tables return nullptr so we get empty result.
        assert result.strip() == ""

        ## This triggers the fillData main loop path.
        result = node.query(
            f"SELECT * FROM system.tables WHERE database = '{CATALOG_NAME}' "
            f"SETTINGS show_data_lake_catalogs_in_system_tables = 1"
        )
        assert result.strip() == ""

        ## Also test with count() to exercise a different code path.
        result = node.query(
            f"SELECT count(engine) FROM system.tables WHERE database = '{CATALOG_NAME}' "
            f"AND engine LIKE '%ReplicatedMergeTree' "
            f"SETTINGS show_data_lake_catalogs_in_system_tables = 1"
        )
        assert result.strip() == "0"
    finally:
        node.query(
            "SYSTEM DISABLE FAILPOINT datalake_try_get_table_return_nullptr"
        )

    ## After disabling the failpoint, verify normal operation still works.
    result = node.query(
        f"SELECT count() FROM system.tables WHERE database = '{CATALOG_NAME}' "
        f"AND name ILIKE '%{table_name}%' "
        f"SETTINGS show_data_lake_catalogs_in_system_tables = 1"
    )
    assert int(result.strip()) > 0

    node.query(f"DROP DATABASE IF EXISTS {CATALOG_NAME}")

def test_gcs(started_cluster):
    node = started_cluster.instances["node1"]

    node.query("SYSTEM ENABLE FAILPOINT database_iceberg_gcs")
    node.query(f"DROP DATABASE IF EXISTS {CATALOG_NAME};")

    with pytest.raises(Exception) as err:
        node.query(
            f"""
            CREATE DATABASE {CATALOG_NAME}
            ENGINE = DataLakeCatalog('{BASE_URL_DOCKER}', 'gcs', 'dummy')
            SETTINGS
                catalog_type = 'rest',
                warehouse = 'demo',
            """,
            settings={"allow_database_iceberg": 1},
        )
        assert "Google cloud storage converts to S3" in str(err.value)


def test_invalid_auth_header_format(started_cluster):
    node = started_cluster.instances["node1"]

    node.query(f"DROP DATABASE IF EXISTS {CATALOG_NAME};")
    with pytest.raises(Exception) as err:
        node.query(
            f"""
            SET allow_database_iceberg = 1;
            CREATE DATABASE {CATALOG_NAME}
            ENGINE = DataLakeCatalog('{BASE_URL}', 'minio', 'dummy')
            SETTINGS
                catalog_type = 'rest',
                warehouse = 'demo',
                auth_header = 'wrong.header'
            """
        )
    assert "Invalid auth header format" in str(err.value)


def test_namespace_filter(started_cluster):
    node = started_cluster.instances["node1"]

    # Use the same table name in all namespaces
    table_name = f"table_{uuid.uuid4()}"
    table2_name = f"table2_{uuid.uuid4()}"
    namespace_prefix = f"namespace_{uuid.uuid4()}_"

    catalog = load_catalog_impl(started_cluster)

    def create_namespace(suffix):
        namespace = f"{namespace_prefix}{suffix}"
        catalog.create_namespace(namespace)
        create_table(catalog, namespace, table_name, DEFAULT_SCHEMA, PartitionSpec(), DEFAULT_SORT_ORDER)

    create_namespace("alpha");
    create_namespace("alpha.a1");
    create_namespace("alpha.a2");
    create_namespace("bravo");
    create_namespace("bravo.b1");
    create_namespace("charlie");
    create_namespace("charlie.c1");
    create_namespace("delta");
    create_namespace("delta.d1");
    create_namespace("delta.d2");
    create_namespace("echo");
    create_namespace("echo.e1");

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME,
                                       additional_settings={
                                           "namespaces": f"{namespace_prefix}alpha,{namespace_prefix}alpha.a1,{namespace_prefix}bravo,{namespace_prefix}bravo.*,{namespace_prefix}charlie,{namespace_prefix}delta.d1,{namespace_prefix}echo.*"
                                       })

    assert node.query(f"SELECT name FROM system.tables WHERE database='{CATALOG_NAME}' ORDER BY name", settings={"show_data_lake_catalogs_in_system_tables": 1}) == TSV(
        [
            [f"{namespace_prefix}alpha.a1.{table_name}"],
            [f"{namespace_prefix}alpha.{table_name}"],
            [f"{namespace_prefix}bravo.b1.{table_name}"],
            [f"{namespace_prefix}bravo.{table_name}"],
            [f"{namespace_prefix}charlie.{table_name}"],
            [f"{namespace_prefix}delta.d1.{table_name}"],
            [f"{namespace_prefix}echo.e1.{table_name}"],
        ])

    assert node.query(f"SELECT count() FROM {CATALOG_NAME}.`{namespace_prefix}alpha.{table_name}`") == "0\n"
    assert node.query(f"SELECT count() FROM {CATALOG_NAME}.`{namespace_prefix}alpha.a1.{table_name}`") == "0\n"
    assert "is filtered by `namespaces` database parameter." in node.query_and_get_error(f"SELECT count() FROM {CATALOG_NAME}.`{namespace_prefix}alpha.a2.{table_name}`")
    assert node.query(f"SELECT count() FROM {CATALOG_NAME}.`{namespace_prefix}bravo.{table_name}`") == "0\n"
    assert node.query(f"SELECT count() FROM {CATALOG_NAME}.`{namespace_prefix}bravo.b1.{table_name}`") == "0\n"
    assert node.query(f"SELECT count() FROM {CATALOG_NAME}.`{namespace_prefix}charlie.{table_name}`") == "0\n"
    assert "is filtered by `namespaces` database parameter." in node.query_and_get_error(f"SELECT count() FROM {CATALOG_NAME}.`{namespace_prefix}charlie.c1.{table_name}`")
    assert "is filtered by `namespaces` database parameter." in node.query_and_get_error(f"SELECT count() FROM {CATALOG_NAME}.`{namespace_prefix}delta.{table_name}`")
    assert node.query(f"SELECT count() FROM {CATALOG_NAME}.`{namespace_prefix}delta.d1.{table_name}`") == "0\n"
    assert "is filtered by `namespaces` database parameter." in node.query_and_get_error(f"SELECT count() FROM {CATALOG_NAME}.`{namespace_prefix}delta.d2.{table_name}`")
    assert "is filtered by `namespaces` database parameter." in node.query_and_get_error(f"SELECT count() FROM {CATALOG_NAME}.`{namespace_prefix}echo.{table_name}`")
    assert node.query(f"SELECT count() FROM {CATALOG_NAME}.`{namespace_prefix}echo.e1.{table_name}`") == "0\n"

    node.query(f"CREATE TABLE {CATALOG_NAME}.`{namespace_prefix}alpha.{table2_name}` (x String) ENGINE = IcebergS3('http://minio:9000/warehouse-rest/{namespace_prefix}alpha/{table2_name}/', '{minio_access_key}', '{minio_secret_key}')",
         settings={
            "allow_database_iceberg": 1,
            "write_full_path_in_iceberg_metadata": 1,
        },
    )
    node.query(f"CREATE TABLE {CATALOG_NAME}.`{namespace_prefix}alpha.a1.{table2_name}` (x String) ENGINE = IcebergS3('http://minio:9000/warehouse-rest/{namespace_prefix}alpha/a1/{table2_name}/', '{minio_access_key}', '{minio_secret_key}')",
         settings={
            "allow_database_iceberg": 1,
            "write_full_path_in_iceberg_metadata": 1,
        },
    )
    assert "is filtered by `namespaces` database parameter." in node.query_and_get_error(f"CREATE TABLE {CATALOG_NAME}.`{namespace_prefix}alpha.a2.{table2_name}` (x String) ENGINE = IcebergS3('http://minio:9000/warehouse-rest/{namespace_prefix}alpha/a2/{table2_name}/', '{minio_access_key}', '{minio_secret_key}')")

    node.query(f"DROP TABLE {CATALOG_NAME}.`{namespace_prefix}alpha.{table_name}`")
    node.query(f"DROP TABLE {CATALOG_NAME}.`{namespace_prefix}alpha.a1.{table_name}`")
    assert "is filtered by `namespaces` database parameter." in node.query_and_get_error(f"DROP TABLE {CATALOG_NAME}.`{namespace_prefix}alpha.a2.{table_name}`")


def test_writes_mutate_update(started_cluster):
    node = started_cluster.instances["node1"]

    test_ref = f"test_writes_mutate_update_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"
    table_ref = f"{CATALOG_NAME}.`{root_namespace}.{table_name}`"
    write_settings = {"allow_insert_into_iceberg": 1, "write_full_path_in_iceberg_metadata": 1}

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)
    create_clickhouse_iceberg_table(started_cluster, node, root_namespace, table_name, "(x String, y Int32)")

    node.query(f"INSERT INTO {table_ref} VALUES ('123', 1);", settings=write_settings)
    node.query(f"INSERT INTO {table_ref} VALUES ('456', 2);", settings=write_settings)
    node.query(f"INSERT INTO {table_ref} VALUES ('999', 3);", settings=write_settings)
    assert node.query(f"SELECT * FROM {table_ref} ORDER BY ALL") == "123\t1\n456\t2\n999\t3\n"

    node.query(f"ALTER TABLE {table_ref} UPDATE x = '777' WHERE x = '123';", settings=write_settings)
    assert node.query(f"SELECT * FROM {table_ref} ORDER BY ALL") == "456\t2\n777\t1\n999\t3\n"

    node.query(f"ALTER TABLE {table_ref} UPDATE x = 'goshan dr' WHERE x = '777';", settings=write_settings)
    assert node.query(f"SELECT * FROM {table_ref} ORDER BY ALL") == "456\t2\n999\t3\ngoshan dr\t1\n"

    node.query(f"ALTER TABLE {table_ref} UPDATE x = 'pudge1000-7' WHERE y = 2;", settings=write_settings)
    assert node.query(f"SELECT * FROM {table_ref} ORDER BY ALL") == "999\t3\ngoshan dr\t1\npudge1000-7\t2\n"


def test_writes_mutate_delete(started_cluster):
    node = started_cluster.instances["node1"]

    test_ref = f"test_writes_mutate_delete_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"
    table_ref = f"{CATALOG_NAME}.`{root_namespace}.{table_name}`"
    write_settings = {"allow_insert_into_iceberg": 1, "write_full_path_in_iceberg_metadata": 1}

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)
    create_clickhouse_iceberg_table(started_cluster, node, root_namespace, table_name, "(x String)")

    # DELETE on empty table is a no-op.
    node.query(f"ALTER TABLE {table_ref} DELETE WHERE x = 'pudge1000-7';", settings=write_settings)
    assert node.query(f"SELECT * FROM {table_ref} ORDER BY ALL") == ""

    node.query(f"INSERT INTO {table_ref} VALUES ('123');", settings=write_settings)
    node.query(f"INSERT INTO {table_ref} VALUES ('456');", settings=write_settings)
    node.query(f"INSERT INTO {table_ref} VALUES ('789'), ('890'), ('999');", settings=write_settings)
    assert node.query(f"SELECT * FROM {table_ref} ORDER BY ALL") == "123\n456\n789\n890\n999\n"

    # No-match DELETE keeps the table intact.
    node.query(f"ALTER TABLE {table_ref} DELETE WHERE x = 'pudge1000-7';", settings=write_settings)
    assert node.query(f"SELECT * FROM {table_ref} ORDER BY ALL") == "123\n456\n789\n890\n999\n"

    node.query(f"ALTER TABLE {table_ref} DELETE WHERE x = '789';", settings=write_settings)
    assert node.query(f"SELECT * FROM {table_ref} ORDER BY ALL") == "123\n456\n890\n999\n"

    # Lightweight DELETE syntax should work identically against catalog tables.
    node.query(f"DELETE FROM {table_ref} WHERE x = '123';", settings=write_settings)
    assert node.query(f"SELECT * FROM {table_ref} ORDER BY ALL") == "456\n890\n999\n"

    node.query(f"ALTER TABLE {table_ref} DELETE WHERE x = '999';", settings=write_settings)
    assert node.query(f"SELECT * FROM {table_ref} ORDER BY ALL") == "456\n890\n"


@pytest.mark.parametrize("join_mode", ["local", "global"])
def test_cluster_joins(started_cluster, join_mode):
    node = started_cluster.instances["node1"]

    test_ref = f"test_join_tables_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    table_name_2 = f"{test_ref}_table_2"
    table_name_local = f"{test_ref}_table_local"

    root_namespace = f"{test_ref}_namespace"

    catalog = load_catalog_impl(started_cluster)
    catalog.create_namespace(root_namespace)

    schema = Schema(
        NestedField(
            field_id=1,
            name="tag",
            field_type=LongType(),
            required=False
        ),
        NestedField(
            field_id=2,
            name="name",
            field_type=StringType(),
            required=False,
        ),
    )
    table = create_table(catalog, root_namespace, table_name, schema,
                         partition_spec=UNPARTITIONED_PARTITION_SPEC, sort_order=UNSORTED_SORT_ORDER)
    data = [{"tag": 1, "name": "John"}, {"tag": 2, "name": "Jack"}]
    df = pa.Table.from_pylist(data)
    table.append(df)

    schema2 = Schema(
        NestedField(
            field_id=1,
            name="id",
            field_type=LongType(),
            required=False
        ),
        NestedField(
            field_id=2,
            name="second_name",
            field_type=StringType(),
            required=False,
        ),
    )
    table2 = create_table(catalog, root_namespace, table_name_2, schema2,
                          partition_spec=UNPARTITIONED_PARTITION_SPEC, sort_order=UNSORTED_SORT_ORDER)
    data = [{"id": 1, "second_name": "Dow"}, {"id": 2, "second_name": "Sparrow"}]
    df = pa.Table.from_pylist(data)
    table2.append(df)

    node.query(f"CREATE TABLE `{table_name_local}` (id Int64, second_name String) ENGINE = Memory()")
    node.query(f"INSERT INTO `{table_name_local}` VALUES (1, 'Silver'), (2, 'Black')")

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

    res = node.query(
        f"""
            SELECT t1.name,t2.second_name
            FROM {CATALOG_NAME}.`{root_namespace}.{table_name}` AS t1
                JOIN {CATALOG_NAME}.`{root_namespace}.{table_name_2}` AS t2
                ON t1.tag=t2.id
            WHERE t1.tag < 10 AND t2.id < 20
            ORDER BY ALL
            SETTINGS
                object_storage_cluster='cluster_simple',
                object_storage_cluster_join_mode='{join_mode}'
        """
    )

    assert res == "Jack\tSparrow\nJohn\tDow\n"

    res = node.query(
        f"""
            SELECT name
            FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`
            WHERE tag in (
                SELECT id
                FROM {CATALOG_NAME}.`{root_namespace}.{table_name_2}`
            )
            ORDER BY ALL
            SETTINGS
                object_storage_cluster='cluster_simple',
                object_storage_cluster_join_mode='{join_mode}'
        """
    )

    assert res == "Jack\nJohn\n"

    res = node.query(
        f"""
            SELECT t1.name,t2.second_name
            FROM {CATALOG_NAME}.`{root_namespace}.{table_name}` AS t1
                JOIN `{table_name_local}` AS t2
                ON t1.tag=t2.id
            WHERE t1.tag < 10 AND t2.id < 20
            ORDER BY ALL
            SETTINGS
                object_storage_cluster='cluster_simple',
                object_storage_cluster_join_mode='{join_mode}'
        """
    )

    assert res == "Jack\tBlack\nJohn\tSilver\n"

    res = node.query(
        f"""
            SELECT name
            FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`
            WHERE tag in (
                SELECT id
                FROM `{table_name_local}`
            )
            ORDER BY ALL
            SETTINGS
                object_storage_cluster='cluster_simple',
                object_storage_cluster_join_mode='{join_mode}'
        """
    )

    assert res == "Jack\nJohn\n"

    res = node.query(
        f"""
            SELECT t1.name,t2.second_name
            FROM {CATALOG_NAME}.`{root_namespace}.{table_name}` AS t1
                CROSS JOIN `{table_name_local}` AS t2
            WHERE t1.tag < 10 AND t2.id < 20
            ORDER BY ALL
            SETTINGS
                object_storage_cluster='cluster_simple',
                object_storage_cluster_join_mode='{join_mode}'
        """
    )

    assert res == "Jack\tBlack\nJack\tSilver\nJohn\tBlack\nJohn\tSilver\n"


def test_partitioning_by_time(started_cluster):
    node = started_cluster.instances["node1"]

    test_ref = f"test_partitioning_by_time_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    namespace = f"{root_namespace}.A"
    catalog = load_catalog_impl(started_cluster)
    catalog.create_namespace(namespace)

    schema = Schema(
        NestedField(
            field_id=1,
            name="key",
            field_type=TimeType(),
            required=False
        ),
        NestedField(
            field_id=2,
            name="value",
            field_type=StringType(),
            required=False,
        ),
    )

    partition_spec = PartitionSpec(
        PartitionField(
            source_id=1, field_id=1000, transform=IdentityTransform(), name="partition_key"
        )
    )

    table = create_table(catalog, namespace, table_name, schema=schema, partition_spec=partition_spec)
    data = [{"key": dtime(12,0,0), "value": "test1"},
            {"key": dtime(13,0,0), "value": "test2"},
            {"key": dtime(14,0,0), "value": "test3"},
            ]
    df = pa.Table.from_pylist(data)
    table.append(df)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

    assert node.query(f"SELECT * FROM {CATALOG_NAME}.`{namespace}.{table_name}` ORDER BY key") == "12:00:00.000000\ttest1\n13:00:00.000000\ttest2\n14:00:00.000000\ttest3\n"
    assert node.query(f"SELECT * FROM {CATALOG_NAME}.`{namespace}.{table_name}` WHERE key = '13:00:00.000000' ORDER BY key") == "13:00:00.000000\ttest2\n"
    assert node.query(f"SELECT * FROM {CATALOG_NAME}.`{namespace}.{table_name}` WHERE key >= '13:00:00.000000' ORDER BY key") == "13:00:00.000000\ttest2\n14:00:00.000000\ttest3\n"
    assert node.query(f"SELECT * FROM {CATALOG_NAME}.`{namespace}.{table_name}` WHERE key <= '13:00:00.000000' ORDER BY key") == "12:00:00.000000\ttest1\n13:00:00.000000\ttest2\n"


def test_partitioning_by_string(started_cluster):
    node = started_cluster.instances["node1"]

    test_ref = f"test_partitioning_by_string_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    namespace = f"{root_namespace}.A"
    catalog = load_catalog_impl(started_cluster)
    catalog.create_namespace(namespace)

    schema = Schema(
        NestedField(
            field_id=1,
            name="key",
            field_type=StringType(),
            required=False
        ),
        NestedField(
            field_id=2,
            name="value",
            field_type=StringType(),
            required=False,
        ),
        NestedField(
            field_id=3,
            name="time_value",
            field_type=TimeType(),
            required=False,
        ),
    )

    partition_spec = PartitionSpec(
        PartitionField(
            source_id=1, field_id=1000, transform=IdentityTransform(), name="partition_key"
        )
    )

    table = create_table(catalog, namespace, table_name, schema=schema, partition_spec=partition_spec)
    data = [{"key": "a:b,c[d=e/f%g?h", "value": "test", "time_value": dtime(12,0,0)}]
    df = pa.Table.from_pylist(data)
    table.append(df)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

    assert node.query(f"SELECT * FROM {CATALOG_NAME}.`{namespace}.{table_name}`") == "a:b,c[d=e/f%g?h\ttest\t12:00:00.000000\n"


def test_iceberg_file_progress_callback(started_cluster):
    """
    Regression test for the `IcebergIterator::next` file-progress callback wiring (PR #105413).

    `IcebergIterator` stored a `FileProgressCallback` but never invoked it, so the
    per-query `Progress.total_bytes_to_read` stayed at zero for Iceberg scans and
    the progress bar showed no estimate. The fix invokes the callback with the data
    file size for every object info returned. The assertion below uses the
    `FileProgressCallbackInvocations` ProfileEvent, which is incremented inside the
    callback lambda installed by `TCPHandler::setFileProgressCallback`, so removing
    the `callback(...)` call in `IcebergIterator::next` makes this event stay at
    zero for the test query.
    """
    node = started_cluster.instances["node1"]

    test_ref = f"test_progress_callback_{uuid.uuid4().hex[:8]}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    catalog = load_catalog_impl(started_cluster)
    catalog.create_namespace(root_namespace)

    table = create_table(
        catalog,
        root_namespace,
        table_name,
        DEFAULT_SCHEMA,
        PartitionSpec(),
        DEFAULT_SORT_ORDER,
    )

    # Append a small but non-empty batch so the iterator returns a data-file entry.
    num_rows = 50
    data = [generate_record() for _ in range(num_rows)]
    df = pa.Table.from_pylist(data)
    table.append(df)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

    # `node.query` uses native TCP, the only protocol path where
    # `setFileProgressCallback` is currently wired. `SELECT *` with `FORMAT Null`
    # forces a full scan: the metadata-only `SELECT count()` path resolves the row
    # count from manifest statistics and bypasses the data-file iterator.
    query_id = f"iceberg_progress_callback_{uuid.uuid4().hex}"
    node.query(
        f"SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}` FORMAT Null",
        query_id=query_id,
    )

    node.query("SYSTEM FLUSH LOGS")

    # `FileProgressCallbackInvocations` is incremented inside the lambda installed
    # by `TCPHandler::setFileProgressCallback`. For an Iceberg-table scan it can
    # only fire from `IcebergIterator::next` (the generic
    # `StorageObjectStorageSource::KeysIterator` path is replaced by
    # `IcebergIterator` for Iceberg storage), so a non-zero value proves the
    # iterator's `callback(FileProgress(...))` invocation was executed.
    profile_event_value = node.query(
        f"""
        SELECT ProfileEvents['FileProgressCallbackInvocations']
        FROM system.query_log
        WHERE query_id = '{query_id}' AND type = 'QueryFinish'
        ORDER BY event_time_microseconds DESC
        LIMIT 1
        """
    ).strip()
    assert profile_event_value, (
        f"`system.query_log` has no `QueryFinish` row for query_id={query_id}."
    )
    file_progress_callback_invocations = int(profile_event_value)
    assert file_progress_callback_invocations > 0, (
        f"Expected `FileProgressCallbackInvocations` > 0 from the Iceberg scan, "
        f"got {file_progress_callback_invocations}. "
        f"`IcebergIterator::next` did not invoke the file-progress callback "
        f"(regression of PR #105413 wiring)."
    )


def test_alter_database_settings_not_supported(started_cluster):
    node = started_cluster.instances["node1"]

    db_name = f"iceberg_alter_settings_{uuid.uuid4().hex}"
    create_clickhouse_iceberg_database(started_cluster, node, db_name)

    fake_token = f"fake_secret_token_{uuid.uuid4().hex}"

    qid_alter = uuid.uuid4().hex
    error = node.query_and_get_error(
        f"ALTER DATABASE {db_name} MODIFY SETTING warehouse = 'other_warehouse'"
    )
    assert "BAD_ARGUMENTS" in error
    error = node.query_and_get_error(
        f"ALTER DATABASE {db_name} MODIFY SETTING onelake_bearer_token = '{fake_token}'",
        query_id=qid_alter,
    )
    assert "BAD_ARGUMENTS" in error

    error = node.query_and_get_error(
        f"ALTER DATABASE {db_name} MODIFY SETTING no_such_setting = 1"
    )
    assert "BAD_ARGUMENTS" in error or "UNKNOWN_SETTING" in error

    show_result = node.query(f"SHOW CREATE DATABASE {db_name}")
    assert "other_warehouse" not in show_result
    assert "onelake_bearer_token" not in show_result
    node.query(
        f"SELECT name FROM system.tables WHERE database = '{db_name}' SETTINGS show_data_lake_catalogs_in_system_tables = true"
    )

    node.query("SYSTEM FLUSH LOGS system.query_log")
    logged_query = node.query(
        f"SELECT arrayStringConcat(groupArray(query), '\\n') FROM system.query_log WHERE query_id = '{qid_alter}'"
    )
    assert fake_token not in logged_query
    assert "[HIDDEN]" in logged_query

    node.query(f"DROP DATABASE {db_name}")

    glue_db_name = f"glue_alter_settings_{uuid.uuid4().hex}"
    node.query(
        f"""
        ATTACH DATABASE {glue_db_name} ENGINE = DataLakeCatalog('http://fake-glue:1')
        SETTINGS catalog_type = 'glue', region = 'us-east-1', storage_endpoint = 'http://fake-glue:1/x'
        """
    )
    error = node.query_and_get_error(
        f"ALTER DATABASE {glue_db_name} MODIFY SETTING region = 'eu-west-1'"
    )
    assert "NOT_IMPLEMENTED" in error
    node.query(f"DROP DATABASE {glue_db_name}")


def test_alter_database_settings_rest_auth_header(started_cluster):
    node = started_cluster.instances["node1"]

    db_name = f"rest_alter_auth_header_{uuid.uuid4().hex}"
    old_header = f"Authorization: Bearer old_{uuid.uuid4().hex}"
    new_header = f"Authorization: Bearer new_{uuid.uuid4().hex}"

    node.query(
        f"""
        ATTACH DATABASE {db_name} ENGINE = DataLakeCatalog('http://fake-rest:1/api')
        SETTINGS catalog_type = 'rest', warehouse = 'wh', auth_header = '{old_header}'
        """
    )

    node.query(
        f"ALTER DATABASE {db_name} MODIFY SETTING auth_header = '{new_header}'"
    )

    error = node.query_and_get_error(
        f"ALTER DATABASE {db_name} MODIFY SETTING catalog_credential = 'id:secret'"
    )
    assert "BAD_ARGUMENTS" in error

    show_result = node.query(f"SHOW CREATE DATABASE {db_name}")
    assert new_header not in show_result
    assert "[HIDDEN]" in show_result

    node.restart_clickhouse()

    engine_full_with_secrets = node.query(
        f"SELECT engine_full FROM system.databases WHERE name = '{db_name}'",
        settings={"format_display_secrets_in_show_and_select": 1},
    )
    assert new_header in engine_full_with_secrets
    assert old_header not in engine_full_with_secrets

    node.query(f"DROP DATABASE {db_name}")


def test_alter_database_settings_onelake_persistence(started_cluster):
    node = started_cluster.instances["node1"]

    db_name = f"onelake_alter_persist_{uuid.uuid4().hex}"
    old_token = f"secret_token_{uuid.uuid4().hex}"
    new_token = f"secret_token_{uuid.uuid4().hex}"

    node.query(
        f"""
        ATTACH DATABASE {db_name} ENGINE = DataLakeCatalog('http://fake-onelake:1/api')
        SETTINGS catalog_type = 'onelake', warehouse = 'wh', onelake_tenant_id = 'tenant-0', onelake_tenant_id = 'tenant-1', onelake_bearer_token = '{old_token}'
        """
    )

    node.query(
        f"ALTER DATABASE {db_name} MODIFY SETTING onelake_tenant_id = 'tenant-2', onelake_bearer_token = '{new_token}'"
    )

    error = node.query_and_get_error(
        f"ALTER DATABASE {db_name} MODIFY SETTING onelake_client_id = 'client-1'"
    )
    assert "BAD_ARGUMENTS" in error

    error = node.query_and_get_error(
        f"ALTER DATABASE {db_name} MODIFY SETTING warehouse = 'other_warehouse'"
    )
    assert "BAD_ARGUMENTS" in error

    error = node.query_and_get_error(
        f"ALTER DATABASE {db_name} MODIFY SETTING onelake_bearer_token = ''"
    )
    assert "BAD_ARGUMENTS" in error

    show_result = node.query(f"SHOW CREATE DATABASE {db_name}")
    assert "tenant-2" in show_result
    assert new_token not in show_result
    assert old_token not in show_result
    assert "[HIDDEN]" in show_result

    engine_full_with_secrets = node.query(
        f"SELECT engine_full FROM system.databases WHERE name = '{db_name}'",
        settings={"format_display_secrets_in_show_and_select": 1},
    )
    assert "tenant-2" in engine_full_with_secrets
    assert new_token in engine_full_with_secrets
    assert old_token not in engine_full_with_secrets

    node.restart_clickhouse()

    show_result = node.query(f"SHOW CREATE DATABASE {db_name}")
    assert "tenant-2" in show_result
    assert "tenant-0" not in show_result
    assert "tenant-1" not in show_result
    assert new_token not in show_result
    assert "[HIDDEN]" in show_result

    engine_full = node.query(
        f"SELECT engine_full FROM system.databases WHERE name = '{db_name}'"
    )
    assert "tenant-2" in engine_full
    assert "tenant-0" not in engine_full
    assert "tenant-1" not in engine_full
    assert new_token not in engine_full

    engine_full_with_secrets = node.query(
        f"SELECT engine_full FROM system.databases WHERE name = '{db_name}'",
        settings={"format_display_secrets_in_show_and_select": 1},
    )
    assert new_token in engine_full_with_secrets
    assert old_token not in engine_full_with_secrets

    node.query(f"DROP DATABASE {db_name}")


def test_alter_database_settings_onelake_refresh_token(started_cluster):
    node = started_cluster.instances["node1"]

    db_name = f"onelake_refresh_token_{uuid.uuid4().hex}"
    old_token = f"refresh_token_{uuid.uuid4().hex}"
    new_token = f"refresh_token_{uuid.uuid4().hex}"

    error = node.query_and_get_error(
        f"""
        CREATE DATABASE {db_name} ENGINE = DataLakeCatalog('http://fake-onelake:1/api')
        SETTINGS catalog_type = 'onelake', warehouse = 'wh', onelake_tenant_id = 'tenant-1', onelake_refresh_token = '{old_token}'
        """,
        settings={"allow_database_iceberg": 1},
    )
    assert "BAD_ARGUMENTS" in error

    error = node.query_and_get_error(
        f"""
        CREATE DATABASE {db_name} ENGINE = DataLakeCatalog('http://fake-onelake:1/api')
        SETTINGS catalog_type = 'onelake', warehouse = 'wh', onelake_tenant_id = 'tenant-1', onelake_client_id = 'client-1', onelake_refresh_token = '{old_token}', oauth_server_use_request_body = 0
        """,
        settings={"allow_database_iceberg": 1},
    )
    assert "BAD_ARGUMENTS" in error
    assert "oauth_server_use_request_body" in error

    # In refresh-token mode the catalog access token is reused for Azure storage,
    # so a non-storage auth_scope is rejected at CREATE time.
    error = node.query_and_get_error(
        f"""
        CREATE DATABASE {db_name} ENGINE = DataLakeCatalog('http://fake-onelake:1/api')
        SETTINGS catalog_type = 'onelake', warehouse = 'wh', onelake_tenant_id = 'tenant-1', onelake_client_id = 'client-1', onelake_refresh_token = '{old_token}', auth_scope = 'api://my-catalog/.default'
        """,
        settings={"allow_database_iceberg": 1},
    )
    assert "BAD_ARGUMENTS" in error
    assert "auth_scope" in error

    node.query(
        f"""
        ATTACH DATABASE {db_name} ENGINE = DataLakeCatalog('http://fake-onelake:1/api')
        SETTINGS catalog_type = 'onelake', warehouse = 'wh', onelake_tenant_id = 'tenant-1', onelake_client_id = 'client-1', onelake_refresh_token = '{old_token}'
        """
    )

    node.query(
        f"ALTER DATABASE {db_name} MODIFY SETTING onelake_refresh_token = '{new_token}'"
    )

    error = node.query_and_get_error(
        f"ALTER DATABASE {db_name} MODIFY SETTING onelake_bearer_token = 'token'"
    )
    assert "BAD_ARGUMENTS" in error

    show_result = node.query(f"SHOW CREATE DATABASE {db_name}")
    assert new_token not in show_result
    assert "[HIDDEN]" in show_result

    node.restart_clickhouse()

    engine_full_with_secrets = node.query(
        f"SELECT engine_full FROM system.databases WHERE name = '{db_name}'",
        settings={"format_display_secrets_in_show_and_select": 1},
    )
    assert new_token in engine_full_with_secrets
    assert old_token not in engine_full_with_secrets

    engine_full = node.query(
        f"SELECT engine_full FROM system.databases WHERE name = '{db_name}'"
    )
    assert new_token not in engine_full

    node.query(f"DROP DATABASE {db_name}")


def test_catalog_listing_error_surfaces_in_system_tables(started_cluster):
    """
    Regression test: an error from the catalog while listing tables (e.g. expired
    catalog credentials) must not be silently turned into an empty listing when the
    user explicitly opted into showing datalake catalogs in system tables with
    show_data_lake_catalogs_in_system_tables=1. Without the opt-in the old tolerant
    behaviour is kept (system tables must not fail because of one broken catalog).
    """
    node = started_cluster.instances["node1"]

    root_namespace = f"clickhouse_{uuid.uuid4()}"
    namespace = f"{root_namespace}_test_listing_error"

    catalog = load_catalog_impl(started_cluster)
    catalog.create_namespace(namespace)
    create_table(catalog, namespace, "table_x")

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

    node.query("SYSTEM ENABLE FAILPOINT datalake_get_tables_throw")
    try:
        assert (
            node.query(
                f"SELECT count() FROM system.iceberg_files WHERE database = '{CATALOG_NAME}'"
            ).strip()
            == "0"
        )

        error = node.query_and_get_error(
            f"SELECT count() FROM system.iceberg_files WHERE database = '{CATALOG_NAME}' "
            "SETTINGS show_data_lake_catalogs_in_system_tables = 1"
        )
        assert "Injected catalog listing failure" in error

        error = node.query_and_get_error(
            f"SELECT name FROM system.tables WHERE database = '{CATALOG_NAME}' "
            "SETTINGS show_data_lake_catalogs_in_system_tables = 1"
        )
        assert "Injected catalog listing failure" in error

        error = node.query_and_get_error(
            f"SELECT name, engine FROM system.tables WHERE database = '{CATALOG_NAME}' "
            "SETTINGS show_data_lake_catalogs_in_system_tables = 1"
        )
        assert "Injected catalog listing failure" in error
    finally:
        node.query("SYSTEM DISABLE FAILPOINT datalake_get_tables_throw")

    result = node.query(
        f"SELECT name FROM system.tables WHERE database = '{CATALOG_NAME}' "
        "SETTINGS show_data_lake_catalogs_in_system_tables = 1"
    )
    assert "table_x" in result

    node.query(f"DROP DATABASE IF EXISTS {CATALOG_NAME}")


def test_catalog_commit_conflict_reaches_caller_at_once(started_cluster):
    node = started_cluster.instances["node1"]

    test_ref = f"test_catalog_commit_conflict_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"
    table_ref = f"{CATALOG_NAME}.`{root_namespace}.{table_name}`"

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)
    create_clickhouse_iceberg_table(
        started_cluster, node, root_namespace, table_name, "(x UInt64)"
    )

    # Every sink reads the branch tip in its constructor, and all `max_insert_threads` sinks are
    # constructed before the pipeline starts, so all but one of them commit against a stale parent
    # and are refused with `409`. That makes the conflict a property of the plan rather than a race.
    num_writers = 4
    query_id = uuid.uuid4().hex
    node.query(
        f"INSERT INTO {table_ref} SELECT number FROM numbers_mt(4000000)",
        query_id=query_id,
        settings={
            "allow_insert_into_iceberg": 1,
            "write_full_path_in_iceberg_metadata": 1,
            "max_insert_threads": num_writers,
            "max_threads": num_writers,
        },
    )

    node.query("SYSTEM FLUSH LOGS system.text_log")

    conflicts, http_requests = map(
        int,
        node.query(
            f"""
            SELECT
                countIf(logger_name LIKE 'RestCatalog%' AND message LIKE '%updateMetadata conflict%'),
                countIf(logger_name = 'ReadWriteBufferFromHTTP')
            FROM system.text_log
            WHERE query_id = '{query_id}' AND message LIKE '%409%'
            """
        ).split(),
    )

    assert conflicts, (
        "no writer was refused, so nothing about conflict handling was exercised"
    )
    assert http_requests == conflicts, (
        f"{conflicts} refused commit(s) cost {http_requests} catalog requests, so a conflict is "
        f"resent with backoff instead of being handed back to the sink at once"
    )

    assert int(node.query(f"SELECT count() FROM {table_ref}")) == 4000000

    node.query(f"DROP DATABASE IF EXISTS {CATALOG_NAME}")
