import pytest

from helpers.iceberg_utils import create_iceberg_table, get_uuid_str

# The footer of the data file of this table does not fit into the tail that the Parquet reader reads by default
# (128 KiB or more for object storage, 64 KiB for a local file). The Iceberg manifest has the number of columns and
# the column bounds, which is enough to estimate the footer size and to read it in one request.
NUM_COLUMNS = 3000


def get_footer_second_reads(node, query, settings):
    query_id = "footer_hint_" + get_uuid_str()
    node.query("SYSTEM DROP PARQUET METADATA CACHE")
    result = node.query(query, query_id=query_id, settings=settings)
    node.query("SYSTEM FLUSH LOGS query_log")
    second_reads = int(
        node.query(
            f"""
            SELECT ProfileEvents['ParquetFooterSecondRead'] FROM system.query_log
            WHERE type = 'QueryFinish' AND query_id = '{query_id}'
            """
        ).strip()
    )
    return result, second_reads


@pytest.mark.parametrize("storage_type", ["s3", "local"])
def test_parquet_footer_size_hint(started_cluster_iceberg_no_spark, storage_type):
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_parquet_footer_size_hint_" + storage_type + "_" + get_uuid_str()

    schema = "(" + ", ".join(f"c{i} Int32" for i in range(NUM_COLUMNS)) + ")"
    create_iceberg_table(storage_type, instance, table_name, started_cluster_iceberg_no_spark, schema, format_version=2)

    instance.query(
        f"INSERT INTO {table_name} SELECT "
        + ", ".join(f"toInt32(number + {i}) AS c{i}" for i in range(NUM_COLUMNS))
        + " FROM numbers(100)"
    )

    query = f"SELECT sum(c0), sum(c{NUM_COLUMNS - 1}) FROM {table_name}"
    expected = f"{sum(range(100))}\t{sum(range(NUM_COLUMNS - 1, NUM_COLUMNS + 99))}\n"

    # The default: the footer size is estimated from the manifest, so one read is enough.
    result, second_reads = get_footer_second_reads(instance, query, {})
    assert result == expected
    assert second_reads == 0

    # Without the Parquet metadata cache the reader is not given the object's metadata, so it has no hint and reads
    # the default tail: 128 KiB of an object in object storage, 64 KiB of a local file. The footer is larger.
    result, second_reads = get_footer_second_reads(instance, query, {"use_parquet_metadata_cache": 0})
    assert result == expected
    assert second_reads == 1
