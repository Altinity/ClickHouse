#pragma once
#include <Storages/IStorageCluster.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Storages/ObjectStorage/StorageObjectStorageSource.h>
#include <Interpreters/Context_fwd.h>

namespace DB
{

class StorageObjectStorageCluster : public IStorageCluster
{
public:
    StorageObjectStorageCluster(
        const String & cluster_name_,
        StorageObjectStorageConfigurationPtr configuration_,
        ObjectStoragePtr object_storage_,
        const StorageID & table_id_,
        const ColumnsDescription & columns_in_table_or_function_definition,
        const ConstraintsDescription & constraints_,
        const ASTPtr & partition_by,
        ContextPtr context_,
        bool is_table_function_ = false);

    std::string getName() const override;

    RemoteQueryExecutor::Extension getTaskIteratorExtension(
        const ActionsDAG::Node * predicate,
        const ActionsDAG * filter,
        const ContextPtr & context,
        ClusterPtr cluster,
        StorageMetadataPtr storage_metadata_snapshot) const override;

    String getPathSample(ContextPtr context);

    std::optional<UInt64> totalRows(ContextPtr query_context) const override;
    std::optional<UInt64> totalBytes(ContextPtr query_context) const override;
<<<<<<< HEAD
=======
    void setClusterNameInSettings(bool cluster_name_in_settings_) { cluster_name_in_settings = cluster_name_in_settings_; }

    String getClusterName(ContextPtr context) const override;

    QueryProcessingStage::Enum getQueryProcessingStage(ContextPtr, QueryProcessingStage::Enum, const StorageSnapshotPtr &, SelectQueryInfo &) const override;

    std::optional<QueryPipeline> distributedWrite(
        const ASTInsertQuery & query,
        ContextPtr context) override;

    bool supportsImport(ContextPtr context) const override;

    SinkToStoragePtr import(
        const std::string & file_name,
        Block & block_with_partition_values,
        const std::function<void(const std::string &)> & new_file_path_callback,
        bool overwrite_if_exists,
        std::size_t max_bytes_per_file,
        std::size_t max_rows_per_file,
        const std::optional<std::string> & iceberg_metadata_json_string,
        const std::optional<FormatSettings> & format_settings_,
        ContextPtr context) override;


    bool isDataLake() const override;

    void commitExportPartitionTransaction(
        const String & transaction_id,
        const String & partition_id,
        const Strings & exported_paths,
        const IcebergCommitExportPartitionArguments & iceberg_commit_export_partition_arguments,
        ContextPtr local_context) override;

    void drop() override;

    void dropInnerTableIfAny(bool sync, ContextPtr context) override;

    void truncate(
        const ASTPtr & query,
        const StorageMetadataPtr & metadata_snapshot,
        ContextPtr local_context,
        TableExclusiveLockHolder &) override;

    void checkTableCanBeRenamed(const StorageID & new_name) const override;

    void rename(const String & new_path_to_table_data, const StorageID & new_table_id) override;

    void renameInMemory(const StorageID & new_table_id) override;

    void alter(const AlterCommands & params, ContextPtr context, AlterLockHolder & alter_lock_holder) override;

    void addInferredEngineArgsToCreateQuery(ASTs & args, const ContextPtr & context) const override;

    IDataLakeMetadata * getExternalMetadata(ContextPtr query_context);

    StorageMetadataPtr getInMemoryMetadataPtr(bool bypass_metadata_cache) const override;

    void checkAlterIsPossible(const AlterCommands & commands, ContextPtr context) const override;

    void checkMutationIsPossible(const MutationCommands & commands, const Settings & settings) const override;

    Pipe alterPartition(
        const StorageMetadataPtr & metadata_snapshot,
        const PartitionCommands & commands,
        ContextPtr context) override;

    void checkAlterPartitionIsPossible(
        const PartitionCommands & commands,
        const StorageMetadataPtr & metadata_snapshot,
        const Settings & settings,
        ContextPtr context) const override;

    bool optimize(
        const ASTPtr & query,
        const StorageMetadataPtr & metadata_snapshot,
        const ASTPtr & partition,
        bool final,
        bool deduplicate,
        const Names & deduplicate_by_columns,
        bool cleanup,
        ContextPtr context) override;

    QueryPipeline updateLightweight(const MutationCommands & commands, ContextPtr context) override;

    void mutate(const MutationCommands & commands, ContextPtr context) override;

    CancellationCode killMutation(const String & mutation_id) override;

    void waitForMutation(const String & mutation_id, bool wait_for_another_mutation) override;

    void setMutationCSN(const String & mutation_id, UInt64 csn) override;

    CancellationCode killPartMoveToShard(const UUID & task_uuid) override;

    void startup() override;

    void shutdown(bool is_drop = false) override;

    void flushAndPrepareForShutdown() override;

    ActionLock getActionLock(StorageActionBlockType action_type) override;

    void onActionLockRemove(StorageActionBlockType action_type) override;
>>>>>>> 981a2d92cd0 (Merge pull request #1618 from Altinity/export_partition_iceberg)

    void updateExternalDynamicMetadataIfExists(ContextPtr query_context) override;

private:
    void updateQueryToSendIfNeeded(
        ASTPtr & query,
        const StorageSnapshotPtr & storage_snapshot,
        const ContextPtr & context) override;

    const String engine_name;
    const StorageObjectStorageConfigurationPtr configuration;
    const ObjectStoragePtr object_storage;
    NamesAndTypesList hive_partition_columns_to_read_from_file_path;
};

}
