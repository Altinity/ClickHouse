#!/usr/bin/env python3
"""
Reproducer for Altinity/ClickHouse#1545:
icebergLocal() returns all-NULL columns when allow_experimental_iceberg_read_optimization=1
and the Iceberg manifest has no column statistics.

Root cause
----------
The manifest Avro writer schema intentionally omits the optional stats fields
(value_counts, column_sizes, null_value_counts, lower_bounds, upper_bounds).
AvroForIcebergDeserializer.hasPath() therefore returns false for all three stats
paths in ManifestFile.cpp.  columns_infos is left empty.

StorageObjectStorageSource::createReader's second loop (the schema-evolution
"absent column as NULL" path) iterates over every requested column.  Because
columns_infos is empty, every nullable column is absent → each is injected as a
constant NULL.  With all requested columns constant, need_only_count is set to
true and the Parquet file is read in count-only mode; the output is the correct
number of rows, but every value is NULL.

Tests
-----
- test_iceberg_local_returns_actual_rows_with_stats_less_manifest
  FAILS on unpatched code: returns all-NULL rows instead of real data.

- test_iceberg_local_returns_correct_rows_when_optimization_disabled
  PASSES on unpatched code: regression guard — optimization=0 bypasses the
  buggy path and reads the Parquet file normally.
"""

import json
import os
import tempfile
import time
import uuid

import avro.datafile
import avro.io
import avro.schema
import pyarrow as pa
import pyarrow.parquet as pq

from helpers.iceberg_utils import get_uuid_str
from helpers.s3_tools import LocalUploader


# Iceberg v2 manifest list Avro schema (minimal fields needed by ClickHouse).
_MANIFEST_LIST_SCHEMA_STR = json.dumps({
    "type": "record",
    "name": "manifest_file",
    "fields": [
        {"name": "manifest_path",        "type": "string"},
        {"name": "manifest_length",      "type": "long"},
        {"name": "partition_spec_id",    "type": "int"},
        {"name": "content",              "type": "int"},
        {"name": "sequence_number",      "type": "long"},
        {"name": "min_sequence_number",  "type": "long"},
        {"name": "added_snapshot_id",    "type": "long"},
        {"name": "added_files_count",    "type": "int"},
        {"name": "existing_files_count", "type": "int"},
        {"name": "deleted_files_count",  "type": "int"},
        {"name": "added_rows_count",     "type": "long"},
        {"name": "existing_rows_count",  "type": "long"},
        {"name": "deleted_rows_count",   "type": "long"},
    ],
})

# Manifest entry Avro schema that deliberately omits all per-column stats fields:
#   column_sizes, value_counts, null_value_counts, lower_bounds, upper_bounds.
#
# Absent fields → AvroForIcebergDeserializer.hasPath() returns false for each →
# ManifestFile.cpp skips stats collection → columns_infos is empty →
# the optimization treats every nullable column as a "schema-evolution absent column"
# and injects a constant NULL.
_MANIFEST_ENTRY_NO_STATS_SCHEMA_STR = json.dumps({
    "type": "record",
    "name": "manifest_entry",
    "fields": [
        {"name": "status",               "type": "int"},
        {"name": "snapshot_id",          "type": ["null", "long"]},
        {"name": "sequence_number",      "type": ["null", "long"]},
        {"name": "file_sequence_number", "type": ["null", "long"]},
        {
            "name": "data_file",
            "type": {
                "type": "record",
                "name": "r2",
                "fields": [
                    {"name": "content",            "type": "int"},
                    {"name": "file_path",          "type": "string"},
                    {"name": "file_format",        "type": "string"},
                    {
                        "name": "partition",
                        "type": {"type": "record", "name": "r102", "fields": []},
                    },
                    {"name": "record_count",       "type": "long"},
                    {"name": "file_size_in_bytes", "type": "long"},
                ],
            },
        },
    ],
})


