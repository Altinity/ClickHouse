import logging
import json
import os
import subprocess
import time
import uuid
import time
from datetime import datetime, timezone

import pyspark
import pytest
from minio.deleteobjects import DeleteObject
from pyspark.sql.functions import (
    monotonically_increasing_id,
    row_number,
)
from pyspark.sql.types import (
    ArrayType,
    BooleanType,
    DateType,
    IntegerType,
    StringType,
    StructField,
    StructType,
)
from pyspark.sql.window import Window

from helpers.cluster import ClickHouseCluster, ClickHouseInstance
from helpers.s3_tools import (
    AzureUploader,
    LocalUploader,
    S3Uploader,
    LocalDownloader,
    list_s3_objects,
    prepare_s3_bucket,
)
from helpers.test_tools import TSV

from helpers.iceberg_utils import (
    default_upload_directory,
    additional_upload_directory,
    default_download_directory,
    execute_spark_query_general,
    get_creation_expression,
    write_iceberg_from_df,
    generate_data,
    create_iceberg_table,
    drop_iceberg_table,
    check_schema_and_data,
    get_uuid_str,
    check_validity_and_get_prunned_files_general,
    get_last_snapshot
)

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))


def get_spark():
    builder = (
        pyspark.sql.SparkSession.builder.appName("spark_test")
        .config(
            "spark.sql.catalog.spark_catalog",
            "org.apache.iceberg.spark.SparkSessionCatalog",
        )
        .config("spark.sql.catalog.local", "org.apache.iceberg.spark.SparkCatalog")
        .config("spark.sql.catalog.spark_catalog.type", "hadoop")
        .config("spark.sql.catalog.spark_catalog.warehouse", "/iceberg_data")
        .config(
            "spark.sql.extensions",
            "org.apache.iceberg.spark.extensions.IcebergSparkSessionExtensions",
        )
        .master("local")
    )
    return builder.master("local").getOrCreate()


@pytest.fixture(scope="module")
def started_cluster():
    try:
        cluster = ClickHouseCluster(__file__, with_spark=True)
        cluster.add_instance(
            "node1",
            main_configs=[
                "configs/config.d/query_log.xml",
                "configs/config.d/cluster.xml",
                "configs/config.d/named_collections.xml",
                "configs/config.d/filesystem_caches.xml",
                "configs/config.d/metadata_log.xml",
            ],
            user_configs=["configs/users.d/users.xml"],
            with_minio=True,
            with_azurite=True,
            stay_alive=True,
        )
        cluster.add_instance(
            "node2",
            main_configs=[
                "configs/config.d/query_log.xml",
                "configs/config.d/cluster.xml",
                "configs/config.d/named_collections.xml",
                "configs/config.d/filesystem_caches.xml",
                "configs/config.d/metadata_log.xml",
            ],
            user_configs=["configs/users.d/users.xml"],
            stay_alive=True,
        )
        cluster.add_instance(
            "node3",
            main_configs=[
                "configs/config.d/query_log.xml",
                "configs/config.d/cluster.xml",
                "configs/config.d/named_collections.xml",
                "configs/config.d/filesystem_caches.xml",
                "configs/config.d/metadata_log.xml",
            ],
            user_configs=["configs/users.d/users.xml"],
            stay_alive=True,
        )

        logging.info("Starting cluster...")
        cluster.start()

        prepare_s3_bucket(cluster)
        logging.info("S3 bucket created")

        cluster.spark_session = get_spark()
        cluster.default_s3_uploader = S3Uploader(
            cluster.minio_client, cluster.minio_bucket
        )

        cluster.azure_container_name = "mycontainer"

        cluster.blob_service_client = cluster.blob_service_client

        container_client = cluster.blob_service_client.create_container(
            cluster.azure_container_name
        )

        cluster.container_client = container_client

        cluster.default_azure_uploader = AzureUploader(
            cluster.blob_service_client, cluster.azure_container_name
        )

        cluster.default_local_uploader = LocalUploader(cluster.instances["node1"])
        cluster.default_local_downloader = LocalDownloader(cluster.instances["node1"])

        yield cluster

    finally:
        cluster.shutdown()


@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_single_iceberg_file(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = (
        "test_single_iceberg_file_"
        + format_version
        + "_"
        + storage_type
        + "_"
        + get_uuid_str()
    )

    write_iceberg_from_df(spark, generate_data(spark, 0, 100), TABLE_NAME)

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)

    assert instance.query(f"SELECT * FROM {TABLE_NAME}") == instance.query(
        "SELECT number, toString(number + 1) FROM numbers(100)"
    )


@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_partition_by(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = (
        "test_partition_by_"
        + format_version
        + "_"
        + storage_type
        + "_"
        + get_uuid_str()
    )

    write_iceberg_from_df(
        spark,
        generate_data(spark, 0, 10),
        TABLE_NAME,
        mode="overwrite",
        format_version=format_version,
        partition_by="a",
    )

    files = default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )
    assert len(files) == 14  # 10 partitions + 4 metadata files

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)
    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 10
    assert int(instance.query(f"SELECT count() FROM system.iceberg_history WHERE table = '{TABLE_NAME}'")) == 1


@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_multiple_iceberg_files(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = (
        "test_multiple_iceberg_files_"
        + format_version
        + "_"
        + storage_type
        + "_"
        + get_uuid_str()
    )

    write_iceberg_from_df(
        spark,
        generate_data(spark, 0, 100),
        TABLE_NAME,
        mode="overwrite",
        format_version=format_version,
    )

    files = default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    # ['/iceberg_data/default/test_multiple_iceberg_files/data/00000-1-35302d56-f1ed-494e-a85b-fbf85c05ab39-00001.parquet',
    # '/iceberg_data/default/test_multiple_iceberg_files/metadata/version-hint.text',
    # '/iceberg_data/default/test_multiple_iceberg_files/metadata/3127466b-299d-48ca-a367-6b9b1df1e78c-m0.avro',
    # '/iceberg_data/default/test_multiple_iceberg_files/metadata/snap-5220855582621066285-1-3127466b-299d-48ca-a367-6b9b1df1e78c.avro',
    # '/iceberg_data/default/test_multiple_iceberg_files/metadata/v1.metadata.json']
    assert len(files) == 5

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)
    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 100

    write_iceberg_from_df(
        spark,
        generate_data(spark, 100, 200),
        TABLE_NAME,
        mode="append",
        format_version=format_version,
    )
    files = default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        "",
    )
    assert len(files) == 9

    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 200
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY 1") == instance.query(
        "SELECT number, toString(number + 1) FROM numbers(200)"
    )


@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_types(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = (
        "test_types_" + format_version + "_" + storage_type + "_" + get_uuid_str()
    )

    data = [
        (
            123,
            "string",
            datetime.strptime("2000-01-01", "%Y-%m-%d"),
            ["str1", "str2"],
            True,
        )
    ]
    schema = StructType(
        [
            StructField("a", IntegerType()),
            StructField("b", StringType()),
            StructField("c", DateType()),
            StructField("d", ArrayType(StringType())),
            StructField("e", BooleanType()),
        ]
    )
    df = spark.createDataFrame(data=data, schema=schema)
    df.printSchema()
    write_iceberg_from_df(
        spark, df, TABLE_NAME, mode="overwrite", format_version=format_version
    )

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)
    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 1
    assert (
        instance.query(f"SELECT a, b, c, d, e FROM {TABLE_NAME}").strip()
        == "123\tstring\t2000-01-01\t['str1','str2']\ttrue"
    )

    table_function_expr = get_creation_expression(
        storage_type, TABLE_NAME, started_cluster, table_function=True
    )
    assert (
        instance.query(f"SELECT a, b, c, d, e FROM {table_function_expr}").strip()
        == "123\tstring\t2000-01-01\t['str1','str2']\ttrue"
    )

    assert instance.query(f"DESCRIBE {table_function_expr} FORMAT TSV") == TSV(
        [
            ["a", "Nullable(Int32)"],
            ["b", "Nullable(String)"],
            ["c", "Nullable(Date)"],
            ["d", "Array(Nullable(String))"],
            ["e", "Nullable(Bool)"],
        ]
    )

    # Test storage type as function argument
    table_function_expr = get_creation_expression(
        storage_type,
        TABLE_NAME,
        started_cluster,
        table_function=True,
        storage_type_as_arg=True,
    )
    assert (
        instance.query(f"SELECT a, b, c, d, e FROM {table_function_expr}").strip()
        == "123\tstring\t2000-01-01\t['str1','str2']\ttrue"
    )

    assert instance.query(f"DESCRIBE {table_function_expr} FORMAT TSV") == TSV(
        [
            ["a", "Nullable(Int32)"],
            ["b", "Nullable(String)"],
            ["c", "Nullable(Date)"],
            ["d", "Array(Nullable(String))"],
            ["e", "Nullable(Bool)"],
        ]
    )

    # Test storage type as field in named collection
    table_function_expr = get_creation_expression(
        storage_type,
        TABLE_NAME,
        started_cluster,
        table_function=True,
        storage_type_in_named_collection=True,
    )
    assert (
        instance.query(f"SELECT a, b, c, d, e FROM {table_function_expr}").strip()
        == "123\tstring\t2000-01-01\t['str1','str2']\ttrue"
    )

    assert instance.query(f"DESCRIBE {table_function_expr} FORMAT TSV") == TSV(
        [
            ["a", "Nullable(Int32)"],
            ["b", "Nullable(String)"],
            ["c", "Nullable(Date)"],
            ["d", "Array(Nullable(String))"],
            ["e", "Nullable(Bool)"],
        ]
    )


def count_secondary_subqueries(started_cluster, query_id, expected, comment):
    for node_name, replica in started_cluster.instances.items():
        cluster_secondary_queries = (
            replica.query(
                f"""
                SELECT count(*) FROM system.query_log
                WHERE
                    type = 'QueryFinish'
                    AND NOT is_initial_query
                    AND initial_query_id='{query_id}'
            """
            )
            .strip()
        )

        logging.info(
            f"[{node_name}] cluster_secondary_queries {comment}: {cluster_secondary_queries}"
        )
        assert int(cluster_secondary_queries) == expected


@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
@pytest.mark.parametrize("cluster_name_as_literal", [True, False])
def test_cluster_table_function(started_cluster, format_version, storage_type, cluster_name_as_literal):

    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session

    TABLE_NAME = (
        "test_iceberg_cluster_"
        + format_version
        + "_"
        + storage_type
        + "_"
        + get_uuid_str()
    )

    def add_df(mode):
        write_iceberg_from_df(
            spark,
            generate_data(spark, 0, 100),
            TABLE_NAME,
            mode=mode,
            format_version=format_version,
        )

        files = default_upload_directory(
            started_cluster,
            storage_type,
            f"/iceberg_data/default/{TABLE_NAME}/",
            f"/iceberg_data/default/{TABLE_NAME}/",
        )

        logging.info(f"Adding another dataframe. result files: {files}")

        if storage_type == "local":
            # For local storage we need to upload data from each node
            for node_name, replica in started_cluster.instances.items():
                if node_name == "node1":
                    continue
                additional_upload_directory(
                    started_cluster,
                    node_name,
                    storage_type,
                    f"/iceberg_data/default/{TABLE_NAME}/",
                    f"/iceberg_data/default/{TABLE_NAME}/",
                )

        return files

    files = add_df(mode="overwrite")
    for i in range(1, len(started_cluster.instances)):
        files = add_df(mode="append")

    logging.info(f"Setup complete. files: {files}")
    assert len(files) == 5 + 4 * (len(started_cluster.instances) - 1)

    clusters = instance.query(f"SELECT * FROM system.clusters")
    logging.info(f"Clusters setup: {clusters}")

    # Regular Query only node1
    table_function_expr = get_creation_expression(
        storage_type, TABLE_NAME, started_cluster, table_function=True, cluster_name_as_literal=cluster_name_as_literal
    )
    select_regular = (
        instance.query(f"SELECT * FROM {table_function_expr}").strip().split()
    )

    def make_query_from_function(
            run_on_cluster=False,
            alt_syntax=False,
            remote=False,
            storage_type_as_arg=False,
            storage_type_in_named_collection=False,
            ):
        expr = get_creation_expression(
            storage_type,
            TABLE_NAME,
            started_cluster,
            table_function=True,
            run_on_cluster=run_on_cluster,
            storage_type_as_arg=storage_type_as_arg,
            storage_type_in_named_collection=storage_type_in_named_collection,
            cluster_name_as_literal=cluster_name_as_literal,
        )
        query_id = str(uuid.uuid4())
        settings = f"SETTINGS object_storage_cluster='cluster_simple'" if (alt_syntax and not run_on_cluster) else ""
        if remote:
            query = f"SELECT * FROM remote('node2', {expr}) {settings}"
        else:
            query = f"SELECT * FROM {expr} {settings}"
        responce = instance.query(query, query_id=query_id).strip().split()
        return responce, query_id

    # Cluster Query with node1 as coordinator
    select_cluster, query_id_cluster = make_query_from_function(run_on_cluster=True)

    # Cluster Query with node1 as coordinator with alternative syntax
    select_cluster_alt_syntax, query_id_cluster_alt_syntax = make_query_from_function(
        run_on_cluster=True,
        alt_syntax=True)

    # Cluster Query with node1 as coordinator and storage type as arg
    select_cluster_with_type_arg, query_id_cluster_with_type_arg = make_query_from_function(
        run_on_cluster=True,
        storage_type_as_arg=True,
    )

    # Cluster Query with node1 as coordinator and storage type in named collection
    select_cluster_with_type_in_nc, query_id_cluster_with_type_in_nc = make_query_from_function(
        run_on_cluster=True,
        storage_type_in_named_collection=True,
    )

    # Cluster Query with node1 as coordinator and storage type as arg, alternative syntax
    select_cluster_with_type_arg_alt_syntax, query_id_cluster_with_type_arg_alt_syntax = make_query_from_function(
        storage_type_as_arg=True,
        alt_syntax=True,
    )

    # Cluster Query with node1 as coordinator and storage type in named collection, alternative syntax
    select_cluster_with_type_in_nc_alt_syntax, query_id_cluster_with_type_in_nc_alt_syntax = make_query_from_function(
        storage_type_in_named_collection=True,
        alt_syntax=True,
    )

    #select_remote_cluster, _ = make_query_from_function(run_on_cluster=True, remote=True)

    def make_query_from_table(alt_syntax=False):
        query_id = str(uuid.uuid4())
        settings = "SETTINGS object_storage_cluster='cluster_simple'" if alt_syntax else ""
        responce = (
            instance.query(
                f"SELECT * FROM {TABLE_NAME} {settings}",
                query_id=query_id,
            )
            .strip()
            .split()
        )
        return responce, query_id

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, object_storage_cluster='cluster_simple')
    select_cluster_table_engine, query_id_cluster_table_engine = make_query_from_table()

    #select_remote_cluster = (
    #    instance.query(f"SELECT * FROM remote('node2',{table_function_expr_cluster})")
    #    .strip()
    #    .split()
    #)

    instance.query(f"DROP TABLE IF EXISTS `{TABLE_NAME}` SYNC")

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)
    select_pure_table_engine, query_id_pure_table_engine = make_query_from_table()
    select_pure_table_engine_cluster, query_id_pure_table_engine_cluster = make_query_from_table(alt_syntax=True)

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, storage_type_as_arg=True)
    select_pure_table_engine_with_type_arg, query_id_pure_table_engine_with_type_arg = make_query_from_table()
    select_pure_table_engine_cluster_with_type_arg, query_id_pure_table_engine_cluster_with_type_arg = make_query_from_table(alt_syntax=True)

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, storage_type_in_named_collection=True)
    select_pure_table_engine_with_type_in_nc, query_id_pure_table_engine_with_type_in_nc = make_query_from_table()
    select_pure_table_engine_cluster_with_type_in_nc, query_id_pure_table_engine_cluster_with_type_in_nc = make_query_from_table(alt_syntax=True)

    # Simple size check
    assert len(select_regular) == 600
    assert len(select_cluster) == 600
    assert len(select_cluster_alt_syntax) == 600
    assert len(select_cluster_table_engine) == 600
    #assert len(select_remote_cluster) == 600
    assert len(select_cluster_with_type_arg) == 600
    assert len(select_cluster_with_type_in_nc) == 600
    assert len(select_cluster_with_type_arg_alt_syntax) == 600
    assert len(select_cluster_with_type_in_nc_alt_syntax) == 600
    assert len(select_pure_table_engine) == 600
    assert len(select_pure_table_engine_cluster) == 600
    assert len(select_pure_table_engine_with_type_arg) == 600
    assert len(select_pure_table_engine_cluster_with_type_arg) == 600
    assert len(select_pure_table_engine_with_type_in_nc) == 600
    assert len(select_pure_table_engine_cluster_with_type_in_nc) == 600

    # Actual check
    assert select_cluster == select_regular
    assert select_cluster_alt_syntax == select_regular
    assert select_cluster_table_engine == select_regular
    #assert select_remote_cluster == select_regular
    assert select_cluster_with_type_arg == select_regular
    assert select_cluster_with_type_in_nc == select_regular
    assert select_cluster_with_type_arg_alt_syntax == select_regular
    assert select_cluster_with_type_in_nc_alt_syntax == select_regular
    assert select_pure_table_engine == select_regular
    assert select_pure_table_engine_cluster == select_regular
    assert select_pure_table_engine_with_type_arg == select_regular
    assert select_pure_table_engine_cluster_with_type_arg == select_regular
    assert select_pure_table_engine_with_type_in_nc == select_regular
    assert select_pure_table_engine_cluster_with_type_in_nc == select_regular

    # Check query_log
    for replica in started_cluster.instances.values():
        replica.query("SYSTEM FLUSH LOGS")

    count_secondary_subqueries(started_cluster, query_id_cluster, 1, "table function")
    count_secondary_subqueries(started_cluster, query_id_cluster_alt_syntax, 1, "table function alt syntax")
    count_secondary_subqueries(started_cluster, query_id_cluster_table_engine, 1, "cluster table engine")
    count_secondary_subqueries(started_cluster, query_id_cluster_with_type_arg, 1, "table function with storage type in args")
    count_secondary_subqueries(started_cluster, query_id_cluster_with_type_in_nc, 1, "table function with storage type in named collection")
    count_secondary_subqueries(started_cluster, query_id_cluster_with_type_arg_alt_syntax, 1, "table function with storage type in args alt syntax")
    count_secondary_subqueries(started_cluster, query_id_cluster_with_type_in_nc_alt_syntax, 1, "table function with storage type in named collection alt syntax")
    count_secondary_subqueries(started_cluster, query_id_pure_table_engine, 0, "table engine")
    count_secondary_subqueries(started_cluster, query_id_pure_table_engine_cluster, 1, "table engine with cluster setting")
    count_secondary_subqueries(started_cluster, query_id_pure_table_engine_with_type_arg, 0, "table engine with storage type in args")
    count_secondary_subqueries(started_cluster, query_id_pure_table_engine_cluster_with_type_arg, 1, "table engine with cluster setting with storage type in args")
    count_secondary_subqueries(started_cluster, query_id_pure_table_engine_with_type_in_nc, 0, "table engine with storage type in named collection")
    count_secondary_subqueries(started_cluster, query_id_pure_table_engine_cluster_with_type_in_nc, 1, "table engine with cluster setting with storage type in named collection")


    # Cluster Query with node1 as coordinator
    table_function_expr_cluster = get_creation_expression(
        storage_type,
        TABLE_NAME,
        started_cluster,
        table_function=True,
        run_on_cluster=True,
    )
    select_remote_cluster = (
        instance.query(f"SELECT * FROM remote('node2',{table_function_expr_cluster})")
        .strip()
        .split()
    )
    assert len(select_remote_cluster) == 600
    assert select_remote_cluster == select_regular

    select_remote_cluster = (
        instance.query(f"SELECT * FROM remote('node2',{table_function_expr_cluster})")
        .strip()
        .split()
    )
    assert len(select_remote_cluster) == 600
    assert select_remote_cluster == select_regular


