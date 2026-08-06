import logging
import time
import uuid
from typing import NamedTuple

import pytest

from helpers.cluster import ClickHouseCluster
from helpers.network import PartitionManager


def skip_if_remote_database_disk_enabled(cluster):
    """Skip test if any instance in the cluster has remote database disk enabled.

    Tests that block MinIO cannot run when remote database disk is enabled,
    as the database metadata is stored on MinIO and blocking it would break the database.
    """
    for instance in cluster.instances.values():
        if instance.with_remote_database_disk:
            pytest.skip("Test cannot run with remote database disk enabled (db disk), as it blocks MinIO which stores database metadata")


@pytest.fixture(scope="module")
def cluster():
    try:
        cluster = ClickHouseCluster(__file__)
        cluster.add_instance(
            "node1", 
            main_configs=["configs/named_collections.xml"],
            with_minio=True,
        )
        logging.info("Starting cluster...")
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def create_s3_table(node, s3_table):
    node.query(f"CREATE TABLE {s3_table} (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='{s3_table}', format=Parquet, partition_strategy='hive') PARTITION BY year")


def create_tables_and_insert_data(node, mt_table, s3_table):
    # enable_block_number_column and enable_block_offset_column are needed for patch parts support
    node.query(f"CREATE TABLE {mt_table} (id UInt64, year UInt16) ENGINE = MergeTree() PARTITION BY year ORDER BY tuple() SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1")
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020), (2, 2020), (3, 2020), (4, 2021)")

    create_s3_table(node, s3_table)


