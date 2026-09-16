#pragma once
#include <memory>
#include <optional>
#include <base/types.h>

namespace DB
{

class TableNode;
class IStorageCluster;

class IQueryTreeNode;
using QueryTreeNodePtr = std::shared_ptr<IQueryTreeNode>;

class Context;
using ContextPtr = std::shared_ptr<const Context>;

/// A whole-query JOIN-pushdown candidate for object_storage_cluster_join_mode='distributed'. `query_node`
/// (the exact QueryTreeNodePtr passed to findDistributedObjectStorageCandidate()) is always the dispatch
/// boundary: the entire query is forwarded as a whole to `driver`'s cluster, never a narrower subquery.
struct DistributedObjectStorageCandidate
{
    /// The driving TableNode, reachable from the dispatch boundary only via the left path of every
    /// JOIN/subquery crossing (see findDriverOnLeftSpine() in the .cpp).
    const TableNode * driver = nullptr;

    /// driver's resolved storage.
    IStorageCluster * driver_storage = nullptr;
};

/// Whole-query dispatch is an all-or-nothing decision for `query_node` itself: either the entire subtree
/// reachable from it is safe to forward as one query to a single DataLake-catalog driver's cluster, or it
/// isn't and the caller falls back to ordinary planning for this exact QueryNode -- there is no narrower
/// fallback candidate search. Returns nullopt when: mode isn't 'distributed', `query_node` has no JOIN at
/// all (nothing here for this mode to optimize), no eligible driver is reachable via the left spine, or any
/// unsafe leaf is reachable anywhere in the subtree (explicit `*Cluster()`, local/Distributed table,
/// row policy, missing SELECT access, or a structurally unsupported shape).
///
/// The driver is found by walking strictly down the left spine of `query_node`'s own JOIN/subquery tree:
/// QueryNode -> its join tree; supported JoinNode (INNER ALL or LEFT) -> left operand only; intermediate
/// QueryNode crossed along the way -> only if partition-preserving (see isSafeIntermediateSubquery() in the
/// .cpp); TableNode -> an eligible DataLake-catalog driver with a non-empty cluster. The right side of any
/// JOIN, and anything below it, is never inspected for a competing driver -- it is validated only as
/// worker-local, safe-to-recompute-in-full content (see allWorkerLocalReferencesAreSafe() in the .cpp), which
/// is why a DataLake-catalog table, or even a nested JOIN/GROUP BY over several such tables, is accepted on
/// the right without ever being considered for the driver role itself.
std::optional<DistributedObjectStorageCandidate> findDistributedObjectStorageCandidate(
    const QueryTreeNodePtr & query_node, const ContextPtr & context);

}
