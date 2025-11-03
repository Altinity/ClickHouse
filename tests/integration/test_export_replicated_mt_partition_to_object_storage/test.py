import logging
import pytest
import random
import string
import time
from typing import Optional

from helpers.cluster import ClickHouseCluster
from helpers.network import PartitionManager

@pytest.fixture(scope="module")
def cluster():
    try:
        cluster = ClickHouseCluster(__file__)
        cluster.add_instance(
            "replica1", 
            main_configs=["configs/named_collections.xml"],
            user_configs=[],
            with_minio=True,
            stay_alive=True,
            with_zookeeper=True,
        )
        cluster.add_instance(
            "replica2", 
            main_configs=["configs/named_collections.xml"],
            user_configs=[],
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

    # Add network delays so we can kill the node during the export
    with PartitionManager() as pm:
        pm.add_network_delay(node, delay_ms=1000)
        
        export_queries = f"""
            ALTER TABLE {mt_table}
            EXPORT PARTITION ID '2020' TO TABLE {s3_table}
            SETTINGS allow_experimental_export_merge_tree_part=1;
            ALTER TABLE {mt_table}
            EXPORT PARTITION ID '2021' TO TABLE {s3_table}
            SETTINGS allow_experimental_export_merge_tree_part=1;
        """

        node.query(export_queries)
    
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

    with PartitionManager() as pm:
        pm.add_network_delay(node, delay_ms=1000)
        
        export_queries = f"""
            ALTER TABLE {mt_table}
            EXPORT PARTITION ID '2020' TO TABLE {s3_table}
            SETTINGS allow_experimental_export_merge_tree_part=1;
            ALTER TABLE {mt_table}
            EXPORT PARTITION ID '2021' TO TABLE {s3_table}
            SETTINGS allow_experimental_export_merge_tree_part=1;
        """

        node.query(export_queries)
    
    # kill only 2020, 2021 should still finish
    node.query(f"KILL EXPORT PARTITION WHERE partition_id = '2020'")

    # wait for 2021 to finish
    time.sleep(5)

    # checking for the commit file because maybe the data file was too fast?
    assert node.query(f"SELECT count() FROM s3(s3_conn, filename='{s3_table}/commit_2020_*', format=LineAsString)") == '0\n', "Partition 2020 was written to S3, it was not killed as expected"
    assert node.query(f"SELECT count() FROM s3(s3_conn, filename='{s3_table}/commit_2021_*', format=LineAsString)") != f'0\n', "Partition 2021 was not written to S3, but it should have been"


def test_drop_table_during_export(cluster):
    node = cluster.instances["replica1"]
    # node2 = cluster.instances["replica2"]
    watcher_node = cluster.instances["watcher_node"]

    mt_table = "drop_table_during_export_mt_table"
    s3_table = "drop_table_during_export_s3_table"

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1")
    # create_tables_and_insert_data(node2, mt_table, s3_table, "replica2")
    create_s3_table(watcher_node, s3_table)

    with PartitionManager() as pm:
        pm.add_network_delay(node, delay_ms=1000)
        
        export_queries = f"""
            ALTER TABLE {mt_table}
            EXPORT PARTITION ID '2020' TO TABLE {s3_table}
            SETTINGS allow_experimental_export_merge_tree_part=1;
            ALTER TABLE {mt_table}
            EXPORT PARTITION ID '2021' TO TABLE {s3_table}
            SETTINGS allow_experimental_export_merge_tree_part=1;
        """

        node.query(export_queries)

    # I think this will actually wait until background operations are finished
    node.query(f"DROP TABLE {mt_table} SYNC")
    # this will not wait, but the pointer the background task holds is still valid, so the write will finish
    node.query(f"DROP TABLE {s3_table}")

    time.sleep(5)

    assert node.query(f"SELECT count() FROM s3(s3_conn, filename='{s3_table}/commit_*', format=LineAsString)") != '0\n', "Background operations finished even after the tables were dropped"
