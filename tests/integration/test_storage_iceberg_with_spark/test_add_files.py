import io
import json
import os
import re
import tempfile
from decimal import Decimal

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from helpers.iceberg_utils import (
    create_iceberg_table,
    default_download_directory,
    get_uuid_str,
)


ICEBERG_SETTINGS = {
    "allow_insert_into_iceberg": 1,
    "allow_experimental_iceberg_add_files": 1,
}

# Path of the table location in metadata form (with a leading slash); this is the
# form `add_files` expects, the same one native writes register in manifests.
TABLE_PREFIX = "/var/lib/clickhouse/user_files/iceberg_data/default"
# Storage key prefix inside the MinIO bucket (no leading slash).
S3_KEY_PREFIX = "var/lib/clickhouse/user_files/iceberg_data/default"


# ---------------------------------------------------------------------------
# Test environment — binds cluster/instance/storage_type/table_name once
# so individual tests only express the add_files-specific logic.
# ---------------------------------------------------------------------------

class AddFilesTestEnv:
    def __init__(self, cluster, storage_type, table_prefix, schema="(x Int32)", format_version=2, partition_by=""):
        self.cluster = cluster
        self.instance = cluster.instances["node1"]
        self.storage_type = storage_type
        self.table_name = f"{table_prefix}_{storage_type}_{get_uuid_str()}"
        create_iceberg_table(
            storage_type, self.instance, self.table_name,
            self.cluster, schema, format_version,
            partition_by=partition_by,
        )

    # -- paths ---------------------------------------------------------------

    def table_path(self, subdir, filename):
        """Metadata-form path of a file below the table root, as passed to add_files."""
        return f"{TABLE_PREFIX}/{self.table_name}/{subdir}/{filename}"

    # -- standalone parquet files ---------------------------------------------

    def write_parquet(self, subdir, filename, select_sql):
        """Produce a standalone parquet file (written by ClickHouse itself, so the
        footer matches what Iceberg writers produce) at {table}/{subdir}/{filename}."""
        if self.storage_type == "local":
            target_dir = f"{TABLE_PREFIX}/{self.table_name}/{subdir}"
            self.instance.exec_in_container(["bash", "-c", f"mkdir -p {target_dir}"])
            self.instance.query(
                f"INSERT INTO TABLE FUNCTION "
                f"file('iceberg_data/default/{self.table_name}/{subdir}/{filename}') "
                f"{select_sql} FORMAT Parquet"
            )
        else:
            # Write the file inside the container and pull it out with a
            # binary-safe copy; the host instance dir is not assumed to be
            # reachable from the pytest side.
            tmp_filename = f"add_files_tmp_{get_uuid_str()}.parquet"
            container_path = f"/var/lib/clickhouse/user_files/{tmp_filename}"
            self.instance.query(
                f"INSERT INTO TABLE FUNCTION "
                f"file('{tmp_filename}') "
                f"{select_sql} FORMAT Parquet"
            )
            host_path = os.path.join(tempfile.gettempdir(), tmp_filename)
            self.instance.copy_file_from_container(container_path, host_path)
            with open(host_path, "rb") as f:
                data = f.read()
            os.remove(host_path)
            self.instance.exec_in_container(["rm", "-f", container_path])
            self.put_file(subdir, filename, data)

    def write_parquet_arrow(self, subdir, filename, table, **write_options):
        """Write a parquet file with pyarrow, to get representations that ClickHouse itself
        does not produce (other timestamp units, missing statistics, ...)."""
        buffer = io.BytesIO()
        pq.write_table(table, buffer, **write_options)
        self.put_file(subdir, filename, buffer.getvalue())

    def put_file(self, subdir, filename, data):
        if self.storage_type == "local":
            host_path = os.path.join(tempfile.gettempdir(), f"add_files_tmp_{get_uuid_str()}.parquet")
            with open(host_path, "wb") as f:
                f.write(data)
            target_dir = f"{TABLE_PREFIX}/{self.table_name}/{subdir}"
            self.instance.exec_in_container(["bash", "-c", f"mkdir -p {target_dir}"])
            self.instance.copy_file_to_container(host_path, f"{target_dir}/{filename}")
            os.remove(host_path)
        else:
            key = f"{S3_KEY_PREFIX}/{self.table_name}/{subdir}/{filename}"
            self.cluster.minio_client.put_object(
                self.cluster.minio_bucket, key, io.BytesIO(data), len(data),
            )

    # -- command execution -----------------------------------------------------

    def add_files_query(self, paths, check_duplicate_files=None, allow_missing_columns=None):
        if isinstance(paths, str):
            paths_sql = f"'{paths}'"
        else:
            paths_sql = "[" + ", ".join(f"'{p}'" for p in paths) + "]"
        args = [paths_sql]
        if check_duplicate_files is not None:
            args.append(f"check_duplicate_files = {check_duplicate_files}")
        if allow_missing_columns is not None:
            args.append(f"allow_missing_columns = {allow_missing_columns}")
        return f"ALTER TABLE {self.table_name} EXECUTE add_files({', '.join(args)});"

    @staticmethod
    def parse_metrics(raw):
        counts = {}
        for line in raw.strip().split("\n"):
            if not line:
                continue
            parts = line.split("\t")
            if len(parts) == 2:
                counts[parts[0]] = int(parts[1])
        return counts

    def add_files(self, paths, **kwargs):
        raw = self.instance.query(self.add_files_query(paths, **kwargs), settings=ICEBERG_SETTINGS)
        return self.parse_metrics(raw)

    def add_files_error(self, paths, **kwargs):
        return self.instance.query_and_get_error(self.add_files_query(paths, **kwargs), settings=ICEBERG_SETTINGS)

    # -- helpers ----------------------------------------------------------------

    def insert(self, values):
        self.instance.query(f"INSERT INTO {self.table_name} VALUES {values};", settings=ICEBERG_SETTINGS)

    def query(self, sql):
        return self.instance.query(sql, settings=ICEBERG_SETTINGS)

    def count(self):
        return int(self.query(f"SELECT count() FROM {self.table_name}"))


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("storage_type", ["local", "s3"])
@pytest.mark.parametrize("format_version", [1, 2])
def test_add_files_basic(started_cluster_iceberg_with_spark, storage_type, format_version):
    """Register one external parquet file into an empty table and read it back."""
    env = AddFilesTestEnv(started_cluster_iceberg_with_spark, storage_type, "test_add_basic", format_version=format_version)

    env.write_parquet("data/import", "f1.parquet", "SELECT toInt32(number + 1) AS x FROM numbers(3)")
    counts = env.add_files(env.table_path("data/import", "f1.parquet"))

    assert counts["files_registered"] == 1, f"got {counts}"
    assert counts["rows_registered"] == 3, f"got {counts}"
    assert counts["manifests_written"] == 1, f"got {counts}"
    assert counts["bytes_registered"] > 0, f"got {counts}"
    assert env.query(f"SELECT * FROM {env.table_name} ORDER BY x") == "1\n2\n3\n"


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_array_form_multiple(started_cluster_iceberg_with_spark, storage_type):
    """The array form registers several files in one snapshot."""
    env = AddFilesTestEnv(started_cluster_iceberg_with_spark, storage_type, "test_add_array")

    env.write_parquet("data/import", "f1.parquet", "SELECT toInt32(number) AS x FROM numbers(3)")
    env.write_parquet("data/import", "f2.parquet", "SELECT toInt32(number + 10) AS x FROM numbers(3)")
    counts = env.add_files([
        env.table_path("data/import", "f1.parquet"),
        env.table_path("data/import", "f2.parquet"),
    ])

    assert counts["files_registered"] == 2, f"got {counts}"
    assert counts["rows_registered"] == 6, f"got {counts}"
    assert env.count() == 6
    assert env.query(f"SELECT * FROM {env.table_name} ORDER BY x") == "0\n1\n2\n10\n11\n12\n"


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_onto_nonempty_table(started_cluster_iceberg_with_spark, storage_type):
    """Registration appends a snapshot on top of natively-written data."""
    env = AddFilesTestEnv(started_cluster_iceberg_with_spark, storage_type, "test_add_nonempty")

    env.insert("(1), (2)")
    env.write_parquet("data/import", "f1.parquet", "SELECT toInt32(number + 100) AS x FROM numbers(3)")
    counts = env.add_files(env.table_path("data/import", "f1.parquet"))

    assert counts["files_registered"] == 1, f"got {counts}"
    assert env.count() == 5
    assert env.query(f"SELECT * FROM {env.table_name} ORDER BY x") == "1\n2\n100\n101\n102\n"


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_trailing_slash_expansion(started_cluster_iceberg_with_spark, storage_type):
    """A path ending with '/' registers every *.parquet below it, skipping hidden files."""
    env = AddFilesTestEnv(started_cluster_iceberg_with_spark, storage_type, "test_add_expand")

    env.write_parquet("data/import", "f1.parquet", "SELECT toInt32(1) AS x")
    env.write_parquet("data/import", "f2.parquet", "SELECT toInt32(2) AS x")
    env.write_parquet("data/import", ".hidden.parquet", "SELECT toInt32(3) AS x")

    counts = env.add_files(env.table_path("data/import", ""))

    assert counts["files_registered"] == 2, f"got {counts}"
    assert env.count() == 2
    assert env.query(f"SELECT * FROM {env.table_name} ORDER BY x") == "1\n2\n"


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_partitioned_identity(started_cluster_iceberg_with_spark, storage_type):
    """Single-partition files register into an identity-partitioned table; a file
    spanning two partition values is rejected."""
    env = AddFilesTestEnv(
        started_cluster_iceberg_with_spark, storage_type, "test_add_identity",
        partition_by="identity(x)",
    )

    env.write_parquet("data/import", "f7.parquet", "SELECT toInt32(7) AS x FROM numbers(3)")
    counts = env.add_files(env.table_path("data/import", "f7.parquet"))
    assert counts["files_registered"] == 1, f"got {counts}"
    assert counts["partitions_touched"] == 1, f"got {counts}"

    env.write_parquet("data/import", "f8.parquet", "SELECT toInt32(8) AS x FROM numbers(2)")
    counts = env.add_files(env.table_path("data/import", "f8.parquet"))
    assert counts["partitions_touched"] == 1, f"got {counts}"
    assert env.query(f"SELECT * FROM {env.table_name} ORDER BY x") == "7\n7\n7\n8\n8\n"

    env.write_parquet("data/import", "mixed.parquet", "SELECT toInt32(7) AS x UNION ALL SELECT toInt32(8) AS x")
    error = env.add_files_error(env.table_path("data/import", "mixed.parquet"))
    assert "spans more than one partition" in error, f"got {error}"
    # The failed registration must not have committed anything.
    assert env.count() == 5


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_day_transform(started_cluster_iceberg_with_spark, storage_type):
    """The day() transform is validated from footer bounds: same-day file registers,
    a file crossing midnight is rejected."""
    env = AddFilesTestEnv(
        started_cluster_iceberg_with_spark, storage_type, "test_add_day",
        schema="(d DateTime64(6))",
        partition_by="toRelativeDayNum(d)",
    )

    env.write_parquet(
        "data/import", "same_day.parquet",
        "SELECT toDateTime64('2024-01-01 10:00:00', 6) AS d "
        "UNION ALL SELECT toDateTime64('2024-01-01 23:00:00', 6) AS d",
    )
    counts = env.add_files(env.table_path("data/import", "same_day.parquet"))
    assert counts["files_registered"] == 1, f"got {counts}"
    assert env.count() == 2

    env.write_parquet(
        "data/import", "two_days.parquet",
        "SELECT toDateTime64('2024-01-01 10:00:00', 6) AS d "
        "UNION ALL SELECT toDateTime64('2024-01-02 10:00:00', 6) AS d",
    )
    error = env.add_files_error(env.table_path("data/import", "two_days.parquet"))
    assert "spans more than one partition" in error, f"got {error}"
    assert env.count() == 2


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_duplicate_detection(started_cluster_iceberg_with_spark, storage_type):
    """Duplicates against the current snapshot and inside the input are rejected by
    default; check_duplicate_files = 0 disables the check."""
    env = AddFilesTestEnv(started_cluster_iceberg_with_spark, storage_type, "test_add_dup")

    env.write_parquet("data/import", "f1.parquet", "SELECT toInt32(1) AS x")
    path = env.table_path("data/import", "f1.parquet")

    counts = env.add_files(path)
    assert counts["files_registered"] == 1, f"got {counts}"

    error = env.add_files_error(path)
    assert "already registered in the current snapshot" in error, f"got {error}"

    error = env.add_files_error([path, path])
    assert "listed twice" in error, f"got {error}"

    counts = env.add_files(path, check_duplicate_files=0)
    assert counts["files_registered"] == 1, f"got {counts}"
    assert env.count() == 2


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_allow_missing_columns(started_cluster_iceberg_with_spark, storage_type):
    """A file lacking a schema column is rejected by default and readable with
    allow_missing_columns = 1 (missing values read as NULL)."""
    env = AddFilesTestEnv(
        started_cluster_iceberg_with_spark, storage_type, "test_add_missing",
        schema="(x Int32, y Nullable(String))",
    )

    env.write_parquet("data/import", "only_x.parquet", "SELECT toInt32(1) AS x")
    path = env.table_path("data/import", "only_x.parquet")

    error = env.add_files_error(path)
    assert "lacks column" in error, f"got {error}"
    assert "'y'" in error, f"got {error}"

    counts = env.add_files(path, allow_missing_columns=1)
    assert counts["files_registered"] == 1, f"got {counts}"
    assert env.query(f"SELECT x, y FROM {env.table_name}") == "1\t\\N\n"


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_argument_errors(started_cluster_iceberg_with_spark, storage_type):
    """Missing files, non-parquet paths, and bucket-partitioned tables are rejected."""
    env = AddFilesTestEnv(started_cluster_iceberg_with_spark, storage_type, "test_add_args")

    error = env.add_files_error(env.table_path("data", "nonexistent.parquet"))
    assert "does not exist" in error, f"got {error}"

    error = env.add_files_error(env.table_path("data", "f1.orc"))
    assert ".parquet" in error, f"got {error}"

    env_bucket = AddFilesTestEnv(
        started_cluster_iceberg_with_spark, storage_type, "test_add_bucket",
        partition_by="icebergBucket(4, x)",
    )
    env_bucket.write_parquet("data/import", "f1.parquet", "SELECT toInt32(1) AS x")
    error = env_bucket.add_files_error(env_bucket.table_path("data/import", "f1.parquet"))
    assert "bucket transform" in error, f"got {error}"


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_rejects_mismatched_types(started_cluster_iceberg_with_spark, storage_type):
    """Footer statistics are used as Iceberg bounds as they are, so a file whose column is stored
    differently from the Iceberg type must be rejected. A matching file registers, and filters
    on its bounds return every matching row."""
    env = AddFilesTestEnv(
        started_cluster_iceberg_with_spark, storage_type, "test_add_types",
        schema="(x Nullable(Int64), d Nullable(DateTime64(6)))",
    )

    timestamps = [1704103200000000 + i * 1000000 for i in range(10)]  # 2024-01-01 10:00:00 + i seconds

    # INT32 data for an Iceberg `long` column: the bounds would be 4 bytes instead of 8.
    env.write_parquet_arrow("data/import", "int32.parquet", pa.table({
        "x": pa.array(range(1, 11), type=pa.int32()),
        "d": pa.array(timestamps, type=pa.timestamp("us")),
    }))
    error = env.add_files_error(env.table_path("data/import", "int32.parquet"))
    assert "does not match the Iceberg type 'long'" in error, f"got {error}"

    # Nanosecond timestamps: the bounds would be read as microseconds.
    env.write_parquet_arrow("data/import", "ns.parquet", pa.table({
        "x": pa.array(range(1, 11), type=pa.int64()),
        "d": pa.array([t * 1000 for t in timestamps], type=pa.timestamp("ns")),
    }))
    error = env.add_files_error(env.table_path("data/import", "ns.parquet"))
    assert "does not match the Iceberg type" in error, f"got {error}"

    # A string column for an integer field.
    env.write_parquet_arrow("data/import", "string.parquet", pa.table({
        "x": pa.array([str(i) for i in range(1, 11)], type=pa.string()),
        "d": pa.array(timestamps, type=pa.timestamp("us")),
    }))
    error = env.add_files_error(env.table_path("data/import", "string.parquet"))
    assert "does not match the Iceberg type 'long'" in error, f"got {error}"

    assert env.count() == 0

    env.write_parquet_arrow("data/import", "ok.parquet", pa.table({
        "x": pa.array(range(1, 11), type=pa.int64()),
        "d": pa.array(timestamps, type=pa.timestamp("us")),
    }))
    counts = env.add_files(env.table_path("data/import", "ok.parquet"))
    assert counts["files_registered"] == 1, f"got {counts}"
    assert env.query(f"SELECT count() FROM {env.table_name} WHERE x = 5") == "1\n"
    assert env.query(f"SELECT count() FROM {env.table_name} WHERE x > 8") == "2\n"
    assert env.query(
        f"SELECT count() FROM {env.table_name} WHERE d >= toDateTime64('2024-01-01 10:00:08', 6, 'UTC')"
    ) == "2\n"


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_partition_value_needs_statistics(started_cluster_iceberg_with_spark, storage_type):
    """The partition value is derived from footer statistics. A file without them must be rejected
    instead of being registered in the null partition; a file whose values are all null does belong
    to the null partition."""
    env = AddFilesTestEnv(
        started_cluster_iceberg_with_spark, storage_type, "test_add_no_stats",
        schema="(s Nullable(String))",
        partition_by="identity(s)",
    )

    values = pa.table({"s": pa.array(["abc"] * 3, type=pa.string())})
    env.write_parquet_arrow("data/import", "no_stats.parquet", values, write_statistics=False)
    error = env.add_files_error(env.table_path("data/import", "no_stats.parquet"))
    assert "no complete min/max statistics" in error, f"got {error}"
    assert env.count() == 0

    env.write_parquet_arrow("data/import", "stats.parquet", values)
    counts = env.add_files(env.table_path("data/import", "stats.parquet"))
    assert counts["partitions_touched"] == 1, f"got {counts}"

    env.write_parquet_arrow("data/import", "nulls.parquet", pa.table({"s": pa.array([None, None], type=pa.string())}))
    counts = env.add_files(env.table_path("data/import", "nulls.parquet"))
    assert counts["partitions_touched"] == 1, f"got {counts}"

    assert env.query(f"SELECT count() FROM {env.table_name} WHERE s = 'abc'") == "3\n"
    assert env.query(f"SELECT count() FROM {env.table_name} WHERE s IS NULL") == "2\n"
    assert env.count() == 5


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_requires_setting(started_cluster_iceberg_with_spark, storage_type):
    """add_files is experimental and gated by its own setting."""
    env = AddFilesTestEnv(started_cluster_iceberg_with_spark, storage_type, "test_add_setting")
    env.write_parquet("data/import", "f1.parquet", "SELECT toInt32(1) AS x")

    error = env.instance.query_and_get_error(
        env.add_files_query(env.table_path("data/import", "f1.parquet")),
        settings={"allow_insert_into_iceberg": 1},
    )
    assert "allow_experimental_iceberg_add_files" in error, f"got {error}"
    assert env.count() == 0


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_rejects_paths_outside_table_location(started_cluster_iceberg_with_spark, storage_type):
    """Files are read and registered through the table's storage, so only files under the table
    location may be registered: not the files of another table, not a sibling directory sharing
    the prefix, and not a path escaping the location with '..'."""
    env = AddFilesTestEnv(started_cluster_iceberg_with_spark, storage_type, "test_add_outside")
    other = AddFilesTestEnv(started_cluster_iceberg_with_spark, storage_type, "test_add_outside_other")
    other.write_parquet("data/import", "f1.parquet", "SELECT toInt32(1) AS x")

    for path in [
        other.table_path("data/import", "f1.parquet"),
        other.table_path("data/import", ""),
        f"{TABLE_PREFIX}/{env.table_name}_sibling/data/f1.parquet",
    ]:
        error = env.add_files_error(path)
        assert "is not under the table location" in error, f"{path}: got {error}"

    error = env.add_files_error(env.table_path("data", f"../../{other.table_name}/data/import/f1.parquet"))
    assert "relative components" in error, f"got {error}"

    assert env.count() == 0


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_empty_directory(started_cluster_iceberg_with_spark, storage_type):
    """A directory without parquet files is an error instead of an empty snapshot."""
    env = AddFilesTestEnv(started_cluster_iceberg_with_spark, storage_type, "test_add_empty_dir")
    env.write_parquet("data/import", ".hidden.parquet", "SELECT toInt32(1) AS x")

    error = env.add_files_error(env.table_path("data/import", ""))
    assert "contains no parquet files" in error, f"got {error}"

    snapshots = env.query(f"SELECT count() FROM system.iceberg_history WHERE table = '{env.table_name}'")
    assert snapshots == "0\n", f"got {snapshots}"


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_missing_required_column(started_cluster_iceberg_with_spark, storage_type):
    """A missing column reads as NULL, so allow_missing_columns = 1 does not cover required fields."""
    env = AddFilesTestEnv(
        started_cluster_iceberg_with_spark, storage_type, "test_add_missing_required",
        schema="(x Int32, y Int32)",
    )
    env.write_parquet_arrow("data/import", "only_x.parquet", pa.table({"x": pa.array([1], type=pa.int32())}))
    path = env.table_path("data/import", "only_x.parquet")

    error = env.add_files_error(path, allow_missing_columns=1)
    assert "required in the table schema" in error, f"got {error}"
    assert "'y'" in error, f"got {error}"
    assert env.count() == 0


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_rejects_unsupported_representations(started_cluster_iceberg_with_spark, storage_type):
    """Decimal columns and millisecond timestamps would need their bounds converted to the Iceberg
    encoding, so they are rejected rather than registered with wrong bounds."""
    env_decimal = AddFilesTestEnv(
        started_cluster_iceberg_with_spark, storage_type, "test_add_decimal",
        schema="(x Nullable(Decimal(9, 2)))",
    )
    env_decimal.write_parquet_arrow("data/import", "decimal.parquet", pa.table({
        "x": pa.array([Decimal("1.00"), Decimal("-2.00")], type=pa.decimal128(9, 2)),
    }))
    error = env_decimal.add_files_error(env_decimal.table_path("data/import", "decimal.parquet"))
    assert "which add_files does not support yet" in error, f"got {error}"
    assert env_decimal.count() == 0

    env_ts = AddFilesTestEnv(
        started_cluster_iceberg_with_spark, storage_type, "test_add_ts_millis",
        schema="(d Nullable(DateTime64(6)))",
        partition_by="toRelativeDayNum(d)",
    )
    env_ts.write_parquet_arrow("data/import", "millis.parquet", pa.table({
        "d": pa.array([1704103200000, 1704103201000], type=pa.timestamp("ms")),
    }), coerce_timestamps="ms")
    error = env_ts.add_files_error(env_ts.table_path("data/import", "millis.parquet"))
    assert "does not match the Iceberg type" in error, f"got {error}"
    assert env_ts.count() == 0


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_truncate_multibyte(started_cluster_iceberg_with_spark, storage_type):
    """truncate[N] keeps N code points, as a native insert does: a registered file lands in the same
    partition as natively inserted rows, so partition pruning keeps both."""
    env = AddFilesTestEnv(
        started_cluster_iceberg_with_spark, storage_type, "test_add_truncate_utf8",
        schema="(s Nullable(String))",
        partition_by="icebergTruncate(2, s)",
    )
    env.insert("('äöü')")

    env.write_parquet_arrow("data/import", "same.parquet", pa.table({"s": pa.array(["äöü", "äöx"], type=pa.string())}))
    counts = env.add_files(env.table_path("data/import", "same.parquet"))
    assert counts["partitions_touched"] == 1, f"got {counts}"

    # 'äb' and 'äc' share their first 2 bytes, but not their first 2 code points.
    env.write_parquet_arrow("data/import", "two.parquet", pa.table({"s": pa.array(["äb", "äc"], type=pa.string())}))
    error = env.add_files_error(env.table_path("data/import", "two.parquet"))
    assert "spans more than one partition" in error, f"got {error}"

    pruning = {"use_iceberg_partition_pruning": 1}
    assert env.instance.query(
        f"SELECT count() FROM {env.table_name} WHERE s = 'äöü'", settings=pruning
    ) == "2\n"
    assert env.instance.query(
        f"SELECT count() FROM {env.table_name} WHERE s = 'äöx'", settings=pruning
    ) == "1\n"
    assert env.count() == 3


