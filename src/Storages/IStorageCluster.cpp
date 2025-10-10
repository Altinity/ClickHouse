#include <Storages/IStorageCluster.h>

#include <Common/Exception.h>
#include <Core/Settings.h>
#include <Core/QueryProcessingStage.h>
#include <IO/ConnectionTimeouts.h>
#include <Interpreters/Cluster.h>
#include <Interpreters/Context.h>
#include <Interpreters/getHeaderForProcessingStage.h>
#include <Interpreters/SelectQueryOptions.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Interpreters/AddDefaultDatabaseVisitor.h>
#include <Interpreters/TranslateQualifiedNamesVisitor.h>
#include <Interpreters/InterpreterSelectQueryAnalyzer.h>
#include <Processors/Sources/RemoteSource.h>
#include <QueryPipeline/narrowPipe.h>
#include <QueryPipeline/Pipe.h>
#include <QueryPipeline/RemoteQueryExecutor.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <Storages/IStorage.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/extractTableFunctionFromSelectQuery.h>
#include <Planner/Utils.h>
#include <Analyzer/QueryTreeBuilder.h>
#include <Analyzer/QueryNode.h>
#include <Analyzer/ColumnNode.h>
#include <Analyzer/InDepthQueryTreeVisitor.h>

#include <algorithm>
#include <memory>
#include <string>


namespace DB
{
namespace Setting
{
    extern const SettingsBool allow_experimental_analyzer;
    extern const SettingsBool async_query_sending_for_remote;
    extern const SettingsBool async_socket_for_remote;
    extern const SettingsBool skip_unavailable_shards;
    extern const SettingsBool parallel_replicas_local_plan;
    extern const SettingsString cluster_for_parallel_replicas;
    extern const SettingsNonZeroUInt64 max_parallel_replicas;
    extern const SettingsObjectStorageClusterJoinMode object_storage_cluster_join_mode;
}

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

namespace ErrorCodes
{
    extern const int ALL_CONNECTION_TRIES_FAILED;
}

IStorageCluster::IStorageCluster(
    const String & cluster_name_,
    const StorageID & table_id_,
    LoggerPtr log_)
    : IStorage(table_id_)
    , log(log_)
    , cluster_name(cluster_name_)
{
}

void ReadFromCluster::applyFilters(ActionDAGNodes added_filter_nodes)
{
    SourceStepWithFilter::applyFilters(std::move(added_filter_nodes));

    const ActionsDAG::Node * predicate = nullptr;
    if (filter_actions_dag)
        predicate = filter_actions_dag->getOutputs().at(0);

    createExtension(predicate);
}

void ReadFromCluster::createExtension(const ActionsDAG::Node * predicate)
{
    if (extension)
        return;

    extension = storage->getTaskIteratorExtension(
        predicate,
        filter_actions_dag ? filter_actions_dag.get() : query_info.filter_actions_dag.get(),
        context,
        cluster,
        getStorageSnapshot()->metadata);
}

namespace
{

/*
Helping class to find in query tree first node of required type
*/
class SearcherVisitor : public InDepthQueryTreeVisitorWithContext<SearcherVisitor>
{
public:
    using Base = InDepthQueryTreeVisitorWithContext<SearcherVisitor>;
    using Base::Base;

    explicit SearcherVisitor(QueryTreeNodeType type_, ContextPtr context) : Base(context), type(type_) {}

    bool needChildVisit(QueryTreeNodePtr &, QueryTreeNodePtr & /*child*/)
    {
        return !passed_node;
    }

    void enterImpl(QueryTreeNodePtr & node)
    {
        if (passed_node)
            return;

        auto node_type = node->getNodeType();

        if (node_type == type)
            passed_node = node;
    }

