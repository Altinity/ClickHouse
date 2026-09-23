#include <string_view>
#include "config.h"

#include <Core/Settings.h>
#include <Core/SettingsEnums.h>

#include <Analyzer/FunctionNode.h>
#include <Analyzer/TableFunctionNode.h>
#include <Interpreters/Context.h>
#include <Parsers/ASTSetQuery.h>

#include <TableFunctions/TableFunctionFactory.h>
#include <TableFunctions/TableFunctionObjectStorage.h>
#include <TableFunctions/TableFunctionObjectStorageCluster.h>
#include <TableFunctions/registerTableFunctions.h>

#include <Interpreters/parseColumnsListForTableFunction.h>

#include <Storages/ObjectStorage/Utils.h>
#include <Storages/NamedCollectionsHelpers.h>
#include <Storages/ObjectStorage/Azure/Configuration.h>
#include <Storages/ObjectStorage/HDFS/Configuration.h>
#include <Storages/ObjectStorage/Local/Configuration.h>
#include <Storages/ObjectStorage/S3/Configuration.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Storages/ObjectStorage/StorageObjectStorageCluster.h>
#include <Storages/ObjectStorage/DataLakes/DataLakeStorageSettings.h>
#include <Storages/ObjectStorage/DataLakes/DataLakeConfiguration.h>
#include <Storages/HivePartitioningUtils.h>


namespace DB
{

namespace Setting
{
    extern const SettingsUInt64 allow_experimental_parallel_reading_from_replicas;
    extern const SettingsBool parallel_replicas_for_cluster_engines;
    extern const SettingsString cluster_for_parallel_replicas;
    extern const SettingsParallelReplicasMode parallel_replicas_mode;
}

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

namespace DataLakeStorageSetting
{
    extern const DataLakeStorageSettingsString disk;
}

template <typename Definition, typename Configuration, bool is_data_lake>
ObjectStoragePtr TableFunctionObjectStorage<Definition, Configuration, is_data_lake>::getObjectStorage(const ContextPtr & context, bool create_readonly) const
{
    if (!object_storage)
        object_storage = configuration->createObjectStorage(context, create_readonly, std::nullopt);
    return object_storage;
}

template <typename Definition, typename Configuration, bool is_data_lake>
StorageObjectStorageConfigurationPtr TableFunctionObjectStorage<Definition, Configuration, is_data_lake>::getConfiguration(ContextPtr context) const
{
    if (!configuration)
    {
        if constexpr (is_data_lake)
        {
            const auto disk_name = settings && (*settings)[DataLakeStorageSetting::disk].changed
                ? (*settings)[DataLakeStorageSetting::disk].value
                : "";
            if (!disk_name.empty())
            {
                auto disk = context->getDisk(disk_name);
                switch (disk->getObjectStorage()->getType())
                {
#if USE_AWS_S3 && USE_AVRO
                case ObjectStorageType::S3:
                    if (Definition::object_storage_type != "s3")
                        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Disk type doesn't match with table engine type storage");

                    if (std::string_view(Definition::name).starts_with("iceberg"))
                        configuration = std::make_shared<StorageS3IcebergConfiguration>(settings);
#if USE_PARQUET
                    else
                        configuration = std::make_shared<StorageS3DeltaLakeConfiguration>(settings);
#endif
                    break;
#endif
#if USE_AZURE_BLOB_STORAGE && USE_AVRO
                case ObjectStorageType::Azure:
                    if (Definition::name != "iceberg" &&
                        Definition::name != "icebergCluster" &&
                        Definition::name != "deltaLake" &&
                        Definition::name != "deltaLakeCluster" &&
                        Definition::object_storage_type != "azure")
                        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Disk type doesn't match with table engine type storage");

                    if (std::string_view(Definition::name).starts_with("iceberg"))
                        configuration = std::make_shared<StorageAzureIcebergConfiguration>(settings);
#if USE_PARQUET
                    else
                        configuration = std::make_shared<StorageAzureDeltaLakeConfiguration>(settings);
#endif
                    break;
#endif
#if USE_AVRO
                case ObjectStorageType::Local:
                    if (Definition::name != "iceberg" &&
                        Definition::name != "icebergCluster" &&
                        Definition::name != "deltaLake" &&
                        Definition::name != "deltaLakeCluster" &&
                        Definition::object_storage_type != "local")
                        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Disk type doesn't match with table engine type storage");
                    if (std::string_view(Definition::name).starts_with("iceberg"))
                        configuration = std::make_shared<StorageLocalIcebergConfiguration>(settings);
#if USE_PARQUET
                    else
                        configuration = std::make_shared<StorageLocalDeltaLakeConfiguration>(settings);
#endif
                    break;
#endif
                default:
                    throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unsupported disk type for iceberg {}", disk->getObjectStorage()->getType());
                }
            }
            else
                configuration = std::make_shared<Configuration>(settings);
        }
        else
            configuration = std::make_shared<Configuration>();
    }
    return configuration;
}

template <typename Definition, typename Configuration, bool is_data_lake>
VectorWithMemoryTracking<size_t> TableFunctionObjectStorage<Definition, Configuration, is_data_lake>::skipAnalysisForArguments(
    const QueryTreeNodePtr & query_node_table_function, ContextPtr) const
{
    auto & table_function_node = query_node_table_function->as<TableFunctionNode &>();
    auto & table_function_arguments_nodes = table_function_node.getArguments().getNodes();
    size_t table_function_arguments_size = table_function_arguments_nodes.size();

    VectorWithMemoryTracking<size_t> result;
    for (size_t i = 0; i < table_function_arguments_size; ++i)
    {
        auto * function_node = table_function_arguments_nodes[i]->as<FunctionNode>();
        if (function_node && function_node->getFunctionName() == "headers")
            result.push_back(i);
    }
    return result;
}

template <typename Definition, typename Configuration, bool is_data_lake>
std::shared_ptr<typename TableFunctionObjectStorage<Definition, Configuration, is_data_lake>::Settings>
TableFunctionObjectStorage<Definition, Configuration, is_data_lake>::createEmptySettings()
{
    if constexpr (is_data_lake)
        return std::make_shared<DataLakeStorageSettings>();
    else
        return std::make_shared<StorageObjectStorageSettings>();
}

template <typename Definition, typename Configuration, bool is_data_lake>
void TableFunctionObjectStorage<Definition, Configuration, is_data_lake>::parseArguments(const ASTPtr & ast_function, ContextPtr context)
{
    /// Clone ast function, because we can modify its arguments like removing headers.
    auto ast_copy = ast_function->clone();
    ASTs & args_func = ast_copy->children;
    if (args_func.size() != 1)
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, "Table function '{}' must have arguments.", getName());

