import glob
import json
import os
import re

import pytest

from helpers.iceberg_utils import (
    create_iceberg_table,
    default_download_directory,
    default_upload_directory,
    get_uuid_str,
)


STATES = {"allow_aggregate_function_states_in_open_formats": 1}
WRITE_SETTINGS = {**STATES, "allow_insert_into_iceberg": 1}

# Spark's Arrow reader does not accept the UINT_64 converted type written for UInt64.
SPARK_SCHEMA = "(k Int32, u AggregateFunction(uniq, UInt64), s SimpleAggregateFunction(sum, Int64))"

WAREHOUSE = "/var/lib/clickhouse/user_files/iceberg_data/default"


def insert_states(instance, table_name):
    instance.query(
        f"""
        INSERT INTO {table_name} (k, u, s)
        SELECT toInt32(number % 2), uniqState(toUInt64(number % 23)), sumSimpleState(toInt64(number))
        FROM numbers(200)
        GROUP BY number % 2
        """,
        settings=WRITE_SETTINGS,
    )


def latest_metadata(table_name):
    files = glob.glob(os.path.join(WAREHOUSE, table_name, "metadata", "v*.metadata.json"))
    assert files, f"No metadata JSON downloaded for {table_name}"
    newest = max(files, key=lambda path: int(re.fullmatch(r"v(\d+)\.metadata\.json", os.path.basename(path)).group(1)))
    with open(newest) as f:
        return json.load(f)


def fields_by_name(metadata):
    schema_id = metadata["current-schema-id"]
    for schema in metadata["schemas"]:
        if schema["schema-id"] == schema_id:
            return {field["name"]: field for field in schema["fields"]}
    raise AssertionError(f"Schema {schema_id} not found in {metadata['schemas']}")


@pytest.mark.parametrize("storage_type", ["s3"])
def test_aggregate_states_with_spark(started_cluster_iceberg_with_spark, storage_type):
    instance = started_cluster_iceberg_with_spark.instances["node1"]
    spark = started_cluster_iceberg_with_spark.spark_session
    TABLE_NAME = "test_agg_states_spark_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(
        storage_type,
        instance,
        TABLE_NAME,
        started_cluster_iceberg_with_spark,
        SPARK_SCHEMA,
        format_version=2,
        additional_settings=["allow_aggregate_function_states_in_open_formats = 1"],
    )
    insert_states(instance, TABLE_NAME)

    default_download_directory(
        started_cluster_iceberg_with_spark,
        storage_type,
        f"{WAREHOUSE}/{TABLE_NAME}/",
        f"{WAREHOUSE}/{TABLE_NAME}/",
    )

    with open(f"{WAREHOUSE}/{TABLE_NAME}/metadata/version-hint.text", "wb") as f:
        f.write(b"1")

    df = spark.read.format("iceberg").load(f"{WAREHOUSE}/{TABLE_NAME}")
    assert dict(df.dtypes) == {"k": "int", "u": "binary", "s": "bigint"}
    spark_rows = {row["k"]: (bytes(row["u"]).hex().upper(), row["s"]) for row in df.collect()}
    expected = instance.query(
        f"SELECT k, hex(u), s FROM {TABLE_NAME} ORDER BY k FORMAT TSV", settings=STATES
    ).strip()
    assert spark_rows == {
        int(k): (state_hex, int(sum_value))
        for k, state_hex, sum_value in (line.split("\t") for line in expected.split("\n"))
    }

    appended = instance.query(
        "SELECT toInt32(number % 2) AS k, hex(uniqState(toUInt64(number % 23))), sum(toInt64(number))"
        " FROM numbers(200, 100) GROUP BY k ORDER BY k FORMAT TSV"
    ).strip()
    values = ", ".join(
        f"({k}, X'{state_hex}', {sum_value})"
        for k, state_hex, sum_value in (line.split("\t") for line in appended.split("\n"))
    )
    spark.sql(f"INSERT INTO {TABLE_NAME} VALUES {values}")

    # The appended file must carry bounds for the column holding the states.
    u_field_id = fields_by_name(latest_metadata(TABLE_NAME))["u"]["id"]
    spark_files = spark.sql(
        f"SELECT lower_bounds, upper_bounds FROM spark_catalog.default.{TABLE_NAME}.files WHERE lower_bounds IS NOT NULL"
    ).collect()
    assert any(
        u_field_id in (row["lower_bounds"] or {}) and u_field_id in (row["upper_bounds"] or {}) for row in spark_files
    ), f"No data file has bounds for field {u_field_id}: {spark_files}"

    default_upload_directory(
        started_cluster_iceberg_with_spark,
        storage_type,
        f"{WAREHOUSE}/{TABLE_NAME}/",
        f"{WAREHOUSE}/{TABLE_NAME}/",
    )

    assert instance.query(
        f"SELECT k, uniqMerge(u), sum(s) FROM {TABLE_NAME} GROUP BY k ORDER BY k", settings=STATES
    ) == instance.query(
        "SELECT toInt32(number % 2) AS k, uniq(toUInt64(number % 23)), sum(number)"
        " FROM numbers(300) GROUP BY k ORDER BY k"
    )

    # A filter on the states makes pruning consider the bounds Spark wrote for them, which are not ClickHouse values.
    assert instance.query(
        f"SELECT k, uniqMerge(u) FROM {TABLE_NAME} WHERE finalizeAggregation(u) > 0 GROUP BY k ORDER BY k", settings=STATES
    ) == instance.query(
        "SELECT toInt32(number % 2) AS k, uniq(toUInt64(number % 23)) FROM numbers(300) GROUP BY k ORDER BY k"
    )


