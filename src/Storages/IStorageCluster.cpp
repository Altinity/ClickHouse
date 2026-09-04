#include <Storages/IStorageCluster.h>

#include <pcg_random.hpp>
#include <Common/randomSeed.h>

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
#include <Planner/Utils.h>
#include <Processors/Sources/RemoteSource.h>
#include <Processors/Executors/PullingPipelineExecutor.h>
#include <QueryPipeline/narrowPipe.h>
#include <QueryPipeline/Pipe.h>
#include <QueryPipeline/RemoteQueryExecutor.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <Storages/IStorage.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/buildQueryTreeForShard.h>
#include <Storages/StorageDistributed.h>
#include <TableFunctions/TableFunctionFactory.h>
#include <TableFunctions/TableFunctionRemote.h>
#include <Poco/URI.h>
#include <Storages/extractTableFunctionFromSelectQuery.h>
#include <Analyzer/QueryNode.h>
#include <Analyzer/ColumnNode.h>
#include <Analyzer/InDepthQueryTreeVisitor.h>
#include <Analyzer/Utils.h>
#include <Interpreters/TreeRewriter.h>
#include <Core/Joins.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTSubquery.h>
#include <Parsers/ASTTablesInSelectQuery.h>

#include <Common/ProfileEvents.h>

#include <algorithm>
#include <memory>
#include <string>


namespace ProfileEvents
{
    extern const Event Shards;
}

namespace DB
{
namespace Setting
{
    extern const SettingsBool allow_experimental_analyzer;
    extern const SettingsBool async_query_sending_for_remote;
    extern const SettingsBool async_socket_for_remote;
    extern const SettingsBool skip_unavailable_shards;
    extern const SettingsNonZeroUInt64 max_parallel_replicas;
    extern const SettingsUInt64 object_storage_max_nodes;
    extern const SettingsBool object_storage_remote_initiator;
    extern const SettingsString object_storage_remote_initiator_cluster;
    extern const SettingsObjectStorageClusterJoinMode object_storage_cluster_join_mode;
}

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
    extern const int BAD_ARGUMENTS;
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
    const ActionsDAG * filter = filter_actions_dag ? filter_actions_dag.get() : query_info.filter_actions_dag.get();
    if (filter)
        predicate = filter->getOutputs().at(0);

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

bool astContainsSubquery(const ASTPtr & node)
{
    if (!node)
        return false;

    if (node->as<ASTSelectQuery>() || node->as<ASTSelectWithUnionQuery>() || node->as<ASTSubquery>())
        return true;

    for (const auto & child : node->children)
    {
        if (astContainsSubquery(child))
            return true;
    }

    return false;
}

bool astContainsInTableIdentifier(const ASTPtr & node)
{
    if (!node)
        return false;

    if (const auto * function = node->as<ASTFunction>())
    {
        if (isNameOfInFunction(function->name) && function->arguments && function->arguments->children.size() >= 2)
        {
            const auto & rhs = function->arguments->children[1];
            /// GLOBAL IN is rewritten to an external table (`_subqueryN`) as `ASTTableIdentifier`.
            /// `as<ASTIdentifier>` is an exact typeid match and does not see that subclass.
            if (rhs && (rhs->as<ASTIdentifier>() || rhs->as<ASTTableIdentifier>()))
                return true;
        }
    }

    for (const auto & child : node->children)
    {
        if (astContainsInTableIdentifier(child))
            return true;
    }

    return false;
}

bool astIsNestedSelect(const ASTPtr & node)
{
    return node && (node->as<ASTSelectQuery>() || node->as<ASTSelectWithUnionQuery>() || node->as<ASTSubquery>());
}

void rewriteASTInFunctionsToGlobalIn(ASTPtr & node)
{
    if (!node)
        return;

    if (auto * function = node->as<ASTFunction>())
    {
        if (isNameOfLocalInFunction(function->name) && function->arguments && function->arguments->children.size() >= 2)
        {
            const auto & rhs = function->arguments->children[1];
            if (rhs
                && (rhs->as<ASTSubquery>() || rhs->as<ASTSelectQuery>() || rhs->as<ASTSelectWithUnionQuery>()
                    || rhs->as<ASTIdentifier>() || rhs->as<ASTTableIdentifier>()))
            {
                function->name = getGlobalInFunctionNameForLocalInFunctionName(function->name);
            }
        }
    }

    for (auto & child : node->children)
    {
        if (astIsNestedSelect(child))
            continue;
        rewriteASTInFunctionsToGlobalIn(child);
    }
}

void rewriteASTJoinsToGlobal(ASTPtr & query)
{
    ASTSelectQuery * select_query = query->as<ASTSelectQuery>();
    if (!select_query)
    {
        if (auto * union_query = query->as<ASTSelectWithUnionQuery>())
        {
            if (union_query->list_of_selects)
            {
                for (auto & child : union_query->list_of_selects->children)
                    rewriteASTJoinsToGlobal(child);
            }
        }
        return;
    }

    if (auto tables = select_query->tables())
    {
        auto & tables_in_select_query = tables->as<ASTTablesInSelectQuery &>();
        for (auto & child : tables_in_select_query.children)
        {
            auto & tables_element = child->as<ASTTablesInSelectQueryElement &>();
            if (tables_element.table_join)
                tables_element.table_join->as<ASTTableJoin &>().locality = JoinLocality::Global;
        }
    }

    rewriteASTInFunctionsToGlobalIn(query);
}

}