def _write_avro(schema, records, path, metadata=None):
    with open(path, "wb") as f:
        writer = avro.datafile.DataFileWriter(f, avro.io.DatumWriter(), schema)
        if metadata:
            for k, v in metadata.items():
                writer.set_meta(k, v if isinstance(v, bytes) else v.encode("utf-8"))
        for rec in records:
            writer.append(rec)
        writer.close()


def _create_stats_less_iceberg_table(tmpdir, table_name, container_base):
    """
    Build a minimal Iceberg v2 table under tmpdir/table_name.  The manifest is
    written with a stats-less Avro schema to trigger the optimization bug.

    container_base is the absolute path inside the ClickHouse container where the
    table will be placed after upload.  File paths embedded in the Avro records and
    metadata.json must reference container_base because they are interpreted at
    query time inside the container.

    Returns the local table directory path (tmpdir/table_name).
    """
    table_local = os.path.join(tmpdir, table_name)
    data_local  = os.path.join(table_local, "data")
    meta_local  = os.path.join(table_local, "metadata")
    os.makedirs(data_local)
    os.makedirs(meta_local)

    # Parquet data file with two nullable columns.
    arrow_schema = pa.schema([
        pa.field("id",   pa.int32(),  nullable=True),
        pa.field("data", pa.string(), nullable=True),
    ])
    pq.write_table(
        pa.table(
            {
                "id":   pa.array([1, 2, 3], type=pa.int32()),
                "data": pa.array(["hello", "world", "iceberg"], type=pa.string()),
            },
            schema=arrow_schema,
        ),
        os.path.join(data_local, "00000-0-data.parquet"),
    )
    data_size           = os.path.getsize(os.path.join(data_local, "00000-0-data.parquet"))
    data_container_path = f"{container_base}/data/00000-0-data.parquet"

    snapshot_id = 1
    seq_number  = 1
    ts_ms       = int(time.time() * 1000)

    # Iceberg schema and partition-spec to embed as Avro file-level metadata.
    # ManifestFile.cpp requires both keys to be present in the manifest's Avro header.
    iceberg_schema = {
        "type": "struct",
        "schema-id": 0,
        "fields": [
            {"id": 1, "name": "id",   "required": False, "type": "int"},
            {"id": 2, "name": "data", "required": False, "type": "string"},
        ],
    }

    # Manifest file (stats-less Avro schema).
    manifest_local_path     = os.path.join(meta_local, "00000-0-manifest.avro")
    manifest_container_path = f"{container_base}/metadata/00000-0-manifest.avro"
    _write_avro(
        avro.schema.parse(_MANIFEST_ENTRY_NO_STATS_SCHEMA_STR),
        [{
            "status":               1,             # ADDED
            "snapshot_id":          snapshot_id,
            "sequence_number":      seq_number,
            "file_sequence_number": seq_number,
            "data_file": {
                "content":            0,            # DATA
                "file_path":          data_container_path,
                "file_format":        "PARQUET",
                "partition":          {},
                "record_count":       3,
                "file_size_in_bytes": data_size,
            },
        }],
        manifest_local_path,
        metadata={
            "schema":         json.dumps(iceberg_schema),
            "partition-spec": "[]",
        },
    )
    manifest_size = os.path.getsize(manifest_local_path)

    # Manifest list.
    mlist_filename      = f"snap-{snapshot_id}-0-manifest-list.avro"
    mlist_local_path    = os.path.join(meta_local, mlist_filename)
    mlist_container_path = f"{container_base}/metadata/{mlist_filename}"
    _write_avro(
        avro.schema.parse(_MANIFEST_LIST_SCHEMA_STR),
        [{
            "manifest_path":        manifest_container_path,
            "manifest_length":      manifest_size,
            "partition_spec_id":    0,
            "content":              0,             # DATA
            "sequence_number":      seq_number,
            "min_sequence_number":  seq_number,
            "added_snapshot_id":    snapshot_id,
            "added_files_count":    1,
            "existing_files_count": 0,
            "deleted_files_count":  0,
            "added_rows_count":     3,
            "existing_rows_count":  0,
            "deleted_rows_count":   0,
        }],
        mlist_local_path,
    )

    # Table metadata JSON.
    metadata = {
        "format-version":       2,
        "table-uuid":           str(uuid.uuid4()),
        "location":             container_base,
        "last-sequence-number": seq_number,
        "last-updated-ms":      ts_ms,
        "last-column-id":       2,
        "current-schema-id":    0,
        "schemas": [{
            "type":      "struct",
            "schema-id": 0,
            "fields": [
                {"id": 1, "name": "id",   "required": False, "type": "int"},
                {"id": 2, "name": "data", "required": False, "type": "string"},
            ],
        }],
        "default-spec-id":       0,
        "partition-specs":       [{"spec-id": 0, "fields": []}],
        "last-partition-id":     999,
        "default-sort-order-id": 0,
        "sort-orders":           [{"order-id": 0, "fields": []}],
        "properties":            {},
        "current-snapshot-id":   snapshot_id,
        "snapshots": [{
            "snapshot-id":    snapshot_id,
            "sequence-number": seq_number,
            "timestamp-ms":   ts_ms,
            "manifest-list":  mlist_container_path,
            "summary":        {"operation": "append"},
            "schema-id":      0,
        }],
        "snapshot-log": [{"timestamp-ms": ts_ms, "snapshot-id": snapshot_id}],
        "metadata-log":  [],
        "refs":          {"main": {"snapshot-id": snapshot_id, "type": "branch"}},
    }
    with open(os.path.join(meta_local, "v1.metadata.json"), "w") as f:
        json.dump(metadata, f, indent=2)

    return table_local


