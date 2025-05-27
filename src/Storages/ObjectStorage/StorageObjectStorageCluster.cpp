#include "Storages/ObjectStorage/StorageObjectStorageCluster.h"

#include <Common/Exception.h>
#include <Core/Settings.h>
#include <Formats/FormatFactory.h>
#include <Parsers/queryToString.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTFunction.h>
#include <Processors/Sources/RemoteSource.h>
#include <QueryPipeline/RemoteQueryExecutor.h>
#include <TableFunctions/TableFunctionFactory.h>
#include <Interpreters/ClusterProxy/SelectStreamFactory.h>

#include <Storages/VirtualColumnUtils.h>
#include <Storages/ObjectStorage/Utils.h>
#include <Storages/ObjectStorage/StorageObjectStorageSource.h>
#include <Storages/extractTableFunctionArgumentsFromSelectQuery.h>
#include <Storages/ObjectStorage/StorageObjectStorageStableTaskDistributor.h>


namespace DB
{
namespace Setting
{
    extern const SettingsBool use_hive_partitioning;
    extern const SettingsString object_storage_cluster;
}

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int UNKNOWN_FUNCTION;
    extern const int NOT_IMPLEMENTED;
}


String StorageObjectStorageCluster::getPathSample(StorageInMemoryMetadata metadata, ContextPtr context)
{
    auto query_settings = configuration->getQuerySettings(context);
    /// We don't want to throw an exception if there are no files with specified path.
    query_settings.throw_on_zero_files_match = false;

    if (!configuration->isArchive() && !configuration->isPathWithGlobs())
        return configuration->getPath();

    auto file_iterator = StorageObjectStorageSource::createFileIterator(
        configuration,
        query_settings,
        object_storage,
        false, // distributed_processing
        context,
        {}, // predicate
        {},
        metadata.getColumns().getAll(), // virtual_columns
        nullptr, // read_keys
        {} // file_progress_callback
    );

    if (auto file = file_iterator->next(0))
        return file->getPath();

    return "";
}

StorageObjectStorageCluster::StorageObjectStorageCluster(
    const String & cluster_name_,
    ConfigurationPtr configuration_,
    ObjectStoragePtr object_storage_,
    ContextPtr context_,
    const StorageID & table_id_,
    const ColumnsDescription & columns_,
    const ConstraintsDescription & constraints_,
    const String & comment_,
    std::optional<FormatSettings> format_settings_,
    LoadingStrictnessLevel mode_,
    ASTPtr partition_by_
)
    : IStorageCluster(
        cluster_name_, table_id_, getLogger(fmt::format("{}({})", configuration_->getEngineName(), table_id_.table_name)))
    , configuration{configuration_}
    , object_storage(object_storage_)
    , cluster_name_in_settings(false)
{
    ColumnsDescription columns{columns_};
    std::string sample_path;
    resolveSchemaAndFormat(columns, object_storage, configuration, {}, sample_path, context_);
    configuration->check(context_);

    StorageInMemoryMetadata metadata;
    metadata.setColumns(columns);
    metadata.setConstraints(constraints_);

    if (sample_path.empty()
            && context_->getSettingsRef()[Setting::use_hive_partitioning]
            && !configuration->withPartitionWildcard())
        sample_path = getPathSample(metadata, context_);

    setVirtuals(VirtualColumnUtils::getVirtualsForFileLikeStorage(metadata.columns, context_, sample_path));
    setInMemoryMetadata(metadata);

    pure_storage = std::make_shared<StorageObjectStorage>(
        configuration,
        object_storage,
        context_,
        getStorageID(),
        getInMemoryMetadata().getColumns(),
        getInMemoryMetadata().getConstraints(),
        comment_,
        format_settings_,
        mode_,
        /* distributed_processing */false,
        partition_by_,
        /* is_table_function */false,
        /* lazy_init */false,
        sample_path);

    auto virtuals_ = getVirtualsPtr();
    if (virtuals_)
        pure_storage->setVirtuals(*virtuals_);
    pure_storage->setInMemoryMetadata(getInMemoryMetadata());
}

std::string StorageObjectStorageCluster::getName() const
{
    return configuration->getEngineName();
}