void IStorageCluster::rewriteASTForGlobalJoin(ASTPtr & query)
{
    rewriteASTJoinsToGlobal(query);
}

/*
Try to make subquery to send on nodes
Converts

  SELECT s3.c1, s3.c2, t.c3
  FROM
    s3Cluster(...) AS s3
  JOIN
    localtable as t
  ON s3.key == t.key

to (object_storage_cluster_join_mode='allow' or 'local')

  SELECT s3.c1, s3.c2, s3.key
  FROM
    s3Cluster(...) AS s3

or (object_storage_cluster_join_mode='global')

  SELECT s3.c1, s3.c2, t.c3
  FROM
    s3Cluster(...) as s3
  JOIN
    values('key UInt32, data String', (1, 'one'), (2, 'two'), ...) as t
  ON s3.key == t.key
*/
void IStorageCluster::updateQueryWithJoinToSendIfNeeded(
    ASTPtr & query_to_send,
    SelectQueryInfo query_info,
    const ContextPtr & context)
{
    auto object_storage_cluster_join_mode = context->getSettingsRef()[Setting::object_storage_cluster_join_mode];
    switch (object_storage_cluster_join_mode)
    {
    case ObjectStorageClusterJoinMode::LOCAL: /// Legacy alias of `allow`
    case ObjectStorageClusterJoinMode::ALLOW:
    {
        auto info = getQueryJoinInfo(query_info, context);
        if (!needsInitiatorLocalJoin(info))
            return;

        rewriteQueryForInitiatorLocalJoin(query_to_send, query_info, info, context);
        return;
    }
    case ObjectStorageClusterJoinMode::GLOBAL:
    {
        if (!query_info.query_tree)
            return;

        auto info = getQueryTreeInfo(query_info.query_tree, context);

        if (needsInitiatorLocalJoin(info))
        {
            auto modified_query_tree = query_info.query_tree->clone();

            rewriteJoinToGlobalJoin(modified_query_tree, context, /*force_prefer_global_join*/ true);

            if (info.has_local_columns_in_where)
                rewriteInToGlobalIn(modified_query_tree, context, /*rewrite_for_distributed*/ true);

            modified_query_tree = buildQueryTreeForShard(
                query_info.planner_context,
                modified_query_tree,
                /*allow_global_join_for_right_table*/ false,
                /*find_cross_join*/ true);
            query_to_send = queryNodeToDistributedSelectQuery(modified_query_tree);
        }

        return;
    }
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
    size_t max_block_size,
    size_t num_streams)
{
    if (!isClusterSupported())
    {
        readFallBackToPure(query_plan, column_names, storage_snapshot, query_info, context, processed_stage, max_block_size, num_streams);
        return;
    }

    auto cluster_name_from_settings = getClusterName(context);
    const auto & settings = context->getSettingsRef();
    ASTPtr query_to_send = query_info.query;

    if (cluster_name_from_settings.empty())
    {
        if (settings[Setting::object_storage_remote_initiator])
        {
            auto remote_initiator_cluster_name = settings[Setting::object_storage_remote_initiator_cluster].value;
            if (remote_initiator_cluster_name.empty())
                throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "Setting 'object_storage_remote_initiator' can be used only with 'object_storage_remote_initiator_cluster', 'object_storage_cluster', or cluster name in arguments");

            /// rewrite query to execute `remote('remote_host', s3(...))`
            /// remote_host can execute query itself or make on-cluster query depends on own `object_storage_cluster` setting
            updateConfigurationIfNeeded(context);
            updateQueryWithJoinToSendIfNeeded(query_to_send, query_info, context);
            updateQueryToSendIfNeeded(query_to_send, storage_snapshot, context, /*make_cluster_function*/ false);

            auto remote_initiator_cluster = getClusterImpl(context, remote_initiator_cluster_name);
            auto storage_and_context = convertToRemote(remote_initiator_cluster, context, remote_initiator_cluster_name, query_to_send);
            auto src_distributed = std::dynamic_pointer_cast<StorageDistributed>(storage_and_context.storage);
            auto modified_query_info = query_info;
            modified_query_info.cluster = src_distributed->getCluster();
            auto new_storage_snapshot = storage_and_context.storage->getStorageSnapshot(storage_snapshot->metadata, storage_and_context.context);
            storage_and_context.storage->read(query_plan, column_names, new_storage_snapshot, modified_query_info, storage_and_context.context, processed_stage, max_block_size, num_streams);
            return;
        }

        readFallBackToPure(query_plan, column_names, storage_snapshot, query_info, context, processed_stage, max_block_size, num_streams);
        return;
    }

    updateConfigurationIfNeeded(context);

    storage_snapshot->check(column_names);

    /// Calculate the header. This is significant, because some columns could be thrown away in some cases like query with count(*)

    SharedHeader sample_block;

    updateQueryWithJoinToSendIfNeeded(query_to_send, query_info, context);

    if (settings[Setting::allow_experimental_analyzer])
    {
        sample_block = InterpreterSelectQueryAnalyzer::getSampleBlock(query_to_send, context, SelectQueryOptions(processed_stage));
    }
    else
    {
        auto interpreter = InterpreterSelectQuery(query_to_send, context, SelectQueryOptions(processed_stage).analyze());
        sample_block = interpreter.getSampleBlock();
        query_to_send = interpreter.getQueryInfo().query->clone();
    }

    updateQueryToSendIfNeeded(query_to_send, storage_snapshot, context, /*make_cluster_function*/ true);

    /// In case the current node is not supposed to initiate the clustered query
    /// Sends this query to a remote initiator using the `remote` table function
    if (settings[Setting::object_storage_remote_initiator])
    {
        /// Re-writes queries in the form of:
        /// Input: SELECT * FROM iceberg(...) SETTINGS object_storage_cluster='swarm', object_storage_remote_initiator=1
        /// Output: SELECT * FROM remote('remote_host', icebergCluster('swarm', ...)
        /// Where `remote_host` is a random host from the cluster which will execute the query
        /// This means the initiator node belongs to the same cluster that will execute the query
        /// In case remote_initiator_cluster_name is set, the initiator might be set to a different cluster
        auto remote_initiator_cluster_name = settings[Setting::object_storage_remote_initiator_cluster].value;
        if (remote_initiator_cluster_name.empty())
            remote_initiator_cluster_name = cluster_name_from_settings;
        auto remote_initiator_cluster = getClusterImpl(context, remote_initiator_cluster_name);
        auto storage_and_context = convertToRemote(remote_initiator_cluster, context, remote_initiator_cluster_name, query_to_send);
        auto src_distributed = std::dynamic_pointer_cast<StorageDistributed>(storage_and_context.storage);
        auto modified_query_info = query_info;
        modified_query_info.cluster = src_distributed->getCluster();
        auto new_storage_snapshot = storage_and_context.storage->getStorageSnapshot(storage_snapshot->metadata, storage_and_context.context);
        storage_and_context.storage->read(query_plan, column_names, new_storage_snapshot, modified_query_info, storage_and_context.context, processed_stage, max_block_size, num_streams);
        return;
    }

    auto cluster = getClusterImpl(context, cluster_name_from_settings, isObjectStorage() ? settings[Setting::object_storage_max_nodes] : 0);

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

    std::optional<Tables> external_tables;
    if (query_info.planner_context && query_info.planner_context->getMutableQueryContext())
        external_tables = query_info.planner_context->getMutableQueryContext()->getExternalTables();
    if (!external_tables || external_tables->empty())
        external_tables = context->getExternalTables();

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
        log,
        external_tables);

    query_plan.addStep(std::move(reading));
}

