#!/usr/bin/env bash
# Tags: no-fasttest
# Tag no-fasttest: needs pyarrow to write the Parquet data file.
#
# Reproduces https://github.com/Altinity/ClickHouse/issues/2525
# Iceberg column metrics are optional per column. A manifest that has statistics for
# some columns and none for others (Databricks UniForm keeps metrics for the first 32
# columns only) must still be read from the data file. With
# `allow_experimental_iceberg_read_optimization` enabled, a nullable column missing
# from those maps was treated as absent and replaced with constant NULL.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

TABLE_PATH="${CLICKHOUSE_USER_FILES}/lakehouses/${CLICKHOUSE_DATABASE}_partial_column_stats"
rm -rf "${TABLE_PATH}"

python3 - "${TABLE_PATH}" <<'PY'
import json
import os
import sys
import time
import uuid

import pyarrow as pa
import pyarrow.parquet as pq


def zigzag(n, bits):
    return (n << 1) ^ (n >> (bits - 1))


def encode_varint(n):
    out = bytearray()
    while True:
        byte = n & 0x7F
        n >>= 7
        if n:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def encode_long(n):
    return encode_varint(zigzag(n, 64))


def encode_int(n):
    return encode_varint(zigzag(n, 32))


def encode_bytes(data):
    return encode_long(len(data)) + data


def encode_string(text):
    return encode_bytes(text.encode("utf-8"))


def encode_array(items):
    if not items:
        return encode_long(0)
    return encode_long(len(items)) + b"".join(items) + encode_long(0)


def encode_map(pairs):
    body = b"".join(encode_string(key) + encode_bytes(value) for key, value in pairs)
    return encode_long(len(pairs)) + body + encode_long(0)


def encode_union_long(value):
    # ["null", "long"], branch 1 is the long.
    return encode_int(1) + encode_long(value)


def write_avro(path, schema, records, metadata):
    sync = os.urandom(16)
    header = [("avro.schema", json.dumps(schema).encode("utf-8")), ("avro.codec", b"null")]
    header.extend((key, value if isinstance(value, bytes) else value.encode("utf-8")) for key, value in metadata.items())
    payload = b"".join(records)
    block = encode_long(len(records)) + encode_long(len(payload)) + payload + sync
    with open(path, "wb") as fh:
        fh.write(b"Obj\x01" + encode_map(header) + sync + block)


def kv_array(name):
    return {
        "type": "array",
        "items": {
            "type": "record",
            "name": name,
            "fields": [{"name": "key", "type": "int"}, {"name": "value", "type": "long"}],
        },
    }


MANIFEST_SCHEMA = {
    "type": "record",
    "name": "manifest_entry",
    "fields": [
        {"name": "status", "type": "int"},
        {"name": "snapshot_id", "type": ["null", "long"]},
        {"name": "sequence_number", "type": ["null", "long"]},
        {"name": "file_sequence_number", "type": ["null", "long"]},
        {
            "name": "data_file",
            "type": {
                "type": "record",
                "name": "r2",
                "fields": [
                    {"name": "content", "type": "int"},
                    {"name": "file_path", "type": "string"},
                    {"name": "file_format", "type": "string"},
                    {"name": "partition", "type": {"type": "record", "name": "r102", "fields": []}},
                    {"name": "record_count", "type": "long"},
                    {"name": "file_size_in_bytes", "type": "long"},
                    {"name": "column_sizes", "type": kv_array("k_cs")},
                    {"name": "value_counts", "type": kv_array("k_vc")},
                    {"name": "null_value_counts", "type": kv_array("k_nc")},
                ],
            },
        },
    ],
}

MANIFEST_LIST_SCHEMA = {
    "type": "record",
    "name": "manifest_file",
    "fields": [
        {"name": "manifest_path", "type": "string"},
        {"name": "manifest_length", "type": "long"},
        {"name": "partition_spec_id", "type": "int"},
        {"name": "content", "type": "int"},
        {"name": "sequence_number", "type": "long"},
        {"name": "min_sequence_number", "type": "long"},
        {"name": "added_snapshot_id", "type": "long"},
        {"name": "added_files_count", "type": "int"},
        {"name": "existing_files_count", "type": "int"},
        {"name": "deleted_files_count", "type": "int"},
        {"name": "added_rows_count", "type": "long"},
        {"name": "existing_rows_count", "type": "long"},
        {"name": "deleted_rows_count", "type": "long"},
    ],
}

