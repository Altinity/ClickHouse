import logging
import pytest
import random
import string
import time
from typing import Optional
import uuid

from helpers.cluster import ClickHouseCluster
from helpers.network import PartitionManager

@pytest.fixture(scope="module")
def cluster():
    try:
        cluster = ClickHouseCluster(__file__)
        cluster.add_instance(
            "replica1", 
            main_configs=["configs/named_collections.xml"],
            user_configs=["configs/users.d/profile.xml"],
            with_minio=True,
            stay_alive=True,
            with_zookeeper=True,
        )
        cluster.add_instance(
            "replica2", 
            main_configs=["configs/named_collections.xml"],
            user_configs=["configs/users.d/profile.xml"],
            with_minio=True,
            stay_alive=True,
            with_zookeeper=True,
        )
        # node that does not participate in the export, but will have visibility over the s3 table
        cluster.add_instance(
            "watcher_node", 
            main_configs=["configs/named_collections.xml"],
            user_configs=[],
            with_minio=True,
        )
        logging.info("Starting cluster...")
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def create_s3_table(node, s3_table):
    node.query(f"CREATE TABLE {s3_table} (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='{s3_table}', format=Parquet, partition_strategy='hive') PARTITION BY year")


def create_tables_and_insert_data(node, mt_table, s3_table, replica_name):
    node.query(f"CREATE TABLE {mt_table} (id UInt64, year UInt16) ENGINE = ReplicatedMergeTree('/clickhouse/tables/{mt_table}', '{replica_name}') PARTITION BY year ORDER BY tuple()")
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020), (2, 2020), (3, 2020), (4, 2021)")

    create_s3_table(node, s3_table)


