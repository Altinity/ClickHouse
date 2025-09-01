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
            "node", 
            main_configs=["configs/named_collections.xml"],
            user_configs=[],
            with_minio=True,
            stay_alive=True,
        )
        cluster.add_instance(
            "node2", 
            main_configs=["configs/named_collections.xml"],
            user_configs=[],
            with_minio=True,
        )
        logging.info("Starting cluster...")
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def test_export_partition_with_network_delays(cluster):
    """Test server kill during export with network delays."""
    node = cluster.instances["node"] 
    table_name = "disaster_test_network"
    s3_table = "destination_s3_network"
    
    node.query(f"""
        CREATE TABLE {table_name} (
            id UInt64,
            year UInt16, 
            data String
        ) ENGINE = MergeTree()
        PARTITION BY year
        ORDER BY id
    """)

    node.query(f"INSERT INTO {table_name} VALUES (1, 2020, 'a'), (2, 2020, 'b'), (3, 2021, 'c')")
    
    node.query(f"""
        CREATE TABLE {s3_table} (
            id UInt64, 
            year UInt16, 
            data String, 
        ) ENGINE = S3(s3_conn, filename='disaster-recovery-network', format=Parquet, partition_strategy='hive') 
        PARTITION BY year
    """)

    node2 = cluster.instances["node2"]
    node2.query(f"""
        CREATE TABLE {s3_table} (
            id UInt64, 
            year UInt16, 
            data String, 
        ) ENGINE = S3(s3_conn, filename='disaster-recovery-network', format=Parquet, partition_strategy='hive') 
        PARTITION BY year
    """)
    
    with PartitionManager() as pm:
        pm.add_network_delay(node, delay_ms=1000)  # 5 second delays
        
        export_queries = f"""
            ALTER TABLE {table_name}
            EXPORT PARTITION 2020 TO TABLE {s3_table}
            SETTINGS allow_experimental_export_merge_tree_partition=1;
            ALTER TABLE {table_name}
            EXPORT PARTITION 2021 TO TABLE {s3_table}
            SETTINGS allow_experimental_export_merge_tree_partition=1;
        """

        node.query(export_queries)
        
    # Kill server
    logging.info("Killing server during network-delayed export")
    node.stop_clickhouse(kill=True)

    # check s3 to make sure no data was written
    assert node2.query(f"SELECT count() FROM {s3_table} where year = 2020") == '0\n', "Partition 2020 was written to S3 during network delay crash"

    assert node2.query(f"SELECT count() FROM {s3_table} where year = 2021") == '0\n', "Partition 2021 was written to S3 during network delay crash"

    node.start_clickhouse()

    # wait for the export to resume and complete
    time.sleep(5)
    
    # verify that the export has been resumed and completed
    assert node.query(f"SELECT count() FROM {s3_table} WHERE year = 2020") != f'0\n', "Export of partition 2020 did not resume after crash"

    assert node.query(f"SELECT count() FROM {s3_table} WHERE year = 2020") != f'0\n', "Export of partition 2021 did not resume after crash"

