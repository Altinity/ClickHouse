#include <optional>
#include <Storages/ObjectStorage/StorageObjectStorageCluster.h>

#include <Common/Exception.h>
#include <Common/StringUtils.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTSetQuery.h>
#include <Interpreters/Context.h>

#include <Core/Settings.h>
#include <Formats/FormatFactory.h>
#include <Processors/Sources/RemoteSource.h>
#include <QueryPipeline/RemoteQueryExecutor.h>
#include <Storages/IPartitionStrategy.h>

#include <Storages/VirtualColumnUtils.h>
#include <Storages/HivePartitioningUtils.h>
#include <Storages/ObjectStorage/Utils.h>
#include <Storages/ObjectStorage/StorageObjectStorageSource.h>
#include <Storages/extractTableFunctionFromSelectQuery.h>
#include <Storages/ObjectStorage/StorageObjectStorageStableTaskDistributor.h>

namespace DB
{
namespace Setting
{
    extern const SettingsBool use_hive_partitioning;
    extern const SettingsBool cluster_function_process_archive_on_multiple_nodes;
    extern const SettingsObjectStorageGranularityLevel cluster_table_function_split_granularity;
}

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

String StorageObjectStorageCluster::getPathSample(ContextPtr context)
{
    auto query_settings = configuration->getQuerySettings(context);
    /// We don't want to throw an exception if there are no files with specified path.
    query_settings.throw_on_zero_files_match = false;
    auto file_iterator = StorageObjectStorageSource::createFileIterator(
        configuration,
        query_settings,
        object_storage,
        nullptr, // storage_metadata
        false, // distributed_processing
        context,
        {}, // predicate
        {},
        {}, // virtual_columns
        {}, // hive_columns
        nullptr, // read_keys
        {} // file_progress_callback
    );

    if (auto file = file_iterator->next(0))
        return file->getPath();
    return "";
}

StorageObjectStorageCluster::StorageObjectStorageCluster(
    const String & cluster_name_,
    StorageObjectStorageConfigurationPtr configuration_,
    ObjectStoragePtr object_storage_,
    const StorageID & table_id_,
    const ColumnsDescription & columns_in_table_or_function_definition,
    const ConstraintsDescription & constraints_,
    const ASTPtr & partition_by,
    ContextPtr context_,
    bool is_table_function)
    : IStorageCluster(
        cluster_name_, table_id_, getLogger(fmt::format("{}({})", configuration_->getEngineName(), table_id_.table_name)))
    , configuration{configuration_}
    , object_storage(object_storage_)
{
    configuration->initPartitionStrategy(partition_by, columns_in_table_or_function_definition, context_);
    /// We allow exceptions to be thrown on update(),
    /// because Cluster engine can only be used as table function,
    /// so no lazy initialization is allowed.
    configuration->update(object_storage, context_);

    ColumnsDescription columns{columns_in_table_or_function_definition};
    std::string sample_path;
    resolveSchemaAndFormat(columns, configuration->format, object_storage, configuration, {}, sample_path, context_);
    configuration->check(context_);

    if (sample_path.empty()
        && context_->getSettingsRef()[Setting::use_hive_partitioning]
        && !configuration->isDataLakeConfiguration()
        && !configuration->partition_strategy)
        sample_path = getPathSample(context_);

    /// Not grabbing the file_columns because it is not necessary to do it here.
    std::tie(hive_partition_columns_to_read_from_file_path, std::ignore) = HivePartitioningUtils::setupHivePartitioningForObjectStorage(
        columns,
        configuration,
        sample_path,
        columns_in_table_or_function_definition.empty(),
        std::nullopt,
        context_);

    StorageInMemoryMetadata metadata;
    metadata.setColumns(columns);
    if (is_table_function && configuration->isDataLakeConfiguration())
    {
        /// For datalake table functions, always pin the current snapshot version so that
        /// query execution uses the same snapshot as query analysis (logical-race fix).
        /// Additionally reload columns from the snapshot when the per-format setting is enabled.
        if (auto state = configuration->getTableStateSnapshot(context_))
        {
            metadata.setDataLakeTableState(*state);
            if (configuration->shouldReloadSchemaForConsistency(context_))
            {
                if (auto metadata_snapshot = configuration->buildStorageMetadataFromState(*state, context_))
                    metadata = *metadata_snapshot;
            }
        }
    }

    metadata.setConstraints(constraints_);

    if (configuration->partition_strategy)
    {
        metadata.partition_key = configuration->partition_strategy->getPartitionKeyDescription();
    }

    metadata.setVirtuals(VirtualColumnUtils::getVirtualsForFileLikeStorage(
        metadata.columns,
        context_,
        /* format_settings */std::nullopt,
        configuration->partition_strategy_type,
        sample_path));

    setInMemoryMetadata(metadata);
}

std::string StorageObjectStorageCluster::getName() const
{
    return configuration->getEngineName();
}

std::optional<UInt64> StorageObjectStorageCluster::totalRows(ContextPtr query_context) const
{
    configuration->lazyInitializeIfNeeded(
        object_storage,
        query_context);
    return configuration->totalRows(query_context);
}

std::optional<UInt64> StorageObjectStorageCluster::totalBytes(ContextPtr query_context) const
{
    configuration->lazyInitializeIfNeeded(
        object_storage,
        query_context);
    return configuration->totalBytes(query_context);
}

void StorageObjectStorageCluster::updateQueryToSendIfNeeded(
    ASTPtr & query,
    const DB::StorageSnapshotPtr & storage_snapshot,
    const ContextPtr & context)
{
    auto * table_function = extractTableFunctionFromSelectQuery(query);
    if (!table_function)
        return;
    auto * expression_list = table_function->arguments->as<ASTExpressionList>();
    if (!expression_list)
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Expected SELECT query from table function {}, got '{}'",
            configuration->getEngineName(), query->formatForErrorMessage());
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