def _upload_to_container(instance, local_table_dir, container_base):
    uploader = LocalUploader(instance)
    for root, _dirs, files in os.walk(local_table_dir):
        for fname in files:
            local_path = os.path.join(root, fname)
            rel        = os.path.relpath(local_path, local_table_dir)
            uploader.upload_file(local_path, os.path.join(container_base, rel))


# Manifest entry schema with value_counts only (partial stats).
# column_sizes and null_value_counts are absent.  value_counts alone is enough to
# set any_stats_field_present=true so the absent-NULL guard is lifted.
_MANIFEST_ENTRY_PARTIAL_STATS_SCHEMA_STR = json.dumps({
    "type": "record",
    "name": "manifest_entry",
    "fields": [
        {"name": "status",               "type": "int"},
        {"name": "snapshot_id",          "type": ["null", "long"]},
        {"name": "sequence_number",      "type": ["null", "long"]},
        {"name": "file_sequence_number", "type": ["null", "long"]},
        {
            "name": "data_file",
            "type": {
                "type": "record",
                "name": "r2",
                "fields": [
                    {"name": "content",            "type": "int"},
                    {"name": "file_path",          "type": "string"},
                    {"name": "file_format",        "type": "string"},
                    {
                        "name": "partition",
                        "type": {"type": "record", "name": "r102", "fields": []},
                    },
                    {"name": "record_count",       "type": "long"},
                    {"name": "file_size_in_bytes", "type": "long"},
                    # Only value_counts is present (column_sizes and null_value_counts absent).
                    {
                        "name": "value_counts",
                        "type": {
                            "type": "array",
                            "items": {
                                "type": "record",
                                "name": "k81v81",
                                "fields": [
                                    {"name": "key",   "type": "int"},
                                    {"name": "value", "type": "long"},
                                ],
                            },
                        },
                    },
                ],
            },
        },
    ],
})

