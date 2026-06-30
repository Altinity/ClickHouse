#!/usr/bin/env python3
"""
Regression coverage for Altinity/ClickHouse#1545.
"""
import json
import os
import tempfile
import time

import avro.datafile
import avro.io
import avro.schema
import pyarrow as pa
import pyarrow.parquet as pq

from helpers.iceberg_utils import get_uuid_str
from helpers.s3_tools import LocalUploader
from helpers.iceberg_avro_schemas import (
    MANIFEST_LIST_SCHEMA_STR,
    MANIFEST_ENTRY_NO_STATS_SCHEMA_STR,
)

_IDS = [1, 2, 3]
_DATA = ["hello", "world", "iceberg"]
_REAL_DATA_RESULT = "1\thello\n2\tworld\n3\ticeberg"
_DATA_NULL_RESULT = "1\t\\N\n2\t\\N\n3\t\\N"
_TABLE_UUID = "01234567-89ab-cdef-0123-456789abcdef"

def _schema_fields(with_data):
    fields = [{"id": 1, "name": "id", "required": False, "type": "int"}]
    if with_data:
        fields.append({"id": 2, "name": "data", "required": False, "type": "string"})
    return fields


def _iceberg_schema(schema_id, with_data):
    return {"type": "struct", "schema-id": schema_id, "fields": _schema_fields(with_data)}


def _write_avro(schema_str, records, path, metadata=None):
    with open(path, "wb") as f:
        writer = avro.datafile.DataFileWriter(
            f, avro.io.DatumWriter(), avro.schema.parse(schema_str)
        )
        if metadata:
            for k, v in metadata.items():
                writer.set_meta(k, v if isinstance(v, bytes) else v.encode("utf-8"))
        for rec in records:
            writer.append(rec)
        writer.close()


def _create_iceberg_table(
    tmpdir,
    table_name,
    container_base,
    *,
    file_has_data_column=True,
    schema_evolution=False,
):
    """
    Build a minimal stats-less Iceberg v2 table under ``tmpdir/table_name``.
    """
    table_local = os.path.join(tmpdir, table_name)
    data_local  = os.path.join(table_local, "data")
    meta_local  = os.path.join(table_local, "metadata")
    os.makedirs(data_local)
    os.makedirs(meta_local)

    arrow_fields = [pa.field("id", pa.int32(), nullable=True)]
    table_data = {"id": pa.array(_IDS, type=pa.int32())}
    if file_has_data_column:
        arrow_fields.append(pa.field("data", pa.string(), nullable=True))
        table_data["data"] = pa.array(_DATA, type=pa.string())
    pq.write_table(
        pa.table(table_data, schema=pa.schema(arrow_fields)),
        os.path.join(data_local, "00000-0-data.parquet"),
    )
    data_size           = os.path.getsize(os.path.join(data_local, "00000-0-data.parquet"))
    data_container_path = f"{container_base}/data/00000-0-data.parquet"

    ts_ms = int(time.time() * 1000)

    # snapshot_id is the snapshot that wrote the file. Under schema evolution it
    # carries the older schema (0) while the table reads at schema 1, which makes
    # getInitialSchemaByPath return the file's own schema as initial_header.
    entry_snapshot_id = 1
    write_schema_id   = 0

    data_file_record = {
        "content":            0,            # DATA
        "file_path":          data_container_path,
        "file_format":        "PARQUET",
        "partition":          {},
        "record_count":       len(_IDS),
        "file_size_in_bytes": data_size,
    }

    manifest_local_path     = os.path.join(meta_local, "00000-0-manifest.avro")
    manifest_container_path = f"{container_base}/metadata/00000-0-manifest.avro"
    _write_avro(
        MANIFEST_ENTRY_NO_STATS_SCHEMA_STR,
        [{
            "status":               1,             # ADDED
            "snapshot_id":          entry_snapshot_id,
            "sequence_number":      1,
            "file_sequence_number": 1,
            "data_file":            data_file_record,
        }],
        manifest_local_path,
        metadata={
            # Manifest's own schema = the data file's write schema.
            "schema":         json.dumps(_iceberg_schema(write_schema_id, file_has_data_column)),
            "partition-spec": "[]",
        },
    )
    manifest_size = os.path.getsize(manifest_local_path)

    def _write_manifest_list(snapshot_id):
        filename       = f"snap-{snapshot_id}-0-manifest-list.avro"
        local_path     = os.path.join(meta_local, filename)
        container_path = f"{container_base}/metadata/{filename}"
        _write_avro(
            MANIFEST_LIST_SCHEMA_STR,
            [{
                "manifest_path":        manifest_container_path,
                "manifest_length":      manifest_size,
                "partition_spec_id":    0,
                "content":              0,             # DATA
                "sequence_number":      1,
                "min_sequence_number":  1,
                "added_snapshot_id":    entry_snapshot_id,
                "added_files_count":    1,
                "existing_files_count": 0,
                "deleted_files_count":  0,
                "added_rows_count":     len(_IDS),
                "existing_rows_count":  0,
                "deleted_rows_count":   0,
            }],
            local_path,
        )
        return container_path

    if schema_evolution:
        # Written under schema 0 (id only), later evolved to schema 1 (id+data).
        schemas          = [_iceberg_schema(0, with_data=False), _iceberg_schema(1, with_data=True)]
        current_schema   = 1
        snapshots = [
            {"snapshot-id": 1, "schema-id": 0, "manifest-list": _write_manifest_list(1)},
            {"snapshot-id": 2, "schema-id": 1, "manifest-list": _write_manifest_list(2)},
        ]
        current_snapshot = 2
        last_seq         = 2
    else:
        # Single schema (id+data)
        schemas          = [_iceberg_schema(0, with_data=True)]
        current_schema   = 0
        snapshots = [
            {"snapshot-id": 1, "schema-id": 0, "manifest-list": _write_manifest_list(1)},
        ]
        current_snapshot = 1
        last_seq         = 1

    metadata = {
        "format-version":       2,
        "table-uuid":           _TABLE_UUID,
        "location":             container_base,
        "last-sequence-number": last_seq,
        "last-updated-ms":      ts_ms,
        "last-column-id":       2,
        "current-schema-id":    current_schema,
        "schemas":              schemas,
        "default-spec-id":       0,
        "partition-specs":       [{"spec-id": 0, "fields": []}],
        "last-partition-id":     999,
        "default-sort-order-id": 0,
        "sort-orders":           [{"order-id": 0, "fields": []}],
        "properties":            {},
        "current-snapshot-id":   current_snapshot,
        "snapshots": [
            {
                "snapshot-id":     s["snapshot-id"],
                "sequence-number": s["snapshot-id"],
                "timestamp-ms":    ts_ms,
                "manifest-list":   s["manifest-list"],
                "summary":         {"operation": "append"},
                "schema-id":       s["schema-id"],
            }
            for s in snapshots
        ],
        "snapshot-log": [{"timestamp-ms": ts_ms, "snapshot-id": s["snapshot-id"]} for s in snapshots],
        "metadata-log":  [],
        "refs":          {"main": {"snapshot-id": current_snapshot, "type": "branch"}},
    }
    with open(os.path.join(meta_local, "v1.metadata.json"), "w") as f:
        json.dump(metadata, f, indent=2)

    return table_local


