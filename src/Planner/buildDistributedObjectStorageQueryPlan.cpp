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

JoinTreeQueryPlan buildDistributedObjectStorageQueryPlan(
    const QueryTreeNodePtr & dispatch_boundary_node,
    const DistributedObjectStorageCandidate & candidate,
    const SelectQueryInfo & select_query_info,
    const PlannerContextPtr & planner_context)
{
    const auto context = planner_context->getQueryContext();
    constexpr auto processed_stage = QueryProcessingStage::WithMergeableState;

    /// The header stock (unmodified) planning would have produced, computed against the query tree before
    /// the driver is rewritten -- so downstream code (the caller's own finalization) sees exactly the column
    /// names/types it would have without this optimization, matching buildQueryPlanForParallelReplicas()'s own
    /// original-vs-worker header handling.
    auto initial_header = InterpreterSelectQueryAnalyzer::getSampleBlock(
        dispatch_boundary_node->clone(), context, SelectQueryOptions(processed_stage).analyze());

    /// Reuses the exact snapshot the analyzer resolved the driver against (TableNode owns it), rather than
    /// fetching a fresh one here: metadata could otherwise have changed between analysis and dispatch, leaving
    /// the dispatched query resolved against one snapshot and the replacement built from another.
    const auto & driver_storage_snapshot = candidate.driver->getStorageSnapshot();

    auto cluster_function_ast = candidate.driver_storage->buildClusterTableFunctionAST(
        candidate.driver_storage->getClusterName(context), driver_storage_snapshot, context);

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

    /// candidate.driver's own subtree is not traversed further -- it becomes a leaf, exact-node replacement,
    /// mirroring StorageDistributed::buildQueryTreeDistributed()'s own pattern. cloneAndReplace() rebinds every
    /// weak reference (e.g. ColumnNode source pointers) elsewhere in the tree from the old node to
    /// `replacement`.
    IQueryTreeNode::ReplacementMap replacement_map;
    replacement_map.emplace(candidate.driver, replacement);
    auto modified_query_tree = dispatch_boundary_node->cloneAndReplace(replacement_map);

    auto [remote_header, new_planner_context] = InterpreterSelectQueryAnalyzer::getSampleBlockAndPlannerContext(
        modified_query_tree, context, SelectQueryOptions(processed_stage).analyze());

    /// Convert grouping function specializations (e.g. groupingForGroupingSets -> grouping) in a separate
    /// clone so the AST sent to the driver's cluster contains the generic function name that can be
    /// re-resolved by each worker's own analyzer -- modified_query_tree itself must keep the specialized
    /// functions, since it was already used above for header computation and its planner context.
    auto query_tree_for_ast = modified_query_tree->clone();
    removeGroupingFunctionSpecializations(query_tree_for_ast);
    ASTPtr query_to_send = queryNodeToDistributedSelectQuery(query_tree_for_ast);

    if (!query_to_send->as<ASTSelectQuery>())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Distributed object-storage dispatch: expected a plain SELECT at the dispatch boundary");

    /// SourceStepWithFilter::required_source_columns (and thus updatePrewhereInfo()'s own
    /// driver_storage_snapshot->getSampleBlockForColumns(required_source_columns) lookup) is checked against
    /// storage_snapshot, i.e. the driver's own snapshot here -- not the whole dispatched query's output schema
    /// (that's remote_header, a separate concept). Use the driver's own physical columns, matching what a
    /// normal per-table read() of the driver alone would pass.
    Names column_names = driver_storage_snapshot->getColumns(GetColumnsOptions(GetColumnsOptions::AllPhysical)).getNames();

    SelectQueryInfo query_info = select_query_info;
    query_info.query = query_to_send;
    query_info.query_tree = modified_query_tree;
    query_info.planner_context = new_planner_context;

    JoinTreeQueryPlan result;
    result.stage = processed_stage;

    candidate.driver_storage->readPreparedClusterQuery(
        result.query_plan,
        column_names,
        driver_storage_snapshot,
        query_info,
        context,
        processed_stage,
        query_to_send,
        remote_header);

    /// The remote result's header uses whatever column naming the rewritten/re-analyzed query produced;
    /// convert it back, by position, to the header the unmodified query would have produced -- e.g. an
    /// aggregate like sum() is still AggregateFunction(sum, ...) at this stage, not its finalized type,
    /// matching buildQueryPlanForParallelReplicas()'s own original-vs-worker header conversion. Generic and
    /// position-based rather than the previous per-projection-node ColumnNode renaming, which broke down for
    /// a complex projection mixing CASE expressions over both JOIN sides with an aggregate.
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
