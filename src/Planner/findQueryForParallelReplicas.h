#pragma once
#include <list>
#include <memory>

namespace DB
{

class QueryNode;
class TableNode;
class UnionNode;

class IQueryTreeNode;
using QueryTreeNodePtr = std::shared_ptr<IQueryTreeNode>;

struct SelectQueryOptions;

/// Find a query which can be executed with parallel replicas up to WithMergableStage.
/// Returned query will always contain some (>1) subqueries, possibly with joins.
const QueryNode * findQueryForParallelReplicas(const QueryTreeNodePtr & query_tree_node, const SelectQueryOptions & select_query_options);

/// Find a table from which we should read on follower replica. It's the left-most table within all JOINs and UNIONs.
const TableNode * findTableForParallelReplicas(const QueryTreeNodePtr & query_tree_node, const SelectQueryOptions & select_query_options);

class IStorage;
using StoragePtr = std::shared_ptr<IStorage>;
class Context;
using ContextPtr = std::shared_ptr<const Context>;

/// Check whether a resolved storage is eligible for parallel replicas (MergeTree, replication, no FINAL).
bool isTableNodeEligibleForParallelReplicas(const TableNode & table_node, const StoragePtr & storage, const ContextPtr & context);

/// Find a UNION node whose every child query reads from a table eligible for parallel replicas.
/// Used for views with UNION ALL where each branch reads from a separate MergeTree table.
const UnionNode * findTableUnionForParallelReplicas(const QueryTreeNodePtr & query_tree_node, const SelectQueryOptions & select_query_options);

/// EXPERIMENTAL, DIAGNOSTIC ONLY (see object_storage_cluster_bypass_join_wrap): logs (LOG_WARNING,
/// "ParallelReplicasClusterDiag" / "PR_CLUSTER_DIAG") whether the query tree, with eligibility relaxed from
/// MergeTree to any IStorageCluster-derived storage, would be recognized by the same whole-query/leftmost-table
/// candidate traversal used for MergeTree parallel replicas -- including seeing through CTEs/subqueries. Does
/// not affect query execution; no-op unless the experimental setting is enabled. `select_query_options` is only
/// used for the diagnostic's `depth`/`is_subquery` log fields, to distinguish the outermost query's Planner
/// invocation from the recursive per-subquery Planner invocations that also run through this constructor.
void logObjectStorageClusterParallelReplicasCandidate(
    const QueryTreeNodePtr & query_tree_node, const ContextPtr & context, const SelectQueryOptions & select_query_options);

struct JoinTreeQueryPlan;

class PlannerContext;
using PlannerContextPtr = std::shared_ptr<PlannerContext>;

struct StorageLimits;
using StorageLimitsList = std::list<StorageLimits>;

/// Execute QueryNode with parallel replicas up to WithMergableStage and return a plan.
/// This method does not check that QueryNode is valid. Ideally it should be a result of findParallelReplicasQuery.
JoinTreeQueryPlan buildQueryPlanForParallelReplicas(
    const QueryNode & query_node,
    const PlannerContextPtr & planner_context,
    std::shared_ptr<const StorageLimitsList> storage_limits);

/// EXPERIMENTAL PROTOTYPE (see object_storage_cluster_bypass_join_wrap): returns the IStorageCluster driver
/// TableNode (e.g. Iceberg StorageObjectStorageCluster) for query_tree's whole-query distributed dispatch, as
/// an exact QueryTreeNodePtr suitable for IQueryTreeNode::cloneAndReplace(), or nullptr if query_tree isn't
/// eligible (setting off, no such candidate, or the driver is already handled by PlannerJoinTree.cpp's existing
/// leftmost-table bypass). See §3.4 in ICEBERG_JOIN_EXPERIMENT.md.
QueryTreeNodePtr findObjectStorageClusterWholeQueryDriver(const QueryTreeNodePtr & query_tree, const ContextPtr & context);

struct SelectQueryInfo;

/// EXPERIMENTAL PROTOTYPE (see object_storage_cluster_bypass_join_wrap): sibling of
/// buildQueryPlanForParallelReplicas() for an IStorageCluster driver found by
/// findObjectStorageClusterWholeQueryDriver() -- dispatches the whole (CTE-inlined) query straight to the
/// driver's own IStorageCluster::readPreparedClusterQuery()/ReadFromCluster path instead of MergeTree's
/// ClusterProxy::executeQueryWithParallelReplicas. select_query_info is the *real* enclosing SelectQueryInfo to
/// copy (storage limits, etc.), analogous to StorageDistributed::read()'s
/// `SelectQueryInfo modified_query_info = select_query_info;`. See §3.4-3.6 in ICEBERG_JOIN_EXPERIMENT.md.
JoinTreeQueryPlan buildQueryPlanForObjectStorageCluster(
    const QueryTreeNodePtr & query_tree,
    const QueryTreeNodePtr & driver_table_expression,
    const SelectQueryInfo & select_query_info,
    const PlannerContextPtr & planner_context);

}
