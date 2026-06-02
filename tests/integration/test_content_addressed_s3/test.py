import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

STORAGE_POLICY = "content_addressed_s3"
NUM_ROWS = 1000


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    cluster.add_instance(
        "node",
        main_configs=["configs/storage_conf.xml"],
        with_minio=True,
        stay_alive=True,
    )

    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def test_content_addressed_s3():
    node = cluster.instances["node"]

    node.query("DROP TABLE IF EXISTS cas_test SYNC")
    node.query(
        """
        CREATE TABLE cas_test (
            id Int64,
            data String
        ) ENGINE = MergeTree()
        ORDER BY id
        SETTINGS storage_policy = '{}'
        """.format(
            STORAGE_POLICY
        )
    )

    # First insert of NUM_ROWS deterministic rows.
    node.query(
        "INSERT INTO cas_test SELECT number, toString(number) FROM numbers({})".format(
            NUM_ROWS
        )
    )

    expected_sum = (NUM_ROWS - 1) * NUM_ROWS // 2
    assert int(node.query("SELECT count() FROM cas_test")) == NUM_ROWS
    assert int(node.query("SELECT sum(id) FROM cas_test")) == expected_sum

    # A second identical insert: the row count doubles. Each part's content is identical, so the
    # content-addressed disk deduplicates the blobs, but the logical row count must still double.
    node.query(
        "INSERT INTO cas_test SELECT number, toString(number) FROM numbers({})".format(
            NUM_ROWS
        )
    )
    assert int(node.query("SELECT count() FROM cas_test")) == 2 * NUM_ROWS
    assert int(node.query("SELECT sum(id) FROM cas_test")) == 2 * expected_sum

    # Merge the two parts together.
    node.query("OPTIMIZE TABLE cas_test FINAL")
    assert int(node.query("SELECT count() FROM cas_test")) == 2 * NUM_ROWS
    assert int(node.query("SELECT sum(id) FROM cas_test")) == 2 * expected_sum

    # Persistence: after a restart the refs/footers/blobs in S3 must still resolve the data.
    node.restart_clickhouse()

    assert int(node.query("SELECT count() FROM cas_test")) == 2 * NUM_ROWS
    assert int(node.query("SELECT sum(id) FROM cas_test")) == 2 * expected_sum

    # Drop must complete without error (ref unlink + deferred GC).
    node.query("DROP TABLE cas_test SYNC")
    assert (
        node.query(
            "SELECT count() FROM system.tables WHERE database = currentDatabase() AND name = 'cas_test'"
        ).strip()
        == "0"
    )