def download_for_spark(cluster, storage_type, table_name):
    """Copy the table to the host for Spark and point the version hint at the latest metadata file.
    Returns the local table directory and the parsed latest metadata."""
    local_dir = f"/var/lib/clickhouse/user_files/iceberg_data/default/{table_name}/"
    default_download_directory(cluster, storage_type, local_dir, local_dir)
    metadata_dir = os.path.join(local_dir, "metadata")
    versions = {}
    for name in os.listdir(metadata_dir):
        match = re.match(r"v(\d+)(?:[-.].*)?\.metadata\.json$", name)
        if match:
            versions[int(match.group(1))] = name
    latest = max(versions)
    with open(os.path.join(metadata_dir, "version-hint.text"), "w") as hint:
        hint.write(str(latest))
    with open(os.path.join(metadata_dir, versions[latest])) as f:
        return local_dir, json.load(f)


def name_mapping_of(metadata):
    """Field id -> names of the default name mapping, over all nesting levels."""
    result = {}

    def walk(entries):
        for entry in entries:
            result[entry["field-id"]] = entry["names"]
            walk(entry.get("fields", []))

    walk(json.loads(metadata["properties"]["schema.name-mapping.default"]))
    return result


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_spark_reads_by_name(started_cluster_iceberg_with_spark, storage_type):
    """Registered files carry no Iceberg field ids. add_files sets the default name mapping, so Spark
    maps their columns by name, even when the file's column order differs from the schema."""
    cluster = started_cluster_iceberg_with_spark
    env = AddFilesTestEnv(
        cluster, storage_type, "test_add_spark",
        schema="(a Nullable(Int32), b Nullable(String))",
    )
    env.write_parquet_arrow("data/import", "reordered.parquet", pa.table({
        "b": pa.array(["x", "y"], type=pa.string()),
        "a": pa.array([1, 2], type=pa.int32()),
    }))
    env.add_files(env.table_path("data/import", "reordered.parquet"))
    assert env.query(f"SELECT a, b FROM {env.table_name} ORDER BY a") == "1\tx\n2\ty\n"

    local_dir, metadata = download_for_spark(cluster, storage_type, env.table_name)
    assert sorted(names[0] for names in name_mapping_of(metadata).values()) == ["a", "b"]
    rows = cluster.spark_session.read.format("iceberg").load(local_dir).collect()
    assert sorted((row["a"], row["b"]) for row in rows) == [(1, "x"), (2, "y")]


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_nulls_in_required_column(started_cluster_iceberg_with_spark, storage_type):
    """A required field must have a value in every row, so a file with nulls in its column, or without
    the null counts that prove it has none, is rejected. A column that cannot hold nulls needs no counts."""
    env = AddFilesTestEnv(
        started_cluster_iceberg_with_spark, storage_type, "test_add_required_nulls",
        schema="(x Int32)",
    )

    env.write_parquet_arrow("data/import", "nulls.parquet", pa.table({"x": pa.array([1, None], type=pa.int32())}))
    error = env.add_files_error(env.table_path("data/import", "nulls.parquet"))
    assert "contains 1 null(s)" in error, f"got {error}"

    env.write_parquet_arrow(
        "data/import", "no_stats.parquet", pa.table({"x": pa.array([1, 2], type=pa.int32())}), write_statistics=False)
    error = env.add_files_error(env.table_path("data/import", "no_stats.parquet"))
    assert "may contain nulls" in error, f"got {error}"
    assert env.count() == 0

    required = pa.schema([pa.field("x", pa.int32(), nullable=False)])
    env.write_parquet_arrow(
        "data/import", "required.parquet", pa.table({"x": pa.array([1, 2], type=pa.int32())}, schema=required),
        write_statistics=False)
    counts = env.add_files(env.table_path("data/import", "required.parquet"))
    assert counts["files_registered"] == 1, f"got {counts}"
    assert env.count() == 2
