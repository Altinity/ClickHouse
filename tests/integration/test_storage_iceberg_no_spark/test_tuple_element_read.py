import pytest

from helpers.iceberg_utils import (
    create_iceberg_table,
    get_uuid_str
)


@pytest.mark.parametrize("read_optimization", [0, 1])
def test_tuple_element_read(started_cluster_iceberg_no_spark, read_optimization):
    """Manifests written by ClickHouse carry statistics only for top-level columns,
    so a tuple element such as `t.a` has none. That must not make the read
    optimization treat the element as absent from the file and return NULL."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    TABLE_NAME = "test_tuple_element_read_" + get_uuid_str()

    create_iceberg_table(
        "local",
        instance,
        TABLE_NAME,
        started_cluster_iceberg_no_spark,
        "(id Int64, t Tuple(a Nullable(Int64), b Nullable(String)))",
        format_version=2,
    )
    instance.query(
        f"INSERT INTO {TABLE_NAME} VALUES (1, (42, 'x')), (2, (NULL, 'y'))",
        settings={"allow_insert_into_iceberg": 1},
    )

    settings = {"allow_experimental_iceberg_read_optimization": read_optimization}
    assert (
        instance.query(f"SELECT id, t.a, t.b FROM {TABLE_NAME} ORDER BY id", settings=settings)
        == "1\t42\tx\n2\t\\N\ty\n"
    )
    assert instance.query(f"SELECT t.a FROM {TABLE_NAME} ORDER BY id", settings=settings) == "42\n\\N\n"
    assert (
        instance.query(f"SELECT id, t, t.b FROM {TABLE_NAME} ORDER BY id", settings=settings)
        == "1\t(42,'x')\tx\n2\t(NULL,'y')\ty\n"
    )
