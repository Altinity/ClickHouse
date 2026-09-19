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

/// A whole-query JOIN-pushdown candidate for `object_storage_cluster_join_mode='distributed'`. The entire query
/// passed to findDistributedObjectStorageCandidate is dispatched to `driver`'s cluster as one unit.
struct DistributedObjectStorageCandidate
{
    /// The driving table, reachable from the dispatch boundary only via the left path of every JOIN/subquery
    /// crossing.
    const TableNode * driver = nullptr;

    IStorageCluster * driver_storage = nullptr;
};

/// Decides whether `query_node` as a whole can be executed on a single DataLake-catalog driver's cluster.
///
/// The driver is found by walking strictly down the left spine: QueryNode -> its join tree; INNER ALL or LEFT
/// JOIN -> left operand only; an intermediate QueryNode -> only if partition-preserving; TableNode -> eligible if
/// it resolves through a DataLake catalog and has a non-empty cluster. Everything else reachable from
/// `query_node` is then checked as worker-local content that each worker recomputes in full. That check covers
/// table references only -- other functions the query calls (`dictGet`, a UDF, `hostName`) simply move to the
/// workers and are assumed to be consistent there, as they are for `Distributed`.
///
/// All-or-nothing for `query_node` itself -- there is no narrower fallback candidate. Returns nullopt when the
/// mode is not 'distributed', there is no JOIN, no eligible driver is reachable, or any unsafe leaf appears
/// anywhere in the subtree (explicit `*Cluster()` table function, local/`Distributed` table, row policy, missing
/// SELECT access, unsupported shape). The caller then falls back to ordinary planning.
std::optional<DistributedObjectStorageCandidate> findDistributedObjectStorageCandidate(
    const QueryTreeNodePtr & query_node, const ContextPtr & context);

}
