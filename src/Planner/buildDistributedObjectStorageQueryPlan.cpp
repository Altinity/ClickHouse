#include <Planner/buildDistributedObjectStorageQueryPlan.h>

#include <Analyzer/QueryNode.h>
#include <Analyzer/TableNode.h>
#include <Common/Exception.h>
#include <Interpreters/ActionsDAG.h>
#include <Interpreters/Context.h>
#include <Interpreters/InterpreterSelectQueryAnalyzer.h>
#include <Interpreters/SelectQueryOptions.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTSelectQuery.h>
#include <Planner/PlannerContext.h>
#include <Planner/Utils.h>
#include <Storages/IStorageCluster.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/StorageSnapshot.h>
#include <Storages/VirtualColumnUtils.h>
#include <Storages/removeGroupingFunctionSpecializations.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

namespace
{

/// Counts how many times `storage_id` is named as a table in `ast`. The count is taken on the serialized query --
/// the text a worker actually parses -- not on the query tree, because serialization inlines CTE bodies: a CTE
/// referenced twice is one node in the tree but two table references in the SQL.
size_t countTableReferences(const ASTPtr & ast, const StorageID & storage_id)
{
    if (!ast)
        return 0;

    size_t count = 0;
    if (const auto * identifier = ast->as<ASTTableIdentifier>())
    {
        const auto referenced = identifier->getTableId();
        if (referenced.table_name == storage_id.table_name && referenced.database_name == storage_id.database_name)
            ++count;
    }

    for (const auto & child : ast->children)
        count += countTableReferences(child, storage_id);

    return count;
}

}

/// Follows buildQueryPlanForParallelReplicas (Planner/findParallelReplicasQuery.cpp): header of the query ->
/// serialize to SQL -> remote read.
///
/// Unlike parallel replicas, the query sent is the one the user wrote: no table expression is rewritten. Which
/// table drives the dispatch travels beside the query, in `object_storage_distributed_driver_database`/`_table`,
/// and a worker reads exactly that one table from the initiator's file-task queue.
std::optional<JoinTreeQueryPlan> buildDistributedObjectStorageQueryPlan(
    const QueryTreeNodePtr & dispatch_boundary_node,
    const DistributedObjectStorageCandidate & candidate,
    const SelectQueryInfo & select_query_info,
    const PlannerContextPtr & planner_context)
{
    const auto context = planner_context->getQueryContext();
    constexpr auto processed_stage = QueryProcessingStage::WithMergeableState;

    auto * driver_storage = candidate.driver_storage;
    const auto & driver_storage_snapshot = candidate.driver->getStorageSnapshot();
    const auto driver_storage_id = candidate.driver->getStorageID();

    /// The header the query produces at this stage, on the initiator and on every worker alike -- the same tree
    /// serves both, so nothing has to be reconciled by name afterwards.
    auto [remote_header, new_planner_context] = InterpreterSelectQueryAnalyzer::getSampleBlockAndPlannerContext(
        dispatch_boundary_node->clone(), context, SelectQueryOptions(processed_stage).analyze());

    /// Strip grouping-function specializations in a clone: the workers re-resolve the generic function
    /// themselves, but the tree the header came from must keep them.
    auto query_tree_for_ast = dispatch_boundary_node->clone();
    removeGroupingFunctionSpecializations(query_tree_for_ast);
    ASTPtr query_to_send = queryNodeToDistributedSelectQuery(query_tree_for_ast);

    auto * select_query = query_to_send->as<ASTSelectQuery>();
    if (!select_query)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Distributed object-storage dispatch: expected a plain SELECT at the dispatch boundary");

    /// As `Distributed` does in rewriteSelectQuery: the query's settings are already in its context and travel
    /// with it, so the clause is dropped rather than sent. A worker applies a query's own SETTINGS on top of
    /// the settings it received, and would otherwise restore anything the initiator changed for the dispatch --
    /// `object_storage_cluster`, or a driver name written by hand.
    select_query->setExpression(ASTSelectQuery::Expression::SETTINGS, {});

    /// The driver is named by database and table, so it must be unambiguous in the query a worker receives. A
    /// self-join, or a CTE over the driver referenced more than once, would leave a worker unable to tell which
    /// occurrence owns the file-task queue -- and reading the queue twice is silently wrong, not an error.
    /// Fall back to ordinary planning instead.
    if (countTableReferences(query_to_send, driver_storage_id) != 1)
        return {};

    /// Travels to the workers with the query. `ReadFromClusterQuery::updateSettings` copies from this context.
    auto dispatch_context = Context::createCopy(context);
    dispatch_context->setSetting("object_storage_distributed_driver_database", driver_storage_id.getDatabaseName());
    dispatch_context->setSetting("object_storage_distributed_driver_table", driver_storage_id.getTableName());

    SelectQueryInfo query_info = select_query_info;
    query_info.query = query_to_send;
    query_info.query_tree = dispatch_boundary_node;
    query_info.planner_context = new_planner_context;

    JoinTreeQueryPlan result;
    result.stage = processed_stage;

    /// Prunes the driver's file listing. This is the filter the optimizer pushes down to the driver's own read
    /// in the single-node plan, collected by collectFiltersForAnalysis exactly as for an ordinary cluster read,
    /// so dropping the files it rejects cannot change the result. Without it every file of the driver is handed
    /// out and each worker opens the ones its partitions cannot match only to discard them.
    std::shared_ptr<const ActionsDAG> driver_filter;
    const auto & table_filters = planner_context->getGlobalPlannerContext()->filters_for_table_expressions;
    if (auto it = table_filters.find(candidate.driver_table_expression); it != table_filters.end() && it->second.filter_actions)
    {
        auto filter_dag = it->second.filter_actions->clone();
        VirtualColumnUtils::buildSetsForDAGExcludingGlobalIn(filter_dag, context);
        driver_filter = std::make_shared<const ActionsDAG>(std::move(filter_dag));
    }

    driver_storage->readPreparedClusterQuery(
        result.query_plan,
        driver_storage_snapshot,
        query_info,
        dispatch_context,
        processed_stage,
        query_to_send,
        remote_header,
        std::move(driver_filter));

    /// No converting step, unlike buildQueryPlanForParallelReplicas: that one rewrites the table expression
    /// before serializing, so its two headers are built from different trees and can diverge. Here the query
    /// is sent as written and `readPreparedClusterQuery` is given `remote_header` as the source step's own
    /// output header, so the plan's current header is that same block.

    return result;
}

}