    QueryTreeNodePtr getNode() const { return passed_node; }

private:
    QueryTreeNodeType type;
    QueryTreeNodePtr passed_node;
};

/*
Helping class to find all used columns with specific source
*/
class CollectUsedColumnsForSourceVisitor : public InDepthQueryTreeVisitorWithContext<CollectUsedColumnsForSourceVisitor>
{
public:
    using Base = InDepthQueryTreeVisitorWithContext<CollectUsedColumnsForSourceVisitor>;
    using Base::Base;

    explicit CollectUsedColumnsForSourceVisitor(
        QueryTreeNodePtr source_,
        ContextPtr context,
        bool collect_columns_from_other_sources_ = false)
        : Base(context)
        , source(source_)
        , collect_columns_from_other_sources(collect_columns_from_other_sources_)
        {}

    void enterImpl(QueryTreeNodePtr & node)
    {
        auto node_type = node->getNodeType();

        if (node_type != QueryTreeNodeType::COLUMN)
            return;

        auto & column_node = node->as<ColumnNode &>();
        auto column_source = column_node.getColumnSourceOrNull();
        if (!column_source)
            return;

        if ((column_source == source) != collect_columns_from_other_sources)
        {
            const auto & name = column_node.getColumnName();
            if (!names.count(name))
            {
                columns.emplace_back(column_node.getColumn());
                names.insert(name);
            }
        }
    }

    const NamesAndTypes & getColumns() const { return columns; }

private:
    std::unordered_set<std::string> names;
    QueryTreeNodePtr source;
    NamesAndTypes columns;
    bool collect_columns_from_other_sources;
};

};

/*
Try to make subquery to send on nodes
Converts

  SELECT s3.c1, s3.c2, t.c3
  FROM
    s3Cluster(...) AS s3
  JOIN
    localtable as t
  ON s3.key == t.key

to

  SELECT s3.c1, s3.c2, s3.key
  FROM
    s3Cluster(...) AS s3
*/
void IStorageCluster::updateQueryWithJoinToSendIfNeeded(
    ASTPtr & query_to_send,
    QueryTreeNodePtr query_tree,
    const ContextPtr & context)
{
    auto object_storage_cluster_join_mode = context->getSettingsRef()[Setting::object_storage_cluster_join_mode];
    switch (object_storage_cluster_join_mode)
    {
    case ObjectStorageClusterJoinMode::LOCAL:
    {
        auto modified_query_tree = query_tree->clone();
        bool need_modify = false;

        SearcherVisitor table_function_searcher(QueryTreeNodeType::TABLE_FUNCTION, context);
        table_function_searcher.visit(query_tree);
        auto table_function_node = table_function_searcher.getNode();
        if (!table_function_node)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Can't find table function node");

        if (has_join)
        {
            auto table_function = extractTableFunctionASTPtrFromSelectQuery(query_to_send);
            auto query_tree_distributed = buildTableFunctionQueryTree(table_function, context);
            auto & table_function_ast = table_function->as<ASTFunction &>();
            query_tree_distributed->setAlias(table_function_ast.alias);

            // Find add used columns from table function to make proper projection list
            CollectUsedColumnsForSourceVisitor collector(table_function_node, context);
            collector.visit(query_tree);
            const auto & columns = collector.getColumns();

            auto & query_node = modified_query_tree->as<QueryNode &>();
            query_node.resolveProjectionColumns(columns);
            auto column_nodes_to_select = std::make_shared<ListNode>();
            column_nodes_to_select->getNodes().reserve(columns.size());
            for (auto & column : columns)
                column_nodes_to_select->getNodes().emplace_back(std::make_shared<ColumnNode>(column, table_function_node));
            query_node.getProjectionNode() = column_nodes_to_select;

            // Left only table function to send on cluster nodes
            modified_query_tree = modified_query_tree->cloneAndReplace(query_node.getJoinTree(), query_tree_distributed);

            need_modify = true;
        }

        if (has_local_columns_in_where)
        {
            auto & query_node = modified_query_tree->as<QueryNode &>();
            query_node.getWhere() = {};
        }

        if (need_modify)
            query_to_send = queryNodeToDistributedSelectQuery(modified_query_tree);
        return;
    }
    case ObjectStorageClusterJoinMode::GLOBAL:
        // TODO
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "`Global` mode for `object_storage_cluster_join_mode` setting is unimplemented for now");
    case ObjectStorageClusterJoinMode::ALLOW: // Do nothing special
        return;
    }
}

