#pragma once

#include <Storages/IStorage.h>
#include <Interpreters/ActionsDAG.h>
#include <QueryPipeline/RemoteQueryExecutor.h>
#include <Processors/QueryPlan/SourceStepWithFilter.h>

namespace DB
{

class Cluster;
using ClusterPtr = std::shared_ptr<Cluster>;


/**
 *  Base cluster for Storages used in table functions like s3Cluster and hdfsCluster.
 *  Necessary for code simplification around parallel_distributed_insert_select.
 */
class IStorageCluster : public IStorage
{
public:
    IStorageCluster(
        const String & cluster_name_,
        const StorageID & table_id_,
        LoggerPtr log_);

    void read(
        QueryPlan & query_plan,
        const Names & column_names,
        const StorageSnapshotPtr & storage_snapshot,
        SelectQueryInfo & query_info,
        ContextPtr context,
        QueryProcessingStage::Enum processed_stage,
        size_t max_block_size,
        size_t num_streams) override;

    SinkToStoragePtr write(
        const ASTPtr & query,
        const StorageMetadataPtr & metadata_snapshot,
        ContextPtr context,
        bool async_insert) override;

    ClusterPtr getCluster(ContextPtr context) const { return getClusterImpl(context, cluster_name); }

    /// Query is needed for pruning by virtual columns (_file, _path)
    virtual RemoteQueryExecutor::Extension getTaskIteratorExtension(
        const ActionsDAG::Node * predicate,
        const ActionsDAG * filter_actions_dag,
        const ContextPtr & context,
        ClusterPtr cluster,
        StorageMetadataPtr storage_metadata_snapshot) const
        = 0;

    QueryProcessingStage::Enum getQueryProcessingStage(ContextPtr, QueryProcessingStage::Enum, const StorageSnapshotPtr &, SelectQueryInfo &) const override;

    /// Reads a query the caller has already prepared (see Planner/buildDistributedObjectStorageQueryPlan.h),
    /// through the same ReadFromCluster/task-iterator protocol read() uses. Unlike read(), does no query
    /// preparation of its own: `query_to_send` is already self-contained and `sample_block` already computed.
    void readPreparedClusterQuery(
        QueryPlan & query_plan,
        const Names & column_names,
        const StorageSnapshotPtr & storage_snapshot,
        SelectQueryInfo & query_info,
        ContextPtr context,
        QueryProcessingStage::Enum processed_stage,
        ASTPtr query_to_send,
        SharedHeader sample_block);

    /// Builds a standalone, resolved `*Cluster(cluster_name, ...)` table-function call for this storage. Generic
    /// across engines because the per-engine rewrite is done by the virtual updateQueryToSendIfNeeded, which
    /// also supplies credentials, structure and format arguments. Works on a throwaway single-table SELECT of
    /// its own, so no query in flight is touched.
    ASTPtr buildClusterTableFunctionAST(
        const String & dispatch_cluster_name, const StorageSnapshotPtr & storage_snapshot, const ContextPtr & context);

    bool isRemote() const final { return true; }
    bool supportsSubcolumns() const override  { return true; }
    bool supportsOptimizationToSubcolumns() const override { return false; }
    bool supportsTrivialCountOptimization(const StorageSnapshotPtr &, ContextPtr) const override { return true; }

    const String & getClusterName() const { return cluster_name; }

    const String & getOriginalClusterName() const { return cluster_name; }
    virtual String getClusterName(ContextPtr /* context */) const { return getOriginalClusterName(); }

protected:
    virtual void updateQueryToSendIfNeeded(
        ASTPtr & /*query*/,
        const StorageSnapshotPtr & /*storage_snapshot*/,
        const ContextPtr & /*context*/,
        bool /*make_cluster_function*/) {}
    void updateQueryWithJoinToSendIfNeeded(ASTPtr & query_to_send, SelectQueryInfo query_info, const ContextPtr & context);