void StorageObjectStorageCluster::updateQueryForDistributedEngineIfNeeded(ASTPtr & query, ContextPtr context)
{
    // Change table engine on table function for distributed request
    // CREATE TABLE t (...) ENGINE=IcebergS3(...)
    // SELECT * FROM t
    // change on
    // SELECT * FROM icebergS3(...)
    // to execute on cluster nodes

    auto * select_query = query->as<ASTSelectQuery>();
    if (!select_query || !select_query->tables())
        return;

    auto * tables = select_query->tables()->as<ASTTablesInSelectQuery>();

    if (tables->children.empty())
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Expected SELECT query from table with engine {}, got '{}'",
            configuration->getEngineName(), queryToString(query));

    auto * table_expression = tables->children[0]->as<ASTTablesInSelectQueryElement>()->table_expression->as<ASTTableExpression>();

    if (!table_expression)
        return;

    if (!table_expression->database_and_table_name)
        return;

    auto & table_identifier_typed = table_expression->database_and_table_name->as<ASTTableIdentifier &>();

    auto table_alias = table_identifier_typed.tryGetAlias();

    auto storage_engine_name = configuration->getEngineName();
    if (storage_engine_name == "Iceberg")
    {
        switch (configuration->getType())
        {
            case ObjectStorageType::S3:
                storage_engine_name = "IcebergS3";
                break;
            case ObjectStorageType::Azure:
                storage_engine_name = "IcebergAzure";
                break;
            case ObjectStorageType::HDFS:
                storage_engine_name = "IcebergHDFS";
                break;
            default:
                throw Exception(
                    ErrorCodes::LOGICAL_ERROR,
                    "Can't find table function for engine {}",
                    storage_engine_name
                );
        }
    }

    static std::unordered_map<std::string, std::string> engine_to_function = {
        {"S3", "s3"},
        {"Azure", "azureBlobStorage"},
        {"HDFS", "hdfs"},
        {"Iceberg", "iceberg"},
        {"IcebergS3", "icebergS3"},
        {"IcebergAzure", "icebergAzure"},
        {"IcebergHDFS", "icebergHDFS"},
        {"DeltaLake", "deltaLake"},
        {"Hudi", "hudi"}
    };

    auto p = engine_to_function.find(storage_engine_name);
    if (p == engine_to_function.end())
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Can't find table function for engine {}",
            storage_engine_name
        );
    }

    std::string table_function_name = p->second;

    auto function_ast = std::make_shared<ASTFunction>();
    function_ast->name = table_function_name;

    auto cluster_name = getClusterName(context);

    if (cluster_name.empty())
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Can't be here without cluster name, no cluster name in query {}",
            queryToString(query));
    }

    function_ast->arguments = configuration->createArgsWithAccessData();
    function_ast->children.push_back(function_ast->arguments);
    function_ast->setAlias(table_alias);

    ASTPtr function_ast_ptr(function_ast);

    table_expression->database_and_table_name = nullptr;
    table_expression->table_function = function_ast_ptr;
    table_expression->children[0] = function_ast_ptr;

    auto settings = select_query->settings();
    if (settings)
    {
        auto & settings_ast = settings->as<ASTSetQuery &>();
        settings_ast.changes.insertSetting("object_storage_cluster", cluster_name);
    }
    else
    {
        auto settings_ast_ptr = std::make_shared<ASTSetQuery>();
        settings_ast_ptr->is_standalone = false;
        settings_ast_ptr->changes.setSetting("object_storage_cluster", cluster_name);
        select_query->setExpression(ASTSelectQuery::Expression::SETTINGS, std::move(settings_ast_ptr));
    }

    cluster_name_in_settings = true;
}

void StorageObjectStorageCluster::updateQueryToSendIfNeeded(
    ASTPtr & query,
    const DB::StorageSnapshotPtr & storage_snapshot,
    const ContextPtr & context)
{
    updateQueryForDistributedEngineIfNeeded(query, context);

    ASTExpressionList * expression_list = extractTableFunctionArgumentsFromSelectQuery(query);

    if (!expression_list)
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Expected SELECT query from table function {}, got '{}'",
            configuration->getEngineName(), queryToString(query));
    }

    ASTs & args = expression_list->children;
    const auto & structure = storage_snapshot->metadata->getColumns().getAll().toNamesAndTypesDescription();
    if (args.empty())
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Unexpected empty list of arguments for {}Cluster table function",
            configuration->getEngineName());
    }

    ASTPtr object_storage_type_arg;
    configuration->extractDynamicStorageType(args, context, &object_storage_type_arg);
    if (cluster_name_in_settings)
    {
        configuration->addStructureAndFormatToArgsIfNeeded(args, structure, configuration->getFormat(), context, /*with_structure=*/true);

        /// Convert to old-stype *Cluster table function.
        /// This allows to use old clickhouse versions in cluster.
        static std::unordered_map<std::string, std::string> function_to_cluster_function = {
            {"s3", "s3Cluster"},
            {"azureBlobStorage", "azureBlobStorageCluster"},
            {"hdfs", "hdfsCluster"},
            {"iceberg", "icebergCluster"},
            {"icebergS3", "icebergS3Cluster"},
            {"icebergAzure", "icebergAzureCluster"},
            {"icebergHDFS", "icebergHDFSCluster"},
            {"deltaLake", "deltaLakeCluster"},
            {"hudi", "hudiCluster"},
        };

        ASTFunction * table_function = extractTableFunctionFromSelectQuery(query);

        auto p = function_to_cluster_function.find(table_function->name);
        if (p == function_to_cluster_function.end())
        {
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Can't find cluster name for table function {}",
                table_function->name);
        }

        table_function->name = p->second;

        auto cluster_name = getClusterName(context);
        auto cluster_name_arg = std::make_shared<ASTLiteral>(cluster_name);
        args.insert(args.begin(), cluster_name_arg);

        auto * select_query = query->as<ASTSelectQuery>();
        if (!select_query)
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Expected SELECT query from table function {}",
                configuration->getEngineName());

        auto settings = select_query->settings();
        if (settings)
        {
            auto & settings_ast = settings->as<ASTSetQuery &>();
            if (settings_ast.changes.removeSetting("object_storage_cluster") && settings_ast.changes.empty())
            {
                select_query->setExpression(ASTSelectQuery::Expression::SETTINGS, {});
            }
            /// No throw if not found - `object_storage_cluster` can be global setting.
        }
    }
    else
    {
        ASTPtr cluster_name_arg = args.front();
        args.erase(args.begin());
        configuration->addStructureAndFormatToArgsIfNeeded(args, structure, configuration->getFormat(), context, /*with_structure=*/true);
        args.insert(args.begin(), cluster_name_arg);
    }
    if (object_storage_type_arg)
        args.insert(args.end(), object_storage_type_arg);
}

