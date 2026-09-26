#!/usr/bin/env python3

import json
import uuid
from datetime import datetime

import pyarrow as pa
from pyiceberg.catalog import load_catalog
from pyiceberg.partitioning import PartitionField, PartitionSpec
from pyiceberg.schema import Schema
from pyiceberg.transforms import DayTransform, IdentityTransform
from pyiceberg.types import (
    DoubleType,
    LongType,
    NestedField,
    StringType,
    TimestampType,
)

from helpers.config_cluster import minio_access_key, minio_secret_key
from helpers.iceberg_utils import create_iceberg_table, get_uuid_str
from helpers.s3_tools import list_s3_objects

CATALOG_NAME = "demo"

WRITE_SETTINGS = {
    "allow_experimental_insert_into_iceberg": 1,
    "write_full_path_in_iceberg_metadata": 1
}

ROWS = [
    {"ts": datetime(2024, 1, 15, 8, 0, 0), "country": "US", "id": 1, "amount": 10.5},
    {"ts": datetime(2024, 1, 15, 12, 30, 0), "country": "DE", "id": 2, "amount": 20.0},
    {"ts": datetime(2024, 1, 15, 23, 0, 0), "country": "US", "id": 3, "amount": 30.25},
    {"ts": datetime(2024, 1, 16, 9, 0, 0), "country": "US", "id": 4, "amount": 60.0},
    {"ts": datetime(2024, 1, 16, 18, 45, 0), "country": "DE", "id": 5, "amount": 70.75},
    {"ts": datetime(2024, 1, 17, 11, 11, 11), "country": "JP", "id": 6, "amount": 80.0},
]


def load_catalog_impl(started_cluster):
    return load_catalog(
        CATALOG_NAME,
        **{
            "uri": f"http://localhost:{started_cluster.iceberg_rest_catalog_port}",
            "type": "rest",
            "s3.endpoint": f"http://{started_cluster.minio_ip}:{started_cluster.minio_port}",
            "s3.access-key-id": minio_access_key,
            "s3.secret-access-key": minio_secret_key,
        },
    )


def create_day_partitioned_table(catalog, namespace, table_name):
    """A table partitioned by `day(ts)` filled by a single append that spans three days"""
    catalog.create_namespace(namespace)

    schema = Schema(
        NestedField(field_id=1, name="ts", field_type=TimestampType(), required=False),
        NestedField(field_id=2, name="country", field_type=StringType(), required=False),
        NestedField(field_id=3, name="id", field_type=LongType(), required=False),
        NestedField(field_id=4, name="amount", field_type=DoubleType(), required=False),
    )
    partition_spec = PartitionSpec(
        PartitionField(
            source_id=1, field_id=1000, transform=DayTransform(), name="ts_day"
        )
    )

    table = catalog.create_table(
        identifier=f"{namespace}.{table_name}",
        schema=schema,
        location=f"s3://warehouse-rest/{namespace}.{table_name}",
        partition_spec=partition_spec,
    )
    table.append(pa.Table.from_pylist(ROWS, schema=schema.as_arrow()))

    manifests = table.current_snapshot().manifests(table.io)
    assert len(manifests) == 1, (
        f"this test needs one manifest holding several partitions, pyiceberg wrote {len(manifests)}"
    )
    return table


def create_iceberg_database(instance, namespace):
    instance.query(f"DROP DATABASE IF EXISTS {namespace}")
    instance.query(
        f"""
        CREATE DATABASE {namespace} ENGINE = DataLakeCatalog('http://rest:8181/v1', '{minio_access_key}', '{minio_secret_key}')
        SETTINGS
            catalog_type='rest',
            warehouse='demo',
            storage_endpoint='http://minio1:9001/warehouse-rest';
        """,
        settings={"allow_database_iceberg": 1},
    )


