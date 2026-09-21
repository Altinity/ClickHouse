import logging
import uuid

import jwt
import pytest
import requests

from helpers.cluster import ClickHouseCluster
from helpers.config_cluster import minio_access_key, minio_secret_key

SECRET = "datalake_token_forwarding_secret"
BASE_URL = "http://rest:8181/v1"
CATALOG_NAME = "demo"
DATABASE_SETTINGS = {
    "catalog_type": "rest",
    "warehouse": "demo",
    "storage_endpoint": "http://minio1:9001/warehouse-rest",
    "catalog_credential": "service:principal",
    "oauth_forward_user_token": 1,
}


def make_token(user):
    return jwt.encode({"sub": user}, SECRET, algorithm="HS256")


@pytest.fixture(scope="module")
def started_cluster():
    cluster = ClickHouseCluster(__file__)
    try:
        cluster.add_instance(
            "node1",
            main_configs=["configs/token_forwarding.xml"],
            user_configs=["configs/users.xml"],
            stay_alive=True,
            with_iceberg_catalog=True,
            extra_parameters={
                "docker_compose_file_name": "docker_compose_iceberg_rest_catalog.yml"
            },
        )
        logging.info("Starting cluster...")
        cluster.start()

        node = cluster.instances["node1"]
        node.query("CREATE ROLE IF NOT EXISTS token_users")
        node.query("GRANT CHECK, DROP TABLE, INSERT, SELECT, SHOW ON *.* TO token_users")
        node.query("GRANT S3 ON *.* TO token_users")

        yield cluster
    finally:
        cluster.shutdown()


def catalog_local_url(started_cluster):
    return f"http://localhost:{started_cluster.iceberg_rest_catalog_port}/v1"


def create_namespace(started_cluster, namespace):
    response = requests.post(
        f"{catalog_local_url(started_cluster)}/namespaces",
        json={"namespace": [namespace], "properties": {}},
        timeout=30,
    )
    assert response.status_code in (200, 409), response.text


def query_with_token(node, token, sql, **kwargs):
    response = node.http_request(
        "",
        method="POST",
        data=sql,
        headers={"Authorization": f"Bearer {token}"},
        **kwargs,
    )
    response.raise_for_status()
    return response.text


def create_database(node, name, settings, storage_credentials=False):
    arguments = f"'{BASE_URL}'"
    if storage_credentials:
        arguments += f", '{minio_access_key}', '{minio_secret_key}'"
    node.query(f"DROP DATABASE IF EXISTS {name}")
    node.query(
        f"SET allow_experimental_database_iceberg=true;"
        f"CREATE DATABASE {name} ENGINE = DataLakeCatalog({arguments}) "
        f"SETTINGS {','.join(k + '=' + repr(v) for k, v in settings.items())}"
    )


def create_table_in_catalog(started_cluster, namespace, table):
    response = requests.post(
        f"{catalog_local_url(started_cluster)}/namespaces/{namespace}/tables",
        json={
            "name": table,
            "location": f"s3://warehouse-rest/{table}",
            "schema": {
                "type": "struct",
                "schema-id": 0,
                "fields": [{"id": 1, "name": "x", "required": False, "type": "string"}],
            },
        },
        timeout=30,
    )
    assert response.status_code in (200, 409), response.text


def catalog_tables(started_cluster, namespace):
    response = requests.get(
        f"{catalog_local_url(started_cluster)}/namespaces/{namespace}/tables", timeout=30
    )
    response.raise_for_status()
    return {identifier["name"] for identifier in response.json()["identifiers"]}


def profile_event(node, query_id, event):
    node.query("SYSTEM FLUSH LOGS")
    return int(
        node.query(
            f"SELECT sum(ProfileEvents['{event}']) FROM system.query_log "
            f"WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
        ).strip()
    )