root = sys.argv[1]
data_dir = os.path.join(root, "data")
meta_dir = os.path.join(root, "metadata")
os.makedirs(data_dir)
os.makedirs(meta_dir)

data_path = os.path.join(data_dir, "00000-0-data.parquet")
arrow_schema = pa.schema([
    pa.field("id", pa.int32(), nullable=True, metadata={b"PARQUET:field_id": b"1"}),
    pa.field("extra", pa.string(), nullable=True, metadata={b"PARQUET:field_id": b"2"}),
])
pq.write_table(
    pa.table(
        {
            "id": pa.array([1, 2, 3], type=pa.int32()),
            "extra": pa.array(["alpha", "beta", "gamma"], type=pa.string()),
        },
        schema=arrow_schema,
    ),
    data_path,
)

iceberg_schema = {
    "type": "struct",
    "schema-id": 0,
    "fields": [
        {"id": 1, "name": "id", "required": False, "type": "int"},
        {"id": 2, "name": "extra", "required": False, "type": "string"},
    ],
}

# Stats exist, but only for field id 1 (`id`). `extra` is in the file and in the
# table schema, and has no manifest metrics. Empty maps still count as "some stats".
entry = b"".join([
    encode_int(1),
    encode_union_long(1),
    encode_union_long(1),
    encode_union_long(1),
    encode_int(0),
    encode_string(data_path),
    encode_string("PARQUET"),
    b"",
    encode_long(3),
    encode_long(os.path.getsize(data_path)),
    encode_array([]),
    encode_array([]),
    encode_array([encode_int(1) + encode_long(0)]),
])

manifest_path = os.path.join(meta_dir, "00000-0-manifest.avro")
write_avro(
    manifest_path,
    MANIFEST_SCHEMA,
    [entry],
    {"schema": json.dumps(iceberg_schema), "partition-spec": "[]", "format-version": "2"},
)

mlist_path = os.path.join(meta_dir, "snap-1-0-manifest-list.avro")
mlist_entry = b"".join([
    encode_string(manifest_path),
    encode_long(os.path.getsize(manifest_path)),
    encode_int(0),
    encode_int(0),
    encode_long(1),
    encode_long(1),
    encode_long(1),
    encode_int(1),
    encode_int(0),
    encode_int(0),
    encode_long(3),
    encode_long(0),
    encode_long(0),
])
write_avro(mlist_path, MANIFEST_LIST_SCHEMA, [mlist_entry], {"format-version": "2"})

now_ms = int(time.time() * 1000)
metadata = {
    "format-version": 2,
    "table-uuid": str(uuid.uuid4()),
    "location": root,
    "last-sequence-number": 1,
    "last-updated-ms": now_ms,
    "last-column-id": 2,
    "current-schema-id": 0,
    "schemas": [iceberg_schema],
    "default-spec-id": 0,
    "partition-specs": [{"spec-id": 0, "fields": []}],
    "last-partition-id": 999,
    "default-sort-order-id": 0,
    "sort-orders": [{"order-id": 0, "fields": []}],
    "properties": {},
    "current-snapshot-id": 1,
    "snapshots": [{
        "snapshot-id": 1,
        "sequence-number": 1,
        "timestamp-ms": now_ms,
        "manifest-list": mlist_path,
        "summary": {"operation": "append", "added-records": "3", "total-records": "3"},
        "schema-id": 0,
    }],
    "snapshot-log": [{"timestamp-ms": now_ms, "snapshot-id": 1}],
    "metadata-log": [],
    "refs": {"main": {"snapshot-id": 1, "type": "branch"}},
}
with open(os.path.join(meta_dir, "v1.metadata.json"), "w") as fh:
    json.dump(metadata, fh)
with open(os.path.join(meta_dir, "version-hint.text"), "w") as fh:
    fh.write("1\n")
PY

${CLICKHOUSE_CLIENT} --query "
    SELECT id, extra
    FROM icebergLocal('${TABLE_PATH}')
    ORDER BY id
    SETTINGS allow_experimental_iceberg_read_optimization = 1
"

${CLICKHOUSE_CLIENT} --query "
    SELECT id, extra
    FROM icebergLocal('${TABLE_PATH}')
    ORDER BY id
    SETTINGS allow_experimental_iceberg_read_optimization = 0
"

rm -rf "${TABLE_PATH}"