    ASTPtr settings_temporary_storage = nullptr;
    for (auto it = args.begin(); it != args.end(); ++it)
    {
        ASTSetQuery * settings_ast = (*it)->as<ASTSetQuery>();
        if (settings_ast)
        {
            settings_temporary_storage = std::move(*it);
            args.erase(it);
            break;
        }
    }

    if (!endsWith(table_function->name, "Cluster"))
        configuration->addStructureAndFormatToArgsIfNeeded(args, structure, configuration->format, context, /*with_structure=*/true);
    else
    {
        ASTPtr cluster_name_arg = args.front();
        args.erase(args.begin());
        configuration->addStructureAndFormatToArgsIfNeeded(args, structure, configuration->format, context, /*with_structure=*/true);
        args.insert(args.begin(), cluster_name_arg);
    }
    if (settings_temporary_storage)
    {
        args.insert(args.end(), std::move(settings_temporary_storage));
    }
}

void StorageObjectStorageCluster::updateExternalDynamicMetadataIfExists(ContextPtr query_context)
{
    if (!configuration->isDataLakeConfiguration())
        return;

    /// Always force an update to pick up the latest snapshot version.
    /// Using if_not_updated_before=true would leave latest_snapshot_version
    /// stale from the first query and silently omit new files.
    configuration->update(
        object_storage,
        query_context);

    auto state = configuration->getTableStateSnapshot(query_context);
    if (!state)
        return;

    auto new_metadata = *getInMemoryMetadataPtr(query_context, false);
    new_metadata.setDataLakeTableState(*state);

    if (configuration->shouldReloadSchemaForConsistency(query_context))
    {
        if (auto metadata_snapshot = configuration->buildStorageMetadataFromState(*state, query_context))
            new_metadata = *metadata_snapshot;
    }

    setInMemoryMetadata(new_metadata.withVirtuals(VirtualColumnUtils::getVirtualsForFileLikeStorage(
        new_metadata.columns,
        query_context,
        /* format_settings */ std::nullopt,
        configuration->partition_strategy_type)));
}