@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_delete_files(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = (
        "test_delete_files_"
        + format_version
        + "_"
        + storage_type
        + "_"
        + get_uuid_str()
    )

    write_iceberg_from_df(
        spark,
        generate_data(spark, 0, 100),
        TABLE_NAME,
        mode="overwrite",
        format_version=format_version,
    )

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )
    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)

    # Test trivial count with deleted files
    query_id = "test_trivial_count_" + get_uuid_str()
    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}", query_id=query_id)) == 100
    instance.query("SYSTEM FLUSH LOGS")
    assert instance.query(f"SELECT ProfileEvents['IcebergTrivialCountOptimizationApplied'] FROM system.query_log where query_id = '{query_id}' and type = 'QueryFinish'") == "1\n"

    spark.sql(f"DELETE FROM {TABLE_NAME} WHERE a >= 0")
    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        "",
    )

    query_id = "test_trivial_count_" + get_uuid_str()
    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}", query_id=query_id)) == 0

    instance.query("SYSTEM FLUSH LOGS")
    assert instance.query(f"SELECT ProfileEvents['IcebergTrivialCountOptimizationApplied'] FROM system.query_log where query_id = '{query_id}' and type = 'QueryFinish'") == "1\n"

    write_iceberg_from_df(
        spark,
        generate_data(spark, 100, 200),
        TABLE_NAME,
        mode="upsert",
        format_version=format_version,
    )

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        "",
    )

    query_id = "test_trivial_count_" + get_uuid_str()
    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}", query_id=query_id)) == 100

    instance.query("SYSTEM FLUSH LOGS")
    assert instance.query(f"SELECT ProfileEvents['IcebergTrivialCountOptimizationApplied'] FROM system.query_log where query_id = '{query_id}' and type = 'QueryFinish'") == "1\n"

    spark.sql(f"DELETE FROM {TABLE_NAME} WHERE a >= 150")
    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        "",
    )

    query_id = "test_trivial_count_" + get_uuid_str()
    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}", query_id=query_id)) == 50

    instance.query("SYSTEM FLUSH LOGS")
    assert instance.query(f"SELECT ProfileEvents['IcebergTrivialCountOptimizationApplied'] FROM system.query_log where query_id = '{query_id}' and type = 'QueryFinish'") == "1\n"


def get_array(query_result: str):
    arr = sorted([int(x) for x in query_result.strip().split("\n")])
    print(arr)
    return arr


@pytest.mark.parametrize("use_roaring_bitmaps", [0, 1])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_position_deletes(started_cluster, use_roaring_bitmaps,  storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_position_deletes_" + storage_type + "_" + get_uuid_str()
    instance.query(f"SET use_roaring_bitmap_iceberg_positional_deletes={use_roaring_bitmaps};")

    spark.sql(
        f"""
        CREATE TABLE {TABLE_NAME} (id bigint, data string) USING iceberg PARTITIONED BY (bucket(5, id)) TBLPROPERTIES ('format-version' = '2', 'write.update.mode'=
        'merge-on-read', 'write.delete.mode'='merge-on-read', 'write.merge.mode'='merge-on-read')
        """
    )
    spark.sql(f"INSERT INTO {TABLE_NAME} select id, char(id + ascii('a')) from range(10, 100)")

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)

    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 90

    spark.sql(f"DELETE FROM {TABLE_NAME} WHERE id < 20")
    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    assert get_array(instance.query(f"SELECT id FROM {TABLE_NAME}")) == list(range(20, 100))

    # Check that filters are applied after deletes
    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME} where id >= 15")) == 80
    assert (
        int(
            instance.query(
                f"SELECT count() FROM {TABLE_NAME} where id >= 15 SETTINGS optimize_trivial_count_query=1"
            )
        )
        == 80
    )

    # Check deletes after deletes
    spark.sql(f"DELETE FROM {TABLE_NAME} WHERE id >= 90")
    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )
    assert get_array(instance.query(f"SELECT id FROM {TABLE_NAME}")) == list(range(20, 90))

    spark.sql(f"ALTER TABLE {TABLE_NAME} ADD PARTITION FIELD truncate(1, data)")

    # Check adds after deletes
    spark.sql(
        f"INSERT INTO {TABLE_NAME} select id, char(id + ascii('a')) from range(100, 200)"
    )
    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )
    assert get_array(instance.query(f"SELECT id FROM {TABLE_NAME}")) == list(range(20, 90)) + list(
        range(100, 200)
    )

    # Check deletes after adds
    spark.sql(f"DELETE FROM {TABLE_NAME} WHERE id >= 150")
    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )
    assert get_array(instance.query(f"SELECT id FROM {TABLE_NAME}")) == list(range(20, 90)) + list(
        range(100, 150)
    )

    assert get_array(
        instance.query(
            f"SELECT id FROM {TABLE_NAME} WHERE id = 70 SETTINGS use_iceberg_partition_pruning = 1"
        )
    ) == [70]

    # Clean up
    instance.query(f"DROP TABLE {TABLE_NAME}")


@pytest.mark.parametrize("use_roaring_bitmaps", [0, 1])
def test_position_deletes_out_of_order(started_cluster, use_roaring_bitmaps):
    storage_type = "local"
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_position_deletes_out_of_order_" + get_uuid_str()
    instance.query(f"SET use_roaring_bitmap_iceberg_positional_deletes={use_roaring_bitmaps};")
    instance.query(f"SET input_format_parquet_use_native_reader_v3=1;")

    # There are a few flaky hacks chained together here.
    # We want the parquet reader to produce chunks corresponding to row groups out of order if
    # `format_settings.parquet.preserve_order` wasn't enabled. For that we:
    #  * Use `PREWHERE NOT sleepEachRow(...)` to make the reader take longer for bigger row groups.
    #  * Set spark row group size limit to 1 byte. Rely on current spark implementation detail:
    #    it'll check this limit every 100 rows. So effectively we've set row group size to 100 rows.
    #  * Insert 105 rows. So the first row group will have 100 rows, the second 5 rows.
    # If one of these steps breaks in future, this test will be less effective but won't fail.

    spark.sql(
        f"""
        CREATE TABLE {TABLE_NAME} (id bigint, data string) USING iceberg TBLPROPERTIES ('format-version' = '2', 'write.update.mode'='merge-on-read', 'write.delete.mode'='merge-on-read', 'write.merge.mode'='merge-on-read', 'write.parquet.row-group-size-bytes'='1')
        """
    )
    spark.sql(f"INSERT INTO {TABLE_NAME} select /*+ COALESCE(1) */ id, char(id + ascii('a')) from range(0, 105)")
    # (Fun fact: if you replace these two queries with one query with `WHERE id < 10 OR id = 103`,
    #  spark either quetly fails to delete row 103 or outright crashes with segfault in jre.)
    spark.sql(f"DELETE FROM {TABLE_NAME} WHERE id < 10")
    spark.sql(f"DELETE FROM {TABLE_NAME} WHERE id = 103")

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, additional_settings=["input_format_parquet_use_native_reader_v3=1", f"use_roaring_bitmap_iceberg_positional_deletes={use_roaring_bitmaps}"])

    # TODO: Replace WHERE with PREWHERE when we add prewhere support for datalakes.
    assert get_array(instance.query(f"SELECT id FROM {TABLE_NAME} WHERE NOT sleepEachRow(1/100) order by id")) == list(range(10, 103)) + [104]

    instance.query(f"DROP TABLE {TABLE_NAME}")

