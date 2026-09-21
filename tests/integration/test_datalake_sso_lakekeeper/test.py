import json
import logging
import time
import uuid

import pandas as pd
import pyarrow as pa
import pytest
import requests
from pyiceberg.catalog.rest import RestCatalog
from pyiceberg.schema import Schema
from pyiceberg.types import IntegerType, NestedField, StringType

from helpers.cluster import ClickHouseCluster

REALM = "clickhouse-test"
KEYCLOAK_INTERNAL = f"http://keycloak:8080/realms/{REALM}"
TOKEN_ENDPOINT = f"{KEYCLOAK_INTERNAL}/protocol/openid-connect/token"
CATALOG_INTERNAL_URL = "http://lakekeeper:8181/catalog"

CLIENT_ID = "clickhouse"
CLIENT_SECRET = "clickhouse-secret"
EXCHANGE_CLIENT_ID = "clickhouse-exchange"
EXCHANGE_CLIENT_SECRET = "clickhouse-exchange-secret"
SCOPE = "openid"

WAREHOUSES = ["wh_alice", "wh_bob", "wh_shared"]

SCHEMA = Schema(
    NestedField(field_id=1, name="id", field_type=IntegerType(), required=False),
    NestedField(field_id=2, name="data", field_type=StringType(), required=False),
)


def lakekeeper_host_url(cluster):
    return f"http://localhost:{cluster.iceberg_rest_catalog_port}"


def get_token(node, username, password="secret", client_id=CLIENT_ID, client_secret=CLIENT_SECRET,
              scope=SCOPE):
    form = (
        f"grant_type=password&client_id={client_id}&client_secret={client_secret}"
        f"&username={username}&password={password}"
    )
    if scope:
        form += f"&scope={scope}"
    raw = node.exec_in_container(
        ["bash", "-c", f"curl -s -X POST -d '{form}' {TOKEN_ENDPOINT}"]
    )
    payload = json.loads(raw)
    assert "access_token" in payload, raw
    return payload["access_token"]


def jwt_claim(token, claim):
    import base64

    body = token.split(".")[1]
    body += "=" * (-len(body) % 4)
    return json.loads(base64.urlsafe_b64decode(body))[claim]


def management(cluster, method, path, token, json_body=None, expected=(200, 201, 204, 409)):
    response = requests.request(
        method,
        f"{lakekeeper_host_url(cluster)}/management/v1{path}",
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
        json=json_body,
        timeout=60,
    )
    assert response.status_code in expected, f"{method} {path} -> {response.status_code}: {response.text}"
    return response


def lakekeeper_rejects(cluster, token):
    response = requests.get(
        f"{lakekeeper_host_url(cluster)}/management/v1/whoami",
        headers={"Authorization": f"Bearer {token}"},
        timeout=60,
    )
    return response.status_code == 401


def create_warehouse(cluster, token, name):
    minio_endpoint = f"http://{cluster.minio_ip}:{cluster.minio_port}"
    body = {
        "warehouse-name": name,
        "project-id": "00000000-0000-0000-0000-000000000000",
        "storage-profile": {
            "type": "s3",
            "bucket": "warehouse-rest",
            "key-prefix": name,
            "assume-role-arn": None,
            "endpoint": minio_endpoint,
            "region": "local-01",
            "path-style-access": True,
            "flavor": "minio",
            "sts-enabled": True,
        },
        "storage-credential": {
            "type": "s3",
            "credential-type": "access-key",
            "aws-access-key-id": "minio",
            "aws-secret-access-key": "ClickHouse_Minio_P@ssw0rd",
        },
    }
    response = management(cluster, "POST", "/warehouse", token, body)
    if response.status_code == 409:
        listing = management(cluster, "GET", "/warehouse", token).json()
        for warehouse in listing.get("warehouses", []):
            if warehouse["name"] == name:
                return warehouse["id"]
        raise AssertionError(f"warehouse {name} exists but was not listed")
    return response.json()["id"]


def provision_user(cluster, admin_token, token_of_user, username):
    management(
        cluster,
        "POST",
        "/user",
        admin_token,
        {
            "id": f"oidc~{jwt_claim(token_of_user, 'sub')}",
            "name": username,
            "email": f"{username}@example.com",
            "user-type": "human",
            "update-if-exists": True,
        },
    )
    return f"oidc~{jwt_claim(token_of_user, 'sub')}"


def grant_on_warehouse(cluster, admin_token, warehouse_id, user_id, relations):
    management(
        cluster,
        "POST",
        f"/permissions/warehouse/{warehouse_id}/assignments",
        admin_token,
        {"writes": [{"user": user_id, "type": relation} for relation in relations]},
    )


def pyiceberg_catalog(cluster, warehouse, token):
    return RestCatalog(
        name="lakekeeper",
        warehouse=warehouse,
        uri=f"{lakekeeper_host_url(cluster)}/catalog",
        token=token,
        **{
            "s3.endpoint": f"http://{cluster.minio_ip}:{cluster.minio_port}",
            "s3.access-key-id": "minio",
            "s3.secret-access-key": "ClickHouse_Minio_P@ssw0rd",
        },
    )


