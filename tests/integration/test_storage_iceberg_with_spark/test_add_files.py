import io
import os
import tempfile

import pytest

from helpers.iceberg_utils import (
    create_iceberg_table,
    get_uuid_str,
)


ICEBERG_SETTINGS = {
    "allow_insert_into_iceberg": 1,
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

    env.write_parquet("data/import", "f1.parquet", "SELECT number + 1 AS x FROM numbers(3)")
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

    env.write_parquet("data/import", "f1.parquet", "SELECT number AS x FROM numbers(3)")
    env.write_parquet("data/import", "f2.parquet", "SELECT number + 10 AS x FROM numbers(3)")
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
    env.write_parquet("data/import", "f1.parquet", "SELECT number + 100 AS x FROM numbers(3)")
    counts = env.add_files(env.table_path("data/import", "f1.parquet"))

    assert counts["files_registered"] == 1, f"got {counts}"
    assert env.count() == 5
    assert env.query(f"SELECT * FROM {env.table_name} ORDER BY x") == "1\n2\n100\n101\n102\n"


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_add_files_trailing_slash_expansion(started_cluster_iceberg_with_spark, storage_type):
    """A path ending with '/' registers every *.parquet below it, skipping hidden files."""
    env = AddFilesTestEnv(started_cluster_iceberg_with_spark, storage_type, "test_add_expand")

    env.write_parquet("data/import", "f1.parquet", "SELECT 1 AS x")
    env.write_parquet("data/import", "f2.parquet", "SELECT 2 AS x")
    env.write_parquet("data/import", ".hidden.parquet", "SELECT 3 AS x")

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

    env.write_parquet("data/import", "f7.parquet", "SELECT 7 AS x FROM numbers(3)")
    counts = env.add_files(env.table_path("data/import", "f7.parquet"))
    assert counts["files_registered"] == 1, f"got {counts}"
    assert counts["partitions_touched"] == 1, f"got {counts}"

    env.write_parquet("data/import", "f8.parquet", "SELECT 8 AS x FROM numbers(2)")
    counts = env.add_files(env.table_path("data/import", "f8.parquet"))
    assert counts["partitions_touched"] == 1, f"got {counts}"
    assert env.query(f"SELECT * FROM {env.table_name} ORDER BY x") == "7\n7\n7\n8\n8\n"

    env.write_parquet("data/import", "mixed.parquet", "SELECT 7 AS x UNION ALL SELECT 8 AS x")
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

    env.write_parquet("data/import", "f1.parquet", "SELECT 1 AS x")
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

    env.write_parquet("data/import", "only_x.parquet", "SELECT 1 AS x")
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
    env_bucket.write_parquet("data/import", "f1.parquet", "SELECT 1 AS x")
    error = env_bucket.add_files_error(env_bucket.table_path("data/import", "f1.parquet"))
    assert "bucket" in error, f"got {error}"
