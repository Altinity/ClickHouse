#pragma once

#include <Planner/findDistributedObjectStorageCandidate.h>
#include <Planner/PlannerJoinTree.h>

namespace DB
{

class PlannerContext;
using PlannerContextPtr = std::shared_ptr<PlannerContext>;
struct SelectQueryInfo;

/// Builds the whole-query dispatch plan for `candidate`: replaces the driver with an explicit, resolved
/// `*Cluster()` table function, serializes the result, and reads it back through a single ReadFromCluster step
/// at WithMergeableState, so the caller's normal finalization (MergingAggregated and the rest) applies on top.
///
/// Structurally the same as buildQueryPlanForParallelReplicas in Planner/findParallelReplicasQuery.cpp,
/// including the position-based conversion back to the original query's header.
JoinTreeQueryPlan buildDistributedObjectStorageQueryPlan(
    const QueryTreeNodePtr & dispatch_boundary_node,
    const DistributedObjectStorageCandidate & candidate,
    const SelectQueryInfo & select_query_info,
    const PlannerContextPtr & planner_context);

}
