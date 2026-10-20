#pragma once

#include <memory>
#include <optional>

namespace DB
{

class TableNode;
class QueryNode;

class IQueryTreeNode;
using QueryTreeNodePtr = std::shared_ptr<IQueryTreeNode>;

class Context;
using ContextPtr = std::shared_ptr<const Context>;

struct SelectQueryOptions;

/// A whole-query dispatch to the driver's cluster for `object_storage_cluster_join_mode='distributed'`.
struct DistributedObjectStorageCandidate
{
    /// Only the `Planner` for this query may dispatch. Nested planners share the candidate.
    const QueryNode * dispatch_boundary = nullptr;

    /// The table whose files are partitioned across workers. Also the key for its listing filter in
    /// `GlobalPlannerContext::filters_for_table_expressions`.
    std::shared_ptr<TableNode> driver;
};

/// Finds a driver through the left side of `INNER ALL`/`LEFT JOIN`s and subqueries that preserve partitions.
/// All tables must be readable catalog tables without row policies; workers read every partner in full.
/// Returns `std::nullopt` if the whole query is ineligible. Does not search for a narrower dispatch boundary.
std::optional<DistributedObjectStorageCandidate> findDistributedObjectStorageCandidate(
    const QueryTreeNodePtr & query_node, const ContextPtr & context);

/// Applies the planner guards before selecting a candidate for `GlobalPlannerContext`.
/// Only an initial, outermost `SELECT` may dispatch; analysis and worker queries use ordinary planning.
std::optional<DistributedObjectStorageCandidate> findDistributedObjectStorageCandidate(
    const QueryTreeNodePtr & query_tree_node, const SelectQueryOptions & select_query_options);

}
