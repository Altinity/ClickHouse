#pragma once

#include <Planner/findDistributedObjectStorageCandidate.h>
#include <Planner/PlannerJoinTree.h>

namespace DB
{

class PlannerContext;
using PlannerContextPtr = std::shared_ptr<PlannerContext>;
struct SelectQueryInfo;

/// Builds the whole-query dispatch plan for `candidate`: serializes the query unchanged, announces which table
/// drives it, and reads the result back through a single ReadFromCluster step at WithMergeableState, so the
/// caller's normal finalization (MergingAggregated and the rest) applies on top.
///
/// No table expression is rewritten. The driving table is named to the workers by database and table, through
/// `object_storage_distributed_driver_database`/`_table`, and a worker reads that one table from the initiator's
/// file-task queue while reading every other table in full, locally.
///
/// Returns nullopt when the driver cannot be named unambiguously in the serialized query -- it must appear
/// exactly once. The caller then falls back to ordinary planning.
///
/// Structurally the same as buildQueryPlanForParallelReplicas in Planner/findParallelReplicasQuery.cpp,
/// including the position-based conversion back to the query's header.
std::optional<JoinTreeQueryPlan> buildDistributedObjectStorageQueryPlan(
    const QueryTreeNodePtr & dispatch_boundary_node,
    const DistributedObjectStorageCandidate & candidate,
    const SelectQueryInfo & select_query_info,
    const PlannerContextPtr & planner_context);

}
