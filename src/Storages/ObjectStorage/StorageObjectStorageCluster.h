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
    void updateExternalDynamicMetadataIfExists(ContextPtr query_context) override;
=======
    String getClusterName(ContextPtr context) const override;

    QueryProcessingStage::Enum getQueryProcessingStage(ContextPtr, QueryProcessingStage::Enum, const StorageSnapshotPtr &, SelectQueryInfo &) const override;

    std::optional<QueryPipeline> distributedWrite(
        const ASTInsertQuery & query,
        ContextPtr context) override;

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

    bool updateExternalDynamicMetadataIfExists(ContextPtr context) override;

    StorageMetadataPtr getInMemoryMetadataPtr() const override;

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

    bool supportsImport() const override;

    SinkToStoragePtr import(
        const std::string & /* file_name */,
        Block & /* block_with_partition_values */,
        std::string & /* destination_file_path */,
        bool /* overwrite_if_exists */,
        const std::optional<FormatSettings> & /* format_settings_ */,
        ContextPtr /* context */) override;
    bool prefersLargeBlocks() const override;

    void commitExportPartitionTransaction(
        const String & transaction_id,
        const String & partition_id,
        const Strings & exported_paths,
        ContextPtr local_context) override;

    bool supportsPartitionBy() const override;

    bool supportsSubcolumns() const override;

    bool supportsDynamicSubcolumns() const override;

    bool supportsTrivialCountOptimization(const StorageSnapshotPtr &, ContextPtr) const override;

    /// Things required for PREWHERE.
    bool supportsPrewhere() const override;
    bool canMoveConditionsToPrewhere() const override;
    std::optional<NameSet> supportedPrewhereColumns() const override;
    ColumnSizeByName getColumnSizes() const override;

    bool parallelizeOutputAfterReading(ContextPtr context) const override;

    bool supportsDelete() const override;
>>>>>>> d09fb0bdb7f (Merge pull request #1124 from Altinity/export_replicated_mt_partition_v2)

private:
    void updateQueryToSendIfNeeded(
        ASTPtr & query,
        const StorageSnapshotPtr & storage_snapshot,
        const ContextPtr & context) override;

    void updateConfigurationIfNeeded(ContextPtr context) override;

    const String engine_name;
    const StorageObjectStorageConfigurationPtr configuration;
    const ObjectStoragePtr object_storage;
    NamesAndTypesList virtual_columns;
    NamesAndTypesList hive_partition_columns_to_read_from_file_path;
    bool update_configuration_on_read_write;
};

}