/// The code executes on initiator
void IStorageCluster::read(
    QueryPlan & query_plan,
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & query_info,
    ContextPtr context,
    QueryProcessingStage::Enum processed_stage,
    size_t /*max_block_size*/,
    size_t /*num_streams*/)
{
    updateConfigurationIfNeeded(context);

    storage_snapshot->check(column_names);

    updateBeforeRead(context);
    auto cluster = getCluster(context);

    /// Calculate the header. This is significant, because some columns could be thrown away in some cases like query with count(*)

    SharedHeader sample_block;
    ASTPtr query_to_send = query_info.query;

    updateQueryWithJoinToSendIfNeeded(query_to_send, query_info.query_tree, context);

    if (context->getSettingsRef()[Setting::allow_experimental_analyzer])
    {
        sample_block = InterpreterSelectQueryAnalyzer::getSampleBlock(query_to_send, context, SelectQueryOptions(processed_stage));
    }
    else
    {
        auto interpreter = InterpreterSelectQuery(query_to_send, context, SelectQueryOptions(processed_stage).analyze());
        sample_block = interpreter.getSampleBlock();
        query_to_send = interpreter.getQueryInfo().query->clone();
    }

    updateQueryToSendIfNeeded(query_to_send, storage_snapshot, context);

    RestoreQualifiedNamesVisitor::Data data;
    data.distributed_table = DatabaseAndTableWithAlias(*getTableExpression(query_to_send->as<ASTSelectQuery &>(), 0));
    data.remote_table.database = context->getCurrentDatabase();
    data.remote_table.table = getName();
    RestoreQualifiedNamesVisitor(data).visit(query_to_send);
    AddDefaultDatabaseVisitor visitor(context, context->getCurrentDatabase(),
                                      /* only_replace_current_database_function_= */false,
                                      /* only_replace_in_join_= */true);
    visitor.visit(query_to_send);

    auto this_ptr = std::static_pointer_cast<IStorageCluster>(shared_from_this());

    auto reading = std::make_unique<ReadFromCluster>(
        column_names,
        query_info,
        storage_snapshot,
        context,
        sample_block,
        std::move(this_ptr),
        std::move(query_to_send),
        processed_stage,
        cluster,
        log);

    query_plan.addStep(std::move(reading));
}

void ReadFromCluster::initializePipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings &)
{
    const Scalars & scalars = context->hasQueryContext() ? context->getQueryContext()->getScalars() : Scalars{};
    const bool add_agg_info = processed_stage == QueryProcessingStage::WithMergeableState;

    Pipes pipes;
    auto new_context = updateSettings(context->getSettingsRef());
    const auto & current_settings = new_context->getSettingsRef();
    auto timeouts = ConnectionTimeouts::getTCPTimeoutsWithFailover(current_settings);

    size_t replica_index = 0;
    auto max_replicas_to_use = static_cast<UInt64>(cluster->getShardsInfo().size());
    if (current_settings[Setting::max_parallel_replicas] > 1)
        max_replicas_to_use = std::min(max_replicas_to_use, current_settings[Setting::max_parallel_replicas].value);

    createExtension(nullptr);

    for (const auto & shard_info : cluster->getShardsInfo())
    {
        if (pipes.size() >= max_replicas_to_use)
            break;

        /// We're taking all replicas as shards,
        /// so each shard will have only one address to connect to.
        auto try_results = shard_info.pool->getMany(
            timeouts,
            current_settings,
            PoolMode::GET_ONE,
            {},
            /*skip_unavailable_endpoints=*/true);

        if (try_results.empty())
            continue;

        IConnections::ReplicaInfo replica_info{.number_of_current_replica = replica_index++};

        auto remote_query_executor = std::make_shared<RemoteQueryExecutor>(
            std::vector<IConnectionPool::Entry>{try_results.front()},
            query_to_send->formatWithSecretsOneLine(),
            getOutputHeader(),
            new_context,
            /*throttler=*/nullptr,
            scalars,
            Tables(),
            processed_stage,
            nullptr,
            RemoteQueryExecutor::Extension{.task_iterator = extension->task_iterator, .replica_info = std::move(replica_info)});

        remote_query_executor->setLogger(log);
        Pipe pipe{std::make_shared<RemoteSource>(
            remote_query_executor,
            add_agg_info,
            current_settings[Setting::async_socket_for_remote],
            current_settings[Setting::async_query_sending_for_remote])};
        pipe.addSimpleTransform([&](const SharedHeader & header) { return std::make_shared<UnmarshallBlocksTransform>(header); });
        pipes.emplace_back(std::move(pipe));
    }

    if (pipes.empty())
        throw Exception(ErrorCodes::ALL_CONNECTION_TRIES_FAILED, "Cannot connect to any replica for query execution");

    auto pipe = Pipe::unitePipes(std::move(pipes));
    for (const auto & processor : pipe.getProcessors())
        processors.emplace_back(processor);

    pipeline.init(std::move(pipe));
}