def seed_table(cluster, warehouse, token, namespace, table_name, rows=3):
    catalog = pyiceberg_catalog(cluster, warehouse, token)
    if (namespace,) not in catalog.list_namespaces():
        catalog.create_namespace((namespace,))
    table = catalog.create_table(
        (namespace, table_name),
        schema=SCHEMA,
        properties={"write.metadata.compression-codec": "none"},
    )
    table.append(
        pa.Table.from_pandas(
            pd.DataFrame({"id": list(range(rows)), "data": [f"row{i}" for i in range(rows)]}),
            schema=table.schema().as_arrow(),
        )
    )
    return table


def create_database(node, name, warehouse, extra=None):
    settings = {
        "catalog_type": "rest",
        "warehouse": warehouse,
        "storage_endpoint": "http://minio1:9001/warehouse-rest",
        "oauth_forward_user_token": 1,
    }
    settings.update(extra or {})
    node.query(f"DROP DATABASE IF EXISTS {name}")
    node.query(
        f"SET allow_experimental_database_iceberg=true;"
        f"CREATE DATABASE {name} ENGINE = DataLakeCatalog('{CATALOG_INTERNAL_URL}') "
        f"SETTINGS {','.join(k + '=' + repr(v) for k, v in settings.items())}"
    )


def query_as(node, token, sql, query_id=None):
    params = {"query_id": query_id} if query_id else None
    response = node.http_request(
        "", method="POST", data=sql, params=params,
        headers={"Authorization": f"Bearer {token}"},
    )
    return response


def query_as_ok(node, token, sql, query_id=None):
    response = query_as(node, token, sql, query_id)
    assert response.status_code == 200, response.text
    return response.text


def profile_event(node, query_id, event):
    node.query("SYSTEM FLUSH LOGS")
    value = node.query(
        f"SELECT sum(ProfileEvents['{event}']) FROM system.query_log "
        f"WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
    ).strip()
    return int(value) if value else 0


@pytest.fixture(scope="module")
def started_cluster():
    cluster = ClickHouseCluster(__file__)
    try:
        for name in ("node1", "node2"):
            cluster.add_instance(
                name,
                main_configs=[
                    "configs/token_forwarding.xml",
                    "configs/cluster.xml",
                    "configs/session_log.xml",
                ],
                user_configs=["configs/users.xml"],
                stay_alive=True,
                with_iceberg_catalog=True,
                extra_parameters={
                    "docker_compose_file_name": "docker_compose_iceberg_lakekeeper_oidc_catalog.yml"
                },
            )
        logging.info("Starting cluster...")
        cluster.start()

        node = cluster.instances["node1"]
        for instance in cluster.instances.values():
            instance.query("CREATE ROLE IF NOT EXISTS token_users")
            instance.query("GRANT CHECK, DROP TABLE, INSERT, SELECT, SHOW ON *.* TO token_users")
            instance.query("GRANT S3 ON *.* TO token_users")
            instance.query("GRANT REMOTE ON *.* TO token_users")

        wait_for_lakekeeper(cluster)

        admin_token = get_token(node, "lkadmin")
        management(
            cluster,
            "POST",
            "/bootstrap",
            admin_token,
            {"accept-terms-of-use": True, "is-operator": True},
            expected=(200, 204, 400, 409),
        )

        alice_token = get_token(node, "alice")
        bob_token = get_token(node, "bob")
        alice_id = provision_user(cluster, admin_token, alice_token, "alice")
        bob_id = provision_user(cluster, admin_token, bob_token, "bob")

        warehouse_ids = {name: create_warehouse(cluster, admin_token, name) for name in WAREHOUSES}

        full = ["describe", "select", "create", "modify"]
        grant_on_warehouse(cluster, admin_token, warehouse_ids["wh_alice"], alice_id, full)
        grant_on_warehouse(cluster, admin_token, warehouse_ids["wh_bob"], bob_id, full)
        grant_on_warehouse(cluster, admin_token, warehouse_ids["wh_shared"], alice_id, ["describe", "select"])
        grant_on_warehouse(cluster, admin_token, warehouse_ids["wh_shared"], bob_id, ["describe", "select"])

        seed_table(cluster, "wh_alice", admin_token, "ns", "t_alice")
        seed_table(cluster, "wh_bob", admin_token, "ns", "t_bob")
        seed_table(cluster, "wh_shared", admin_token, "ns", "t_shared")

        cluster.lakekeeper_warehouse_ids = warehouse_ids
        yield cluster
    finally:
        cluster.shutdown()


def wait_for_lakekeeper(cluster, timeout=180):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            response = requests.get(f"{lakekeeper_host_url(cluster)}/health", timeout=5)
            if response.status_code == 200:
                return
            last = response.text
        except requests.exceptions.RequestException as ex:
            last = str(ex)
        time.sleep(2)
    raise AssertionError(f"Lakekeeper did not become healthy: {last}")


