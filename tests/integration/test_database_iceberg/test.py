import io
import json
import logging
import random
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, time as dtime

import pyarrow as pa
import pytest
import requests
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
    NestedField,
    StringType,
    StructType,
    TimestampType,
    TimestamptzType,
    TimeType,
)
from pyiceberg.table.sorting import UNSORTED_SORT_ORDER

from helpers.cluster import ClickHouseCluster
from helpers.config_cluster import minio_secret_key, minio_access_key
from helpers.client import QueryRuntimeException
from helpers.s3_tools import get_file_contents
from helpers.test_tools import TSV

BASE_URL = "http://rest:8181/v1"

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

DEFAULT_CREATE_TABLE = "CREATE TABLE {}.`{}.{}`\\n(\\n    `datetime` Nullable(DateTime64(6)),\\n    `symbol` Nullable(String),\\n    `bid` Nullable(Float64),\\n    `ask` Nullable(Float64),\\n    `details` Tuple(created_by Nullable(String))\\n)\\nENGINE = Iceberg(\\'http://minio1:9001/warehouse-rest/data/\\', \\'minio\\', \\'[HIDDEN]\\')\n"

DEFAULT_PARTITION_SPEC = PartitionSpec(
    PartitionField(
        source_id=1, field_id=1000, transform=DayTransform(), name="datetime_day"
    )
)

DEFAULT_SORT_ORDER = SortOrder(SortField(source_id=2, transform=IdentityTransform()))

AVAILABLE_ENGINES = ["DataLakeCatalog", "Iceberg"]


def list_namespaces(started_cluster):
    base_url_local = f"http://localhost:{started_cluster.iceberg_rest_catalog_port}/v1"
    response = requests.get(f"{base_url_local}/namespaces")
    if response.status_code == 200:
        return response.json()
    else:
        raise Exception(f"Failed to list namespaces: {response.status_code}")


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
        location="s3://warehouse-rest/data",
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
        "storage_endpoint": "http://minio1:9001/warehouse-rest",
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
CREATE TABLE {CATALOG_NAME}.`{database_name}.{table_name}` {schema} ENGINE = IcebergS3('http://minio1:9001/warehouse-rest/{table_name}/', '{minio_access_key}', '{minio_secret_key}')
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
            main_configs=[
                "configs/backups.xml",
                "configs/cluster.xml",
                "configs/text_log.xml",
            ],
            user_configs=[],
            stay_alive=True,
            with_iceberg_catalog=True,
            with_zookeeper=True,
        )

        cluster.add_instance(
            "node2",
            main_configs=[
                "configs/backups.xml",
                "configs/cluster.xml",
                "configs/text_log.xml",
            ],
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
    for namespace_list in list_namespaces(started_cluster)["namespaces"]:
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


def test_check_database(started_cluster):
    node = started_cluster.instances["node1"]

    root_namespace = f"clickhouse_{uuid.uuid4()}"
    namespace_1 = f"{root_namespace}.testA.A"
    namespace_2 = f"{root_namespace}.testB.B"
    namespace_1_tables = ["tableA", "tableB"]
    namespace_2_tables = ["tableC", "tableD"]

    catalog = load_catalog_impl(started_cluster)

    for namespace in [namespace_1, namespace_2]:
        catalog.create_namespace(namespace)

    for namespace in [namespace_1, namespace_2]:
        assert len(catalog.list_tables(namespace)) == 0

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

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

    node.query(
        f"CHECK DATABASE {CATALOG_NAME}"
    )

    try:
        node.query(
            "SYSTEM ENABLE FAILPOINT check_database_datalake_negative"
        )
    
        assert "fault when checking database" in node.query_and_get_error(
            f"CHECK DATABASE {CATALOG_NAME}"
        )
    finally:
        node.query(
            "SYSTEM DISABLE FAILPOINT check_database_datalake_negative"
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
        node.query(
            # Regression test: a session temp table used to be pinned by the query context
            # captured in the S3 client refresher and cached with the manifest file in the
            # global IcebergMetadataFilesCache, crashing the graceful restart below with a
            # use-after-free. The SELECT * is required: it reads a manifest file (count()
            # alone is served from the snapshot summary). All statements must stay in one
            # node.query call = one session.
            f"CREATE TEMPORARY TABLE pin_me (x UInt8) ENGINE = Memory;"
            f"SELECT * FROM {CATALOG_NAME}.`{namespace}.{table_name}` FORMAT Null;"
            f"SELECT count() FROM {CATALOG_NAME}.`{namespace}.{table_name}`"
        )
    )

    assert int(node.query(f"SELECT count() FROM system.iceberg_history WHERE table = '{namespace}.{table_name}' and database = '{CATALOG_NAME}'").strip()) == 1

    # Replays the graceful shutdown; the teardown sanitizer check catches the UAF if it regresses.
    node.restart_clickhouse()


@pytest.mark.parametrize("engine", AVAILABLE_ENGINES)
def test_hide_sensitive_info(started_cluster, engine):
    node = started_cluster.instances["node1"]

    test_ref = f"test_hide_sensitive_info_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    namespace = f"{root_namespace}.A"
    catalog = load_catalog_impl(started_cluster)
    catalog.create_namespace(namespace)

    create_table(catalog, namespace, table_name)

    def check_secret_hidden(secret, additional_settings):
        settings = {
            "catalog_type": "rest",
            "warehouse": "demo",
            "storage_endpoint": "http://minio1:9001/warehouse-rest",
        }
        settings.update(additional_settings)

        node.query(f"DROP DATABASE IF EXISTS {CATALOG_NAME}")
        try:
            node.query(
                f"""CREATE DATABASE {CATALOG_NAME} ENGINE = {engine}('{BASE_URL}', 'minio', '{minio_secret_key}')
SETTINGS {",".join((k + "=" + repr(v) for k, v in settings.items()))}""",
                settings={
                    "allow_database_iceberg": 1,
                    "write_full_path_in_iceberg_metadata": 1,
                },
            )
        except QueryRuntimeException as e:
            assert secret not in str(e), (
                f"Secret {secret!r} leaked into CREATE DATABASE error message"
            )
            return

        assert secret not in node.query(f"SHOW CREATE DATABASE {CATALOG_NAME}")

    check_secret_hidden("SECRET_1", {"catalog_credential": "id:SECRET_1"})
    check_secret_hidden("SECRET_2", {"auth_header": "Authorization: SECRET_2"})


def test_no_secrets_in_logs(started_cluster):
    node = started_cluster.instances["node1"]

    db_name = f"iceberg_query_log_{uuid.uuid4().hex}"
    root_namespace = f"log_check_ns_{uuid.uuid4().hex}"
    table_name = f"log_check_tbl_{uuid.uuid4().hex}"

    catalog = load_catalog_impl(started_cluster)
    catalog.create_namespace(root_namespace)

    db_settings = {
        "catalog_type": "rest",
        "warehouse": "demo",
        "storage_endpoint": "http://minio1:9001/warehouse-rest",
    }
    qid_db = uuid.uuid4().hex
    node.query(f"DROP DATABASE IF EXISTS {db_name}")
    node.query(
        f"""CREATE DATABASE {db_name} ENGINE = DataLakeCatalog('{BASE_URL}', 'minio', '{minio_secret_key}')
SETTINGS {",".join((k + "=" + repr(v) for k, v in db_settings.items()))}""",
        query_id=qid_db,
        settings={
            "allow_database_iceberg": 1,
            "write_full_path_in_iceberg_metadata": 1,
        },
    )

    qid_table = uuid.uuid4().hex
    node.query(
        f"""CREATE TABLE {db_name}.`{root_namespace}.{table_name}` (x String) ENGINE = IcebergS3('http://minio1:9001/warehouse-rest/{table_name}/', '{minio_access_key}', '{minio_secret_key}')""",
        query_id=qid_table,
        settings={
            "allow_experimental_database_iceberg": 1,
            "write_full_path_in_iceberg_metadata": 1,
        },
    )

    qid_show_db = uuid.uuid4().hex
    show_db_result = node.query(
        f"SHOW CREATE DATABASE {db_name}", query_id=qid_show_db
    )
    assert minio_secret_key not in show_db_result
    assert "[HIDDEN]" in show_db_result

    qid_show_table = uuid.uuid4().hex
    show_table_result = node.query(
        f"SHOW CREATE TABLE {db_name}.`{root_namespace}.{table_name}`",
        query_id=qid_show_table,
    )
    assert minio_secret_key not in show_table_result
    assert "[HIDDEN]" in show_table_result

    node.query("SYSTEM FLUSH LOGS system.query_log")
    node.query("SYSTEM FLUSH LOGS system.text_log")

    for qid in (qid_db, qid_table, qid_show_db, qid_show_table):
        assert (
            int(
                node.query(
                    f"SELECT count() FROM system.query_log WHERE query_id = '{qid}' AND type = 'QueryFinish'"
                ).strip()
            )
            >= 1
        )
        query_text = node.query(
            f"SELECT arrayStringConcat(groupArray(query), '\\n') FROM system.query_log WHERE query_id = '{qid}' AND type = 'QueryFinish'"
        ).strip()
        assert minio_secret_key not in query_text

    text_log_rows = node.query(
        f"""
SELECT message, value1, value2, value3, value4, value5, value6, value7, value8, value9, value10
FROM system.text_log
WHERE query_id IN ('{qid_db}', '{qid_table}', '{qid_show_db}', '{qid_show_table}')
FORMAT JSONEachRow
"""
    ).strip()
    assert text_log_rows
    for line in text_log_rows.split("\n"):
        row = json.loads(line)
        for val in row.values():
            if isinstance(val, str):
                assert minio_secret_key not in val


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
        == "CREATE DATABASE backup_database\\nENGINE = DataLakeCatalog(\\'http://rest:8181/v1\\', \\'minio\\', \\'[HIDDEN]\\')\\nSETTINGS catalog_type = \\'rest\\', warehouse = \\'demo\\', storage_endpoint = \\'http://minio1:9001/warehouse-rest\\'\n"
    )


