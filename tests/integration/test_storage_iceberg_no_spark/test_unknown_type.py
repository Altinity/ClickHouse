#!/usr/bin/env python3

"""
Integration test for Iceberg v3 `unknown` primitive type.

Creates an Iceberg v2 table via ClickHouse, then patches the metadata JSON
to v3 and injects an `unknown`-typed field.  Verifies that ClickHouse reads
the table correctly:
- The unknown column is mapped to Nullable(Nothing)
- All values in the unknown column are NULL
- Non-unknown columns read correctly
"""

import json
import re

from helpers.iceberg_utils import create_iceberg_table, get_uuid_str


def _metadata_dir(table_name):
    return (
        f"/var/lib/clickhouse/user_files/iceberg_data/default/{table_name}/metadata"
    )


def _read_latest_metadata(instance, table_name):
    metadata_dir = _metadata_dir(table_name)
    latest = instance.exec_in_container(
        ["bash", "-c", f"ls -v {metadata_dir}/v*.metadata.json | tail -1"]
    ).strip()
    raw = instance.exec_in_container(["cat", latest])
    return json.loads(raw), latest


def _write_next_metadata(instance, table_name, meta, prev_path):
    metadata_dir = _metadata_dir(table_name)
    version_match = re.search(r"/v(\d+)[^/]*\.metadata\.json$", prev_path)
    new_version = int(version_match.group(1)) + 1
    new_path = f"{metadata_dir}/v{new_version}.metadata.json"
    new_content = json.dumps(meta, indent=4)
    instance.exec_in_container(
        ["bash", "-c", f"cat > {new_path} << 'JSONEOF'\n{new_content}\nJSONEOF"]
    )


def test_unknown_type_read(started_cluster_iceberg_no_spark):
    """Create a v2 Iceberg table, patch metadata to v3 with an unknown column,
    and verify ClickHouse reads it correctly."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_unknown_type_" + get_uuid_str()

    # 1. Create a normal v2 table with two columns.
    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(id Int64, name Nullable(String))",
        format_version=2,
    )
    instance.query(
        f"INSERT INTO {table_name} VALUES (1, 'alice'), (2, 'bob'), (3, 'charlie')"
    )

    # 2. Read the latest metadata JSON.
    meta, prev_path = _read_latest_metadata(instance, table_name)

    # 3. Patch: bump format-version to 3 and add an unknown-typed field
    #    via proper schema evolution (new schema-id, not in-place mutation).
    meta["format-version"] = 3

    # Find the current schema to copy its fields.
    current_schema_id = meta.get("current-schema-id", 0)
    current_schema = None
    for schema in meta.get("schemas", []):
        if schema.get("schema-id", 0) == current_schema_id:
            current_schema = schema
            break
    assert current_schema is not None, "current schema not found in metadata"

    # Determine the next field id and next schema id.
    last_column_id = meta.get("last-column-id", 0)
    new_field_id = last_column_id + 1
    new_schema_id = max(s.get("schema-id", 0) for s in meta.get("schemas", [])) + 1

    # Create a new schema that includes the unknown field.
    new_schema = {
        "type": "struct",
        "schema-id": new_schema_id,
        "fields": current_schema["fields"]
        + [
            {
                "id": new_field_id,
                "name": "placeholder",
                "required": False,
                "type": "unknown",
            }
        ],
    }
    meta.setdefault("schemas", []).append(new_schema)
    meta["current-schema-id"] = new_schema_id
    meta["last-column-id"] = new_field_id

    # 4. Write the patched metadata as the next version.
    _write_next_metadata(instance, table_name, meta, prev_path)

    # 5. Drop and recreate the ClickHouse table so it re-reads metadata.
    instance.query(f"DROP TABLE IF EXISTS {table_name}")
    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
    )

    # 6. Verify row count.
    assert instance.query(f"SELECT count() FROM {table_name}").strip() == "3"

    # 7. Verify the unknown column type is Nullable(Nothing).
    describe = instance.query(f"DESCRIBE TABLE {table_name}")
    lines = [line.split("\t") for line in describe.strip().split("\n")]
    placeholder_row = [row for row in lines if row[0] == "placeholder"]
    assert len(placeholder_row) == 1, (
        f"Expected one 'placeholder' column, got: {lines}"
    )
    assert placeholder_row[0][1] == "Nullable(Nothing)"

    # 8. Verify all values in the unknown column are NULL.
    result = instance.query(f"SELECT placeholder FROM {table_name}").strip()
    assert result == "\\N\n\\N\n\\N"

    # 9. Verify non-unknown columns read correctly alongside the unknown column.
    result = instance.query(
        f"SELECT id, name, placeholder FROM {table_name} ORDER BY id"
    ).strip()
    expected = "1\talice\t\\N\n2\tbob\t\\N\n3\tcharlie\t\\N"
    assert result == expected