def test_restart_nodes_during_export(cluster):
    node = cluster.instances["replica1"]
    node2 = cluster.instances["replica2"]
    watcher_node = cluster.instances["watcher_node"]

    mt_table = "disaster_mt_table"
    s3_table = "disaster_s3_table"

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1")
    create_tables_and_insert_data(node2, mt_table, s3_table, "replica2")
    create_s3_table(watcher_node, s3_table)

    # Block S3/MinIO requests to keep exports alive via retry mechanism
    # This allows ZooKeeper operations to proceed quickly
    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    with PartitionManager() as pm:
        # Block responses from MinIO (source_port matches MinIO service)
        pm_rule_reject_responses_node1 = {
            "destination": node.ip_address,
            "source_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm._add_rule(pm_rule_reject_responses_node1)

        pm_rule_reject_responses_node2 = {
            "destination": node2.ip_address,
            "source_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm._add_rule(pm_rule_reject_responses_node2)

        # Block requests to MinIO (destination: MinIO, destination_port: minio_port)
        pm_rule_reject_requests = {
            "destination": minio_ip,
            "destination_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm._add_rule(pm_rule_reject_requests)
        
        export_queries = f"""
            ALTER TABLE {mt_table}
            EXPORT PARTITION ID '2020' TO TABLE {s3_table}
            SETTINGS allow_experimental_export_merge_tree_part=1, export_merge_tree_partition_max_retries = 50;
            ALTER TABLE {mt_table}
            EXPORT PARTITION ID '2021' TO TABLE {s3_table}
            SETTINGS allow_experimental_export_merge_tree_part=1, export_merge_tree_partition_max_retries = 50;
        """

        node.query(export_queries)

        # wait for the exports to start
        time.sleep(3)

        node.stop_clickhouse(kill=True)
        node2.stop_clickhouse(kill=True)

    assert watcher_node.query(f"SELECT count() FROM {s3_table} where year = 2020") == '0\n', "Partition 2020 was written to S3 during network delay crash"

    assert watcher_node.query(f"SELECT count() FROM {s3_table} where year = 2021") == '0\n', "Partition 2021 was written to S3 during network delay crash"

    # start the nodes, they should finish the export
    node.start_clickhouse()
    node2.start_clickhouse()

    time.sleep(5)

    assert node.query(f"SELECT count() FROM {s3_table} WHERE year = 2020") != f'0\n', "Export of partition 2020 did not resume after crash"

    assert node.query(f"SELECT count() FROM {s3_table} WHERE year = 2021") != f'0\n', "Export of partition 2021 did not resume after crash"


def test_kill_export(cluster):
    node = cluster.instances["replica1"]
    node2 = cluster.instances["replica2"]
    watcher_node = cluster.instances["watcher_node"]

    mt_table = "kill_export_mt_table"
    s3_table = "kill_export_s3_table"

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1")
    create_tables_and_insert_data(node2, mt_table, s3_table, "replica2")

    # Block S3/MinIO requests to keep exports alive via retry mechanism
    # This allows ZooKeeper operations (KILL) to proceed quickly
    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    with PartitionManager() as pm:
        # Block responses from MinIO (source_port matches MinIO service)
        pm_rule_reject_responses = {
            "destination": node.ip_address,
            "source_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm._add_rule(pm_rule_reject_responses)

        # Block requests to MinIO (destination: MinIO, destination_port: minio_port)
        pm_rule_reject_requests = {
            "destination": minio_ip,
            "destination_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm._add_rule(pm_rule_reject_requests)
        
        export_queries = f"""
            ALTER TABLE {mt_table}
            EXPORT PARTITION ID '2020' TO TABLE {s3_table}
            SETTINGS allow_experimental_export_merge_tree_part=1, export_merge_tree_partition_max_retries = 50;
            ALTER TABLE {mt_table}
            EXPORT PARTITION ID '2021' TO TABLE {s3_table}
            SETTINGS allow_experimental_export_merge_tree_part=1, export_merge_tree_partition_max_retries = 50;
        """

        node.query(export_queries)
    
        # Kill only 2020 while S3 is blocked - retry mechanism keeps exports alive
        # ZooKeeper operations (KILL) proceed quickly since only S3 is blocked
        node.query(f"KILL EXPORT PARTITION WHERE partition_id = '2020'")

    # wait for 2021 to finish
    time.sleep(5)

    # checking for the commit file because maybe the data file was too fast?
    assert node.query(f"SELECT count() FROM s3(s3_conn, filename='{s3_table}/commit_2020_*', format=LineAsString)") == '0\n', "Partition 2020 was written to S3, it was not killed as expected"
    assert node.query(f"SELECT count() FROM s3(s3_conn, filename='{s3_table}/commit_2021_*', format=LineAsString)") != f'0\n', "Partition 2021 was not written to S3, but it should have been"


def test_drop_source_table_during_export(cluster):
    node = cluster.instances["replica1"]
    # node2 = cluster.instances["replica2"]
    watcher_node = cluster.instances["watcher_node"]

    mt_table = "drop_source_table_during_export_mt_table"
    s3_table = "drop_source_table_during_export_s3_table"

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1")
    # create_tables_and_insert_data(node2, mt_table, s3_table, "replica2")
    create_s3_table(watcher_node, s3_table)

    # Block S3/MinIO requests to keep exports alive via retry mechanism
    # This allows ZooKeeper operations (KILL) to proceed quickly
    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    with PartitionManager() as pm:
        # Block responses from MinIO (source_port matches MinIO service)
        pm_rule_reject_responses = {
            "destination": node.ip_address,
            "source_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm._add_rule(pm_rule_reject_responses)

        # Block requests to MinIO (destination: MinIO, destination_port: minio_port)
        pm_rule_reject_requests = {
            "destination": minio_ip,
            "destination_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm._add_rule(pm_rule_reject_requests)
        
        export_queries = f"""
            ALTER TABLE {mt_table}
            EXPORT PARTITION ID '2020' TO TABLE {s3_table}
            SETTINGS allow_experimental_export_merge_tree_part=1;
            ALTER TABLE {mt_table}
            EXPORT PARTITION ID '2021' TO TABLE {s3_table}
            SETTINGS allow_experimental_export_merge_tree_part=1;
        """

        node.query(export_queries)

        # This should kill the background operations and drop the table
        node.query(f"DROP TABLE {mt_table}")

    # Sleep some time to let the export finish (assuming it was not properly cancelled)
    time.sleep(10)

    assert node.query(f"SELECT count() FROM s3(s3_conn, filename='{s3_table}/commit_*', format=LineAsString)") == '0\n', "Background operations completed even with the table dropped"


def test_drop_destination_table_during_export(cluster):
    node = cluster.instances["replica1"]
    # node2 = cluster.instances["replica2"]
    watcher_node = cluster.instances["watcher_node"]

    mt_table = "drop_destination_table_during_export_mt_table"
    s3_table = "drop_destination_table_during_export_s3_table"

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1")
    create_s3_table(watcher_node, s3_table)

    # Block S3/MinIO requests to keep exports alive via retry mechanism
    # This allows ZooKeeper operations (KILL) to proceed quickly
    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    with PartitionManager() as pm:
        # Block responses from MinIO (source_port matches MinIO service)
        pm_rule_reject_responses = {
            "destination": node.ip_address,
            "source_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm._add_rule(pm_rule_reject_responses)

        # Block requests to MinIO (destination: MinIO, destination_port: minio_port)
        pm_rule_reject_requests = {
            "destination": minio_ip,
            "destination_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm._add_rule(pm_rule_reject_requests)

        export_queries = f"""
            ALTER TABLE {mt_table}
            EXPORT PARTITION ID '2020' TO TABLE {s3_table}
            SETTINGS allow_experimental_export_merge_tree_part=1;
            ALTER TABLE {mt_table}
            EXPORT PARTITION ID '2021' TO TABLE {s3_table}
            SETTINGS allow_experimental_export_merge_tree_part=1;
        """

        node.query(export_queries)

        # The pointer to the destination table is still valid, so the write will continue
        node.query(f"DROP TABLE {s3_table}")

    # give some time for the export to finish
    time.sleep(10)

    # not sure this is the expected behavior, but adding until we make a decision
    assert node.query(f"SELECT count() FROM s3(s3_conn, filename='{s3_table}/commit_*', format=LineAsString)") != '0\n', "Background operations did not complete after dropping the destination table"


def test_kill_export_by_table(cluster):
    node = cluster.instances["replica1"]

    mt_table = "kill_granularity_by_table_mt"
    s3_table = "kill_granularity_by_table_s3"
    alt_mt_table = "kill_granularity_by_table_alt_mt"
    alt_s3_table = "kill_granularity_by_table_alt_s3"

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1")
    create_tables_and_insert_data(node, alt_mt_table, alt_s3_table, "replica1")

    # Block S3/MinIO requests to keep exports alive via retry mechanism
    # This allows ZooKeeper operations (KILL) to proceed quickly
    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    with PartitionManager() as pm:
        # Block responses from MinIO (source_port matches MinIO service)
        pm_rule_reject_responses = {
            "destination": node.ip_address,
            "source_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm._add_rule(pm_rule_reject_responses)

        # Block requests to MinIO (destination: MinIO, destination_port: minio_port)
        pm_rule_reject_requests = {
            "destination": minio_ip,
            "destination_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm._add_rule(pm_rule_reject_requests)

        # Start two exports for the same table and one export for another table concurrently
        node.query(
            f"""
            ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} SETTINGS allow_experimental_export_merge_tree_part=1, export_merge_tree_partition_max_retries = 50;
            ALTER TABLE {mt_table} EXPORT PARTITION ID '2021' TO TABLE {s3_table} SETTINGS allow_experimental_export_merge_tree_part=1, export_merge_tree_partition_max_retries = 50;
            ALTER TABLE {alt_mt_table} EXPORT PARTITION ID '2020' TO TABLE {alt_s3_table} SETTINGS allow_experimental_export_merge_tree_part=1, export_merge_tree_partition_max_retries = 50;
            """
        )

        # Kill all exports for the first table only while S3 is blocked
        # Retry mechanism keeps exports alive, ZooKeeper operations proceed quickly
        node.query(f"KILL EXPORT PARTITION WHERE source_table = '{mt_table}'")

    # Give some time for effects to propagate
    time.sleep(5)

    # The killed table should have no commit for either partition
    assert (
        node.query(
            f"SELECT count() FROM s3(s3_conn, filename='{s3_table}/commit_*', format=LineAsString)"
        )
        == '0\n'
    ), "Partition 2020 was written to S3, but KILL by table should have stopped it"

    # The alternate table (not killed) should complete
    assert (
        node.query(
            f"SELECT count() FROM s3(s3_conn, filename='{alt_s3_table}/commit_*', format=LineAsString)"
        )
        != '0\n'
    ), "Alternate table export was affected by KILL on a different table"


def test_concurrent_exports_to_different_targets(cluster):
    node = cluster.instances["replica1"]

    mt_table = "concurrent_diff_targets_mt_table"
    s3_table_a = "concurrent_diff_targets_s3_a"
    s3_table_b = "concurrent_diff_targets_s3_b"

    create_tables_and_insert_data(node, mt_table, s3_table_a, "replica1")
    create_s3_table(node, s3_table_b)

    # Launch two exports of the same partition to two different S3 tables concurrently
    with PartitionManager() as pm:
        pm.add_network_delay(node, delay_ms=1000)

        node.query(
            f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table_a} SETTINGS allow_experimental_export_merge_tree_part=1;"
        )
        node.query(
            f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table_b} SETTINGS allow_experimental_export_merge_tree_part=1;"
        )

    time.sleep(5)

    # Both targets should receive the same data independently
    assert node.query(f"SELECT count() FROM {s3_table_a} WHERE year = 2020") == '3\n', "First target did not receive expected rows"
    assert node.query(f"SELECT count() FROM {s3_table_b} WHERE year = 2020") == '3\n', "Second target did not receive expected rows"

    # And both should have a commit marker
    assert node.query(
        f"SELECT count() FROM s3(s3_conn, filename='{s3_table_a}/commit_2020_*', format=LineAsString)"
    ) != '0\n', "Commit file missing for first target"
    assert node.query(
        f"SELECT count() FROM s3(s3_conn, filename='{s3_table_b}/commit_2020_*', format=LineAsString)"
    ) != '0\n', "Commit file missing for second target"


def test_failure_is_logged_in_system_table(cluster):
    node = cluster.instances["replica1"]

    mt_table = "failure_is_logged_in_system_table_mt_table"
    s3_table = "failure_is_logged_in_system_table_s3_table"

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1")

    # Block traffic to/from MinIO to force upload errors and retries, following existing S3 tests style
    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    with PartitionManager() as pm:
        # Block responses from MinIO (source_port matches MinIO service)
        pm_rule_reject_responses = {
            "destination": node.ip_address,
            "source_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm._add_rule(pm_rule_reject_responses)

        # Also block requests to MinIO (destination: MinIO, destination_port: 9001) with REJECT to fail fast
        pm_rule_reject_requests = {
            "destination": minio_ip,
            "destination_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm._add_rule(pm_rule_reject_requests)

        node.query(
            f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} SETTINGS allow_experimental_export_merge_tree_part=1, export_merge_tree_partition_max_retries=1;"
        )

        # Wait so that the export fails
        time.sleep(5)

    # Network restored; verify the export is marked as FAILED in the system table
    # Also verify we captured at least one exception and no commit file exists
    status = node.query(
        f"""
        SELECT status FROM system.replicated_partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{s3_table}'
          AND partition_id = '2020'
        """
    )

    assert status.strip() == "FAILED", f"Expected FAILED status, got: {status!r}"

    exception_count = node.query(
        f"""
        SELECT any(exception_count) FROM system.replicated_partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{s3_table}'
          AND partition_id = '2020'
        """
    )
    assert int(exception_count.strip()) > 0, "Expected non-zero exception_count in system.replicated_partition_exports"

    # No commit should have been produced for this partition
    assert node.query(
        f"SELECT count() FROM s3(s3_conn, filename='{s3_table}/commit_2020_*', format=LineAsString)"
    ) == '0\n', "Commit file exists despite forced S3 failures"


def test_inject_short_living_failures(cluster):
    node = cluster.instances["replica1"]

    mt_table = "inject_short_living_failures_mt_table"
    s3_table = "inject_short_living_failures_s3_table"

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1")

    # Block traffic to/from MinIO to force upload errors and retries, following existing S3 tests style
    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    with PartitionManager() as pm:
        # Block responses from MinIO (source_port matches MinIO service)
        pm_rule_reject_responses = {
            "destination": node.ip_address,
            "source_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm._add_rule(pm_rule_reject_responses)

        # Also block requests to MinIO (destination: MinIO, destination_port: 9001) with REJECT to fail fast
        pm_rule_reject_requests = {
            "destination": minio_ip,
            "destination_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm._add_rule(pm_rule_reject_requests)

        # set big max_retries so that the export does not fail completely
        node.query(
            f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} SETTINGS allow_experimental_export_merge_tree_part=1, export_merge_tree_partition_max_retries=100;"
        )

        # wait only for a second to get at least one failure, but not enough to finish the export
        time.sleep(5)
    
    # wait for the export to finish
    time.sleep(5)

    # Assert the export succeeded
    assert node.query(f"SELECT count() FROM {s3_table} WHERE year = 2020") == '3\n', "Export did not succeed"
    assert node.query(f"SELECT count() FROM s3(s3_conn, filename='{s3_table}/commit_2020_*', format=LineAsString)") == '1\n', "Export did not succeed"

    # check system.replicated_partition_exports for the export
    assert node.query(
        f"""
        SELECT status FROM system.replicated_partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{s3_table}'
          AND partition_id = '2020'
        """
    ) == "COMPLETED\n", "Export should be marked as COMPLETED"

    exception_count = node.query(
        f"""
        SELECT exception_count FROM system.replicated_partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{s3_table}'
          AND partition_id = '2020'
        """
    )
    assert int(exception_count.strip()) >= 1, "Expected at least one exception"


def test_export_ttl(cluster):
    node = cluster.instances["replica1"]

    mt_table = "export_ttl_mt_table"
    s3_table = "export_ttl_s3_table"

    expiration_time = 5

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1")

    # start export
    node.query(f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} SETTINGS allow_experimental_export_merge_tree_part=1, export_merge_tree_partition_manifest_ttl={expiration_time};")

    # assert that I get an error when trying to export the same partition again, query_and_get_error
    error = node.query_and_get_error(f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} SETTINGS allow_experimental_export_merge_tree_part=1;")
    assert "Export with key" in error, "Expected error about expired export"

    # wait for the export to finish and for the manifest to expire
    time.sleep(expiration_time)

    # assert that the export succeeded, check the commit file
    assert node.query(f"SELECT count() FROM s3(s3_conn, filename='{s3_table}/commit_2020_*', format=LineAsString)") == '1\n', "Export did not succeed"

    # start export again
    node.query(f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} SETTINGS allow_experimental_export_merge_tree_part=1")

    # wait for the export to finish
    time.sleep(expiration_time)

    # assert that the export succeeded, check the commit file
    assert node.query(f"SELECT count() FROM s3(s3_conn, filename='{s3_table}/commit_2020_*', format=LineAsString)") == '1\n', "Export did not succeed"


# export an individual part with alter table export part
# and then try to export the partition. It should not fail because export partition is idempotent.
def test_export_part_and_partition(cluster):
    node = cluster.instances["replica1"]

    mt_table = "export_part_and_partition_mt_table"
    s3_table = "export_part_and_partition_s3_table"

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1")

    # stop merges so part names remain stable. it is important for the test.
    node.query(f"SYSTEM STOP MERGES {mt_table}")

    query_id_1 = uuid.uuid4().hex
    query_id_2 = uuid.uuid4().hex
    query_id_3 = uuid.uuid4().hex
    query_id_4 = uuid.uuid4().hex

    # Export all parts
    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} SETTINGS allow_experimental_export_merge_tree_part=1",
        query_id=query_id_1,
    )

    # check system.replicated_partition_exports for the export
    assert node.query(
        f"""
        SELECT status FROM system.replicated_partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{s3_table}'
          AND partition_id = '2020'
          AND transaction_id = '{query_id_1}'
        """
    ) == "COMPLETED\n", "Export should be marked as COMPLETED"

    # wait for the exports to finish
    time.sleep(3)

    # try to export the partition
    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} SETTINGS allow_experimental_export_merge_tree_part=1, export_merge_tree_partition_force_export=1",
        query_id=query_id_2,
    )

    time.sleep(3)

    # check system.replicated_partition_exports for the export
    assert node.query(
        f"""
        SELECT status FROM system.replicated_partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{s3_table}'
          AND partition_id = '2020'
          AND transaction_id = '{query_id_2}'
        """
    ) == "COMPLETED\n", "Export should be marked as COMPLETED"

    # now let's try with a file exists policy that is not NO_OP
    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} SETTINGS allow_experimental_export_merge_tree_part=1, export_merge_tree_partition_force_export=1, export_merge_tree_part_file_already_exists_policy='OVERWRITE'",
        query_id=query_id_3,
    )

    # wait for the export to finish
    time.sleep(3)

    # check system.replicated_partition_exports for the export
    assert node.query(
        f"""
        SELECT status FROM system.replicated_partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{s3_table}'
          AND partition_id = '2020'
          AND transaction_id = '{query_id_3}'
        """
    ) == "COMPLETED\n", "Export should be marked as COMPLETED"

    # last but not least, let's try with the error policy
    # max retries = 1 so it fails fast
    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} SETTINGS allow_experimental_export_merge_tree_part=1, export_merge_tree_partition_force_export=1, export_merge_tree_part_file_already_exists_policy='ERROR', export_merge_tree_partition_max_retries=1",
        query_id=query_id_4,
    )

    # wait for the export to finish
    time.sleep(3)

    # check system.replicated_partition_exports for the export
    assert node.query(
        f"""
        SELECT status FROM system.replicated_partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{s3_table}'
          AND partition_id = '2020'
          AND transaction_id = '{query_id_4}'
        """
    ) == "FAILED\n", "Export should be marked as FAILED"


# def test_source_mutations_during_export_snapshot(cluster):
#     node = cluster.instances["replica1"]

#     mt_table = "mutations_snapshot_mt_table"
#     s3_table = "mutations_snapshot_s3_table"

#     create_tables_and_insert_data(node, mt_table, s3_table, "replica1")

#     # Ensure export sees a consistent snapshot at start time even if we mutate the source later
#     with PartitionManager() as pm:
#         pm.add_network_delay(node, delay_ms=5000)

#         # Start export of 2020
#         node.query(
#             f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} SETTINGS allow_experimental_export_merge_tree_part=1;"
#         )

#     # Mutate the source after export started (delete the same partition)
#     node.query(f"ALTER TABLE {mt_table} DROP COLUMN id")

#     # assert the mutation has been applied AND the data has not been exported yet
#     assert node.query(f"SELECT count() FROM {mt_table} WHERE year = 2020") == '0\n', "Mutation has not been applied"
#     assert node.query(f"SELECT count() FROM {s3_table} WHERE year = 2020") == '0\n', "Data has been exported"

#     # Wait for export to finish and then verify destination still reflects the original snapshot (3 rows)
#     time.sleep(5)
#     assert node.query(f"SELECT count() FROM {s3_table} WHERE year = 2020") == '3\n', "Export did not preserve snapshot at start time after source mutation"
