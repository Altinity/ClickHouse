import pytest
import uuid

from helpers.iceberg_utils import (
    get_uuid_str,
    create_iceberg_table,
    execute_spark_query_general,
)


@pytest.mark.parametrize("storage_type", ["s3"])
def test_remote_initiator_after_non_remote(started_cluster_iceberg_with_spark, storage_type):
    instance = started_cluster_iceberg_with_spark.instances["node1"]
    spark = started_cluster_iceberg_with_spark.spark_session
    TABLE_NAME = "test_remote_initiator_after_non_remote_table_" + get_uuid_str()
    VIEW_NAME = TABLE_NAME + "_view"

    def execute_spark_query(query: str):
        return execute_spark_query_general(
            spark,
            started_cluster_iceberg_with_spark,
            storage_type,
            TABLE_NAME,
            query,
        )

    execute_spark_query(
        f"""
            CREATE TABLE {TABLE_NAME} (
                tag INT,
                number INT
            )
            USING iceberg
            PARTITIONED BY (identity(tag))
            OPTIONS('format-version'='2')
        """
    )

    execute_spark_query(
        f"""
            INSERT INTO {TABLE_NAME} VALUES
            (1, 1)
        """
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster_iceberg_with_spark)

    def flush_logs():
        for node in started_cluster_iceberg_with_spark.instances.values():
            node.query("SYSTEM FLUSH LOGS")

    query_id = uuid.uuid4().hex
    res = instance.query(f"""
        SELECT * 
            FROM {TABLE_NAME}
            WHERE number=1
        SETTINGS
            object_storage_cluster='cluster_simple'
        """,
        query_id = query_id)
    assert res == "1\t1\n"
    flush_logs()
    queries = instance.query(f"""
        SELECT count()
            FROM clusterAllReplicas('cluster_simple', system.query_log)
            WHERE type='QueryFinish' AND initial_query_id='{query_id}'
        """)
    assert queries == "4\n"

    query_id = uuid.uuid4().hex
    res = instance.query(f"""
        SELECT * 
            FROM {TABLE_NAME}
            WHERE number=1
        SETTINGS
            object_storage_remote_initiator=1,
            object_storage_cluster='cluster_simple'
        """,
        query_id = query_id)
    assert res == "1\t1\n"
    flush_logs()
    queries = instance.query(f"""
        SELECT count()
            FROM clusterAllReplicas('cluster_simple', system.query_log)
            WHERE type='QueryFinish' AND initial_query_id='{query_id}'
        """)
    assert queries == "6\n"

    query_id = uuid.uuid4().hex
    res = instance.query(f"""
        SELECT * 
            FROM {TABLE_NAME}
            WHERE number=1
        """,
        query_id = query_id)
    assert res == "1\t1\n"
    flush_logs()
    queries = instance.query(f"""
        SELECT count()
            FROM clusterAllReplicas('cluster_simple', system.query_log)
            WHERE type='QueryFinish' AND initial_query_id='{query_id}'
        """)
    assert queries == "1\n"

