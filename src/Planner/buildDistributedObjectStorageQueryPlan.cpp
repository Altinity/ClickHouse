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
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Planner/PlannerContext.h>
#include <Planner/Utils.h>
#include <Storages/IStorageCluster.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/StorageSnapshot.h>
#include <Storages/VirtualColumnUtils.h>
#include <Storages/removeGroupingFunctionSpecializations.h>

#include <map>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

namespace
{

/// Table nodes by database and table name, as the serialized query names them.
using TableNodesByName = std::map<std::pair<String, String>, const TableNode *>;

void collectTableNodes(const QueryTreeNodePtr & node, TableNodesByName & table_nodes)
{
    if (const auto * table_node = node->as<TableNode>())
    {
        const auto & storage_id = table_node->getStorageID();
        table_nodes.emplace(std::pair{storage_id.database_name, storage_id.table_name}, table_node);
    }

    for (const auto & child : node->getChildren())
        if (child)
            collectTableNodes(child, table_nodes);
}

/// Replaces every table named in `ast` with the table function a worker reads it through: the driver's `*Cluster`
/// function reads the files this node hands out, any other table's plain function reads them all. Returns how
/// many references to the driver it replaced. The text is serialized after CTE bodies are inlined, so a CTE over
/// the driver used twice counts twice.
size_t replaceTablesWithTableFunctions(
    const ASTPtr & ast, const TableNodesByName & table_nodes, const TableNode & driver, const ContextPtr & context)
{
    auto * table_expression = ast->as<ASTTableExpression>();
    if (!table_expression || !table_expression->database_and_table_name)
    {
        size_t driver_references = 0;
        for (const auto & child : ast->children)
            driver_references += replaceTablesWithTableFunctions(child, table_nodes, driver, context);
        return driver_references;
    }

    const auto & identifier = table_expression->database_and_table_name->as<ASTTableIdentifier &>();
    const auto storage_id = identifier.getTableId();

    /// findDistributedObjectStorageCandidate admits only DataLake catalog tables, all of them `IStorageCluster`s.
    auto it = table_nodes.find({storage_id.database_name, storage_id.table_name});
    auto * storage = it == table_nodes.end() ? nullptr : dynamic_cast<IStorageCluster *>(it->second->getStorage().get());
    if (!storage)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Whole-query dispatch: table {} is not a cluster table of the query", storage_id.getNameForLogs());

    const bool is_driver = it->second->getStorage() == driver.getStorage();
    auto table_function = storage->getTableFunctionToSend(it->second->getStorageSnapshot(), context, /*read_initiator_file_tasks=*/is_driver);
    table_function->setAlias(identifier.tryGetAlias());

    table_expression->reset(table_expression->database_and_table_name);
    table_expression->set(table_expression->table_function, table_function);
    return is_driver ? 1 : 0;
}

}

/// Follows buildQueryPlanForParallelReplicas (Planner/findParallelReplicasQuery.cpp): header of the query ->
/// serialize to SQL -> remote read.
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
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Whole-query dispatch: expected a plain SELECT at the dispatch boundary");

    /// As `Distributed` does in rewriteSelectQuery: the query's settings are already in its context and travel
    /// with it, so the clause is dropped rather than sent. A worker applies a query's own SETTINGS on top of
    /// the settings it received, and would otherwise restore anything the initiator changed for the dispatch --
    /// `object_storage_cluster`.
    select_query->setExpression(ASTSelectQuery::Expression::SETTINGS, {});

    /// A worker resolves no catalog table: every table is sent as a table function, and an Iceberg table's
    /// function names the metadata file the catalog resolved here. A driver referenced twice -- a self-join, or
    /// a CTE over it used twice -- would read the initiator's file-task queue twice, which is silently wrong, so
    /// fall back to ordinary planning instead.
    TableNodesByName table_nodes;
    collectTableNodes(dispatch_boundary_node, table_nodes);
    if (replaceTablesWithTableFunctions(query_to_send, table_nodes, *candidate.driver, context) != 1)
        return {};

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
        context,
        processed_stage,
        query_to_send,
        remote_header,
        std::move(driver_filter));

    /// No converting step, unlike buildQueryPlanForParallelReplicas: tables are replaced only in the serialized
    /// text, after the header was computed, so the header the workers produce is `remote_header` itself.

    return result;
}

}