def test_password_user_is_denied(started_cluster):
    node = started_cluster.instances["node1"]
    create_database(node, CATALOG_NAME, DATABASE_SETTINGS)

    response = node.http_request(
        "",
        method="POST",
        data=f"SELECT * FROM {CATALOG_NAME}.`nonexistent.table`",
        params={"user": "passworduser", "password": "passworduser_password"},
    )
    assert "CATALOG_USER_TOKEN_NOT_AVAILABLE" in response.text, response.text


def test_native_protocol_forwards_jwt(started_cluster):
    node = started_cluster.instances["node1"]
    create_database(node, CATALOG_NAME, DATABASE_SETTINGS)

    node.exec_in_container(
        ["clickhouse", "client", "--jwt", make_token("alice"), "--query", f"CHECK DATABASE {CATALOG_NAME}"]
    )


def test_no_forwarding_without_the_server_setting(started_cluster):
    node = started_cluster.instances["node1"]

    create_database(node, CATALOG_NAME, DATABASE_SETTINGS)

    node.replace_in_config(
        "/etc/clickhouse-server/config.d/token_forwarding.xml",
        "<enable_token_forwarding>1</enable_token_forwarding>",
        "<enable_token_forwarding>0</enable_token_forwarding>",
    )
    node.query("SYSTEM RELOAD CONFIG")
    try:
        response = node.http_request(
            "",
            method="POST",
            data=f"SELECT * FROM {CATALOG_NAME}.`nonexistent.table`",
            headers={"Authorization": f"Bearer {make_token('alice')}"},
        )
        assert "CATALOG_USER_TOKEN_NOT_AVAILABLE" in response.text, response.text
    finally:
        node.replace_in_config(
            "/etc/clickhouse-server/config.d/token_forwarding.xml",
            "<enable_token_forwarding>0</enable_token_forwarding>",
            "<enable_token_forwarding>1</enable_token_forwarding>",
        )
        node.query("SYSTEM RELOAD CONFIG")


def write_fixture(started_cluster, node):
    namespace = f"ns_{uuid.uuid4().hex[:8]}"
    table = f"t_{uuid.uuid4().hex[:8]}"
    create_namespace(started_cluster, namespace)
    create_table_in_catalog(started_cluster, namespace, table)
    create_database(node, CATALOG_NAME, DATABASE_SETTINGS, storage_credentials=True)
    return namespace, table


def test_async_insert_retains_the_querying_user_token(started_cluster):
    node = started_cluster.instances["node1"]
    namespace, table = write_fixture(started_cluster, node)
    token = make_token("writer")
    query_id = f"insert-{uuid.uuid4()}"
    query_with_token(
        node,
        token,
        f"INSERT INTO {CATALOG_NAME}.`{namespace}.{table}` VALUES ('written by the token user')",
        params={
            "query_id": query_id,
            "allow_insert_into_iceberg": 1,
            "write_full_path_in_iceberg_metadata": 1,
            "async_insert": 1,
            "wait_for_async_insert": 0,
            "async_insert_use_adaptive_busy_timeout": 0,
            "async_insert_busy_timeout_ms": 60000,
        },
    )

    node.query("SYSTEM FLUSH ASYNC INSERT QUEUE")
    assert profile_event(node, query_id, "AsyncInsertQuery") == 1
    assert (
        query_with_token(node, token, f"SELECT x FROM {CATALOG_NAME}.`{namespace}.{table}`").strip()
        == "written by the token user"
    )


def test_drop_table_reaches_the_catalog_as_the_querying_user(started_cluster):
    node = started_cluster.instances["node1"]
    namespace, table = write_fixture(started_cluster, node)

    query_id = f"drop-{uuid.uuid4()}"
    query_with_token(
        node,
        make_token("dropper"),
        f"DROP TABLE {CATALOG_NAME}.`{namespace}.{table}`",
        params={"query_id": query_id},
    )

    assert table not in catalog_tables(started_cluster, namespace)
    assert profile_event(node, query_id, "DataLakeRestCatalogAuthTokenRetrieve") == 0