def _upload_to_node(instance, local_table_dir, container_base):
    uploader = LocalUploader(instance)
    for root, _dirs, files in os.walk(local_table_dir):
        for fname in files:
            local_path = os.path.join(root, fname)
            rel        = os.path.relpath(local_path, local_table_dir)
            uploader.upload_file(local_path, os.path.join(container_base, rel))


def _container_base(table_name):
    return f"/var/lib/clickhouse/user_files/iceberg_data/default/{table_name}"


def _query_local(instance, container_base, optimization=1):
    return instance.query(
        f"SELECT * FROM icebergLocal(local, path='{container_base}', format=Parquet)"
        " ORDER BY id"
        f" SETTINGS allow_experimental_iceberg_read_optimization={optimization}"
    ).strip()


def test_stats_less_manifest_returns_real_data(started_cluster_iceberg_no_spark):
    instance       = started_cluster_iceberg_no_spark.instances["node1"]
    table_name     = "test_stats_less_real_" + get_uuid_str()
    container_base = _container_base(table_name)

    with tempfile.TemporaryDirectory() as tmpdir:
        local_dir = _create_iceberg_table(tmpdir, table_name, container_base)
        _upload_to_node(instance, local_dir, container_base)

    result = _query_local(instance, container_base)
    assert result == _REAL_DATA_RESULT, f"Got: {result!r}"


def test_stats_less_manifest_schema_evolution_absent_column(started_cluster_iceberg_no_spark):
    instance       = started_cluster_iceberg_no_spark.instances["node1"]
    table_name     = "test_stats_less_evolve_" + get_uuid_str()
    container_base = _container_base(table_name)

    with tempfile.TemporaryDirectory() as tmpdir:
        local_dir = _create_iceberg_table(
            tmpdir, table_name, container_base,
            file_has_data_column=False,
            schema_evolution=True,
        )
        _upload_to_node(instance, local_dir, container_base)

    result = _query_local(instance, container_base)
    # id NULL too -> empty-stats bug fired; a query error -> the fall-through
    # failed to synthesize the schema-evolved-absent column.
    assert result == _DATA_NULL_RESULT, f"Got: {result!r}"


def test_stats_less_manifest_cluster_returns_real_data(started_cluster_iceberg_no_spark):
    # Cluster path: empty columns_info must survive the DataFileMetaInfo JSON
    # round-trip to the worker, not just the single-node read.
    cluster        = started_cluster_iceberg_no_spark
    instance       = cluster.instances["node1"]
    table_name     = "test_stats_less_cluster_" + get_uuid_str()
    container_base = _container_base(table_name)

    with tempfile.TemporaryDirectory() as tmpdir:
        local_dir = _create_iceberg_table(tmpdir, table_name, container_base)
        for node in cluster.instances.values():
            _upload_to_node(node, local_dir, container_base)

    result = instance.query(
        f"SELECT * FROM icebergLocalCluster('cluster_simple', local, path='{container_base}', format=Parquet)"
        " ORDER BY id"
        " SETTINGS allow_experimental_iceberg_read_optimization=1"
    ).strip()
    assert result == _REAL_DATA_RESULT, f"Got: {result!r}"


def test_mergetree_lasagna_roundtrip(started_cluster_iceberg_no_spark):
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table    = "test_lasagna_" + get_uuid_str()

    instance.query(f"DROP TABLE IF EXISTS {table}")
    instance.query(
        f"CREATE TABLE {table} (id Int32, ingredient String) "
        "ENGINE = MergeTree ORDER BY id"
    )

    ingredients = [
        "pasta", "tomato", "mozzarella", "basil",
        "ground beef", "onion", "garlic", "parmesan", "ricotta",
    ]
    values = ", ".join(f"({i + 1}, '{ing}')" for i, ing in enumerate(ingredients))
    instance.query(f"INSERT INTO {table} VALUES {values}")

    result   = instance.query(f"SELECT id, ingredient FROM {table} ORDER BY id").strip()
    expected = "\n".join(f"{i + 1}\t{ing}" for i, ing in enumerate(ingredients))
    assert result == expected, f"Got: {result!r}"

    instance.query(f"DROP TABLE IF EXISTS {table}")