@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_schema_inference(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    for format in ["Parquet", "ORC", "Avro"]:
        TABLE_NAME = (
            "test_schema_inference_"
            + format
            + "_"
            + format_version
            + "_"
            + storage_type
            + "_"
            + get_uuid_str()
        )

        # Types time, timestamptz, fixed are not supported in Spark.
        spark.sql(
            f"CREATE TABLE {TABLE_NAME} (intC int, longC long, floatC float, doubleC double, decimalC1 decimal(10, 3), decimalC2 decimal(20, 10), decimalC3 decimal(38, 30), dateC date,  timestampC timestamp, stringC string, binaryC binary, arrayC1 array<int>, mapC1 map<string, string>, structC1 struct<field1: int, field2: string>, complexC array<struct<field1: map<string, array<map<string, int>>>, field2: struct<field3: int, field4: string>>>) USING iceberg TBLPROPERTIES ('format-version' = '{format_version}', 'write.format.default' = '{format}')"
        )

        spark.sql(
            f"insert into {TABLE_NAME} select 42, 4242, 42.42, 4242.4242, decimal(42.42), decimal(42.42), decimal(42.42), date('2020-01-01'), timestamp('2020-01-01 20:00:00'), 'hello', binary('hello'), array(1,2,3), map('key', 'value'), struct(42, 'hello'), array(struct(map('key', array(map('key', 42))), struct(42, 'hello')))"
        )
        default_upload_directory(
            started_cluster,
            storage_type,
            f"/iceberg_data/default/{TABLE_NAME}/",
            f"/iceberg_data/default/{TABLE_NAME}/",
        )

        create_iceberg_table(
            storage_type, instance, TABLE_NAME, started_cluster, format=format
        )

        res = instance.query(
            f"DESC {TABLE_NAME} FORMAT TSVRaw", settings={"print_pretty_type_names": 0}
        )
        expected = TSV(
            [
                ["intC", "Nullable(Int32)"],
                ["longC", "Nullable(Int64)"],
                ["floatC", "Nullable(Float32)"],
                ["doubleC", "Nullable(Float64)"],
                ["decimalC1", "Nullable(Decimal(10, 3))"],
                ["decimalC2", "Nullable(Decimal(20, 10))"],
                ["decimalC3", "Nullable(Decimal(38, 30))"],
                ["dateC", "Nullable(Date)"],
                ["timestampC", "Nullable(DateTime64(6, 'UTC'))"],
                ["stringC", "Nullable(String)"],
                ["binaryC", "Nullable(String)"],
                ["arrayC1", "Array(Nullable(Int32))"],
                ["mapC1", "Map(String, Nullable(String))"],
                ["structC1", "Tuple(field1 Nullable(Int32), field2 Nullable(String))"],
                [
                    "complexC",
                    "Array(Tuple(field1 Map(String, Array(Map(String, Nullable(Int32)))), field2 Tuple(field3 Nullable(Int32), field4 Nullable(String))))",
                ],
            ]
        )

        assert res == expected

        # Check that we can parse data
        instance.query(f"SELECT * FROM {TABLE_NAME}")


@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_explanation(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    for format in ["Parquet", "ORC", "Avro"]:
        TABLE_NAME = (
            "test_explanation_"
            + format
            + "_"
            + format_version
            + "_"
            + storage_type
            + "_"
            + get_uuid_str()
        )

        # Types time, timestamptz, fixed are not supported in Spark.
        spark.sql(
            f"CREATE TABLE {TABLE_NAME} (x int) USING iceberg TBLPROPERTIES ('format-version' = '{format_version}', 'write.format.default' = '{format}')"
        )

        spark.sql(f"insert into {TABLE_NAME} select 42")
        default_upload_directory(
            started_cluster,
            storage_type,
            f"/iceberg_data/default/{TABLE_NAME}/",
            f"/iceberg_data/default/{TABLE_NAME}/",
        )

        create_iceberg_table(
            storage_type, instance, TABLE_NAME, started_cluster, format=format
        )

        res = instance.query(f"EXPLAIN SELECT * FROM {TABLE_NAME}")
        res = list(
            map(
                lambda x: x.split("\t"),
                filter(lambda x: len(x) > 0, res.strip().split("\n")),
            )
        )

        expected = [
            [
                "Expression ((Project names + (Projection + Change column names to column identifiers)))"
            ],
            [f"  ReadFromObjectStorage"],
        ]

        assert res == expected

        # Check that we can parse data
        instance.query(f"SELECT * FROM {TABLE_NAME}")


@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_metadata_file_selection(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = (
        "test_metadata_selection_"
        + format_version
        + "_"
        + storage_type
        + "_"
        + get_uuid_str()
    )

    spark.sql(
        f"CREATE TABLE {TABLE_NAME} (id bigint, data string) USING iceberg TBLPROPERTIES ('format-version' = '2', 'write.update.mode'='merge-on-read', 'write.delete.mode'='merge-on-read', 'write.merge.mode'='merge-on-read')"
    )

    for i in range(50):
        spark.sql(
            f"INSERT INTO {TABLE_NAME} select id, char(id + ascii('a')) from range(10)"
        )

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)

    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 500

@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_metadata_file_format_with_uuid(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = (
        "test_metadata_selection_with_uuid_"
        + format_version
        + "_"
        + storage_type
        + "_"
        + get_uuid_str()
    )

    spark.sql(
        f"CREATE TABLE {TABLE_NAME} (id bigint, data string) USING iceberg TBLPROPERTIES ('format-version' = '2', 'write.update.mode'='merge-on-read', 'write.delete.mode'='merge-on-read', 'write.merge.mode'='merge-on-read')"
    )

    for i in range(50):
        spark.sql(
            f"INSERT INTO {TABLE_NAME} select id, char(id + ascii('a')) from range(10)"
        )

    for i in range(50):
        os.rename(
            f"/iceberg_data/default/{TABLE_NAME}/metadata/v{i + 1}.metadata.json",
            f"/iceberg_data/default/{TABLE_NAME}/metadata/{str(i).zfill(5)}-{get_uuid_str()}.metadata.json",
        )

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)

    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 500


@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_metadata_file_selection_from_version_hint(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = (
        "test_metadata_file_selection_from_version_hint_"
        + format_version
        + "_"
        + storage_type
        + "_"
        + get_uuid_str()
    )

    spark.sql(
        f"CREATE TABLE {TABLE_NAME} (id bigint, data string) USING iceberg TBLPROPERTIES ('format-version' = '2', 'write.update.mode'='merge-on-read', 'write.delete.mode'='merge-on-read', 'write.merge.mode'='merge-on-read')"
    )

    for i in range(10):
        spark.sql(
            f"INSERT INTO {TABLE_NAME} select id, char(id + ascii('a')) from range(10)"
        )

    # test the case where version_hint.text file contains just the version number
    with open(f"/iceberg_data/default/{TABLE_NAME}/metadata/version-hint.text", "w") as f:
        f.write('5')

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, use_version_hint=True)

    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 40

    # test the case where version_hint.text file contains the whole metadata file name
    with open(f"/iceberg_data/default/{TABLE_NAME}/metadata/version-hint.text", "w") as f:
        f.write('v3.metadata.json')

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, use_version_hint=True)

    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 20


def test_restart_broken_s3(started_cluster):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_restart_broken_table_function_s3" + "_" + get_uuid_str()

    minio_client = started_cluster.minio_client
    bucket = "broken2"

    if not minio_client.bucket_exists(bucket):
        minio_client.make_bucket(bucket)

    write_iceberg_from_df(
        spark,
        generate_data(spark, 0, 100),
        TABLE_NAME,
        mode="overwrite",
        format_version="1",
    )

    files = default_upload_directory(
        started_cluster,
        "s3",
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
        bucket=bucket,
    )
    create_iceberg_table("s3", instance, TABLE_NAME, started_cluster, bucket=bucket)
    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 100

    s3_objects = list_s3_objects(minio_client, bucket, prefix="")
    assert (
        len(
            list(
                minio_client.remove_objects(
                    bucket,
                    [DeleteObject(obj) for obj in s3_objects],
                )
            )
        )
        == 0
    )
    minio_client.remove_bucket(bucket)

    instance.restart_clickhouse()

    assert "NoSuchBucket" in instance.query_and_get_error(
        f"SELECT count() FROM {TABLE_NAME}"
    )

    minio_client.make_bucket(bucket)

    files = default_upload_directory(
        started_cluster,
        "s3",
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
        bucket=bucket,
    )

    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 100
    instance.query(f"DROP TABLE {TABLE_NAME}")


@pytest.mark.parametrize("storage_type", ["s3"])
def test_filesystem_cache(started_cluster, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_filesystem_cache_" + storage_type + "_" + get_uuid_str()

    write_iceberg_from_df(
        spark,
        generate_data(spark, 0, 10),
        TABLE_NAME,
        mode="overwrite",
        format_version="1",
        partition_by="a",
    )

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)

    query_id = f"{TABLE_NAME}-{uuid.uuid4()}"
    instance.query(
        f"SELECT * FROM {TABLE_NAME} SETTINGS filesystem_cache_name = 'cache1'",
        query_id=query_id,
    )

    instance.query("SYSTEM FLUSH LOGS")

    written_to_cache_first_select = int(
        instance.query(
            f"SELECT ProfileEvents['CachedReadBufferCacheWriteBytes'] FROM system.query_log WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
        )
    )

    read_from_cache_first_select = int(
        instance.query(
            f"SELECT ProfileEvents['CachedReadBufferReadFromCacheBytes'] FROM system.query_log WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
        )
    )

    assert 0 < int(
        instance.query(
            f"SELECT ProfileEvents['S3GetObject'] FROM system.query_log WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
        )
    )

    query_id = f"{TABLE_NAME}-{uuid.uuid4()}"
    instance.query(
        f"SELECT * FROM {TABLE_NAME} SETTINGS filesystem_cache_name = 'cache1'",
        query_id=query_id,
    )

    instance.query("SYSTEM FLUSH LOGS")

    read_from_cache_second_select = int(
        instance.query(
            f"SELECT ProfileEvents['CachedReadBufferReadFromCacheBytes'] FROM system.query_log WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
        )
    )

    assert (
        read_from_cache_second_select
        == read_from_cache_first_select + written_to_cache_first_select
    )

    assert 0 == int(
        instance.query(
            f"SELECT ProfileEvents['S3GetObject'] FROM system.query_log WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
        )
    )

@pytest.mark.parametrize(
    "storage_type, run_on_cluster",
    [("s3", False), ("s3", True), ("azure", False), ("azure", True), ("local", False), ("local", True)],
)
def test_partition_pruning(started_cluster, storage_type, run_on_cluster):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_partition_pruning_" + storage_type + "_" + get_uuid_str()

    def execute_spark_query(query: str):
        return execute_spark_query_general(
            spark,
            started_cluster,
            storage_type,
            TABLE_NAME,
            query,
            additional_nodes=["node2", "node3"] if storage_type=="local" else [],
        )

    execute_spark_query(
        f"""
            CREATE TABLE {TABLE_NAME} (
                tag INT,
                date DATE,
                date2 DATE,
                ts TIMESTAMP,
                ts2 TIMESTAMP,
                time_struct struct<a : DATE, b : TIMESTAMP>,
                name VARCHAR(50),
                number BIGINT
            )
            USING iceberg
            PARTITIONED BY (identity(tag), days(date), years(date2), hours(ts), months(ts2), TRUNCATE(3, name), TRUNCATE(3, number))
            OPTIONS('format-version'='2')
        """
    )

    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (1, DATE '2024-01-20', DATE '2024-01-20',
        TIMESTAMP '2024-02-20 10:00:00', TIMESTAMP '2024-02-20 10:00:00', named_struct('a', DATE '2024-01-20', 'b', TIMESTAMP '2024-02-20 10:00:00'), 'vasya', 5),
        (2, DATE '2024-01-30', DATE '2024-01-30',
        TIMESTAMP '2024-03-20 15:00:00', TIMESTAMP '2024-03-20 15:00:00', named_struct('a', DATE '2024-03-20', 'b', TIMESTAMP '2024-03-20 14:00:00'), 'vasilisa', 6),
        (1, DATE '2024-02-20', DATE '2024-02-20',
        TIMESTAMP '2024-03-20 20:00:00', TIMESTAMP '2024-03-20 20:00:00', named_struct('a', DATE '2024-02-20', 'b', TIMESTAMP '2024-02-20 10:00:00'), 'iceberg', 7),
        (2, DATE '2025-01-20', DATE '2025-01-20',
        TIMESTAMP '2024-04-30 14:00:00', TIMESTAMP '2024-04-30 14:00:00', named_struct('a', DATE '2024-04-30', 'b', TIMESTAMP '2024-04-30 14:00:00'), 'icebreaker', 8);
    """
    )

    creation_expression = get_creation_expression(
        storage_type, TABLE_NAME, started_cluster, table_function=True, run_on_cluster=run_on_cluster
    )

    def check_validity_and_get_prunned_files(select_expression):
        settings1 = {
            "use_iceberg_partition_pruning": 0
        }
        settings2 = {
            "use_iceberg_partition_pruning": 1
        }
        return check_validity_and_get_prunned_files_general(
            instance, TABLE_NAME, settings1, settings2, 'IcebergPartitionPrunedFiles', select_expression
        )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} ORDER BY ALL"
        )
        == 0
    )
    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE date <= '2024-01-25' ORDER BY ALL"
        )
        == 3
    )
    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE date2 <= '2024-01-25' ORDER BY ALL"
        )
        == 1
    )
    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE ts <= timestamp('2024-03-20 14:00:00.000000') ORDER BY ALL"
        )
        == 3
    )
    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE ts2 <= timestamp('2024-03-20 14:00:00.000000') ORDER BY ALL"
        )
        == 1
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE tag == 1 ORDER BY ALL"
        )
        == 2
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE tag <= 1 ORDER BY ALL"
        )
        == 2
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE name == 'vasilisa' ORDER BY ALL"
        )
        == 2
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE name < 'kek' ORDER BY ALL"
        )
        == 2
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE number == 8 ORDER BY ALL"
        )
        == 1
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE number <= 5 ORDER BY ALL"
        )
        == 3
    )

    execute_spark_query(f"ALTER TABLE {TABLE_NAME} RENAME COLUMN date TO date3")

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE date3 <= '2024-01-25' ORDER BY ALL"
        )
        == 3
    )

    execute_spark_query(f"ALTER TABLE {TABLE_NAME} ALTER COLUMN tag TYPE BIGINT")

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE tag <= 1 ORDER BY ALL"
        )
        == 2
    )

    execute_spark_query(f"ALTER TABLE {TABLE_NAME} ADD PARTITION FIELD time_struct.a")

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE time_struct.a <= '2024-02-01' ORDER BY ALL"
        )
        == 0
    )

    execute_spark_query(
        f"INSERT INTO {TABLE_NAME} VALUES (1, DATE '2024-01-20', DATE '2024-01-20', TIMESTAMP '2024-02-20 10:00:00', TIMESTAMP '2024-02-20 10:00:00', named_struct('a', DATE '2024-03-15', 'b', TIMESTAMP '2024-02-20 10:00:00'), 'kek', 10)"
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE time_struct.a <= '2024-02-01' ORDER BY ALL"
        )
        == 1
    )


@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_schema_evolution_with_time_travel(
    started_cluster, format_version, storage_type
):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = (
        "test_schema_evolution_with_time_travel_"
        + format_version
        + "_"
        + storage_type
        + "_"
        + get_uuid_str()
    )

    def execute_spark_query(query: str):
        return execute_spark_query_general(
            spark,
            started_cluster,
            storage_type,
            TABLE_NAME,
            query,
        )

    execute_spark_query(
        f"""
            DROP TABLE IF EXISTS {TABLE_NAME};
        """
    )

    execute_spark_query(
        f"""
            CREATE TABLE IF NOT EXISTS {TABLE_NAME} (
                a int NOT NULL
            )
            USING iceberg
            OPTIONS ('format-version'='{format_version}')
        """
    )

    table_creation_expression = get_creation_expression(
        storage_type,
        TABLE_NAME,
        started_cluster,
        table_function=True,
        allow_dynamic_metadata_for_data_lakes=True,
    )

    table_select_expression =  table_creation_expression

    check_schema_and_data(
        instance,
        table_select_expression,
        [
            ["a", "Int32"]
        ],
        [],
    )

    first_timestamp_ms = int(datetime.now().timestamp() * 1000)

    time.sleep(0.5)

    execute_spark_query(
        f"""
            INSERT INTO {TABLE_NAME} VALUES (4);
        """
    )

    check_schema_and_data(
        instance,
        table_select_expression,
        [
            ["a", "Int32"],
        ],
        [["4"]],
    )

    error_message = instance.query_and_get_error(f"SELECT * FROM {table_select_expression} ORDER BY ALL SETTINGS iceberg_timestamp_ms = {first_timestamp_ms}")
    assert "No snapshot found in snapshot log before requested timestamp" in error_message

    second_timestamp_ms = int(datetime.now().timestamp() * 1000)

    time.sleep(0.5)

    execute_spark_query(
        f"""
            ALTER TABLE {TABLE_NAME} ADD COLUMNS (
                b double
            );
        """
    )

    check_schema_and_data(
        instance,
        table_select_expression,
        [
            ["a", "Int32"],
            ["b", "Nullable(Float64)"]
        ],
        [["4", "\\N"]],
    )

    check_schema_and_data(
        instance,
        table_select_expression,
        [
            ["a", "Int32"],
        ],
        [["4"]],
        timestamp_ms=second_timestamp_ms,
    )

    third_timestamp_ms = int(datetime.now().timestamp() * 1000)

    time.sleep(0.5)

    execute_spark_query(
        f"""
            INSERT INTO {TABLE_NAME} VALUES (7, 5.0);
        """
    )

    check_schema_and_data(
        instance,
        table_select_expression,
        [
            ["a", "Int32"],
            ["b", "Nullable(Float64)"]
        ],
        [["4", "\\N"], ["7", "5"]],
    )

    check_schema_and_data(
        instance,
        table_select_expression,
        [
            ["a", "Int32"],
        ],
        [["4"]],
        timestamp_ms=second_timestamp_ms,
    )

    check_schema_and_data(
        instance,
        table_select_expression,
        [
            ["a", "Int32"],        ],
        [["4"]],
        timestamp_ms=third_timestamp_ms,
    )

    execute_spark_query(
        f"""
            ALTER TABLE {TABLE_NAME} ADD COLUMNS (
                c double
            );
        """
    )

    time.sleep(0.5)
    fourth_timestamp_ms = int(datetime.now().timestamp() * 1000)

    check_schema_and_data(
        instance,
        table_select_expression,
        [
            ["a", "Int32"],
            ["b", "Nullable(Float64)"]
        ],
        [["4", "\\N"], ["7", "5"]],
        timestamp_ms=fourth_timestamp_ms,
    )

    check_schema_and_data(
        instance,
        table_select_expression,
        [
            ["a", "Int32"],
            ["b", "Nullable(Float64)"],
            ["c", "Nullable(Float64)"]
        ],
        [["4", "\\N", "\\N"], ["7", "5", "\\N"]],
    )

@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_iceberg_snapshot_reads(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = (
        "test_iceberg_snapshot_reads"
        + format_version
        + "_"
        + storage_type
        + "_"
        + get_uuid_str()
    )

    write_iceberg_from_df(
        spark,
        generate_data(spark, 0, 100),
        TABLE_NAME,
        mode="overwrite",
        format_version=format_version,
    )
    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        "",
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)
    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 100
    snapshot1_timestamp = datetime.now(timezone.utc)
    snapshot1_id = get_last_snapshot(f"/iceberg_data/default/{TABLE_NAME}/")
    time.sleep(0.1)

    write_iceberg_from_df(
        spark,
        generate_data(spark, 100, 200),
        TABLE_NAME,
        mode="append",
        format_version=format_version,
    )
    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        "",
    )
    snapshot2_timestamp = datetime.now(timezone.utc)
    snapshot2_id = get_last_snapshot(f"/iceberg_data/default/{TABLE_NAME}/")
    time.sleep(0.1)

    write_iceberg_from_df(
        spark,
        generate_data(spark, 200, 300),
        TABLE_NAME,
        mode="append",
        format_version=format_version,
    )
    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        "",
    )
    snapshot3_timestamp = datetime.now(timezone.utc)
    snapshot3_id = get_last_snapshot(f"/iceberg_data/default/{TABLE_NAME}/")
    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 300
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY 1") == instance.query(
        "SELECT number, toString(number + 1) FROM numbers(300)"
    )

    # Validate that each snapshot timestamp only sees the data inserted by that time.
    assert (
        instance.query(
            f"""
                          SELECT * FROM {TABLE_NAME} ORDER BY 1
                          SETTINGS iceberg_timestamp_ms = {int(snapshot1_timestamp.timestamp() * 1000)}"""
        )
        == instance.query("SELECT number, toString(number + 1) FROM numbers(100)")
    )

    assert (
        instance.query(
            f"""
                          SELECT * FROM {TABLE_NAME} ORDER BY 1
                          SETTINGS iceberg_snapshot_id = {snapshot1_id}"""
        )
        == instance.query("SELECT number, toString(number + 1) FROM numbers(100)")
    )


    assert (
        instance.query(
            f"""
                          SELECT * FROM {TABLE_NAME} ORDER BY 1
                          SETTINGS iceberg_timestamp_ms = {int(snapshot2_timestamp.timestamp() * 1000)}"""
        )
        == instance.query("SELECT number, toString(number + 1) FROM numbers(200)")
    )

    assert (
        instance.query(
            f"""
                          SELECT * FROM {TABLE_NAME} ORDER BY 1
                          SETTINGS iceberg_snapshot_id = {snapshot2_id}"""
        )
        == instance.query("SELECT number, toString(number + 1) FROM numbers(200)")
    )


    assert (
        instance.query(
            f"""SELECT * FROM {TABLE_NAME} ORDER BY 1
                          SETTINGS iceberg_timestamp_ms = {int(snapshot3_timestamp.timestamp() * 1000)}"""
        )
        == instance.query("SELECT number, toString(number + 1) FROM numbers(300)")
    )

    assert (
        instance.query(
            f"""
                          SELECT * FROM {TABLE_NAME} ORDER BY 1
                          SETTINGS iceberg_snapshot_id = {snapshot3_id}"""
        )
        == instance.query("SELECT number, toString(number + 1) FROM numbers(300)")
    )


@pytest.mark.parametrize("storage_type", ["s3", "azure"])
def test_metadata_cache(started_cluster, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_metadata_cache_" + storage_type + "_" + get_uuid_str()

    write_iceberg_from_df(
        spark,
        generate_data(spark, 0, 10),
        TABLE_NAME,
        mode="overwrite",
        format_version="1",
        partition_by="a",
    )

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    table_expr = get_creation_expression(storage_type, TABLE_NAME, started_cluster, table_function=True)

    query_id = f"{TABLE_NAME}-{uuid.uuid4()}"
    instance.query(
        f"SELECT * FROM {table_expr}", query_id=query_id,
    )

    instance.query("SYSTEM FLUSH LOGS")

    assert 0 < int(
        instance.query(
            f"SELECT ProfileEvents['IcebergMetadataFilesCacheMisses'] FROM system.query_log WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
        )
    )

    query_id = f"{TABLE_NAME}-{uuid.uuid4()}"
    instance.query(
        f"SELECT * FROM {table_expr}",
        query_id=query_id,
    )

    instance.query("SYSTEM FLUSH LOGS")

    assert 0 == int(
        instance.query(
            f"SELECT ProfileEvents['IcebergMetadataFilesCacheMisses'] FROM system.query_log WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
        )
    )

    assert 0 < int(
        instance.query(
            f"SELECT ProfileEvents['IcebergMetadataFilesCacheHits'] FROM system.query_log WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
        )
    )

    instance.query("SYSTEM DROP ICEBERG METADATA CACHE")

    query_id = f"{TABLE_NAME}-{uuid.uuid4()}"
    instance.query(
        f"SELECT * FROM {table_expr}", query_id=query_id,
    )

    instance.query("SYSTEM FLUSH LOGS")

    assert 0 < int(
        instance.query(
            f"SELECT ProfileEvents['IcebergMetadataFilesCacheMisses'] FROM system.query_log WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
        )
    )

    query_id = f"{TABLE_NAME}-{uuid.uuid4()}"
    instance.query(
        f"SELECT * FROM {table_expr}",
        query_id=query_id,
        settings={"use_iceberg_metadata_files_cache":"0"},
    )

    instance.query("SYSTEM FLUSH LOGS")
    assert "0\t0\n" == instance.query(
            f"SELECT ProfileEvents['IcebergMetadataFilesCacheHits'], ProfileEvents['IcebergMetadataFilesCacheMisses'] FROM system.query_log WHERE query_id = '{query_id}' AND type = 'QueryFinish'",
        )


@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
@pytest.mark.parametrize("is_table_function", [False, True])
def test_minmax_pruning(started_cluster, storage_type, is_table_function):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_minmax_pruning_" + storage_type + "_" + get_uuid_str()

    def execute_spark_query(query: str):
        return execute_spark_query_general(
            spark,
            started_cluster,
            storage_type,
            TABLE_NAME,
            query,
        )

    execute_spark_query(
        f"""
            CREATE TABLE {TABLE_NAME} (
                tag INT,
                date DATE,
                ts TIMESTAMP,
                time_struct struct<a : DATE, b : TIMESTAMP>,
                name VARCHAR(50),
                number BIGINT
            )
            USING iceberg
            OPTIONS('format-version'='2')
        """
    )

    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (1, DATE '2024-01-20',
        TIMESTAMP '2024-02-20 10:00:00', named_struct('a', DATE '2024-01-20', 'b', TIMESTAMP '2024-02-20 10:00:00'), 'vasya', 5)
    """
    )

    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (2, DATE '2024-02-20',
        TIMESTAMP '2024-03-20 15:00:00', named_struct('a', DATE '2024-02-20', 'b', TIMESTAMP '2024-03-20 14:00:00'), 'vasilisa', 6)
    """
    )

    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (3, DATE '2025-03-20',
        TIMESTAMP '2024-04-30 14:00:00', named_struct('a', DATE '2024-03-20', 'b', TIMESTAMP '2024-04-30 14:00:00'), 'icebreaker', 7)
    """
    )
    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (4, DATE '2025-04-20',
        TIMESTAMP '2024-05-30 14:00:00', named_struct('a', DATE '2024-04-20', 'b', TIMESTAMP '2024-05-30 14:00:00'), 'iceberg', 8)
    """
    )

    if is_table_function:
        creation_expression = get_creation_expression(
        storage_type, TABLE_NAME, started_cluster, table_function=True
    )
    else:
        instance.query(get_creation_expression(
            storage_type, TABLE_NAME, started_cluster, table_function=False
        ))
        creation_expression = TABLE_NAME

    def check_validity_and_get_prunned_files(select_expression):
        settings1 = {
            "use_iceberg_partition_pruning": 0,
            "input_format_parquet_bloom_filter_push_down": 0,
            "input_format_parquet_filter_push_down": 0,
        }
        settings2 = {
            "use_iceberg_partition_pruning": 1,
            "input_format_parquet_bloom_filter_push_down": 0,
            "input_format_parquet_filter_push_down": 0,
        }
        return check_validity_and_get_prunned_files_general(
            instance, TABLE_NAME, settings1, settings2, 'IcebergMinMaxIndexPrunedFiles', select_expression
        )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} ORDER BY ALL"
        )
        == 0
    )
    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE date <= '2024-01-25' ORDER BY ALL"
        )
        == 3
    )
    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE ts <= timestamp('2024-03-20 14:00:00.000000') ORDER BY ALL"
        )
        == 3
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE tag == 1 ORDER BY ALL"
        )
        == 3
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE tag <= 1 ORDER BY ALL"
        )
        == 3
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE name == 'vasilisa' ORDER BY ALL"
        )
        == 3
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE name < 'kek' ORDER BY ALL"
        )
        == 2
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE number == 8 ORDER BY ALL"
        )
        == 3
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE number <= 5 ORDER BY ALL"
        )
        == 3
    )

    if not is_table_function:
        return

    execute_spark_query(f"ALTER TABLE {TABLE_NAME} RENAME COLUMN date TO date3")

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE date3 <= '2024-01-25' ORDER BY ALL"
        )
        == 3
    )

    execute_spark_query(f"ALTER TABLE {TABLE_NAME} ALTER COLUMN tag TYPE BIGINT")

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE tag <= 1 ORDER BY ALL"
        )
        == 3
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE time_struct.a <= '2024-02-01' ORDER BY ALL"
        )
        == 3
    )

    execute_spark_query(
        f"INSERT INTO {TABLE_NAME} VALUES (1, DATE '2024-01-20', TIMESTAMP '2024-02-20 10:00:00', named_struct('a', DATE '2024-03-15', 'b', TIMESTAMP '2024-02-20 10:00:00'), 'kek', 10)"
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE time_struct.a <= '2024-02-01' ORDER BY ALL"
        )
        == 4
    )

    execute_spark_query(f"ALTER TABLE {TABLE_NAME} ADD COLUMNS (ddd decimal(10, 3))")

    execute_spark_query(
        f"INSERT INTO {TABLE_NAME} VALUES (1, DATE '2024-01-20', TIMESTAMP '2024-02-20 10:00:00', named_struct('a', DATE '2024-03-15', 'b', TIMESTAMP '2024-02-20 10:00:00'), 'kek', 30, decimal(17.22))"
    )

    execute_spark_query(
        f"INSERT INTO {TABLE_NAME} VALUES (1, DATE '2024-01-20', TIMESTAMP '2024-02-20 10:00:00', named_struct('a', DATE '2024-03-15', 'b', TIMESTAMP '2024-02-20 10:00:00'), 'kek', 10, decimal(14311.772))"
    )

    execute_spark_query(
        f"INSERT INTO {TABLE_NAME} VALUES (1, DATE '2024-01-20', TIMESTAMP '2024-02-20 10:00:00', named_struct('a', DATE '2024-03-15', 'b', TIMESTAMP '2024-02-20 10:00:00'), 'kek', 10, decimal(-8888.999))"
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE ddd >= 100 ORDER BY ALL"
        )
        == 2
    )
    # Spark store rounded values of decimals, this query checks that we work it around.
    # Please check the code where we parse lower bounds and upper bounds
    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE ddd >= toDecimal64('17.21', 3) ORDER BY ALL"
        )
        == 1
    )


@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_explicit_metadata_file(started_cluster, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = (
        "test_explicit_metadata_file_"
        + storage_type
        + "_"
        + get_uuid_str()
    )

    spark.sql(
        f"CREATE TABLE {TABLE_NAME} (id bigint, data string) USING iceberg TBLPROPERTIES ('format-version' = '2', 'write.update.mode'='merge-on-read', 'write.delete.mode'='merge-on-read', 'write.merge.mode'='merge-on-read')"
    )

    for i in range(50):
        spark.sql(
            f"INSERT INTO {TABLE_NAME} select id, char(id + ascii('a')) from range(10)"
        )

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, explicit_metadata_path="")

    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 500

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, explicit_metadata_path="metadata/v31.metadata.json")

    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 300

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, explicit_metadata_path="metadata/v11.metadata.json")

    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 100

    with pytest.raises(Exception):
        create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, explicit_metadata_path=chr(0) + chr(1))
    with pytest.raises(Exception):
        create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, explicit_metadata_path="../metadata/v11.metadata.json")


@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
@pytest.mark.parametrize("run_on_cluster", [False, True])
def test_minmax_pruning_with_null(started_cluster, storage_type, run_on_cluster):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_minmax_pruning_with_null" + storage_type + "_" + get_uuid_str()

    def execute_spark_query(query: str):
        return execute_spark_query_general(
            spark,
            started_cluster,
            storage_type,
            TABLE_NAME,
            query,
            additional_nodes=["node2", "node3"] if storage_type=="local" else [],
        )

    execute_spark_query(
        f"""
            CREATE TABLE {TABLE_NAME} (
                tag INT,
                date DATE,
                ts TIMESTAMP,
                time_struct struct<a : DATE, b : TIMESTAMP>,
                name VARCHAR(50),
                number BIGINT
            )
            USING iceberg
            OPTIONS('format-version'='2')
        """
    )

    # min-max value of time_struct in manifest file is null.
    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (1, DATE '2024-01-20',
        TIMESTAMP '2024-02-20 10:00:00', null, 'vasya', 5)
    """
    )

    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (2, DATE '2024-02-20',
        TIMESTAMP '2024-03-20 15:00:00', null, 'vasilisa', 6)
    """
    )

    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (3, DATE '2025-03-20',
        TIMESTAMP '2024-04-30 14:00:00', null, 'icebreaker', 7)
    """
    )
    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (4, DATE '2025-04-20',
        TIMESTAMP '2024-05-30 14:00:00', null, 'iceberg', 8)
    """
    )

    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (1, DATE '2024-01-20',
        TIMESTAMP '2024-02-20 10:00:00', named_struct('a', DATE '2024-02-20', 'b', TIMESTAMP '2024-02-20 10:00:00'), 'vasya', 5)
    """
    )

    creation_expression = get_creation_expression(
        storage_type, TABLE_NAME, started_cluster, table_function=True, run_on_cluster=run_on_cluster
    )

    def check_validity_and_get_prunned_files(select_expression):
        settings1 = {
            "use_iceberg_partition_pruning": 0,
            "input_format_parquet_bloom_filter_push_down": 0,
            "input_format_parquet_filter_push_down": 0,
        }
        settings2 = {
            "use_iceberg_partition_pruning": 1,
            "input_format_parquet_bloom_filter_push_down": 0,
            "input_format_parquet_filter_push_down": 0,
        }
        return check_validity_and_get_prunned_files_general(
            instance, TABLE_NAME, settings1, settings2, 'IcebergMinMaxIndexPrunedFiles', select_expression
        )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE time_struct.a <= '2024-02-01' ORDER BY ALL"
        )
        == 1
    )