QueryProcessingStage::Enum IStorageCluster::getQueryProcessingStage(
    ContextPtr context, QueryProcessingStage::Enum to_stage, const StorageSnapshotPtr &, SelectQueryInfo & query_info) const
{
    auto object_storage_cluster_join_mode = context->getSettingsRef()[Setting::object_storage_cluster_join_mode];

    if (object_storage_cluster_join_mode != ObjectStorageClusterJoinMode::ALLOW)
    {
        if (!context->getSettingsRef()[Setting::allow_experimental_analyzer])
            throw Exception(ErrorCodes::NOT_IMPLEMENTED,
                "object_storage_cluster_join_mode!='allow' is not supported without allow_experimental_analyzer=true");

        SearcherVisitor join_searcher(QueryTreeNodeType::JOIN, context);
        join_searcher.visit(query_info.query_tree);
        if (join_searcher.getNode())
            has_join = true;

        SearcherVisitor table_function_searcher(QueryTreeNodeType::TABLE_FUNCTION, context);
        table_function_searcher.visit(query_info.query_tree);
        auto table_function_node = table_function_searcher.getNode();
        if (!table_function_node)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Can't find table function node");

        CollectUsedColumnsForSourceVisitor collector_where(table_function_node, context, true);
        auto & query_node = query_info.query_tree->as<QueryNode &>();
        if (query_node.hasWhere())
            collector_where.visit(query_node.getWhere());

        // Can't use 'WHERE' on remote node if it contains columns from other sources
        if (!collector_where.getColumns().empty())
            has_local_columns_in_where = true;

        if (has_join || has_local_columns_in_where)
            return QueryProcessingStage::Enum::FetchColumns;
    }

    /// Initiator executes query on remote node.
    if (context->getClientInfo().query_kind == ClientInfo::QueryKind::INITIAL_QUERY)
        if (to_stage >= QueryProcessingStage::Enum::WithMergeableState)
            return QueryProcessingStage::Enum::WithMergeableState;

    /// Follower just reads the data.
    return QueryProcessingStage::Enum::FetchColumns;
}

ContextPtr ReadFromCluster::updateSettings(const Settings & settings)
{
    Settings new_settings{settings};

    /// Cluster table functions should always skip unavailable shards.
    new_settings[Setting::skip_unavailable_shards] = true;

    auto new_context = Context::createCopy(context);
    new_context->setSettings(new_settings);
    return new_context;
}

ClusterPtr IStorageCluster::getCluster(ContextPtr context) const
{
    return context->getCluster(cluster_name)->getClusterWithReplicasAsShards(context->getSettingsRef());
}

}
