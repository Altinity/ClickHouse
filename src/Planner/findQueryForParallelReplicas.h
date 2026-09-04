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

/// Initiator-safe driver lookup for `candidate` -- the exact QueryNode findQueryForParallelReplicas() selected
/// (GlobalPlannerContext::parallel_replicas_node), NOT the original root query tree. Unlike the public
/// findTableForParallelReplicas() overload above (whose follower-only gate makes it return nullptr
/// unconditionally on a normal initiator -- that overload exists for PlannerJoinTree.cpp's View/
/// MaterializedView follower-recursion-safety check, GlobalPlannerContext::parallel_replicas_table, a
/// different and unrelated purpose), this always finds the driver associated with an already-selected
/// candidate, on the initiator, so Planner::buildPlanForQueryNode() can pick the correct execution backend
/// (MergeTree vs. object-storage-cluster) for it. Returns nullptr if `candidate` is null.
const TableNode * findParallelReplicasCandidateDriver(const QueryNode * candidate);

class IStorage;
using StoragePtr = std::shared_ptr<IStorage>;
class Context;
using ContextPtr = std::shared_ptr<const Context>;

/// Check whether a resolved storage is eligible for parallel replicas (MergeTree, replication, no FINAL;
/// or, under object_storage_cluster_bypass_join_wrap, a StorageObjectStorageCluster driver -- see
/// isObjectStorageClusterDriverEligible() below, which this delegates to first).
bool isTableNodeEligibleForParallelReplicas(const TableNode & table_node, const StoragePtr & storage, const ContextPtr & context);

/// Storage-eligibility policy for the object-storage-cluster whole-query dispatch (see
/// buildQueryPlanForObjectStorageCluster() below): true iff `storage` is a StorageObjectStorageCluster and the
/// query's settings opt into cluster-wide JOIN pushdown (parallel_replicas_for_cluster_engines,
/// cluster_for_parallel_replicas, object_storage_cluster_bypass_join_wrap). Shared between the two places that
/// pick an object-storage-cluster driver: isTableNodeEligibleForParallelReplicas() above (feeds
/// findQueryForParallelReplicas()/findTableForParallelReplicas() for a driver nested under a CTE/subquery), and
/// PlannerJoinTree.cpp's allowParallelReplicasForJoinTree() (for a driver that is the immediate leftmost/
/// rightmost table of a JOIN). Both mechanisms end up calling the same buildQueryPlanForObjectStorageCluster()
/// execution backend once they've picked a driver.
bool isObjectStorageClusterDriverEligible(const IStorage & storage, const ContextPtr & context);

/// Find a UNION node whose every child query reads from a table eligible for parallel replicas.
/// Used for views with UNION ALL where each branch reads from a separate MergeTree table.
const UnionNode * findTableUnionForParallelReplicas(const QueryTreeNodePtr & query_tree_node, const SelectQueryOptions & select_query_options);

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

struct SelectQueryInfo;

/// Execution backend for an object-storage-cluster (e.g. Iceberg StorageObjectStorageCluster) driver, shared by
/// both candidate-selection mechanisms:
///   - a driver reachable through findQueryForParallelReplicas()/findTableForParallelReplicas() (nested under a
///     CTE/subquery, e.g. IcebergBench q21), dispatched from Planner::buildPlanForQueryNode();
///   - a driver that is the immediate leftmost/rightmost table of a JOIN (e.g. IcebergBench q17), recognized by
///     PlannerJoinTree.cpp's allowParallelReplicasForJoinTree() and dispatched from buildJoinTreeQueryPlan().
/// Does NOT go through ClusterProxy::executeQueryWithParallelReplicas (MergeTree/task-based-coordinator
/// machinery); instead it replaces `driver_table_node` in `query_tree` with the driver's own *Cluster
/// table-function form (StorageObjectStorageCluster::buildClusterTableFunctionAST(), with the cluster name
/// embedded as a literal argument -- never as a propagated Context setting) via exact-node
/// IQueryTreeNode::cloneAndReplace(), mirroring StorageDistributed::buildQueryTreeDistributed(), then hands the
/// whole (CTE-inlined) query to the driver's own IStorageCluster::readPreparedClusterQuery()/ReadFromCluster.
/// select_query_info is the *real* enclosing SelectQueryInfo to copy (storage limits, etc.), analogous to
/// StorageDistributed::read()'s `SelectQueryInfo modified_query_info = select_query_info;`.
JoinTreeQueryPlan buildQueryPlanForObjectStorageCluster(
    const QueryTreeNodePtr & query_tree,
    const TableNode & driver_table_node,
    const SelectQueryInfo & select_query_info,
    const PlannerContextPtr & planner_context);

}
