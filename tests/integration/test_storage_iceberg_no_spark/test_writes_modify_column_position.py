import pytest

from helpers.iceberg_utils import (
    create_iceberg_table,
    get_uuid_str,
)

INSERT_SETTINGS = {"allow_insert_into_iceberg": 1}


@pytest.mark.parametrize("format_version", [1, 2])
@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_modify_column_first(started_cluster_iceberg_no_spark, format_version, storage_type):
    """MODIFY COLUMN ... FIRST moves the column to the front of the schema."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    TABLE_NAME = "test_modify_first_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(
        storage_type,
        instance,
        TABLE_NAME,
        started_cluster_iceberg_no_spark,
        "(a Int32, b Nullable(String), c Nullable(Int64))",
        format_version,
    )

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (1, 'x', 10);", settings=INSERT_SETTINGS)

    instance.query(
        f"ALTER TABLE {TABLE_NAME} MODIFY COLUMN c Nullable(Int64) FIRST;",
        settings=INSERT_SETTINGS,
    )

    col_names = instance.query(
        f"SELECT name FROM system.columns WHERE database = currentDatabase() AND table = '{TABLE_NAME}' ORDER BY position"
    ).strip()
    assert col_names == "c\na\nb", f"Expected c,a,b but got: {col_names}"

    result = instance.query(f"SELECT * FROM {TABLE_NAME}")
    assert result.strip() == "10\t1\tx"


@pytest.mark.parametrize("format_version", [1, 2])
@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_modify_column_after(started_cluster_iceberg_no_spark, format_version, storage_type):
    """MODIFY COLUMN ... AFTER moves the column after the named column."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    TABLE_NAME = "test_modify_after_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(
        storage_type,
        instance,
        TABLE_NAME,
        started_cluster_iceberg_no_spark,
        "(a Int32, b Nullable(String), c Nullable(Int64))",
        format_version,
    )

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (1, 'x', 10);", settings=INSERT_SETTINGS)

    instance.query(
        f"ALTER TABLE {TABLE_NAME} MODIFY COLUMN a Int32 AFTER b;",
        settings=INSERT_SETTINGS,
    )

    col_names = instance.query(
        f"SELECT name FROM system.columns WHERE database = currentDatabase() AND table = '{TABLE_NAME}' ORDER BY position"
    ).strip()
    assert col_names == "b\na\nc", f"Expected b,a,c but got: {col_names}"


@pytest.mark.parametrize("format_version", [1, 2])
@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_modify_column_type_and_first(started_cluster_iceberg_no_spark, format_version, storage_type):
    """MODIFY COLUMN with both a type change and FIRST does both."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    TABLE_NAME = "test_modify_type_first_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(
        storage_type,
        instance,
        TABLE_NAME,
        started_cluster_iceberg_no_spark,
        "(a Int32, b Nullable(String), c Nullable(Int32))",
        format_version,
    )

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (1, 'x', 10);", settings=INSERT_SETTINGS)

    instance.query(
        f"ALTER TABLE {TABLE_NAME} MODIFY COLUMN c Nullable(Int64) FIRST;",
        settings=INSERT_SETTINGS,
    )

    col_names = instance.query(
        f"SELECT name FROM system.columns WHERE database = currentDatabase() AND table = '{TABLE_NAME}' ORDER BY position"
    ).strip()
    assert col_names == "c\na\nb", f"Expected c,a,b but got: {col_names}"

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (3000000000, 2, 'y');", settings=INSERT_SETTINGS)
    result = instance.query(f"SELECT c FROM {TABLE_NAME} ORDER BY c")
    assert "3000000000" in result


@pytest.mark.parametrize("format_version", [1, 2])
@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_modify_column_unrecordable_type_with_position_rejected(started_cluster_iceberg_no_spark, format_version, storage_type):
    """Positioning must not bypass the rejection of a type change Iceberg cannot record."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    TABLE_NAME = "test_modify_unrecordable_pos_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(
        storage_type,
        instance,
        TABLE_NAME,
        started_cluster_iceberg_no_spark,
        "(a Int32, b Nullable(String), c Int32)",
        format_version,
    )

    for clause in ["FIRST", "AFTER a"]:
        error = instance.query_and_get_error(
            f"ALTER TABLE {TABLE_NAME} MODIFY COLUMN c UInt32 {clause};",
            settings=INSERT_SETTINGS,
        )
        assert "BAD_ARGUMENTS" in error, error

    columns = instance.query(
        f"SELECT name, type FROM system.columns WHERE database = currentDatabase() AND table = '{TABLE_NAME}' ORDER BY position"
    ).strip()
    assert columns == "a\tInt32\nb\tNullable(String)\nc\tInt32", columns


@pytest.mark.parametrize("format_version", [1, 2])
@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_modify_column_after_itself(started_cluster_iceberg_no_spark, format_version, storage_type):
    """MODIFY COLUMN ... AFTER the column itself keeps its position."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    TABLE_NAME = "test_modify_after_self_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(
        storage_type,
        instance,
        TABLE_NAME,
        started_cluster_iceberg_no_spark,
        "(a Int32, b Nullable(Int32), c Nullable(String))",
        format_version,
    )

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (1, 10, 'x');", settings=INSERT_SETTINGS)

    instance.query(f"ALTER TABLE {TABLE_NAME} MODIFY COLUMN b Nullable(Int32) AFTER b;", settings=INSERT_SETTINGS)
    instance.query(f"ALTER TABLE {TABLE_NAME} MODIFY COLUMN b Nullable(Int64) AFTER b;", settings=INSERT_SETTINGS)

    columns = instance.query(
        f"SELECT name, type FROM system.columns WHERE database = currentDatabase() AND table = '{TABLE_NAME}' ORDER BY position"
    ).strip()
    assert columns == "a\tInt32\nb\tNullable(Int64)\nc\tNullable(String)", columns

    assert instance.query(f"SELECT * FROM {TABLE_NAME}").strip() == "1\t10\tx"


@pytest.mark.parametrize("format_version", [1, 2])
@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_modify_column_complex_type_with_position(started_cluster_iceberg_no_spark, format_version, storage_type):
    """An unrecordable complex type change is rejected with FIRST/AFTER, restating the same type only moves the column."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    TABLE_NAME = "test_modify_complex_pos_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(
        storage_type,
        instance,
        TABLE_NAME,
        started_cluster_iceberg_no_spark,
        "(x Int32, t Tuple(a Int32))",
        format_version,
    )

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (1, tuple(10));", settings=INSERT_SETTINGS)

    for clause in ["FIRST", "AFTER x"]:
        error = instance.query_and_get_error(
            f"ALTER TABLE {TABLE_NAME} MODIFY COLUMN t Tuple(a UInt32) {clause};",
            settings=INSERT_SETTINGS,
        )
        assert "BAD_ARGUMENTS" in error, error

    instance.query(f"ALTER TABLE {TABLE_NAME} MODIFY COLUMN t Tuple(a Int32) FIRST;", settings=INSERT_SETTINGS)

    columns = instance.query(
        f"SELECT name, type FROM system.columns WHERE database = currentDatabase() AND table = '{TABLE_NAME}' ORDER BY position"
    ).strip()
    assert columns == "t\tTuple(a Int32)\nx\tInt32", columns

    assert instance.query(f"SELECT * FROM {TABLE_NAME}").strip() == "(10)\t1"
