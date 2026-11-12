#include <Planner/findDistributedObjectStorageCandidate.h>

#include <Access/Common/RowPolicyDefs.h>
#include <Access/ContextAccess.h>
#include <Access/EnabledRowPolicies.h>
#include <Analyzer/AggregationUtils.h>
#include <Analyzer/IQueryTreeNode.h>
#include <Analyzer/JoinNode.h>
#include <Analyzer/QueryNode.h>
#include <Analyzer/TableFunctionNode.h>
#include <Analyzer/TableNode.h>
#include <Analyzer/WindowFunctionsUtils.h>
#include <Core/Settings.h>
#include <Core/SettingsEnums.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/SelectQueryOptions.h>
#include <Storages/IStorageCluster.h>

namespace DB
{
namespace Setting
{
    extern const SettingsObjectStorageClusterJoinMode object_storage_cluster_join_mode;
    extern const SettingsBool object_storage_remote_initiator;
    extern const SettingsMap additional_table_filters;
}

namespace
{

/// Replacing a table with a table function would bypass its row policy.
bool hasEffectiveRowPolicy(const TableNode & table_node, const ContextPtr & context)
{
    const auto & storage_id = table_node.getStorageID();
    auto filter = context->getRowPolicyFilter(
        storage_id.getDatabaseName(), storage_id.getTableName(), RowPolicyFilterType::SELECT_FILTER);
    return filter && !filter->isAlwaysTrue();
}

/// Dispatch bypasses `prepareBuildQueryPlanForTableExpression`, so check access here.
/// If table-level access is absent, ordinary planning enforces column-level grants.
bool hasSelectAccess(const TableNode & table_node, const ContextPtr & context)
{
    const auto & storage_id = table_node.getStorageID();
    return context->getAccess()->isGranted(
        AccessType::SELECT, storage_id.getDatabaseName(), storage_id.getTableName());
}

/// Both the driver and its partners must be readable through catalog-generated table functions.
bool isSafeCatalogTable(const TableNode & table_node, const ContextPtr & context)
{
    const auto & storage_id = table_node.getStorageID();
    if (!storage_id.hasDatabase())
        return false;

    if (!dynamic_cast<const IStorageCluster *>(table_node.getStorage().get()))
        return false;

    if (!DatabaseCatalog::instance().isDatalakeCatalog(storage_id.getDatabaseName()))
        return false;

    return !hasEffectiveRowPolicy(table_node, context) && hasSelectAccess(table_node, context);
}

bool isEligibleDriver(const TableNode & table_node, const ContextPtr & context)
{
    if (!isSafeCatalogTable(table_node, context))
        return false;

    const auto & storage = static_cast<const IStorageCluster &>(*table_node.getStorage());
    return !storage.getClusterName(context).empty();
}

/// An aggregate such as `count` needs finalization even without `GROUP BY`.
bool hasAggregateOrWindowFunctionNodes(const QueryTreeNodePtr & node)
{
    return hasAggregateFunctionNodes(node) || hasWindowFunctionNodes(node);
}

/// Driver subqueries run on separate partitions, so they must not finalize partial results.
/// Partner subqueries are exempt because each worker reads them in full.
bool isSafeIntermediateSubquery(const QueryNode & query_node)
{
    return !query_node.isDistinct() && !query_node.hasGroupBy() && !query_node.hasHaving() && !query_node.hasWindow()
        && !query_node.hasQualify() && !query_node.hasOrderBy() && !query_node.hasInterpolate() && !query_node.hasLimitBy()
        && !query_node.hasLimit() && !query_node.hasOffset() && !hasAggregateOrWindowFunctionNodes(query_node.getProjectionNode())
        && !hasAggregateOrWindowFunctionNodes(query_node.getWithNode());
}

struct DriverPathResult
{
    std::shared_ptr<TableNode> driver;

