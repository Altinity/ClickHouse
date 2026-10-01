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

from helpers.iceberg_utils import (
    create_iceberg_table,
    get_creation_expression,
    get_uuid_str,
)


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


def _patch_table_with_unknown_column(instance, table_name):
    """Read the latest metadata, add an unknown-typed column via schema
    evolution (v2 -> v3), and write the new metadata version.  Returns the
    name of the new column."""
    meta, prev_path = _read_latest_metadata(instance, table_name)
    meta["format-version"] = 3

    current_schema_id = meta.get("current-schema-id", 0)
    current_schema = None
    for schema in meta.get("schemas", []):
        if schema.get("schema-id", 0) == current_schema_id:
            current_schema = schema
            break
    assert current_schema is not None

    last_column_id = meta.get("last-column-id", 0)
    new_field_id = last_column_id + 1
    new_schema_id = max(s.get("schema-id", 0) for s in meta.get("schemas", [])) + 1

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

    _write_next_metadata(instance, table_name, meta, prev_path)


def test_unknown_type_write(started_cluster_iceberg_no_spark):
    """Verify that inserting into a table with an unknown-typed column
    succeeds: the unknown column is silently dropped from the data file
    (it has no physical representation) but still reads back as NULLs."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_unknown_type_write_" + get_uuid_str()

    # 1. Create a v2 table, insert initial data, and patch to v3 with unknown column.
    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(id Int64, name Nullable(String))",
        format_version=2,
    )
    instance.query(
        f"INSERT INTO {table_name} VALUES (1, 'alice'), (2, 'bob')"
    )
    _patch_table_with_unknown_column(instance, table_name)

    # 2. Re-create the ClickHouse table to pick up the new schema.
    instance.query(f"DROP TABLE IF EXISTS {table_name}")
    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        settings={"allow_insert_into_iceberg": 1},
    )

    # 3. Insert new rows -- this must NOT fail with UNKNOWN_TYPE.
    instance.query(
        f"INSERT INTO {table_name} VALUES (3, 'charlie', NULL), (4, 'dave', NULL)",
        settings={"allow_insert_into_iceberg": 1},
    )

    # 4. Verify all rows are readable.
    result = instance.query(
        f"SELECT id, name, placeholder FROM {table_name} ORDER BY id"
    ).strip()
    expected = (
        "1\talice\t\\N\n"
        "2\tbob\t\\N\n"
        "3\tcharlie\t\\N\n"
        "4\tdave\t\\N"
    )
    assert result == expected

    # 5. Verify the unknown column is still Nullable(Nothing) after the write.
    describe = instance.query(f"DESCRIBE TABLE {table_name}")
    lines = [line.split("\t") for line in describe.strip().split("\n")]
    placeholder_row = [row for row in lines if row[0] == "placeholder"]
    assert len(placeholder_row) == 1
    assert placeholder_row[0][1] == "Nullable(Nothing)"


def _patch_table_with_v3_field(instance, table_name, build_field):
    """Bump the table to format-version 3 and append the top-level field returned by
    `build_field(last_column_id)`, which returns `(field, new_last_column_id)`."""
    meta, prev_path = _read_latest_metadata(instance, table_name)
    meta["format-version"] = 3

    current_schema_id = meta.get("current-schema-id", 0)
    current_schema = None
    for schema in meta.get("schemas", []):
        if schema.get("schema-id", 0) == current_schema_id:
            current_schema = schema
            break
    assert current_schema is not None

    field, new_last_column_id = build_field(meta.get("last-column-id", 0))
    new_schema_id = max(s.get("schema-id", 0) for s in meta.get("schemas", [])) + 1

    new_schema = {
        "type": "struct",
        "schema-id": new_schema_id,
        "fields": current_schema["fields"] + [field],
    }
    meta.setdefault("schemas", []).append(new_schema)
    meta["current-schema-id"] = new_schema_id
    meta["last-column-id"] = new_last_column_id

    _write_next_metadata(instance, table_name, meta, prev_path)


def _patch_table_with_nested_unknown_column(instance, table_name):
    """Like `_patch_table_with_unknown_column`, but adds a struct column whose
    subfield has the unknown type, so Nothing appears nested inside a Tuple."""

    def build_field(last_column_id):
        field = {
            "id": last_column_id + 1,
            "name": "nested",
            "required": False,
            "type": {
                "type": "struct",
                "fields": [
                    {
                        "id": last_column_id + 2,
                        "name": "a",
                        "required": False,
                        "type": "long",
                    },
                    {
                        "id": last_column_id + 3,
                        "name": "u",
                        "required": False,
                        "type": "unknown",
                    },
                ],
            },
        }
        return field, last_column_id + 3

    _patch_table_with_v3_field(instance, table_name, build_field)


def _patch_table_with_list_unknown_column(instance, table_name):
    """Adds a `list<unknown>` column, whose only content is its list lengths."""

    def build_field(last_column_id):
        field = {
            "id": last_column_id + 1,
            "name": "tags",
            "required": False,
            "type": {
                "type": "list",
                "element-id": last_column_id + 2,
                "element": "unknown",
                "element-required": False,
            },
        }
        return field, last_column_id + 2

    _patch_table_with_v3_field(instance, table_name, build_field)


def _recreate_table(instance, table_name, cluster):
    instance.query(f"DROP TABLE IF EXISTS {table_name}")
    create_iceberg_table(
        "local",
        instance,
        table_name,
        cluster,
        settings={"allow_insert_into_iceberg": 1},
    )


def test_unknown_type_nested_write(started_cluster_iceberg_no_spark):
    """Verify that inserting into a table whose struct column contains an
    unknown-typed subfield succeeds: Nothing nested inside a Tuple must not
    reach the Parquet writer, which cannot represent it (`UNKNOWN_TYPE`)."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_unknown_type_nested_write_" + get_uuid_str()

    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(id Int64, name Nullable(String))",
        format_version=2,
    )
    instance.query(
        f"INSERT INTO {table_name} VALUES (1, 'alice'), (2, 'bob')"
    )
    _patch_table_with_nested_unknown_column(instance, table_name)
    _recreate_table(instance, table_name, started_cluster_iceberg_no_spark)

    describe = instance.query(f"DESCRIBE TABLE {table_name}")
    lines = [line.split("\t") for line in describe.strip().split("\n")]
    nested_row = [row for row in lines if row[0] == "nested"]
    assert len(nested_row) == 1
    assert "Nothing" in nested_row[0][1], nested_row

    # This must NOT fail with UNKNOWN_TYPE, and must keep the known subfield `a`:
    # only the unknown leaf `u` is left out of the data file.
    instance.query(
        f"INSERT INTO {table_name} (id, name, nested) VALUES (3, 'charlie', (42, NULL)), (4, 'dave', (NULL, NULL))",
        settings={"allow_insert_into_iceberg": 1},
    )

    result = instance.query(
        f"SELECT id, name, nested.a, nested.u FROM {table_name} ORDER BY id"
    ).strip()
    expected = (
        "1\talice\t\\N\t\\N\n"
        "2\tbob\t\\N\t\\N\n"
        "3\tcharlie\t42\t\\N\n"
        "4\tdave\t\\N\t\\N"
    )
    assert result == expected