@pytest.mark.parametrize("storage_type", ["s3"])
def test_aggregate_states_after_spark_schema_evolution(started_cluster_iceberg_with_spark, storage_type):
    """Spark evolves annotated fields as the Iceberg spec allows: the table stays readable, and the evolved fields read as their Iceberg types."""
    instance = started_cluster_iceberg_with_spark.instances["node1"]
    spark = started_cluster_iceberg_with_spark.spark_session
    TABLE_NAME = "test_agg_states_spark_evolution_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(
        storage_type,
        instance,
        TABLE_NAME,
        started_cluster_iceberg_with_spark,
        "(k Int32, u AggregateFunction(uniq, UInt64), m SimpleAggregateFunction(max, Int32))",
        format_version=2,
        additional_settings=["allow_aggregate_function_states_in_open_formats = 1"],
    )
    instance.query(
        f"""
        INSERT INTO {TABLE_NAME} (k, u, m)
        SELECT toInt32(number % 2), uniqState(toUInt64(number % 23)), maxSimpleState(toInt32(number))
        FROM numbers(200)
        GROUP BY number % 2
        """,
        settings=WRITE_SETTINGS,
    )
    expected = instance.query(f"SELECT k, hex(u), m FROM {TABLE_NAME} ORDER BY k", settings=STATES)

    default_download_directory(
        started_cluster_iceberg_with_spark,
        storage_type,
        f"{WAREHOUSE}/{TABLE_NAME}/",
        f"{WAREHOUSE}/{TABLE_NAME}/",
    )

    with open(f"{WAREHOUSE}/{TABLE_NAME}/metadata/version-hint.text", "wb") as f:
        f.write(b"1")

    spark.sql(f"ALTER TABLE {TABLE_NAME} ALTER COLUMN u DROP NOT NULL")
    spark.sql(f"ALTER TABLE {TABLE_NAME} ALTER COLUMN m TYPE BIGINT")

    default_upload_directory(
        started_cluster_iceberg_with_spark,
        storage_type,
        f"{WAREHOUSE}/{TABLE_NAME}/",
        f"{WAREHOUSE}/{TABLE_NAME}/",
    )

    assert (
        instance.query(f"SELECT toTypeName(u), toTypeName(m) FROM {TABLE_NAME} LIMIT 1", settings=STATES)
        == "Nullable(String)\tInt64\n"
    )
    assert instance.query(f"SELECT k, hex(u), m FROM {TABLE_NAME} ORDER BY k", settings=STATES) == expected
