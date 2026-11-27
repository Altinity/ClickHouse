import pytest

from helpers.cluster import ClickHouseCluster


cluster = ClickHouseCluster(__file__)
node = cluster.add_instance(
    "node", main_configs=["configs/storage_conf.xml"], with_rustfs=True, stay_alive=True
)


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    try:
        cluster.start()
        yield
    finally:
        cluster.shutdown()


@pytest.fixture
def staged_table():
    node.query(
        "CREATE TABLE staged (n UInt64 CODEC(NONE)) ENGINE = MergeTree "
        "ORDER BY tuple() SETTINGS storage_policy = 'cas_staged', "
        "min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0"
    )
    try:
        yield
    finally:
        node.query("DROP TABLE staged SYNC")


rows = 2 * 1024 * 1024
insert_query = f"INSERT INTO staged SELECT number FROM numbers({rows})"
insert_settings = {
    "min_insert_block_size_rows": rows,
    "min_insert_block_size_bytes": 0,
    "max_insert_threads": 1,
}
multipart_counter = "SELECT sum(value) FROM system.events WHERE event = 'DiskS3UploadPart'"


def assert_staging_empty():
    bucket = cluster.rustfs_bucket
    cluster.rustfs_client.stat_object(bucket, "cas_staging_it/_pool_meta")
    objects = cluster.rustfs_client.list_objects(
        bucket, prefix="cas_staging_it/staging/itest-staging/", recursive=True
    )
    assert list(objects) == []


def test_multipart_staging_is_released_and_part_reads_after_restart(staged_table):
    before = int(node.query(multipart_counter))
    node.query(insert_query, settings=insert_settings)
    assert int(node.query(multipart_counter)) > before
    assert_staging_empty()

    node.restart_clickhouse()
    assert node.query("SELECT count(), sum(n) FROM staged").strip() == (
        f"{rows}\t{rows * (rows - 1) // 2}"
    )
    assert node.query("CHECK TABLE staged SETTINGS check_query_single_value_result = 1").strip() == "1"


def test_failed_disk_commit_releases_private_objects(staged_table):
    before = int(node.query(multipart_counter))
    failpoint = "part_storage_fail_commit_transaction"
    node.query(f"SYSTEM ENABLE FAILPOINT {failpoint}")
    try:
        error = node.query_and_get_error(insert_query, settings=insert_settings)
        assert failpoint in error
    finally:
        node.query(f"SYSTEM DISABLE FAILPOINT {failpoint}")

    assert int(node.query(multipart_counter)) > before
    assert node.query("SELECT count() FROM staged").strip() == "0"
    assert_staging_empty()