# Manifest entry schema with all three stats fields (value_counts, column_sizes,
# null_value_counts).  Represents a manifest written by a full-stats writer such as Spark.
_MANIFEST_ENTRY_FULL_STATS_SCHEMA_STR = json.dumps({
    "type": "record",
    "name": "manifest_entry",
    "fields": [
        {"name": "status",               "type": "int"},
        {"name": "snapshot_id",          "type": ["null", "long"]},
        {"name": "sequence_number",      "type": ["null", "long"]},
        {"name": "file_sequence_number", "type": ["null", "long"]},
        {
            "name": "data_file",
            "type": {
                "type": "record",
                "name": "r2",
                "fields": [
                    {"name": "content",            "type": "int"},
                    {"name": "file_path",          "type": "string"},
                    {"name": "file_format",        "type": "string"},
                    {
                        "name": "partition",
                        "type": {"type": "record", "name": "r102", "fields": []},
                    },
                    {"name": "record_count",       "type": "long"},
                    {"name": "file_size_in_bytes", "type": "long"},
                    {
                        "name": "column_sizes",
                        "type": {
                            "type": "array",
                            "items": {
                                "type": "record",
                                "name": "k81v81",
                                "fields": [
                                    {"name": "key",   "type": "int"},
                                    {"name": "value", "type": "long"},
                                ],
                            },
                        },
                    },
                    {
                        "name": "value_counts",
                        "type": {
                            "type": "array",
                            "items": {
                                "type": "record",
                                "name": "k81v81_vc",
                                "fields": [
                                    {"name": "key",   "type": "int"},
                                    {"name": "value", "type": "long"},
                                ],
                            },
                        },
                    },
                    {
                        "name": "null_value_counts",
                        "type": {
                            "type": "array",
                            "items": {
                                "type": "record",
                                "name": "k81v81_nc",
                                "fields": [
                                    {"name": "key",   "type": "int"},
                                    {"name": "value", "type": "long"},
                                ],
                            },
                        },
                    },
                ],
            },
        },
    ],
})


def _create_iceberg_table_with_schema(tmpdir, table_name, container_base,
                                      manifest_schema_str, manifest_extra_fields=None):
    """
    Build a minimal Iceberg v2 table.  manifest_extra_fields is added to each
    manifest entry's data_file record when using schemas that include stats.
    """
    table_local = os.path.join(tmpdir, table_name)
    data_local  = os.path.join(table_local, "data")
    meta_local  = os.path.join(table_local, "metadata")
    os.makedirs(data_local)
    os.makedirs(meta_local)

    arrow_schema = pa.schema([
        pa.field("id",   pa.int32(),  nullable=True),
        pa.field("data", pa.string(), nullable=True),
    ])
    pq.write_table(
        pa.table(
            {
                "id":   pa.array([1, 2, 3], type=pa.int32()),
                "data": pa.array(["hello", "world", "iceberg"], type=pa.string()),
            },
            schema=arrow_schema,
        ),
        os.path.join(data_local, "00000-0-data.parquet"),
    )
    data_size           = os.path.getsize(os.path.join(data_local, "00000-0-data.parquet"))
    data_container_path = f"{container_base}/data/00000-0-data.parquet"

    snapshot_id = 1
    seq_number  = 1
    ts_ms       = int(time.time() * 1000)

    iceberg_schema = {
        "type": "struct",
        "schema-id": 0,
        "fields": [
            {"id": 1, "name": "id",   "required": False, "type": "int"},
            {"id": 2, "name": "data", "required": False, "type": "string"},
        ],
    }

    data_file_record = {
        "content":            0,
        "file_path":          data_container_path,
        "file_format":        "PARQUET",
        "partition":          {},
        "record_count":       3,
        "file_size_in_bytes": data_size,
    }
    if manifest_extra_fields:
        data_file_record.update(manifest_extra_fields)

    manifest_local_path     = os.path.join(meta_local, "00000-0-manifest.avro")
    manifest_container_path = f"{container_base}/metadata/00000-0-manifest.avro"
    _write_avro(
        avro.schema.parse(manifest_schema_str),
        [{
            "status":               1,
            "snapshot_id":          snapshot_id,
            "sequence_number":      seq_number,
            "file_sequence_number": seq_number,
            "data_file":            data_file_record,
        }],
        manifest_local_path,
        metadata={
            "schema":         json.dumps(iceberg_schema),
            "partition-spec": "[]",
        },
    )
    manifest_size = os.path.getsize(manifest_local_path)

    mlist_filename       = f"snap-{snapshot_id}-0-manifest-list.avro"
    mlist_local_path     = os.path.join(meta_local, mlist_filename)
    mlist_container_path = f"{container_base}/metadata/{mlist_filename}"
    _write_avro(
        avro.schema.parse(_MANIFEST_LIST_SCHEMA_STR),
        [{
            "manifest_path":        manifest_container_path,
            "manifest_length":      manifest_size,
            "partition_spec_id":    0,
            "content":              0,
            "sequence_number":      seq_number,
            "min_sequence_number":  seq_number,
            "added_snapshot_id":    snapshot_id,
            "added_files_count":    1,
            "existing_files_count": 0,
            "deleted_files_count":  0,
            "added_rows_count":     3,
            "existing_rows_count":  0,
            "deleted_rows_count":   0,
        }],
        mlist_local_path,
    )

    metadata = {
        "format-version":       2,
        "table-uuid":           str(uuid.uuid4()),
        "location":             container_base,
        "last-sequence-number": seq_number,
        "last-updated-ms":      ts_ms,
        "last-column-id":       2,
        "current-schema-id":    0,
        "schemas": [{
            "type":      "struct",
            "schema-id": 0,
            "fields": [
                {"id": 1, "name": "id",   "required": False, "type": "int"},
                {"id": 2, "name": "data", "required": False, "type": "string"},
            ],
        }],
        "default-spec-id":       0,
        "partition-specs":       [{"spec-id": 0, "fields": []}],
        "last-partition-id":     999,
        "default-sort-order-id": 0,
        "sort-orders":           [{"order-id": 0, "fields": []}],
        "properties":            {},
        "current-snapshot-id":   snapshot_id,
        "snapshots": [{
            "snapshot-id":    snapshot_id,
            "sequence-number": seq_number,
            "timestamp-ms":   ts_ms,
            "manifest-list":  mlist_container_path,
            "summary":        {"operation": "append"},
            "schema-id":      0,
        }],
        "snapshot-log": [{"timestamp-ms": ts_ms, "snapshot-id": snapshot_id}],
        "metadata-log":  [],
        "refs":          {"main": {"snapshot-id": snapshot_id, "type": "branch"}},
    }
    with open(os.path.join(meta_local, "v1.metadata.json"), "w") as f:
        json.dump(metadata, f, indent=2)

    return table_local


