"""
End-to-end SSO: the identity that authenticated to ClickHouse is the identity the Iceberg REST
catalog authorizes.

Layer 3 of the verification plan. Keycloak issues the tokens, Lakekeeper validates them and -- with
`LAKEKEEPER__AUTHZ_BACKEND=openfga`, not the `allowall` default -- actually enforces per-user
permissions. Without that backend every assertion here would pass for the wrong reason.

Passthrough is what makes this layer possible at all: Lakekeeper accepts IdP tokens directly, so no
token endpoint is involved. The exchange-at-IdP variant is one extra case on the same topology.

Run:
    python -m ci.praktika run "integration" --test test_datalake_sso_lakekeeper
"""

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

# Client whose tokens already carry `aud=lakekeeper` (default client scope), for passthrough.
CLIENT_ID = "clickhouse"
CLIENT_SECRET = "clickhouse-secret"
# Client whose tokens do NOT, so an exchange is what has to produce the audience.
EXCHANGE_CLIENT_ID = "clickhouse-exchange"
EXCHANGE_CLIENT_SECRET = "clickhouse-exchange-secret"

WAREHOUSES = ["wh_alice", "wh_bob", "wh_shared"]

SCHEMA = Schema(
    NestedField(field_id=1, name="id", field_type=IntegerType(), required=False),
    NestedField(field_id=2, name="data", field_type=StringType(), required=False),
)


# --- helpers ---------------------------------------------------------------------------------

def lakekeeper_host_url(cluster):
    return f"http://localhost:{cluster.iceberg_rest_catalog_port}"


def get_token(node, username, password="secret", client_id=CLIENT_ID, client_secret=CLIENT_SECRET,
              scope=None):
    """
    Tokens are fetched from inside the ClickHouse container so that every participant -- ClickHouse,
    Lakekeeper and this test -- sees the same issuer, `http://keycloak:8080/realms/...`.
    """
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


# --- fixture ---------------------------------------------------------------------------------

