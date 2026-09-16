#pragma once

#include <Planner/findDistributedObjectStorageCandidate.h>
#include <Planner/PlannerJoinTree.h>

namespace DB
{

class PlannerContext;
using PlannerContextPtr = std::shared_ptr<PlannerContext>;
struct SelectQueryInfo;

/// Builds the whole-query dispatch plan for `candidate`: replaces `candidate.driver` in `dispatch_boundary_node`
/// (the exact QueryTreeNodePtr findDistributedObjectStorageCandidate() was called with) with a resolved,
/// explicit `*Cluster()` TableFunctionNode via IQueryTreeNode::cloneAndReplace() -- mirroring
/// StorageDistributed::buildQueryTreeDistributed()'s own exact-node replacement pattern -- serializes the
/// result, and executes it via IStorageCluster::readPreparedClusterQuery(): a single ReadFromCluster step at
/// WithMergeableState, so the caller's normal finalization applies unmodified on top (see
/// Planner::buildPlanForQueryNode()). A generic, position-based ActionsDAG::makeConvertingActions() converts
/// the remote result back to the header the unmodified `dispatch_boundary_node` would have produced, matching
/// buildQueryPlanForParallelReplicas()'s own original-vs-worker header handling.
JoinTreeQueryPlan buildDistributedObjectStorageQueryPlan(
    const QueryTreeNodePtr & dispatch_boundary_node,
    const DistributedObjectStorageCandidate & candidate,
    const SelectQueryInfo & select_query_info,
    const PlannerContextPtr & planner_context);

}
