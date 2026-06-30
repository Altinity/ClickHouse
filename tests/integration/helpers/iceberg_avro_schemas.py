import json

# Iceberg v2 manifest list schema (only the fields ClickHouse reads).
MANIFEST_LIST_SCHEMA_STR = json.dumps({
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

# Stats-less manifest entry: no optional stats fields -> empty columns_info.
MANIFEST_ENTRY_NO_STATS_SCHEMA_STR = json.dumps({
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
                    {"name": "partition", "type": {"type": "record", "name": "r102", "fields": []}},
                    {"name": "record_count",       "type": "long"},
                    {"name": "file_size_in_bytes", "type": "long"},
                ],
            },
        },
    ],
})
