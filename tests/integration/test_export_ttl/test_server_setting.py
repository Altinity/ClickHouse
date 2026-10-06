from helpers.export_partition_helpers import unique_suffix

from .common import (
    COLUMNS,
    DUE,
    assert_exactly_once,
    create_iceberg,
    create_source,
    iceberg_ids,
    ttl_rows,
    wait_for_partitions_exported,
    wait_until,
)

CLUSTER_INSTANCES = ["replica1"]

# Without the server setting `allow_experimental_export_merge_tree_partition` a table has neither the
# export index nor the merge fence, so merges could mix exported parts with the others, whose rows
# would then never be exported. A table with an `EXPORT` TTL is therefore not loaded without it.

SETTING_CONFIG = "/etc/clickhouse-server/config.d/allow_experimental_export_partition.xml"


def set_server_setting(node, value):
    node.replace_in_config(
        SETTING_CONFIG,
        f"<allow_experimental_export_merge_tree_partition>{1 - value}</allow_experimental_export_merge_tree_partition>",
        f"<allow_experimental_export_merge_tree_partition>{value}</allow_experimental_export_merge_tree_partition>",
    )
    node.restart_clickhouse()


def test_table_is_not_attached_without_the_server_setting(cluster, source_engine):
    node = cluster.instances["replica1"]
    suffix = unique_suffix()
    mt_table, iceberg_table = f"setting_mt_{suffix}", f"setting_iceberg_{suffix}"
    create_iceberg(node, iceberg_table)
    create_source(node, mt_table, COLUMNS, "year", f"t + INTERVAL 1 DAY EXPORT TO TABLE {iceberg_table}", engine=source_engine)
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"])

    # Detached, so that the server starts without loading it.
    node.query(f"DETACH TABLE {mt_table} PERMANENTLY")
    set_server_setting(node, 0)
    try:
        error = node.query_and_get_error(f"ATTACH TABLE {mt_table}")
        assert "SUPPORT_IS_DISABLED" in error and "allow_experimental_export_merge_tree_partition" in error, error
    finally:
        set_server_setting(node, 1)

    node.query(f"ATTACH TABLE {mt_table}")
    wait_until(
        lambda: ttl_rows(node, mt_table).get("2020", {}).get("exported_parts") == 1, 60,
        "The exported part is not known as exported after the table is attached again",
    )
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1])
