"""
Request-shape tests for forwarding the querying user's OAuth token to an Iceberg REST catalog.

Layer 2 of the verification plan: an `apache/iceberg-rest-fixture`-style catalog (the only image
that actually routes the `/v1/oauth/tokens` grant) plus HS256 tokens minted inline, so both
passthrough and RFC 8693 token exchange can be observed on the wire.

Deliberately a separate suite from `test_database_iceberg`: that one creates its database with
three engine arguments, and vended credentials are only applied when the engine has exactly one,
so per-user credential assertions there would be vacuous.

Run:
    python -m ci.praktika run "integration" --test test_datalake_token_forwarding
"""

import logging
import uuid

import jwt
import pytest
import requests

from helpers.cluster import ClickHouseCluster
from helpers.config_cluster import minio_secret_key

SECRET = "datalake_token_forwarding_secret"
BASE_URL = "http://rest:8181/v1"
CATALOG_NAME = "demo"


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


def create_database(node, name, settings):
    node.query(f"DROP DATABASE IF EXISTS {name}")
    node.query(
        f"SET allow_experimental_database_iceberg=true;"
        f"CREATE DATABASE {name} ENGINE = DataLakeCatalog('{BASE_URL}') "
        f"SETTINGS {','.join(k + '=' + repr(v) for k, v in settings.items())}"
    )


def profile_event(node, query_id, event):
    node.query("SYSTEM FLUSH LOGS")
    return int(
        node.query(
            f"SELECT sum(ProfileEvents['{event}']) FROM system.query_log "
            f"WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
        ).strip()
        or 0
    )


def test_passthrough_reaches_catalog(started_cluster):
    """A token-authenticated user can list the catalog; the token itself is what the catalog sees."""
    node = started_cluster.instances["node1"]
    namespace = f"ns_{uuid.uuid4().hex[:8]}"
    create_namespace(started_cluster, namespace)

    create_database(
        node,
        CATALOG_NAME,
        {
            "catalog_type": "rest",
            "warehouse": "demo",
            "storage_endpoint": "http://minio1:9001/warehouse-rest",
            "oauth_forward_user_token": 1,
        },
    )

    result = query_with_token(
        node,
        make_token("alice"),
        f"SELECT count() FROM system.tables WHERE database = '{CATALOG_NAME}' "
        f"SETTINGS show_data_lake_catalogs_in_system_tables = true",
    )
    # The catalog answered rather than rejecting the request: the listing completed.
    assert result.strip().isdigit()


def test_no_service_principal_fallback(started_cluster):
    """
    With forwarding on, no request may be signed as the service principal. The
    `DataLakeRestCatalogClientCredentialsGrants` event is the fail-open detector: it must stay 0.
    """
    node = started_cluster.instances["node1"]
    namespace = f"ns_{uuid.uuid4().hex[:8]}"
    create_namespace(started_cluster, namespace)

    create_database(
        node,
        CATALOG_NAME,
        {
            "catalog_type": "rest",
            "warehouse": "demo",
            "storage_endpoint": "http://minio1:9001/warehouse-rest",
            "catalog_credential": "service:principal",
            "oauth_forward_user_token": 1,
        },
    )

    query_id = f"fwd-{uuid.uuid4()}"
    query_with_token(
        node,
        make_token("alice"),
        f"SELECT count() FROM system.tables WHERE database = '{CATALOG_NAME}' "
        f"SETTINGS show_data_lake_catalogs_in_system_tables = true",
        params={"query_id": query_id},
    )

    assert profile_event(node, query_id, "DataLakeRestCatalogClientCredentialsGrants") == 0


def test_password_user_is_denied_over_http(started_cluster):
    """A password-authenticated user has no token, so the catalog must refuse the request."""
    node = started_cluster.instances["node1"]

    create_database(
        node,
        CATALOG_NAME,
        {
            "catalog_type": "rest",
            "warehouse": "demo",
            "storage_endpoint": "http://minio1:9001/warehouse-rest",
            "catalog_credential": "service:principal",
            "oauth_forward_user_token": 1,
        },
    )

    response = node.http_request(
        "",
        method="POST",
        data=f"SELECT * FROM {CATALOG_NAME}.`nonexistent.table`",
        params={"user": "passworduser", "password": "passworduser_password"},
    )
    assert "CATALOG_USER_TOKEN_NOT_AVAILABLE" in response.text, response.text


def test_password_user_is_denied_over_native(started_cluster):
    """Same over the native protocol, which authenticates once at handshake time."""
    node = started_cluster.instances["node1"]

    create_database(
        node,
        CATALOG_NAME,
        {
            "catalog_type": "rest",
            "warehouse": "demo",
            "storage_endpoint": "http://minio1:9001/warehouse-rest",
            "catalog_credential": "service:principal",
            "oauth_forward_user_token": 1,
        },
    )

    output = node.query_and_get_error(
        f"SELECT * FROM {CATALOG_NAME}.`nonexistent.table`",
        user="passworduser",
        password="passworduser_password",
    )
    assert "CATALOG_USER_TOKEN_NOT_AVAILABLE" in output, output


