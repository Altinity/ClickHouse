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
            object_storage_cluster='cluster_23'
        """,
        query_id = query_id)
    assert res == "1\t1\n"
    flush_logs()
    queries = instance.query(f"""
        SELECT count()
            FROM clusterAllReplicas('cluster_simple', system.query_log)
            WHERE type='QueryFinish' AND initial_query_id='{query_id}'
        """)
    # initial node + 2 subqueries on replicas
    assert queries == "3\n"

    query_id = uuid.uuid4().hex
    res = instance.query(f"""
        SELECT * 
            FROM {TABLE_NAME}
            WHERE number=1
        SETTINGS
            object_storage_remote_initiator=1,
            object_storage_cluster='cluster_23'
        """,
        query_id = query_id)
    assert res == "1\t1\n"
    flush_logs()
    queries = instance.query(f"""
        SELECT count()
            FROM clusterAllReplicas('cluster_simple', system.query_log)
            WHERE type='QueryFinish' AND initial_query_id='{query_id}'
        """)
    # initial node + remote initiator + 2 subqueries on replicas
    assert queries == "4\n"

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


@pytest.mark.parametrize("storage_type", ["s3"])
def test_remote_initiator_global_join_without_analyzer(started_cluster_iceberg_with_spark, storage_type):
    instance = started_cluster_iceberg_with_spark.instances["node1"]
    spark = started_cluster_iceberg_with_spark.spark_session
    TABLE_NAME = "test_remote_initiator_after_with_join_old_analyzer_table_" + get_uuid_str()
    TABLE2_NAME = "test_remote_initiator_after_with_join_old_analyzer_table_2_" + get_uuid_str()

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

    instance.query(f"CREATE TABLE {TABLE2_NAME} (tag INT, number2 INT) ENGINE=Memory")
    instance.query(f"INSERT INTO {TABLE2_NAME} VALUES (1, 2)")

    # `convertToRemote` picks one replica of `cluster_simple`. The Memory table exists only on
    # node1, so a single run can pass by landing there. Repeat until every replica is likely.
    for _ in range(9):
        res = instance.query(f"""
            SELECT t1.tag, t1.number, t2.number2
            FROM {TABLE_NAME} AS t1
            JOIN {TABLE2_NAME} AS t2 USING (tag)
            SETTINGS
                object_storage_remote_initiator=1,
                object_storage_remote_initiator_cluster='cluster_simple',
                object_storage_cluster_join_mode='global',
                allow_experimental_analyzer=0
            """)
        assert res == "1\t1\t2\n"

    # The alias `t2` is already a temporary table with a different row. The join must use the Memory row.
    # One client session, so the temporary table is visible to the SELECT.
    # The integration image has `/usr/bin/clickhouse` and no `clickhouse-client` on `PATH`.
    res = instance.exec_in_container(
        [
            "/usr/bin/clickhouse",
            "client",
            "--multiquery",
            "--query",
            f"""
            CREATE TEMPORARY TABLE t2 (tag INT, number2 INT);
            INSERT INTO t2 VALUES (1, 9);
            SELECT t1.tag, t1.number, t2.number2
            FROM {TABLE_NAME} AS t1
            JOIN {TABLE2_NAME} AS t2 USING (tag)
            SETTINGS
                object_storage_remote_initiator=1,
                object_storage_remote_initiator_cluster='cluster_simple',
                object_storage_cluster_join_mode='global',
                allow_experimental_analyzer=0
            """,
        ]
    )
    assert res == "1\t1\t2\n"

    res = instance.query(f"""
        SELECT t1.tag, t1.number
        FROM {TABLE_NAME} AS t1
        JOIN numbers(2) AS t2 ON t1.tag = toInt32(t2.number + 1)
        SETTINGS
            object_storage_remote_initiator=1,
            object_storage_remote_initiator_cluster='cluster_simple',
            object_storage_cluster_join_mode='global',
            allow_experimental_analyzer=0
        """)
    assert res == "1\t1\n"

    res = instance.query(f"""
        SELECT tag, number
        FROM {TABLE_NAME}
        WHERE tag IN (SELECT tag FROM {TABLE2_NAME})
        SETTINGS
            object_storage_remote_initiator=1,
            object_storage_remote_initiator_cluster='cluster_simple',
            object_storage_cluster_join_mode='global',
            allow_experimental_analyzer=0
        """)
    assert res == "1\t1\n"

    # `IN (SELECT 1)` has no table, so a replica must still see that subquery.
    # The sent text is `IN ((SELECT 1) AS _subqueryN)`. Choosing node1 runs in-process and writes no non-initial `query_log` row.
    saw_remote_subquery = False
    for _ in range(9):
        query_id = uuid.uuid4().hex
        res = instance.query(
            f"""
            SELECT tag, number
            FROM {TABLE_NAME}
            WHERE number IN (SELECT 1)
            SETTINGS
                object_storage_remote_initiator=1,
                object_storage_remote_initiator_cluster='cluster_simple',
                object_storage_cluster_join_mode='global',
                allow_experimental_analyzer=0
            """,
            query_id=query_id,
        )
        assert res == "1\t1\n"
        for node in started_cluster_iceberg_with_spark.instances.values():
            node.query("SYSTEM FLUSH LOGS")
        sent_with_subquery = instance.query(f"""
            SELECT count()
            FROM clusterAllReplicas('cluster_simple', system.query_log)
            WHERE initial_query_id = '{query_id}'
                AND type = 'QueryFinish'
                AND is_initial_query = 0
                AND position(query, '(SELECT 1)') > 0
            """)
        if sent_with_subquery != "0\n":
            saw_remote_subquery = True
            break
    assert saw_remote_subquery
