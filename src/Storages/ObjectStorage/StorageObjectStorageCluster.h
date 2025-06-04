#pragma once
#include <Storages/IStorageCluster.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Storages/ObjectStorage/StorageObjectStorageSource.h>

namespace DB
{

class Context;

class StorageObjectStorageCluster : public IStorageCluster
{
public:
    using ConfigurationPtr = StorageObjectStorage::ConfigurationPtr;

    StorageObjectStorageCluster(
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
        ASTPtr partition_by_ = nullptr
    );

    std::string getName() const override;

    RemoteQueryExecutor::Extension getTaskIteratorExtension(
        const ActionsDAG::Node * predicate,
        const std::optional<ActionsDAG> & filter_actions_dag,
        const ContextPtr & context,
        ClusterPtr cluster) const override;

    String getPathSample(StorageInMemoryMetadata metadata, ContextPtr context);

    void setClusterNameInSettings(bool cluster_name_in_settings_) { cluster_name_in_settings = cluster_name_in_settings_; }

    String getClusterName(ContextPtr context) const override;

    bool hasExternalDynamicMetadata() const override
    {
        return (pure_storage && pure_storage->hasExternalDynamicMetadata())
            || configuration->hasExternalDynamicMetadata();
    }

    void updateExternalDynamicMetadata(ContextPtr context_ptr) override
    {
        if (pure_storage && pure_storage->hasExternalDynamicMetadata())
            pure_storage->updateExternalDynamicMetadata(context_ptr);
        if (configuration->hasExternalDynamicMetadata())
        {
            StorageInMemoryMetadata metadata;
            metadata.setColumns(configuration->updateAndGetCurrentSchema(object_storage, context_ptr));
            IStorageCluster::setInMemoryMetadata(metadata);
        }
    }

    QueryProcessingStage::Enum getQueryProcessingStage(ContextPtr, QueryProcessingStage::Enum, const StorageSnapshotPtr &, SelectQueryInfo &) const override;

    void truncate(
        const ASTPtr & query,
        const StorageMetadataPtr & metadata_snapshot,
        ContextPtr local_context,
        TableExclusiveLockHolder &) override;

    void addInferredEngineArgsToCreateQuery(ASTs & args, const ContextPtr & context) const override;

    std::optional<UInt64> totalRows(ContextPtr query_context) const override
    {
        if (pure_storage)
            return pure_storage->totalRows(query_context);
        configuration->update(object_storage, query_context);
        return configuration->totalRows();
    }

    std::optional<UInt64> totalBytes(ContextPtr query_context) const override
    {
        if (pure_storage)
            return pure_storage->totalBytes(query_context);
        configuration->update(object_storage, query_context);
        return configuration->totalBytes();
    }

private:
    void updateQueryToSendIfNeeded(
        ASTPtr & query,
        const StorageSnapshotPtr & storage_snapshot,
        const ContextPtr & context) override;

    void readFallBackToPure(
        QueryPlan & query_plan,
        const Names & column_names,
        const StorageSnapshotPtr & storage_snapshot,
        SelectQueryInfo & query_info,
        ContextPtr context,
        QueryProcessingStage::Enum processed_stage,
        size_t max_block_size,
        size_t num_streams) override;

    SinkToStoragePtr writeFallBackToPure(
        const ASTPtr & query,
        const StorageMetadataPtr & metadata_snapshot,
        ContextPtr context,
        bool async_insert) override;

    /*
    In case the table was created with `object_storage_cluster` setting,
    modify the AST query object so that it uses the table function implementation
    by mapping the engine name to table function name and setting `object_storage_cluster`.
    For table like
    CREATE TABLE table ENGINE=S3(...) SETTINGS object_storage_cluster='cluster'
    coverts request
    SELECT * FROM table
    to
    SELECT * FROM s3(...) SETTINGS object_storage_cluster='cluster'
    to make distributed request over cluster 'cluster'.
    */
    void updateQueryForDistributedEngineIfNeeded(ASTPtr & query, ContextPtr context);

    const String engine_name;
    StorageObjectStorage::ConfigurationPtr configuration;
    const ObjectStoragePtr object_storage;
    bool cluster_name_in_settings;

    /// non-clustered storage to fall back on pure realisation if needed
    std::shared_ptr<StorageObjectStorage> pure_storage;
};

}