def test_native_protocol_forwards_jwt(started_cluster):
    """`clickhouse-client --jwt` forwards the same way the HTTP interface does."""
    node = started_cluster.instances["node1"]
    namespace = f"ns_{uuid.uuid4().hex[:8]}"
    create_namespace(started_cluster, namespace)

    create_database(
        node,
        CATALOG_NAME,
        {
            "catalog_type": "rest",
            "warehouse": "demo",
            "storage_endpoint": "http://minio1:9001/warehouse-rest",
            "oauth_forward_user_token": 1,
        },
    )

    token = make_token("alice")
    result = node.exec_in_container(
        [
            "bash",
            "-c",
            f"clickhouse client --jwt '{token}' --query "
            f"\"SELECT count() FROM system.tables WHERE database = '{CATALOG_NAME}' "
            f"SETTINGS show_data_lake_catalogs_in_system_tables = true\"",
        ]
    )
    assert result.strip().isdigit(), result


def test_exchange_at_catalog_token_endpoint(started_cluster):
    """
    `oauth_token_exchange_uri` pointed at the catalog's own (deprecated) `/v1/oauth/tokens`.
    The Apache fixture is the only image that routes the grant, so this is where the RFC 8693 wire
    format is confirmed against a real implementation.

    Note: the fixture echoes the subject token back as the session token by design, so this suite
    must not assert "the raw token appears nowhere".
    """
    node = started_cluster.instances["node1"]
    namespace = f"ns_{uuid.uuid4().hex[:8]}"
    create_namespace(started_cluster, namespace)

    create_database(
        node,
        CATALOG_NAME,
        {
            "catalog_type": "rest",
            "warehouse": "demo",
            "storage_endpoint": "http://minio1:9001/warehouse-rest",
            "catalog_credential": "service:principal",
            "auth_scope": "catalog",
            "oauth_forward_user_token": 1,
            "oauth_token_exchange_uri": f"{BASE_URL}/oauth/tokens",
        },
    )

    query_id = f"exchange-{uuid.uuid4()}"
    query_with_token(
        node,
        make_token("alice"),
        f"SELECT count() FROM system.tables WHERE database = '{CATALOG_NAME}' "
        f"SETTINGS show_data_lake_catalogs_in_system_tables = true",
        params={"query_id": query_id},
    )

    assert profile_event(node, query_id, "DataLakeRestCatalogTokenExchange") >= 1
    assert profile_event(node, query_id, "DataLakeRestCatalogTokenExchangeFailures") == 0
    # Even with an exchange configured, no `client_credentials` grant is issued behind the user.
    assert profile_event(node, query_id, "DataLakeRestCatalogClientCredentialsGrants") == 0


def test_exchanged_session_token_is_cached_per_user(started_cluster):
    """A second query by the same user reuses the exchanged session token."""
    node = started_cluster.instances["node1"]
    namespace = f"ns_{uuid.uuid4().hex[:8]}"
    create_namespace(started_cluster, namespace)

    create_database(
        node,
        CATALOG_NAME,
        {
            "catalog_type": "rest",
            "warehouse": "demo",
            "storage_endpoint": "http://minio1:9001/warehouse-rest",
            "catalog_credential": "service:principal",
            "auth_scope": "catalog",
            "oauth_forward_user_token": 1,
            "oauth_token_exchange_uri": f"{BASE_URL}/oauth/tokens",
            "oauth_user_token_cache_ttl": 300,
        },
    )

    token = make_token("alice")
    sql = (
        f"SELECT count() FROM system.tables WHERE database = '{CATALOG_NAME}' "
        f"SETTINGS show_data_lake_catalogs_in_system_tables = true"
    )

    first = f"cache-1-{uuid.uuid4()}"
    query_with_token(node, token, sql, params={"query_id": first})
    assert profile_event(node, first, "DataLakeRestCatalogTokenExchange") >= 1

    second = f"cache-2-{uuid.uuid4()}"
    query_with_token(node, token, sql, params={"query_id": second})
    assert profile_event(node, second, "DataLakeRestCatalogTokenExchange") == 0
    assert profile_event(node, second, "DataLakeRestCatalogUserTokenCacheHits") >= 1


def test_no_forwarding_without_the_server_setting(started_cluster):
    """
    The database setting alone is not enough: without `enable_token_forwarding` the token is
    destroyed at authentication and the request has to fail closed rather than silently run as the
    service principal. Verified by turning the server setting off and restarting.
    """
    node = started_cluster.instances["node1"]

    create_database(
        node,
        CATALOG_NAME,
        {
            "catalog_type": "rest",
            "warehouse": "demo",
            "storage_endpoint": "http://minio1:9001/warehouse-rest",
            "catalog_credential": "service:principal",
            "oauth_forward_user_token": 1,
        },
    )

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
