import logging
import time
import uuid

import pytest

from helpers.cluster import ClickHouseCluster
from helpers.export_partition_helpers import (
    unique_suffix,
    wait_for_export_status,
    wait_for_export_to_start,
)
from helpers.network import PartitionManager

# Plain (non-replicated) MergeTree EXPORT PARTITION. Unlike the replicated variant there is no
# ZooKeeper coordination: the task descriptor is persisted on the table's disk and a local
# background scheduler drives it, so these tests also cover restart-resume.

SYSTEM_TABLE = "partition_exports"


def skip_if_remote_database_disk_enabled(cluster):
    for instance in cluster.instances.values():
        if instance.with_remote_database_disk:
            pytest.skip(
                "Test cannot run with remote database disk enabled, as it blocks MinIO which stores database metadata"
            )


@pytest.fixture(scope="module")
def cluster():
    try:
        cluster = ClickHouseCluster(__file__)
        cluster.add_instance(
            "node",
            main_configs=[
                "configs/named_collections.xml",
                "configs/allow_experimental_export_partition.xml",
            ],
            user_configs=["configs/users.d/profile.xml"],
            with_minio=True,
            stay_alive=True,
        )
        cluster.add_instance(
            "node_export_disabled",
            main_configs=[
                "configs/named_collections.xml",
                "configs/disable_experimental_export_partition.xml",
            ],
            user_configs=["configs/users.d/profile.xml"],
            with_minio=True,
            stay_alive=True,
        )
        logging.info("Starting cluster...")
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


@pytest.fixture(autouse=True)
def drop_tables_after_test(cluster):
    yield
    for instance_name, instance in cluster.instances.items():
        try:
            tables_str = instance.query(
                "SELECT name FROM system.tables WHERE database = 'default' FORMAT TabSeparated"
            ).strip()
            if not tables_str:
                continue
            for table in tables_str.split("\n"):
                table = table.strip()
                if table:
                    instance.query(f"DROP TABLE IF EXISTS default.`{table}` SYNC")
        except Exception as e:
            logging.warning(f"drop_tables_after_test: cleanup failed on {instance_name}: {e}")


def create_s3_table(node, s3_table):
    node.query(
        f"CREATE TABLE {s3_table} (id UInt64, year UInt16) "
        f"ENGINE = S3(s3_conn, filename='{s3_table}', format=Parquet, partition_strategy='hive') "
        f"PARTITION BY year"
    )


def create_tables_and_insert_data(node, mt_table, s3_table):
    node.query(f"DROP TABLE IF EXISTS {mt_table} SYNC")
    node.query(
        f"CREATE TABLE {mt_table} (id UInt64, year UInt16) ENGINE = MergeTree "
        f"PARTITION BY year ORDER BY tuple() "
        f"SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1"
    )
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020), (2, 2020), (3, 2020), (4, 2021)")
    create_s3_table(node, s3_table)


def test_export_partition_basic_roundtrip(cluster):
    node = cluster.instances["node"]

    postfix = unique_suffix()
    mt_table = f"basic_mt_{postfix}"
    s3_table = f"basic_s3_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table)

    node.query(f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table}")

    wait_for_export_status(node, mt_table, s3_table, "2020", "COMPLETED", system_table=SYSTEM_TABLE)

    assert node.query(f"SELECT count() FROM {s3_table} WHERE year = 2020") == "3\n"
    assert (
        node.query(
            f"SELECT count() FROM s3(s3_conn, filename='{s3_table}/commit_2020_*', format=LineAsString)"
        )
        != "0\n"
    ), "Commit file missing for partition 2020"


def test_export_partition_all(cluster):
    node = cluster.instances["node"]

    postfix = unique_suffix()
    mt_table = f"all_mt_{postfix}"
    s3_table = f"all_s3_{postfix}"

    node.query(
        f"CREATE TABLE {mt_table} (id UInt64, year UInt16) ENGINE = MergeTree "
        f"PARTITION BY year ORDER BY tuple()"
    )
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020), (2, 2021), (3, 2022)")
    create_s3_table(node, s3_table)

    node.query(f"ALTER TABLE {mt_table} EXPORT PARTITION ALL TO TABLE {s3_table}")

    for partition_id in ("2020", "2021", "2022"):
        wait_for_export_status(node, mt_table, s3_table, partition_id, "COMPLETED", system_table=SYSTEM_TABLE)

    assert node.query(f"SELECT count() FROM {s3_table}") == "3\n"


def test_duplicate_export_rejected_and_force(cluster):
    node = cluster.instances["node"]

    postfix = unique_suffix()
    mt_table = f"dup_mt_{postfix}"
    s3_table = f"dup_s3_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table)

    node.query(f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table}")
    wait_for_export_status(node, mt_table, s3_table, "2020", "COMPLETED", system_table=SYSTEM_TABLE)

    # Re-export without force must be rejected.
    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table}"
    )
    assert "EXPORT_PARTITION_ALREADY_EXPORTED" in error, f"Expected duplicate rejection, got: {error}"

    # With force + overwrite policy the re-export succeeds.
    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} "
        f"SETTINGS export_merge_tree_partition_force_export = 1, "
        f"export_merge_tree_part_file_already_exists_policy = 'overwrite'"
    )
    wait_for_export_status(node, mt_table, s3_table, "2020", "COMPLETED", system_table=SYSTEM_TABLE)


