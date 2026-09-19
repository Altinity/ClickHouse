#include <Planner/findDistributedObjectStorageCandidate.h>

#include <Access/Common/RowPolicyDefs.h>
#include <Access/ContextAccess.h>
#include <Access/EnabledRowPolicies.h>
#include <Analyzer/FunctionNode.h>
#include <Analyzer/IQueryTreeNode.h>
#include <Analyzer/JoinNode.h>
#include <Analyzer/QueryNode.h>
#include <Analyzer/TableFunctionNode.h>
#include <Analyzer/TableNode.h>
#include <Analyzer/UnionNode.h>
#include <Core/Settings.h>
#include <Core/SettingsEnums.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
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

/// Dispatch drops the initiator's row policies: the driver is rewritten into an explicit `*Cluster()` call and
/// every partner is re-resolved independently by each worker, so neither carries the policy across. Reject.
bool hasEffectiveRowPolicy(const TableNode & table_node, const ContextPtr & context)
{
    const auto & storage_id = table_node.getStorageID();
    auto filter = context->getRowPolicyFilter(
        storage_id.getDatabaseName(), storage_id.getTableName(), RowPolicyFilterType::SELECT_FILTER);
    return filter && !filter->isAlwaysTrue();
}

/// Dispatch replaces the per-table access check that `prepareBuildQueryPlanForTableExpression` would have run on
/// each table, so do it here instead. Table-level and non-throwing: a failure falls back to ordinary planning,
/// which then enforces the real column-level rules with its own error.
bool hasSelectAccess(const TableNode & table_node, const ContextPtr & context)
{
    const auto & storage_id = table_node.getStorageID();
    return context->getAccess()->isGranted(
        AccessType::SELECT, storage_id.getDatabaseName(), storage_id.getTableName());
}

/// Safe to include in the dispatch, as driver or as partner: resolved through a DataLake catalog, so every worker
/// re-resolves it identically; no row policy; visible to the user.
bool isSafeDataLakeLeaf(const TableNode & table_node, const ContextPtr & context)
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

bool isEligibleDriver(const TableNode & table_node, const ContextPtr & context, IStorageCluster *& out_storage)
{
    if (!isSafeDataLakeLeaf(table_node, context))
        return false;

    auto * storage = dynamic_cast<IStorageCluster *>(table_node.getStorage().get());
    if (storage->getClusterName(context).empty())
        return false;

    out_storage = storage;
    return true;
}

/// Aggregate/window function in this node's own subtree, not crossing into a nested query. Catches
/// `SELECT count() FROM driver`, which has no GROUP BY node at all.
bool containsAggregateOrWindowFunction(const QueryTreeNodePtr & node)
{
    if (!node)
        return false;

    if (const auto * function_node = node->as<FunctionNode>())
        if (function_node->isAggregateFunction() || function_node->isWindowFunction())
            return true;

    if (node->as<QueryNode>() || node->as<UnionNode>())
        return false;

    for (const auto & child : node->getChildren())
        if (containsAggregateOrWindowFunction(child))
            return true;

    return false;
}

/// A subquery crossed on the way down to the driver runs independently on each worker's own partition of the
/// driver, so anything that finalizes across rows (aggregation, DISTINCT, LIMIT, ...) would turn a partial result
/// into a final one whenever a group spans two workers. Reject rather than prove which ones are partition-safe.
/// Does not apply to a JOIN partner's subquery, which every worker recomputes in full.
bool isSafeIntermediateSubquery(const QueryNode & query_node)
{
    return !query_node.isDistinct() && !query_node.hasGroupBy() && !query_node.hasHaving() && !query_node.hasWindow()
        && !query_node.hasQualify() && !query_node.hasOrderBy() && !query_node.hasInterpolate() && !query_node.hasLimitBy()
        && !query_node.hasLimit() && !query_node.hasOffset() && !containsAggregateOrWindowFunction(query_node.getProjectionNode())
        && !containsAggregateOrWindowFunction(query_node.getWithNode());
}

struct DriverPathResult
{
    bool unusable = false;
    const TableNode * driver = nullptr;
    IStorageCluster * driver_storage = nullptr;

    /// No JOIN on the path means there is nothing here for this mode to optimize; stock `IStorageCluster::read`
    /// already handles a plain single-table cluster read.
    bool has_join = false;
};

DriverPathResult unusableDriverPath()
{
    DriverPathResult result;
    result.unusable = true;
    return result;
}

