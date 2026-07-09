import logging

import pytest

from helpers.cluster import ClickHouseCluster
from helpers.export_partition_helpers import (
    make_iceberg_s3,
    make_mt,
    unique_suffix,
    wait_for_export_status,
)

# Plain (non-replicated) MergeTree EXPORT PARTITION to an Iceberg (IcebergS3, catalog-less)
# destination. The Iceberg commit path is shared with the replicated implementation; these tests
# focus on the plain-MergeTree entry point and EXPORT PARTITION ALL.
#
# Restart-resume is deliberately NOT retested here: it is destination-agnostic (driven by the
# scheduler + on-disk descriptor) and is covered by
# test_export_mt_partition_to_object_storage::test_export_partition_resumes_after_restart. It cannot
# be reproduced the same way for Iceberg anyway, because an Iceberg destination reads its
# metadata.json from object storage at request time, so blocking object storage up-front would make
# the ALTER itself fail instead of leaving a resumable in-flight task.

SYSTEM_TABLE = "partition_exports"


@pytest.fixture(scope="module")
def cluster():
    try:
        cluster = ClickHouseCluster(__file__)
        cluster.add_instance(
            "node",
            main_configs=[
                "configs/allow_experimental_export_partition.xml",
                "configs/config.d/metadata_log.xml",
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


def setup_tables(node, mt_table, iceberg_table):
    make_mt(node, mt_table, "id Int64, year Int32", partition_by="year")
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020), (2, 2020), (3, 2020), (4, 2021)")
    make_iceberg_s3(node, iceberg_table, "id Int64, year Int32", partition_by="year")


def test_export_partition_to_iceberg(cluster):
    node = cluster.instances["node"]

    uid = unique_suffix()
    mt_table = f"mt_{uid}"
    iceberg_table = f"iceberg_{uid}"

    setup_tables(node, mt_table, iceberg_table)

    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {iceberg_table}",
        settings={"allow_insert_into_iceberg": 1},
    )
    wait_for_export_status(node, mt_table, iceberg_table, "2020", "COMPLETED", system_table=SYSTEM_TABLE)

    count = int(node.query(f"SELECT count() FROM {iceberg_table}").strip())
    assert count == 3, f"Expected 3 rows in Iceberg table after export, got {count}"

    result = node.query(f"SELECT id, year FROM {iceberg_table} ORDER BY id").strip()
    assert result == "1\t2020\n2\t2020\n3\t2020", f"Unexpected data in Iceberg table:\n{result}"


def test_export_partition_all_to_iceberg(cluster):
    node = cluster.instances["node"]

    uid = unique_suffix()
    mt_table = f"mt_all_{uid}"
    iceberg_table = f"iceberg_all_{uid}"

    setup_tables(node, mt_table, iceberg_table)

    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ALL TO TABLE {iceberg_table}",
        settings={"allow_insert_into_iceberg": 1},
    )

    wait_for_export_status(node, mt_table, iceberg_table, "2020", "COMPLETED", system_table=SYSTEM_TABLE)
    wait_for_export_status(node, mt_table, iceberg_table, "2021", "COMPLETED", system_table=SYSTEM_TABLE)

    count_2020 = int(node.query(f"SELECT count() FROM {iceberg_table} WHERE year = 2020").strip())
    count_2021 = int(node.query(f"SELECT count() FROM {iceberg_table} WHERE year = 2021").strip())
    assert count_2020 == 3, f"Expected 3 rows for year=2020, got {count_2020}"
    assert count_2021 == 1, f"Expected 1 row for year=2021, got {count_2021}"
