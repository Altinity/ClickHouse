#include <Planner/buildDistributedObjectStorageQueryPlan.h>

#include <Analyzer/QueryNode.h>
#include <Analyzer/TableNode.h>
#include <Common/Exception.h>
#include <Interpreters/ActionsDAG.h>
#include <Interpreters/Context.h>
#include <Interpreters/InterpreterSelectQueryAnalyzer.h>
#include <Interpreters/SelectQueryOptions.h>
#include <Processors/QueryPlan/ExpressionStep.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTSelectQuery.h>
#include <Planner/PlannerContext.h>
#include <Planner/Utils.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/IStorageCluster.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/StorageSnapshot.h>
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

/// This mirrors buildQueryPlanForParallelReplicas (Planner/findParallelReplicasQuery.cpp) step for step:
/// header of the query -> serialize to SQL -> remote read -> convert the remote header back by position.
/// Keep the two in sync.
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

    if (!query_to_send->as<ASTSelectQuery>())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Distributed object-storage dispatch: expected a plain SELECT at the dispatch boundary");

    /// The driver is named by database and table, so it must be unambiguous in the query a worker receives. A
    /// self-join, or a CTE over the driver referenced more than once, would leave a worker unable to tell which
    /// occurrence owns the file-task queue -- and reading the queue twice is silently wrong, not an error.
    /// Fall back to ordinary planning instead.
    if (countTableReferences(query_to_send, driver_storage_id) != 1)
        return {};

    /// Travels to the workers with the query. `ReadFromCluster::updateSettings` copies from this context.
    auto dispatch_context = Context::createCopy(context);
    dispatch_context->setSetting("object_storage_distributed_driver_database", driver_storage_id.getDatabaseName());
    dispatch_context->setSetting("object_storage_distributed_driver_table", driver_storage_id.getTableName());

    /// SourceStepWithFilter checks required_source_columns against storage_snapshot, which here is the driver's.
    /// Pass the driver's own physical columns, exactly as an ordinary per-table read would.
    Names column_names = driver_storage_snapshot->getColumns(GetColumnsOptions(GetColumnsOptions::AllPhysical)).getNames();

    SelectQueryInfo query_info = select_query_info;
    query_info.query = query_to_send;
    query_info.query_tree = dispatch_boundary_node;
    query_info.planner_context = new_planner_context;

    JoinTreeQueryPlan result;
    result.stage = processed_stage;

    driver_storage->readPreparedClusterQuery(
        result.query_plan,
        column_names,
        driver_storage_snapshot,
        query_info,
        dispatch_context,
        processed_stage,
        query_to_send,
        remote_header);

    /// Kept from the parallel-replicas shape. With the query no longer rewritten the two headers are built from
    /// the same tree and should already agree, so this is normally an identity; it stays as the one place that
    /// would catch a divergence rather than let it reach the caller's finalization.
    auto converting_actions = ActionsDAG::makeConvertingActions(
        result.query_plan.getCurrentHeader()->getColumnsWithTypeAndName(),
        remote_header->getColumnsWithTypeAndName(),
        ActionsDAG::MatchColumnsMode::Position,
        context,
        false,
        false,
        nullptr);

    auto converting_step = std::make_unique<ExpressionStep>(result.query_plan.getCurrentHeader(), std::move(converting_actions));
    converting_step->setStepDescription("Convert columns to the original query's header");
    result.query_plan.addStep(std::move(converting_step));

    return result;
}

}