@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_bucket_partition_pruning(started_cluster, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_bucket_partition_pruning_" + storage_type + "_" + get_uuid_str()

    def execute_spark_query(query: str):
        return execute_spark_query_general(
            spark,
            started_cluster,
            storage_type,
            TABLE_NAME,
            query,
        )

    execute_spark_query(
        f"""
            CREATE TABLE {TABLE_NAME} (
                id INT,
                name STRING,
                value DECIMAL(10, 2),
                created_at DATE,
                event_time TIMESTAMP
            )
            USING iceberg
            PARTITIONED BY (bucket(3, id), bucket(2, name), bucket(4, value), bucket(5, created_at), bucket(3, event_time))
            OPTIONS('format-version'='2')
        """
    )

    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (1, 'Alice', 10.50, DATE '2024-01-20', TIMESTAMP '2024-01-20 10:00:00'),
        (2, 'Bob', 20.00, DATE '2024-01-21', TIMESTAMP '2024-01-21 11:00:00'),
        (3, 'Charlie', 30.50, DATE '2024-01-22', TIMESTAMP '2024-01-22 12:00:00'),
        (4, 'Diana', 40.00, DATE '2024-01-23', TIMESTAMP '2024-01-23 13:00:00'),
        (5, 'Eve', 50.50, DATE '2024-01-24', TIMESTAMP '2024-01-24 14:00:00');
        """
    )

    def check_validity_and_get_prunned_files(select_expression):
        settings1 = {
            "use_iceberg_partition_pruning": 0
        }
        settings2 = {
            "use_iceberg_partition_pruning": 1
        }
        return check_validity_and_get_prunned_files_general(
            instance,
            TABLE_NAME,
            settings1,
            settings2,
            "IcebergPartitionPrunedFiles",
            select_expression,
        )

    creation_expression = get_creation_expression(
        storage_type, TABLE_NAME, started_cluster, table_function=True
    )

    queries = [
        f"SELECT * FROM {creation_expression} WHERE id == 1 ORDER BY ALL",
        f"SELECT * FROM {creation_expression} WHERE value == 20.00 OR event_time == '2024-01-24 14:00:00' ORDER BY ALL",
        f"SELECT * FROM {creation_expression} WHERE id == 3 AND name == 'Charlie' ORDER BY ALL",
        f"SELECT * FROM {creation_expression} WHERE (event_time == TIMESTAMP '2024-01-21 11:00:00' AND name == 'Bob') OR (name == 'Eve' AND id == 5) ORDER BY ALL",
    ]

    for query in queries:
        assert check_validity_and_get_prunned_files(query) > 0


@pytest.mark.parametrize("format_version", ["2"])
@pytest.mark.parametrize("storage_type", ["s3"])
def test_cluster_table_function_with_partition_pruning(
    started_cluster, format_version, storage_type
):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session

    TABLE_NAME = (
        "test_cluster_table_function_with_partition_pruning_"
        + format_version
        + "_"
        + storage_type
        + "_"
        + get_uuid_str()
    )

    def execute_spark_query(query: str):
        return execute_spark_query_general(
            spark,
            started_cluster,
            storage_type,
            TABLE_NAME,
            query,
        )

    execute_spark_query(
        f"""
            DROP TABLE IF EXISTS {TABLE_NAME};
        """
    )

    execute_spark_query(
        f"""
            CREATE TABLE {TABLE_NAME} (
                a int,
                b float
            )
            USING iceberg
            PARTITIONED BY (identity(a))
            OPTIONS ('format-version'='{format_version}')
        """
    )

    execute_spark_query(f"INSERT INTO {TABLE_NAME} VALUES (1, 1.0), (2, 2.0), (3, 3.0)")

    table_function_expr_cluster = get_creation_expression(
        storage_type,
        TABLE_NAME,
        started_cluster,
        table_function=True,
        run_on_cluster=True,
    )

    instance.query(f"SELECT * FROM {table_function_expr_cluster} WHERE a = 1")


def test_time_travel_bug_fix_validation(started_cluster):
    instance = started_cluster.instances["node1"]
    TABLE_NAME = "test_bucket_partition_pruning_" + get_uuid_str()

    create_iceberg_table("local", instance, TABLE_NAME, started_cluster, "(x String, y Int64)")

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES ('123', 1);", settings={"allow_experimental_insert_into_iceberg": 1, "write_full_path_in_iceberg_metadata": True})

    default_download_directory(
        started_cluster,
        "local",
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    first_snapshot = get_last_snapshot(f"/iceberg_data/default/{TABLE_NAME}/")

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES ('123', 1);", settings={"allow_experimental_insert_into_iceberg": 1, "write_full_path_in_iceberg_metadata": True})

    instance.query(f"SELECT count() FROM {TABLE_NAME}", settings={"iceberg_snapshot_id": first_snapshot})


    assert int((instance.query(f"SELECT count() FROM {TABLE_NAME}")).strip()) == 2


@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_writes_complex_types(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_bucket_partition_pruning_" + storage_type + "_" + get_uuid_str()

    schema = "(x Array(Nullable(Int32)), z Map(Int32, Nullable(Int64)), y Tuple(zip Nullable(Int32), foo Nullable(Int32)))"
    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, schema, format_version)

    with pytest.raises(Exception):
        create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, schema, format_version)

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, schema, format_version, "", True)

    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == ''

    map_value = "{5:6}"
    instance.query(f"INSERT INTO {TABLE_NAME} VALUES ([1,2], {map_value}, (3,4));", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '[1,2]\t{5:6}\t(3,4)\n'

    if storage_type != "local":
        return

    default_download_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    with open(f"/iceberg_data/default/{TABLE_NAME}/metadata/version-hint.text", "wb") as f:
        f.write(b"1")

    df = spark.read.format("iceberg").load(f"/iceberg_data/default/{TABLE_NAME}").collect()
    assert len(df) == 1


@pytest.mark.parametrize("storage_type", ["local", "s3"])
def test_compressed_metadata(started_cluster, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_compressed_metadata_" + storage_type + "_" + get_uuid_str()

    table_properties = {
        "write.metadata.compression": "gzip"
    }

    df = spark.createDataFrame([
        (1, "Alice"),
        (2, "Bob")
    ], ["id", "name"])

    # for some reason write.metadata.compression is not working :(
    df.writeTo(TABLE_NAME) \
        .tableProperty("write.metadata.compression", "gzip") \
        .using("iceberg") \
        .create()

    # manual compression of metadata file before upload, still test some scenarios
    subprocess.check_output(f"gzip /iceberg_data/default/{TABLE_NAME}/metadata/v1.metadata.json", shell=True)

    # Weird but compression extension is really in the middle of the file name, not in the end...
    subprocess.check_output(f"mv /iceberg_data/default/{TABLE_NAME}/metadata/v1.metadata.json.gz /iceberg_data/default/{TABLE_NAME}/metadata/v1.gz.metadata.json", shell=True)

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, explicit_metadata_path="")

    assert instance.query(f"SELECT * FROM {TABLE_NAME} WHERE not ignore(*)") == "1\tAlice\n2\tBob\n"


@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_writes(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session

    TABLE_NAME = "test_row_based_deletes_" + storage_type + "_" + get_uuid_str()

    spark.sql(
        f"CREATE TABLE {TABLE_NAME} (id int) USING iceberg TBLPROPERTIES ('format-version' = '{format_version}')")

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)
    spark.sql(f"INSERT INTO {TABLE_NAME} VALUES (42);")

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (123);", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '42\n123\n'
    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (456);", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '42\n123\n456\n'

    if storage_type != "local":
        return

    initial_files = default_download_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    with open(f"/iceberg_data/default/{TABLE_NAME}/metadata/version-hint.text", "wb") as f:
        f.write(b"4")

    df = spark.read.format("iceberg").load(f"/iceberg_data/default/{TABLE_NAME}").collect()
    assert len(df) == 3

    instance.query("SYSTEM ENABLE FAILPOINT iceberg_writes_cleanup")
    with pytest.raises(Exception):
        instance.query(f"INSERT INTO {TABLE_NAME} VALUES (777777777777);", settings={"allow_experimental_insert_into_iceberg": 1})


    files = default_download_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    assert len(initial_files) == len(files)


@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_writes_from_zero(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session

    TABLE_NAME = "test_row_based_deletes_" + storage_type + "_" + get_uuid_str()

    spark.sql(
        f"CREATE TABLE {TABLE_NAME} (id int) USING iceberg TBLPROPERTIES ('format-version' = '{format_version}')")
    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (123);", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '123\n'
    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (456);", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '123\n456\n'

    if storage_type != "local":
        return

    default_download_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    with open(f"/iceberg_data/default/{TABLE_NAME}/metadata/version-hint.text", "wb") as f:
        f.write(b"3")

    df = spark.read.format("iceberg").load(f"/iceberg_data/default/{TABLE_NAME}").collect()
    assert len(df) == 2


@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_writes_with_partitioned_table(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_bucket_partition_pruning_" + storage_type + "_" + get_uuid_str()

    def execute_spark_query(query: str):
        spark.sql(query)
        default_upload_directory(
            started_cluster,
            storage_type,
            f"/iceberg_data/default/{TABLE_NAME}/",
            f"/iceberg_data/default/{TABLE_NAME}/",
        )
        return

    execute_spark_query(
        f"""
            CREATE TABLE {TABLE_NAME} (
                id INT,
                name STRING,
                value DECIMAL(10, 2),
                created_at DATE,
                event_time TIMESTAMP
            )
            USING iceberg
            PARTITIONED BY (bucket(3, id), bucket(2, name), bucket(5, created_at), bucket(3, event_time))
            OPTIONS('format-version'='{format_version}')
        """
    )
    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)

    instance.query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (1, 'Alice', 10.50, DATE '2024-01-20', TIMESTAMP '2024-01-20 10:00:00'),
        (2, 'Bob', 20.00, DATE '2024-01-21', TIMESTAMP '2024-01-21 11:00:00'),
        (3, 'Charlie', 30.50, DATE '2024-01-22', TIMESTAMP '2024-01-22 12:00:00'),
        (4, 'Diana', 40.00, DATE '2024-01-23', TIMESTAMP '2024-01-23 13:00:00'),
        (5, 'Eve', 50.50, DATE '2024-01-24', TIMESTAMP '2024-01-24 14:00:00');
        """,
        settings={"allow_experimental_insert_into_iceberg": 1}
    )

    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '1\tAlice\t10.5\t2024-01-20\t2024-01-20 10:00:00.000000\n2\tBob\t20\t2024-01-21\t2024-01-21 11:00:00.000000\n3\tCharlie\t30.5\t2024-01-22\t2024-01-22 12:00:00.000000\n4\tDiana\t40\t2024-01-23\t2024-01-23 13:00:00.000000\n5\tEve\t50.5\t2024-01-24\t2024-01-24 14:00:00.000000\n'

    instance.query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (10, 'Alice', 10.50, DATE '2024-01-20', TIMESTAMP '2024-01-20 10:00:00'),
        (20, 'Bob', 20.00, DATE '2024-01-21', TIMESTAMP '2024-01-21 11:00:00'),
        (30, 'Charlie', 30.50, DATE '2024-01-22', TIMESTAMP '2024-01-22 12:00:00'),
        (40, 'Diana', 40.00, DATE '2024-01-23', TIMESTAMP '2024-01-23 13:00:00'),
        (50, 'Eve', 50.50, DATE '2024-01-24', TIMESTAMP '2024-01-24 14:00:00');
        """,
        settings={"allow_experimental_insert_into_iceberg": 1}
    )

    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '1\tAlice\t10.5\t2024-01-20\t2024-01-20 10:00:00.000000\n2\tBob\t20\t2024-01-21\t2024-01-21 11:00:00.000000\n3\tCharlie\t30.5\t2024-01-22\t2024-01-22 12:00:00.000000\n4\tDiana\t40\t2024-01-23\t2024-01-23 13:00:00.000000\n5\tEve\t50.5\t2024-01-24\t2024-01-24 14:00:00.000000\n10\tAlice\t10.5\t2024-01-20\t2024-01-20 10:00:00.000000\n20\tBob\t20\t2024-01-21\t2024-01-21 11:00:00.000000\n30\tCharlie\t30.5\t2024-01-22\t2024-01-22 12:00:00.000000\n40\tDiana\t40\t2024-01-23\t2024-01-23 13:00:00.000000\n50\tEve\t50.5\t2024-01-24\t2024-01-24 14:00:00.000000\n'

    if storage_type != "local":
        return

    default_download_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    with open(f"/iceberg_data/default/{TABLE_NAME}/metadata/version-hint.text", "wb") as f:
        f.write(b"3")

    df = spark.read.format("iceberg").load(f"/iceberg_data/default/{TABLE_NAME}").collect()
    assert len(df) == 10

@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_minmax_pruning_for_arrays_and_maps_subfields_disabled(started_cluster, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = (
        "test_disable_minmax_pruning_for_arrays_and_maps_subfields_"
        + storage_type
        + "_"
        + get_uuid_str()
    )

    def execute_spark_query(query: str):
        return execute_spark_query_general(
            spark,
            started_cluster,
            storage_type,
            TABLE_NAME,
            query,
        )

    execute_spark_query(
        f"""
            DROP TABLE IF EXISTS {TABLE_NAME};
        """
    )

    execute_spark_query(
        f"""
            CREATE TABLE {TABLE_NAME} (
            id BIGINT,
            measurements ARRAY<DOUBLE>
            ) USING iceberg
            TBLPROPERTIES (
            'write.metadata.metrics.max' = 'measurements.element',
            'write.metadata.metrics.min' = 'measurements.element'
            );
        """
    )

    execute_spark_query(
        f"""
            INSERT INTO {TABLE_NAME} VALUES
            (1, array(23.5, 24.1, 22.8, 25.3, 23.9)),
            (2, array(18.2, 19.5, 17.8, 20.1, 19.3, 18.7)),
            (3, array(30.0, 31.2, 29.8, 32.1, 30.5, 29.9, 31.0)),
            (4, array(15.5, 16.2, 14.8, 17.1, 16.5)),
            (5, array(27.3, 28.1, 26.9, 29.2, 28.5, 27.8, 28.3, 27.6));
        """
    )

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    table_creation_expression = get_creation_expression(
        storage_type,
        TABLE_NAME,
        started_cluster,
        table_function=True,
        allow_dynamic_metadata_for_data_lakes=True,
    )

    table_select_expression = table_creation_expression

    instance.query(f"SELECT * FROM {table_select_expression} ORDER BY ALL")


@pytest.mark.parametrize("format_version", [1, 2])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_writes_create_table(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_bucket_partition_pruning_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, "(x String)", format_version)

    with pytest.raises(Exception):
        create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, "(x String)", format_version)

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, "(x String)", format_version, "", True)

    assert '`x` String' in instance.query(f"SHOW CREATE TABLE {TABLE_NAME}")

    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == ''

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (123);", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '123\n'
    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (456);", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '123\n456\n'

    if storage_type != "local":
        return

    default_download_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    with open(f"/iceberg_data/default/{TABLE_NAME}/metadata/version-hint.text", "wb") as f:
        f.write(b"2")

    df = spark.read.format("iceberg").load(f"/iceberg_data/default/{TABLE_NAME}").collect()
    assert len(df) == 2


@pytest.mark.parametrize("format_version", [1, 2])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
@pytest.mark.parametrize("partition_type", ["identity(y)", "(identity(y))", "icebergTruncate(3, y)", "(identity(y), icebergBucket(3, x))"])
def test_writes_create_partitioned_table(started_cluster, format_version, storage_type, partition_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_bucket_partition_pruning_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, "(x String, y Int64)", format_version, partition_type)

    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == ''

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES ('123', 1);", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '123\t1\n'

    if storage_type != "local":
        return

    default_download_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    with open(f"/iceberg_data/default/{TABLE_NAME}/metadata/version-hint.text", "wb") as f:
        f.write(b"2")

    df = spark.read.format("iceberg").load(f"/iceberg_data/default/{TABLE_NAME}").collect()
    assert len(df) == 1


@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_relevant_iceberg_schema_chosen(started_cluster, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_relevant_iceberg_schema_chosen_" + storage_type + "_" + get_uuid_str()

    spark.sql(
        f"""
        CREATE TABLE IF NOT EXISTS {TABLE_NAME} (
            a INT NOT NULL
        ) using iceberg
        TBLPROPERTIES ('format-version' = '2',
            'commit.manifest.min-count-to-merge' = '1',
            'commit.manifest-merge.enabled' = 'true');
        """
    )

    values_list = ", ".join(["(1)" for _ in range(5)])
    spark.sql(
        f"""
        INSERT INTO {TABLE_NAME} VALUES {values_list};
        """
    )

    spark.sql(
    f"""
        ALTER TABLE {TABLE_NAME} ADD COLUMN b INT;
    """
    )

    values_list = ", ".join(["(1, 2)" for _ in range(5)])
    spark.sql(
        f"""
        INSERT INTO {TABLE_NAME} VALUES {values_list};
        """
    )

    spark.sql(f"CALL system.rewrite_manifests('{TABLE_NAME}')")

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )


    table_creation_expression = get_creation_expression(
        storage_type,
        TABLE_NAME,
        started_cluster,
        table_function=True,
    )

    instance.query(f"SELECT * FROM {table_creation_expression} WHERE b >= 2", settings={"input_format_parquet_filter_push_down": 0, "input_format_parquet_bloom_filter_push_down": 0})


def test_writes_create_table_with_empty_data(started_cluster):
    instance = started_cluster.instances["node1"]
    TABLE_NAME = "test_relevant_iceberg_schema_chosen_" + get_uuid_str()
    instance.query(
        f"CREATE TABLE {TABLE_NAME} (c0 Int) ENGINE = IcebergLocal('/iceberg_data/default/{TABLE_NAME}/', 'CSV') AS (SELECT 1 OFFSET 1 ROW);",
        settings={"allow_experimental_insert_into_iceberg": 1}
    )

@pytest.mark.parametrize("format_version", [1, 2])
@pytest.mark.parametrize("storage_type", ["local"])
def test_writes_with_compression_metadata(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_bucket_partition_pruning_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, "(x String, y Int64)", format_version, use_version_hint=True, compression_method="gzip")

    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == ''
    instance.query(f"INSERT INTO {TABLE_NAME} VALUES ('123', 1);", settings={"allow_experimental_insert_into_iceberg": 1, "iceberg_metadata_compression_method": "gzip"})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '123\t1\n'

def test_writes_create_table_bugs(started_cluster):
    instance = started_cluster.instances["node1"]
    TABLE_NAME = "test_writes_create_table_bugs_" + get_uuid_str()
    TABLE_NAME_1 = "test_writes_create_table_bugs_" + get_uuid_str()
    instance.query(
        f"CREATE TABLE {TABLE_NAME} (c0 Int) ENGINE = IcebergLocal('/iceberg_data/default/{TABLE_NAME}/', 'CSV') AS (SELECT 1 OFFSET 1 ROW);",
        settings={"allow_experimental_insert_into_iceberg": 1}
    )

    error = instance.query_and_get_error(
        f"CREATE TABLE {TABLE_NAME_1} (c0 Int) ENGINE = IcebergLocal('/iceberg_data/default/{TABLE_NAME_1}/', 'CSV') PARTITION BY (icebergTruncate(c0));",
        settings={"allow_experimental_insert_into_iceberg": 1}
    )

    assert "BAD_ARGUMENTS" in error
    assert "LOGICAL_ERROR" not in error


@pytest.mark.parametrize("format_version", [1, 2])
@pytest.mark.parametrize("storage_type", ["local"])
def test_writes_create_version_hint(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_bucket_partition_pruning_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, "(x String, y Int64)", format_version, use_version_hint=True)

    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == ''
    default_download_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    target_suffix = b'v1.metadata.json'
    with open(f"/iceberg_data/default/{TABLE_NAME}/metadata/version-hint.text", "rb") as f:
        assert f.read()[-len(target_suffix):] == target_suffix

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES ('123', 1);", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL", ) == '123\t1\n'

    default_download_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    target_suffix = b'v2.metadata.json'
    with open(f"/iceberg_data/default/{TABLE_NAME}/metadata/version-hint.text", "rb") as f:
        assert f.read()[-len(target_suffix):] == target_suffix

    df = spark.read.format("iceberg").load(f"/iceberg_data/default/{TABLE_NAME}").collect()
    assert len(df) == 1


@pytest.mark.parametrize("format_version", [1, 2])
@pytest.mark.parametrize("storage_type", ["s3", "azure", "local"])
def test_writes_statistics_by_minmax_pruning(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_minmax_pruning_" + storage_type + "_" + get_uuid_str()

    schema = """
    (tag Int32,
    date Date32,
    ts DateTime,
    name String,
    number Int64)
    """
    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, schema, format_version)

    instance.query(
    f"""
        INSERT INTO {TABLE_NAME} VALUES
        (1, '2024-01-20',
        '2024-02-20 10:00:00',
        'vasya', 5);
    """,
    settings={"allow_experimental_insert_into_iceberg": 1}
    )

    instance.query(
    f"""
        INSERT INTO {TABLE_NAME} VALUES
        (2, '2024-02-20',
        '2024-03-20 15:00:00',
        'vasilisa', 6);
    """,
    settings={"allow_experimental_insert_into_iceberg": 1}
    )

    instance.query(
    f"""
        INSERT INTO {TABLE_NAME} VALUES
        (3, '2025-03-20',
        '2024-04-30 14:00:00',
        'icebreaker', 7);
    """,
    settings={"allow_experimental_insert_into_iceberg": 1}
    )

    instance.query(
    f"""
        INSERT INTO {TABLE_NAME} VALUES
        (4, '2025-04-20',
        '2024-05-30 14:00:00',
        'iceberg', 8);
    """,
    settings={"allow_experimental_insert_into_iceberg": 1}
    )


    def check_validity_and_get_prunned_files(select_expression):
        settings1 = {
            "use_iceberg_partition_pruning": 0,
            "input_format_parquet_bloom_filter_push_down": 0,
            "input_format_parquet_filter_push_down": 0,
        }
        settings2 = {
            "use_iceberg_partition_pruning": 1,
            "input_format_parquet_bloom_filter_push_down": 0,
            "input_format_parquet_filter_push_down": 0,
        }
        return check_validity_and_get_prunned_files_general(
            instance, TABLE_NAME, settings1, settings2, 'IcebergMinMaxIndexPrunedFiles', select_expression
        )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {TABLE_NAME} ORDER BY ALL"
        )
        == 0
    )
    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {TABLE_NAME} WHERE date <= '2024-01-25' ORDER BY ALL"
        )
        == 3
    )
    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {TABLE_NAME} WHERE ts <= timestamp('2024-03-20 14:00:00.000000') ORDER BY ALL"
        )
        == 3
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {TABLE_NAME} WHERE tag == 1 ORDER BY ALL"
        )
        == 3
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {TABLE_NAME} WHERE tag <= 1 ORDER BY ALL"
        )
        == 3
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {TABLE_NAME} WHERE name == 'vasilisa' ORDER BY ALL"
        )
        == 3
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {TABLE_NAME} WHERE name < 'kek' ORDER BY ALL"
        )
        == 2
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {TABLE_NAME} WHERE number == 8 ORDER BY ALL"
        )
        == 3
    )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {TABLE_NAME} WHERE number <= 5 ORDER BY ALL"
        )
        == 3
    )

@pytest.mark.parametrize("storage_type", ["local", "s3", "azure"])
def test_optimize(started_cluster, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_position_deletes_" + storage_type + "_" + get_uuid_str()

    spark.sql(
        f"""
        CREATE TABLE {TABLE_NAME} (id long, data string) USING iceberg TBLPROPERTIES ('format-version' = '2', 'write.update.mode'=
        'merge-on-read', 'write.delete.mode'='merge-on-read', 'write.merge.mode'='merge-on-read')
        """
    )
    spark.sql(f"INSERT INTO {TABLE_NAME} select id, char(id + ascii('a')) from range(10, 100)")

    def get_array(query_result: str):
        arr = sorted([int(x) for x in query_result.strip().split("\n")])
        print(arr)
        return arr

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)
    snapshot_id = get_last_snapshot(f"/iceberg_data/default/{TABLE_NAME}/")
    snapshot_timestamp = datetime.now(timezone.utc)

    time.sleep(0.1)
    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 90

    spark.sql(f"DELETE FROM {TABLE_NAME} WHERE id < 20")
    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )
    spark.sql(f"INSERT INTO {TABLE_NAME} select id, char(id + ascii('a')) from range(100, 110)")
    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 90

    instance.query(f"OPTIMIZE TABLE {TABLE_NAME};", settings={"allow_experimental_iceberg_compaction" : 1})

    assert int(instance.query(f"SELECT count() FROM {TABLE_NAME}")) == 90
    assert instance.query(f"SELECT id FROM {TABLE_NAME} ORDER BY id") == instance.query(
        "SELECT number FROM numbers(20, 90)"
    )

    # check that timetravel works with previous snapshot_ids and timestamps
    assert instance.query(f"SELECT id FROM {TABLE_NAME} ORDER BY id SETTINGS iceberg_snapshot_id = {snapshot_id}") == instance.query(
        "SELECT number FROM numbers(20, 80)"
    )

    assert instance.query(f"SELECT id FROM {TABLE_NAME} ORDER BY id SETTINGS iceberg_timestamp_ms = {int(snapshot_timestamp.timestamp() * 1000)}") == instance.query(
        "SELECT number FROM numbers(20, 80)"
    )
    if storage_type != "local":
        return

    default_download_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )
    df = spark.read.format("iceberg").load(f"/iceberg_data/default/{TABLE_NAME}").collect()
    assert len(df) == 90


@pytest.mark.parametrize("format_version", [1, 2])
@pytest.mark.parametrize("storage_type", ["local"])
def test_writes_drop_table(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_bucket_partition_pruning_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, "(x String, y Int64)", format_version, use_version_hint=True)

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES ('123', 1);", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL", ) == '123\t1\n'

    drop_iceberg_table(instance, TABLE_NAME)
    with pytest.raises(Exception):
        drop_iceberg_table(instance, TABLE_NAME)
    drop_iceberg_table(instance, TABLE_NAME, True)

    files = default_download_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )
    # drop should not delete user data
    assert len(files) > 0


@pytest.mark.parametrize("format_version", [1, 2])
@pytest.mark.parametrize("storage_type", ["s3", "local", "azure"])
def test_writes_schema_evolution(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_bucket_partition_pruning_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, "(x Nullable(Int32))", format_version)
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == ''

    with pytest.raises(Exception):
        instance.query(f"ALTER TABLE {TABLE_NAME} MODIFY COLUMN x Int64;", settings={"allow_experimental_insert_into_iceberg": 1})

    instance.query(f"ALTER TABLE {TABLE_NAME} MODIFY COLUMN x Nullable(Int64);", settings={"allow_experimental_insert_into_iceberg": 1})
    assert '`x` Nullable(Int64)' in instance.query(f"SHOW CREATE TABLE {TABLE_NAME}")

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (123);", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '123\n'

    instance.query(f"ALTER TABLE {TABLE_NAME} ADD COLUMN y Nullable(Float64);", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '123\t\\N\n'
    assert '`y` Nullable(Float64)' in instance.query(f"SHOW CREATE TABLE {TABLE_NAME}")

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (124, 4.56);", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '123\t\\N\n124\t4.5600000000000005\n'

    instance.query(f"ALTER TABLE {TABLE_NAME} DROP COLUMN x;", settings={"allow_experimental_insert_into_iceberg": 1})
    assert '`x` Nullable(Int64)' not in instance.query(f"SHOW CREATE TABLE {TABLE_NAME}")

    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '4.5600000000000005\n\\N\n'

    if storage_type != "local":
        return

    default_download_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    df = spark.read.format("iceberg").load(f"/iceberg_data/default/{TABLE_NAME}").collect()
    assert len(df) == 2


@pytest.mark.parametrize("storage_type", ["s3", "local", "azure"])
@pytest.mark.parametrize("partition_type", ["", "identity(x)", "icebergBucket(3, x)"])
def test_writes_mutate_delete(started_cluster, storage_type, partition_type):
    format_version = 2
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_bucket_partition_pruning_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster, "(x String)", format_version, partition_type)

    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == ''
    instance.query(f"ALTER TABLE {TABLE_NAME} DELETE WHERE x = 'pudge1000-7';", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == ''

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (123);", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '123\n'
    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (456);", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '123\n456\n'
    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (999);", settings={"allow_experimental_insert_into_iceberg": 1})

    instance.query(f"ALTER TABLE {TABLE_NAME} DELETE WHERE x = 'pudge1000-7';", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '123\n456\n999\n'

    instance.query(f"ALTER TABLE {TABLE_NAME} DELETE WHERE x = '123';", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '456\n999\n'

    instance.query(f"ALTER TABLE {TABLE_NAME} DELETE WHERE x = '999';", settings={"allow_experimental_insert_into_iceberg": 1})
    assert instance.query(f"SELECT * FROM {TABLE_NAME} ORDER BY ALL") == '456\n'

    if storage_type != "local":
        return
    initial_files = default_download_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    instance.query("SYSTEM ENABLE FAILPOINT iceberg_writes_cleanup")
    with pytest.raises(Exception):
        instance.query(f"ALTER TABLE {TABLE_NAME} DELETE WHERE x = '456';", settings={"allow_experimental_insert_into_iceberg": 1})

    files = default_download_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    assert len(initial_files) == len(files)

    df = spark.read.format("iceberg").load(f"/iceberg_data/default/{TABLE_NAME}").collect()
    assert len(df) == 1

class PrunedInfo:
    def __init__(self, not_pruned, partition_pruned, min_max_index_pruned):
        self.not_pruned = not_pruned
        self.partition_pruned = partition_pruned
        self.min_max_index_pruned = min_max_index_pruned

    def __repr__(self):
        return "PrunedInfo(not_pruned={}, partition_pruned={}, min_max_index_pruned={})".format(self.not_pruned, self.partition_pruned, self.min_max_index_pruned)

    def __eq__(self, other):
        return (self.not_pruned == other.not_pruned and
                self.partition_pruned == other.partition_pruned and
                self.min_max_index_pruned == other.min_max_index_pruned)

def get_date_and_time_columns(instance, query_id: str):
    result = dict()
    for name in ['event_date', 'event_time']:
        query_result = instance.query(f"SELECT {name} FROM system.iceberg_metadata_log WHERE query_id = '{query_id}'")
        result[name] = query_result.split('\n')
        result[name] = list(filter(lambda x: len(x) > 0, result[name]))
    return result

def get_iceberg_metadata_to_dict(instance, query_id: str):
    result = dict()
    for name in ['content', 'content_type', 'table_path', 'file_path', 'row_in_file', 'pruning_status']:
        # We are ok with duplicates in the table itself but for test purposes we want to remove duplicates here
        select_distinct_expression = f"SELECT DISTINCT(*) FROM (SELECT content, content_type, table_path, file_path, row_in_file, pruning_status FROM system.iceberg_metadata_log WHERE query_id = '{query_id}') ORDER BY ALL"
        query_result = instance.query(f"SELECT {name} FROM ({select_distinct_expression})")
        print("Query result for {}: {}".format(name, query_result))
        result[name] = query_result.split('\n')[:-1]
    result['row_in_file'] = list(map(lambda x : int(x) if x.isdigit() else None, result['row_in_file']))
    result['pruning_status'] = list(map(lambda x : x if x != '\\N' else None, result['pruning_status']))
    print("Result dictionary: {}".format(result))
    return result

def verify_result_dictionary(diction : dict, allowed_content_types : set):
    prunned_info = PrunedInfo(0, 0, 0)
    # Expected content_type and only it is present
    if set(diction['content_type']) != allowed_content_types:
        raise ValueError("Content type mismatch. Expected: {}, got: {}".format(allowed_content_types, set(diction['content_type'])))
    # For all entries we have the same table_path
    if not (len(set(diction['table_path'])) == 1 or (len(allowed_content_types) == 0 and len(diction['table_path']) == 0)):
        raise ValueError("Unexpected number of table paths are found for one query. Set: {}".format(set(diction['table_path'])))
    extensions = list(map(lambda x: x.split('.')[-1], diction['file_path']))
    for i in range(len(diction['content_type'])):
        if diction['content_type'][i] == 'Metadata':
            # File with content_type 'Metadata' has json extension
            if extensions[i] != 'json':
                raise ValueError("Unexpected file extension for Metadata. Expected: json, got: {}".format(extensions[i]))
        else:
            # File with content_types except 'Metadata' has avro extension
            if extensions[i] != 'avro':
                raise ValueError("Unexpected file extension for {}. Expected: avro, got: {}".format(diction['content_type'][i], extensions[i]))

    # All content is json-serializable
    for content in diction['content']:
        if content == '':
            continue
        try:
            json.loads(content)
        except:
            raise ValueError("Content is not valid JSON. Content: {}".format(content))
    for file_path in set(diction['file_path']):
        row_values = set()
        number_of_missing_row_values = 0
        number_of_rows = 0
        partitioned_rows = set()
        not_deleted_files = set()
        for i in range(len(diction['file_path'])):
            if file_path == diction['file_path'][i]:
                if diction['row_in_file'][i] is not None:
                    row_values.add(diction['row_in_file'][i])
                    # If row is present the type is entry
                    if diction['content_type'][i] not in ['ManifestFileEntry', 'ManifestListEntry']:
                        raise ValueError("Row should not be specified for an entry {}, file_path: {}".format(diction['content_type'][i], file_path))
                    if diction['content'][i] != '':
                        number_of_rows += 1

                    if diction['content_type'][i] == 'ManifestFileEntry':
                        if diction['content'][i] == '':
                            if diction['pruning_status'][i] is None:
                                raise ValueError("Pruning status should be specified for this manifest file entry, file_path: {}".format(file_path))
                            partitioned_rows.add(diction['row_in_file'][i])
                            if diction['pruning_status'][i] == 'NotPruned':
                                prunned_info.not_pruned += 1
                            elif diction['pruning_status'][i] == 'PartitionPruned':
                                prunned_info.partition_pruned += 1
                            elif diction['pruning_status'][i] == 'MinMaxIndexPruned':
                                prunned_info.min_max_index_pruned += 1
                            else:
                                raise ValueError("Unexpected pruning status: {}, file_path: {}".format(diction['pruning_status'][i], file_path))
                        else:
                            data_object = json.loads(diction['content'][i])
                            print("Data object: {}".format(data_object))
                            if data_object['status'] < 2:
                                not_deleted_files.add(diction['row_in_file'][i])
                else:
                    # If row is not present that the type is metadata
                    if diction['content_type'][i] not in ['Metadata', 'ManifestFileMetadata', 'ManifestListMetadata']:
                        raise ValueError("Row should be specified for an entry {}, file_path: {}".format(diction['content_type'][i], file_path))

                    number_of_missing_row_values += 1
        if partitioned_rows != not_deleted_files:
            raise ValueError("Partitioned rows are not consistent with not deleted files for file path: {}, partitioned rows: {}, not deleted files: {}".format(file_path, partitioned_rows, not_deleted_files))

        # We have exactly one metadata file
        if number_of_missing_row_values != 1:
            raise ValueError("Not a one row value (corresponding to metadata file) is missing for file path: {}".format(file_path))

        # Rows in avro files are consistent
        if len(row_values) != number_of_rows:
            raise ValueError("Unexpected number of row values for file path: {}".format(file_path))
        for i in range(number_of_rows):
            if not i in row_values:
                raise ValueError("Missing row value for file path: {}, missing row index: {}".format(file_path, i))
    return prunned_info

def get_prunned_info_from_profile_events(instance, query_id: str):
    instance.query("SYSTEM FLUSH LOGS")

    not_pruned = int(
        instance.query(
            f"SELECT ProfileEvents['IcebergMetadataReturnedObjectInfos'] FROM system.query_log WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
        )
    )

    partition_pruned = int(
        instance.query(
            f"SELECT ProfileEvents['IcebergPartitionPrunedFiles'] FROM system.query_log WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
        )
    )

    min_max_index_pruned = int(
        instance.query(
            f"SELECT ProfileEvents['IcebergMinMaxIndexPrunedFiles'] FROM system.query_log WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
        )
    )

    return PrunedInfo(not_pruned, partition_pruned, min_max_index_pruned)


@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "local", "azure"])
def test_system_iceberg_metadata(started_cluster, format_version, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = (
        "test_system_iceberg_metadata_"
        + format_version
        + "_"
        + storage_type
        + "_"
        + get_uuid_str()
    )

    def execute_spark_query(query: str):
        return execute_spark_query_general(
            spark,
            started_cluster,
            storage_type,
            TABLE_NAME,
            query,
        )

    execute_spark_query(
        f"""
            CREATE TABLE {TABLE_NAME} (
                a INT,
                b STRING
            )
            USING iceberg
            PARTITIONED BY (identity(a))
            OPTIONS('format-version'='2')
        """
    )

    for i in range(5):
        spark.sql(
            f"""
                INSERT INTO {TABLE_NAME} VALUES
                ({i}, '{i}');
            """
        )

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)

    content_types = ["Metadata", "ManifestListMetadata", "ManifestListEntry", "ManifestFileMetadata", "ManifestFileEntry"]
    settings = ["none", "metadata", "manifest_list_metadata", "manifest_list_entry", "manifest_file_metadata", "manifest_file_entry"]


    for i in range(len(settings)):
        allowed_content_types = set(content_types[:i])

        query_id = TABLE_NAME + "_" + str(i) + "_" + get_uuid_str()

        instance.query(f"SELECT * FROM {TABLE_NAME} WHERE a >= 2", query_id = query_id,  settings={"iceberg_metadata_log_level":settings[i], "use_iceberg_partition_pruning": 1})

        expected_prunned_info = get_prunned_info_from_profile_events(instance, query_id)

        diction = get_iceberg_metadata_to_dict(instance, query_id)

        try:
            if settings[i] == 'manifest_file_entry':
                table_prunned_info = verify_result_dictionary(diction, allowed_content_types)
                assert table_prunned_info == expected_prunned_info, "Not prunned files count mismatch. Table: {}, ProfileEvents: {}".format(table_prunned_info, expected_prunned_info)
        except:
            print("Dictionary: {}, Allowed Content Types: {}".format(diction, allowed_content_types))
            raise


@pytest.mark.parametrize("storage_type", ["s3", "azure"])
@pytest.mark.parametrize("run_on_cluster", [False, True])
def test_read_constant_columns_optimization(started_cluster, storage_type, run_on_cluster):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_read_constant_columns_optimization_" + storage_type + "_" + get_uuid_str()

    def execute_spark_query(query: str):
        return execute_spark_query_general(
            spark,
            started_cluster,
            storage_type,
            TABLE_NAME,
            query,
        )

    execute_spark_query(
        f"""
            CREATE TABLE {TABLE_NAME} (
                tag INT,
                date DATE,
                date2 DATE,
                name VARCHAR(50),
                number BIGINT
            )
            USING iceberg
            PARTITIONED BY (identity(tag), years(date))
            OPTIONS('format-version'='2')
        """
    )

    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (1, DATE '2024-01-20', DATE '2024-01-20', 'vasya', 5),
        (1, DATE '2024-01-20', DATE '2024-01-20', 'vasilisa', 5),
        (1, DATE '2025-01-20', DATE '2025-01-20', 'vasya', 5),
        (1, DATE '2025-01-20', DATE '2025-01-20', 'vasya', 5),
        (2, DATE '2025-01-20', DATE '2025-01-20', 'vasilisa', 5),
        (2, DATE '2025-01-21', DATE '2025-01-20', 'vasilisa', 5)
    """
    )

    execute_spark_query(
        f"""
            ALTER TABLE {TABLE_NAME} ALTER COLUMN number FIRST;
        """
    )

    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (5, 3, DATE '2025-01-20', DATE '2024-01-20', 'vasilisa'),
        (5, 3, DATE '2025-01-20', DATE '2025-01-20', 'vasilisa')
    """
    )

    execute_spark_query(
        f"""
            ALTER TABLE {TABLE_NAME} RENAME COLUMN name TO name_old;
        """
    )

    execute_spark_query(
        f"""
            ALTER TABLE {TABLE_NAME}
            ADD COLUMNS (
                name string
            );
        """
    )

    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (5, 4, DATE '2025-01-20', DATE '2024-01-20', 'vasya', 'iceberg'),
        (5, 4, DATE '2025-01-20', DATE '2025-01-20', 'vasilisa', 'iceberg'),
        (5, 5, DATE '2025-01-20', DATE '2024-01-20', 'vasya', 'iceberg'),
        (5, 5, DATE '2025-01-20', DATE '2024-01-20', 'vasilisa', 'icebreaker'),
        (5, 6, DATE '2025-01-20', DATE '2024-01-20', 'vasya', 'iceberg'),
        (5, 6, DATE '2025-01-20', DATE '2024-01-20', 'vasya', 'iceberg')
    """
    )

    # Totally must be 7 files
    # Partitioned column 'tag' is constant in each file
    # Column 'date' is constant in 6 files, has different values in (2-2025)
    # Column 'date2' is constant in 4 files (1-2024, 2-2025, 5-2025, 6-2025)
    # Column 'name_old' is constant in 3 files (1-2025, 2-2025 as 'name', 6-2025 as 'name_old')
    # Column 'number' is globally constant
    # New column 'name2' is present only in 3 files (4-2025, 5-2025, 6-2025), constant in two (4-2025, 6-2025)
    # Files 1-2025 and 6-2025 have only constant columns

    creation_expression = get_creation_expression(
        storage_type, TABLE_NAME, started_cluster, table_function=True, run_on_cluster=run_on_cluster
    )

    # Warm up metadata cache
    for replica in started_cluster.instances.values():
        replica.query("SYSTEM DROP UNCOMPRESSED CACHE")
        replica.query("SYSTEM DROP QUERY CACHE")
        replica.query("SYSTEM DROP FILESYSTEM CACHE")
        replica.query("SYSTEM DROP ICEBERG METADATA CACHE")
        replica.query(f"SELECT * FROM {creation_expression} ORDER BY ALL SETTINGS allow_experimental_iceberg_read_optimization=0")

    all_data_expected_query_id = str(uuid.uuid4())
    all_data_expected = instance.query(
        f"SELECT * FROM {creation_expression} ORDER BY ALL SETTINGS allow_experimental_iceberg_read_optimization=0",
        query_id=all_data_expected_query_id,
        )
    const_only_expected_query_id = str(uuid.uuid4())
    const_only_expected = instance.query(
        f"SELECT tag, number FROM {creation_expression} ORDER BY ALL SETTINGS allow_experimental_iceberg_read_optimization=0",
        query_id=const_only_expected_query_id,
        )
    const_partial_expected_query_id = str(uuid.uuid4())
    const_partial_expected = instance.query(
        f"SELECT tag, date2, number, name_old FROM {creation_expression} ORDER BY ALL SETTINGS allow_experimental_iceberg_read_optimization=0",
        query_id=const_partial_expected_query_id,
        )
    const_partial2_expected_query_id = str(uuid.uuid4())
    const_partial2_expected = instance.query(
        f"SELECT tag, date2, number, name FROM {creation_expression} ORDER BY ALL SETTINGS allow_experimental_iceberg_read_optimization=0",
        query_id=const_partial2_expected_query_id,
        )
    count_expected_query_id = str(uuid.uuid4())
    count_expected = instance.query(
        f"SELECT count(),tag FROM {creation_expression} GROUP BY ALL ORDER BY ALL SETTINGS allow_experimental_iceberg_read_optimization=0",
        query_id=count_expected_query_id,
        )

    all_data_query_id = str(uuid.uuid4())
    all_data_optimized = instance.query(
        f"SELECT * FROM {creation_expression} ORDER BY ALL SETTINGS allow_experimental_iceberg_read_optimization=1",
        query_id=all_data_query_id,
        )
    const_only_query_id = str(uuid.uuid4())
    const_only_optimized = instance.query(
        f"SELECT tag, number FROM {creation_expression} ORDER BY ALL SETTINGS allow_experimental_iceberg_read_optimization=1",
        query_id=const_only_query_id,
        )
    const_partial_query_id = str(uuid.uuid4())
    const_partial_optimized = instance.query(
        f"SELECT tag, date2, number, name_old FROM {creation_expression} ORDER BY ALL SETTINGS allow_experimental_iceberg_read_optimization=1",
        query_id=const_partial_query_id,
        )
    const_partial2_query_id = str(uuid.uuid4())
    const_partial2_optimized = instance.query(
        f"SELECT tag, date2, number, name FROM {creation_expression} ORDER BY ALL SETTINGS allow_experimental_iceberg_read_optimization=1",
        query_id=const_partial2_query_id,
        )
    count_query_id = str(uuid.uuid4())
    count_optimized = instance.query(
        f"SELECT count(),tag FROM {creation_expression} GROUP BY ALL ORDER BY ALL SETTINGS allow_experimental_iceberg_read_optimization=1",
        query_id=count_query_id,
        )

    assert all_data_expected == all_data_optimized
    assert const_only_expected == const_only_optimized
    assert const_partial_expected == const_partial_optimized
    assert const_partial2_expected == const_partial2_optimized
    assert count_expected == count_optimized

    for replica in started_cluster.instances.values():
        replica.query("SYSTEM FLUSH LOGS")

    def check_events(query_id, event, expected):
        res = instance.query(
            f"""
            SELECT
                sum(tupleElement(arrayJoin(ProfileEvents),2)) as value
            FROM
                clusterAllReplicas('cluster_simple', system.query_log)
            WHERE
                type='QueryFinish'
                AND tupleElement(arrayJoin(ProfileEvents),1)='{event}'
                AND initial_query_id='{query_id}'
            GROUP BY ALL
            FORMAT CSV
            """)
        assert int(res) == expected

    event = "S3GetObject" if storage_type == "s3" else "AzureGetObject"

    # Without optimization clickhouse reads all 7 files
    check_events(all_data_expected_query_id, event, 7)
    check_events(const_only_expected_query_id, event, 7)
    check_events(const_partial_expected_query_id, event, 7)
    check_events(const_partial2_expected_query_id, event, 7)
    check_events(count_expected_query_id, event, 7)

    # If file has only constant columns it is not read
    check_events(all_data_query_id, event, 5) # 1-2025, 6-2025 must not be read
    check_events(const_only_query_id, event, 0) # All must not be read
    check_events(const_partial_query_id, event, 4) # 1-2025, 6-2025 and 2-2025 must not be read
    check_events(const_partial2_query_id, event, 3) # 6-2025 must not be read, 1-2024, 1-2025, 2-2025 don't have new column 'name'
    check_events(count_query_id, event, 0) # All must not be read

    def compare_selects(query):
        result_expected = instance.query(f"{query} SETTINGS allow_experimental_iceberg_read_optimization=0")
        result_optimized = instance.query(f"{query} SETTINGS allow_experimental_iceberg_read_optimization=1")
        assert result_expected == result_optimized

    compare_selects(f"SELECT _path,* FROM {creation_expression} ORDER BY ALL")
    compare_selects(f"SELECT _path,* FROM {creation_expression} WHERE name_old='vasily' ORDER BY ALL")
    compare_selects(f"SELECT _path,* FROM {creation_expression} WHERE ((tag + length(name_old)) % 2 = 1) ORDER BY ALL")


