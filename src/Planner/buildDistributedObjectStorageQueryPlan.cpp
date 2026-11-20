#include <Planner/buildDistributedObjectStorageQueryPlan.h>

#include <Analyzer/ColumnNode.h>
#include <Analyzer/QueryNode.h>
#include <Analyzer/TableNode.h>
#include <Common/Exception.h>
#include <Common/quoteString.h>
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
#include <set>

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

/// The tables the query reads, in every nested subquery and CTE, with the columns read from each. The header
/// analysis does not plan nested scopes, so their tables do not reach the planner context.
struct AccessedTable
{
    StorageID storage_id = StorageID::createEmpty();
    std::set<String> columns;
};
using AccessedTables = std::map<String, AccessedTable>;

void collectAccessedTables(const QueryTreeNodePtr & node, AccessedTables & accessed_tables)
{
    const TableNode * table_node = node->as<TableNode>();
    if (!table_node)
        if (const auto * column_node = node->as<ColumnNode>())
        {
            auto column_source = column_node->getColumnSourceOrNull();
            table_node = column_source ? column_source->as<TableNode>() : nullptr;
            if (table_node)
                accessed_tables[table_node->getStorageID().getFullTableName()].columns.insert(column_node->getColumnName());
        }

    if (table_node)
    {
        auto & accessed_table = accessed_tables[table_node->getStorageID().getFullTableName()];
        accessed_table.storage_id = table_node->getStorageID();
    }

    for (const auto & child : node->getChildren())
        if (child)
            collectAccessedTables(child, accessed_tables);
}

/// Only the driver's `*Cluster` function consumes file tasks; partners use plain table functions.
/// Count driver references after CTE expansion so repeated uses cannot share one task queue.
/// Returns `std::nullopt` when a partner does not pin the snapshot the initiator resolved: each worker would then
/// resolve the partner's latest snapshot itself, and a commit during the query would give different driver files
/// different partner snapshots. The ordinary path reads a partner once, on the initiator.
std::optional<size_t> replaceTablesWithTableFunctions(
    const ASTPtr & ast, const TableNodesByName & table_nodes, const TableNode & driver, const ContextPtr & context)
{
    auto * table_expression = ast->as<ASTTableExpression>();
    if (!table_expression || !table_expression->database_and_table_name)
    {
        size_t driver_references = 0;
        for (const auto & child : ast->children)
        {
            auto child_references = replaceTablesWithTableFunctions(child, table_nodes, driver, context);
            if (!child_references)
                return std::nullopt;
            driver_references += *child_references;
        }
        return driver_references;
    }

    const auto & identifier = table_expression->database_and_table_name->as<ASTTableIdentifier &>();
    const auto storage_id = identifier.getTableId();

    /// `findDistributedObjectStorageCandidate` admits only catalog tables backed by `IStorageCluster`.
    auto it = table_nodes.find({storage_id.database_name, storage_id.table_name});
    auto * storage = it == table_nodes.end() ? nullptr : dynamic_cast<IStorageCluster *>(it->second->getStorage().get());
    if (!storage)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Whole-query dispatch: table {} is not a cluster table of the query", storage_id.getNameForLogs());

    const bool is_driver = it->second->getStorage() == driver.getStorage();
    auto table_function = storage->getTableFunctionToSend(it->second->getStorageSnapshot(), context, /*read_initiator_file_tasks=*/is_driver);
    if (!is_driver && !storage->isSnapshotPinnedByTableFunction(table_function))
        return std::nullopt;

    table_function->setAlias(identifier.tryGetAlias());

    table_expression->reset(table_expression->database_and_table_name);
    table_expression->set(table_expression->table_function, table_function);
    return is_driver ? 1 : 0;
}

