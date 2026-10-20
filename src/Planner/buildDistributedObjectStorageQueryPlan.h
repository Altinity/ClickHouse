#pragma once

#include <Planner/findDistributedObjectStorageCandidate.h>
#include <Planner/PlannerJoinTree.h>

namespace DB
{

class PlannerContext;
using PlannerContextPtr = std::shared_ptr<PlannerContext>;
struct SelectQueryInfo;

/// Builds a `ReadFromClusterQuery` at `WithMergeableState`; the caller applies normal query finalization.
///
/// The driver uses its `*Cluster` table function to consume file tasks from the initiator.
/// Partners use plain table functions and are read in full on every worker.
///
/// Returns `std::nullopt` when the serialized query does not contain exactly one driver reference.
std::optional<JoinTreeQueryPlan> buildDistributedObjectStorageQueryPlan(
    const QueryTreeNodePtr & dispatch_boundary_node,
    const DistributedObjectStorageCandidate & candidate,
    const SelectQueryInfo & select_query_info,
    const PlannerContextPtr & planner_context);

}