def test_unknown_type_list_write(started_cluster_iceberg_no_spark):
    """A `list<unknown>` column has no serialisable leaf, so it is left out of the
    data file. That only loses nothing when every list is empty: a non-empty list
    such as [NULL] must be rejected instead of silently read back as []."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_unknown_type_list_write_" + get_uuid_str()

    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(id Int64, name Nullable(String))",
        format_version=2,
    )
    instance.query(
        f"INSERT INTO {table_name} VALUES (1, 'alice'), (2, 'bob')"
    )
    _patch_table_with_list_unknown_column(instance, table_name)
    _recreate_table(instance, table_name, started_cluster_iceberg_no_spark)

    instance.query(
        f"INSERT INTO {table_name} (id, name, tags) VALUES (3, 'charlie', [])",
        settings={"allow_insert_into_iceberg": 1},
    )

    error = instance.query_and_get_error(
        f"INSERT INTO {table_name} (id, name, tags) VALUES (4, 'dave', [NULL])",
        settings={"allow_insert_into_iceberg": 1},
    )
    assert "NOT_IMPLEMENTED" in error, error
    assert "Iceberg `unknown` element" in error, error

    result = instance.query(
        f"SELECT id, name, tags FROM {table_name} ORDER BY id"
    ).strip()
    expected = (
        "1\talice\t[]\n"
        "2\tbob\t[]\n"
        "3\tcharlie\t[]"
    )
    assert result == expected


def test_unknown_type_rejected_on_v2(started_cluster_iceberg_no_spark):
    """`unknown` exists only from Iceberg format version 3, so ClickHouse must not
    write it into the metadata of a table with an older format version."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]

    table_name = "test_unknown_type_create_v2_" + get_uuid_str()
    error = instance.query_and_get_error(
        get_creation_expression(
            "local",
            table_name,
            started_cluster_iceberg_no_spark,
            "(id Int64, u Nullable(Nothing))",
            2,
        )
    )
    assert "BAD_ARGUMENTS" in error, error
    assert "requires format version 3" in error, error

    table_name = "test_unknown_type_add_column_v2_" + get_uuid_str()
    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(id Int64, name Nullable(String))",
        format_version=2,
    )
    error = instance.query_and_get_error(
        f"ALTER TABLE {table_name} ADD COLUMN u Nullable(Nothing)",
        settings={"allow_insert_into_iceberg": 1},
    )
    assert "BAD_ARGUMENTS" in error, error
    assert "requires format version 3" in error, error
    describe = instance.query(f"DESCRIBE TABLE {table_name}")
    assert [line.split("\t")[0] for line in describe.strip().split("\n")] == ["id", "name"]

    table_name = "test_unknown_type_create_v3_" + get_uuid_str()
    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(id Int64, u Nullable(Nothing))",
        format_version=3,
    )
    describe = instance.query(f"DESCRIBE TABLE {table_name}")
    u_row = [line.split("\t") for line in describe.strip().split("\n") if line.startswith("u\t")]
    assert len(u_row) == 1 and u_row[0][1] == "Nullable(Nothing)", describe