    settings = createEmptySettings();

    auto & args = args_func.at(0)->children;
    /// Support storage settings in table function,
    /// e.g. `s3(endpoint, ..., SETTINGS setting=value, ..., setting=value)`
    /// We do similarly for some other table functions
    /// whose storage implementation supports storage settings (for example, MySQL).
    for (auto it = args.begin(); it != args.end(); ++it)
    {
        ASTSetQuery * settings_ast = (*it)->as<ASTSetQuery>();
        if (settings_ast)
        {
            settings->loadFromQuery(*settings_ast);
            args.erase(it);
            break;
        }
    }
    parseArgumentsImpl(args, context);
}

template <typename Definition, typename Configuration, bool is_data_lake>
ColumnsDescription TableFunctionObjectStorage<
    Definition, Configuration, is_data_lake>::getActualTableStructure(ContextPtr context, bool is_insert_query) const
{
    if (configuration->getStructure() == "auto")
    {
        configuration->check(context);
        auto storage = getObjectStorage(context, !is_insert_query);
        configuration->lazyInitializeIfNeeded(object_storage, context);

        std::string sample_path;
        ColumnsDescription columns;
        resolveSchemaAndFormat(
            columns,
            std::move(storage),
            configuration,
            /* format_settings */std::nullopt,
            sample_path,
            context);

        HivePartitioningUtils::setupHivePartitioningForObjectStorage(
            columns,
            configuration,
            sample_path,
            /* inferred_schema */ true,
            /* format_settings */ std::nullopt,
            context);

        return columns;
    }
    return parseColumnsListFromString(configuration->getStructure(), context);
}