def test_users_see_different_tables(started_cluster):
    node = started_cluster.instances["node1"]
    create_database(node, "db_alice", "wh_alice")
    create_database(node, "db_bob", "wh_bob")

    alice = get_token(node, "alice")
    bob = get_token(node, "bob")

    listing_sql = (
        "SELECT name FROM system.tables WHERE database = '{db}' ORDER BY name "
        "SETTINGS show_data_lake_catalogs_in_system_tables = true"
    )
    assert query_as_ok(node, alice, listing_sql.format(db="db_alice")).strip() == "ns.t_alice"
    assert query_as_ok(node, bob, listing_sql.format(db="db_bob")).strip() == "ns.t_bob"

    denied = query_as(node, alice, listing_sql.format(db="db_bob"))
    assert denied.status_code != 200, denied.text
    denied = query_as(node, bob, listing_sql.format(db="db_alice"))
    assert denied.status_code != 200, denied.text


def test_warm_credentials_cache_does_not_serve_another_user(started_cluster):
    node = started_cluster.instances["node1"]
    create_database(node, "db_alice", "wh_alice", {"vended_credentials_cache_ttl": 300})

    alice = get_token(node, "alice")
    assert int(query_as_ok(node, alice, "SELECT count() FROM db_alice.`ns.t_alice`")) == 3
    assert int(query_as_ok(node, alice, "SELECT count() FROM db_alice.`ns.t_alice`")) == 3

    denied = query_as(node, get_token(node, "bob"), "SELECT count() FROM db_alice.`ns.t_alice`")
    assert denied.status_code != 200, denied.text


def test_no_token_in_system_logs(started_cluster):
    node = started_cluster.instances["node1"]
    create_database(node, "db_alice", "wh_alice")

    token = get_token(node, "alice")
    query_as_ok(node, token, "SELECT count() FROM db_alice.`ns.t_alice`")
    query_as(node, token, "SELECT count() FROM db_alice.`ns.does_not_exist`")
    query_as(node, token[:-4] + "AAAA", "SELECT 1")

    node.query("SYSTEM FLUSH LOGS")
    needle = token.split(".")[2][:32]
    for table, columns in (
        ("system.query_log", ["query", "exception", "stack_trace"]),
        ("system.text_log", ["message"]),
        ("system.session_log", ["failure_reason"]),
    ):
        condition = " OR ".join(f"{column} LIKE '%{needle}%'" for column in columns)
        found = node.query(f"SELECT count() FROM {table} WHERE {condition}").strip()
        assert found == "0", f"token leaked into {table}"


def test_swarm_read_does_not_reach_the_catalog_from_workers(started_cluster):
    started = started_cluster
    node1 = started.instances["node1"]
    node2 = started.instances["node2"]
    create_database(node1, "db_shared", "wh_shared")

    def catalog_requests(node):
        node.query("SYSTEM FLUSH LOGS")
        value = node.query(
            "SELECT value FROM system.events WHERE event = 'DataLakeRestCatalogGetTableMetadata'"
        ).strip()
        return int(value) if value else 0

    query_id = f"swarm-{uuid.uuid4()}"
    before = catalog_requests(node2)
    assert int(query_as_ok(
        node1,
        get_token(node1, "alice"),
        "SELECT sum(id) FROM db_shared.`ns.t_shared` "
        "SETTINGS object_storage_cluster = 'cluster_simple'",
        query_id,
    )) == 3

    node2.query("SYSTEM FLUSH LOGS")
    worker_queries = node2.query(
        f"SELECT count() FROM system.query_log "
        f"WHERE initial_query_id = '{query_id}' AND type = 'QueryFinish'"
    ).strip()
    assert int(worker_queries) > 0, "node2 never ran a part of the query"

    assert catalog_requests(node2) == before


def test_exchange_at_the_idp(started_cluster):
    node = started_cluster.instances["node1"]
    create_database(
        node,
        "db_exchange",
        "wh_alice",
        {
            "catalog_credential": f"{CLIENT_ID}:{CLIENT_SECRET}",
            "auth_scope": SCOPE,
            "oauth_token_exchange_uri": TOKEN_ENDPOINT,
        },
    )

    token = get_token(
        node, "alice", client_id=EXCHANGE_CLIENT_ID, client_secret=EXCHANGE_CLIENT_SECRET
    )
    audience = jwt_claim(token, "aud")
    assert "lakekeeper" not in ([audience] if isinstance(audience, str) else audience)
    assert lakekeeper_rejects(started_cluster, token)

    query_id = f"exchange-{uuid.uuid4()}"
    assert int(query_as_ok(node, token, "SELECT count() FROM db_exchange.`ns.t_alice`", query_id)) == 3
    assert profile_event(node, query_id, "DataLakeRestCatalogTokenExchange") >= 1
    assert profile_event(node, query_id, "DataLakeRestCatalogTokenExchangeFailures") == 0
    assert profile_event(node, query_id, "DataLakeRestCatalogAuthTokenRetrieve") == 0