IStorageCluster::RemoteCallVariables IStorageCluster::convertToRemote(
    ClusterPtr cluster,
    ContextPtr context,
    const std::string & cluster_name_from_settings,
    ASTPtr query_to_send)
{
    /// TODO: Allow to use secret for remote queries
    if (!cluster->getSecret().empty())
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Can't convert query to remote when cluster uses secret");

    auto host_addresses = cluster->getShardsAddresses();
    if (host_addresses.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Empty cluster {}", cluster_name_from_settings);

    pcg64 rng(randomSeed());
    size_t shard_num = rng() % host_addresses.size();
    auto shard_addresses = host_addresses[shard_num];
    /// After getClusterImpl each shard must have exactly 1 replica
    if (shard_addresses.size() != 1)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Size of shard {} in cluster {} is not equal 1", shard_num, cluster_name_from_settings);
    std::string host_name;
    Poco::URI::decode(shard_addresses[0].toString(), host_name);

    LOG_INFO(log, "Choose remote initiator '{}'", host_name);

    bool secure = shard_addresses[0].secure == Protocol::Secure::Enable;
    std::string remote_function_name = secure ? "remoteSecure" : "remote";

    /// Clean object_storage_remote_initiator setting to avoid infinite remote call
    auto new_context = Context::createCopy(context);
    std::vector<std::string> settings_to_remove = {"object_storage_remote_initiator", "object_storage_remote_initiator_cluster"};
    new_context->resetSettingsToDefaultValue(settings_to_remove);

    auto * select_query = query_to_send->as<ASTSelectQuery>();
    if (!select_query)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Expected SELECT query");

    auto query_settings = select_query->settings();
    if (query_settings)
    {
        auto & settings_ast = query_settings->as<ASTSetQuery &>();
        bool settings_changed = false;
        for (const auto & setting_to_remove : settings_to_remove)
            settings_changed |= settings_ast.changes.removeSetting(setting_to_remove);
        if (settings_changed && settings_ast.changes.empty())
            select_query->setExpression(ASTSelectQuery::Expression::SETTINGS, {});
    }

    ASTTableExpression * table_expression = extractTableExpressionASTPtrFromSelectQuery(query_to_send);
    if (!table_expression)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Can't find table expression");
    if (!table_expression->table_function)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Can't find table function in table expression");

    boost::intrusive_ptr<ASTFunction> remote_query;

    if (shard_addresses[0].user_specified)
    { // with user/password for clsuter access remote query is executed from this user, add it in query parameters
        remote_query = makeASTFunction(remote_function_name,
            make_intrusive<ASTLiteral>(host_name),
            table_expression->table_function,
            make_intrusive<ASTLiteral>(shard_addresses[0].user),
            make_intrusive<ASTLiteral>(shard_addresses[0].password));
    }
    else
    { // without specified user/password remote query is executed from default user
        remote_query = makeASTFunction(remote_function_name, make_intrusive<ASTLiteral>(host_name), table_expression->table_function);
    }

    table_expression->table_function = remote_query;

    auto remote_function = TableFunctionFactory::instance().get(remote_query, new_context);

    std::shared_ptr<TableFunctionRemote> remote_table_function = std::dynamic_pointer_cast<TableFunctionRemote>(remote_function);
    if (remote_table_function)
    {
        auto metadata_snapshot = getInMemoryMetadataPtr(context, false);
        remote_table_function->setActualTableStructure(metadata_snapshot->columns);
    }

    auto storage = remote_function->execute(query_to_send, new_context, remote_function_name);

    return RemoteCallVariables{storage, new_context};
}

SinkToStoragePtr IStorageCluster::write(
    const ASTPtr & query,
    const StorageMetadataPtr & metadata_snapshot,
    ContextPtr context,
    bool async_insert)
{
    auto cluster_name_from_settings = getClusterName(context);

    if (cluster_name_from_settings.empty())
        return writeFallBackToPure(query, metadata_snapshot, context, async_insert);

    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Method write is not supported by storage {}", getName());
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

    ProfileEvents::increment(ProfileEvents::Shards, max_replicas_to_use);

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
            external_tables.has_value() ? *external_tables : Tables(),
            processed_stage,
            nullptr,
            RemoteQueryExecutor::Extension{.task_iterator = extension->task_iterator, .replica_info = std::move(replica_info)},
            shard_info.pool);

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

IStorageCluster::QueryTreeInfo IStorageCluster::getQueryTreeInfo(QueryTreeNodePtr query_tree, ContextPtr context)
{
    QueryTreeInfo info;

    auto & query_node = query_tree->as<QueryNode &>();
    auto join_tree = query_node.getJoinTree();
    if (!join_tree)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Can't find table or table function node");

    if (join_tree->getNodeType() == QueryTreeNodeType::JOIN)
        info.has_join = true;
    else if (join_tree->getNodeType() == QueryTreeNodeType::CROSS_JOIN)
        info.has_cross_join = true;

    auto left_table_expression = extractLeftTableExpression(join_tree);
    if (!left_table_expression)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Can't find table or table function node");

    if (query_node.hasWhere() || query_node.hasPrewhere())
    {
        CollectUsedColumnsForSourceVisitor collector_where(left_table_expression, context, true);
        if (query_node.hasPrewhere())
            collector_where.visit(query_node.getPrewhere());
        if (query_node.hasWhere())
            collector_where.visit(query_node.getWhere());

        // SELECT x FROM datalake.table WHERE x IN local.table.
        // Need to modify 'WHERE' on remote node if it contains columns from other sources
        // because remote node might not have those sources.
        if (!collector_where.getColumns().empty())
            info.has_local_columns_in_where = true;
    }

    return info;
}

bool IStorageCluster::needsInitiatorLocalJoin(const QueryTreeInfo & info)
{
    return info.has_join || info.has_cross_join || info.has_local_columns_in_where;
}

IStorageCluster::QueryTreeInfo IStorageCluster::getQueryJoinInfoFromAST(const ASTPtr & query)
{
    QueryTreeInfo info;
    if (!query)
        return info;

    const ASTSelectQuery * select_query = query->as<ASTSelectQuery>();
    if (!select_query)
    {
        if (const auto * union_query = query->as<ASTSelectWithUnionQuery>())
        {
            if (union_query->list_of_selects && union_query->list_of_selects->children.size() == 1)
                select_query = union_query->list_of_selects->children[0]->as<ASTSelectQuery>();
        }
    }
    if (!select_query)
        return info;

    if (select_query->hasJoin())
        info.has_join = true;

    if (astContainsSubquery(select_query->where()) || astContainsSubquery(select_query->prewhere())
        || astContainsInTableIdentifier(select_query->where()) || astContainsInTableIdentifier(select_query->prewhere()))
        info.has_local_columns_in_where = true;

    return info;
}

IStorageCluster::QueryTreeInfo IStorageCluster::getQueryJoinInfo(const SelectQueryInfo & query_info, const ContextPtr & context)
{
    if (query_info.query_tree && query_info.query_tree->as<QueryNode>())
        return getQueryTreeInfo(query_info.query_tree, context);

    return getQueryJoinInfoFromAST(query_info.query);
}

void IStorageCluster::rewriteQueryForInitiatorLocalJoin(
    ASTPtr & query_to_send,
    const SelectQueryInfo & query_info,
    const QueryTreeInfo & info,
    const ContextPtr & context)
{
    /// Analyzer path: reuse extractLeftTableExpression + buildQueryToReadColumnsFromTableExpression
    /// (same helpers the planner uses when wrapping IStorageCluster in a subquery).
    if (query_info.query_tree)
    {
        auto modified_query_tree = query_info.query_tree->clone();
        auto & query_node = modified_query_tree->as<QueryNode &>();
        auto join_tree = query_node.getJoinTree();
        if (!join_tree)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Can't find table or table function node");

        auto left_table_expression = extractLeftTableExpression(join_tree);
        if (!left_table_expression)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Can't find table or table function node");

        CollectUsedColumnsForSourceVisitor collector(left_table_expression, context);
        collector.visit(modified_query_tree);

        if (query_node.getPrewhere())
            removeExpressionsThatDoNotDependOnTableIdentifiers(query_node.getPrewhere(), left_table_expression, context);
        if (query_node.getWhere())
            removeExpressionsThatDoNotDependOnTableIdentifiers(query_node.getWhere(), left_table_expression, context);

        auto rewritten_query_tree = buildQueryToReadColumnsFromTableExpression(
            collector.getColumns(), left_table_expression, context);
        auto & rewritten_query_node = rewritten_query_tree->as<QueryNode &>();
        rewritten_query_node.getPrewhere() = query_node.getPrewhere();
        rewritten_query_node.getWhere() = query_node.getWhere();

        query_to_send = queryNodeToDistributedSelectQuery(rewritten_query_tree);
        return;
    }

    /// Old interpreter: reuse removeJoin used by StorageDistributed / StorageMerge / StorageWindowView.
    if (!query_to_send)
        return;

    query_to_send = query_to_send->clone();
    if (auto * union_query = query_to_send->as<ASTSelectWithUnionQuery>())
    {
        if (union_query->list_of_selects && union_query->list_of_selects->children.size() == 1)
            query_to_send = union_query->list_of_selects->children[0]->clone();
    }

    auto * select_query = query_to_send->as<ASTSelectQuery>();
    if (!select_query)
        return;

    if (select_query->hasJoin())
    {
        if (!query_info.syntax_analyzer_result)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Query is not analyzed: no syntax analyzer result");

        TreeRewriterResult rewriter_result = *query_info.syntax_analyzer_result;
        if (!removeJoin(*select_query, rewriter_result, context))
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Failed to strip JOIN from query sent to cluster nodes");

        /// `removeJoin` keeps left-table WHERE, including `GLOBAL IN (_subqueryN)`, which remotes cannot resolve.
        if (astContainsInTableIdentifier(select_query->where()) || astContainsInTableIdentifier(select_query->prewhere())
            || astContainsSubquery(select_query->where()) || astContainsSubquery(select_query->prewhere()))
        {
            select_query->setExpression(ASTSelectQuery::Expression::PREWHERE, {});
            select_query->setExpression(ASTSelectQuery::Expression::WHERE, {});
        }
    }
    else if (info.has_local_columns_in_where)
    {
        if (!query_info.syntax_analyzer_result)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Query is not analyzed: no syntax analyzer result");

        auto select_expression_list = make_intrusive<ASTExpressionList>();
        const auto & required_columns = query_info.syntax_analyzer_result->required_source_columns;
        if (required_columns.empty())
        {
            select_expression_list->children.push_back(make_intrusive<ASTLiteral>(Field{static_cast<UInt64>(1)}));
        }
        else
        {
            select_expression_list->children.reserve(required_columns.size());
            for (const auto & column : required_columns)
                select_expression_list->children.push_back(make_intrusive<ASTIdentifier>(column.name));
        }
        select_query->setExpression(ASTSelectQuery::Expression::SELECT, std::move(select_expression_list));
        select_query->setExpression(ASTSelectQuery::Expression::PREWHERE, {});
        select_query->setExpression(ASTSelectQuery::Expression::WHERE, {});
        select_query->setExpression(ASTSelectQuery::Expression::GROUP_BY, {});
        select_query->group_by_all = false;
        select_query->setExpression(ASTSelectQuery::Expression::HAVING, {});
        select_query->setExpression(ASTSelectQuery::Expression::ORDER_BY, {});
        select_query->order_by_all = false;
    }

    /// FetchColumns on remotes must not apply initiator-only LIMIT / WINDOW / QUALIFY.
    select_query->setExpression(ASTSelectQuery::Expression::WINDOW, {});
    select_query->setExpression(ASTSelectQuery::Expression::QUALIFY, {});
    select_query->setExpression(ASTSelectQuery::Expression::LIMIT_BY_OFFSET, {});
    select_query->setExpression(ASTSelectQuery::Expression::LIMIT_BY_LENGTH, {});
    select_query->setExpression(ASTSelectQuery::Expression::LIMIT_BY, {});
    select_query->setExpression(ASTSelectQuery::Expression::LIMIT_OFFSET, {});
    select_query->setExpression(ASTSelectQuery::Expression::LIMIT_LENGTH, {});
    select_query->setExpression(ASTSelectQuery::Expression::INTERPOLATE, {});
}

QueryProcessingStage::Enum IStorageCluster::getQueryProcessingStage(
    ContextPtr context, QueryProcessingStage::Enum to_stage, const StorageSnapshotPtr &, SelectQueryInfo & query_info) const
{
    auto object_storage_cluster_join_mode = context->getSettingsRef()[Setting::object_storage_cluster_join_mode];

    if (object_storage_cluster_join_mode != ObjectStorageClusterJoinMode::GLOBAL
        && needsInitiatorLocalJoin(getQueryJoinInfo(query_info, context)))
    {
        return QueryProcessingStage::Enum::FetchColumns;
    }

    /// Initiator executes query on remote node.
    if (context->getClientInfo().query_kind == ClientInfo::QueryKind::INITIAL_QUERY)
        if (to_stage >= QueryProcessingStage::Enum::WithMergeableState)
            return QueryProcessingStage::Enum::WithMergeableState;

    /// Follower just reads the data.
    return QueryProcessingStage::Enum::FetchColumns;
}

NamesAndTypesList IStorageCluster::getHivePartitionColumnsWithoutVirtuals(const StorageMetadataPtr & metadata_snapshot) const
{
    // Virtual columns can contain hive columns, so we remove these hive coulmns to avoid duplicates.
    // In non-cluster case these columns are filtered in DB::prepareReadingFromFormat function.
    auto virtual_columns = metadata_snapshot->virtuals.getSampleBlock(VirtualsKind::All, VirtualsMaterializationPlace::Reader).getNamesAndTypesList();
    NamesAndTypesList hive_partition_filtered;
    for (const auto & hive_name_and_type : hive_partition_columns_to_read_from_file_path)
    {
        if (!virtual_columns.contains(hive_name_and_type.name))
            hive_partition_filtered.emplace_back(hive_name_and_type);
    }
    return hive_partition_filtered;
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

ClusterPtr IStorageCluster::getClusterImpl(ContextPtr context, const String & cluster_name_, size_t max_hosts)
{
    return context->getCluster(cluster_name_)->getClusterWithReplicasAsShards(context->getSettingsRef(), /* max_replicas_from_shard */ 0, max_hosts);
}

}
