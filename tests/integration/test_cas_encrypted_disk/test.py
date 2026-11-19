import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)
node = cluster.add_instance(
    "node",
    main_configs=["configs/storage.xml"],
    with_rustfs=True,
)

TABLE = "encrypted_cas_insert"
INSERT = f"INSERT INTO {TABLE} SELECT number, toString(number) FROM numbers(1000)"


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


@pytest.fixture(autouse=True)
def cleanup_table(start_cluster):
    try:
        yield
    finally:
        node.query(f"DROP TABLE IF EXISTS {TABLE} SYNC")


def create_table(policy):
    node.query(
        f"""
        CREATE TABLE {TABLE} (id UInt64, data String)
        ENGINE = MergeTree
        ORDER BY id
        SETTINGS storage_policy = '{policy}',
                 min_bytes_for_wide_part = 0,
                 min_rows_for_wide_part = 0,
                 min_bytes_for_full_part_storage = 0,
                 min_rows_for_full_part_storage = 0,
                 min_level_for_full_part_storage = 0
        """
    )


def test_insert_into_encrypted_cas_is_rejected():
    create_table("cas_encrypted")

    error = node.query_and_get_error(INSERT)

    assert "NOT_IMPLEMENTED" in error, error
    assert (
        "Writing content part files through an encrypted content-addressed disk is not supported"
        in error
    ), error
    assert node.query(f"SELECT count() FROM {TABLE}") == "0\n"
    assert node.query(
        f"SELECT count() FROM system.parts "
        f"WHERE database = currentDatabase() AND table = '{TABLE}' AND active"
    ) == "0\n"


@pytest.mark.parametrize("policy", ["cas", "s3_encrypted"])
def test_insert_into_supported_disk(policy):
    create_table(policy)

    node.query(INSERT)

    assert node.query(
        f"SELECT count(), sum(id), countIf(data != toString(id)) FROM {TABLE}"
    ) == "1000\t499500\t0\n"
    assert node.query(
        f"SELECT DISTINCT disk_name FROM system.parts "
        f"WHERE database = currentDatabase() AND table = '{TABLE}' AND active"
    ) == f"disk_{policy}\n"