def current_snapshot_history(instance, namespace, table_name):
    """`operation` and `summary` of the table's current snapshot"""
    operation, summary = (
        instance.query(
            f"SELECT operation, toJSONString(summary) FROM system.iceberg_history "
            f"WHERE database = '{namespace}' AND table = '{namespace}.{table_name}' "
            f"ORDER BY made_current_at DESC, snapshot_id DESC LIMIT 1 FORMAT TSV"
        )
        .strip()
        .split("\t", 1)
    )
    return operation, json.loads(summary)


def create_partitioned_table(instance, namespace, table_name, columns, partition_by):
    """Create an Iceberg table through ClickHouse and return its qualified name."""
    ch_table = f"{namespace}.`{namespace}.{table_name}`"
    instance.query(
        f"CREATE TABLE {ch_table} ({columns}) "
        f"ENGINE = IcebergS3('http://minio1:9001/warehouse-rest/{namespace}/{table_name}', "
        f"'{minio_access_key}', '{minio_secret_key}') "
        f"PARTITION BY {partition_by}",
        settings=WRITE_SETTINGS,
    )
    return ch_table


def setup(instance, table_name, columns, partition_by):
    namespace = f"clickhouse_drop_partition_{uuid.uuid4().hex}"
    create_iceberg_database(instance, namespace)
    ch_table = create_partitioned_table(
        instance, namespace, table_name, columns, partition_by
    )
    return namespace, ch_table


def test_drop_partition_separate_manifest(started_cluster_iceberg_no_spark):
    instance = started_cluster_iceberg_no_spark.instances["node1"]

    namespace = f"clickhouse_drop_partition_{uuid.uuid4().hex}"
    table_name = "separate_manifest"

    create_iceberg_database(instance, namespace)

    ch_table = f"{namespace}.`{namespace}.{table_name}`"

    instance.query(
        f"CREATE TABLE {ch_table} ("
        f"ts DateTime64(6),"
        f"country String,"
        f"id Int64,"
        f"amount Float64) "
        f"ENGINE = IcebergS3('http://minio1:9001/warehouse-rest/{namespace}/{table_name}', '{minio_access_key}', '{minio_secret_key}') "
        f"PARTITION BY toRelativeDayNum(ts)",
        settings = WRITE_SETTINGS
    )

    instance.query(
        f"INSERT INTO {ch_table} VALUES "
        f"(toDateTime64('2024-01-15 08:00:00', 6), 'US', 1, 10.5), "
        f"(toDateTime64('2024-01-15 12:30:00', 6), 'DE', 2, 20.0), "
        f"(toDateTime64('2024-01-15 23:59:59', 6), 'US', 3, 30.25)",
        settings = WRITE_SETTINGS
    )

    instance.query(
        f"INSERT INTO {ch_table} VALUES "
        f"(toDateTime64('2024-01-15 01:00:00', 6), 'FR', 4, 40.0), "
        f"(toDateTime64('2024-01-15 06:15:00', 6), 'US', 5, 50.5)",
        settings = WRITE_SETTINGS
    )

    instance.query(
        f"INSERT INTO {ch_table} VALUES "
        f"(toDateTime64('2024-01-16 09:00:00', 6), 'US', 6, 60.0), "
        f"(toDateTime64('2024-01-16 18:45:00', 6), 'DE', 7, 70.75)",
        settings = WRITE_SETTINGS
    )

    instance.query(f"INSERT INTO {ch_table} VALUES (toDateTime64('2024-01-17 11:11:11', 6), 'JP', 8, 80.0)", settings = WRITE_SETTINGS)

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "8"

    assert instance.query(
        f"SELECT toDate(ts) AS d, count() FROM {ch_table} GROUP BY d ORDER BY d FORMAT TSV"
    ) == "2024-01-15\t5\n2024-01-16\t2\n2024-01-17\t1\n"

    instance.query(f"ALTER TABLE {ch_table} DROP PARTITION '2024-01-15'", settings = WRITE_SETTINGS)

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "3"

    assert instance.query(
        f"SELECT toDate(ts) AS d, count() FROM {ch_table} GROUP BY d ORDER BY d FORMAT TSV"
    ) == "2024-01-16\t2\n2024-01-17\t1\n"

    # Drop the rest of the partitions
    instance.query(f"ALTER TABLE {ch_table} DROP PARTITION '2024-01-16'", settings = WRITE_SETTINGS)
    instance.query(f"ALTER TABLE {ch_table} DROP PARTITION '2024-01-17'", settings = WRITE_SETTINGS)

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "0"