/// Walks strictly down the left spine for exactly one driver. The right side of a JOIN is never inspected here --
/// it is validated separately as worker-local content by `allWorkerLocalTableReferencesAreSafe`, which is why a
/// DataLake table (or a whole nested JOIN/GROUP BY) on the right never competes for the driver role.
DriverPathResult findDriverOnLeftSpine(const QueryTreeNodePtr & node, const ContextPtr & context)
{
    if (const auto * table_node = node->as<TableNode>())
    {
        IStorageCluster * storage = nullptr;
        if (!isEligibleDriver(*table_node, context, storage))
            return unusableDriverPath();

        DriverPathResult result;
        result.driver = table_node;
        result.driver_storage = storage;
        return result;
    }

    if (const auto * query_node = node->as<QueryNode>())
    {
        const auto & join_tree = query_node->getJoinTree();
        if (!join_tree)
            return unusableDriverPath();

        auto result = findDriverOnLeftSpine(join_tree, context);
        if (result.unusable)
            return result;

        /// Crossed as an intermediate subquery, not as the dispatch boundary (that is the node originally passed
        /// to findDistributedObjectStorageCandidate).
        if (!isSafeIntermediateSubquery(*query_node))
            return unusableDriverPath();

        return result;
    }

    if (const auto * join_node = node->as<JoinNode>())
    {
        const auto join_kind = join_node->getKind();
        const auto join_strictness = join_node->getStrictness();
        if ((join_kind != JoinKind::Inner || join_strictness != JoinStrictness::All) && join_kind != JoinKind::Left)
            return unusableDriverPath();

        auto left = findDriverOnLeftSpine(join_node->getLeftTableExpression(), context);
        if (left.unusable)
            return unusableDriverPath();

        left.has_join = true;
        return left;
    }

    return unusableDriverPath();
}

/// Everything else reachable from the dispatch boundary is recomputed in full on every worker, so every table it
/// reaches must re-resolve identically there. No structural restrictions apply here (unlike the driver's own
/// path), because none of this content is partitioned.
///
/// Proves this for table references only. Ordinary FunctionNodes are accepted unexamined, so anything the query
/// calls that is server-local or externally backed -- `hostName`, a dictionary via `dictGet`, a user-defined
/// function -- moves from the initiator to the workers and must be present and consistent across the cluster.
/// Same assumption `Distributed` makes; stated in the setting's own documentation.
bool allWorkerLocalTableReferencesAreSafe(const QueryTreeNodePtr & node, const TableNode * driver, const ContextPtr & context)
{
    if (!node)
        return true;

    if (const auto * table_node = node->as<TableNode>())
        return table_node == driver || isSafeDataLakeLeaf(*table_node, context);

    if (node->as<TableFunctionNode>())
        return false;

    for (const auto & child : node->getChildren())
        if (!allWorkerLocalTableReferencesAreSafe(child, driver, context))
            return false;

    return true;
}

}

std::optional<DistributedObjectStorageCandidate> findDistributedObjectStorageCandidate(
    const QueryTreeNodePtr & query_node, const ContextPtr & context)
{
    if (context->getSettingsRef()[Setting::object_storage_cluster_join_mode] != ObjectStorageClusterJoinMode::DISTRIBUTED)
        return {};

    /// Dispatch goes straight to the driver's cluster, bypassing the remote-initiator topology that
    /// `IStorageCluster::read` would apply via convertToRemote.
    if (context->getSettingsRef()[Setting::object_storage_remote_initiator])
        return {};

    /// additional_table_filters is keyed by the initiator's current_database and the query's own naming, both of
    /// which shift once the query is serialized for remote execution. Planner.cpp disables parallel replicas for
    /// the same reason.
    if (!context->getSettingsRef()[Setting::additional_table_filters].value.empty())
        return {};

    const auto * query_node_typed = query_node->as<QueryNode>();
    if (!query_node_typed)
        return {};

    const auto & join_tree = query_node_typed->getJoinTree();
    if (!join_tree)
        return {};

    auto driver_path = findDriverOnLeftSpine(join_tree, context);
    if (driver_path.unusable || !driver_path.driver || !driver_path.has_join)
        return {};

    if (!allWorkerLocalTableReferencesAreSafe(query_node, driver_path.driver, context))
        return {};

    DistributedObjectStorageCandidate candidate;
    candidate.driver = driver_path.driver;
    candidate.driver_storage = driver_path.driver_storage;
    return candidate;
}

}
