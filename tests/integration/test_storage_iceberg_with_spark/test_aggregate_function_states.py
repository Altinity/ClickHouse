import glob
import json
import os
import re

import pytest

from helpers.iceberg_utils import (
    create_iceberg_table,
    default_download_directory,
    get_creation_expression,
    get_uuid_str,
)

from helpers.test_tools import TSV


SETTING = "allow_experimental_aggregate_function_states_in_iceberg"

STATE_SETTINGS = {SETTING: 1}
WRITE_SETTINGS = {"allow_insert_into_iceberg": 1}
PARQUET_STATE_SETTINGS = ["allow_experimental_aggregate_function_states_in_parquet = 1"]

SCHEMA = "(k Int32, u AggregateFunction(uniq, UInt64), s SimpleAggregateFunction(sum, UInt64))"

# Spark's Arrow reader does not accept the UINT_64 converted type written for UInt64.
SPARK_SCHEMA = "(k Int32, u AggregateFunction(uniq, UInt64), s SimpleAggregateFunction(sum, Int64))"

WAREHOUSE = "/var/lib/clickhouse/user_files/iceberg_data/default"


def insert_states(instance, table_name, sum_expression="number"):
    instance.query(
        f"""
        INSERT INTO {table_name} (k, u, s)
        SELECT toInt32(number % 2), uniqState(toUInt64(number % 23)), sumSimpleState({sum_expression})
        FROM numbers(200)
        GROUP BY number % 2
        """,
        settings={**WRITE_SETTINGS, **STATE_SETTINGS},
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
def test_aggregate_states_require_setting(started_cluster_iceberg_with_spark, storage_type):
    instance = started_cluster_iceberg_with_spark.instances["node1"]
    TABLE_NAME = "test_agg_states_setting_" + storage_type + "_" + get_uuid_str()

    creation_expression = get_creation_expression(
        storage_type, TABLE_NAME, started_cluster_iceberg_with_spark, SCHEMA, format_version=2
    )
    error = instance.query_and_get_error(creation_expression)
    assert SETTING in error, f"CREATE TABLE error does not name the setting: {error}"

    create_iceberg_table(
        storage_type,
        instance,
        TABLE_NAME,
        started_cluster_iceberg_with_spark,
        SCHEMA,
        format_version=2,
        settings=STATE_SETTINGS,
        additional_settings=PARQUET_STATE_SETTINGS,
    )

    error = instance.query_and_get_error(
        f"ALTER TABLE {TABLE_NAME} ADD COLUMN extra SimpleAggregateFunction(anyLast, Nullable(String))",
        settings=WRITE_SETTINGS,
    )
    assert SETTING in error, f"ADD COLUMN error does not name the setting: {error}"

    instance.query(
        f"ALTER TABLE {TABLE_NAME} ADD COLUMN extra SimpleAggregateFunction(anyLast, Nullable(String))",
        settings={**WRITE_SETTINGS, **STATE_SETTINGS},
    )

    insert_states(instance, TABLE_NAME)
    assert instance.query(f"SELECT count() FROM {TABLE_NAME}", settings=STATE_SETTINGS) == "2\n"

    table_function_expr = get_creation_expression(
        storage_type,
        TABLE_NAME,
        started_cluster_iceberg_with_spark,
        table_function=True,
    )
    error = instance.query_and_get_error(f"SELECT count() FROM {table_function_expr}")
    assert SETTING in error, f"Read error does not name the setting: {error}"

    assert (
        instance.query(f"SELECT count() FROM {table_function_expr}", settings=STATE_SETTINGS)
        == "2\n"
    )


@pytest.mark.parametrize("storage_type", ["s3"])
def test_aggregate_states_round_trip(started_cluster_iceberg_with_spark, storage_type):
    instance = started_cluster_iceberg_with_spark.instances["node1"]
    TABLE_NAME = "test_agg_states_round_trip_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(
        storage_type,
        instance,
        TABLE_NAME,
        started_cluster_iceberg_with_spark,
        SCHEMA,
        format_version=2,
        partition_by="k",
        settings=STATE_SETTINGS,
        additional_settings=PARQUET_STATE_SETTINGS,
    )
    insert_states(instance, TABLE_NAME)

    table_function_expr = get_creation_expression(
        storage_type,
        TABLE_NAME,
        started_cluster_iceberg_with_spark,
        table_function=True,
    )
    assert instance.query(
        f"DESCRIBE {table_function_expr} FORMAT TSV",
        settings={"print_pretty_type_names": 0, **STATE_SETTINGS},
    ) == TSV(
        [
            ["k", "Int32"],
            ["u", "AggregateFunction(uniq, UInt64)"],
            ["s", "SimpleAggregateFunction(sum, UInt64)"],
        ]
    )

    assert instance.query(
        f"SELECT k, uniqMerge(u), sum(s) FROM {TABLE_NAME} GROUP BY k ORDER BY k",
        settings=STATE_SETTINGS,
    ) == instance.query(
        "SELECT toInt32(number % 2) AS k, uniq(toUInt64(number % 23)), sum(number)"
        " FROM numbers(200) GROUP BY k ORDER BY k"
    )

    default_download_directory(
        started_cluster_iceberg_with_spark,
        storage_type,
        f"{WAREHOUSE}/{TABLE_NAME}/",
        f"{WAREHOUSE}/{TABLE_NAME}/",
    )

    fields = fields_by_name(latest_metadata(TABLE_NAME))
    assert fields["k"]["type"] == "int"
    assert "clickhouse.type" not in fields["k"]
    assert fields["u"]["type"] == "binary"
    assert fields["u"]["clickhouse.type"] == "AggregateFunction(uniq, UInt64)"
    assert fields["s"]["type"] == "long"
    assert fields["s"]["clickhouse.type"] == "SimpleAggregateFunction(sum, UInt64)"


@pytest.mark.parametrize("storage_type", ["s3"])
def test_aggregate_states_read_by_spark(started_cluster_iceberg_with_spark, storage_type):
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
        settings=STATE_SETTINGS,
        additional_settings=PARQUET_STATE_SETTINGS,
    )
    insert_states(instance, TABLE_NAME, "toInt64(number)")

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
    assert len(spark_rows) == 2

    expected = instance.query(
        f"SELECT k, hex(u), s FROM {TABLE_NAME} ORDER BY k FORMAT TSV", settings=STATE_SETTINGS
    ).strip()
    assert expected != ""
    for line in expected.split("\n"):
        k, state_hex, sum_value = line.split("\t")
        assert spark_rows[int(k)] == (state_hex, int(sum_value))
