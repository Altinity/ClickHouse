#include <Storages/IStorageCluster.h>

#include <pcg_random.hpp>
#include <Common/randomSeed.h>

#include <Analyzer/createUniqueAliasesIfNecessary.h>
#include <Functions/FunctionsLogical.h>
#include <Functions/IFunctionAdaptors.h>

#include <Common/Exception.h>
#include <Core/Settings.h>
#include <Core/QueryProcessingStage.h>
#include <IO/ConnectionTimeouts.h>
#include <IO/Operators.h>
#include <Interpreters/Cluster.h>
#include <Interpreters/ClusterProxy/executeQuery.h>
#include <Interpreters/Context.h>
#include <Interpreters/getHeaderForProcessingStage.h>
#include <Interpreters/SelectQueryOptions.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Interpreters/AddDefaultDatabaseVisitor.h>
#include <Interpreters/TranslateQualifiedNamesVisitor.h>
#include <Interpreters/InterpreterSelectQueryAnalyzer.h>
#include <Interpreters/formatWithPossiblyHidingSecrets.h>
#include <Planner/Utils.h>
#include <Processors/QueryPlan/QueryPlanFormat.h>
#include <Processors/Sources/RemoteSource.h>
#include <Processors/Executors/PullingPipelineExecutor.h>
#include <QueryPipeline/narrowPipe.h>
#include <QueryPipeline/Pipe.h>
#include <QueryPipeline/RemoteQueryExecutor.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <Storages/IStorage.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/VirtualColumnUtils.h>
#include <Storages/buildQueryTreeForShard.h>
#include <Storages/StorageDistributed.h>
#include <TableFunctions/TableFunctionFactory.h>
#include <TableFunctions/TableFunctionRemote.h>
#include <Poco/URI.h>
#include <Storages/extractTableFunctionFromSelectQuery.h>
#include <Planner/Utils.h>
#include <Analyzer/QueryTreeBuilder.h>
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
    extern const SettingsString object_storage_cluster;
    extern const SettingsObjectStorageClusterJoinMode object_storage_cluster_join_mode;
    extern const SettingsBool object_storage_cluster_fallback_to_local_if_empty;
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

void ReadFromCluster::describeActions(FormatSettings & format_settings) const
{
    SourceStepWithFilter::describeActions(format_settings);
    if (query_to_send)
        format_settings.out << format_settings.detail_prefix << "Query: "
                            << format({.ctx = getContext(), .query = *query_to_send}) << '\n';
}

namespace
{

/// Independent filter DAGs (wrap `WHERE`, then a later pushed FilterStep) cannot
/// be `merge`d: that wires the second DAG through the first's boolean output.
/// `mergeNodes` keeps both predicates, then `and` is the listing condition.
ActionsDAG andListingFilterDAGs(ActionsDAG first, ActionsDAG second)
{
    if (first.getOutputs().empty())
        return second;
    if (second.getOutputs().empty())
        return first;

    const auto * first_filter = first.getOutputs().front();
    ActionsDAG::NodeRawConstPtrs second_outputs;
    first.mergeNodes(std::move(second), &second_outputs);
    if (second_outputs.empty())
        return first;

    const auto * second_filter = second_outputs.front();
    if (first_filter == second_filter)
        return first;

    FunctionOverloadResolverPtr func_and
        = std::make_unique<FunctionToOverloadResolverAdaptor>(std::make_shared<FunctionAnd>());
    const auto & and_node = first.addFunction(func_and, {first_filter, second_filter}, {});
    first.getOutputs() = {&and_node};
    first.removeUnusedActions();
    return first;
}

}

void ReadFromCluster::applyFilters(ActionDAGNodes added_filter_nodes)
{
    SourceStepWithFilter::applyFilters(std::move(added_filter_nodes));
    /// Empty later `applyFilters` (optimizer walk stops at JOIN) wipes
    /// `filter_actions_dag` and must not drop wrap `WHERE`.
    if (!filter_actions_dag)
        return;

    if (!listing_filter_dag)
    {
        listing_filter_dag = filter_actions_dag;
    }
    else if (listing_filter_dag->getHash() != filter_actions_dag->getHash())
    {
        listing_filter_dag = std::make_shared<const ActionsDAG>(
            andListingFilterDAGs(listing_filter_dag->clone(), filter_actions_dag->clone()));
    }

    VirtualColumnUtils::buildSetsForDAGExcludingGlobalIn(*listing_filter_dag, getContext());
}