template <typename Definition, typename Configuration, bool is_data_lake>
StoragePtr TableFunctionObjectStorage<Definition, Configuration, is_data_lake>::executeImpl(
    const ASTPtr & /* ast_function */,
    ContextPtr context,
    const std::string & table_name,
    ColumnsDescription cached_columns,
    bool is_insert_query) const
{
    chassert(configuration);
    ColumnsDescription columns;

    if (configuration->getStructure() != "auto")
        columns = parseColumnsListFromString(configuration->getStructure(), context);
    else if (!structure_hint.empty())
        columns = structure_hint;
    else if (!cached_columns.empty())
        columns = cached_columns;

    StoragePtr storage;
    const auto & query_settings = context->getSettingsRef();

    const auto parallel_replicas_cluster_name = query_settings[Setting::cluster_for_parallel_replicas].toString();
    /// Only use parallel replicas if the Cluster variant of this table function exists
    /// (e.g. `s3Cluster` for `s3`). Table functions without a Cluster variant (e.g. `paimonLocal`)
    /// cannot distribute work via task iterators, so distributing would just read all data on every replica.
    /// `getName`, not `Definition::name`: with `TableFunctionObjectStorageClusterFallback`
    /// the definition is the *Cluster one, while the invoked function is `s3`.
    const auto can_use_parallel_replicas = !parallel_replicas_cluster_name.empty()
        && query_settings[Setting::parallel_replicas_for_cluster_engines]
        && context->canUseTaskBasedParallelReplicas()
        && !context->isDistributed()
        && TableFunctionFactory::instance().isTableFunctionName(getName() + "Cluster");

    const auto is_secondary_query = context->getClientInfo().query_kind == ClientInfo::QueryKind::SECONDARY_QUERY;

    if (can_use_parallel_replicas && !is_secondary_query && !is_insert_query)
    {
        storage = std::make_shared<StorageObjectStorageCluster>(
            parallel_replicas_cluster_name,
            configuration,
            getObjectStorage(context, !is_insert_query),
            StorageID(getDatabaseName(), table_name),
            columns,
            ConstraintsDescription{},
            partition_by,
            /* order_by */ nullptr,
            context,
            /* comment */ String{},
            /* format_settings */ std::nullopt, /// No format_settings
            /* mode */ LoadingStrictnessLevel::CREATE,
            configuration->getCatalog(context, StorageID(getDatabaseName(), table_name)),
            /* if_not_exists */ false,
            /* is_datalake_query*/ false,
            /* is_table_function */ true);

        storage->startup();
        return storage;
    }

    std::string disk_name;
    if constexpr (is_data_lake)
    {
        disk_name = settings && (*settings)[DataLakeStorageSetting::disk].changed
            ? (*settings)[DataLakeStorageSetting::disk].value
            : "";
    }

    ObjectStoragePtr current_object_storage;
    if (configuration->isDataLakeConfiguration() && !disk_name.empty())
        current_object_storage = context->getDisk(disk_name)->getObjectStorage();
    else
        current_object_storage = getObjectStorage(context, !is_insert_query);

    /// Note: distributed_processing is always false for non-cluster table functions (s3, azure, etc.).
    /// Cluster table functions (s3Cluster, etc.) handle distributed processing in their own getStorage() method.
    storage = std::make_shared<StorageObjectStorage>(
        configuration,
        current_object_storage,
        context,
        StorageID(getDatabaseName(), table_name),
        columns,
        ConstraintsDescription{},
        /* comment */ String{},
        /* format_settings */ std::nullopt,
        /* mode */ LoadingStrictnessLevel::CREATE,
        /* catalog*/ nullptr,
        /* if_not_exists*/ false,
        /* is_datalake_query*/ false,
        /* distributed_processing */ false,
        /* partition_by */ partition_by,
        /* order_by */ nullptr,
        /* is_table_function */true);

    storage->startup();
    return storage;
}