def test_drop_partition_rewrites_mixed_manifest(started_cluster_iceberg_no_spark):
    """DROP PARTITION on a manifest that also holds files of other partitions must rewrite
    that manifest without the dropped files, rather than unreferencing it completely."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    catalog = load_catalog_impl(started_cluster_iceberg_no_spark)

    namespace = f"clickhouse_drop_partition_{uuid.uuid4().hex}"
    table_name = "mixed_manifest"
    create_day_partitioned_table(catalog, namespace, table_name)

    create_iceberg_database(instance, namespace)
    ch_table = f"{namespace}.`{namespace}.{table_name}`"

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "6"

    instance.query(
        f"ALTER TABLE {ch_table} DROP PARTITION '2024-01-15'", settings=WRITE_SETTINGS
    )

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "3"
    assert instance.query(
        f"SELECT id, country FROM {ch_table} ORDER BY id FORMAT TSV"
    ) == "4\tUS\n5\tDE\n6\tJP\n"
    assert instance.query(
        f"SELECT toDate(ts) AS d, count() FROM {ch_table} GROUP BY d ORDER BY d FORMAT TSV"
    ) == "2024-01-16\t2\n2024-01-17\t1\n"

    # Only the file of the dropped partition is gone while the two that shared its manifest
    # stay
    assert instance.query(
        f"SELECT partition, record_count, sequence_number FROM system.iceberg_files "
        f"WHERE database = '{namespace}' AND table = '{namespace}.{table_name}' "
        f"AND content = 'DATA' ORDER BY partition FORMAT TSV"
    ) == "{19738}\t2\t1\n{19739}\t1\t1\n"

    operation, summary = current_snapshot_history(instance, namespace, table_name)
    assert operation == "DELETE"
    assert summary["deleted-data-files"] == "1"
    assert summary["deleted-records"] == "3"
    assert summary["total-data-files"] == "2"
    assert summary["total-records"] == "3"
    assert summary["changed-partition-count"] == "1"

    instance.query(f"DROP DATABASE {namespace}")


def test_drop_partition_without_matching_files_is_noop(started_cluster_iceberg_no_spark):
    """A partition value that no file belongs to must leave the table untouched, with no
    new snapshot committed."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    catalog = load_catalog_impl(started_cluster_iceberg_no_spark)

    namespace = f"clickhouse_drop_partition_noop_{uuid.uuid4().hex}"
    table_name = "mixed_manifest"
    create_day_partitioned_table(catalog, namespace, table_name)

    create_iceberg_database(instance, namespace)
    ch_table = f"{namespace}.`{namespace}.{table_name}`"

    snapshots_before = instance.query(
        f"SELECT count() FROM system.iceberg_history "
        f"WHERE database = '{namespace}' AND table = '{namespace}.{table_name}'"
    ).strip()

    instance.query(
        f"ALTER TABLE {ch_table} DROP PARTITION '2024-02-20'", settings=WRITE_SETTINGS
    )

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "6"
    assert (
        instance.query(
            f"SELECT count() FROM system.iceberg_history "
            f"WHERE database = '{namespace}' AND table = '{namespace}.{table_name}'"
        ).strip()
        == snapshots_before
    )

    instance.query(f"DROP DATABASE {namespace}")