def test_drop_column_during_export_snapshot(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")

    mt_table = f"mutations_snapshot_mt_table_{postfix}"
    s3_table = f"mutations_snapshot_s3_table_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table)

    # Block traffic to/from MinIO to force upload errors and retries, following existing S3 tests style
    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    # Ensure export sees a consistent snapshot at start time even if we mutate the source later
    with PartitionManager() as pm:
        # Block responses from MinIO (source_port matches MinIO service)
        pm_rule_reject_responses = {
            "instance": node,
            "destination": node.ip_address,
            "protocol": "tcp",
            "source_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm.add_rule(pm_rule_reject_responses)

        # Block requests to MinIO (destination: MinIO, destination_port: minio_port)
        pm_rule_reject_requests = {
            "instance": node,
            "destination": minio_ip,
            "protocol": "tcp",
            "destination_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm.add_rule(pm_rule_reject_requests)

        # Start export of 2020
        node.query(
            f"ALTER TABLE {mt_table} EXPORT PART '2020_1_1_0' TO TABLE {s3_table};"
        )

        # Drop a column that is required for the export
        node.query(f"ALTER TABLE {mt_table} DROP COLUMN id")

        time.sleep(3)
        # assert the mutation has been applied AND the data has not been exported yet
        assert "Unknown expression identifier `id`" in node.query_and_get_error(f"SELECT id FROM {mt_table}"), "Column id is not removed"

    # Wait for export to finish and then verify destination still reflects the original snapshot (3 rows)
    time.sleep(5)
    assert node.query(f"SELECT count() FROM {s3_table} WHERE id >= 0") == '3\n', "Export did not preserve snapshot at start time after source mutation"


def test_add_column_during_export(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")

    mt_table = f"add_column_during_export_mt_table_{postfix}"
    s3_table = f"add_column_during_export_s3_table_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table)

    # Block traffic to/from MinIO to force upload errors and retries, following existing S3 tests style
    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    # Ensure export sees a consistent snapshot at start time even if we mutate the source later
    with PartitionManager() as pm:
        # Block responses from MinIO (source_port matches MinIO service)
        pm_rule_reject_responses = {
            "instance": node,
            "destination": node.ip_address,
            "protocol": "tcp",
            "source_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm.add_rule(pm_rule_reject_responses)

        # Block requests to MinIO (destination: MinIO, destination_port: minio_port)
        pm_rule_reject_requests = {
            "instance": node,
            "destination": minio_ip,
            "protocol": "tcp",
            "destination_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm.add_rule(pm_rule_reject_requests)

        # Start export of 2020
        node.query(
            f"ALTER TABLE {mt_table} EXPORT PART '2020_1_1_0' TO TABLE {s3_table};"
        )

        node.query(f"ALTER TABLE {mt_table} ADD COLUMN id2 UInt64")

        time.sleep(3)

        # assert the mutation has been applied AND the data has not been exported yet
        assert node.query(f"SELECT count(id2) FROM {mt_table}") == '4\n', "Column id2 is not added"

    # Wait for export to finish and then verify destination still reflects the original snapshot (3 rows)
    time.sleep(5)
    assert node.query(f"SELECT count() FROM {s3_table} WHERE id >= 0") == '3\n', "Export did not preserve snapshot at start time after source mutation"
    assert "Unknown expression identifier `id2`" in node.query_and_get_error(f"SELECT id2 FROM {s3_table}"), "Column id2 is present in the exported data"


def test_pending_mutations_throw_before_export(cluster):
    """Test that pending mutations before export throw an error with default settings."""
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")

    mt_table = f"pending_mutations_throw_mt_table_{postfix}"
    s3_table = f"pending_mutations_throw_s3_table_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table)

    node.query(f"SYSTEM STOP MERGES {mt_table}")

    node.query(f"ALTER TABLE {mt_table} UPDATE id = id + 100 WHERE year = 2020")

    mutations = node.query(f"SELECT count() FROM system.mutations WHERE table = '{mt_table}' AND is_done = 0")
    assert mutations.strip() != '0', "Mutation should be pending"

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '2020_1_1_0' TO TABLE {s3_table} SETTINGS export_merge_tree_part_throw_on_pending_mutations=true"
    )

    assert "PENDING_MUTATIONS_NOT_ALLOWED" in error, f"Expected error about pending mutations, got: {error}"


def test_pending_mutations_skip_before_export(cluster):
    """Test that pending mutations before export are skipped with throw_on_pending_mutations=false."""
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")

    mt_table = f"pending_mutations_skip_mt_table_{postfix}"
    s3_table = f"pending_mutations_skip_s3_table_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table)

    node.query(f"SYSTEM STOP MERGES {mt_table}")

    node.query(f"ALTER TABLE {mt_table} UPDATE id = id + 100 WHERE year = 2020")

    mutations = node.query(f"SELECT count() FROM system.mutations WHERE table = '{mt_table}' AND is_done = 0")
    assert mutations.strip() != '0', "Mutation should be pending"

    node.query(
        f"ALTER TABLE {mt_table} EXPORT PART '2020_1_1_0' TO TABLE {s3_table} "
        f"SETTINGS export_merge_tree_part_throw_on_pending_mutations=false"
    )

    time.sleep(5)

    result = node.query(f"SELECT id FROM {s3_table} WHERE year = 2020 ORDER BY id")
    assert "101" not in result and "102" not in result and "103" not in result, \
        "Export should contain original data before mutation"
    assert "1\n2\n3" in result, "Export should contain original data"


def test_data_mutations_after_export_started(cluster):
    """Test that mutations applied after export starts don't affect the exported data."""
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")

    mt_table = f"mutations_after_export_mt_table_{postfix}"
    s3_table = f"mutations_after_export_s3_table_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table)

    # Block traffic to MinIO to delay export
    minio_ip = cluster.minio_ip
    minio_port = cluster.minio_port

    with PartitionManager() as pm:
        pm_rule_reject_responses = {
            "instance": node,
            "destination": node.ip_address,
            "protocol": "tcp",
            "source_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm.add_rule(pm_rule_reject_responses)

        pm_rule_reject_requests = {
            "instance": node,
            "destination": minio_ip,
            "protocol": "tcp",
            "destination_port": minio_port,
            "action": "REJECT --reject-with tcp-reset",
        }
        pm.add_rule(pm_rule_reject_requests)

        node.query(
            f"ALTER TABLE {mt_table} EXPORT PART '2020_1_1_0' TO TABLE {s3_table} "
            f"SETTINGS export_merge_tree_part_throw_on_pending_mutations=true"
        )

        node.query(f"ALTER TABLE {mt_table} UPDATE id = id + 100 WHERE year = 2020")

    time.sleep(5)

    result = node.query(f"SELECT id FROM {s3_table} WHERE year = 2020 ORDER BY id")
    assert "1\n2\n3" in result, "Export should contain original data before mutation"
    assert "101" not in result, "Export should not contain mutated data"


def test_pending_patch_parts_throw_before_export(cluster):
    """Test that pending patch parts before export throw an error with default settings."""
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")

    mt_table = f"pending_patches_throw_mt_table_{postfix}"
    s3_table = f"pending_patches_throw_s3_table_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table)

    node.query(f"SYSTEM STOP MERGES {mt_table}")

    node.query(f"UPDATE {mt_table} SET id = id + 100 WHERE year = 2020")

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '2020_1_1_0' TO TABLE {s3_table}"
    )

    node.query(f"DROP TABLE {mt_table}")

    assert "PENDING_MUTATIONS_NOT_ALLOWED" in error or "pending patch parts" in error.lower(), \
        f"Expected error about pending patch parts, got: {error}"


def test_pending_patch_parts_skip_before_export(cluster):
    """Test that pending patch parts before export are skipped with throw_on_pending_patch_parts=false."""
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")

    mt_table = f"pending_patches_skip_mt_table_{postfix}"
    s3_table = f"pending_patches_skip_s3_table_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table)

    node.query(f"SYSTEM STOP MERGES {mt_table}")

    node.query(f"UPDATE {mt_table} SET id = id + 100 WHERE year = 2020")
    
    node.query(
        f"ALTER TABLE {mt_table} EXPORT PART '2020_1_1_0' TO TABLE {s3_table} "
        f"SETTINGS export_merge_tree_part_throw_on_pending_patch_parts=false"
    )

    time.sleep(5)

    result = node.query(f"SELECT id FROM {s3_table} WHERE year = 2020 ORDER BY id")
    assert "1\n2\n3" in result, "Export should contain original data before patch"

    node.query(f"DROP TABLE {mt_table}")


class RejectedPartExportCase(NamedTuple):
    src_columns: str
    src_partition_by: str
    dst_columns: str
    dst_partition_by: str
    insert_values: str
    error_substrings: tuple = ()


REJECTED_PART_EXPORT_CASES = [
    pytest.param(
        RejectedPartExportCase(
            src_columns="a Int32, b Int32",
            src_partition_by="a",
            dst_columns="b Int32, a Int32",
            dst_partition_by="a",
            insert_values="(1, 1), (1, 2)",
            error_substrings=("partition key column",),
        ),
        id="same_partition_key_different_column_order_single_column",
    ),
    pytest.param(
        RejectedPartExportCase(
            src_columns="a Int32, b Int32, c Int32, val String",
            src_partition_by="(a, b, c)",
            dst_columns="c Int32, b Int32, a Int32, val String",
            dst_partition_by="(a, b, c)",
            insert_values="(1, 1, 1, 'x'), (1, 1, 1, 'y')",
            error_substrings=("partition key column",),
        ),
        id="same_partition_key_different_column_order_multi_column",
    ),
    pytest.param(
        RejectedPartExportCase(
            src_columns="a Int32, b Int32, c Int32, val String",
            src_partition_by="(a, b, c)",
            dst_columns="a Int32, b Int32, c Int32, val String",
            dst_partition_by="(c, b, a)",
            insert_values="(1, 2, 3, 'x')",
            error_substrings=("different `PARTITION BY` expressions",),
        ),
        id="multi_column_partition_key_order_mismatch",
    ),
    pytest.param(
        RejectedPartExportCase(
            src_columns="a Int32, b Int32, c Int32, val String",
            src_partition_by="(a, b, c)",
            dst_columns="a Int32, b Int32, c Int32, val String",
            dst_partition_by="(a, b)",
            insert_values="(1, 2, 3, 'x')",
            error_substrings=("different `PARTITION BY` expressions",),
        ),
        id="multi_column_partition_key_fewer_in_destination",
    ),
    pytest.param(
        RejectedPartExportCase(
            src_columns="a Int32, b Int32, c Int32, val String",
            src_partition_by="(a, b)",
            dst_columns="a Int32, b Int32, c Int32, val String",
            dst_partition_by="(a, b, c)",
            insert_values="(1, 2, 3, 'x')",
            error_substrings=("different `PARTITION BY` expressions",),
        ),
        id="multi_column_partition_key_more_in_destination",
    ),
    pytest.param(
        RejectedPartExportCase(
            src_columns="id Int64, ts DateTime('UTC')",
            src_partition_by="ts",
            dst_columns="id Int64, ts DateTime('Asia/Tokyo')",
            dst_partition_by="ts",
            insert_values="(1, '2024-03-05 15:00:00')",
            error_substrings=("timezone",),
        ),
        id="partition_key_timezone_mismatch",
    ),
]


@pytest.mark.parametrize("case", REJECTED_PART_EXPORT_CASES)
def test_export_part_partition_key_mismatch_variants_are_rejected(cluster, case):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"rejected_mt_table_{postfix}"
    s3_table = f"rejected_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} ({case.src_columns})
        ENGINE = MergeTree()
        PARTITION BY {case.src_partition_by}
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} ({case.dst_columns})
        ENGINE = S3(s3_conn, filename='{s3_table}', format=Parquet, partition_strategy='hive')
        PARTITION BY {case.dst_partition_by}
    """)

    node.query(f"INSERT INTO {mt_table} VALUES {case.insert_values}")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}")
    assert "BAD_ARGUMENTS" in error, f"Expected BAD_ARGUMENTS, got: {error}"
    for substring in case.error_substrings:
        assert substring in error, f"Expected {substring!r} in error, got: {error}"

    count = int(node.query(f"SELECT count() FROM {s3_table}").strip())
    assert count == 0, f"Expected 0 rows in destination after rejected export, got {count}"


def test_export_part_multi_column_partition_key_success(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"multi_pkey_ok_mt_table_{postfix}"
    s3_table = f"multi_pkey_ok_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (a Int32, b Int32, c Int32, val String)
        ENGINE = MergeTree()
        PARTITION BY (a, b, c)
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (a Int32, b Int32, c Int32, val String)
        ENGINE = S3(s3_conn, filename='{s3_table}', format=Parquet, partition_strategy='hive')
        PARTITION BY (a, b, c)
    """)

    node.query(f"INSERT INTO {mt_table} VALUES (1, 2, 3, 'x'), (1, 2, 3, 'y')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    node.query(f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}")

    time.sleep(5)

    count = int(node.query(f"SELECT count() FROM {s3_table}").strip())
    assert count == 2, f"Expected 2 rows in destination after export, got {count}"

    result = node.query(f"SELECT a, b, c, val FROM {s3_table} ORDER BY val").strip()
    assert result == "1\t2\t3\tx\n1\t2\t3\ty", f"Unexpected exported data:\n{result}"


def test_export_part_non_partition_key_timezone_mismatch_is_allowed(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"tz_ok_mt_table_{postfix}"
    s3_table = f"tz_ok_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (id Int64, ts DateTime('UTC'))
        ENGINE = MergeTree()
        PARTITION BY id
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (id Int64, ts DateTime('Asia/Tokyo'))
        ENGINE = S3(s3_conn, filename='{s3_table}', format=Parquet, partition_strategy='hive')
        PARTITION BY id
    """)

    node.query(f"INSERT INTO {mt_table} VALUES (1, '2024-03-05 15:00:00')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    node.query(f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}")

    time.sleep(5)

    count = int(node.query(f"SELECT count() FROM {s3_table}").strip())
    assert count == 1, f"Expected 1 row in destination after export, got {count}"

    source_ts = node.query(f"SELECT ts FROM {mt_table}").strip()
    assert source_ts == "2024-03-05 15:00:00", f"Unexpected source value: {source_ts}"

    dest_ts = node.query(f"SELECT ts FROM {s3_table}").strip()
    assert dest_ts == "2024-03-06 00:00:00", (
        f"Expected the exported value to be the same instant displayed in the "
        f"destination's Asia/Tokyo timezone ('2024-03-06 00:00:00'), got: {dest_ts}"
    )

    source_unix_ts = int(node.query(f"SELECT toUnixTimestamp(ts) FROM {mt_table}").strip())
    dest_unix_ts = int(node.query(f"SELECT toUnixTimestamp(ts) FROM {s3_table}").strip())
    assert source_unix_ts == dest_unix_ts, (
        f"Expected exported DateTime value to be preserved regardless of the "
        f"destination column's timezone, got source={source_unix_ts}, dest={dest_unix_ts}"
    )


def test_export_part_tuple_subcolumn_partition_key_hive_destination_rejected(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    s3_table = f"tuple_subcol_s3_table_{postfix}"

    error = node.query_and_get_error(f"""
        CREATE TABLE {s3_table} (t Tuple(b Int32, a Int32), val String)
        ENGINE = S3(s3_conn, filename='{s3_table}', format=Parquet, partition_strategy='hive')
        PARTITION BY t.a
    """)
    assert "BAD_ARGUMENTS" in error and "part of the storage columns" in error, (
        f"Expected hive partition strategy to reject the tuple subcolumn "
        f"partition expression at CREATE time, got: {error!r}"
    )


def read_exported_files(node, s3_table):
    return node.query(
        f"SELECT t.a, t.b, val FROM "
        f"s3('http://minio1:9001/root/data/{s3_table}/**', 'minio', 'ClickHouse_Minio_P@ssw0rd', 'Parquet', "
        f"'t Tuple(b Int32, a Int32), val String') "
        f"WHERE _file NOT LIKE 'commit%'"
    ).strip()


def get_export_part_log(node, mt_table):
    node.query("SYSTEM FLUSH LOGS")
    return node.query(
        f"SELECT part_name, error, exception FROM system.part_log "
        f"WHERE event_type = 'ExportPart' AND database = currentDatabase() "
        f"AND table = '{mt_table}' ORDER BY event_time"
    ).strip()


def test_export_part_tuple_subcolumn_partition_key_wildcard_destination(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"tuple_subcol_wc_mt_table_{postfix}"
    s3_table = f"tuple_subcol_wc_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (t Tuple(a Int32, b Int32), val String)
        ENGINE = MergeTree()
        PARTITION BY t.a
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (t Tuple(b Int32, a Int32), val String)
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY t.a
    """)

    node.query(f"INSERT INTO {mt_table} VALUES ((1, 99), 'x')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    node.query(f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}")

    time.sleep(5)

    part_log = get_export_part_log(node, mt_table)
    result = read_exported_files(node, s3_table)
    assert result == "1\t99\tx", (
        f"Tuple element values were remapped: source t = (a=1, b=99), "
        f"exported files read back (t.a, t.b) as: {result!r}; part_log: {part_log!r}"
    )


def test_export_part_subcolumn_partition_key_different_subcolumn_is_rejected(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"subcol_diff_subcol_mt_table_{postfix}"
    s3_table = f"subcol_diff_subcol_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (a Tuple(b Int32, c Int32), val String)
        ENGINE = MergeTree()
        PARTITION BY a.b
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (a Tuple(b Int32, c Int32), val String)
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY a.c
    """)

    node.query(f"INSERT INTO {mt_table} VALUES ((1, 2), 'x')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    assert "BAD_ARGUMENTS" in error and "different `PARTITION BY` expressions" in error, (
        f"Both tables declare `a` as the same Tuple(b Int32, c Int32) (so the column-cast "
        f"check passes and the owner-name-only partition_key_column_set = {{'a'}} in "
        f"verifyExportSchemaCastable cannot distinguish `a.b` from `a.c`), but the source "
        f"partitions by `a.b` and the destination by `a.c` — a genuinely different "
        f"partition key that must be caught by the `PARTITION BY` AST comparison; "
        f"got: {error!r}"
    )


def test_export_part_tuple_subcolumn_partition_key_owner_column_reordered_is_rejected(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"tuple_subcol_owner_mt_table_{postfix}"
    s3_table = f"tuple_subcol_owner_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (t Tuple(a Int32, b Int32), decoy Tuple(a Int32, b Int32), val String)
        ENGINE = MergeTree()
        PARTITION BY t.a
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (decoy Tuple(a Int32, b Int32), t Tuple(a Int32, b Int32), val String)
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY t.a
    """)

    node.query(f"INSERT INTO {mt_table} VALUES ((1, 100), (2, 200), 'x')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    assert "BAD_ARGUMENTS" in error and "partition key column" in error, (
        f"Expected export to reject `t` and `decoy` swapping positions around the "
        f"partition key column `t.a`, the same way a plain (non-tuple) partition key "
        f"column position swap is rejected; got: {error!r}"
    )


def test_export_part_subcolumn_partition_key_timezone_mismatch_is_rejected(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"subcol_tz_mt_table_{postfix}"
    s3_table = f"subcol_tz_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (t Tuple(a Int32, ts DateTime('UTC')), val String)
        ENGINE = MergeTree()
        PARTITION BY t.ts
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (t Tuple(a Int32, ts DateTime('Asia/Tokyo')), val String)
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY t.ts
    """)

    node.query(f"INSERT INTO {mt_table} VALUES ((1, '2024-03-05 15:00:00'), 'x')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    assert "BAD_ARGUMENTS" in error and "timezone" in error, (
        f"Expected export to reject the nested partition key column `t.ts` changing "
        f"timezone from UTC to Asia/Tokyo between source and destination, the same way "
        f"a top-level DateTime partition key column with mismatched timezones is "
        f"rejected (see test_export_part_partition_key_mismatch_variants_are_rejected's "
        f"'partition_key_timezone_mismatch' case). getDateTimeTimeZoneName() is called "
        f"on the owning column's type (Tuple(...)), not on the actual DateTime "
        f"subcolumn's type, so it can never recognize a timezone at all here; "
        f"got: {error!r}"
    )


def test_export_part_multi_level_subcolumn_partition_key_owner_reordered_is_rejected(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"nested_subcol_owner_mt_table_{postfix}"
    s3_table = f"nested_subcol_owner_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (
            t Tuple(x Tuple(a Int32, b Int32), c Int32),
            decoy Tuple(x Tuple(a Int32, b Int32), c Int32),
            val String
        )
        ENGINE = MergeTree()
        PARTITION BY t.x.a
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (
            decoy Tuple(x Tuple(a Int32, b Int32), c Int32),
            t Tuple(x Tuple(a Int32, b Int32), c Int32),
            val String
        )
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY t.x.a
    """)

    node.query(f"INSERT INTO {mt_table} VALUES ((((1, 100), 1000)), (((2, 200), 2000)), 'x')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    assert "BAD_ARGUMENTS" in error and "partition key column" in error, (
        f"Expected export to reject `t` and `decoy` swapping positions around the "
        f"two-level-deep partition key column `t.x.a`. This only works if "
        f"getNameInStorage() resolves all the way to the top-level column `t`, not to "
        f"the intermediate level `t.x`; got: {error!r}"
    )


def test_export_part_multiple_subcolumn_partition_keys_owner_reordered_is_rejected(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"multi_subcol_key_mt_table_{postfix}"
    s3_table = f"multi_subcol_key_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (
            t Tuple(a Int32, x Int32),
            u Tuple(b Int32, y Int32),
            decoy Tuple(b Int32, y Int32),
            val String
        )
        ENGINE = MergeTree()
        PARTITION BY (t.a, u.b)
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (
            t Tuple(a Int32, x Int32),
            decoy Tuple(b Int32, y Int32),
            u Tuple(b Int32, y Int32),
            val String
        )
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY (t.a, u.b)
    """)

    node.query(f"INSERT INTO {mt_table} VALUES ((1, 10), (2, 20), (3, 30), 'x')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    assert "BAD_ARGUMENTS" in error and "partition key column 'u'" in error, (
        f"`t` (owner of key part `t.a`) stays at position 0 on both sides, so the guard "
        f"must independently catch `u` (owner of key part `u.b`) swapping positions "
        f"with `decoy` — a partition key with two subcolumn-owning columns must have "
        f"both validated, not just the first one encountered; got: {error!r}"
    )


def test_export_part_mixed_flat_and_subcolumn_partition_key_flat_part_reordered_is_rejected(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"mixed_key_mt_table_{postfix}"
    s3_table = f"mixed_key_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (a Int32, t Tuple(b Int32, c Int32), decoy Int32, val String)
        ENGINE = MergeTree()
        PARTITION BY (a, t.b)
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (decoy Int32, t Tuple(b Int32, c Int32), a Int32, val String)
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY (a, t.b)
    """)

    node.query(f"INSERT INTO {mt_table} VALUES (1, (2, 3), 4, 'x')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    assert "BAD_ARGUMENTS" in error and "partition key column 'a'" in error, (
        f"`t` (owner of key part `t.b`) stays at position 1 on both sides, so the guard "
        f"must independently catch the plain, non-tuple key part `a` swapping positions "
        f"with `decoy` — the pre-existing flat-column check and the new subcolumn-owner "
        f"resolution must both keep working when combined in one `PARTITION BY` "
        f"expression; got: {error!r}"
    )


def test_export_part_subcolumn_partition_key_owner_reordered_rejected_even_with_allow_lossy_cast(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"lossy_owner_mt_table_{postfix}"
    s3_table = f"lossy_owner_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (t Tuple(a Int32, b Int32), decoy Tuple(a Int32, b Int32), val String)
        ENGINE = MergeTree()
        PARTITION BY t.a
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (decoy Tuple(a Int32, b Int32), t Tuple(a Int32, b Int32), val String)
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY t.a
    """)

    node.query(f"INSERT INTO {mt_table} VALUES ((1, 100), (2, 200), 'x')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table} "
        f"SETTINGS export_merge_tree_part_allow_lossy_cast = 1"
    )
    assert "BAD_ARGUMENTS" in error and "partition key column" in error, (
        f"The partition-key position/name guard is checked before the "
        f"`allow_lossy_cast` early-continue in verifyExportSchemaCastable, so setting "
        f"`export_merge_tree_part_allow_lossy_cast = 1` must not suppress the rejection "
        f"of `t`/`decoy` swapping positions around the partition key column `t.a`; "
        f"got: {error!r}"
    )


def test_export_part_tuple_column_real_narrowing_same_order_is_rejected_diagnostic(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"tuple_narrow_same_order_mt_table_{postfix}"
    s3_table = f"tuple_narrow_same_order_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (t Tuple(a Int32, b Int64), val String)
        ENGINE = MergeTree()
        PARTITION BY val
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (t Tuple(a Int32, b Int32), val String)
        ENGINE = S3(s3_conn, filename='{s3_table}', format=Parquet, partition_strategy='hive')
        PARTITION BY val
    """)

    node.query(f"INSERT INTO {mt_table} VALUES ((100, 5000000000), 'x')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    print(f"DIAGNOSTIC SAME-ORDER-NARROWING ERROR: {error!r}")
    assert "INCOMPATIBLE_COLUMNS" in error and "lossy cast" in error, (
        f"Expected the genuinely narrowing `b` field (Int64 -> Int32, same field order "
        f"on both sides) to be rejected as a lossy cast; got: {error!r}"
    )


def test_export_part_tuple_column_fewer_fields_in_destination_is_rejected(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"tuple_fewer_fields_mt_table_{postfix}"
    s3_table = f"tuple_fewer_fields_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (t Tuple(a Int32, ts DateTime('UTC')), val String)
        ENGINE = MergeTree()
        PARTITION BY val
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (t Tuple(a Int32), val String)
        ENGINE = S3(s3_conn, filename='{s3_table}', format=Parquet, partition_strategy='hive')
        PARTITION BY val
    """)

    node.query(f"INSERT INTO {mt_table} VALUES ((1, '2024-03-05 15:00:00'), 'x')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    assert "INCOMPATIBLE_COLUMNS" in error and "lossy cast" in error, (
        f"`t` is not a partition key column here. The destination's `t` (Tuple(a Int32)) "
        f"has fewer fields than the source's `t` (Tuple(a Int32, ts DateTime('UTC'))), "
        f"which canBeSafelyCast's tuple arity check (lhs_type_elements_size != "
        f"to_tuple_type_elements.size()) must reject; got: {error!r}"
    )


def test_export_part_subcolumn_partition_key_tuple_fewer_fields_in_destination_is_rejected(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"subcol_key_fewer_fields_mt_table_{postfix}"
    s3_table = f"subcol_key_fewer_fields_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (t Tuple(a Int32, ts DateTime('UTC')), val String)
        ENGINE = MergeTree()
        PARTITION BY t.ts
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (t Tuple(a Int32), val String)
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY t.a
    """)

    node.query(f"INSERT INTO {mt_table} VALUES ((1, '2024-03-05 15:00:00'), 'x')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    assert "INCOMPATIBLE_COLUMNS" in error and "lossy cast" in error, (
        f"The partition key `t.ts` requires a field the destination's `t` doesn't have "
        f"at all, so verifyPartitionKeyColumn's timezone lookup finds "
        f"destination_resolved == nullopt for 't.ts' and defers (continue); the arity "
        f"mismatch must still be caught right after by canBeSafelyCast on the whole "
        f"`t` column; got: {error!r}"
    )


def test_export_part_tuple_column_fewer_fields_in_source_is_rejected(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"tuple_fewer_fields_src_mt_table_{postfix}"
    s3_table = f"tuple_fewer_fields_src_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (t Tuple(a Int32), val String)
        ENGINE = MergeTree()
        PARTITION BY val
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (t Tuple(a Int32, ts DateTime('UTC')), val String)
        ENGINE = S3(s3_conn, filename='{s3_table}', format=Parquet, partition_strategy='hive')
        PARTITION BY val
    """)

    node.query(f"INSERT INTO {mt_table} VALUES ((1,), 'x')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    assert "INCOMPATIBLE_COLUMNS" in error and "lossy cast" in error, (
        f"Reverse direction of the fewer-fields case: the source's `t` (Tuple(a Int32)) "
        f"has fewer fields than the destination's `t` (Tuple(a Int32, ts "
        f"DateTime('UTC'))). canBeSafelyCast's arity check is a plain size "
        f"inequality, so it must reject this direction too; got: {error!r}"
    )


def test_export_part_subcolumn_partition_key_tuple_fewer_fields_in_source_is_rejected(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"subcol_key_fewer_fields_src_mt_table_{postfix}"
    s3_table = f"subcol_key_fewer_fields_src_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (t Tuple(a Int32), val String)
        ENGINE = MergeTree()
        PARTITION BY t.a
        ORDER BY tuple()
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (t Tuple(a Int32, ts DateTime('UTC')), val String)
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY t.a
    """)

    node.query(f"INSERT INTO {mt_table} VALUES ((1,), 'x')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    assert "INCOMPATIBLE_COLUMNS" in error and "lossy cast" in error, (
        f"Here `t.a` (the partition key) exists and resolves fine on both sides, so "
        f"verifyPartitionKeyColumn's timezone check passes without throwing; the extra "
        f"`ts` field the destination has beyond the source's `t` must still be caught "
        f"by canBeSafelyCast's arity check, independent of the partition key guard "
        f"succeeding; got: {error!r}"
    )


def test_export_part_nullable_datetime_partition_key_timezone_mismatch_is_rejected(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"nullable_tz_mt_table_{postfix}"
    s3_table = f"nullable_tz_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (id Int32, ts Nullable(DateTime('UTC')))
        ENGINE = MergeTree()
        PARTITION BY ts
        ORDER BY id
        SETTINGS allow_nullable_key = 1, enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (id Int32, ts Nullable(DateTime('Asia/Tokyo')))
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY ts
    """)

    node.query(f"INSERT INTO {mt_table} VALUES (1, '2024-03-05 15:00:00')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    assert "BAD_ARGUMENTS" in error and "timezone" in error, (
        f"getDateTimeTimeZoneName() only recognizes bare DateTime/DateTime64, so wrapping "
        f"the partition key in Nullable(...) hides the timezone from it entirely; "
        f"canBeSafelyCast() provides no fallback here either, since "
        f"DataTypeDateTime::equals() treats any two DateTime types as equal regardless "
        f"of timezone by design, so this needs its own dedicated rejection - even "
        f"without export_merge_tree_part_allow_lossy_cast being set; got: {error!r}"
    )


def test_export_part_lowcardinality_datetime_partition_key_timezone_mismatch_is_rejected(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"lc_tz_mt_table_{postfix}"
    s3_table = f"lc_tz_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (id Int32, ts LowCardinality(DateTime('UTC')))
        ENGINE = MergeTree()
        PARTITION BY ts
        ORDER BY id
        SETTINGS allow_suspicious_low_cardinality_types = 1, enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (id Int32, ts LowCardinality(DateTime('Asia/Tokyo')))
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY ts
        SETTINGS allow_suspicious_low_cardinality_types = 1
    """)

    node.query(f"INSERT INTO {mt_table} VALUES (1, '2024-03-05 15:00:00')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    assert "BAD_ARGUMENTS" in error and "timezone" in error, (
        f"Same gap as the Nullable case, but for LowCardinality(DateTime(...)); "
        f"got: {error!r}"
    )


def test_export_part_timezone_invariant_expression_partition_key_is_allowed(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"tz_invariant_mt_table_{postfix}"
    s3_table = f"tz_invariant_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (id Int32, ts DateTime('UTC'))
        ENGINE = MergeTree()
        PARTITION BY toUnixTimestamp(ts)
        ORDER BY id
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (id Int32, ts DateTime('Asia/Tokyo'))
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY toUnixTimestamp(ts)
    """)

    node.query(f"INSERT INTO {mt_table} VALUES (1, '2024-03-05 15:00:00')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    node.query(f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}")

    time.sleep(3)
    result = node.query(
        f"SELECT count() FROM s3('http://minio1:9001/root/data/{s3_table}/**', "
        f"'minio', 'ClickHouse_Minio_P@ssw0rd', 'Parquet', "
        f"'id Int32, ts DateTime(\\'Asia/Tokyo\\')') "
        f"WHERE _file NOT LIKE 'commit%'"
    ).strip()
    assert result == "1", (
        f"Expected the export to succeed and produce 1 row: toUnixTimestamp(ts) does "
        f"not depend on ts's declared timezone, so a mismatched timezone between "
        f"source and destination must not block it; got count={result!r}"
    )


def test_export_part_timezone_sensitive_expression_partition_key_is_rejected(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"tz_sensitive_mt_table_{postfix}"
    s3_table = f"tz_sensitive_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (id Int32, ts DateTime('UTC'))
        ENGINE = MergeTree()
        PARTITION BY toDate(ts)
        ORDER BY id
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (id Int32, ts DateTime('Asia/Tokyo'))
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY toDate(ts)
    """)

    node.query(f"INSERT INTO {mt_table} VALUES (1, '2024-03-05 15:00:00')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    assert "BAD_ARGUMENTS" in error and "timezone" in error, (
        f"Unlike toUnixTimestamp(ts), toDate(ts) genuinely depends on ts's declared "
        f"timezone (day boundaries differ between UTC and Asia/Tokyo), so it must "
        f"remain protected by the timezone guard - the allowlist in "
        f"collectColumnsRequiringTimezoneCheck() must not be so broad that it lets "
        f"this through too; got: {error!r}"
    )


def test_export_part_timezone_sensitive_function_nested_in_invariant_function_is_rejected(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"tz_nested_mt_table_{postfix}"
    s3_table = f"tz_nested_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (id Int32, ts DateTime('UTC'))
        ENGINE = MergeTree()
        PARTITION BY toUnixTimestamp(toDate(ts))
        ORDER BY id
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (id Int32, ts DateTime('Asia/Tokyo'))
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY toUnixTimestamp(toDate(ts))
    """)

    node.query(f"INSERT INTO {mt_table} VALUES (1, '2024-03-05 15:00:00')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    assert "BAD_ARGUMENTS" in error and "timezone" in error, (
        f"toDate(ts) inside toUnixTimestamp(toDate(ts)) already produces a "
        f"timezone-dependent value before toUnixTimestamp ever runs, so wrapping it "
        f"in an outer invariant function must not exempt ts from the check - only "
        f"ts's *immediate* parent function determines that; got: {error!r}"
    )


def test_export_part_unix_timestamp64_second_partition_key_is_allowed(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"tz_u64s_mt_table_{postfix}"
    s3_table = f"tz_u64s_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (id Int32, ts DateTime64(3, 'UTC'))
        ENGINE = MergeTree()
        PARTITION BY toUnixTimestamp64Second(ts)
        ORDER BY id
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (id Int32, ts DateTime64(3, 'Asia/Tokyo'))
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY toUnixTimestamp64Second(ts)
    """)

    node.query(f"INSERT INTO {mt_table} VALUES (1, '2024-03-05 15:00:00.000')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    node.query(f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}")

    time.sleep(3)
    result = node.query(
        f"SELECT count() FROM s3('http://minio1:9001/root/data/{s3_table}/**', "
        f"'minio', 'ClickHouse_Minio_P@ssw0rd', 'Parquet', "
        f"'id Int32, ts DateTime64(3, \\'Asia/Tokyo\\')') "
        f"WHERE _file NOT LIKE 'commit%'"
    ).strip()
    assert result == "1", (
        f"toUnixTimestamp64Second(ts) is documented as being relative to UTC, not the "
        f"declared timezone of ts (see toUnixTimestamp64Second.cpp's own "
        f"documentation), so a mismatched timezone between source and destination "
        f"must not block this export; got count={result!r}"
    )


def test_export_part_value_preserving_function_wrapped_in_invariant_function_is_allowed(cluster):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"tz_passthrough_mt_table_{postfix}"
    s3_table = f"tz_passthrough_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (id Int32, ts DateTime('UTC'))
        ENGINE = MergeTree()
        PARTITION BY toUnixTimestamp(assumeNotNull(ts))
        ORDER BY id
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (id Int32, ts DateTime('Asia/Tokyo'))
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY toUnixTimestamp(assumeNotNull(ts))
    """)

    node.query(f"INSERT INTO {mt_table} VALUES (1, '2024-03-05 15:00:00')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    node.query(f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}")

    time.sleep(3)
    result = node.query(
        f"SELECT count() FROM s3('http://minio1:9001/root/data/{s3_table}/**', "
        f"'minio', 'ClickHouse_Minio_P@ssw0rd', 'Parquet', "
        f"'id Int32, ts DateTime(\\'Asia/Tokyo\\')') "
        f"WHERE _file NOT LIKE 'commit%'"
    ).strip()
    assert result == "1", (
        f"assumeNotNull(ts) is identity on the underlying value, so it must not break "
        f"the chain between ts and the enclosing toUnixTimestamp - the check must look "
        f"past value-preserving wrapper functions, not just the immediate parent; "
        f"got count={result!r}"
    )


def test_export_part_timezone_sensitive_function_behind_value_preserving_wrapper_is_rejected(
    cluster,
):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"tz_nested_passthrough_mt_table_{postfix}"
    s3_table = f"tz_nested_passthrough_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (id Int32, ts DateTime('UTC'))
        ENGINE = MergeTree()
        PARTITION BY toUnixTimestamp(assumeNotNull(toDate(ts)))
        ORDER BY id
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (id Int32, ts DateTime('Asia/Tokyo'))
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY toUnixTimestamp(assumeNotNull(toDate(ts)))
    """)

    node.query(f"INSERT INTO {mt_table} VALUES (1, '2024-03-05 15:00:00')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    assert "BAD_ARGUMENTS" in error and "timezone" in error, (
        f"toDate(ts) is still timezone-sensitive even when reached through the "
        f"value-preserving assumeNotNull() and wrapped by the invariant "
        f"toUnixTimestamp() two levels up - skipping past passthrough functions must "
        f"not also skip past genuinely timezone-sensitive ones; got: {error!r}"
    )


# `timezone_invariant_functions` in `collectColumnsRequiringTimezoneCheck` is a
# fixed allowlist, not a proof of timezone-independence. This test documents the
# conservative rejection of safe expressions that the allowlist does not recognize.
@pytest.mark.parametrize(
    "function_name, source_type, destination_type, value",
    [
        pytest.param(
            "toUInt32",
            "DateTime('UTC')",
            "DateTime('Asia/Tokyo')",
            "2024-03-05 15:00:00",
            id="toUInt32-DateTime",
        ),
        pytest.param(
            "toInt64",
            "DateTime64(3, 'UTC')",
            "DateTime64(3, 'Asia/Tokyo')",
            "2024-03-05 15:00:00.000",
            id="toInt64-DateTime64",
        ),
    ],
)
def test_export_part_unrecognized_timezone_invariant_function_is_rejected(
    cluster, function_name, source_type, destination_type, value
):
    skip_if_remote_database_disk_enabled(cluster)
    node = cluster.instances["node1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"tz_unrecognized_mt_table_{postfix}"
    s3_table = f"tz_unrecognized_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (id Int32, ts {source_type})
        ENGINE = MergeTree()
        PARTITION BY {function_name}(ts)
        ORDER BY id
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
    """)

    node.query(f"""
        CREATE TABLE {s3_table} (id Int32, ts {destination_type})
        ENGINE = S3(s3_conn, filename='{s3_table}/{{_partition_id}}/{{_file}}', format=Parquet, partition_strategy='wildcard')
        PARTITION BY {function_name}(ts)
    """)

    node.query(f"INSERT INTO {mt_table} VALUES (1, '{value}')")

    part_name = node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() "
        f"AND table = '{mt_table}' AND active ORDER BY name LIMIT 1"
    ).strip()

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PART '{part_name}' TO TABLE {s3_table}"
    )
    assert "BAD_ARGUMENTS" in error and "timezone" in error, (
        f"`{function_name}(ts)` uses the stored epoch value and does not perform a "
        f"calendar-time conversion, but `{function_name}` is not on the allowlist. "
        f"The export must be conservatively rejected; got: {error!r}"
    )