RemoteQueryExecutor::Extension StorageObjectStorageCluster::getTaskIteratorExtension(
    const ActionsDAG::Node * predicate,
    const std::optional<ActionsDAG> & filter_actions_dag,
    const ContextPtr & local_context,
    ClusterPtr cluster) const
{
    auto iterator = StorageObjectStorageSource::createFileIterator(
        configuration, configuration->getQuerySettings(local_context), object_storage, /* distributed_processing */false,
        local_context, predicate, filter_actions_dag, getVirtualsList(), nullptr, local_context->getFileProgressCallback());

    std::vector<std::string> ids_of_hosts;
    for (const auto & shard : cluster->getShardsInfo())
    {
        if (shard.per_replica_pools.empty())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Cluster {} with empty shard {}", cluster->getName(), shard.shard_num);
        for (const auto & replica : shard.per_replica_pools)
        {
            if (!replica)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Cluster {}, shard {} with empty node", cluster->getName(), shard.shard_num);
            ids_of_hosts.push_back(replica->getAddress());
        }
    }

    auto task_distributor = std::make_shared<StorageObjectStorageStableTaskDistributor>(iterator, ids_of_hosts);

    auto callback = std::make_shared<TaskIterator>(
        [task_distributor](size_t number_of_current_replica) mutable -> String {
            return task_distributor->getNextTask(number_of_current_replica).value_or("");
        });

    return RemoteQueryExecutor::Extension{ .task_iterator = std::move(callback) };
}

void StorageObjectStorageCluster::readFallBackToPure(
    QueryPlan & query_plan,
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & query_info,
    ContextPtr context,
    QueryProcessingStage::Enum processed_stage,
    size_t max_block_size,
    size_t num_streams)
{
    pure_storage->read(query_plan, column_names, storage_snapshot, query_info, context, processed_stage, max_block_size, num_streams);
}

SinkToStoragePtr StorageObjectStorageCluster::writeFallBackToPure(
    const ASTPtr & query,
    const StorageMetadataPtr & metadata_snapshot,
    ContextPtr context,
    bool async_insert)
{
    return pure_storage->write(query, metadata_snapshot, context, async_insert);
}

String StorageObjectStorageCluster::getClusterName(ContextPtr context) const
{
    /// StorageObjectStorageCluster is always created for cluster or non-cluster variants.
    /// User can specify cluster name in table definition or in setting `object_storage_cluster`
    /// only for several queries. When it specified in both places, priority is given to the query setting.
    /// When it is empty, non-cluster realization is used.
    auto cluster_name_from_settings = context->getSettingsRef()[Setting::object_storage_cluster].value;
    if (cluster_name_from_settings.empty())
        cluster_name_from_settings = getOriginalClusterName();
    return cluster_name_from_settings;
}

QueryProcessingStage::Enum StorageObjectStorageCluster::getQueryProcessingStage(
    ContextPtr context, QueryProcessingStage::Enum to_stage, const StorageSnapshotPtr & storage_snapshot, SelectQueryInfo & query_info) const
{
    /// Full query if fall back to pure storage.
    if (getClusterName(context).empty())
        return QueryProcessingStage::Enum::FetchColumns;

    /// Distributed storage.
    return IStorageCluster::getQueryProcessingStage(context, to_stage, storage_snapshot, query_info);
}

void StorageObjectStorageCluster::truncate(
    const ASTPtr & query,
    const StorageMetadataPtr & metadata_snapshot,
    ContextPtr local_context,
    TableExclusiveLockHolder & lock_holder)
{
    /// Full query if fall back to pure storage.
    if (getClusterName(local_context).empty())
        return pure_storage->truncate(query, metadata_snapshot, local_context, lock_holder);

    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Truncate is not supported by storage {}", getName());
}

void StorageObjectStorageCluster::addInferredEngineArgsToCreateQuery(ASTs & args, const ContextPtr & context) const
{
    configuration->addStructureAndFormatToArgsIfNeeded(args, "", configuration->getFormat(), context, /*with_structure=*/false);
}

}