void registerTableFunctionObjectStorage(TableFunctionFactory & factory)
{
    UNUSED(factory);
#if USE_AWS_S3
    factory.registerFunction<TableFunctionObjectStorage<GCSDefinition, StorageS3Configuration, false>>(
        {.description = R"DOCS_MD(
Provides a table-like interface to `SELECT` and `INSERT` data from [Google Cloud Storage](https://cloud.google.com/storage/). Requires the [`Storage Object User` IAM role](https://cloud.google.com/storage/docs/access-control/iam-roles).

This is an alias of the [s3 table function](/reference/functions/table-functions/s3).

If you have multiple replicas in your cluster, you can use the [s3Cluster function](/reference/functions/table-functions/s3Cluster) (which works with GCS) instead to parallelize inserts.

## Syntax {#syntax}

```sql
gcs(url [, NOSIGN | hmac_key, hmac_secret] [,format] [,structure] [,compression_method] [,partition_strategy])
gcs(named_collection[, option=value [,..]])
```

<Tip>
**GCS**

The GCS Table Function integrates with Google Cloud Storage by using the GCS XML API and HMAC keys. 
See the [Google interoperability docs](https://cloud.google.com/storage/docs/interoperability) for more details about the endpoint and HMAC.
</Tip>

## Arguments {#arguments}

| Argument                     | Description                                                                                                                                                                              |
|------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `url`                        | Bucket path to file. Supports following wildcards in readonly mode: `*`, `**`, `?`, `{abc,def}` and `{N..M}` where `N`, `M` — numbers, `'abc'`, `'def'` — strings.                       |
| `NOSIGN`                     | If this keyword is provided in place of credentials, all the requests will not be signed.                                                                                                |
| `hmac_key` and `hmac_secret` | Keys that specify credentials to use with given endpoint. Optional.                                                                                                                      |
| `format`                     | The [format](/reference/formats/index) of the file.                                                                                                                                        |
| `structure`                  | Structure of the table. Format `'column1_name column1_type, column2_name column2_type, ...'`.                                                                                            |
| `compression_method`         | Parameter is optional. Supported values: `none`, `gzip` or `gz`, `brotli` or `br`, `xz` or `LZMA`, `zstd` or `zst`. By default, it will autodetect compression method by file extension. |
| `partition_strategy`         | Optional. Supported values: `wildcard` or `hive`. `wildcard` requires `{_partition_id}` in the path. Without an explicit strategy, a path with `{_partition_id}` uses `wildcard`. A path with another glob uses no partition strategy and ignores `PARTITION BY`. A path without a glob uses `hive` when `file_like_engine_default_partition_strategy` is `hive`; otherwise it uses no partition strategy. |

<Info>
**GCS**

The GCS path is in this format as the endpoint for the Google XML API is different than the JSON API:

```text
  https://storage.googleapis.com/<bucket>/<folder>/<filename(s)>
```

and not ~~https://storage.cloud.google.com~~.
</Info>

Arguments can also be passed using [named collections](/concepts/features/configuration/server-config/named-collections). In this case `url`, `format`, `structure`, `compression_method`, `partition_strategy` work in the same way, and some extra parameters are supported:

| Parameter                     | Description                                                                                                                                                                                                                       |
|-------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `access_key_id`               | `hmac_key`, optional.                                                                                                                                                                                                             |
| `secret_access_key`           | `hmac_secret`, optional.                                                                                                                                                                                                          |
| `filename`                    | Appended to the url if specified.                                                                                                                                                                                                 |
| `use_environment_credentials` | Enabled by default, allows passing extra parameters using environment variables `AWS_CONTAINER_CREDENTIALS_RELATIVE_URI`, `AWS_CONTAINER_CREDENTIALS_FULL_URI`, `AWS_CONTAINER_AUTHORIZATION_TOKEN`, `AWS_EC2_METADATA_DISABLED`. |
| `no_sign_request`             | Disabled by default.                                                                                                                                                                                                              |
| `expiration_window_seconds`   | Default value is 120.                                                                                                                                                                                                             |

## Returned value {#returned-value}

A table with the specified structure for reading or writing data in the specified file.

## Examples {#examples}

Selecting the first two rows from the GCS file `https://storage.googleapis.com/clickhouse_public_datasets/my-test-bucket-768/data.csv.gz`. The compression method is detected automatically from the `.gz` file extension:

```sql
SELECT *
FROM gcs('https://storage.googleapis.com/clickhouse_public_datasets/my-test-bucket-768/data.csv.gz', 'CSV', 'column1 UInt32, column2 UInt32, column3 UInt32')
LIMIT 2;
```

```text
┌─column1─┬─column2─┬─column3─┐
│       1 │       2 │       3 │
│       3 │       2 │       1 │
└─────────┴─────────┴─────────┘
```

The same query as above, but with the `gzip` compression method specified explicitly instead of relying on autodetection:

```sql
SELECT *
FROM gcs('https://storage.googleapis.com/clickhouse_public_datasets/my-test-bucket-768/data.csv.gz', 'CSV', 'column1 UInt32, column2 UInt32, column3 UInt32', 'gzip')
LIMIT 2;
```

```text
┌─column1─┬─column2─┬─column3─┐
│       1 │       2 │       3 │
│       3 │       2 │       1 │
└─────────┴─────────┴─────────┘
```

## Usage {#usage}

Suppose that we have several files with following URIs on GCS:

-   'https://storage.googleapis.com/my-test-bucket-768/some_prefix/some_file_1.csv'
-   'https://storage.googleapis.com/my-test-bucket-768/some_prefix/some_file_2.csv'
-   'https://storage.googleapis.com/my-test-bucket-768/some_prefix/some_file_3.csv'
-   'https://storage.googleapis.com/my-test-bucket-768/some_prefix/some_file_4.csv'
-   'https://storage.googleapis.com/my-test-bucket-768/another_prefix/some_file_1.csv'
-   'https://storage.googleapis.com/my-test-bucket-768/another_prefix/some_file_2.csv'
-   'https://storage.googleapis.com/my-test-bucket-768/another_prefix/some_file_3.csv'
-   'https://storage.googleapis.com/my-test-bucket-768/another_prefix/some_file_4.csv'

Count the amount of rows in files ending with numbers from 1 to 3:

```sql
SELECT count(*)
FROM gcs('https://storage.googleapis.com/clickhouse_public_datasets/my-test-bucket-768/{some,another}_prefix/some_file_{1..3}.csv', 'CSV', 'column1 UInt32, column2 UInt32, column3 UInt32')
```

```text
┌─count()─┐
│      18 │
└─────────┘
```

Count the total amount of rows in all files in these two directories:

```sql
SELECT count(*)
FROM gcs('https://storage.googleapis.com/clickhouse_public_datasets/my-test-bucket-768/{some,another}_prefix/*', 'CSV', 'column1 UInt32, column2 UInt32, column3 UInt32')
```

```text
┌─count()─┐
│      24 │
└─────────┘
```

<Warning>
If your listing of files contains number ranges with leading zeros, use the construction with braces for each digit separately or use `?`.
</Warning>

Count the total amount of rows in files named `file-000.csv`, `file-001.csv`, ... , `file-999.csv`:

```sql
SELECT count(*)
FROM gcs('https://storage.googleapis.com/clickhouse_public_datasets/my-test-bucket-768/big_prefix/file-{000..999}.csv', 'CSV', 'name String, value UInt32');
```

```text
┌─count()─┐
│      12 │
└─────────┘
```

Insert data into file `test-data.csv.gz`:

```sql
INSERT INTO FUNCTION gcs('https://storage.googleapis.com/my-test-bucket-768/test-data.csv.gz', 'CSV', 'name String, value UInt32', 'gzip')
VALUES ('test-data', 1), ('test-data-2', 2);
```

Insert data into file `test-data.csv.gz` from existing table:

```sql
INSERT INTO FUNCTION gcs('https://storage.googleapis.com/my-test-bucket-768/test-data.csv.gz', 'CSV', 'name String, value UInt32', 'gzip')
SELECT name, value FROM existing_table;
```

Glob ** can be used for recursive directory traversal. Consider the below example, it will fetch all files from `my-test-bucket-768` directory recursively:

```sql
SELECT * FROM gcs('https://storage.googleapis.com/my-test-bucket-768/**', 'CSV', 'name String, value UInt32', 'gzip');
```

The below get data from all `test-data.csv.gz` files from any folder inside `my-test-bucket` directory recursively:

```sql
SELECT * FROM gcs('https://storage.googleapis.com/my-test-bucket-768/**/test-data.csv.gz', 'CSV', 'name String, value UInt32', 'gzip');
```

For production use cases it is recommended to use [named collections](/concepts/features/configuration/server-config/named-collections). Here is the example:
```sql

CREATE NAMED COLLECTION creds AS
        access_key_id = '***',
        secret_access_key = '***';
SELECT count(*)
FROM gcs(creds, url='https://s3-object-url.csv')
```

## Partitioned Write {#partitioned-write}

If you specify `PARTITION BY` expression when inserting data into `GCS` table, a separate file is created for each partition value. Splitting the data into separate files helps to improve reading operations efficiency.

A path containing `{_partition_id}` implies the `wildcard` partition strategy, so the explicit `partition_strategy='wildcard'` in the examples below is optional. A path with another glob uses no partition strategy and ignores `PARTITION BY`. A path without a glob uses `hive` when `file_like_engine_default_partition_strategy` is `hive`; otherwise it uses no partition strategy.

**Examples**

1. Using partition ID in a key creates separate files:

```sql
INSERT INTO TABLE FUNCTION
    gcs('http://bucket.amazonaws.com/my_bucket/file_{_partition_id}.csv', 'CSV', 'a String, b UInt32, c UInt32', partition_strategy='wildcard')
    PARTITION BY a VALUES ('x', 2, 3), ('x', 4, 5), ('y', 11, 12), ('y', 13, 14), ('z', 21, 22), ('z', 23, 24);
```
As a result, the data is written into three files: `file_x.csv`, `file_y.csv`, and `file_z.csv`.

2. Using partition ID in a bucket name creates files in different buckets:

```sql
INSERT INTO TABLE FUNCTION
    gcs('http://bucket.amazonaws.com/my_bucket_{_partition_id}/file.csv', 'CSV', 'a UInt32, b UInt32, c UInt32', partition_strategy='wildcard')
    PARTITION BY a VALUES (1, 2, 3), (1, 4, 5), (10, 11, 12), (10, 13, 14), (20, 21, 22), (20, 23, 24);
```
As a result, the data is written into three files in different buckets: `my_bucket_1/file.csv`, `my_bucket_10/file.csv`, and `my_bucket_20/file.csv`.

## Related {#related}
- [S3 table function](/reference/functions/table-functions/s3)
- [S3 engine](/reference/engines/table-engines/integrations/s3)
)DOCS_MD", .category = FunctionDocumentation::Category::TableFunction},
        {.allow_readonly = false}
    );

    factory.registerFunction<TableFunctionObjectStorage<COSNDefinition, StorageS3Configuration, false>>(
        {
            .description=R"(The table function can be used to read the data stored on COSN.)",
            .examples{{COSNDefinition::name, "SELECT * FROM cosn(url, access_key_id, secret_access_key)", ""}},
            .category = FunctionDocumentation::Category::TableFunction
        },
        {.allow_readonly = false}
    );

    factory.registerFunction<TableFunctionObjectStorage<OSSDefinition, StorageS3Configuration, false>>(
        {
            .description=R"(The table function can be used to read the data stored on OSS.)",
            .examples{{OSSDefinition::name, "SELECT * FROM oss(url, access_key_id, secret_access_key)", ""}},
            .category = FunctionDocumentation::Category::TableFunction
        },
        {.allow_readonly = false}
    );
#endif
}

#if USE_AZURE_BLOB_STORAGE
template class TableFunctionObjectStorage<AzureDefinition, StorageAzureConfiguration, false>;
template class TableFunctionObjectStorage<AzureClusterDefinition, StorageAzureConfiguration, false>;
#endif

#if USE_AWS_S3
template class TableFunctionObjectStorage<S3Definition, StorageS3Configuration, false>;
template class TableFunctionObjectStorage<S3ClusterDefinition, StorageS3Configuration, false>;
template class TableFunctionObjectStorage<GCSDefinition, StorageS3Configuration, false>;
template class TableFunctionObjectStorage<COSNDefinition, StorageS3Configuration, false>;
template class TableFunctionObjectStorage<OSSDefinition, StorageS3Configuration, false>;
#endif

#if USE_HDFS
template class TableFunctionObjectStorage<HDFSDefinition, StorageHDFSConfiguration, false>;
template class TableFunctionObjectStorage<HDFSClusterDefinition, StorageHDFSConfiguration, false>;
#endif

#if USE_AVRO
template class TableFunctionObjectStorage<IcebergClusterDefinition, StorageIcebergConfiguration, true>;
#endif

#if USE_AVRO
template class TableFunctionObjectStorage<IcebergLocalClusterDefinition, StorageLocalIcebergConfiguration, true>;
#endif

#if USE_AVRO && USE_AWS_S3
template class TableFunctionObjectStorage<IcebergS3ClusterDefinition, StorageS3IcebergConfiguration, true>;
template class TableFunctionObjectStorage<IcebergClusterDefinition, StorageS3IcebergConfiguration, true>;
#endif

#if USE_AVRO && USE_AZURE_BLOB_STORAGE
template class TableFunctionObjectStorage<IcebergAzureClusterDefinition, StorageAzureIcebergConfiguration, true>;
#endif

#if USE_AVRO && USE_HDFS
template class TableFunctionObjectStorage<IcebergHDFSClusterDefinition, StorageHDFSIcebergConfiguration, true>;
#endif

#if USE_AVRO && USE_AWS_S3
template class TableFunctionObjectStorage<PaimonS3ClusterDefinition, StorageS3PaimonConfiguration, true>;
template class TableFunctionObjectStorage<PaimonClusterDefinition, StorageS3PaimonConfiguration, true>;
#endif

#if USE_AVRO && USE_AZURE_BLOB_STORAGE
template class TableFunctionObjectStorage<PaimonAzureClusterDefinition, StorageAzurePaimonConfiguration, true>;
#endif

#if USE_AVRO && USE_HDFS
template class TableFunctionObjectStorage<PaimonHDFSClusterDefinition, StorageHDFSPaimonConfiguration, true>;
#endif

#if USE_PARQUET && USE_AWS_S3 && USE_DELTA_KERNEL_RS
template class TableFunctionObjectStorage<DeltaLakeClusterDefinition, StorageS3DeltaLakeConfiguration, true>;
template class TableFunctionObjectStorage<DeltaLakeS3ClusterDefinition, StorageS3DeltaLakeConfiguration, true>;
#endif

#if USE_PARQUET && USE_AZURE_BLOB_STORAGE && USE_DELTA_KERNEL_RS
template class TableFunctionObjectStorage<DeltaLakeAzureClusterDefinition, StorageAzureDeltaLakeConfiguration, true>;
#endif

#if USE_AWS_S3
template class TableFunctionObjectStorage<HudiClusterDefinition, StorageS3HudiConfiguration, true>;
#endif


#if USE_AVRO
void registerTableFunctionPaimon(TableFunctionFactory & factory);
void registerTableFunctionPaimon(TableFunctionFactory & factory)
{
#if USE_AWS_S3
    factory.registerFunction<TableFunctionPaimon>(
         {.description = R"DOCS_MD(
import { ExperimentalBadge } from "/snippets/components/ExperimentalBadge/ExperimentalBadge.jsx";

<ExperimentalBadge />

Provides a read-only table-like interface to Apache [Paimon](https://paimon.apache.org/) tables in Amazon S3, Azure, HDFS or locally stored.

## Syntax {#syntax}

```sql
paimon(url [,access_key_id, secret_access_key] [,format] [,structure] [,compression] [,extra_credentials])

paimonS3(url [,access_key_id, secret_access_key] [,format] [,structure] [,compression] [,extra_credentials])

paimonAzure(connection_string|storage_account_url, container_name, blobpath, [,account_name], [,account_key] [,format] [,compression_method])

paimonHDFS(path_to_table, [,format] [,compression_method])

paimonLocal(path_to_table, [,format] [,compression_method])
```

## Arguments {#arguments}

Description of the arguments coincides with description of arguments in table functions `s3`, `azureBlobStorage`, `HDFS` and `file` correspondingly.
`format` stands for the format of data files in the Paimon table.

For `paimonS3`, an optional `extra_credentials` parameter can be used to pass a `role_arn` for role-based access in ClickHouse Cloud. See [Secure S3](/products/cloud/guides/data-sources/accessing-s3-data-securely) for configuration steps.

### Returned value {#returned-value}

A table with the specified structure for reading data in the specified Paimon table.

## Defining a named collection {#defining-a-named-collection}

Here is an example of configuring a named collection for storing the URL and credentials:

```xml
<clickhouse>
    <named_collections>
        <paimon_conf>
            <url>http://test.s3.amazonaws.com/clickhouse-bucket/</url>
            <access_key_id>test</access_key_id>
            <secret_access_key>test</secret_access_key>
            <format>auto</format>
            <structure>auto</structure>
        </paimon_conf>
    </named_collections>
</clickhouse>
```

```sql
SELECT * FROM paimonS3(paimon_conf, filename = 'test_table')
DESCRIBE paimonS3(paimon_conf, filename = 'test_table')
```

## Aliases {#aliases}

Table function `paimon` is an alias to `paimonS3` now.

## Virtual Columns {#virtual-columns}

- `_path` — Path to the file. Type: `LowCardinality(String)`.
- `_file` — Name of the file. Type: `LowCardinality(String)`.
- `_size` — Size of the file in bytes. Type: `Nullable(UInt64)`. If the file size is unknown, the value is `NULL`.
- `_time` — Last modified time of the file. Type: `Nullable(DateTime)`. If the time is unknown, the value is `NULL`.
- `_etag` — The etag of the file. Type: `LowCardinality(String)`. If the etag is unknown, the value is `NULL`.

## Data Types supported {#data-types-supported}

| Paimon Data Type | ClickHouse Data Type
|-------|--------|
|BOOLEAN     |Int8      |
|TINYINT     |Int8      |
|SMALLINT     |Int16      |
|INTEGER     |Int32      |
|BIGINT     |Int64      |
|FLOAT     |Float32      |
|DOUBLE     |Float64      |
|STRING,VARCHAR,BYTES,VARBINARY     |String      |
|DATE     |Date      |
|TIME(p),TIME     |Time('UTC')      |
|TIMESTAMP(p) WITH LOCAL TIME ZONE     |DateTime64      |
|TIMESTAMP(p)     |DateTime64('UTC')      |
|CHAR     |FixedString(1)      |
|BINARY(n)     |FixedString(n)      |
|DECIMAL(P,S)     |Decimal(P,S)      |
|ARRAY     |Array      |
|MAP     |Map    |

## Partition supported {#partition-supported}
Data types supported in Paimon partition keys:
* `CHAR`
* `VARCHAR`
* `BOOLEAN`
* `DECIMAL`
* `TINYINT`
* `SMALLINT`
* `INTEGER`
* `DATE`
* `TIME`
* `TIMESTAMP`
* `TIMESTAMP WITH LOCAL TIME ZONE`
* `BIGINT`
* `FLOAT`
* `DOUBLE`

## See Also {#see-also}

* [Paimon cluster table function](/reference/functions/table-functions/paimonCluster)
)DOCS_MD", .category = FunctionDocumentation::Category::TableFunction},
         {.allow_readonly = false});
    factory.registerFunction<TableFunctionPaimonS3>(
         {.description = R"(The table function can be used to read the Paimon table stored on S3 object store.)",
            .examples{{"paimonS3", "SELECT * FROM paimonS3(url, access_key_id, secret_access_key)", ""}},
            .category = FunctionDocumentation::Category::TableFunction},
         {.allow_readonly = false});

#endif
#if USE_AZURE_BLOB_STORAGE
    factory.registerFunction<TableFunctionPaimonAzure>(
         {.description = R"(The table function can be used to read the Paimon table stored on Azure object store.)",
            .examples{{"paimonAzure", "SELECT * FROM paimonAzure(url, access_key_id, secret_access_key)", ""}},
            .category = FunctionDocumentation::Category::TableFunction},
         {.allow_readonly = false});
#endif
#if USE_HDFS
    factory.registerFunction<TableFunctionPaimonHDFS>(
         {.description = R"(The table function can be used to read the Paimon table stored on HDFS virtual filesystem.)",
            .examples{{"paimonHDFS", "SELECT * FROM paimonHDFS(url)", ""}},
            .category = FunctionDocumentation::Category::TableFunction},
         {.allow_readonly = false});
#endif
    factory.registerFunction<TableFunctionPaimonLocal>(
         {.description = R"(The table function can be used to read the Paimon table stored locally.)",
            .examples{{"paimonLocal", "SELECT * FROM paimonLocal(filename)", ""}},
            .category = FunctionDocumentation::Category::TableFunction},
         {.allow_readonly = false});
}
#endif

#if USE_PARQUET && USE_DELTA_KERNEL_RS
void registerTableFunctionDeltaLake(TableFunctionFactory & factory);
void registerTableFunctionDeltaLake(TableFunctionFactory & factory)
{
    // Register the new local Delta Lake table function
    factory.registerFunction<TableFunctionDeltaLakeLocal>(
         {.description = R"(The table function can be used to read the DeltaLake table stored locally.)",
            .examples{{DeltaLakeLocalDefinition::name, "SELECT * FROM deltaLakeLocal(path)", ""}},
            .category = FunctionDocumentation::Category::TableFunction},
         {.allow_readonly = false});
}
#endif

void registerDataLakeTableFunctions(TableFunctionFactory & factory)
{
    UNUSED(factory);

#if USE_PARQUET && USE_DELTA_KERNEL_RS
    registerTableFunctionDeltaLake(factory);
#endif
#if USE_AVRO
    registerTableFunctionPaimon(factory);
#endif
}
}