@pytest.mark.parametrize("storage_type", ["s3", "azure"])
def test_cluster_joins(started_cluster, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_cluster_joins_" + storage_type + "_" + get_uuid_str()
    TABLE_NAME_2 = "test_cluster_joins_2_" + storage_type + "_" + get_uuid_str()
    TABLE_NAME_LOCAL = "test_cluster_joins_local_" + storage_type + "_" + get_uuid_str()

    def execute_spark_query(query: str, table_name):
        return execute_spark_query_general(
            spark,
            started_cluster,
            storage_type,
            table_name,
            query,
        )

    execute_spark_query(
        f"""
            CREATE TABLE {TABLE_NAME} (
                tag INT,
                name VARCHAR(50)
            )
            USING iceberg
            OPTIONS('format-version'='2')
        """, TABLE_NAME
    )

    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (1, 'john'),
        (2, 'jack')
    """, TABLE_NAME
    )

    execute_spark_query(
        f"""
            CREATE TABLE {TABLE_NAME_2} (
                id INT,
                second_name VARCHAR(50)
            )
            USING iceberg
            OPTIONS('format-version'='2')
        """, TABLE_NAME_2
    )

    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME_2} VALUES
        (1, 'dow'),
        (2, 'sparrow')
    """, TABLE_NAME_2
    )

    creation_expression = get_creation_expression(
        storage_type, TABLE_NAME, started_cluster, table_function=True, run_on_cluster=True
    )

    creation_expression_2 = get_creation_expression(
        storage_type, TABLE_NAME_2, started_cluster, table_function=True, run_on_cluster=True
    )

    instance.query(f"CREATE TABLE `{TABLE_NAME_LOCAL}` (id Int64, second_name String) ENGINE = Memory()")
    instance.query(f"INSERT INTO `{TABLE_NAME_LOCAL}` VALUES (1, 'silver'), (2, 'black')")

    res = instance.query(
        f"""
            SELECT t1.name,t2.second_name
            FROM {creation_expression} AS t1
                JOIN {creation_expression_2} AS t2
                ON t1.tag=t2.id
            WHERE t1.tag < 10 AND t2.id < 20
            ORDER BY ALL
            SETTINGS
                object_storage_cluster='cluster_simple',
                object_storage_cluster_join_mode='local'
        """
    )

    assert res == "jack\tsparrow\njohn\tdow\n"

    res = instance.query(
        f"""
            SELECT name
            FROM {creation_expression}
            WHERE tag in (
                SELECT id
                FROM {creation_expression_2}
            )
            ORDER BY ALL
            SETTINGS
                object_storage_cluster='cluster_simple',
                object_storage_cluster_join_mode='local'
        """
    )

    assert res == "jack\njohn\n"

    res = instance.query(
        f"""
            SELECT t1.name,t2.second_name
            FROM {creation_expression} AS t1
                JOIN `{TABLE_NAME_LOCAL}` AS t2
                ON t1.tag=t2.id
            WHERE t1.tag < 10 AND t2.id < 20
            ORDER BY ALL
            SETTINGS
                object_storage_cluster='cluster_simple',
                object_storage_cluster_join_mode='local'
        """
    )

    assert res == "jack\tblack\njohn\tsilver\n"

    res = instance.query(
        f"""
            SELECT name
            FROM {creation_expression}
            WHERE tag in (
                SELECT id
                FROM `{TABLE_NAME_LOCAL}`
            )
            ORDER BY ALL
            SETTINGS
                object_storage_cluster='cluster_simple',
                object_storage_cluster_join_mode='local'
        """
    )

    assert res == "jack\njohn\n"

    res = instance.query(
        f"""
            SELECT t1.name,t2.second_name
            FROM {creation_expression} AS t1
                CROSS JOIN `{TABLE_NAME_LOCAL}` AS t2
            WHERE t1.tag < 10 AND t2.id < 20
            ORDER BY ALL
            SETTINGS
                object_storage_cluster='cluster_simple',
                object_storage_cluster_join_mode='local'
        """
    )

    assert res == "jack\tblack\njack\tsilver\njohn\tblack\njohn\tsilver\n"