    /// Single-table reads already use `IStorageCluster::read`.
    bool has_join = false;
};

/// Follow the partitioned side of each `JOIN`. Partners are checked separately and never become drivers.
std::optional<DriverPathResult> findDriverOnLeftSpine(const QueryTreeNodePtr & node, const ContextPtr & context)
{
    if (const auto * table_node = node->as<TableNode>())
    {
        if (!isEligibleDriver(*table_node, context))
            return {};

        return DriverPathResult{.driver = std::static_pointer_cast<TableNode>(node)};
    }

    if (const auto * query_node = node->as<QueryNode>())
    {
        const auto & join_tree = query_node->getJoinTreeNode();
        if (!join_tree || !isSafeIntermediateSubquery(*query_node))
            return {};

        return findDriverOnLeftSpine(join_tree, context);
    }

    if (const auto * join_node = node->as<JoinNode>())
    {
        const auto join_kind = join_node->getKind();
        const auto join_strictness = join_node->getStrictness();
        if ((join_kind != JoinKind::Inner || join_strictness != JoinStrictness::All) && join_kind != JoinKind::Left)
            return {};

        auto left = findDriverOnLeftSpine(join_node->getLeftTableExpressionNode(), context);
        if (!left)
            return {};

        left->has_join = true;
        return left;
    }

    return {};
}

/// Workers recompute every partner in full, including nested queries.
/// This checks tables only: dictionaries and functions such as `dictGet` or `hostName` must be consistent
/// across workers, as documented for this setting and assumed by `Distributed`.
bool allWorkerLocalTableReferencesAreSafe(const QueryTreeNodePtr & node, const TableNode * driver, const ContextPtr & context)
{
    if (!node)
        return true;

    if (const auto * table_node = node->as<TableNode>())
        return table_node == driver || isSafeCatalogTable(*table_node, context);

    if (node->as<TableFunctionNode>())
        return false;

    for (const auto & child : node->getChildren())
        if (!allWorkerLocalTableReferencesAreSafe(child, driver, context))
            return false;

    return true;
}

/// The checks use the outer query's settings, but a nested `SETTINGS` clause applies only to its own scope,
/// e.g. `additional_table_filters` or `object_storage_cluster` set for the driver's subquery.
bool hasNestedSettingsChanges(const QueryTreeNodePtr & node)
{
    for (const auto & child : node->getChildren())
    {
        if (!child)
            continue;

        if (const auto * query_node = child->as<QueryNode>(); query_node && query_node->hasSettingsChanges())
            return true;

        if (hasNestedSettingsChanges(child))
            return true;
    }

    return false;
}

}

std::optional<DistributedObjectStorageCandidate> findDistributedObjectStorageCandidate(
    const QueryTreeNodePtr & query_node, const ContextPtr & context)
{
    if (context->getSettingsRef()[Setting::object_storage_cluster_join_mode] != ObjectStorageClusterJoinMode::DISTRIBUTED)
        return {};

    /// Dispatch does not implement the remote-initiator topology from `IStorageCluster::read`.
    if (context->getSettingsRef()[Setting::object_storage_remote_initiator])
        return {};

    /// Serialization changes the names used to match `additional_table_filters`.
    /// Parallel replicas are disabled for the same reason.
    if (!context->getSettingsRef()[Setting::additional_table_filters].value.empty())
        return {};

    if (hasNestedSettingsChanges(query_node))
        return {};

    const auto * query_node_typed = query_node->as<QueryNode>();
    if (!query_node_typed)
        return {};

    const auto & join_tree = query_node_typed->getJoinTreeNode();
    if (!join_tree)
        return {};

    auto driver_path = findDriverOnLeftSpine(join_tree, context);
    if (!driver_path || !driver_path->has_join)
        return {};

    if (!allWorkerLocalTableReferencesAreSafe(query_node, driver_path->driver.get(), context))
        return {};

    return DistributedObjectStorageCandidate{.dispatch_boundary = query_node_typed, .driver = std::move(driver_path->driver)};
}

std::optional<DistributedObjectStorageCandidate> findDistributedObjectStorageCandidate(
    const QueryTreeNodePtr & query_tree_node, const SelectQueryOptions & select_query_options)
{
    /// Header analysis in `buildDistributedObjectStorageQueryPlan` must not dispatch recursively.
    if (select_query_options.only_analyze)
        return {};

    /// A plan built for a shard is serialized, and `ReadFromClusterQuery` has no serialized form.
    if (select_query_options.build_logical_plan)
        return {};

    /// Independently interpreted subqueries number their `__tableN` identifiers locally.
    /// Their dispatched headers would not match the enclosing query's names.
    if (select_query_options.is_subquery)
        return {};

    const auto * query_node = query_tree_node->as<QueryNode>();
    if (!query_node)
        return {};

    /// A worker executes what it was sent; it never dispatches onwards.
    const auto & context = query_node->getContext();
    if (context->getClientInfo().query_kind != ClientInfo::QueryKind::INITIAL_QUERY)
        return {};

    return findDistributedObjectStorageCandidate(query_tree_node, context);
}

}