def test_restore_database_replace_external_to_null(started_cluster):
    node = started_cluster.instances["node1"]
    db_name = "backup_database_null"
    create_clickhouse_iceberg_database(started_cluster, node, db_name)

    backup_id = uuid.uuid4().hex
    backup_name = f"File('/backups/test_backup_{backup_id}/')"

    node.query(f"BACKUP DATABASE {db_name} TO {backup_name}")
    node.query(f"DROP DATABASE {db_name} SYNC")
    assert db_name not in node.query("SHOW DATABASES")

    node.query(
        f"RESTORE DATABASE {db_name} FROM {backup_name}",
        settings={
            "restore_replace_external_engines_to_null": 1,
            "restore_replace_external_table_functions_to_null": 1,
            "restore_replace_external_dictionary_source_to_null": 1,
        },
    )
    assert db_name not in node.query("SHOW DATABASES")


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

    assert node.query(f"SHOW CREATE TABLE {CATALOG_NAME}.`{root_namespace}.{table_name}`") == f"CREATE TABLE {CATALOG_NAME}.`{root_namespace}.{table_name}`\\n(\\n    `timestamp` Nullable(DateTime64(6)),\\n    `timestamptz` Nullable(DateTime64(6, \\'UTC\\'))\\n)\\nENGINE = Iceberg(\\'http://minio1:9001/warehouse-rest/data/\\', \\'minio\\', \\'[HIDDEN]\\')\n"
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

    assert node.query(f"SHOW CREATE TABLE {CATALOG_NAME}.`{root_namespace}.{table_name}` SETTINGS iceberg_timezone_for_timestamptz='UTC'") == f"CREATE TABLE {CATALOG_NAME}.`{root_namespace}.{table_name}`\\n(\\n    `timestamp` Nullable(DateTime64(6)),\\n    `timestamptz` Nullable(DateTime64(6, \\'UTC\\'))\\n)\\nENGINE = Iceberg(\\'http://minio1:9001/warehouse-rest/data/\\', \\'minio\\', \\'[HIDDEN]\\')\n"
    assert node.query(f"SHOW CREATE TABLE {CATALOG_NAME}.`{root_namespace}.{table_name}` SETTINGS iceberg_timezone_for_timestamptz='Europe/Berlin'") == f"CREATE TABLE {CATALOG_NAME}.`{root_namespace}.{table_name}`\\n(\\n    `timestamp` Nullable(DateTime64(6)),\\n    `timestamptz` Nullable(DateTime64(6, \\'Europe/Berlin\\'))\\n)\\nENGINE = Iceberg(\\'http://minio1:9001/warehouse-rest/data/\\', \\'minio\\', \\'[HIDDEN]\\')\n"

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


