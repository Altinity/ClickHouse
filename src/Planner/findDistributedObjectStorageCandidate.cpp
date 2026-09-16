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

/// A row-level security filter is normally attached to a table's own SelectQueryInfo during per-table
/// planning (PlannerJoinTree.cpp), keyed by that table's own catalog identity; every table admitted here,
/// driver or partner, is either rewritten away (the driver, into its explicit `*Cluster()` form) or
/// independently re-resolved by each worker's own DatabaseDataLake lookup (a partner) -- neither preserves or
/// safely re-derives the initiator user's own effective policy. Conservative: a nontrivial policy on any table
/// in the candidate subtree blocks this whole-query dispatch outright (see the setting's own documentation).
bool hasEffectiveRowPolicy(const TableNode & table_node, const ContextPtr & context)
{
    const auto & storage_id = table_node.getStorageID();
    if (!storage_id.hasDatabase())
        return false;

    auto row_policy_filter = context->getRowPolicyFilter(storage_id.getDatabaseName(), storage_id.getTableName(), RowPolicyFilterType::SELECT_FILTER);
    return row_policy_filter && !row_policy_filter->isAlwaysTrue();
}

/// Stock per-table planning (prepareBuildQueryPlanForTableExpression() in PlannerJoinTree.cpp) checks SELECT
/// access on every TableNode it plans, including ones buried in a subquery reached only under only_analyze via
/// its own separate check_subquery_table_access path. This whole-query dispatch replaces that per-table walk
/// entirely, so every table it admits -- driver or partner, at any depth -- needs the same check performed
/// here instead. A conservative, non-throwing, table-level (not column-level) check: missing access simply
/// falls back to ordinary planning, which enforces the real, precise access rules with its own error.
bool hasSelectAccess(const TableNode & table_node, const ContextPtr & context)
{
    const auto & storage_id = table_node.getStorageID();
    if (!storage_id.hasDatabase())
        return false;

    return context->getAccess()->isGranted(AccessType::SELECT, storage_id.getDatabaseName(), storage_id.getTableName());
}

/// Whether `table_node` is trustworthy to include in the dispatch at all, as either the driver or a JOIN
/// partner / WHERE-HAVING-projection reference: a DataLake-catalog table (so every worker can independently
/// and identically re-resolve it -- see the setting's own documentation for what is and isn't verified here),
/// with no row policy that dispatch would silently drop, and visible to the current user.
bool isSafeDataLakeLeaf(const TableNode & table_node, const ContextPtr & context)
{
    auto * storage_cluster = dynamic_cast<IStorageCluster *>(table_node.getStorage().get());
    if (!storage_cluster || !storage_cluster->isResolvedViaDataLakeCatalog())
        return false;

    if (hasEffectiveRowPolicy(table_node, context))
        return false;

    return hasSelectAccess(table_node, context);
}

bool isEligibleDriver(const TableNode & table_node, const ContextPtr & context, IStorageCluster *& out_storage)
{
    if (!isSafeDataLakeLeaf(table_node, context))
        return false;

    auto * storage_cluster = dynamic_cast<IStorageCluster *>(table_node.getStorage().get());
    if (storage_cluster->getClusterName(context).empty())
        return false;

    out_storage = storage_cluster;
    return true;
}

/// True if `node`'s own subtree (not crossing into a nested QueryNode/UnionNode) contains an aggregate or
/// window function -- catches e.g. `SELECT count() FROM driver`, which has no GROUP BY node at all.
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

/// The dispatch boundary itself may freely aggregate/order/limit -- WithMergeableState plus stock
/// finalization on top handles that correctly. But an *intermediate* QueryNode sitting between the dispatch
/// boundary and the driver (crossed via a nested subquery, strictly on the driver's own left path) gets
/// executed independently and completely on each worker's own partition of the driver; if it aggregates,
/// dedups, or limits, worker-local partial results get treated as final ones, which is wrong whenever a
/// group/row spans multiple workers' partitions. Conservative: reject any such construct here rather than try
/// to prove which ones happen to be partition-preserving. This never applies to a JOIN partner's own subquery
/// on the right of a JOIN -- that content is recomputed in full on every worker (see
/// allWorkerLocalReferencesAreSafe()), so GROUP BY/LIMIT/etc there is not a hazard at all.
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

    /// Whether a supported JoinNode was found anywhere on the path down to the driver -- a candidate with no
    /// JOIN at all has nothing for this mode to optimize, and dispatching it anyway would just replace stock
    /// IStorageCluster::read() with a narrower prepared path.
    bool has_join = false;
};

DriverPathResult unusableDriverPath()
{
    DriverPathResult result;
    result.unusable = true;
    return result;
}

/// Walks strictly down the left spine looking for exactly one scheduling driver. Never inspects the right
/// side of a JOIN for a competing driver -- the right side is validated separately, as worker-local content
/// (see allWorkerLocalReferencesAreSafe()), which is why an RHS DataLake-catalog table, or a nested JOIN/
/// GROUP BY over several such tables, never poisons or competes with the driver found here.
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

        /// `node` is crossed as an intermediate subquery here, not the dispatch boundary itself (that's the
        /// QueryNode originally passed to findDistributedObjectStorageCandidate()).
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

/// After a driver is found, every other table reachable anywhere in the whole dispatch-boundary subtree --
/// on the right of any JOIN, nested arbitrarily deep in a JOIN/GROUP BY of its own, or referenced from a
/// WHERE/HAVING/projection subquery -- must be safe to recompute in full, identically, on every worker: a
/// DataLake-catalog table with no row policy of its own and visible to the current user. This walk does not
/// classify anything as another driver and does not restrict GROUP BY/JOIN/LIMIT anywhere in this content --
/// unlike the driver's own left-spine path, it is never partitioned, so each worker simply recomputes it
/// whole (see the setting's own documentation and findDistributedObjectStorageCandidate.h).
bool allWorkerLocalReferencesAreSafe(const QueryTreeNodePtr & node, const TableNode * driver, const ContextPtr & context)
{
    if (!node)
        return true;

    if (const auto * table_node = node->as<TableNode>())
        return table_node == driver || isSafeDataLakeLeaf(*table_node, context);

    if (node->as<TableFunctionNode>())
        return false;

    for (const auto & child : node->getChildren())
        if (!allWorkerLocalReferencesAreSafe(child, driver, context))
            return false;

    return true;
}

}

std::optional<DistributedObjectStorageCandidate> findDistributedObjectStorageCandidate(
    const QueryTreeNodePtr & query_node, const ContextPtr & context)
{
    if (context->getSettingsRef()[Setting::object_storage_cluster_join_mode] != ObjectStorageClusterJoinMode::DISTRIBUTED)
        return {};

    /// readPreparedClusterQuery() goes straight to the driver's own cluster, bypassing the remote-initiator
    /// topology (convertToRemote()) that stock IStorageCluster::read() applies for this setting; falls back to
    /// ordinary planning instead of silently ignoring it.
    if (context->getSettingsRef()[Setting::object_storage_remote_initiator])
        return {};

    /// additional_table_filters keys are matched against the initiator's current_database and the query's own
    /// aliasing/naming, both of which shift once forwarded as fully serialized remote SQL -- Planner.cpp
    /// disables parallel replicas for the exact same reason (see the comment there). Rather than replicate
    /// case-by-case matching here, disable the combination entirely, same as that precedent.
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

    if (!allWorkerLocalReferencesAreSafe(query_node, driver_path.driver, context))
        return {};

    DistributedObjectStorageCandidate candidate;
    candidate.driver = driver_path.driver;
    candidate.driver_storage = driver_path.driver_storage;
    return candidate;
}

}
