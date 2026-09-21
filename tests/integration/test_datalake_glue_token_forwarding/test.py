import json
import logging
import os
import uuid

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
    "region": "us-east-1",
    "aws_role_arn": ROLE_ARN,
    "oauth_forward_user_token": "1",
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
        start_mock_servers(
            cluster,
            os.path.join(os.path.dirname(__file__), "s3_mocks"),
            [("mock_sts.py", STS_CONTAINER, "80")],
        )

        node = cluster.instances["node1"]
        node.query("CREATE ROLE IF NOT EXISTS token_users")
        node.query("GRANT SHOW, SELECT ON *.* TO token_users")

        yield cluster
    finally:
        cluster.shutdown()


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


@pytest.fixture(autouse=True)
def clean_sts_log(started_cluster):
    started_cluster.exec_in_container(
        started_cluster.get_container_id(STS_CONTAINER),
        [
            "python3",
            "-c",
            "import urllib.request; urllib.request.urlopen('http://localhost:80/_reset').read()",
        ],
    )


def create_database(node, name):
    node.query(
        f"DROP DATABASE IF EXISTS {name}; "
        f"CREATE DATABASE {name} ENGINE = DataLakeCatalog('{BASE_URL}') "
        f"SETTINGS {','.join(k + '=' + repr(v) for k, v in DATABASE_SETTINGS.items())}",
        settings={"allow_database_glue_catalog": 1},
    )


def query_with_token(node, token, sql):
    response = node.http_request(
        "", method="POST", data=sql, headers={"Authorization": f"Bearer {token}"}
    )
    response.raise_for_status()


def test_user_tokens_are_exchanged_into_separate_sts_sessions(started_cluster):
    node = started_cluster.instances["node1"]
    db = f"glue_{uuid.uuid4().hex[:8]}"
    create_database(node, db)

    token = make_token("alice")
    query_with_token(node, token, f"SHOW TABLES FROM {db}")

    query_with_token(node, make_token("bob"), f"SHOW TABLES FROM {db}")
    sessions = sts_requests(started_cluster)
    assert sorted(request["role_session_name"] for request in sessions) == ["alice", "bob"]
    assert {request["web_identity_token"] for request in sessions} == {token, make_token("bob")}
    assert all(request["role_arn"] == ROLE_ARN for request in sessions)


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