void ReadFromCluster::createExtension()
{
    if (extension)
        return;

    const ActionsDAG * filter = listing_filter_dag
        ? listing_filter_dag.get()
        : (filter_actions_dag ? filter_actions_dag.get() : query_info.filter_actions_dag.get());
    const ActionsDAG::Node * predicate = filter ? filter->getOutputs().at(0) : nullptr;
    extension = storage->getTaskIteratorExtension(
        predicate,
        filter,
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

        /// `SELECT 1 FROM dim` names a table and projects no column. Column collection misses it.
        if (collect_columns_from_other_sources
            && node != source
            && (node_type == QueryTreeNodeType::TABLE || node_type == QueryTreeNodeType::TABLE_FUNCTION))
            has_foreign_table = true;

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
    bool hasForeignTable() const { return has_foreign_table; }

private:
    std::unordered_set<std::string> names;
    QueryTreeNodePtr source;
    NamesAndTypes columns;
    bool collect_columns_from_other_sources;
    bool has_foreign_table = false;
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

void stripObjectStorageRemoteInitiatorSettings(ASTPtr & query)
{
    auto * select_query = query ? query->as<ASTSelectQuery>() : nullptr;
    if (!select_query)
        return;

    auto query_settings = select_query->settings();
    if (!query_settings)
        return;

    auto & settings_ast = query_settings->as<ASTSetQuery &>();
    bool settings_changed = false;
    for (const auto * setting_name : {"object_storage_remote_initiator", "object_storage_remote_initiator_cluster"})
        settings_changed |= settings_ast.changes.removeSetting(setting_name);

    if (settings_changed && settings_ast.changes.empty())
        select_query->setExpression(ASTSelectQuery::Expression::SETTINGS, {});
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

bool queryHasRightOrFullJoin(const ASTPtr & query)
{
    if (!query)
        return false;

    if (const auto * select_query = query->as<ASTSelectQuery>())
    {
        const auto tables = select_query->tables();
        if (!tables)
            return false;

        for (const auto & child : tables->children)
        {
            const auto & element = child->as<ASTTablesInSelectQueryElement &>();
            if (element.table_join)
            {
                const auto kind = element.table_join->as<ASTTableJoin &>().kind;
                /// `PASTE` pairs by position, so a per-shard join would emit one pair per shard.
                if (isRightOrFull(kind) || isPaste(kind))
                    return true;
            }
        }
        return false;
    }

    const auto * union_query = query->as<ASTSelectWithUnionQuery>();
    if (!union_query || !union_query->list_of_selects)
        return false;

    for (const auto & child : union_query->list_of_selects->children)
        if (queryHasRightOrFullJoin(child))
            return true;
    return false;
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

    /// `RIGHT`/`FULL` preserve broadcast rows, and `PASTE` pairs by position, so the join stays on the initiator.
    if (queryHasRightOrFullJoin(query))
        return;

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
    const ContextPtr & context,
    QueryTreeNodePtr * resolved_query_tree)
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

        rewriteQueryForInitiatorLocalJoin(query_to_send, query_info, info, context, resolved_query_tree);
        return;
    }
    case ObjectStorageClusterJoinMode::GLOBAL:
    {
        /// Old analyzer: ship the left scan only. `INNER`/`LEFT`/`CROSS` stay in `query_to_send`.
        if (!query_info.query_tree)
        {
            if (queryHasRightOrFullJoin(query_to_send))
            {
                auto info = getQueryJoinInfoFromAST(query_to_send);
                rewriteQueryForInitiatorLocalJoin(query_to_send, query_info, info, context, resolved_query_tree);
            }
            return;
        }

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

bool IStorageCluster::shouldFallbackToLocalOnEmptyCluster(ContextPtr context) const
{
    return context->getSettingsRef()[Setting::object_storage_cluster_fallback_to_local_if_empty]
        && usesObjectStorageClusterSettingSyntax()
        && !getClusterName(context).empty();
}

bool IStorageCluster::shouldReadLocallyOnFallbackToPure(const ResolvedClusterRead & resolved, ContextPtr context) const
{
    if (!resolved.fallback_to_pure)
        return false;

    if (!context->getSettingsRef()[Setting::object_storage_remote_initiator])
        return true;

    if (resolved.remote_initiator_cluster)
        return false;

    if (resolved.local_fallback)
        return true;

    throw Exception(ErrorCodes::BAD_ARGUMENTS,
        "Setting 'object_storage_remote_initiator' can be used only with 'object_storage_remote_initiator_cluster', 'object_storage_cluster', or cluster name in arguments");
}

IStorageCluster::ResolvedClusterRead IStorageCluster::resolveClusterRead(ContextPtr context) const
{
    /// Decision matrix for object_storage_cluster_fallback_to_local_if_empty / remote initiator:
    /// - s3()/iceberg() (or ENGINE ... SETTINGS object_storage_cluster) + empty/unknown cluster + setting
    ///   -> read locally (fallback_to_pure).
    /// - s3Cluster(...)/explicit *Cluster argument -> never fall back locally.
    /// - object_storage_remote_initiator=1 with object_storage_remote_initiator_cluster set
    ///   -> defer object_storage_cluster resolution on the initiator. For setting-syntax / ENGINE,
    ///     always send plain s3()/iceberg() to the remote initiator with object_storage_cluster as a
    ///     query SETTINGS value (not *Cluster). OSC may be unknown locally and defined only on the
    ///     remote (or the reverse); *Cluster would bake the name into the function argument and
    ///     would also ignore object_storage_cluster_fallback_to_local_if_empty on the remote.
    ///     Empty local OSC is omitted so the remote can supply its own.
    /// - object_storage_remote_initiator=1 with only object_storage_cluster (no remote_initiator_cluster)
    ///   -> clustered remote path: default initiator cluster to object_storage_cluster, send *Cluster.
    /// - writes -> never apply local fallback (see write()).
    ResolvedClusterRead result;

    if (!isClusterSupported())
    {
        result.fallback_to_pure = true;
        return result;
    }

    auto cluster_name_from_settings = getClusterName(context);
    const auto & settings = context->getSettingsRef();
    result.local_fallback = shouldFallbackToLocalOnEmptyCluster(context);

    /// Defer OSC resolution when a separate remote-initiator cluster is set: the initiator must not
    /// require a locally known object_storage_cluster (it may exist only on the remote node).
    const bool defer_object_storage_cluster_resolution
        = settings[Setting::object_storage_remote_initiator]
        && !settings[Setting::object_storage_remote_initiator_cluster].value.empty();

    /// Under deferral, setting-syntax / ENGINE always take the pure remote path (plain s3() + query SETTINGS).
    result.fallback_to_pure = cluster_name_from_settings.empty()
        || (defer_object_storage_cluster_resolution && usesObjectStorageClusterSettingSyntax());

    const bool try_resolve_with_local_fallback
        = !defer_object_storage_cluster_resolution
        && !result.fallback_to_pure
        && result.local_fallback;

    if (try_resolve_with_local_fallback)
    {
        result.object_storage_cluster = getClusterImpl(
            context,
            cluster_name_from_settings,
            isObjectStorage() ? settings[Setting::object_storage_max_nodes] : 0,
            /*allow_null*/ true);
        if (!result.object_storage_cluster)
            result.fallback_to_pure = true;
    }

    if (settings[Setting::object_storage_remote_initiator])
    {
        auto remote_initiator_cluster_name = settings[Setting::object_storage_remote_initiator_cluster].value;
        if (!remote_initiator_cluster_name.empty())
        {
            /// Never soft-fail a missing/empty remote-initiator cluster via
            /// object_storage_cluster_fallback_to_local_if_empty: that setting applies only to object_storage_cluster.
            result.remote_initiator_cluster = getClusterImpl(
                context,
                remote_initiator_cluster_name,
                /*max_hosts*/ 0);
        }
    }

    return result;
}

void IStorageCluster::readFromRemoteInitiator(
    QueryPlan & query_plan,
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & query_info,
    ContextPtr context,
    QueryProcessingStage::Enum processed_stage,
    size_t max_block_size,
    size_t num_streams,
    ASTPtr query_to_send,
    ClusterPtr remote_initiator_cluster,
    const String & remote_initiator_cluster_name)
{
    /// Clone the stripped query before `convertToRemote` wraps it. `StorageDistributed` must not
    /// analyze the original `query_info`, which still contains the JOIN.
    const bool query_was_rewritten = query_to_send.get() != query_info.query.get();
    ASTPtr query_for_remote = query_was_rewritten ? query_to_send->clone() : ASTPtr{};
    if (query_for_remote)
        stripObjectStorageRemoteInitiatorSettings(query_for_remote);

    /// `convertToRemote` wraps the left table function in place and moves its alias inside `remote`.
    /// `query_to_send` may be `query_info.query`, which `StorageDistributed` analyzes with its JOINs.
    auto storage_and_context = convertToRemote(remote_initiator_cluster, context, remote_initiator_cluster_name, query_to_send->clone());
    auto src_distributed = std::dynamic_pointer_cast<StorageDistributed>(storage_and_context.storage);
    auto modified_query_info = query_info;
    modified_query_info.cluster = src_distributed->getCluster();

    if (query_for_remote)
    {
        modified_query_info.query = query_for_remote;
        modified_query_info.syntax_analyzer_result.reset();

        if (storage_and_context.context->getSettingsRef()[Setting::allow_experimental_analyzer])
        {
            InterpreterSelectQueryAnalyzer interpreter(
                query_for_remote,
                storage_and_context.context,
                SelectQueryOptions(processed_stage).analyze());
            modified_query_info.query_tree = interpreter.getQueryTree();
            modified_query_info.planner_context = interpreter.getPlanner().getPlannerContext();

            auto * query_node = modified_query_info.query_tree->as<QueryNode>();
            if (!query_node || !query_node->getJoinTreeNode())
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Expected a table scan after stripping the initiator-local JOIN");

            modified_query_info.table_expression = extractLeftTableExpression(query_node->getJoinTreeNodeTyped());
        }
        else
            modified_query_info.query_tree.reset();
    }

    auto new_storage_snapshot = storage_and_context.storage->getStorageSnapshot(storage_snapshot->metadata, storage_and_context.context);
    storage_and_context.storage->read(
        query_plan, column_names, new_storage_snapshot, modified_query_info, storage_and_context.context, processed_stage, max_block_size, num_streams);
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
    updateBeforeRead(context);

    if (!isClusterSupported())
    {
        readFallBackToPure(query_plan, column_names, storage_snapshot, query_info, context, processed_stage, max_block_size, num_streams);
        return;
    }

    auto cluster_name_from_settings = getClusterName(context);
    const auto & settings = context->getSettingsRef();
    /// The ALLOW path can forward the original AST without calling `queryNodeToDistributedSelectQuery`.
    if (settings[Setting::allow_experimental_analyzer]
        && (!cluster_name_from_settings.empty() || settings[Setting::object_storage_remote_initiator]))
        assertNoPendingAliasMarkersForDistributedSerialization(query_info.query_tree);

    ASTPtr query_to_send = query_info.query;

    auto resolved = resolveClusterRead(context);
    ClusterPtr cluster = resolved.object_storage_cluster;

    if (resolved.fallback_to_pure)
    {
        if (shouldReadLocallyOnFallbackToPure(resolved, context))
        {
            readFallBackToPure(query_plan, column_names, storage_snapshot, query_info, context, processed_stage, max_block_size, num_streams);
            return;
        }

        auto remote_initiator_cluster_name = settings[Setting::object_storage_remote_initiator_cluster].value;

        /// Send remote('host', s3(...)) with object_storage_cluster in query SETTINGS (not s3Cluster).
        /// The remote may resolve OSC, fall back locally, or use its own OSC when local OSC is empty.
        updateConfigurationIfNeeded(context);
        updateQueryWithJoinToSendIfNeeded(query_to_send, query_info, context);
        updateQueryToSendIfNeeded(query_to_send, storage_snapshot, context, /*make_cluster_function*/ false);

        /// ENGINE tables keep object_storage_cluster on the storage, not in the initiator query context.
        /// Propagate it into the context copied for remote() so the remote sees the same OSC as alt-syntax.
        auto context_for_remote = context;
        if (!cluster_name_from_settings.empty()
            && context->getSettingsRef()[Setting::object_storage_cluster].value.empty())
        {
            auto ctx = Context::createCopy(context);
            ctx->setSetting("object_storage_cluster", cluster_name_from_settings);
            context_for_remote = ctx;
        }

        readFromRemoteInitiator(
            query_plan,
            column_names,
            storage_snapshot,
            query_info,
            context_for_remote,
            processed_stage,
            max_block_size,
            num_streams,
            query_to_send,
            resolved.remote_initiator_cluster,
            remote_initiator_cluster_name);
        return;
    }

    updateConfigurationIfNeeded(context);

    storage_snapshot->check(column_names);

    /// Calculate the header. This is significant, because some columns could be thrown away in some cases like query with count(*)

    SharedHeader sample_block;

    /// A join kept on the initiator replaces `query_to_send` but already has a resolved left-scan tree.
    QueryTreeNodePtr resolved_query_tree;
    updateQueryWithJoinToSendIfNeeded(query_to_send, query_info, context, &resolved_query_tree);

    if (settings[Setting::allow_experimental_analyzer])
    {
        /// Re-analyzing the shard AST resolves a `DataLakeCatalog` table again and repeats a remote catalog lookup.
        if (canReuseResolvedQueryTreeForHeader() && resolved_query_tree)
            sample_block = InterpreterSelectQueryAnalyzer::getSampleBlock(resolved_query_tree, context, SelectQueryOptions(processed_stage));
        else if (canReuseResolvedQueryTreeForHeader() && query_to_send == query_info.query && query_info.query_tree)
        {
            /// A shard re-analyzes this query and `createUniqueAliasesIfNecessary` restarts `__tableN` at 1.
            /// The resolved tree still has the outer alias (`__table2` when this table is not leftmost).
            auto query_info_for_header = query_info;
            query_info_for_header.query_tree = query_info.query_tree->clone();
            createUniqueAliasesIfNecessary(query_info_for_header.query_tree, context);
            sample_block = getHeaderForProcessingStage(column_names, storage_snapshot, query_info_for_header, context, processed_stage);
        }
        else
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
        /// Prefer the cluster already resolved in resolveClusterRead (when remote_initiator_cluster was set).
        ClusterPtr remote_initiator_cluster = resolved.remote_initiator_cluster;
        if (!remote_initiator_cluster)
            remote_initiator_cluster = getClusterImpl(context, remote_initiator_cluster_name);

        readFromRemoteInitiator(
            query_plan,
            column_names,
            storage_snapshot,
            query_info,
            context,
            processed_stage,
            max_block_size,
            num_streams,
            query_to_send,
            remote_initiator_cluster,
            remote_initiator_cluster_name);
        return;
    }

    if (!cluster)
        cluster = getClusterImpl(context, cluster_name_from_settings, isObjectStorage() ? settings[Setting::object_storage_max_nodes] : 0);

    RestoreQualifiedNamesVisitor::Data data;
    data.distributed_table = DatabaseAndTableWithAlias(*getTableExpression(query_to_send->as<ASTSelectQuery &>(), 0));
    data.remote_table.database = context->getCurrentDatabase();
    data.remote_table.table = getName();
    RestoreQualifiedNamesVisitor(data).visit(query_to_send);
    AddDefaultDatabaseVisitor visitor(context, context->getCurrentDatabase(),
                                      /* only_replace_current_database_function_= */false,
                                      /* only_replace_in_join_= */true);
    visitor.visit(query_to_send);

    /// Strip initiator-only settings from the forwarded query text as well: the inter-server settings
    /// packet is stripped in `ReadFromCluster::updateSettings`, but `query_to_send` is also serialized via
    /// `formatWithSecretsOneLine()` with its `SETTINGS` clause intact, which would otherwise leak those
    /// names to shards (and trip `UNKNOWN_SETTING` on an older shard in a rolling upgrade).
    ClusterProxy::stripInitiatorOnlySettingsFromQuery(query_to_send);

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

    stripObjectStorageRemoteInitiatorSettings(query_to_send);

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

    // Intentionally do not apply object_storage_cluster_fallback_to_local_if_empty here.
    // Cluster write is not supported; applying fallback would make INSERT depend on cluster availability.
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

    createExtension();

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
    auto join_tree = query_node.getJoinTreeNode();
    if (!join_tree)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Can't find table or table function node");

    if (join_tree->getNodeType() == QueryTreeNodeType::JOIN)
        info.has_join = true;
    else if (join_tree->getNodeType() == QueryTreeNodeType::CROSS_JOIN)
        info.has_cross_join = true;

    auto left_table_expression = extractLeftTableExpression(query_node.getJoinTreeNodeTyped());
    if (!left_table_expression)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Can't find table or table function node");

    if (query_node.hasWhere() || query_node.hasPrewhere())
    {
        CollectUsedColumnsForSourceVisitor collector_where(left_table_expression, context, true);
        if (query_node.hasPrewhere())
            collector_where.visit(query_node.getPrewhere());
        if (query_node.hasWhere())
            collector_where.visit(query_node.getWhere());

        // `WHERE` must be rewritten on the remote if it uses another table, including
        // `WHERE x IN (SELECT 1 FROM local_table)`, which has no column from that table.
        if (!collector_where.getColumns().empty() || collector_where.hasForeignTable())
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
    const ContextPtr & context,
    QueryTreeNodePtr * resolved_query_tree)
{
    /// Analyzer path: reuse `extractLeftTableExpression` and `buildQueryToReadColumnsFromTableExpression`
    /// (same helpers the planner uses when wrapping `IStorageCluster` in a subquery).
    if (query_info.query_tree && query_info.query_tree->as<QueryNode>())
    {
        auto modified_query_tree = query_info.query_tree->clone();
        auto & query_node = modified_query_tree->as<QueryNode &>();
        auto join_tree = query_node.getJoinTreeNode();
        if (!join_tree)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Can't find table or table function node");

        auto left_table_expression = extractLeftTableExpression(query_node.getJoinTreeNodeTyped());
        if (!left_table_expression)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Can't find table or table function node");

        CollectUsedColumnsForSourceVisitor collector(left_table_expression, context);
        collector.visit(modified_query_tree);

        /// A left-only `WHERE` under `RIGHT`/`FULL` changes which right rows are unmatched.
        /// `PREWHERE` filters the left scan before the join, so it stays on that scan.
        if (queryHasRightOrFullJoin(query_to_send))
            query_node.getWhere() = {};

        if (query_node.getPrewhere())
            removeExpressionsThatDoNotDependOnTableIdentifiers(query_node.getPrewhere(), left_table_expression, context);
        if (query_node.getWhere())
            removeExpressionsThatDoNotDependOnTableIdentifiers(query_node.getWhere(), left_table_expression, context);

        /// The wrap predicate runs on remote nodes. Drop conjuncts such as `dictGet` and `hostName`
        /// that must stay on the initiator.
        if (query_node.getPrewhere())
            removeExpressionsThatAreUnsafeToDuplicate(query_node.getPrewhere(), context);
        if (query_node.getWhere())
            removeExpressionsThatAreUnsafeToDuplicate(query_node.getWhere(), context);

        auto rewritten_query_tree = buildQueryToReadColumnsFromTableExpression(
            collector.getColumns(), left_table_expression, context);
        auto & rewritten_query_node = rewritten_query_tree->as<QueryNode &>();
        rewritten_query_node.getPrewhere() = query_node.getPrewhere();
        rewritten_query_node.getWhere() = query_node.getWhere();

        if (resolved_query_tree)
            *resolved_query_tree = rewritten_query_tree;

        query_to_send = queryNodeToDistributedSelectQuery(rewritten_query_tree);
        return;
    }

    /// Old interpreter: reuse `removeJoin` used by `StorageDistributed`, `StorageMerge`, and `StorageWindowView`.
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
        /// Read the kind before `removeJoin` drops it. Left-only `WHERE` under `RIGHT`/`FULL` changes unmatched rows.
        const bool preserves_right_side = queryHasRightOrFullJoin(query_to_send);
        /// `removeJoin` only looks at the second table element. An `ARRAY JOIN` there hides a later join.
        if (auto tables = select_query->tables())
        {
            auto & children = tables->children;
            for (size_t i = 1; i < children.size();)
            {
                if (children[i]->as<ASTTablesInSelectQueryElement &>().array_join)
                    children.erase(children.begin() + i);
                else
                    ++i;
            }
        }
        if (!removeJoin(*select_query, rewriter_result, context))
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Failed to strip JOIN from query sent to cluster nodes");

        /// `removeJoin` drops the ORDER BY and GROUP BY expressions but leaves `order_by_all` and
        /// `group_by_all` set. The non-join branch below clears both flags. `IStorageCluster::read`
        /// re-analyzes this AST with `TreeRewriter`, which still honors them: `ORDER BY ALL`
        /// dereferences a null `orderBy`, and `GROUP BY ALL` aggregates the stripped left columns
        /// on the shards before the initiator join.
        select_query->group_by_all = false;
        select_query->order_by_all = false;

        /// `removeJoin` keeps left-table predicates. A left `WHERE` under `RIGHT`/`FULL` changes unmatched rows.
        /// A subquery cannot be resolved on the shards. `PREWHERE` stays: it filters the left scan before the join.
        if (preserves_right_side || astContainsInTableIdentifier(select_query->where()) || astContainsSubquery(select_query->where()))
            select_query->setExpression(ASTSelectQuery::Expression::WHERE, {});
        if (astContainsInTableIdentifier(select_query->prewhere()) || astContainsSubquery(select_query->prewhere()))
            select_query->setExpression(ASTSelectQuery::Expression::PREWHERE, {});
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
        /// `removeJoin` clears these. A leftover modifier with no `GROUP BY` rejects the shard query.
        select_query->group_by_with_totals = false;
        select_query->group_by_with_rollup = false;
        select_query->group_by_with_cube = false;
        select_query->group_by_with_grouping_sets = false;
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
    const auto join_info = getQueryJoinInfo(query_info, context);
    /// `global` still fetches columns for `RIGHT`/`FULL`/`PASTE`: those joins stay on the initiator.
    const bool keep_join_on_initiator = object_storage_cluster_join_mode != ObjectStorageClusterJoinMode::GLOBAL
        || queryHasRightOrFullJoin(query_info.query);

    if (keep_join_on_initiator && needsInitiatorLocalJoin(join_info))
    {
        return QueryProcessingStage::Enum::FetchColumns;
    }

    /// Only a follower reached by another node's cluster function (SECONDARY_QUERY) just fetches
    /// raw data. Everything else is the initiator of the distributed read, including internal
    /// contexts that never set the kind (NO_QUERY), e.g. a Replicated database DDL worker.
    if (context->getClientInfo().query_kind != ClientInfo::QueryKind::SECONDARY_QUERY)
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

    /// Strip the initiator-only settings (the query-shaping settings, the result-serialisation
    /// settings, and `database`): they are materialized on the initiator and must not be forwarded
    /// to the remote servers, where they would re-shape the per-shard subquery a second time or
    /// break it (e.g. `format = 'Null'`). This mirrors the `Distributed` fan-out.
    ClusterProxy::stripInitiatorOnlySettings(new_settings);

    auto new_context = Context::createCopy(context);
    new_context->setSettings(new_settings);
    return new_context;
}

ClusterPtr IStorageCluster::getClusterImpl(ContextPtr context, const String & cluster_name_, size_t max_hosts, bool allow_null)
{
    ClusterPtr cluster = nullptr;
    if (allow_null)
    {
        cluster = context->tryGetCluster(cluster_name_);
        if (!cluster || !cluster->getAllNodeCount())
            return nullptr;
    }
    else
        cluster = context->getCluster(cluster_name_);
    return cluster->getClusterWithReplicasAsShards(context->getSettingsRef(), /* max_replicas_from_shard */ 0, max_hosts);
}

}