def test_iceberg_local_returns_actual_rows_with_stats_less_manifest(
    started_cluster_iceberg_no_spark,
):
    """
    FAILS on unpatched code (Altinity#1545).

    With allow_experimental_iceberg_read_optimization=1 (the default), reading an
    icebergLocal table whose manifest omits column statistics returns all-NULL rows
    instead of real data.  The correct output is 3 rows: (1,'hello'), (2,'world'),
    (3,'iceberg').
    """
    instance   = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_opt_on_stats_less_" + get_uuid_str()
    container_base = (
        f"/var/lib/clickhouse/user_files/iceberg_data/default/{table_name}"
    )

    with tempfile.TemporaryDirectory() as tmpdir:
        local_dir = _create_stats_less_iceberg_table(tmpdir, table_name, container_base)
        _upload_to_container(instance, local_dir, container_base)

    result = instance.query(
        f"SELECT * FROM icebergLocal(local, path='{container_base}', format=Parquet)"
        " ORDER BY id"
        " SETTINGS allow_experimental_iceberg_read_optimization=1"
    ).strip()

    assert result == "1\thello\n2\tworld\n3\ticeberg", (
        f"Got: {result!r}\n"
        "All-NULL rows means the bug fired: empty columns_infos caused the "
        "optimization to treat every nullable column as an absent schema-evolution "
        "column and inject constant NULL (need_only_count=1)."
    )


