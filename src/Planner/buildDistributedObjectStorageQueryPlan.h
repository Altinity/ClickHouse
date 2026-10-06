#pragma once

#include <Planner/findDistributedObjectStorageCandidate.h>
#include <Planner/PlannerJoinTree.h>

namespace DB
{

class PlannerContext;
using PlannerContextPtr = std::shared_ptr<PlannerContext>;
struct SelectQueryInfo;

/// Builds the whole-query dispatch plan for `candidate`: serializes the query and reads the result back through a
/// single ReadFromClusterQuery step at WithMergeableState, so the caller's normal finalization (MergingAggregated
/// and the rest) applies on top.
///
/// Every table in the serialized query is replaced with the table function its own cluster read would send: the
/// driver with its `*Cluster` function, which on a worker reads the files this node hands out, and every other
/// table with its plain function, which a worker reads in full, locally.
///
/// Returns nullopt when the driver appears more than once in the serialized query. The caller then falls back to
/// ordinary planning.
///
/// Structurally the same as buildQueryPlanForParallelReplicas in Planner/findParallelReplicasQuery.cpp.
std::optional<JoinTreeQueryPlan> buildDistributedObjectStorageQueryPlan(
    const QueryTreeNodePtr & dispatch_boundary_node,
    const DistributedObjectStorageCandidate & candidate,
    const SelectQueryInfo & select_query_info,
    const PlannerContextPtr & planner_context);

}