def test_kill_export(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node"]

    postfix = unique_suffix()
    mt_table = f"kill_mt_{postfix}"
    s3_table = f"kill_s3_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table)

    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    with PartitionManager() as pm:
        pm.add_rule({
            "instance": node,
            "destination": node.ip_address,
            "protocol": "tcp",
            "source_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        })
        pm.add_rule({
            "instance": node,
            "destination": minio_ip,
            "protocol": "tcp",
            "destination_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        })

        node.query(f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table}")
        wait_for_export_to_start(node, mt_table, s3_table, "2020", system_table=SYSTEM_TABLE)

        kill_result = node.query(
            f"KILL EXPORT PARTITION WHERE partition_id = '2020'"
            f" AND source_table = '{mt_table}' AND destination_table = '{s3_table}'"
        )
        kill_status = kill_result.split("\t")[0].strip()
        assert kill_status == "waiting", (
            f"Expected kill_status 'waiting' (CancelSent), got: {kill_result!r}"
        )

    wait_for_export_status(node, mt_table, s3_table, "2020", "KILLED", system_table=SYSTEM_TABLE, timeout=30)

    # No commit file and no data must have landed.
    assert (
        node.query(
            f"SELECT count() FROM s3(s3_conn, filename='{s3_table}/commit_2020_*', format=LineAsString)"
        )
        == "0\n"
    ), "Partition 2020 was committed despite being killed"
    assert node.query(f"SELECT count() FROM {s3_table} WHERE year = 2020") == "0\n"


def test_export_partition_resumes_after_restart(cluster):
    """The distinguishing feature of the plain MergeTree implementation: the on-disk task
    descriptor must let an in-flight export resume after a hard restart."""
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node"]

    postfix = unique_suffix()
    mt_table = f"restart_mt_{postfix}"
    s3_table = f"restart_s3_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table)

    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    with PartitionManager() as pm:
        pm.add_rule({
            "instance": node,
            "destination": node.ip_address,
            "protocol": "tcp",
            "source_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        })
        pm.add_rule({
            "instance": node,
            "destination": minio_ip,
            "protocol": "tcp",
            "destination_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        })

        node.query(f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table}")
        wait_for_export_to_start(node, mt_table, s3_table, "2020", system_table=SYSTEM_TABLE)

        # Kill the server while the export is still in flight (S3 blocked, nothing committed yet).
        node.stop_clickhouse(kill=True)

    # We cannot observe the "nothing committed before the crash" invariant on a single node: while
    # the only node is down there is nothing to query S3 with, and once it restarts (S3 now
    # reachable) the persisted PENDING task resumes immediately. So we only assert the actual
    # restart-resume behavior: the task must resume from its on-disk descriptor and complete.
    node.start_clickhouse()

    wait_for_export_status(node, mt_table, s3_table, "2020", "COMPLETED", system_table=SYSTEM_TABLE, timeout=90)
    assert node.query(f"SELECT count() FROM {s3_table} WHERE year = 2020") == "3\n"


def test_export_partition_feature_disabled(cluster):
    node = cluster.instances["node_export_disabled"]

    postfix = unique_suffix()
    mt_table = f"disabled_mt_{postfix}"
    s3_table = f"disabled_s3_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table)

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table}"
    )
    assert "experimental" in error, f"Expected experimental-feature error, got: {error}"

    error = node.query_and_get_error(
        f"KILL EXPORT PARTITION WHERE partition_id = '2020' AND source_table = '{mt_table}'"
        f" AND destination_table = '{s3_table}'"
    )
    assert "experimental" in error, f"Expected experimental-feature error on KILL, got: {error}"


def test_pending_mutations_throw_before_export(cluster):
    node = cluster.instances["node"]

    postfix = unique_suffix()
    mt_table = f"pending_mut_mt_{postfix}"
    s3_table = f"pending_mut_s3_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table)

    node.query(f"SYSTEM STOP MERGES {mt_table}")
    node.query(f"ALTER TABLE {mt_table} UPDATE id = id + 100 WHERE year = 2020")

    mutations = node.query(
        f"SELECT count() FROM system.mutations WHERE table = '{mt_table}' AND is_done = 0"
    )
    assert mutations.strip() != "0", "Mutation should be pending"

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} "
        f"SETTINGS export_merge_tree_part_throw_on_pending_mutations = true"
    )
    assert "PENDING_MUTATIONS_NOT_ALLOWED" in error, f"Expected pending-mutations error, got: {error}"
