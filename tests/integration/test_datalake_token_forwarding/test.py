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
from helpers.config_cluster import minio_access_key, minio_secret_key

SECRET = "datalake_token_forwarding_secret"
BASE_URL = "http://rest:8181/v1"
CATALOG_NAME = "demo"
# `async_insert` defaults to 1 here, and an asynchronous insert is flushed from a background queue
# whose context carries no forwarded token, so it can only ever fail closed against a forwarding
# database. These tests are about the synchronous commit in `IcebergStorageSink`, which is the path
# `docs/en/engines/database-engines/datalake.md` describes, so they pin the setting off.
WRITE_SETTINGS = {
    "allow_insert_into_iceberg": 1,
    "write_full_path_in_iceberg_metadata": 1,
    "async_insert": 0,
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

        # Auto-provisioned token users hold no privileges of their own, so hand every one of them
        # this role -- `common_roles` in `token_forwarding.xml` grants it. Created here rather than
        # inside a test so that it already exists the first time a token user authenticates.
        node = cluster.instances["node1"]
        node.query("CREATE ROLE IF NOT EXISTS token_users")
        node.query("GRANT CHECK, DROP TABLE, INSERT, SELECT, SHOW ON *.* TO token_users")
        # Reading and writing table data goes to S3, which the `SOURCES` privileges guard separately.
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
    """
    One engine argument by default, which is what makes per-user credential vending observable.

    `storage_credentials` adds the MinIO key pair as the second and third arguments. Anything that
    reads or writes table *data* needs them: with a single argument the storage credentials have to
    come from the catalog, and the fixture's catalog does not vend any. Pinning them leaves the
    catalog identity as the only per-user thing in the query, which is what the write-path tests
    are about.
    """
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
    """
    Create a table through the catalog's own REST API, bypassing ClickHouse entirely, so that a
    listing has something to find. Without it `system.tables` returns zero rows whether the catalog
    answered or refused, and an assertion on the count could not fail.
    """
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
    """The catalog's own view of a namespace, fetched without going through ClickHouse."""
    response = requests.get(
        f"{catalog_local_url(started_cluster)}/namespaces/{namespace}/tables", timeout=30
    )
    response.raise_for_status()
    return {identifier["name"] for identifier in response.json()["identifiers"]}


def visible_tables(node, token, namespace, table, **kwargs):
    """
    How many rows `system.tables` shows for one known table. Zero means the catalog listing did not
    happen: `DatabaseDataLake::getLightweightTablesIterator` swallows catalog errors so that one
    unreachable database cannot break the whole system table.
    """
    return query_with_token(
        node,
        token,
        f"SELECT count() FROM system.tables WHERE database = '{CATALOG_NAME}' "
        f"AND name = '{namespace}.{table}' "
        f"SETTINGS show_data_lake_catalogs_in_system_tables = true",
        **kwargs,
    ).strip()


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
    table = f"t_{uuid.uuid4().hex[:8]}"
    create_namespace(started_cluster, namespace)
    create_table_in_catalog(started_cluster, namespace, table)

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

    # A table the catalog is known to hold has to come back. Asserting only that the count is a
    # number would hold just as well when the catalog refused the request, because the listing
    # swallows catalog errors and returns nothing.
    assert visible_tables(node, make_token("alice"), namespace, table) == "1"


def test_no_service_principal_fallback(started_cluster):
    """
    With forwarding on, no request may be signed as the service principal. The
    `DataLakeRestCatalogClientCredentialsGrants` event is the fail-open detector: it must stay 0.
    """
    node = started_cluster.instances["node1"]
    namespace = f"ns_{uuid.uuid4().hex[:8]}"
    table = f"t_{uuid.uuid4().hex[:8]}"
    create_namespace(started_cluster, namespace)
    create_table_in_catalog(started_cluster, namespace, table)

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
    # The listing has to succeed, otherwise a zero grant count would only mean nothing was asked.
    assert visible_tables(
        node, make_token("alice"), namespace, table, params={"query_id": query_id}
    ) == "1"

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
    table = f"t_{uuid.uuid4().hex[:8]}"
    create_namespace(started_cluster, namespace)
    create_table_in_catalog(started_cluster, namespace, table)

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
            f"AND name = '{namespace}.{table}' "
            f"SETTINGS show_data_lake_catalogs_in_system_tables = true\"",
        ]
    )
    # As over HTTP: the known table has to be listed, not merely some number returned.
    assert result.strip() == "1", result


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