@pytest.mark.parametrize("storage_type", ["s3"])
def test_system_tables_partition_sorting_keys(started_cluster, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session

    table_name = f"test_sys_tables_keys_{storage_type}_{uuid.uuid4().hex[:8]}"
    fq_table = f"spark_catalog.default.{table_name}"

    spark.sql(f"DROP TABLE IF EXISTS {fq_table}")
    spark.sql(f"""
        CREATE TABLE {fq_table} (
            id INT,
            ts TIMESTAMP,
            payload STRING
        )
        USING iceberg
        PARTITIONED BY (bucket(16, id), day(ts))
        TBLPROPERTIES ('format-version' = '2')
    """)
    spark.sql(f"ALTER TABLE {fq_table} WRITE ORDERED BY (id DESC NULLS LAST, hour(ts))")
    spark.sql(f"""
        INSERT INTO {fq_table} VALUES
        (1, timestamp'2024-01-01 10:00:00', 'a'),
        (2, timestamp'2024-01-02 11:00:00', 'b'),
        (NULL, timestamp'2024-01-03 12:00:00', 'c')
    """)

    time.sleep(2)
    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{table_name}/",
        f"/iceberg_data/default/{table_name}/",
    )

    create_iceberg_table(storage_type, instance, table_name, started_cluster)

    res = instance.query(f"""
        SELECT partition_key, sorting_key
        FROM system.tables
        WHERE name = '{table_name}' FORMAT csv
    """).strip().lower()

    assert res == '"bucket(16, id), day(ts)","id desc, hour(ts) asc"'