/// Reuse the predicate collected for the driver's ordinary read to prune its file listing.
/// Filters on the whole query's result do not describe which driver files can be skipped.
std::shared_ptr<const ActionsDAG> buildDriverFilter(
    const QueryTreeNodePtr & driver, const PlannerContextPtr & planner_context)
{
    const auto & table_filters = planner_context->getGlobalPlannerContext()->filters_for_table_expressions;
    auto it = table_filters.find(driver);
    if (it == table_filters.end() || !it->second.filter_actions)
        return {};

    auto filter = it->second.filter_actions->clone();
    VirtualColumnUtils::buildSetsForDAGExcludingGlobalIn(filter, planner_context->getQueryContext());
    return std::make_shared<const ActionsDAG>(std::move(filter));
}

}

/// Follows `buildQueryPlanForParallelReplicas`: compute the header, serialize the query, then build a remote read.
std::optional<JoinTreeQueryPlan> buildDistributedObjectStorageQueryPlan(
    const QueryTreeNodePtr & dispatch_boundary_node,
    const DistributedObjectStorageCandidate & candidate,
    const SelectQueryInfo & select_query_info,
    const SelectQueryOptions & select_query_options,
    const PlannerContextPtr & planner_context)
{
    const auto context = planner_context->getQueryContext();
    constexpr auto processed_stage = QueryProcessingStage::WithMergeableState;

    auto & driver_storage = static_cast<IStorageCluster &>(*candidate.driver->getStorage());

    /// Compute the header before replacing tables in the serialized query, preserving the resolved column names.
    auto [remote_header, new_planner_context] = InterpreterSelectQueryAnalyzer::getSampleBlockAndPlannerContext(
        dispatch_boundary_node->clone(), context, SelectQueryOptions(processed_stage).analyze());

    /// Workers resolve grouping functions again. Keep specializations in the tree used for the header.
    auto query_tree_for_ast = dispatch_boundary_node->clone();
    removeGroupingFunctionSpecializations(query_tree_for_ast);
    ASTPtr query_to_send = queryNodeToDistributedSelectQuery(query_tree_for_ast);

    auto * select_query = query_to_send->as<ASTSelectQuery>();
    if (!select_query)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Whole-query dispatch: expected a plain SELECT at the dispatch boundary");

    /// Settings travel in the context, as in `Distributed`. Sending the `SETTINGS` clause too would override
    /// dispatch adjustments such as clearing `object_storage_cluster` on workers.
    select_query->setExpression(ASTSelectQuery::Expression::SETTINGS, {});

    /// Table functions carry the catalog-resolved metadata to workers.
    /// Repeated driver references would consume one task queue independently and produce incomplete joins.
    TableNodesByName table_nodes;
    collectTableNodes(dispatch_boundary_node, table_nodes);
    const auto driver_references = replaceTablesWithTableFunctions(query_to_send, table_nodes, *candidate.driver, context);
    if (driver_references != 1)
        return {};

    SelectQueryInfo query_info = select_query_info;
    query_info.query = query_to_send;
    query_info.query_tree = dispatch_boundary_node;
    query_info.planner_context = new_planner_context;

    JoinTreeQueryPlan result;
    result.stage = processed_stage;

    driver_storage.readPreparedClusterQuery(
        result.query_plan,
        candidate.driver->getStorageSnapshot(),
        query_info,
        context,
        processed_stage,
        query_to_send,
        remote_header,
        buildDriverFilter(candidate.driver, planner_context));

    /// Workers receive table functions, so the original catalog tables are recorded here. Only after
    /// the dispatch is built: a declined dispatch is planned, and recorded, by the ordinary path.
    if (context->hasQueryContext() && !select_query_options.is_internal)
    {
        AccessedTables accessed_tables;
        collectAccessedTables(dispatch_boundary_node, accessed_tables);

        for (const auto & [full_table_name, accessed_table] : accessed_tables)
            context->getQueryContext()->addQueryAccessInfo(
                backQuoteIfNeed(accessed_table.storage_id.getDatabaseName()),
                full_table_name,
                Names(accessed_table.columns.begin(), accessed_table.columns.end()));
    }

    return result;
}

}