@pytest.fixture(scope="module")
def started_cluster():
    cluster = ClickHouseCluster(__file__)
    try:
        for name in ("node1", "node2"):
            cluster.add_instance(
                name,
                main_configs=["configs/token_forwarding.xml", "configs/cluster.xml"],
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
    """
    Lakekeeper is started before Keycloak by the cluster helper, so it may restart a few times
    while the IdP comes up.
    """
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


# --- tests -----------------------------------------------------------------------------------

def test_users_see_different_tables(started_cluster):
    """The catalog authorizes the human, so two ClickHouse users see two different table sets."""
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

    # And neither can list the other's warehouse.
    assert query_as(node, alice, listing_sql.format(db="db_bob")).status_code != 200
    assert query_as(node, bob, listing_sql.format(db="db_alice")).status_code != 200


def test_alice_cannot_read_bobs_table(started_cluster):
    node = started_cluster.instances["node1"]
    create_database(node, "db_bob", "wh_bob")

    assert int(query_as_ok(node, get_token(node, "bob"), "SELECT count() FROM db_bob.`ns.t_bob`")) == 3

    denied = query_as(node, get_token(node, "alice"), "SELECT count() FROM db_bob.`ns.t_bob`")
    assert denied.status_code != 200
    assert "t_bob" not in denied.text or "403" in denied.text or "Forbidden" in denied.text


def test_warm_credentials_cache_does_not_serve_another_user(started_cluster):
    """
    The highest-value test of the feature. `credentials_cache` used to be keyed on
    `(namespace, table)` and is consulted before any HTTP call, so a warm entry would hand Bob the
    STS credentials Lakekeeper vended for Alice with the catalog never consulted.
    """
    node = started_cluster.instances["node1"]
    create_database(node, "db_alice", "wh_alice", {"vended_credentials_cache_ttl": 300})

    alice = get_token(node, "alice")
    assert int(query_as_ok(node, alice, "SELECT count() FROM db_alice.`ns.t_alice`")) == 3
    # Warm.
    assert int(query_as_ok(node, alice, "SELECT count() FROM db_alice.`ns.t_alice`")) == 3

    denied = query_as(node, get_token(node, "bob"), "SELECT count() FROM db_alice.`ns.t_alice`")
    assert denied.status_code != 200, denied.text


def test_each_user_gets_its_own_vended_credentials(started_cluster):
    """
    Both users may read the same table, but each must be vended its own credentials: the second
    user's request has to reach the catalog rather than reuse the first user's cache entry.
    """
    node = started_cluster.instances["node1"]
    create_database(node, "db_shared", "wh_shared", {"vended_credentials_cache_ttl": 300})

    sql = "SELECT count() FROM db_shared.`ns.t_shared`"

    alice_qid = f"alice-{uuid.uuid4()}"
    query_as_ok(node, get_token(node, "alice"), sql, alice_qid)
    query_as_ok(node, get_token(node, "alice"), sql)  # warm alice's entry

    bob_qid = f"bob-{uuid.uuid4()}"
    query_as_ok(node, get_token(node, "bob"), sql, bob_qid)

    assert profile_event(node, bob_qid, "DataLakeRestCatalogCredentialsCacheMisses") >= 1
    assert profile_event(node, bob_qid, "DataLakeRestCatalogCredentialsCacheHits") == 0


def test_no_service_principal_fallback(started_cluster):
    """`DataLakeRestCatalogClientCredentialsGrants` is the fail-open detector; it must stay at 0."""
    node = started_cluster.instances["node1"]
    create_database(node, "db_alice", "wh_alice", {"catalog_credential": "service:principal"})

    query_id = f"nofallback-{uuid.uuid4()}"
    query_as_ok(node, get_token(node, "alice"), "SELECT count() FROM db_alice.`ns.t_alice`", query_id)
    assert profile_event(node, query_id, "DataLakeRestCatalogClientCredentialsGrants") == 0


def test_password_user_is_denied(started_cluster):
    node = started_cluster.instances["node1"]
    create_database(node, "db_alice", "wh_alice", {"catalog_credential": "service:principal"})

    over_http = node.http_request(
        "",
        method="POST",
        data="SELECT count() FROM db_alice.`ns.t_alice`",
        params={"user": "passworduser", "password": "passworduser_password"},
    )
    assert "CATALOG_USER_TOKEN_NOT_AVAILABLE" in over_http.text, over_http.text

    over_native = node.query_and_get_error(
        "SELECT count() FROM db_alice.`ns.t_alice`",
        user="passworduser",
        password="passworduser_password",
    )
    assert "CATALOG_USER_TOKEN_NOT_AVAILABLE" in over_native, over_native


def test_expired_token_gives_a_clean_error(started_cluster):
    """An expired token is rejected at authentication; nothing reaches the catalog."""
    node = started_cluster.instances["node1"]
    create_database(node, "db_alice", "wh_alice")

    # A structurally valid token whose signature will not verify against the realm's keys.
    bogus = get_token(node, "alice")[:-4] + "AAAA"
    response = query_as(node, bogus, "SELECT count() FROM db_alice.`ns.t_alice`")
    assert response.status_code != 200
    assert "AUTHENTICATION_FAILED" in response.text or "Authentication failed" in response.text


def test_token_rotation_over_http(started_cluster):
    """HTTP re-authenticates per request, so a freshly issued token takes effect immediately."""
    node = started_cluster.instances["node1"]
    create_database(node, "db_alice", "wh_alice")

    first = get_token(node, "alice")
    assert int(query_as_ok(node, first, "SELECT count() FROM db_alice.`ns.t_alice`")) == 3

    # A second, distinct token for the same principal must work just as well.
    time.sleep(1)
    second = get_token(node, "alice")
    assert int(query_as_ok(node, second, "SELECT count() FROM db_alice.`ns.t_alice`")) == 3


def test_no_token_in_system_logs(started_cluster):
    """The forwarded token must not surface in any log table."""
    node = started_cluster.instances["node1"]
    create_database(node, "db_alice", "wh_alice")

    token = get_token(node, "alice")
    query_as_ok(node, token, "SELECT count() FROM db_alice.`ns.t_alice`")
    # Also exercise a failing path, which is where an error message could echo the token.
    query_as(node, token, "SELECT count() FROM db_alice.`ns.does_not_exist`")

    node.query("SYSTEM FLUSH LOGS")
    # The signature segment is the part that is unique to this token and long enough not to
    # collide with anything else.
    needle = token.split(".")[2][:32]
    for table, columns in (
        ("system.query_log", ["query", "exception", "stack_trace"]),
        ("system.text_log", ["message"]),
        ("system.session_log", ["auth_id"]),
    ):
        condition = " OR ".join(f"{column} LIKE '%{needle}%'" for column in columns)
        found = node.query(f"SELECT count() FROM {table} WHERE {condition}").strip()
        assert found == "0", f"token leaked into {table}"

    running = node.query(f"SELECT count() FROM system.processes WHERE query LIKE '%{needle}%'").strip()
    assert running == "0"


def test_swarm_read_does_not_reach_the_catalog_from_workers(started_cluster):
    """
    The initiator resolves everything; workers run a plain table function with the credentials the
    catalog vended, so a secondary node makes no catalog request of its own.
    """
    started = started_cluster
    node1 = started.instances["node1"]
    node2 = started.instances["node2"]
    create_database(node1, "db_shared", "wh_shared", {"object_storage_cluster": "cluster_simple"})

    def catalog_requests(node):
        node.query("SYSTEM FLUSH LOGS")
        value = node.query(
            "SELECT value FROM system.events WHERE event = 'DataLakeRestCatalogGetTableMetadata'"
        ).strip()
        return int(value) if value else 0

    before = catalog_requests(node2)
    assert int(query_as_ok(node1, get_token(node1, "alice"), "SELECT count() FROM db_shared.`ns.t_shared`")) == 3
    assert catalog_requests(node2) == before


def test_exchange_at_the_idp(started_cluster):
    """
    The RFC 8693 variant: the token ClickHouse receives has no `lakekeeper` audience, so the
    exchange at Keycloak is what produces a token Lakekeeper accepts.
    """
    node = started_cluster.instances["node1"]
    create_database(
        node,
        "db_exchange",
        "wh_alice",
        {
            "catalog_credential": f"{EXCHANGE_CLIENT_ID}:{EXCHANGE_CLIENT_SECRET}",
            "auth_scope": "lakekeeper",
            "oauth_token_exchange_uri": TOKEN_ENDPOINT,
        },
    )

    token = get_token(
        node, "alice", client_id=EXCHANGE_CLIENT_ID, client_secret=EXCHANGE_CLIENT_SECRET
    )
    # Precondition: this token on its own is not accepted by Lakekeeper.
    assert "lakekeeper" not in (jwt_claim(token, "aud") or [])

    query_id = f"exchange-{uuid.uuid4()}"
    assert int(query_as_ok(node, token, "SELECT count() FROM db_exchange.`ns.t_alice`", query_id)) == 3
    assert profile_event(node, query_id, "DataLakeRestCatalogTokenExchange") >= 1
    assert profile_event(node, query_id, "DataLakeRestCatalogTokenExchangeFailures") == 0
    assert profile_event(node, query_id, "DataLakeRestCatalogClientCredentialsGrants") == 0
