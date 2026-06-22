#!/usr/bin/env python3
"""
Regression coverage for Altinity/ClickHouse#1545:
the Iceberg read optimization (``allow_experimental_iceberg_read_optimization=1``)
returned all-NULL columns when the manifest carried no per-file column statistics.

Root cause
----------
When a manifest's optional stats fields (``value_counts``, ``column_sizes``,
``null_value_counts``, ``lower_bounds``, ``upper_bounds``) are all absent --
common for non-Spark writers such as pyiceberg with default settings --
``DataFileMetaInfo::columns_info`` is left empty.  The optimization's
absent-column loop in ``StorageObjectStorageSource::createReader`` then iterated
every requested column, found none in the empty ``columns_info`` map, and
injected each nullable column as a constant ``NULL``.  With every requested
column constant, ``need_only_count`` became true and the Parquet file was read
in count-only mode: correct row count, every value ``NULL``.

The fix gates that loop on ``!columns_info.empty()``; an empty map now falls
through to the Parquet reader, which reads physically-present columns normally
and synthesizes schema-evolved-absent columns as ``NULL`` via
``IcebergMetadata::getInitialSchemaByPath``.

Test taxonomy
-------------
Discriminating (fail on the unpatched build, pass on the fixed build):
  - test_stats_less_manifest_returns_real_data
  - test_stats_less_manifest_schema_evolution_absent_column
Cluster/serialization path (the cluster JSON round-trip of an empty
``columns_info``):
  - test_stats_less_manifest_cluster_returns_real_data
Non-regression guards (pass on both builds; the optimization's legitimate
absent-NULL and present-column behavior must keep working when stats exist):
  - test_non_empty_stats_absent_column_still_null
  - test_non_empty_stats_returns_real_data

Why the Iceberg fixtures are generated in code rather than checked in as static
files: every manifest entry and the table ``metadata.json`` embed per-test
runtime values -- the UUID-suffixed container path, the byte sizes of the
freshly written Parquet and Avro files, and a random ``table-uuid`` -- so they
are templates filled at run time, not the static blobs that a checked-in fixture
(e.g. ``tests/queries/0_stateless/data_minio/.../v1.metadata.json``) would be.
The one genuinely static part, the Avro schemas, is deduplicated into the
module-level constants below; the stats-less manifest entry -- the artifact
under test -- is necessarily produced in code.
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
import pytest

from helpers.iceberg_utils import get_uuid_str
from helpers.s3_tools import LocalUploader


# The three rows every fixture stores; ``data`` is omitted from the Parquet file
# when a test needs a column the file physically lacks.
_IDS = [1, 2, 3]
_DATA = ["hello", "world", "iceberg"]
_REAL_DATA_RESULT = "1\thello\n2\tworld\n3\ticeberg"
_DATA_NULL_RESULT = "1\t\\N\n2\t\\N\n3\t\\N"


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


def _data_file_fields(stats_fields):
    """data_file record fields, optionally followed by the given stats arrays."""
    fields = [
        {"name": "content",            "type": "int"},
        {"name": "file_path",          "type": "string"},
        {"name": "file_format",        "type": "string"},
        {"name": "partition", "type": {"type": "record", "name": "r102", "fields": []}},
        {"name": "record_count",       "type": "long"},
        {"name": "file_size_in_bytes", "type": "long"},
    ]
    for i, name in enumerate(stats_fields):
        fields.append({
            "name": name,
            "type": {
                "type": "array",
                "items": {
                    "type": "record",
                    "name": f"k81v81_{i}",
                    "fields": [
                        {"name": "key",   "type": "int"},
                        {"name": "value", "type": "long"},
                    ],
                },
            },
        })
    return fields


def _manifest_entry_schema(stats_fields):
    """Iceberg v2 manifest entry Avro schema carrying the given stats arrays."""
    return json.dumps({
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
                    "fields": _data_file_fields(stats_fields),
                },
            },
        ],
    })


# Stats-less manifest: omits every optional stats field -> columns_info is empty.
_MANIFEST_ENTRY_NO_STATS_SCHEMA_STR = _manifest_entry_schema([])
# Partial stats: value_counts only (column_sizes and null_value_counts absent);
# enough to populate columns_info so the empty-stats guard is not triggered.
_MANIFEST_ENTRY_PARTIAL_STATS_SCHEMA_STR = _manifest_entry_schema(["value_counts"])
# Full stats: the common Spark-written shape.
_MANIFEST_ENTRY_FULL_STATS_SCHEMA_STR = _manifest_entry_schema(
    ["column_sizes", "value_counts", "null_value_counts"]
)


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
    manifest_entry_schema_str,
    file_has_data_column=True,
    data_file_stats=None,
    schema_evolution=False,
):
    """
    Build a minimal Iceberg v2 table under ``tmpdir/table_name``.

    container_base is the absolute path inside the ClickHouse container where the
    table is placed after upload; file paths embedded in the Avro records and
    metadata.json must reference it because they are interpreted at query time
    inside the container.

    file_has_data_column controls whether the Parquet file physically contains
    the ``data`` column.  data_file_stats, when given, supplies the manifest
    entry's stats arrays (e.g. ``value_counts``).  schema_evolution lays the
    table out with two schemas (0: ``id`` only, 1: ``id``+``data``) and two
    snapshots so the data file resolves to the older schema id, exercising the
    Parquet-reader fall-through that synthesizes the absent ``data`` column.

    Returns the local table directory path.
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

    # The manifest entry's snapshot_id is the snapshot that wrote the data file.
    # Under schema evolution that snapshot (id 1) carries the older schema (0),
    # so the iterator resolves the file to schema 0 while reading the table at
    # schema 1 -- the condition that makes getInitialSchemaByPath return the
    # file's own schema as initial_header.
    entry_snapshot_id = 1
    write_schema_id   = 0  # schema the data file was written with

    data_file_record = {
        "content":            0,            # DATA
        "file_path":          data_container_path,
        "file_format":        "PARQUET",
        "partition":          {},
        "record_count":       len(_IDS),
        "file_size_in_bytes": data_size,
    }
    if data_file_stats:
        data_file_record.update(data_file_stats)

    manifest_local_path     = os.path.join(meta_local, "00000-0-manifest.avro")
    manifest_container_path = f"{container_base}/metadata/00000-0-manifest.avro"
    _write_avro(
        manifest_entry_schema_str,
        [{
            "status":               1,             # ADDED
            "snapshot_id":          entry_snapshot_id,
            "sequence_number":      1,
            "file_sequence_number": 1,
            "data_file":            data_file_record,
        }],
        manifest_local_path,
        metadata={
            # The manifest's own schema is the data file's write schema.
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
            _MANIFEST_LIST_SCHEMA_STR,
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
        # Two schemas, two snapshots: the data file was written under schema 0
        # (id only); the table later evolved to schema 1 (id + data).
        schemas          = [_iceberg_schema(0, with_data=False), _iceberg_schema(1, with_data=True)]
        current_schema   = 1
        last_column_id   = 2
        snapshots = [
            {"snapshot-id": 1, "schema-id": 0, "manifest-list": _write_manifest_list(1)},
            {"snapshot-id": 2, "schema-id": 1, "manifest-list": _write_manifest_list(2)},
        ]
        current_snapshot = 2
        last_seq         = 2
    else:
        # Single schema (id + data); the data file resolves to it directly.
        schemas          = [_iceberg_schema(0, with_data=True)]
        current_schema   = 0
        last_column_id   = 2
        snapshots = [
            {"snapshot-id": 1, "schema-id": 0, "manifest-list": _write_manifest_list(1)},
        ]
        current_snapshot = 1
        last_seq         = 1

    metadata = {
        "format-version":       2,
        "table-uuid":           str(uuid.uuid4()),
        "location":             container_base,
        "last-sequence-number": last_seq,
        "last-updated-ms":      ts_ms,
        "last-column-id":       last_column_id,
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
    """
    Discriminating reproducer for Altinity#1545.

    A stats-less manifest (empty columns_info) must read real data, not all-NULL
    rows.  Unpatched build: every nullable column is injected as a constant NULL
    and the result is 3 all-NULL rows.  Fixed build: the empty-stats guard falls
    through to the Parquet reader and returns the real values.
    """
    instance       = started_cluster_iceberg_no_spark.instances["node1"]
    table_name     = "test_stats_less_real_" + get_uuid_str()
    container_base = _container_base(table_name)

    with tempfile.TemporaryDirectory() as tmpdir:
        local_dir = _create_iceberg_table(
            tmpdir, table_name, container_base,
            manifest_entry_schema_str=_MANIFEST_ENTRY_NO_STATS_SCHEMA_STR,
        )
        _upload_to_node(instance, local_dir, container_base)

    result = _query_local(instance, container_base)
    assert result == _REAL_DATA_RESULT, (
        f"Got: {result!r}\n"
        "All-NULL rows mean the bug fired: empty columns_info caused the "
        "optimization to treat every nullable column as absent and inject "
        "constant NULL (need_only_count=1)."
    )


def test_stats_less_manifest_schema_evolution_absent_column(started_cluster_iceberg_no_spark):
    """
    Discriminating test for the schema-evolution fall-through (highest risk).

    The Iceberg schema declares ``id``+``data`` but the Parquet file was written
    under the older schema and physically contains only ``id``.  With the
    stats-less manifest, the fixed build must fall through to the Parquet reader,
    read ``id`` for real, and synthesize the absent ``data`` column as NULL via
    IcebergMetadata::getInitialSchemaByPath (file schema as initial_header).

    Unpatched build: the optimization marks BOTH columns (including ``id``)
    constant NULL, so ``id`` comes back NULL too -- the discriminator.
    """
    instance       = started_cluster_iceberg_no_spark.instances["node1"]
    table_name     = "test_stats_less_evolve_" + get_uuid_str()
    container_base = _container_base(table_name)

    with tempfile.TemporaryDirectory() as tmpdir:
        local_dir = _create_iceberg_table(
            tmpdir, table_name, container_base,
            manifest_entry_schema_str=_MANIFEST_ENTRY_NO_STATS_SCHEMA_STR,
            file_has_data_column=False,
            schema_evolution=True,
        )
        _upload_to_node(instance, local_dir, container_base)

    result = _query_local(instance, container_base)
    assert result == _DATA_NULL_RESULT, (
        f"Got: {result!r}\n"
        "Expected real 'id' with NULL 'data'.  If 'id' is also NULL the bug "
        "fired; if the query errors, the Parquet fall-through did not synthesize "
        "the schema-evolved-absent column as NULL."
    )


def test_stats_less_manifest_cluster_returns_real_data(started_cluster_iceberg_no_spark):
    """
    Cluster/serialization-path coverage for Altinity#1545.

    icebergLocalCluster distributes file-level tasks to workers, serializing
    DataFileMetaInfo (and its empty columns_info) through the cluster JSON path.
    An empty columns_info must survive the round-trip and still read real data on
    the worker, exercising the same fix on the *Cluster variants.  The table is
    uploaded to every node because local storage is read by each worker from its
    own filesystem.
    """
    cluster        = started_cluster_iceberg_no_spark
    instance       = cluster.instances["node1"]
    table_name     = "test_stats_less_cluster_" + get_uuid_str()
    container_base = _container_base(table_name)

    with tempfile.TemporaryDirectory() as tmpdir:
        local_dir = _create_iceberg_table(
            tmpdir, table_name, container_base,
            manifest_entry_schema_str=_MANIFEST_ENTRY_NO_STATS_SCHEMA_STR,
        )
        for node in cluster.instances.values():
            _upload_to_node(node, local_dir, container_base)

    result = instance.query(
        f"SELECT * FROM icebergLocalCluster('cluster_simple', local, path='{container_base}', format=Parquet)"
        " ORDER BY id"
        " SETTINGS allow_experimental_iceberg_read_optimization=1"
    ).strip()
    assert result == _REAL_DATA_RESULT, (
        f"Got: {result!r}\n"
        "All-NULL rows mean empty columns_info was mishandled on the worker after "
        "the cluster JSON round-trip."
    )


def test_non_empty_stats_absent_column_still_null(started_cluster_iceberg_no_spark):
    """
    Non-regression guard: the optimization's legitimate absent-NULL path.

    The Iceberg schema declares ``id``+``data`` but the Parquet file contains
    only ``id``, and the manifest carries stats for ``id`` only (non-empty
    columns_info).  The optimization must keep marking the genuinely-absent
    ``data`` column NULL while ``id`` reads real values.  Passes on both builds;
    the fix only changes the empty-columns_info case.
    """
    instance       = started_cluster_iceberg_no_spark.instances["node1"]
    table_name     = "test_nonempty_absent_" + get_uuid_str()
    container_base = _container_base(table_name)

    # value_counts for field_id 1 (id) only -> columns_info = {id}.
    stats = {"value_counts": [{"key": 1, "value": len(_IDS)}]}

    with tempfile.TemporaryDirectory() as tmpdir:
        local_dir = _create_iceberg_table(
            tmpdir, table_name, container_base,
            manifest_entry_schema_str=_MANIFEST_ENTRY_PARTIAL_STATS_SCHEMA_STR,
            file_has_data_column=False,
            data_file_stats=stats,
        )
        _upload_to_node(instance, local_dir, container_base)

    result = _query_local(instance, container_base)
    assert result == _DATA_NULL_RESULT, (
        f"Got: {result!r}\n"
        "With non-empty stats the optimization must still read 'id' and inject "
        "the absent 'data' column as NULL."
    )


@pytest.mark.parametrize(
    "manifest_entry_schema_str,stats",
    [
        (
            _MANIFEST_ENTRY_PARTIAL_STATS_SCHEMA_STR,
            {"value_counts": [{"key": 1, "value": 3}, {"key": 2, "value": 3}]},
        ),
        (
            _MANIFEST_ENTRY_FULL_STATS_SCHEMA_STR,
            {
                "column_sizes":      [{"key": 1, "value": 64}, {"key": 2, "value": 128}],
                "value_counts":      [{"key": 1, "value": 3}, {"key": 2, "value": 3}],
                "null_value_counts": [{"key": 1, "value": 0}, {"key": 2, "value": 0}],
            },
        ),
    ],
    ids=["partial_stats", "full_stats"],
)
def test_non_empty_stats_returns_real_data(
    started_cluster_iceberg_no_spark, manifest_entry_schema_str, stats
):
    """
    Non-regression guard: manifests that do populate columns_info (partial
    value_counts-only, and full Spark-style stats) must keep returning real data
    when the optimization is enabled.  These are the cases the empty-stats guard
    must not disturb.
    """
    instance       = started_cluster_iceberg_no_spark.instances["node1"]
    table_name     = "test_nonempty_real_" + get_uuid_str()
    container_base = _container_base(table_name)

    with tempfile.TemporaryDirectory() as tmpdir:
        local_dir = _create_iceberg_table(
            tmpdir, table_name, container_base,
            manifest_entry_schema_str=manifest_entry_schema_str,
            data_file_stats=stats,
        )
        _upload_to_node(instance, local_dir, container_base)

    result = _query_local(instance, container_base)
    assert result == _REAL_DATA_RESULT, (
        f"Got: {result!r}\n"
        "A manifest with non-empty stats must return real data when the "
        "optimization is enabled."
    )