@pytest.mark.parametrize(
    "fields_to_remove",
    [
        ["snapshots"],
        ["metadata-log"],
        ["snapshot-log"],
        ["snapshots", "metadata-log", "snapshot-log"],
    ],
)
def test_insert_into_table_without_optional_metadata_arrays(started_cluster, fields_to_remove):
    # The Iceberg spec marks snapshots / metadata-log / snapshot-log as optional, so external
    # engines may create empty-table metadata that omits any of them. Inserting into such a table
    # must still succeed instead of aborting in the metadata write path.
    node = started_cluster.instances["node1"]

    test_ref = f"test_insert_no_optional_arrays_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    catalog = load_catalog_impl(started_cluster)
    catalog.create_namespace(root_namespace)
    create_table(catalog, root_namespace, table_name, DEFAULT_SCHEMA, PartitionSpec(), DEFAULT_SORT_ORDER)

    iceberg_table = catalog.load_table(f"{root_namespace}.{table_name}")
    assert iceberg_table.metadata_location.startswith("s3://")
    metadata_bucket, metadata_key = iceberg_table.metadata_location[len("s3://"):].split("/", 1)
    metadata = json.loads(get_file_contents(started_cluster.minio_client, metadata_bucket, metadata_key))
    for field in fields_to_remove:
        metadata.pop(field, None)
    metadata_bytes = json.dumps(metadata).encode()
    started_cluster.minio_client.put_object(
        metadata_bucket,
        metadata_key,
        io.BytesIO(metadata_bytes),
        len(metadata_bytes),
        content_type="application/json",
    )

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)
    node.query(
        f"INSERT INTO {CATALOG_NAME}.`{root_namespace}.{table_name}` VALUES (NULL, 'AAPL', 193.24, 193.31, tuple('bot'));",
        settings={"allow_insert_into_iceberg": 1, "write_full_path_in_iceberg_metadata": 1},
    )
    assert node.query(f"SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`") == "\\N\tAAPL\t193.24\t193.31\t('bot')\n"


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

    load_catalog_impl(started_cluster)
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
                """
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

    assert node2.query(f"SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`", settings={"parallel_replicas_for_cluster_engines": 1, "enable_parallel_replicas": 2, "cluster_for_parallel_replicas": "cluster_simple"}) == 'pablo\n'


def test_on_cluster_ddl_rejected_for_datalake_catalog(started_cluster):
    # ON CLUSTER DDL against a shared DataLakeCatalog must be rejected on the worker as well as the initiator,
    # because an initiator that lacks the catalog database locally cannot detect it and enqueues the query anyway.
    node1 = started_cluster.instances["node1"]
    node2 = started_cluster.instances["node2"]

    test_ref = f"test_on_cluster_ban_{uuid.uuid4().hex}"
    table_name = f"{test_ref}_table"
    namespace = f"{test_ref}_namespace"
    qualified = f"{CATALOG_NAME}.`{namespace}.{table_name}`"
    engine = (
        f"ENGINE = IcebergS3('http://minio1:9001/warehouse-rest/{table_name}/', "
        f"'{minio_access_key}', '{minio_secret_key}')"
    )
    ddl_settings = {
        "allow_experimental_database_iceberg": 1,
        "allow_database_iceberg": 1,
        "write_full_path_in_iceberg_metadata": 1,
        "distributed_ddl_output_mode": "throw",
    }

    # Initiator has the catalog locally: rejected centrally before the query is ever enqueued.
    create_clickhouse_iceberg_database(started_cluster, node1, CATALOG_NAME)
    create_clickhouse_iceberg_database(started_cluster, node2, CATALOG_NAME)
    err = node1.query_and_get_error(
        f"CREATE TABLE {qualified} ON CLUSTER cluster_simple (x String) {engine}",
        settings=ddl_settings,
    )
    assert "ON CLUSTER is not supported for DataLakeCatalog" in err, err

    # Initiator lacks the catalog, but a worker (node1) has it, so only the worker guard can fire. The plain
    # ON CLUSTER control below confirms the query reaches the workers, ruling out an unrelated failure.
    node2.query(f"DROP DATABASE IF EXISTS {CATALOG_NAME}")
    try:
        control_table = f"{test_ref}_control"
        node2.query(
            f"CREATE TABLE default.{control_table} ON CLUSTER cluster_simple (x Int32) ENGINE = MergeTree ORDER BY x",
            settings={"distributed_ddl_output_mode": "throw"},
        )
        assert node1.query(f"EXISTS TABLE default.{control_table}") == "1\n"
        node2.query(
            f"DROP TABLE default.{control_table} ON CLUSTER cluster_simple SYNC",
            settings={"distributed_ddl_output_mode": "throw"},
        )

        node2.query_and_get_error(
            f"CREATE TABLE {qualified} ON CLUSTER cluster_simple (x String) {engine}",
            settings=ddl_settings,
        )

        # The shared catalog must be untouched: without the worker guard node1 would have created the table here.
        catalog = load_catalog_impl(started_cluster)
        existing_namespaces = {".".join(ns) for ns in catalog.list_namespaces()}
        if namespace in existing_namespaces:
            tables = {ident[-1] for ident in catalog.list_tables(namespace)}
            assert table_name not in tables, f"table must not have been created in the shared catalog: {tables}"
    finally:
        # Restore the catalog database on node2 for subsequent tests, even if an assertion above failed.
        create_clickhouse_iceberg_database(started_cluster, node2, CATALOG_NAME)


def test_used_storages_in_query_log(started_cluster):
    node1 = started_cluster.instances["node1"]
    node2 = started_cluster.instances["node2"]

    test_ref = f"test_query_log_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    load_catalog_impl(started_cluster)
    create_clickhouse_iceberg_database(started_cluster, node1, CATALOG_NAME)
    create_clickhouse_iceberg_database(started_cluster, node2, CATALOG_NAME)
    create_clickhouse_iceberg_table(
        started_cluster, node1, root_namespace, table_name, "(x String)"
    )
    node1.query(
        f"INSERT INTO {CATALOG_NAME}.`{root_namespace}.{table_name}` VALUES ('test_log');",
        settings={
            "allow_insert_into_iceberg": 1,
            "write_full_path_in_iceberg_metadata": 1,
        },
    )

    query_id_non_cluster = uuid.uuid4().hex
    node1.query(
        f"SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`",
        query_id=query_id_non_cluster,
    )

    query_id_cluster = uuid.uuid4().hex
    node1.query(
        f"SELECT * FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`"
        f" SETTINGS parallel_replicas_for_cluster_engines=1,"
        f" enable_parallel_replicas=2,"
        f" cluster_for_parallel_replicas='cluster_simple'",
        query_id=query_id_cluster,
    )

    node1.query("SYSTEM FLUSH LOGS")

    result_non_cluster = node1.query(
        f"SELECT used_storages FROM system.query_log"
        f" WHERE query_id = '{query_id_non_cluster}' AND type = 'QueryFinish'"
    ).strip()
    assert (
        "'IcebergS3'" in result_non_cluster
    ), f"Non-cluster: expected IcebergS3 in used_storages, got {result_non_cluster}"

    result_cluster = node1.query(
        f"SELECT used_storages FROM system.query_log"
        f" WHERE query_id = '{query_id_cluster}' AND type = 'QueryFinish'"
    ).strip()
    assert (
        "'IcebergS3'" in result_cluster
    ), f"Cluster: expected IcebergS3 in used_storages, got {result_cluster}"


def test_not_specified_catalog_type(started_cluster):
    node = started_cluster.instances["node1"]
    settings = {
        "warehouse": "demo",
        "storage_endpoint": "http://minio1:9001/warehouse-rest",
    }

    node.query(f"DROP DATABASE IF EXISTS {CATALOG_NAME}")

    with pytest.raises(QueryRuntimeException) as exc_info:
        node.query(
            f"""CREATE DATABASE {CATALOG_NAME} ENGINE = DataLakeCatalog('{BASE_URL}', 'minio', '{minio_secret_key}')