@pytest.mark.parametrize(
    "storage_type",
    ["s3", "azure", "local"],
)
def test_partition_pruning_with_subquery_set(started_cluster, storage_type):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_partition_pruning_" + storage_type + "_" + get_uuid_str()
    IN_MEMORY_TABLE = "in_memory_table_" + get_uuid_str()

    def execute_spark_query(query: str):
        return execute_spark_query_general(
            spark,
            started_cluster,
            storage_type,
            TABLE_NAME,
            query,
        )

    execute_spark_query(
        f"""
            CREATE TABLE {TABLE_NAME} (
                id INT,
                data STRING
            )
            USING iceberg
            PARTITIONED BY (identity(id))
            OPTIONS('format-version'='2')
        """
    )

    execute_spark_query(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (1, 'a'),
        (2, 'b'),
        (3, 'c'),
        (4, 'd'),
        (5, 'e');
    """
    )

    creation_expression = get_creation_expression(
        storage_type, TABLE_NAME, started_cluster, table_function=True
    )

    instance.query(f"CREATE TABLE {IN_MEMORY_TABLE} (id INT) ENGINE = Memory")
    instance.query(f"INSERT INTO {IN_MEMORY_TABLE} VALUES (2), (4)")

    def check_validity_and_get_prunned_files(select_expression):
        settings1 = {
            "use_iceberg_partition_pruning": 0
        }
        settings2 = {
            "use_iceberg_partition_pruning": 1
        }
        return check_validity_and_get_prunned_files_general(
            instance, TABLE_NAME, settings1, settings2, 'IcebergPartitionPrunedFiles', select_expression
        )

    assert (
        check_validity_and_get_prunned_files(
            f"SELECT * FROM {creation_expression} WHERE id in (SELECT id FROM {IN_MEMORY_TABLE}) ORDER BY ALL"
        )
        == 3
    )


def test_iceberg_write_minmax(started_cluster):
    instance = started_cluster.instances["node1"]
    TABLE_NAME = "test_iceberg_write_minmax_" + get_uuid_str()

    create_iceberg_table("local", instance, TABLE_NAME, started_cluster, "(x Int32, y Int32)", partition_by="identity(x)")

    instance.query(f"INSERT INTO {TABLE_NAME} VALUES (1, 1), (1, 2)", settings={"allow_experimental_insert_into_iceberg": 1})

    res = instance.query(f"SELECT x,y FROM {TABLE_NAME} WHERE y=1 ORDER BY ALL").strip()
    assert res == "1\t1"

    res = instance.query(f"SELECT x,y FROM {TABLE_NAME} WHERE y=2 ORDER BY ALL").strip()
    assert res == "1\t2"


@pytest.mark.parametrize("format_version", ["1", "2"])
@pytest.mark.parametrize("storage_type", ["s3", "azure"])
@pytest.mark.parametrize("cluster_table_function_buckets_batch_size", [0, 100, 1000])
@pytest.mark.parametrize("input_format_parquet_use_native_reader_v3", [0, 1])
def test_cluster_table_function_split_by_row_groups(started_cluster, format_version, storage_type, cluster_table_function_buckets_batch_size,input_format_parquet_use_native_reader_v3):
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session

    TABLE_NAME = (
        "test_iceberg_cluster_"
        + format_version
        + "_"
        + storage_type
        + "_"
        + get_uuid_str()
    )

    def add_df(mode):
        write_iceberg_from_df(
            spark,
            generate_data(spark, 0, 100000),
            TABLE_NAME,
            mode=mode,
            format_version=format_version,
        )

        files = default_upload_directory(
            started_cluster,
            storage_type,
            f"/iceberg_data/default/{TABLE_NAME}/",
            f"/iceberg_data/default/{TABLE_NAME}/",
        )

        logging.info(f"Adding another dataframe. result files: {files}")

        return files

    files = add_df(mode="overwrite")
    for i in range(1, 5 * len(started_cluster.instances)):
        files = add_df(mode="append")

    clusters = instance.query(f"SELECT * FROM system.clusters")
    logging.info(f"Clusters setup: {clusters}")

    # Regular Query only node1
    table_function_expr = get_creation_expression(
        storage_type, TABLE_NAME, started_cluster, table_function=True
    )
    select_regular = (
        instance.query(f"SELECT * FROM {table_function_expr} ORDER BY ALL").strip().split()
    )

    # Cluster Query with node1 as coordinator
    table_function_expr_cluster = get_creation_expression(
        storage_type,
        TABLE_NAME,
        started_cluster,
        table_function=True,
        run_on_cluster=True,
    )
    instance.query("SYSTEM FLUSH LOGS")

    def get_buffers_count(func):
        buffers_count_before = int(
            instance.query(
                f"SELECT sum(ProfileEvents['EngineFileLikeReadFiles']) FROM system.query_log WHERE type = 'QueryFinish'"
            )
        )

        func()
        instance.query("SYSTEM FLUSH LOGS")
        buffers_count = int(
            instance.query(
                f"SELECT sum(ProfileEvents['EngineFileLikeReadFiles']) FROM system.query_log WHERE type = 'QueryFinish'"
            )
        )
        return buffers_count - buffers_count_before

    select_cluster = (
        instance.query(f"SELECT * FROM {table_function_expr_cluster} ORDER BY ALL SETTINGS input_format_parquet_use_native_reader_v3={input_format_parquet_use_native_reader_v3},cluster_table_function_split_granularity='bucket', cluster_table_function_buckets_batch_size={cluster_table_function_buckets_batch_size}").strip().split()
    )

    # Simple size check
    assert len(select_cluster) == len(select_regular)
    # Actual check
    assert select_cluster == select_regular

    buffers_count_with_splitted_tasks = get_buffers_count(lambda: instance.query(f"SELECT * FROM {table_function_expr_cluster} ORDER BY ALL SETTINGS input_format_parquet_use_native_reader_v3={input_format_parquet_use_native_reader_v3},cluster_table_function_split_granularity='bucket', cluster_table_function_buckets_batch_size={cluster_table_function_buckets_batch_size}").strip().split())
    buffers_count_default = get_buffers_count(lambda: instance.query(f"SELECT * FROM {table_function_expr_cluster} ORDER BY ALL SETTINGS input_format_parquet_use_native_reader_v3={input_format_parquet_use_native_reader_v3}, cluster_table_function_buckets_batch_size={cluster_table_function_buckets_batch_size}").strip().split())
    assert buffers_count_with_splitted_tasks > buffers_count_default


@pytest.mark.parametrize("storage_type", ["s3"])
def test_column_names_with_dots(started_cluster, storage_type):
    """
    Test that Iceberg tables with dot-separated column names are read correctly.
    This tests the fix for field ID-based column name mapping in Parquet V3 reader.
    """
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_column_names_with_dots_" + storage_type + "_" + get_uuid_str()

    # Create DataFrame with column names containing dots
    data = [
        (1, "value1", "multi_dot_value1"),
        (2, "value2", "multi_dot_value2"),
        (3, "value3", "multi_dot_value3"),
    ]
    schema = StructType(
        [
            StructField("id", IntegerType()),
            StructField("name.column", StringType()),
            StructField("double.column.dot", StringType()),
        ]
    )
    df = spark.createDataFrame(data=data, schema=schema)

    write_iceberg_from_df(spark, df, TABLE_NAME, mode="overwrite", format_version="2")

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    # Test via table function
    table_function_expr = get_creation_expression(
        storage_type, TABLE_NAME, started_cluster, table_function=True
    )

    # Verify single-dot column name
    result = instance.query(
        f"SELECT `name.column` FROM {table_function_expr} ORDER BY id"
    ).strip()
    assert result == "value1\nvalue2\nvalue3", f"Expected values, got: {result}"

    # Verify multi-dot column name
    result = instance.query(
        f"SELECT `double.column.dot` FROM {table_function_expr} ORDER BY id"
    ).strip()
    assert result == "multi_dot_value1\nmulti_dot_value2\nmulti_dot_value3", f"Expected values, got: {result}"

    # Verify all columns together
    result = instance.query(
        f"SELECT id, `name.column`, `double.column.dot` FROM {table_function_expr} ORDER BY id"
    ).strip()
    expected = "1\tvalue1\tmulti_dot_value1\n2\tvalue2\tmulti_dot_value2\n3\tvalue3\tmulti_dot_value3"
    assert result == expected, f"Expected:\n{expected}\nGot:\n{result}"

    # Test via table engine
    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)

    result = instance.query(
        f"SELECT `name.column`, `double.column.dot` FROM {TABLE_NAME} ORDER BY id"
    ).strip()
    expected = "value1\tmulti_dot_value1\nvalue2\tmulti_dot_value2\nvalue3\tmulti_dot_value3"
    assert result == expected, f"Expected:\n{expected}\nGot:\n{result}"


@pytest.mark.parametrize("storage_type", ["s3"])
def test_nested_struct_with_dotted_field(started_cluster, storage_type):
    """
    Test that nested struct fields with dot-separated names are read correctly.
    This tests the fix for prefix stripping (not splitName) in useColumnMapperIfNeeded.
    E.g., for my_struct.weird.field, we should return "weird.field" not just "field".
    """
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_nested_struct_dotted_field_" + storage_type + "_" + get_uuid_str()

    # Create DataFrame with nested struct containing a dotted field name
    nested_schema = StructType(
        [
            StructField("id", IntegerType()),
            StructField(
                "my_struct",
                StructType(
                    [
                        StructField("normal_field", IntegerType()),
                        StructField("weird.field", StringType()),
                    ]
                ),
            ),
        ]
    )

    data = [
        (1, (100, "nested_dot_value1")),
        (2, (200, "nested_dot_value2")),
        (3, (300, "nested_dot_value3")),
    ]
    df = spark.createDataFrame(data=data, schema=nested_schema)

    write_iceberg_from_df(spark, df, TABLE_NAME, mode="overwrite", format_version="2")

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    # Test via table function
    table_function_expr = get_creation_expression(
        storage_type, TABLE_NAME, started_cluster, table_function=True
    )

    # Verify nested struct with dotted field via table function
    result = instance.query(
        f"SELECT my_struct.normal_field, `my_struct.weird.field` FROM {table_function_expr} ORDER BY id"
    ).strip()
    expected = "100\tnested_dot_value1\n200\tnested_dot_value2\n300\tnested_dot_value3"
    assert result == expected, f"Expected:\n{expected}\nGot:\n{result}"

    # Test via table engine
    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)

    result = instance.query(
        f"SELECT my_struct.normal_field, `my_struct.weird.field` FROM {TABLE_NAME} ORDER BY id"
    ).strip()
    assert result == expected, f"Expected:\n{expected}\nGot:\n{result}"


@pytest.mark.parametrize("storage_type", ["s3"])
def test_deeply_nested_struct_with_dotted_names(started_cluster, storage_type):
    """
    Test deeply nested structs where EVERY level has dots in the name.
    Structure: my.struct -> some_dot.separated_parent -> weird.field
    Full path: my.struct.some_dot.separated_parent.weird.field

    This verifies that prefix stripping works correctly at all nesting depths.
    """
    instance = started_cluster.instances["node1"]
    spark = started_cluster.spark_session
    TABLE_NAME = "test_deeply_nested_dotted_" + storage_type + "_" + get_uuid_str()

    # Create 3-level nested schema with dots at every level
    nested_schema = StructType(
        [
            StructField("id", IntegerType()),
            StructField(
                "my.struct",
                StructType(
                    [
                        StructField(
                            "some_dot.separated_parent",
                            StructType(
                                [
                                    StructField("weird.field", StringType()),
                                ]
                            ),
                        ),
                    ]
                ),
            ),
        ]
    )

    data = [
        (1, (("deep_value1",),)),
        (2, (("deep_value2",),)),
        (3, (("deep_value3",),)),
    ]
    df = spark.createDataFrame(data=data, schema=nested_schema)

    write_iceberg_from_df(spark, df, TABLE_NAME, mode="overwrite", format_version="2")

    default_upload_directory(
        started_cluster,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    # Test via table function
    table_function_expr = get_creation_expression(
        storage_type, TABLE_NAME, started_cluster, table_function=True
    )

    # Query the deeply nested dotted field
    result = instance.query(
        f"SELECT `my.struct.some_dot.separated_parent.weird.field` FROM {table_function_expr} ORDER BY id"
    ).strip()
    expected = "deep_value1\ndeep_value2\ndeep_value3"
    assert result == expected, f"Expected:\n{expected}\nGot:\n{result}"

    # Test via table engine
    create_iceberg_table(storage_type, instance, TABLE_NAME, started_cluster)

    result = instance.query(
        f"SELECT `my.struct.some_dot.separated_parent.weird.field` FROM {TABLE_NAME} ORDER BY id"
    ).strip()
    assert result == expected, f"Expected:\n{expected}\nGot:\n{result}"