RemoteQueryExecutor::Extension StorageObjectStorageCluster::getTaskIteratorExtension(
    const ActionsDAG::Node * predicate,
    const ActionsDAG * filter,
    const ContextPtr & local_context,
    ClusterPtr cluster,
    StorageMetadataPtr storage_metadata_snapshot) const
{
    auto iterator = StorageObjectStorageSource::createFileIterator(
        configuration,
        configuration->getQuerySettings(local_context),
        object_storage,
        storage_metadata_snapshot,
        /* distributed_processing */ false,
        local_context,
        predicate,
        filter,
        storage_metadata_snapshot->virtuals.getSampleBlock(VirtualsKind::All, VirtualsMaterializationPlace::Reader).getNamesAndTypesList(),
        hive_partition_columns_to_read_from_file_path,
        nullptr,
        local_context->getFileProgressCallback(),
        /*ignore_archive_globs=*/false,
        /*skip_object_metadata=*/true);

    if (local_context->getSettingsRef()[Setting::cluster_table_function_split_granularity] == ObjectStorageGranularityLevel::BUCKET)
    {
        iterator = std::make_shared<ObjectIteratorSplitByBuckets>(
            std::move(iterator),
            configuration->format,
            object_storage,
            local_context
        );
    }

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

    auto task_distributor = std::make_shared<StorageObjectStorageStableTaskDistributor>(
        iterator,
        std::move(ids_of_hosts),
        /* send_over_whole_archive */!local_context->getSettingsRef()[Setting::cluster_function_process_archive_on_multiple_nodes]);

    auto callback = std::make_shared<TaskIterator>(
        [task_distributor, local_context](size_t number_of_current_replica) mutable -> ClusterFunctionReadTaskResponsePtr
        {
            auto task = task_distributor->getNextTask(number_of_current_replica);
            if (task)
                return std::make_shared<ClusterFunctionReadTaskResponse>(std::move(task), local_context);
            return std::make_shared<ClusterFunctionReadTaskResponse>();
        });

    return RemoteQueryExecutor::Extension{ .task_iterator = std::move(callback) };
}

}

<<<<<<< HEAD
=======
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