def test_unknown_type_write_keeps_bounds(started_cluster_iceberg_no_spark):
    """A top-level `unknown` column is left out of the data file, so it must not
    contribute statistics: before the fix its null bound made the writer drop
    lower/upper bounds for every column, and `column_sizes` described a column
    that was never written."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_unknown_type_write_keeps_bounds_" + get_uuid_str()

    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(id Int64, name Nullable(String))",
        format_version=2,
    )
    instance.query(f"INSERT INTO {table_name} VALUES (1, 'alice'), (2, 'bob')")
    _patch_table_with_unknown_column(instance, table_name)
    _recreate_table(instance, table_name, started_cluster_iceberg_no_spark)

    meta, _ = _read_latest_metadata(instance, table_name)
    current_schema = [
        s for s in meta["schemas"] if s["schema-id"] == meta["current-schema-id"]
    ][0]
    placeholder_id = [
        f["id"] for f in current_schema["fields"] if f["name"] == "placeholder"
    ][0]

    # Two inserts, so each new row range lands in its own data file.
    for values in ["(3, 'c', NULL), (4, 'd', NULL)", "(10, 'x', NULL), (11, 'y', NULL)"]:
        instance.query(
            f"INSERT INTO {table_name} VALUES {values}",
            settings={"allow_insert_into_iceberg": 1},
        )

    files = instance.query(
        f"SELECT count(), countIf(mapContains(column_sizes, {placeholder_id})) "
        f"FROM system.iceberg_files "
        f"WHERE database = currentDatabase() AND table = '{table_name}' AND content = 'DATA'"
    ).strip()
    assert files == "3\t0", files

    # With bounds on `id` in both new files, min/max pruning skips the files of
    # rows 1-2 and 3-4. Without them only the file written before the patch is skipped.
    query_id = f"{table_name}_{get_uuid_str()}"
    result = instance.query(
        f"SELECT id, name FROM {table_name} WHERE id = 10",
        query_id=query_id,
        settings={
            "use_iceberg_partition_pruning": 1,
            "input_format_parquet_bloom_filter_push_down": 0,
            "input_format_parquet_filter_push_down": 0,
        },
    ).strip()
    assert result == "10\tx"

    instance.query("SYSTEM FLUSH LOGS")
    pruned = int(
        instance.query(
            f"SELECT ProfileEvents['IcebergMinMaxIndexPrunedFiles'] FROM system.query_log "
            f"WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
        )
    )
    assert pruned == 2, pruned


def test_unknown_type_only_column_rejected(started_cluster_iceberg_no_spark):
    """When every column has only the `unknown` type, nothing can be written: the
    insert must fail instead of writing a zero-column Parquet file that later
    makes every SELECT fail."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_unknown_type_only_column_" + get_uuid_str()

    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(u Nullable(Nothing))",
        format_version=3,
    )

    error = instance.query_and_get_error(
        f"INSERT INTO {table_name} VALUES (NULL), (NULL)",
        settings={"allow_insert_into_iceberg": 1},
    )
    assert "NOT_IMPLEMENTED" in error, error
    assert "every column contains only the Iceberg `unknown` type" in error, error

    assert instance.query(f"SELECT count() FROM {table_name}").strip() == "0"