def test_iceberg_local_returns_correct_rows_when_optimization_disabled(
    started_cluster_iceberg_no_spark,
):
    """
    PASSES on unpatched code.

    With allow_experimental_iceberg_read_optimization=0 the optimization block is
    skipped entirely and the Parquet file is read normally.  This test is a
    regression guard: it must keep passing after the bug is fixed.
    """
    instance   = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_opt_off_stats_less_" + get_uuid_str()
    container_base = (
        f"/var/lib/clickhouse/user_files/iceberg_data/default/{table_name}"
    )

    with tempfile.TemporaryDirectory() as tmpdir:
        local_dir = _create_stats_less_iceberg_table(tmpdir, table_name, container_base)
        _upload_to_container(instance, local_dir, container_base)

    result = instance.query(
        f"SELECT * FROM icebergLocal(local, path='{container_base}', format=Parquet)"
        " ORDER BY id"
        " SETTINGS allow_experimental_iceberg_read_optimization=0"
    ).strip()

    assert result == "1\thello\n2\tworld\n3\ticeberg"


def test_iceberg_local_partial_stats_manifest_reads_correctly(
    started_cluster_iceberg_no_spark,
):
    """
    Regression test for Altinity#1545: partial stats (value_counts only).

    A manifest that includes value_counts but omits column_sizes and
    null_value_counts still sets any_stats_field_present=true.  The fix must
    treat the columns as real (not absent), so real data is returned.
    """
    instance   = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_opt_on_partial_stats_" + get_uuid_str()
    container_base = (
        f"/var/lib/clickhouse/user_files/iceberg_data/default/{table_name}"
    )

    # value_counts for field_id 1 (id) and 2 (data): 3 rows each.
    extra = {
        "value_counts": [
            {"key": 1, "value": 3},
            {"key": 2, "value": 3},
        ],
    }

    with tempfile.TemporaryDirectory() as tmpdir:
        local_dir = _create_iceberg_table_with_schema(
            tmpdir, table_name, container_base,
            _MANIFEST_ENTRY_PARTIAL_STATS_SCHEMA_STR,
            manifest_extra_fields=extra,
        )
        _upload_to_container(instance, local_dir, container_base)

    result = instance.query(
        f"SELECT * FROM icebergLocal(local, path='{container_base}', format=Parquet)"
        " ORDER BY id"
        " SETTINGS allow_experimental_iceberg_read_optimization=1"
    ).strip()

    assert result == "1\thello\n2\tworld\n3\ticeberg", (
        f"Got: {result!r}\n"
        "Partial-stats manifest (value_counts only) should not trigger the absent-NULL "
        "path because any_stats_field_present=true.  All-NULL rows means the guard "
        "did not fire for partial-stats manifests."
    )


def test_iceberg_local_full_stats_manifest_reads_correctly(
    started_cluster_iceberg_no_spark,
):
    """
    Control test: full-stats manifest (Spark-like) must continue to return real data.

    A manifest with all three stats fields (column_sizes, value_counts,
    null_value_counts) is the common case written by Spark.  This test verifies the
    optimization does not regress for the normal path after the fix.
    """
    instance   = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_opt_on_full_stats_" + get_uuid_str()
    container_base = (
        f"/var/lib/clickhouse/user_files/iceberg_data/default/{table_name}"
    )

    # Full stats for field_id 1 (id) and 2 (data).
    extra = {
        "column_sizes": [
            {"key": 1, "value": 64},
            {"key": 2, "value": 128},
        ],
        "value_counts": [
            {"key": 1, "value": 3},
            {"key": 2, "value": 3},
        ],
        "null_value_counts": [
            {"key": 1, "value": 0},
            {"key": 2, "value": 0},
        ],
    }

    with tempfile.TemporaryDirectory() as tmpdir:
        local_dir = _create_iceberg_table_with_schema(
            tmpdir, table_name, container_base,
            _MANIFEST_ENTRY_FULL_STATS_SCHEMA_STR,
            manifest_extra_fields=extra,
        )
        _upload_to_container(instance, local_dir, container_base)

    result = instance.query(
        f"SELECT * FROM icebergLocal(local, path='{container_base}', format=Parquet)"
        " ORDER BY id"
        " SETTINGS allow_experimental_iceberg_read_optimization=1"
    ).strip()

    assert result == "1\thello\n2\tworld\n3\ticeberg", (
        f"Got: {result!r}\n"
        "Full-stats manifest should return real data when optimization is enabled."
    )