bool StorageObjectStorageCluster::isClusterSupported() const
{
    return configuration->isClusterSupported();
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

    if (!isClusterSupported())
        return "";

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

std::optional<QueryPipeline> StorageObjectStorageCluster::distributedWrite(
    const ASTInsertQuery & query,
    ContextPtr context)
{
    if (getClusterName(context).empty())
        return pure_storage->distributedWrite(query, context);
    return IStorageCluster::distributedWrite(query, context);
}

void StorageObjectStorageCluster::drop()
{
    if (pure_storage)
    {
        pure_storage->drop();
        return;
    }
    IStorageCluster::drop();
}

void StorageObjectStorageCluster::dropInnerTableIfAny(bool sync, ContextPtr context)
{
    if (getClusterName(context).empty())
    {
        pure_storage->dropInnerTableIfAny(sync, context);
        return;
    }
    IStorageCluster::dropInnerTableIfAny(sync, context);
}

void StorageObjectStorageCluster::truncate(
    const ASTPtr & query,
    const StorageMetadataPtr & metadata_snapshot,
    ContextPtr local_context,
    TableExclusiveLockHolder & lock_holder)
{
    /// Full query if fall back to pure storage.
    if (getClusterName(local_context).empty())
    {
        pure_storage->truncate(query, metadata_snapshot, local_context, lock_holder);
        return;
    }

    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Truncate is not supported by storage {}", getName());
}

void StorageObjectStorageCluster::checkTableCanBeRenamed(const StorageID & new_name) const
{
    if (pure_storage)
    {
        pure_storage->checkTableCanBeRenamed(new_name);
        return;
    }
    IStorageCluster::checkTableCanBeRenamed(new_name);
}

void StorageObjectStorageCluster::rename(const String & new_path_to_table_data, const StorageID & new_table_id)
{
    if (pure_storage)
    {
        pure_storage->rename(new_path_to_table_data, new_table_id);
        return;
    }
    IStorageCluster::rename(new_path_to_table_data, new_table_id);
}

void StorageObjectStorageCluster::renameInMemory(const StorageID & new_table_id)
{
    if (pure_storage)
    {
        pure_storage->renameInMemory(new_table_id);
        return;
    }
    IStorageCluster::renameInMemory(new_table_id);
}

void StorageObjectStorageCluster::alter(const AlterCommands & params, ContextPtr context, AlterLockHolder & alter_lock_holder)
{
    if (getClusterName(context).empty())
    {
        pure_storage->alter(params, context, alter_lock_holder);
        setInMemoryMetadata(pure_storage->getInMemoryMetadata());
        return;
    }
    IStorageCluster::alter(params, context, alter_lock_holder);
    pure_storage->setInMemoryMetadata(IStorageCluster::getInMemoryMetadata());
}

void StorageObjectStorageCluster::addInferredEngineArgsToCreateQuery(ASTs & args, const ContextPtr & context) const
{
    configuration->addStructureAndFormatToArgsIfNeeded(args, "", configuration->getFormat(), context, /*with_structure=*/false);
}

StorageMetadataPtr StorageObjectStorageCluster::getInMemoryMetadataPtr(bool bypass_metadata_cache) const
{
    if (pure_storage)
        return pure_storage->getInMemoryMetadataPtr(bypass_metadata_cache);
    return IStorageCluster::getInMemoryMetadataPtr(bypass_metadata_cache);
}

IDataLakeMetadata * StorageObjectStorageCluster::getExternalMetadata(ContextPtr query_context)
{
    if (getClusterName(query_context).empty())
        return pure_storage->getExternalMetadata(query_context);

    configuration->update(
        object_storage,
        query_context,
        /* if_not_updated_before */false);

    return configuration->getExternalMetadata();
}

void StorageObjectStorageCluster::checkAlterIsPossible(const AlterCommands & commands, ContextPtr context) const
{
    if (getClusterName(context).empty())
    {
        pure_storage->checkAlterIsPossible(commands, context);
        return;
    }
    IStorageCluster::checkAlterIsPossible(commands, context);
}

void StorageObjectStorageCluster::checkMutationIsPossible(const MutationCommands & commands, const Settings & settings) const
{
    if (pure_storage)
    {
        pure_storage->checkMutationIsPossible(commands, settings);
        return;
    }
    IStorageCluster::checkMutationIsPossible(commands, settings);
}

Pipe StorageObjectStorageCluster::alterPartition(
    const StorageMetadataPtr & metadata_snapshot,
    const PartitionCommands & commands,
    ContextPtr context)
{
    if (getClusterName(context).empty())
        return pure_storage->alterPartition(metadata_snapshot, commands, context);
    return IStorageCluster::alterPartition(metadata_snapshot, commands, context);
}

void StorageObjectStorageCluster::checkAlterPartitionIsPossible(
    const PartitionCommands & commands,
    const StorageMetadataPtr & metadata_snapshot,
    const Settings & settings,
    ContextPtr context) const
{
    if (getClusterName(context).empty())
    {
        pure_storage->checkAlterPartitionIsPossible(commands, metadata_snapshot, settings, context);
        return;
    }
    IStorageCluster::checkAlterPartitionIsPossible(commands, metadata_snapshot, settings, context);
}

bool StorageObjectStorageCluster::optimize(
    const ASTPtr & query,
    const StorageMetadataPtr & metadata_snapshot,
    const ASTPtr & partition,
    bool final,
    bool deduplicate,
    const Names & deduplicate_by_columns,
    bool cleanup,
    ContextPtr context)
{
    if (getClusterName(context).empty())
        return pure_storage->optimize(query, metadata_snapshot, partition, final, deduplicate, deduplicate_by_columns, cleanup, context);
    return IStorageCluster::optimize(query, metadata_snapshot, partition, final, deduplicate, deduplicate_by_columns, cleanup, context);
}

QueryPipeline StorageObjectStorageCluster::updateLightweight(const MutationCommands & commands, ContextPtr context)
{
    if (getClusterName(context).empty())
        return pure_storage->updateLightweight(commands, context);
    return IStorageCluster::updateLightweight(commands, context);
}

void StorageObjectStorageCluster::mutate(const MutationCommands & commands, ContextPtr context)
{
    if (getClusterName(context).empty())
    {
        pure_storage->mutate(commands, context);
        return;
    }
    IStorageCluster::mutate(commands, context);
}

CancellationCode StorageObjectStorageCluster::killMutation(const String & mutation_id)
{
    if (pure_storage)
        return pure_storage->killMutation(mutation_id);
    return IStorageCluster::killMutation(mutation_id);
}

void StorageObjectStorageCluster::waitForMutation(const String & mutation_id, bool wait_for_another_mutation)
{
    if (pure_storage)
    {
        pure_storage->waitForMutation(mutation_id, wait_for_another_mutation);
        return;
    }
    IStorageCluster::waitForMutation(mutation_id, wait_for_another_mutation);
}

void StorageObjectStorageCluster::setMutationCSN(const String & mutation_id, UInt64 csn)
{
    if (pure_storage)
    {
        pure_storage->setMutationCSN(mutation_id, csn);
        return;
    }
    IStorageCluster::setMutationCSN(mutation_id, csn);
}

CancellationCode StorageObjectStorageCluster::killPartMoveToShard(const UUID & task_uuid)
{
    if (pure_storage)
        return pure_storage->killPartMoveToShard(task_uuid);
    return IStorageCluster::killPartMoveToShard(task_uuid);
}

void StorageObjectStorageCluster::startup()
{
    if (pure_storage)
    {
        pure_storage->startup();
        return;
    }
    IStorageCluster::startup();
}

void StorageObjectStorageCluster::shutdown(bool is_drop)
{
    if (pure_storage)
    {
        pure_storage->shutdown(is_drop);
        return;
    }
    IStorageCluster::shutdown(is_drop);
}

void StorageObjectStorageCluster::flushAndPrepareForShutdown()
{
    if (pure_storage)
    {
        pure_storage->flushAndPrepareForShutdown();
        return;
    }
    IStorageCluster::flushAndPrepareForShutdown();
}

ActionLock StorageObjectStorageCluster::getActionLock(StorageActionBlockType action_type)
{
    if (pure_storage)
        return pure_storage->getActionLock(action_type);
    return IStorageCluster::getActionLock(action_type);
}

void StorageObjectStorageCluster::onActionLockRemove(StorageActionBlockType action_type)
{
    if (pure_storage)
    {
        pure_storage->onActionLockRemove(action_type);
        return;
    }
    IStorageCluster::onActionLockRemove(action_type);
}

bool StorageObjectStorageCluster::supportsDelete() const
{
    if (pure_storage)
        return pure_storage->supportsDelete();
    return IStorageCluster::supportsDelete();
}

bool StorageObjectStorageCluster::supportsParallelInsert() const
{
    if (pure_storage)
        return pure_storage->supportsParallelInsert();
    return IStorageCluster::supportsParallelInsert();
}

bool StorageObjectStorageCluster::prefersLargeBlocks() const
{
    if (pure_storage)
        return pure_storage->prefersLargeBlocks();
    return IStorageCluster::prefersLargeBlocks();
}

bool StorageObjectStorageCluster::supportsPartitionBy() const
{
    if (pure_storage)
        return pure_storage->supportsPartitionBy();
    return IStorageCluster::supportsPartitionBy();
}

bool StorageObjectStorageCluster::supportsSubcolumns() const
{
    if (pure_storage)
        return pure_storage->supportsSubcolumns();
    return IStorageCluster::supportsSubcolumns();
}

bool StorageObjectStorageCluster::supportsDynamicSubcolumns() const
{
    if (pure_storage)
        return pure_storage->supportsDynamicSubcolumns();
    return IStorageCluster::supportsDynamicSubcolumns();
}

bool StorageObjectStorageCluster::supportsTrivialCountOptimization(const StorageSnapshotPtr & snapshot, ContextPtr context) const
{
    if (pure_storage)
        return pure_storage->supportsTrivialCountOptimization(snapshot, context);
    return IStorageCluster::supportsTrivialCountOptimization(snapshot, context);
}

bool StorageObjectStorageCluster::supportsPrewhere() const
{
    if (pure_storage)
        return pure_storage->supportsPrewhere();
    return IStorageCluster::supportsPrewhere();
}

bool StorageObjectStorageCluster::canMoveConditionsToPrewhere() const
{
    if (pure_storage)
        return pure_storage->canMoveConditionsToPrewhere();
    return IStorageCluster::canMoveConditionsToPrewhere();
}

std::optional<NameSet> StorageObjectStorageCluster::supportedPrewhereColumns() const
{
    if (pure_storage)
        return pure_storage->supportedPrewhereColumns();
    return IStorageCluster::supportedPrewhereColumns();
}

IStorageCluster::ColumnSizeByName StorageObjectStorageCluster::getColumnSizes() const
{
    if (pure_storage)
        return pure_storage->getColumnSizes();
    return IStorageCluster::getColumnSizes();
}

bool StorageObjectStorageCluster::parallelizeOutputAfterReading(ContextPtr context) const
{
    if (pure_storage)
        return pure_storage->parallelizeOutputAfterReading(context);
    return IStorageCluster::parallelizeOutputAfterReading(context);
}

bool StorageObjectStorageCluster::supportsImport(ContextPtr context) const
{
    if (pure_storage)
        return pure_storage->supportsImport(context);
    return IStorageCluster::supportsImport(context);
}

SinkToStoragePtr StorageObjectStorageCluster::import(
    const std::string & file_name,
    Block & block_with_partition_values,
    const std::function<void(const std::string &)> & new_file_path_callback,
    bool overwrite_if_exists,
    std::size_t max_bytes_per_file,
    std::size_t max_rows_per_file,
    const std::optional<std::string> & iceberg_metadata_json_string,
    const std::optional<FormatSettings> & format_settings_,
    ContextPtr context)
{
    if (pure_storage)
        return pure_storage->import(
            file_name,
            block_with_partition_values,
            new_file_path_callback,
            overwrite_if_exists,
            max_bytes_per_file,
            max_rows_per_file,
            iceberg_metadata_json_string,
            format_settings_,
            context);
    return IStorageCluster::import(
        file_name,
        block_with_partition_values,
        new_file_path_callback,
        overwrite_if_exists,
        max_bytes_per_file,
        max_rows_per_file,
        iceberg_metadata_json_string,
        format_settings_,
        context);
}

bool StorageObjectStorageCluster::isDataLake() const
{
    if (pure_storage)
        return pure_storage->isDataLake();
    return IStorageCluster::isDataLake();
}

void StorageObjectStorageCluster::commitExportPartitionTransaction(
    const String & transaction_id,
    const String & partition_id,
    const Strings & exported_paths,
    const IcebergCommitExportPartitionArguments & iceberg_commit_export_partition_arguments,
    ContextPtr local_context)
{
    if (pure_storage)
    {
        pure_storage->commitExportPartitionTransaction(transaction_id, partition_id, exported_paths, iceberg_commit_export_partition_arguments, local_context);
        return;
    }
    IStorageCluster::commitExportPartitionTransaction(transaction_id, partition_id, exported_paths, iceberg_commit_export_partition_arguments, local_context);
}

}
>>>>>>> 981a2d92cd0 (Merge pull request #1618 from Altinity/export_partition_iceberg)
