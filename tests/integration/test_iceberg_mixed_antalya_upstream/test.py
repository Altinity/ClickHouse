import pytest

from helpers.cluster import ClickHouseCluster
from helpers.config_cluster import minio_access_key, minio_secret_key

# Four-part tag. `latest` and `26.6` move when a new build is published.
UPSTREAM_IMAGE = "clickhouse/clickhouse-server"
UPSTREAM_TAG = "26.6.8.7"

cluster = ClickHouseCluster(__file__)
antalya = cluster.add_instance(
    "antalya",
    main_configs=["configs/config.d/cluster.xml"],
    with_minio=True,
)
upstream = cluster.add_instance(
    "upstream",
    main_configs=["configs/config.d/cluster.xml"],
    image=UPSTREAM_IMAGE,
    tag=UPSTREAM_TAG,
    with_installed_binary=True,
    with_remote_database_disk=False,
)

TABLE_URL = "http://minio1:9001/root/mixed_antalya_upstream/"


@pytest.fixture(scope="module")
def started_cluster():
    try:
        cluster.start()
        antalya.query(
            f"""
            CREATE TABLE mixed_proto (id Int32, tag String)
            ENGINE = IcebergS3('{TABLE_URL}', '{minio_access_key}', '{minio_secret_key}')
            """
        )
        # Two files, each with a constant `tag`, so the Antalya read optimization has
        # something to send. The other side must still return these values.
        antalya.query(
            "INSERT INTO mixed_proto VALUES (1, 'alpha')",
            settings={"allow_insert_into_iceberg": 1},
        )
        antalya.query(
            "INSERT INTO mixed_proto VALUES (2, 'beta')",
            settings={"allow_insert_into_iceberg": 1},
        )
        yield cluster
    finally:
        cluster.shutdown()


@pytest.mark.parametrize(
    ("initiator_name", "cluster_name", "settings"),
    [
        pytest.param(
            "antalya",
            "upstream_only",
            # Default 500 makes the Antalya initiator send a JSON retry command in the task
            # path. An upstream worker would open that text as an object key.
            {"lock_object_storage_task_distribution_ms": 0},
            id="antalya_initiator_upstream_worker",
        ),
        pytest.param(
            "upstream",
            "antalya_only",
            None,
            id="upstream_initiator_antalya_worker",
        ),
    ],
)
def test_mixed_iceberg_cluster_read(started_cluster, initiator_name, cluster_name, settings):
    expected = antalya.query(f"SELECT id, tag FROM icebergS3('{TABLE_URL}', '{minio_access_key}', '{minio_secret_key}') ORDER BY id")
    initiator = started_cluster.instances[initiator_name]
    got = initiator.query(
        f"SELECT id, tag FROM icebergS3Cluster('{cluster_name}', '{TABLE_URL}', '{minio_access_key}', '{minio_secret_key}') ORDER BY id",
        settings=settings,
    )
    assert got == expected
    assert got == "1\talpha\n2\tbeta\n"
