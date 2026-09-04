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
# Reading a column whose `clickhouse.type` names an `AggregateFunction` is gated by the same setting,
# because the table's metadata - not the query - picks the deserializer handed the state bytes. The
# setting is read from the query that parses the Iceberg schema, so every query that touches such a
# table carries it here: a SELECT, but an INSERT too. A table function parses the schema afresh each
# time, so for it the setting acts per query; a table engine parses it once and keeps the result for
# the lifetime of the storage object, so passing it everywhere keeps these tests independent of which
# query happens to parse the schema first.
WRITE_SETTINGS = {"allow_insert_into_iceberg": 1}
# Iceberg data files are Parquet, and writing an `AggregateFunction` state to Parquet is opt-in of
# its own; there is no internal override for the Iceberg path. An object storage table engine freezes
# its format settings at CREATE TABLE - global server settings plus the SETTINGS clause of that
# query, session settings deliberately ignored (registerStorageObjectStorage.cpp) - so the Parquet
# setting belongs in the CREATE, not on the INSERT.
PARQUET_STATE_SETTINGS = ["allow_experimental_aggregate_function_states_in_parquet = 1"]

# `k` is Int32 and not UInt32 because Iceberg maps both to `int`.
SCHEMA = "(k Int32, u AggregateFunction(uniq, UInt64), s SimpleAggregateFunction(sum, UInt64))"

# The same, with a signed storage type for the SimpleAggregateFunction: Iceberg's vectorized Arrow
# reader rejects the UINT_64 converted type ClickHouse writes for UInt64, which is a general
# interoperability gap for unsigned types, unrelated to aggregate states.
SPARK_SCHEMA = "(k Int32, u AggregateFunction(uniq, UInt64), s SimpleAggregateFunction(sum, Int64))"

# Where the iceberg warehouse is mounted, both inside the ClickHouse container and - after
# default_download_directory() - on the host running the test and the Spark session.
WAREHOUSE = "/var/lib/clickhouse/user_files/iceberg_data/default"


def insert_states(instance, table_name, sum_expression="number"):
    """Two groups of pre-aggregated states, the same shape as an AggregatingMergeTree part."""
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
    """The newest metadata JSON of a table already fetched by default_download_directory()."""
    files = glob.glob(os.path.join(WAREHOUSE, table_name, "metadata", "v*.metadata.json"))
    assert files, f"No metadata JSON downloaded for {table_name}"
    # Sort by version rather than by name, so that v10 does not come before v2.
    newest = max(files, key=lambda path: int(re.fullmatch(r"v(\d+)\.metadata\.json", os.path.basename(path)).group(1)))
    with open(newest) as f:
        return json.load(f)


def fields_by_name(metadata):
    schema_id = metadata["current-schema-id"]
    for schema in metadata["schemas"]:
        if schema["schema-id"] == schema_id:
            return {field["name"]: field for field in schema["fields"]}
    raise AssertionError(f"Schema {schema_id} not found in {metadata['schemas']}")


# Every code path these tests exercise sits above the object storage layer, so one storage type is
# enough; the per-storage write paths are covered by the other tests in this directory.
@pytest.mark.parametrize("storage_type", ["s3"])
def test_aggregate_states_require_setting(started_cluster_iceberg_with_spark, storage_type):
    """Every DDL path that can introduce an aggregate state into an Iceberg table is gated."""
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

    # A plain table plus ADD COLUMN would otherwise bypass the CREATE TABLE check. Iceberg only
    # allows adding optional columns, hence the Nullable state type.
    error = instance.query_and_get_error(
        f"ALTER TABLE {TABLE_NAME} ADD COLUMN extra SimpleAggregateFunction(anyLast, Nullable(String))",
        settings=WRITE_SETTINGS,
    )
    assert SETTING in error, f"ADD COLUMN error does not name the setting: {error}"

    error = instance.query_and_get_error(
        f"ALTER TABLE {TABLE_NAME} MODIFY COLUMN k SimpleAggregateFunction(anyLast, Nullable(String))",
        settings=WRITE_SETTINGS,
    )
    assert SETTING in error, f"MODIFY COLUMN error does not name the setting: {error}"

    instance.query(
        f"ALTER TABLE {TABLE_NAME} ADD COLUMN extra SimpleAggregateFunction(anyLast, Nullable(String))",
        settings={**WRITE_SETTINGS, **STATE_SETTINGS},
    )

    # Reading the recorded `AggregateFunction` type needs the setting on the query, the same way the
    # INSERT inside insert_states() does; the Parquet gate the INSERT also passes through is opened by
    # the CREATE above.
    insert_states(instance, TABLE_NAME)
    assert instance.query(f"SELECT count() FROM {TABLE_NAME}", settings=STATE_SETTINGS) == "2\n"

    # Without it the recorded type is refused rather than read as `String`. The refusal is asserted
    # through the table function, which parses the schema afresh on every query; the table engine
    # above has already parsed it and keeps the parsed schema for the lifetime of its storage object.
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
    """States written by ClickHouse merge back to exactly what the source data produces."""
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

    # The table function reads the schema out of the Iceberg metadata rather than out of the
    # CREATE TABLE definition, so this is what proves the annotation round-tripped.
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

    assert instance.query(
        f"SELECT uniqMerge(u), sum(s) FROM {TABLE_NAME}", settings=STATE_SETTINGS
    ) == instance.query(
        "SELECT uniq(toUInt64(number % 23)), sum(number) FROM numbers(200)"
    )

    # Min/max bounds over serialized states are meaningless, so none are recorded for `u`: a
    # filter over the state column must still see every row rather than prune some away.
    assert instance.query(
        f"SELECT count() FROM {TABLE_NAME} WHERE finalizeAggregation(u) > 0",
        settings=STATE_SETTINGS,
    ) == "2\n"

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
    """Spark ignores the `clickhouse.type` key and sees plain binary / bigint columns."""
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

    # CREATE TABLE wrote v0, the single INSERT wrote v1.
    with open(f"{WAREHOUSE}/{TABLE_NAME}/metadata/version-hint.text", "wb") as f:
        f.write(b"1")

    df = spark.read.format("iceberg").load(f"{WAREHOUSE}/{TABLE_NAME}")
    assert dict(df.dtypes) == {"k": "int", "u": "binary", "s": "bigint"}

    spark_rows = {row["k"]: (bytes(row["u"]).hex().upper(), row["s"]) for row in df.collect()}
    assert len(spark_rows) == 2

    # Spark sees byte for byte the states ClickHouse wrote, with no reinterpretation.
    expected = instance.query(
        f"SELECT k, hex(u), s FROM {TABLE_NAME} ORDER BY k FORMAT TSV", settings=STATE_SETTINGS
    ).strip()
    assert expected != ""
    for line in expected.split("\n"):
        k, state_hex, sum_value = line.split("\t")
        assert spark_rows[int(k)] == (state_hex, int(sum_value))