def test_check_database_forwards_the_user_token(started_cluster):
    """
    `CHECK DATABASE` contacts the catalog, so it has to carry the querying user's token like every
    other statement. It used to send no token at all and therefore could never succeed against a
    forwarding database, no matter how the session had authenticated.
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
            "oauth_forward_user_token": 1,
        },
    )

    # `CHECK DATABASE` returns no rows: it either completes or throws.
    query_with_token(node, make_token("checker"), f"CHECK DATABASE {CATALOG_NAME}")


def test_check_database_without_a_token_is_denied(started_cluster):
    """The other half of the same statement: a session with no token must still fail closed."""
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
        f"CHECK DATABASE {CATALOG_NAME}",
        user="passworduser",
        password="passworduser_password",
    )
    assert "CATALOG_USER_TOKEN_NOT_AVAILABLE" in output, output


WRITE_DATABASE_SETTINGS = {
    "catalog_type": "rest",
    "warehouse": "demo",
    "storage_endpoint": "http://minio1:9001/warehouse-rest",
    # A service principal is configured, so a fallback to it would succeed if one existed.
    "catalog_credential": "service:principal",
    "oauth_forward_user_token": 1,
}


def write_fixture(started_cluster, node):
    """
    A namespace and a table the catalog already holds, plus a forwarding database that can reach
    the data behind them.

    The table is registered through the catalog's own REST API rather than with `CREATE TABLE`:
    `createStorageObjectStorage` builds the storage from the global context, which never holds a
    user token, so `CREATE TABLE` against a forwarding database always fails closed.
    """
    namespace = f"ns_{uuid.uuid4().hex[:8]}"
    table = f"t_{uuid.uuid4().hex[:8]}"
    create_namespace(started_cluster, namespace)
    create_table_in_catalog(started_cluster, namespace, table)
    create_database(node, CATALOG_NAME, WRITE_DATABASE_SETTINGS, storage_credentials=True)
    return namespace, table


def test_insert_reaches_the_catalog_as_the_querying_user(started_cluster):
    """
    `INSERT` commits through `catalog->updateMetadata(..., context->getForwardedAuthToken())`, so
    the write path has to carry the querying user's identity exactly as the read path does.

    `docs/en/engines/database-engines/datalake.md` promises this under "What is and is not
    covered"; until this test there was nothing behind the promise.
    """
    node = started_cluster.instances["node1"]
    namespace, table = write_fixture(started_cluster, node)

    token = make_token("writer")
    query_id = f"insert-{uuid.uuid4()}"
    query_with_token(
        node,
        token,
        f"INSERT INTO {CATALOG_NAME}.`{namespace}.{table}` VALUES ('written by the token user')",
        params={"query_id": query_id, **WRITE_SETTINGS},
    )

    # The commit was signed with the user's own identity, not quietly with the service principal.
    assert profile_event(node, query_id, "DataLakeRestCatalogClientCredentialsGrants") == 0
    # The snapshot the catalog now points at is the one this INSERT wrote.
    assert (
        query_with_token(node, token, f"SELECT x FROM {CATALOG_NAME}.`{namespace}.{table}`").strip()
        == "written by the token user"
    )


def test_insert_without_a_token_is_denied(started_cluster):
    """The other half: a session with no token must not be able to write through the catalog."""
    node = started_cluster.instances["node1"]
    namespace, table = write_fixture(started_cluster, node)

    output = node.query_and_get_error(
        f"INSERT INTO {CATALOG_NAME}.`{namespace}.{table}` VALUES ('written by nobody')",
        user="passworduser",
        password="passworduser_password",
        settings=WRITE_SETTINGS,
    )
    assert "CATALOG_USER_TOKEN_NOT_AVAILABLE" in output, output


def test_drop_table_reaches_the_catalog_as_the_querying_user(started_cluster):
    """
    `StorageObjectStorage::drop` sends `catalog_auth_token`, a member captured when the storage was
    constructed rather than read from a query context. The invariant that makes that safe is that
    `DatabaseDataLake::dropTable` builds the storage from the query context and calls `drop` on it
    synchronously, so the captured token is the querying user's. Nothing else exercises it.
    """
    node = started_cluster.instances["node1"]
    namespace, table = write_fixture(started_cluster, node)

    query_id = f"drop-{uuid.uuid4()}"
    query_with_token(
        node,
        make_token("dropper"),
        f"DROP TABLE {CATALOG_NAME}.`{namespace}.{table}`",
        params={"query_id": query_id},
    )

    # The catalog's own view, not ClickHouse's: the drop really reached it.
    assert table not in catalog_tables(started_cluster, namespace)
    assert profile_event(node, query_id, "DataLakeRestCatalogClientCredentialsGrants") == 0


def test_drop_table_without_a_token_is_denied(started_cluster):
    """A session with no token cannot drop, and the refusal leaves the table intact."""
    node = started_cluster.instances["node1"]
    namespace, table = write_fixture(started_cluster, node)

    output = node.query_and_get_error(
        f"DROP TABLE {CATALOG_NAME}.`{namespace}.{table}`",
        user="passworduser",
        password="passworduser_password",
    )
    assert "CATALOG_USER_TOKEN_NOT_AVAILABLE" in output, output
    # Fail closed means the table survives, not that it is half dropped.
    assert table in catalog_tables(started_cluster, namespace)
