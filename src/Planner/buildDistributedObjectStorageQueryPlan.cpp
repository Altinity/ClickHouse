#include <Planner/buildDistributedObjectStorageQueryPlan.h>

#include <Analyzer/ColumnNode.h>
#include <Analyzer/FunctionNode.h>
#include <Analyzer/QueryNode.h>
#include <Analyzer/TableNode.h>
#include <Analyzer/UnionNode.h>
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


/// Splits a condition into its top-level `and` operands. Anything that is not an `and` is one atom.
void collectConjunctionAtoms(const QueryTreeNodePtr & node, QueryTreeNodes & atoms)
{
    if (const auto * function_node = node->as<FunctionNode>(); function_node && function_node->getFunctionName() == "and")
    {
        for (const auto & argument : function_node->getArguments().getNodes())
            collectConjunctionAtoms(argument, atoms);
        return;
    }

    atoms.push_back(node);
}

/// True when every column this condition reads comes from `driver` and nothing in it has to be executed to be
/// understood. A subquery is rejected outright: this condition is evaluated while listing the driver's files,
/// long before there is a pipeline to run one in.
bool readsOnlyDriverColumns(const QueryTreeNodePtr & node, const TableNode * driver)
{
    if (node->as<QueryNode>() || node->as<UnionNode>())
        return false;

    if (const auto * column_node = node->as<ColumnNode>())
        return column_node->getColumnSource().get() == driver;

    for (const auto & child : node->getChildren())
        if (child && !readsOnlyDriverColumns(child, driver))
            return false;

    return true;
}

/// The predicate over the driver's own columns, as an ActionsDAG the file listing can prune with.
///
/// Correctness rests on two restrictions `findDistributedObjectStorageCandidate` already enforces, and would
/// break if either were relaxed: the driver sits on the left spine of INNER ALL / LEFT joins only, so a driver
/// row dropped here cannot have produced a result row; and every QueryNode crossed to reach it is
/// partition-preserving (`isSafeIntermediateSubquery` -- no GROUP BY, DISTINCT, LIMIT, window), so dropping a
/// row cannot change what the surviving rows compute.
///
/// Returns nothing when no atom qualifies, which simply means every file is listed.
std::optional<ActionsDAG> buildDriverOnlyFilter(
    const DistributedObjectStorageCandidate & candidate, const PlannerContextPtr & planner_context)
{
    QueryTreeNodes atoms;
    for (const auto * query_node : candidate.query_nodes_on_driver_path)
    {
        if (query_node->hasPrewhere())
            collectConjunctionAtoms(query_node->getPrewhere(), atoms);
        if (query_node->hasWhere())
            collectConjunctionAtoms(query_node->getWhere(), atoms);
    }

    QueryTreeNodes driver_atoms;
    for (const auto & atom : atoms)
        if (readsOnlyDriverColumns(atom, candidate.driver))
            driver_atoms.push_back(atom->clone());

    if (driver_atoms.empty())
        return {};

    const auto context = planner_context->getQueryContext();
    /// mergeConditionNodes always builds an `and`, which needs at least two arguments.
    auto condition = driver_atoms.size() == 1 ? driver_atoms.front() : mergeConditionNodes(driver_atoms, context);

    /// Passed explicitly so buildFilterInfo does not go looking for this table expression in the planner
    /// context: the dispatch boundary is planned as one unit and never registers the driver on its own.
    const auto driver_columns = candidate.driver->getStorageSnapshot()->metadata->getColumns().getNamesOfPhysical();
    NameSet required_names(driver_columns.begin(), driver_columns.end());

    auto mutable_planner_context = planner_context;
    auto filter_info = buildFilterInfo(
        std::move(condition), candidate.driver_table_expression, mutable_planner_context, std::move(required_names));

    return std::move(filter_info.actions);
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

    SelectQueryInfo query_info = select_query_info;
    query_info.query = query_to_send;
    query_info.query_tree = dispatch_boundary_node;
    query_info.planner_context = new_planner_context;

    JoinTreeQueryPlan result;
    result.stage = processed_stage;

    /// Prunes the driver's file listing. Without it every file of the driver is handed out and each worker
    /// opens the ones its partitions cannot match only to discard them.
    std::shared_ptr<const ActionsDAG> driver_filter;
    if (auto filter_dag = buildDriverOnlyFilter(candidate, planner_context))
    {
        VirtualColumnUtils::buildSetsForDAGExcludingGlobalIn(*filter_dag, context);
        driver_filter = std::make_shared<const ActionsDAG>(std::move(*filter_dag));
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