SETTINGS {",".join((k + "=" + repr(v) for k, v in settings.items()))}""",
            settings={
                "allow_database_iceberg": 1,
                "write_full_path_in_iceberg_metadata": 1,
            },
        )
    message = str(exc_info.value)
    assert "Unspecified catalog type" in message, message
    assert "Code: 36" in message, message


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


def test_create_table_as(started_cluster):
    node = started_cluster.instances["node1"]

    namespace = "test_ctas_ns"
    src_table = "src_ctas"
    catalog = load_catalog_impl(started_cluster)

    create_clickhouse_iceberg_database(
        started_cluster,
        node,
        CATALOG_NAME,
        additional_settings={"default_base_location": "s3://warehouse-rest/data"},
    )

    node.query(f"DROP TABLE IF EXISTS default.{src_table}")
    for table in ["from_as", "override"]:
        node.query(
            f"DROP TABLE IF EXISTS {CATALOG_NAME}.`{namespace}.{table}` SETTINGS allow_database_iceberg=1"
        )

    # Intentional: CTAS must work from a source with a functional partition key.
    node.query(
        f"""
        CREATE TABLE default.{src_table}
        (
            id Int64,
            name String,
            dt Date
        )
        ENGINE = MergeTree
        PARTITION BY toYearNumSinceEpoch(dt)
        ORDER BY (id, name)
    """
    )

    node.query(
        f"""
        CREATE TABLE {CATALOG_NAME}.`{namespace}.from_as`
        AS default.{src_table} SETTINGS allow_database_iceberg=1;
    """
    )

    # CTAS with explicit PARTITION BY / ORDER BY overriding the source table.
    node.query(
        f"""
        CREATE TABLE {CATALOG_NAME}.`{namespace}.override`
        AS default.{src_table}
        PARTITION BY id
        ORDER BY name
        SETTINGS allow_database_iceberg=1;
    """
    )

    tables = catalog.list_tables(namespace)
    table_names = [t[1] for t in tables]
    assert "from_as" in table_names
    assert "override" in table_names

    tbl = catalog.load_table(f"{namespace}.from_as")
    col_names = [f.name for f in tbl.schema().fields]
    assert col_names == ["id", "name", "dt"]

    tbl = catalog.load_table(f"{namespace}.override")
    assert len(tbl.spec().fields) == 1
    assert tbl.spec().fields[0].name == "id"
    assert str(tbl.spec().fields[0].transform) == "identity"

    col_names = [f.name for f in tbl.schema().fields]
    assert col_names == ["id", "name", "dt"]

    # Unsupported storage clauses written explicitly on CREATE ... AS must be rejected, not silently dropped
    # when the storage is rebuilt to keep only PARTITION BY / ORDER BY.
    for clause in [
        "PRIMARY KEY id",
        "SAMPLE BY id",
        "TTL toDate('2099-01-01')",
        "UNIQUE KEY id",
        "SETTINGS index_granularity = 8192",
    ]:
        err = node.query_and_get_error(
            f"CREATE TABLE {CATALOG_NAME}.`{namespace}.ctas_unsupp` "
            f"AS default.{src_table} {clause}",
            settings={"allow_database_iceberg": 1},
        )
        assert "supports only PARTITION BY and ORDER BY" in err

    for table in ["from_as", "override"]:
        node.query(
            f"DROP TABLE {CATALOG_NAME}.`{namespace}.{table}` SETTINGS allow_database_iceberg=1"
        )
    node.query(f"DROP TABLE default.{src_table}")


def test_create_table_as_rejects_column_modifiers(started_cluster):
    node = started_cluster.instances["node1"]

    namespace = "test_ctas_colmod_ns"

    create_clickhouse_iceberg_database(
        started_cluster,
        node,
        CATALOG_NAME,
        additional_settings={"default_base_location": "s3://warehouse-rest/data"},
    )

    # Each source table carries a column modifier that Iceberg tables do not support. CREATE TABLE ... AS
    # must reject it instead of silently creating the Iceberg table with weaker semantics than the source.
    cases = [
        ("comment", "id Int64, name String COMMENT 'the name'"),
        ("codec", "id Int64, name String CODEC(ZSTD)"),
        ("ttl", "id Int64, dt Date, val Int64 TTL dt + INTERVAL 1 DAY"),
    ]
    for src_suffix, cols in cases:
        src_table = f"src_colmod_{src_suffix}"
        node.query(f"DROP TABLE IF EXISTS default.{src_table}")
        node.query(
            f"CREATE TABLE default.{src_table} ({cols}) ENGINE = MergeTree ORDER BY id"
        )
        err = node.query_and_get_error(
            f"CREATE TABLE {CATALOG_NAME}.`{namespace}.dst_{src_suffix}` "
            f"AS default.{src_table} SETTINGS allow_database_iceberg = 1"
        )
        assert "COMMENT, CODEC, TTL, STATISTICS, SETTINGS, and PRIMARY KEY are not supported" in err
        node.query(f"DROP TABLE default.{src_table}")


def test_create_table_explicit_columns(started_cluster):
    node = started_cluster.instances["node1"]

    namespace = "test_ctex_ns"
    catalog = load_catalog_impl(started_cluster)

    create_clickhouse_iceberg_database(
        started_cluster,
        node,
        CATALOG_NAME,
        additional_settings={"default_base_location": "s3://warehouse-rest/data"},
    )

    node.query(
        f"DROP TABLE IF EXISTS {CATALOG_NAME}.`{namespace}.explicit` SETTINGS allow_database_iceberg=1"
    )

    node.query(
        f"""
        CREATE TABLE {CATALOG_NAME}.`{namespace}.explicit`
        (
            id Int64,
            name String,
            value Float64
        )
        PARTITION BY id
        ORDER BY name
        SETTINGS allow_database_iceberg=1;
    """
    )

    tables = catalog.list_tables(namespace)
    table_names = [t[1] for t in tables]
    assert "explicit" in table_names

    tbl = catalog.load_table(f"{namespace}.explicit")
    col_names = [f.name for f in tbl.schema().fields]
    assert col_names == ["id", "name", "value"]

    iceberg_types = {f.name: str(f.field_type) for f in tbl.schema().fields}
    assert iceberg_types["id"] == "long"
    assert iceberg_types["name"] == "string"
    assert iceberg_types["value"] == "double"

    node.query(
        f"INSERT INTO {CATALOG_NAME}.`{namespace}.explicit` VALUES (1, 'a', 1.5);",
        settings={
            "allow_insert_into_iceberg": 1,
            "write_full_path_in_iceberg_metadata": 1,
        },
    )
    assert (
        node.query(
            f"SELECT id, name, value FROM {CATALOG_NAME}.`{namespace}.explicit`"
        )
        == "1\ta\t1.5\n"
    )

    node.query(
        f"DROP TABLE {CATALOG_NAME}.`{namespace}.explicit` SETTINGS allow_database_iceberg=1"
    )


def test_create_table_nested_namespace(started_cluster):
    node = started_cluster.instances["node1"]

    namespace = "test_nested_ns.a.b"
    catalog = load_catalog_impl(started_cluster)

    create_clickhouse_iceberg_database(
        started_cluster,
        node,
        CATALOG_NAME,
        additional_settings={"default_base_location": "s3://warehouse-rest/data"},
    )

    node.query(
        f"DROP TABLE IF EXISTS {CATALOG_NAME}.`{namespace}.nested` SETTINGS allow_database_iceberg=1"
    )
    node.query(
        f"""
        CREATE TABLE {CATALOG_NAME}.`{namespace}.nested`
        (
            id Int64
        )
        SETTINGS allow_database_iceberg=1;
        """
    )

    tables = catalog.list_tables(namespace)
    table_names = [t[-1] for t in tables]
    assert "nested" in table_names

    write_settings = {"allow_insert_into_iceberg": 1, "write_full_path_in_iceberg_metadata": 1}

    # INSERT exercises RestCatalog::updateMetadata for the nested namespace.
    node.query(
        f"INSERT INTO {CATALOG_NAME}.`{namespace}.nested` VALUES (1);",
        settings=write_settings,
    )
    assert node.query(
        f"SELECT id FROM {CATALOG_NAME}.`{namespace}.nested`",
        settings={"allow_database_iceberg": 1},
    ).strip() == "1"

    # ALTER exercises RestCatalog::updateSchema for the nested namespace.
    node.query(
        f"ALTER TABLE {CATALOG_NAME}.`{namespace}.nested` ADD COLUMN z Nullable(String);",
        settings=write_settings,
    )
    assert "z" in node.query(
        f"DESCRIBE TABLE {CATALOG_NAME}.`{namespace}.nested`",
        settings=write_settings,
    )

    node.query(
        f"DROP TABLE {CATALOG_NAME}.`{namespace}.nested` SETTINGS allow_database_iceberg=1"
    )


def test_create_non_table_rejected(started_cluster):
    node = started_cluster.instances["node1"]

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

    # Each DDL flips a distinct flag in the interpreter's "plain CREATE TABLE only" guard.
    for ddl in [
        f"CREATE VIEW {CATALOG_NAME}.`ns.v` AS SELECT 1",
        f"CREATE MATERIALIZED VIEW {CATALOG_NAME}.`ns.mv` ENGINE = Memory AS SELECT 1",
        f"ATTACH TABLE {CATALOG_NAME}.`ns.attached` (x Int32) ENGINE = Memory",
        f"CREATE OR REPLACE TABLE {CATALOG_NAME}.`ns.replaced` (x Int32)",
    ]:
        err = node.query_and_get_error(ddl, settings={"allow_database_iceberg": 1})
        assert "supports only plain CREATE TABLE" in err

    node.query("DROP TABLE IF EXISTS default.src_clone")
    node.query(
        "CREATE TABLE default.src_clone (x Int32) ENGINE = MergeTree ORDER BY x"
    )
    err = node.query_and_get_error(
        f"CREATE TABLE {CATALOG_NAME}.`ns.cloned` CLONE AS default.src_clone",
        settings={"allow_database_iceberg": 1},
    )
    assert "supports only plain CREATE TABLE" in err
    node.query("DROP TABLE default.src_clone")

    err = node.query_and_get_error(
        f"CREATE TABLE {CATALOG_NAME}.`ns.mem` (x Int32) ENGINE = Memory",
        settings={"allow_database_iceberg": 1},
    )
    assert "only supports Iceberg-family table engines" in err


def test_create_table_unsupported_clauses(started_cluster):
    node = started_cluster.instances["node1"]

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

    base_ddl = f"CREATE TABLE {CATALOG_NAME}.`ns.unsupp` (id Int64, name String)"
    for clause in [
        "PRIMARY KEY id ORDER BY id",
        "ORDER BY id SAMPLE BY id",
        "ORDER BY id TTL toDate('2099-01-01')",
        "ORDER BY id SETTINGS index_granularity = 8192",
    ]:
        err = node.query_and_get_error(
            f"{base_ddl} {clause}",
            settings={"allow_database_iceberg": 1},
        )
        assert "supports only PARTITION BY and ORDER BY" in err

    # The table COMMENT is not persisted anywhere, so it is rejected by a separate check.
    err = node.query_and_get_error(
        f"{base_ddl} ORDER BY id COMMENT 'tbl comment'",
        settings={"allow_database_iceberg": 1},
    )
    assert "Table COMMENT is not supported" in err

    for table_element in [
        "INDEX idx_name name TYPE bloom_filter GRANULARITY 1",
        "PROJECTION p (SELECT id ORDER BY name)",
        "CONSTRAINT c CHECK id > 0",
    ]:
        err = node.query_and_get_error(
            f"CREATE TABLE {CATALOG_NAME}.`ns.unsupp_elem` (id Int64, name String, {table_element}) ORDER BY id",
            settings={"allow_database_iceberg": 1},
        )
        assert "does not support PRIMARY KEY, indices" in err

    # Column-level PRIMARY KEY is normalized into the storage-level clause, covered above.
    for col_clause in [
        "(id Int64 COMMENT 'pk', name String)",
        "(id Int64, name String CODEC(ZSTD))",
        "(id Int64, dt Date TTL dt + INTERVAL 1 DAY)",
        "(id Int64, name String SETTINGS (max_compress_block_size = 1))",
    ]:
        err = node.query_and_get_error(
            f"CREATE TABLE {CATALOG_NAME}.`ns.unsupp_col` {col_clause} ORDER BY id",
            settings={"allow_database_iceberg": 1},
        )
        assert "COMMENT, CODEC, TTL, STATISTICS, SETTINGS, and PRIMARY KEY are not supported" in err

    for col_clause in [
        "(id Int64, d Int64 DEFAULT 1)",
        "(id Int64, d Int64 MATERIALIZED id + 1)",
    ]:
        err = node.query_and_get_error(
            f"CREATE TABLE {CATALOG_NAME}.`ns.unsupp_def` {col_clause} ORDER BY id",
            settings={"allow_database_iceberg": 1},
        )
        assert "is not yet supported" in err


def test_create_table_with_engine_unsupported_clauses(started_cluster):
    node = started_cluster.instances["node1"]

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

    engine = (
        f"ENGINE = IcebergS3('http://minio1:9001/warehouse-rest/engine_unsupp/', "
        f"'{minio_access_key}', '{minio_secret_key}')"
    )

    # The explicit-ENGINE path persists only column names/types, PARTITION BY, and ORDER BY into the
    # initial Iceberg metadata, so unsupported storage clauses must be rejected there too, not silently
    # dropped. Unlike the engine-less path, engine SETTINGS stay allowed: they are real data-lake
    # storage settings (e.g. `iceberg_format_version`) used during creation.
    for clause in [
        "PRIMARY KEY id",
        "ORDER BY id SAMPLE BY id",
        "TTL toDate('2099-01-01')",
    ]:
        err = node.query_and_get_error(
            f"CREATE TABLE {CATALOG_NAME}.`ns.engine_unsupp` (id Int64, name String) {engine} {clause}",
            settings={"allow_database_iceberg": 1},
        )
        assert "PRIMARY KEY, SAMPLE BY, TTL, and UNIQUE KEY are not supported" in err

    for table_element in [
        "INDEX idx_name name TYPE bloom_filter GRANULARITY 1",
        "PROJECTION p (SELECT id ORDER BY name)",
        "CONSTRAINT c CHECK id > 0",
    ]:
        err = node.query_and_get_error(
            f"CREATE TABLE {CATALOG_NAME}.`ns.engine_unsupp` (id Int64, name String, {table_element}) {engine}",
            settings={"allow_database_iceberg": 1},
        )
        assert "does not support PRIMARY KEY, indices" in err

    for col_clause in [
        "(id Int64 COMMENT 'pk', name String)",
        "(id Int64, name String CODEC(ZSTD))",
        "(id Int64, dt Date TTL dt + INTERVAL 1 DAY)",
        "(id Int64, name String SETTINGS (max_compress_block_size = 1))",
    ]:
        err = node.query_and_get_error(
            f"CREATE TABLE {CATALOG_NAME}.`ns.engine_unsupp` {col_clause} {engine}",
            settings={"allow_database_iceberg": 1},
        )
        assert "COMMENT, CODEC, TTL, STATISTICS, SETTINGS, and PRIMARY KEY are not supported" in err

    for col_clause in [
        "(id Int64, d Int64 DEFAULT 1)",
        "(id Int64, d Int64 MATERIALIZED id + 1)",
    ]:
        err = node.query_and_get_error(
            f"CREATE TABLE {CATALOG_NAME}.`ns.engine_unsupp` {col_clause} {engine}",
            settings={"allow_database_iceberg": 1},
        )
        assert "is not yet supported" in err

    # Modifiers inherited via CREATE TABLE ... AS are rejected on the explicit-ENGINE path too.
    node.query("DROP TABLE IF EXISTS default.src_engine_colmod")
    node.query(
        "CREATE TABLE default.src_engine_colmod (id Int64, name String COMMENT 'the name') "
        "ENGINE = MergeTree ORDER BY id"
    )
    err = node.query_and_get_error(
        f"CREATE TABLE {CATALOG_NAME}.`ns.engine_unsupp` AS default.src_engine_colmod {engine}",
        settings={"allow_database_iceberg": 1},
    )
    assert "COMMENT, CODEC, TTL, STATISTICS, SETTINGS, and PRIMARY KEY are not supported" in err
    node.query("DROP TABLE default.src_engine_colmod")

    # Positive control: PARTITION BY, ORDER BY, and engine SETTINGS remain supported with an explicit engine.
    namespace = f"test_engine_supp_{uuid.uuid4().hex}"
    node.query(
        f"CREATE TABLE {CATALOG_NAME}.`{namespace}.engine_supp` (id Int64, name String) "
        f"ENGINE = IcebergS3('http://minio1:9001/warehouse-rest/engine_supp/', "
        f"'{minio_access_key}', '{minio_secret_key}') "
        f"PARTITION BY id ORDER BY name SETTINGS iceberg_format_version = 2",
        settings={
            "allow_database_iceberg": 1,
            "write_full_path_in_iceberg_metadata": 1,
        },
    )
    node.query(
        f"DROP TABLE {CATALOG_NAME}.`{namespace}.engine_supp`",
        settings={"allow_database_iceberg": 1},
    )


def test_create_table_invalid_partition_transforms(started_cluster):
    node = started_cluster.instances["node1"]

    namespace = "test_invalid_part_ns"
    catalog = load_catalog_impl(started_cluster)

    create_clickhouse_iceberg_database(
        started_cluster,
        node,
        CATALOG_NAME,
        additional_settings={"default_base_location": "s3://warehouse-rest/data"},
    )

    # A valid positive transform is accepted (and creates the namespace).
    node.query(
        f"DROP TABLE IF EXISTS {CATALOG_NAME}.`{namespace}.good` SETTINGS allow_database_iceberg=1"
    )
    node.query(
        f"""
        CREATE TABLE {CATALOG_NAME}.`{namespace}.good`
        (
            id Int64
        )
        PARTITION BY icebergBucket(8, id)
        SETTINGS allow_database_iceberg=1;
        """
    )
    assert "good" in [t[1] for t in catalog.list_tables(namespace)]

    # Invalid transform parameters must be rejected before any catalog metadata is written.
    # Otherwise CREATE TABLE could register an unreadable Iceberg table whose partition spec
    # serializes as bucket[0], bucket[-1], or truncate[0].
    for i, transform in enumerate(
        ["icebergBucket(0, id)", "icebergBucket(-1, id)", "icebergTruncate(0, id)"]
    ):
        tbl = f"bad_{i}"
        err = node.query_and_get_error(
            f"CREATE TABLE {CATALOG_NAME}.`{namespace}.{tbl}` (id Int64) "
            f"PARTITION BY {transform} SETTINGS allow_database_iceberg=1"
        )
        assert "requires a positive" in err, err
        assert tbl not in [t[1] for t in catalog.list_tables(namespace)]

    node.query(
        f"DROP TABLE {CATALOG_NAME}.`{namespace}.good` SETTINGS allow_database_iceberg=1"
    )


def test_create_table_namespace_location(started_cluster):
    node = started_cluster.instances["node1"]

    namespace = f"test_ns_location_{uuid.uuid4().hex[:8]}"
    catalog = load_catalog_impl(started_cluster)

    create_clickhouse_iceberg_database(
        started_cluster,
        node,
        CATALOG_NAME,
        additional_settings={"default_base_location": "s3://warehouse-rest/data"},
    )

    node.query(
        f"DROP TABLE IF EXISTS {CATALOG_NAME}.`{namespace}.first` SETTINGS allow_database_iceberg=1"
    )
    node.query(
        f"""
        CREATE TABLE {CATALOG_NAME}.`{namespace}.first`
        (
            id Int64
        )
        SETTINGS allow_database_iceberg=1;
        """
    )

    table_location = catalog.load_table(f"{namespace}.first").location().rstrip("/")
    ns_location = catalog.load_namespace_properties(namespace).get("location")

    # The namespace default location must point at the namespace base, not at the first table's
    # directory. Otherwise another client creating a table in the same namespace without an explicit
    # location could be placed under the first table's path.
    assert ns_location is not None, "namespace is missing its location property"
    ns_location = ns_location.rstrip("/")
    assert ns_location != table_location, (
        f"namespace location {ns_location} must not equal the first table location {table_location}"
    )
    assert table_location.startswith(ns_location + "/"), (ns_location, table_location)
    assert table_location[len(ns_location):].strip("/") == "first", (ns_location, table_location)

    node.query(
        f"DROP TABLE {CATALOG_NAME}.`{namespace}.first` SETTINGS allow_database_iceberg=1"
    )


def test_create_table_with_engine_namespace_location(started_cluster):
    node = started_cluster.instances["node1"]

    namespace = f"test_ns_engine_location_{uuid.uuid4().hex[:8]}"
    table_dir = f"engine_ns_location_{uuid.uuid4().hex[:8]}"
    catalog = load_catalog_impl(started_cluster)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

    node.query(
        f"CREATE TABLE {CATALOG_NAME}.`{namespace}.first` (id Int64) "
        f"ENGINE = IcebergS3('http://minio1:9001/warehouse-rest/{table_dir}/', "
        f"'{minio_access_key}', '{minio_secret_key}')",
        settings={
            "allow_database_iceberg": 1,
            "write_full_path_in_iceberg_metadata": 1,
        },
    )

    table_location = catalog.load_table(f"{namespace}.first").location().rstrip("/")
    assert table_location.endswith(table_dir), table_location

    # The engine path here is `<bucket>/<table_dir>/`, which does not follow `<base>/<namespace>/<table>`,
    # so there is no namespace base to derive. The table's own directory must not be registered as the
    # namespace default location: another client creating a table in the same namespace without an
    # explicit location would then be placed under this table's directory.
    ns_location = catalog.load_namespace_properties(namespace).get("location")
    if ns_location is not None:
        ns_location = ns_location.rstrip("/")
        assert ns_location != table_location, (ns_location, table_location)
        assert not ns_location.startswith(table_location + "/"), (ns_location, table_location)

    node.query(
        f"DROP TABLE {CATALOG_NAME}.`{namespace}.first`",
        settings={"allow_database_iceberg": 1},
    )

    # When the engine path does follow `<base>/<namespace>/<table>`, the namespace base is unambiguous
    # and is registered as the namespace default location, so tables created later by other clients
    # land next to this one instead of inside it.
    base_dir = f"engine_ns_base_{uuid.uuid4().hex[:8]}"
    nested_namespace = f"test_ns_engine_derived_{uuid.uuid4().hex[:8]}"
    node.query(
        f"CREATE TABLE {CATALOG_NAME}.`{nested_namespace}.second` (id Int64) "
        f"ENGINE = IcebergS3('http://minio1:9001/warehouse-rest/{base_dir}/{nested_namespace}/second/', "
        f"'{minio_access_key}', '{minio_secret_key}')",
        settings={
            "allow_database_iceberg": 1,
            "write_full_path_in_iceberg_metadata": 1,
        },
    )

    ns_location = catalog.load_namespace_properties(nested_namespace).get("location")
    assert ns_location is not None, "namespace is missing its location property"
    assert ns_location.rstrip("/") == f"s3://warehouse-rest/{base_dir}/{nested_namespace}", ns_location

    node.query(
        f"DROP TABLE {CATALOG_NAME}.`{nested_namespace}.second`",
        settings={"allow_database_iceberg": 1},
    )


def test_drop_table_purge(started_cluster):
    node = started_cluster.instances["node1"]

    namespace = "test_drop_purge_ns"
    catalog = load_catalog_impl(started_cluster)
    minio_client = Minio(
        f"{started_cluster.minio_ip}:{started_cluster.minio_port}",
        access_key=minio_access_key,
        secret_key=minio_secret_key,
        secure=False,
    )

    create_clickhouse_iceberg_database(
        started_cluster,
        node,
        CATALOG_NAME,
        additional_settings={"default_base_location": "s3://warehouse-rest/data"},
    )

    for table in ["to_keep", "to_purge"]:
        node.query(
            f"DROP TABLE IF EXISTS {CATALOG_NAME}.`{namespace}.{table}` SETTINGS allow_database_iceberg=1"
        )
        node.query(
            f"""
            CREATE TABLE {CATALOG_NAME}.`{namespace}.{table}`
            (
                id Int64
            )
            SETTINGS allow_database_iceberg=1;
        """
        )

    table_names = [t[1] for t in catalog.list_tables(namespace)]
    assert "to_keep" in table_names
    assert "to_purge" in table_names

    def table_prefix(table):
        location = catalog.load_table(f"{namespace}.{table}").location()
        assert location.startswith("s3://warehouse-rest/")
        prefix = location[len("s3://warehouse-rest/"):].rstrip("/") + "/"
        assert list(
            minio_client.list_objects("warehouse-rest", prefix=prefix, recursive=True)
        ), f"Expected metadata under {prefix} before drop"
        return prefix

    keep_prefix = table_prefix("to_keep")
    purge_prefix = table_prefix("to_purge")

    node.query(
        f"DROP TABLE {CATALOG_NAME}.`{namespace}.to_keep` SETTINGS allow_database_iceberg=1"
    )
    node.query(
        f"DROP TABLE {CATALOG_NAME}.`{namespace}.to_purge` SETTINGS allow_database_iceberg=1, data_lake_delete_data_on_drop=1"
    )

    table_names = [t[1] for t in catalog.list_tables(namespace)]
    assert "to_keep" not in table_names
    assert "to_purge" not in table_names

    # A drop without purge unregisters the table from the catalog but must keep its data.
    assert list(
        minio_client.list_objects("warehouse-rest", prefix=keep_prefix, recursive=True)
    ), f"Expected objects under {keep_prefix} to survive a drop without purge"

    remaining = [
        o.object_name
        for o in minio_client.list_objects("warehouse-rest", prefix=purge_prefix, recursive=True)
    ]
    assert not remaining, f"Expected purge to remove objects under {purge_prefix}, found: {remaining}"



def test_create_if_not_exists_with_engine_over_leftover_metadata(started_cluster):
    """
    A drop without `data_lake_delete_data_on_drop` keeps the metadata, so a later
    `CREATE TABLE IF NOT EXISTS ... ENGINE = IcebergS3(...)` over the same path registers nothing and
    must report that nothing was created - otherwise the insert of `... AS SELECT` gets no table.
    """
    node = started_cluster.instances["node1"]
    namespace = f"test_ns_leftover_{uuid.uuid4().hex[:8]}"
    engine = (
        f"IcebergS3('http://minio1:9001/warehouse-rest/{namespace}/t/', "
        f"'{minio_access_key}', '{minio_secret_key}')"
    )
    settings = {
        "allow_database_iceberg": 1,
        "allow_insert_into_iceberg": 1,
        "write_full_path_in_iceberg_metadata": 1,
    }

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)
    node.query(
        f"CREATE TABLE {CATALOG_NAME}.`{namespace}.t` (id Int64) ENGINE = {engine}",
        settings=settings,
    )
    node.query(f"DROP TABLE {CATALOG_NAME}.`{namespace}.t`", settings=settings)

    node.query(
        f"CREATE TABLE IF NOT EXISTS {CATALOG_NAME}.`{namespace}.t` (id Int64) "
        f"ENGINE = {engine} AS SELECT 1 AS id",
        settings=settings,
    )
    assert node.query(f"EXISTS TABLE {CATALOG_NAME}.`{namespace}.t`", settings=settings) == "0\n"


def test_delete_on_lazy_initialized_table(started_cluster):
    """
    Regression test for https://github.com/ClickHouse/ClickHouse/issues/96806.

    Tables in a DataLakeCatalog database use lazy metadata initialization
    (lazy_init=true), meaning the DataLake metadata is not loaded at table
    construction time.  Prior to the fix, running ALTER TABLE ... DELETE (or
    DELETE FROM ...) as the very first operation on such a table -- before any
    SELECT had a chance to trigger metadata initialization -- resulted in a
    LOGICAL_ERROR: 'Metadata is not initialized'.
    """
    node = started_cluster.instances["node1"]

    test_ref = f"test_delete_lazy_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)
    create_clickhouse_iceberg_table(
        started_cluster, node, root_namespace, table_name, "(x String)"
    )

    # Insert rows without any prior SELECT so that metadata starts uninitialized.
    node.query(
        f"INSERT INTO {CATALOG_NAME}.`{root_namespace}.{table_name}` VALUES ('keep');",
        settings={"allow_insert_into_iceberg": 1, "write_full_path_in_iceberg_metadata": 1},
    )
    node.query(
        f"INSERT INTO {CATALOG_NAME}.`{root_namespace}.{table_name}` VALUES ('delete_me');",
        settings={"allow_insert_into_iceberg": 1, "write_full_path_in_iceberg_metadata": 1},
    )

    # Run ALTER TABLE DELETE without a prior SELECT.  This is exactly the query
    # that triggered LOGICAL_ERROR: 'Metadata is not initialized' before the fix.
    node.query(
        f"ALTER TABLE {CATALOG_NAME}.`{root_namespace}.{table_name}` DELETE WHERE x = 'delete_me';",
        settings={"allow_insert_into_iceberg": 1, "write_full_path_in_iceberg_metadata": 1},
    )

    # Also exercise the DELETE FROM syntax (InterpreterDeleteQuery path).
    node.query(
        f"DELETE FROM {CATALOG_NAME}.`{root_namespace}.{table_name}` WHERE x = 'keep';",
        settings={"allow_insert_into_iceberg": 1, "write_full_path_in_iceberg_metadata": 1},
    )

    assert node.query(f"SELECT count() FROM {CATALOG_NAME}.`{root_namespace}.{table_name}`") == "0\n"


def test_writes_schema_evolution(started_cluster):
    node = started_cluster.instances["node1"]

    test_ref = f"test_writes_schema_evolution_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"
    table_ref = f"{CATALOG_NAME}.`{root_namespace}.{table_name}`"
    write_settings = {"allow_insert_into_iceberg": 1, "write_full_path_in_iceberg_metadata": 1}

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)
    create_clickhouse_iceberg_table(started_cluster, node, root_namespace, table_name, "(x String, y Int32)")

    node.query(f"INSERT INTO {table_ref} VALUES ('123', 1);", settings=write_settings)

    node.query(f"ALTER TABLE {table_ref} ADD COLUMN z Nullable(String);", settings=write_settings)
    assert "z" in node.query(f"DESCRIBE TABLE {table_ref}", settings=write_settings)
    assert node.query(f"SELECT x, y, z FROM {table_ref} ORDER BY ALL", settings=write_settings) == "123\t1\t\\N\n"

    node.query(f"INSERT INTO {table_ref} VALUES ('456', 2, 'hello');", settings=write_settings)
    assert (
        node.query(f"SELECT x, y, z FROM {table_ref} ORDER BY ALL", settings=write_settings)
        == "123\t1\t\\N\n456\t2\thello\n"
    )

    node.query(f"ALTER TABLE {table_ref} RENAME COLUMN z TO w;", settings=write_settings)
    assert "w" in node.query(f"DESCRIBE TABLE {table_ref}", settings=write_settings)
    assert (
        node.query(f"SELECT x, y, w FROM {table_ref} ORDER BY ALL", settings=write_settings)
        == "123\t1\t\\N\n456\t2\thello\n"
    )


def test_writes_schema_evolution_drop_last_column(started_cluster):
    """DROP COLUMN of the highest-id column must not be rejected by the catalog.

    Reproducer for the bug where the REST add-schema update omitted
    last-column-id, causing the catalog to derive it from the schema's
    highestFieldId which decreases after dropping the last-added column.
    """
    node = started_cluster.instances["node1"]

    test_ref = f"test_writes_schema_evolution_drop_last_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"
    table_ref = f"{CATALOG_NAME}.`{root_namespace}.{table_name}`"
    write_settings = {"allow_insert_into_iceberg": 1, "write_full_path_in_iceberg_metadata": 1}

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)
    create_clickhouse_iceberg_table(started_cluster, node, root_namespace, table_name, "(x String, y Int32)")

    node.query(f"INSERT INTO {table_ref} VALUES ('abc', 1);", settings=write_settings)

    node.query(f"ALTER TABLE {table_ref} ADD COLUMN z Nullable(String);", settings=write_settings)
    assert "z" in node.query(f"DESCRIBE TABLE {table_ref}", settings=write_settings)

    node.query(f"ALTER TABLE {table_ref} DROP COLUMN z;", settings=write_settings)
    desc = node.query(f"DESCRIBE TABLE {table_ref}", settings=write_settings)
    assert "z" not in desc

    assert node.query(f"SELECT x, y FROM {table_ref} ORDER BY ALL", settings=write_settings) == "abc\t1\n"

    # Add another column after the drop to exercise schema-id allocation when
    # current-schema-id is not the highest in the schemas list (Fix 1 reproducer).
    node.query(f"ALTER TABLE {table_ref} ADD COLUMN w Nullable(Int64);", settings=write_settings)
    desc = node.query(f"DESCRIBE TABLE {table_ref}", settings=write_settings)
    assert "w" in desc
    assert "z" not in desc

    node.query(f"INSERT INTO {table_ref} (x, y, w) VALUES ('def', 2, 42);", settings=write_settings)
    assert node.query(f"SELECT x, y, w FROM {table_ref} ORDER BY x", settings=write_settings) == "abc\t1\t\\N\ndef\t2\t42\n"


def test_writes_alter_when_commit_is_reported_as_failed(started_cluster):
    """An Iceberg commit can land in the catalog while the client observes a failure
    (commit state unknown, e.g. a proxy rewriting the response to 5xx). The ALTER retry
    must notice that the change is already present instead of applying it a second time
    and failing with `Column already exists`.
    """
    node = started_cluster.instances["node1"]

    test_ref = f"test_writes_alter_commit_unknown_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"
    table_ref = f"{CATALOG_NAME}.`{root_namespace}.{table_name}`"
    write_settings = {"allow_insert_into_iceberg": 1, "write_full_path_in_iceberg_metadata": 1}

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)
    create_clickhouse_iceberg_table(started_cluster, node, root_namespace, table_name, "(x String, y Int32)")

    node.query(f"INSERT INTO {table_ref} VALUES ('abc', 1);", settings=write_settings)

    failpoint = "iceberg_alter_catalog_commit_reported_as_failed"
    node.query(f"SYSTEM ENABLE FAILPOINT {failpoint}")
    try:
        node.query(f"ALTER TABLE {table_ref} ADD COLUMN z Nullable(String);", settings=write_settings)
    finally:
        node.query(f"SYSTEM DISABLE FAILPOINT {failpoint}")

    description = node.query(f"DESCRIBE TABLE {table_ref}", settings=write_settings)
    columns = [line.split("\t")[0] for line in description.strip().split("\n")]
    assert columns.count("z") == 1, f"expected exactly one `z` column in:\n{description}"
    assert sorted(columns) == sorted(["x", "y", "z"])

    node.query(f"INSERT INTO {table_ref} VALUES ('def', 2, 'zz');", settings=write_settings)
    assert (
        node.query(f"SELECT x, y, z FROM {table_ref} ORDER BY x", settings=write_settings)
        == "abc\t1\t\\N\ndef\t2\tzz\n"
    )



def test_writes_schema_evolution_concurrent_add_columns(started_cluster):
    node = started_cluster.instances["node1"]

    test_ref = f"test_writes_schema_evolution_concurrent_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"
    table_ref = f"{CATALOG_NAME}.`{root_namespace}.{table_name}`"
    write_settings = {"allow_insert_into_iceberg": 1, "write_full_path_in_iceberg_metadata": 1}

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)
    create_clickhouse_iceberg_table(started_cluster, node, root_namespace, table_name, "(x String, y Int32)")

    node.query(f"INSERT INTO {table_ref} VALUES ('123', 1);", settings=write_settings)

    num_columns = 10

    def add_column(idx):
        node.query(
            f"ALTER TABLE {table_ref} ADD COLUMN col_{idx} Nullable(String);",
            settings=write_settings,
        )

    with ThreadPoolExecutor(max_workers=num_columns) as executor:
        list(executor.map(add_column, range(num_columns)))

    description = node.query(f"DESCRIBE TABLE {table_ref}", settings=write_settings)
    for idx in range(num_columns):
        assert f"col_{idx}" in description, f"col_{idx} missing from:\n{description}"

    columns = [line.split("\t")[0] for line in description.strip().split("\n")]
    assert sorted(columns) == sorted(["x", "y"] + [f"col_{idx}" for idx in range(num_columns)])

    select_cols = ", ".join(["x", "y"] + [f"col_{idx}" for idx in range(num_columns)])
    expected = "123\t1" + "\t\\N" * num_columns + "\n"
    assert node.query(f"SELECT {select_cols} FROM {table_ref} ORDER BY ALL", settings=write_settings) == expected


def test_gcs(started_cluster):
    node = started_cluster.instances["node1"]

    node.query("SYSTEM ENABLE FAILPOINT database_iceberg_gcs")
    node.query(f"DROP DATABASE IF EXISTS {CATALOG_NAME};")

    with pytest.raises(Exception) as err:
        node.query(
            f"""
            CREATE DATABASE {CATALOG_NAME}
            ENGINE = DataLakeCatalog('{BASE_URL}', 'gcs', 'dummy')
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

    node.query(f"CREATE TABLE {CATALOG_NAME}.`{namespace_prefix}alpha.{table2_name}` (x String) ENGINE = IcebergS3('http://minio1:9001/warehouse-rest/{namespace_prefix}alpha/{table2_name}/', '{minio_access_key}', '{minio_secret_key}')",
         settings={
            "allow_database_iceberg": 1,
            "write_full_path_in_iceberg_metadata": 1,
        },
    )
    node.query(f"CREATE TABLE {CATALOG_NAME}.`{namespace_prefix}alpha.a1.{table2_name}` (x String) ENGINE = IcebergS3('http://minio1:9001/warehouse-rest/{namespace_prefix}alpha/a1/{table2_name}/', '{minio_access_key}', '{minio_secret_key}')",
         settings={
            "allow_database_iceberg": 1,
            "write_full_path_in_iceberg_metadata": 1,
        },
    )
    assert "is filtered by `namespaces` database parameter." in node.query_and_get_error(f"CREATE TABLE {CATALOG_NAME}.`{namespace_prefix}alpha.a2.{table2_name}` (x String) ENGINE = IcebergS3('http://minio1:9001/warehouse-rest/{namespace_prefix}alpha/a2/{table2_name}/', '{minio_access_key}', '{minio_secret_key}')")

    node.query(f"DROP TABLE {CATALOG_NAME}.`{namespace_prefix}alpha.{table_name}`")
    node.query(f"DROP TABLE {CATALOG_NAME}.`{namespace_prefix}alpha.a1.{table_name}`")
    assert "is filtered by `namespaces` database parameter." in node.query_and_get_error(f"DROP TABLE {CATALOG_NAME}.`{namespace_prefix}alpha.a2.{table_name}`")


# TODO - turn on after merge alternative syntax
@pytest.mark.parametrize("join_mode", ["local", "global"])
def _test_cluster_joins(started_cluster, join_mode):
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
