#include <Planner/buildDistributedObjectStorageQueryPlan.h>

#include <Analyzer/FunctionNode.h>
#include <Analyzer/Passes/QueryAnalysisPass.h>
#include <Analyzer/QueryNode.h>
#include <Analyzer/QueryTreeBuilder.h>
#include <Analyzer/TableFunctionNode.h>
#include <Analyzer/TableNode.h>
#include <Common/Exception.h>
#include <Interpreters/ActionsDAG.h>
#include <Interpreters/Context.h>
#include <Interpreters/InterpreterSelectQueryAnalyzer.h>
#include <Interpreters/SelectQueryOptions.h>
#include <Processors/QueryPlan/ExpressionStep.h>
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

/// This mirrors buildQueryPlanForParallelReplicas (Planner/findParallelReplicasQuery.cpp) step for step:
/// header of the original query -> rewrite the tree -> header of the rewritten tree -> serialize to SQL ->
/// remote read -> convert the remote header back to the original one by position. Keep the two in sync.
JoinTreeQueryPlan buildDistributedObjectStorageQueryPlan(
    const QueryTreeNodePtr & dispatch_boundary_node,
    const DistributedObjectStorageCandidate & candidate,
    const SelectQueryInfo & select_query_info,
    const PlannerContextPtr & planner_context)
{
    const auto context = planner_context->getQueryContext();
    constexpr auto processed_stage = QueryProcessingStage::WithMergeableState;

    /// The header the unmodified query would have produced, so the caller's finalization sees the column
    /// names/types it expects.
    auto initial_header = InterpreterSelectQueryAnalyzer::getSampleBlock(
        dispatch_boundary_node->clone(), context, SelectQueryOptions(processed_stage).analyze());

    /// Reuse the snapshot the analyzer resolved the driver against, so the dispatched query and its replacement
    /// are built from the same metadata version.
    auto * driver_storage = candidate.driver_storage;
    const auto & driver_storage_snapshot = candidate.driver->getStorageSnapshot();

    auto cluster_function_ast = driver_storage->buildClusterTableFunctionAST(
        driver_storage->getClusterName(context), driver_storage_snapshot, context);

    auto cluster_function_query_tree = buildQueryTree(cluster_function_ast, context);
    auto & cluster_function_node = cluster_function_query_tree->as<FunctionNode &>();

    auto replacement = std::make_shared<TableFunctionNode>(cluster_function_node.getFunctionName());
    replacement->getArgumentsNode() = cluster_function_node.getArgumentsNode();
    replacement->setSettingsChanges(cluster_function_node.getSettingsChanges());
    if (candidate.driver->hasTableExpressionModifiers())
        replacement->setTableExpressionModifiers(*candidate.driver->getTableExpressionModifiers());
    replacement->setAlias(candidate.driver->getAlias());

    {
        QueryAnalysisPass query_analysis_pass;
        QueryTreeNodePtr node = replacement;
        query_analysis_pass.run(node, context);
    }

    /// Exact-node replacement, as StorageDistributed::buildQueryTreeDistributed does. cloneAndReplace rebinds
    /// every weak reference to the driver (e.g. ColumnNode sources) elsewhere in the tree.
    IQueryTreeNode::ReplacementMap replacement_map;
    replacement_map.emplace(candidate.driver, replacement);
    auto modified_query_tree = dispatch_boundary_node->cloneAndReplace(replacement_map);

    auto [remote_header, new_planner_context] = InterpreterSelectQueryAnalyzer::getSampleBlockAndPlannerContext(
        modified_query_tree, context, SelectQueryOptions(processed_stage).analyze());

    /// Strip grouping-function specializations in a separate clone: the workers re-resolve the generic function
    /// themselves, but modified_query_tree must keep them, having already produced the header above.
    auto query_tree_for_ast = modified_query_tree->clone();
    removeGroupingFunctionSpecializations(query_tree_for_ast);
    ASTPtr query_to_send = queryNodeToDistributedSelectQuery(query_tree_for_ast);

    if (!query_to_send->as<ASTSelectQuery>())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Distributed object-storage dispatch: expected a plain SELECT at the dispatch boundary");

    /// SourceStepWithFilter checks required_source_columns against storage_snapshot, which here is the driver's.
    /// Pass the driver's own physical columns, exactly as an ordinary per-table read would.
    Names column_names = driver_storage_snapshot->getColumns(GetColumnsOptions(GetColumnsOptions::AllPhysical)).getNames();

    SelectQueryInfo query_info = select_query_info;
    query_info.query = query_to_send;
    query_info.query_tree = modified_query_tree;
    query_info.planner_context = new_planner_context;

    JoinTreeQueryPlan result;
    result.stage = processed_stage;

    driver_storage->readPreparedClusterQuery(
        result.query_plan,
        column_names,
        driver_storage_snapshot,
        query_info,
        context,
        processed_stage,
        query_to_send,
        remote_header);

    /// The rewritten query numbers its tables independently, so the remote header's column names differ from the
    /// original's (e.g. `__table1` vs `__table5`) even though the types line up. Rename by position, the same way
    /// buildQueryPlanForParallelReplicas does. Aggregates are still AggregateFunction(...) at this stage.
    auto converting_actions = ActionsDAG::makeConvertingActions(
        result.query_plan.getCurrentHeader()->getColumnsWithTypeAndName(),
        initial_header->getColumnsWithTypeAndName(),
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