def test_drop_partition_tuple_of_identities(started_cluster_iceberg_no_spark):
    """A partition spec of several identity fields: the values are given as a tuple, one per
    partition field."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    namespace, ch_table = setup(
        instance,
        "tuple_of_identities",
        "country String, region String, id Int64",
        "(country, region)",
    )

    instance.query(
        f"INSERT INTO {ch_table} VALUES "
        f"('US', 'west', 1), ('US', 'east', 2), ('DE', 'west', 3)",
        settings=WRITE_SETTINGS,
    )

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "3"

    error = instance.query_and_get_error(
        f"ALTER TABLE {ch_table} DROP PARTITION 'US'", settings=WRITE_SETTINGS
    )
    assert "Expected a tuple for a partition key with 2 fields" in error

    error = instance.query_and_get_error(
        f"ALTER TABLE {ch_table} DROP PARTITION ('US', 'west', 'extra')",
        settings=WRITE_SETTINGS,
    )
    assert "Wrong number of fields in the partition expression: 3, must be: 2" in error

    instance.query(
        f"ALTER TABLE {ch_table} DROP PARTITION ('US', 'west')", settings=WRITE_SETTINGS
    )

    assert (
        instance.query(
            f"SELECT country, region, id FROM {ch_table} ORDER BY id FORMAT TSV"
        )
        == "US\teast\t2\nDE\twest\t3\n"
    )

    instance.query(f"DROP DATABASE {namespace}")


def test_drop_partition_month_transform(started_cluster_iceberg_no_spark):
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    namespace, ch_table = setup(
        instance,
        "month_transform",
        "ts DateTime64(6), id Int64",
        "toMonthNumSinceEpoch(ts)",
    )

    instance.query(
        f"INSERT INTO {ch_table} VALUES "
        f"(toDateTime64('2024-01-02 00:00:00', 6), 1), "
        f"(toDateTime64('2024-01-30 23:59:59', 6), 2), "
        f"(toDateTime64('2024-02-10 12:00:00', 6), 3), "
        f"(toDateTime64('2024-02-28 23:59:59', 6), 4)",
        settings=WRITE_SETTINGS,
    )

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "4"

    instance.query(
        f"ALTER TABLE {ch_table} DROP PARTITION '2024-01-15 08:30:00'",
        settings=WRITE_SETTINGS,
    )

    assert (
        instance.query(f"SELECT id FROM {ch_table} ORDER BY id FORMAT TSV") == "3\n4\n"
    )

    instance.query(
        f"ALTER TABLE {ch_table} DROP PARTITION '2024-02-01'", settings=WRITE_SETTINGS
    )

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "0"

    instance.query(f"DROP DATABASE {namespace}")


def test_drop_partition_day_transform(started_cluster_iceberg_no_spark):
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    namespace, ch_table = setup(
        instance,
        "day_transform",
        "ts DateTime64(6), id Int64",
        "toRelativeDayNum(ts)",
    )

    instance.query(
        f"INSERT INTO {ch_table} VALUES "
        f"(toDateTime64('2024-01-15 08:00:00', 6), 1), "
        f"(toDateTime64('2024-01-15 23:59:59', 6), 2), "
        f"(toDateTime64('2024-01-16 09:00:00', 6), 3), "
        f"(toDateTime64('2024-01-17 09:00:00', 6), 4)",
        settings=WRITE_SETTINGS,
    )

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "4"

    instance.query(
        f"ALTER TABLE {ch_table} DROP PARTITION '2024-01-15 12:00:00'",
        settings=WRITE_SETTINGS,
    )
    assert instance.query(f"SELECT id FROM {ch_table} ORDER BY id FORMAT TSV") == "3\n4\n"

    instance.query(
        f"ALTER TABLE {ch_table} DROP PARTITION ('2024-01-16')", settings=WRITE_SETTINGS
    )
    assert instance.query(f"SELECT id FROM {ch_table} FORMAT TSV") == "4\n"

    instance.query(f"DROP DATABASE {namespace}")


def test_drop_partition_int_identity(started_cluster_iceberg_no_spark):
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    namespace, ch_table = setup(
        instance, "int_identity", "key Int64, val String", "key"
    )

    instance.query(
        f"INSERT INTO {ch_table} VALUES (10, 'a'), (10, 'b'), (20, 'c'), (30, 'd')",
        settings=WRITE_SETTINGS,
    )

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "4"

    instance.query(
        f"ALTER TABLE {ch_table} DROP PARTITION 10", settings=WRITE_SETTINGS
    )
    assert (
        instance.query(f"SELECT key, val FROM {ch_table} ORDER BY key FORMAT TSV")
        == "20\tc\n30\td\n"
    )

    instance.query(
        f"ALTER TABLE {ch_table} DROP PARTITION '20'", settings=WRITE_SETTINGS
    )
    assert instance.query(f"SELECT key FROM {ch_table} FORMAT TSV") == "30\n"

    instance.query(f"DROP DATABASE {namespace}")


def test_drop_partition_date_identity(started_cluster_iceberg_no_spark):
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    namespace, ch_table = setup(
        instance, "date_identity", "event_date Date, id Int64", "event_date"
    )

    instance.query(
        f"INSERT INTO {ch_table} VALUES "
        f"('2024-01-15', 1), ('2024-01-15', 2), ('2024-01-16', 3)",
        settings=WRITE_SETTINGS,
    )

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "3"

    instance.query(
        f"ALTER TABLE {ch_table} DROP PARTITION '2024-01-15'", settings=WRITE_SETTINGS
    )

    assert (
        instance.query(
            f"SELECT event_date, id FROM {ch_table} ORDER BY id FORMAT TSV"
        )
        == "2024-01-16\t3\n"
    )

    instance.query(f"DROP DATABASE {namespace}")


def test_drop_partition_unpartitioned_table(started_cluster_iceberg_no_spark):
    instance = started_cluster_iceberg_no_spark.instances["node1"]

    namespace = f"clickhouse_drop_partition_{uuid.uuid4().hex}"
    table_name = "unpartitioned"
    ch_table = f"{namespace}.`{namespace}.{table_name}`"

    create_iceberg_database(instance, namespace)

    instance.query(
        f"CREATE TABLE {ch_table} (id Int64, country String) "
        f"ENGINE = IcebergS3('http://minio1:9001/warehouse-rest/{namespace}/{table_name}', "
        f"'{minio_access_key}', '{minio_secret_key}')",
        settings=WRITE_SETTINGS,
    )

    instance.query(
        f"INSERT INTO {ch_table} VALUES "
        f"(1, 'CA'), (2, 'PT'), (3, 'MX')",
        settings=WRITE_SETTINGS,
    )

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "3"

    error = instance.query_and_get_error(
        f"ALTER TABLE {ch_table} DROP PARTITION 'CA'", settings=WRITE_SETTINGS
    )
    assert "table is not partitioned" in error

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "3"

    instance.query(f"DROP DATABASE {namespace}")


def test_drop_partition_without_catalog(started_cluster_iceberg_no_spark):
    instance = started_cluster_iceberg_no_spark.instances["node1"]

    table_name = "test_drop_partition_no_catalog_" + get_uuid_str()

    create_iceberg_table(
        "s3",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(ts DateTime64(6), id Int64)",
        format_version=2,
        partition_by="toRelativeDayNum(ts)",
        use_version_hint=True
    )

    instance.query(
        f"INSERT INTO {table_name} VALUES "
        f"(toDateTime64('2024-01-15 08:00:00', 6), 1), "
        f"(toDateTime64('2024-01-15 23:59:59', 6), 2), "
        f"(toDateTime64('2024-01-16 09:00:00', 6), 3), "
        f"(toDateTime64('2024-01-17 09:00:00', 6), 4)",
        settings=WRITE_SETTINGS
    )

    assert instance.query(f"SELECT count() FROM {table_name}").strip() == "4"

    instance.query(
        f"ALTER TABLE {table_name} DROP PARTITION '2024-01-15'",
        settings=WRITE_SETTINGS
    )

    assert instance.query(f"SELECT count() FROM {table_name}").strip() == "2"


def test_drop_partition_with_position_deletes(started_cluster_iceberg_no_spark):
    instance = started_cluster_iceberg_no_spark.instances["node1"]

    table_name = "position_deletes"
    namespace, ch_table = setup(
        instance, table_name, "event_date Date, id Int64", "event_date"
    )

    instance.query(
        f"INSERT INTO {ch_table} VALUES "
        f"('2024-01-15', 1), ('2024-01-15', 2), ('2024-01-16', 3), ('2024-01-17', 4)",
        settings=WRITE_SETTINGS
    )

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "4"

    instance.query(
        f"DELETE FROM {ch_table} WHERE event_date = '2024-01-16'",
        settings=WRITE_SETTINGS
    )

    instance.query(
        f"DELETE FROM {ch_table} WHERE id = 1",
        settings=WRITE_SETTINGS
    )

    assert instance.query(
        f"SELECT count() FROM system.iceberg_files "
        f"WHERE database = '{namespace}' AND table = '{namespace}.{table_name}' "
        f"AND content = 'POSITION_DELETE'"
    ).strip() == "2"

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "2"

    instance.query(
        f"ALTER TABLE {ch_table} DROP PARTITION '2024-01-15'",
        settings=WRITE_SETTINGS
    )

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "1"

    assert instance.query(
        f"SELECT content, count() FROM system.iceberg_files "
        f"WHERE database = '{namespace}' AND table = '{namespace}.{table_name}' "
        f"GROUP BY content ORDER BY content FORMAT TSV"
    ) == "DATA\t2\nPOSITION_DELETE\t1\n"

    operation, summary = current_snapshot_history(instance, namespace, table_name)
    assert operation == "DELETE"
    assert summary["deleted-data-files"] == "1"
    assert summary["removed-position-delete-files"] == "1"
    assert summary["removed-position-deletes"] == "1"

    instance.query(f"DROP DATABASE {namespace}")


def test_drop_partition_hour_transform(started_cluster_iceberg_no_spark):
    instance = started_cluster_iceberg_no_spark.instances["node1"]

    namespace, ch_table = setup(
        instance, "hour_transform", "ts DateTime64(6), id Int64", "toRelativeHourNum(ts)"
    )

    instance.query(
        f"INSERT INTO {ch_table} VALUES "
        f"(toDateTime64('2024-01-15 08:15:00', 6, 'UTC'), 1), "
        f"(toDateTime64('2024-01-15 08:45:00', 6, 'UTC'), 2), "
        f"(toDateTime64('2024-01-15 09:30:00', 6, 'UTC'), 3), "
        f"(toDateTime64('2024-01-15 10:00:00', 6, 'UTC'), 4)",
        settings=WRITE_SETTINGS,
    )

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "4"

    instance.query(
        f"ALTER TABLE {ch_table} DROP PARTITION '2024-01-15 09:30:00'",
        settings=WRITE_SETTINGS,
    )

    assert (
        instance.query(f"SELECT id FROM {ch_table} ORDER BY id FORMAT TSV")
        == "1\n2\n4\n"
    )

    instance.query(
        f"ALTER TABLE {ch_table} DROP PARTITION '2024-01-15 08:59:59'",
        settings=WRITE_SETTINGS,
    )

    assert instance.query(f"SELECT id FROM {ch_table} ORDER BY id FORMAT TSV") == "4\n"

    instance.query(f"DROP DATABASE {namespace}")


def test_drop_partition_after_partition_spec_evolution(started_cluster_iceberg_no_spark):
    """Files written under `day(ts), country` are partitioned more finely than the current
    `day(ts)` spec, so a drop by day must take them along with the files of the current spec."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    catalog = load_catalog_impl(started_cluster_iceberg_no_spark)

    namespace = f"clickhouse_drop_partition_{uuid.uuid4().hex}"
    table_name = "spec_evolution"
    catalog.create_namespace(namespace)

    schema = Schema(
        NestedField(field_id=1, name="ts", field_type=TimestampType(), required=False),
        NestedField(field_id=2, name="country", field_type=StringType(), required=False),
        NestedField(field_id=3, name="id", field_type=LongType(), required=False),
    )
    table = catalog.create_table(
        identifier=f"{namespace}.{table_name}",
        schema=schema,
        location=f"s3://warehouse-rest/{namespace}.{table_name}",
        partition_spec=PartitionSpec(
            PartitionField(
                source_id=1, field_id=1000, transform=DayTransform(), name="ts_day"
            ),
            PartitionField(
                source_id=2, field_id=1001, transform=IdentityTransform(), name="country"
            ),
        ),
    )
    table.append(
        pa.Table.from_pylist(
            [
                {"ts": datetime(2024, 1, 15, 8, 0, 0), "country": "US", "id": 1},
                {"ts": datetime(2024, 1, 15, 12, 0, 0), "country": "DE", "id": 2},
                {"ts": datetime(2024, 1, 16, 9, 0, 0), "country": "US", "id": 3},
            ],
            schema=schema.as_arrow(),
        )
    )

    with table.update_spec() as update:
        update.remove_field("country")

    table.append(
        pa.Table.from_pylist(
            [
                {"ts": datetime(2024, 1, 15, 20, 0, 0), "country": "JP", "id": 4},
                {"ts": datetime(2024, 1, 17, 10, 0, 0), "country": "US", "id": 5},
            ],
            schema=schema.as_arrow(),
        )
    )

    create_iceberg_database(instance, namespace)
    ch_table = f"{namespace}.`{namespace}.{table_name}`"

    assert instance.query(f"SELECT count() FROM {ch_table}").strip() == "5"

    instance.query(
        f"ALTER TABLE {ch_table} DROP PARTITION '2024-01-15'", settings=WRITE_SETTINGS
    )

    assert (
        instance.query(f"SELECT id FROM {ch_table} ORDER BY id FORMAT TSV") == "3\n5\n"
    )

    operation, summary = current_snapshot_history(instance, namespace, table_name)
    assert operation == "DELETE"
    assert summary["deleted-data-files"] == "3"
    assert summary["deleted-records"] == "3"

    instance.query(f"DROP DATABASE {namespace}")


