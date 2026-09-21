import json
import logging
import os
import uuid

import boto3
import jwt
import pytest

from helpers.cluster import ClickHouseCluster
from helpers.mock_servers import start_mock_servers

SECRET = "glue_token_forwarding_secret"
BASE_URL = "http://glue:3000"
ROLE_ARN = "arn:aws:iam::123456789012:role/data-lake-reader"
STS_CONTAINER = "sts.us-east-1.amazonaws.com"

DATABASE_SETTINGS = {
    "catalog_type": "glue",
    "warehouse": "test",
    "storage_endpoint": "http://minio1:9001/warehouse-glue",
    "region": "us-east-1",
    "aws_role_arn": ROLE_ARN,
    "oauth_forward_user_token": "1",
}


def make_token(user):
    return jwt.encode({"sub": user}, SECRET, algorithm="HS256")


def run_sts_mock(cluster):
    start_mock_servers(
        cluster,
        os.path.join(os.path.dirname(__file__), "s3_mocks"),
        [("mock_sts.py", STS_CONTAINER, "80")],
    )


@pytest.fixture(scope="module")
def started_cluster():
    try:
        os.environ["AWS_ACCESS_KEY_ID"] = "testing"
        os.environ["AWS_SECRET_ACCESS_KEY"] = "testing"

        cluster = ClickHouseCluster(__file__)
        cluster.add_instance(
            "node1",
            main_configs=["configs/token_forwarding.xml"],
            user_configs=["configs/users.xml"],
            stay_alive=True,
            with_glue_catalog=True,
        )

        sts = cluster.add_instance(
            name=STS_CONTAINER,
            hostname=STS_CONTAINER,
            image="altinityinfra/python-bottle",
            tag="latest",
            stay_alive=True,
        )
        sts.stop_clickhouse(kill=True)

        logging.info("Starting cluster...")
        cluster.start()
        run_sts_mock(cluster)

        node = cluster.instances["node1"]
        node.query("CREATE ROLE IF NOT EXISTS token_users")
        node.query("GRANT SHOW, SELECT ON *.* TO token_users")

        yield cluster
    finally:
        cluster.shutdown()


def glue_client(started_cluster):
    return boto3.client(
        "glue",
        endpoint_url=f"http://localhost:{started_cluster.glue_catalog_port}",
        region_name="us-east-1",
        aws_access_key_id="testing",
        aws_secret_access_key="testing",
    )


def create_glue_table(started_cluster, namespace, table):
    client = glue_client(started_cluster)
    client.create_database(DatabaseInput={"Name": namespace})
    client.create_table(
        DatabaseName=namespace,
        TableInput={
            "Name": table,
            "Parameters": {"table_type": "ICEBERG"},
            "StorageDescriptor": {
                "Columns": [{"Name": "x", "Type": "int"}],
                "Location": f"s3://warehouse-glue/{namespace}/{table}",
            },
        },
    )


def sts_requests(started_cluster):
    output = started_cluster.exec_in_container(
        started_cluster.get_container_id(STS_CONTAINER),
        [
            "python3",
            "-c",
            "import urllib.request;"
            "print(urllib.request.urlopen('http://localhost:80/_requests').read().decode())",
        ],
    )
    return json.loads(output)


def reset_sts(started_cluster):
    started_cluster.exec_in_container(
        started_cluster.get_container_id(STS_CONTAINER),
        [
            "python3",
            "-c",
            "import urllib.request; urllib.request.urlopen('http://localhost:80/_reset').read()",
        ],
    )


@pytest.fixture(autouse=True)
def clean_sts_log(started_cluster):
    reset_sts(started_cluster)
    yield


def create_database(node, name, extra_settings=None):
    settings = dict(DATABASE_SETTINGS)
    settings.update(extra_settings or {})
    node.query(
        f"DROP DATABASE IF EXISTS {name}; "
        f"CREATE DATABASE {name} ENGINE = DataLakeCatalog('{BASE_URL}') "
        f"SETTINGS {','.join(k + '=' + repr(v) for k, v in settings.items())}",
        settings={"allow_database_glue_catalog": 1},
    )


def query_with_token(node, token, sql, **kwargs):
    response = node.http_request(
        "", method="POST", data=sql, headers={"Authorization": f"Bearer {token}"}, **kwargs
    )
    response.raise_for_status()
    return response.text


def profile_event(node, query_id, event):
    node.query("SYSTEM FLUSH LOGS")
    return int(
        node.query(
            f"SELECT sum(ProfileEvents['{event}']) FROM system.query_log "
            f"WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
        ).strip()
        or 0
    )


def test_user_tokens_are_exchanged_into_separate_sts_sessions(started_cluster):
    node = started_cluster.instances["node1"]
    namespace = f"ns_{uuid.uuid4().hex[:8]}"
    create_glue_table(started_cluster, namespace, "t")

    db = f"glue_{uuid.uuid4().hex[:8]}"
    create_database(node, db)

    token = make_token("alice")
    query_id = str(uuid.uuid4())
    query_with_token(node, token, f"SHOW TABLES FROM {db}", params={"query_id": query_id})

    requests = sts_requests(started_cluster)
    assert len(requests) == 1, requests

    request = requests[0]
    assert request["action"] == "AssumeRoleWithWebIdentity"
    assert request["version"] == "2011-06-15"
    assert request["role_arn"] == ROLE_ARN
    assert request["role_session_name"] == "alice"
    assert request["web_identity_token"] == token

    assert token not in request["query_string"]

    assert profile_event(node, query_id, "DataLakeGlueCatalogServiceIdentityRequests") == 0

    query_with_token(node, make_token("bob"), f"SHOW TABLES FROM {db}")
    sessions = sts_requests(started_cluster)
    assert sorted(request["role_session_name"] for request in sessions) == ["alice", "bob"]
    assert {request["web_identity_token"] for request in sessions} == {token, make_token("bob")}


def test_no_token_fails_closed(started_cluster):
    node = started_cluster.instances["node1"]
    db = f"glue_{uuid.uuid4().hex[:8]}"
    create_database(node, db)

    error = node.query_and_get_error(
        f"SHOW TABLES FROM {db}", user="passworduser", password="passworduser_password"
    )
    assert "carries no bearer token" in error, error

    assert sts_requests(started_cluster) == []


def test_rejected_token_does_not_fall_back(started_cluster):
    node = started_cluster.instances["node1"]
    db = f"glue_{uuid.uuid4().hex[:8]}"
    create_database(node, db)

    response = node.http_request(
        "",
        method="POST",
        data=f"SHOW TABLES FROM {db}",
        headers={"Authorization": f"Bearer {make_token('rejected')}"},
    )
    assert response.status_code != 200
    assert "Could not assume role" in response.text, response.text
    assert "InvalidIdentityToken" in response.text, response.text

    assert len(sts_requests(started_cluster)) == 1
