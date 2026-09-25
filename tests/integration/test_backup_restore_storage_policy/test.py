import pytest

from helpers.cluster import ClickHouseCluster

backup_id_counter = 0

cluster = ClickHouseCluster(__file__)

instance = cluster.add_instance(
    "instance",
    main_configs=["configs/storage_config.xml"],
    with_rustfs=True,
)


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


@pytest.fixture(autouse=True)
def cleanup_after_test():
    try:
        yield
    finally:
        instance.query("DROP DATABASE IF EXISTS test")


def new_backup_name():
    global backup_id_counter
    backup_id_counter += 1
    return f"File('/backups/{backup_id_counter}/')"


def create_table_backup(backup_name, storage_policy=None):
    instance.query("CREATE DATABASE test")
    create_query = "CREATE TABLE test.table(x UInt32) ENGINE=MergeTree ORDER BY x"
    if storage_policy is not None:
        create_query += f" SETTINGS storage_policy = '{storage_policy}'"
    instance.query(create_query)
    instance.query("INSERT INTO test.table SELECT number FROM numbers(10)")
    assert instance.query("SELECT count(), sum(x) FROM test.table") == "10\t45\n"
    if storage_policy is not None:
        assert (
            instance.query("SELECT storage_policy FROM system.tables WHERE name='table'")
            == f"{storage_policy}\n"
        )
    instance.query(f"BACKUP TABLE test.table TO {backup_name}")
    instance.query("DROP TABLE test.table SYNC")


def restore_table(backup_name, storage_policy=None):
    restore_query = f"RESTORE TABLE test.table FROM {backup_name}"
    if storage_policy is not None:
        restore_query += f" SETTINGS storage_policy = '{storage_policy}'"
    instance.query(restore_query)


@pytest.mark.parametrize(
    "origin_policy, restore_policy, expected_policy",
    [
        (None, "", "default"),
        (None, None, "default"),
        (None, "policy1", "policy1"),
        ("policy1", "policy1", "policy1"),
        ("policy1", "policy2", "policy2"),
        ("policy1", "", "default"),
        ("policy1", None, "policy1"),
        ("cas_policy", None, "cas_policy"),
        ("cas_policy", "policy1", "policy1"),
        (None, "cas_policy", "cas_policy"),
        ("policy1", "cas_policy", "cas_policy"),
        ("cas_policy", "cas_policy", "cas_policy"),
    ],
)
def test_storage_policies(origin_policy, restore_policy, expected_policy):
    backup_name = new_backup_name()
    create_table_backup(backup_name, origin_policy)
    restore_table(backup_name, restore_policy)

    assert (
        instance.query("SELECT storage_policy FROM system.tables WHERE name='table'")
        == f"{expected_policy}\n"
    )
    assert instance.query("SELECT count(), sum(x) FROM test.table") == "10\t45\n"
    assert (
        instance.query(
            "SELECT sum(rows) FROM system.parts WHERE database = 'test' AND table = 'table' AND active"
        )
        == "10\n"
    )
    assert (
        instance.query("CHECK TABLE test.table SETTINGS check_query_single_value_result = 1")
        == "1\n"
    )