def data_files_in_storage(started_cluster, namespace, table_name):
    return sorted(
        name
        for name in list_s3_objects(
            started_cluster.minio_client,
            "warehouse-rest",
            prefix=f"{namespace}/{table_name}/",
        )
        if name.endswith(".parquet")
    )


def test_drop_partition_purges_data_files(started_cluster_iceberg_no_spark):
    """`iceberg_delete_data_on_drop` decides whether the dropped files leave object storage.
    """
    instance = started_cluster_iceberg_no_spark.instances["node1"]

    table_name = "purge_data_files"
    namespace, ch_table = setup(
        instance, table_name, "event_date Date, id Int64", "event_date"
    )

    instance.query(
        f"INSERT INTO {ch_table} VALUES "
        f"('2024-01-15', 1), ('2024-01-15', 2), ('2024-01-16', 3), ('2024-01-17', 4)",
        settings=WRITE_SETTINGS,
    )

    after_insert = data_files_in_storage(
        started_cluster_iceberg_no_spark, namespace, table_name
    )
    assert len(after_insert) == 3

    instance.query(
        f"ALTER TABLE {ch_table} DROP PARTITION '2024-01-16'",
        settings=dict(WRITE_SETTINGS, iceberg_delete_data_on_drop=0),
    )

    assert instance.query(f"SELECT id FROM {ch_table} ORDER BY id FORMAT TSV") == "1\n2\n4\n"
    assert (
        data_files_in_storage(started_cluster_iceberg_no_spark, namespace, table_name)
        == after_insert
    )

    instance.query(
        f"ALTER TABLE {ch_table} DROP PARTITION '2024-01-15'",
        settings=dict(WRITE_SETTINGS, iceberg_delete_data_on_drop=1),
    )

    assert instance.query(f"SELECT id FROM {ch_table} ORDER BY id FORMAT TSV") == "4\n"

    after_purge = data_files_in_storage(
        started_cluster_iceberg_no_spark, namespace, table_name
    )
    assert len(after_purge) == 2
    assert set(after_purge) < set(after_insert)

    operation, summary = current_snapshot_history(instance, namespace, table_name)
    assert operation == "DELETE"
    assert summary["deleted-data-files"] == "1"

    instance.query(f"DROP DATABASE {namespace}")