    virtual void updateConfigurationIfNeeded(ContextPtr /* context */) {}

    struct RemoteCallVariables
    {
        StoragePtr storage;
        ContextPtr context;
    };

    RemoteCallVariables convertToRemote(
        ClusterPtr cluster,
        ContextPtr context,
        const std::string & cluster_name_from_settings,
        ASTPtr query_to_send);

    virtual void readFallBackToPure(
        QueryPlan & /* query_plan */,
        const Names & /* column_names */,
        const StorageSnapshotPtr & /* storage_snapshot */,
        SelectQueryInfo & /* query_info */,
        ContextPtr /* context */,
        QueryProcessingStage::Enum /* processed_stage */,
        size_t /* max_block_size */,
        size_t /* num_streams */)
    {
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Method readFallBackToPure is not supported by storage {}", getName());
    }
    
    virtual SinkToStoragePtr writeFallBackToPure(
        const ASTPtr & /*query*/,
        const StorageMetadataPtr & /*metadata_snapshot*/,
        ContextPtr /*context*/,
        bool /*async_insert*/)
    {
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Method writeFallBackToPure is not supported by storage {}", getName());
    }

    NamesAndTypesList getHivePartitionColumnsWithoutVirtuals(const StorageMetadataPtr & metadata_snapshot) const;

    NamesAndTypesList hive_partition_columns_to_read_from_file_path;

private:
    static ClusterPtr getClusterImpl(ContextPtr context, const String & cluster_name_, size_t max_hosts = 0);

    virtual bool isClusterSupported() const { return true; }

    LoggerPtr log;
    String cluster_name;

    struct QueryTreeInfo
    {
        bool has_join = false;
        bool has_cross_join = false;
        bool has_local_columns_in_where = false;
    };

    static QueryTreeInfo getQueryTreeInfo(QueryTreeNodePtr query_tree, ContextPtr context);
};


class ReadFromCluster : public SourceStepWithFilter
{
public:
    std::string getName() const override { return "ReadFromCluster"; }
    void initializePipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings &) override;
    void applyFilters(ActionDAGNodes added_filter_nodes) override;
    void describeActions(FormatSettings & format_settings) const override;

    ReadFromCluster(
        const Names & column_names_,
        const SelectQueryInfo & query_info_,
        const StorageSnapshotPtr & storage_snapshot_,
        const ContextPtr & context_,
        SharedHeader sample_block,
        std::shared_ptr<IStorageCluster> storage_,
        ASTPtr query_to_send_,
        QueryProcessingStage::Enum processed_stage_,
        ClusterPtr cluster_,
        LoggerPtr log_,
        std::optional<Tables> external_tables_,
        bool is_whole_query_dispatch_ = false)
        : SourceStepWithFilter(
            std::move(sample_block),
            column_names_,
            query_info_,
            storage_snapshot_,
            context_)
        , storage(std::move(storage_))
        , query_to_send(std::move(query_to_send_))
        , processed_stage(processed_stage_)
        , cluster(std::move(cluster_))
        , log(log_)
        , external_tables(external_tables_)
        , is_whole_query_dispatch(is_whole_query_dispatch_)
    {
    }

private:
    std::shared_ptr<IStorageCluster> storage;
    ASTPtr query_to_send;
    QueryProcessingStage::Enum processed_stage;
    ClusterPtr cluster;
    LoggerPtr log;

    std::optional<RemoteQueryExecutor::Extension> extension;
    std::shared_ptr<const ActionsDAG> listing_filter_dag;
    std::optional<Tables> external_tables;

    /// Set only by readPreparedClusterQuery. This step's output is then the whole dispatched query's result
    /// rather than one table's rows, which changes how filters may be used (see applyFilters, createExtension).
    bool is_whole_query_dispatch = false;

    void createExtension();
    ContextPtr updateSettings(const Settings & settings);
};

}
