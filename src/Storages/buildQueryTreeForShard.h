#pragma once

#include <memory>
#include <optional>

#include <Interpreters/ActionsDAG.h>

namespace DB
{

struct SelectQueryInfo;

class IQueryTreeNode;
using QueryTreeNodePtr = std::shared_ptr<IQueryTreeNode>;

class PlannerContext;
using PlannerContextPtr = std::shared_ptr<PlannerContext>;

class Context;
using ContextPtr = std::shared_ptr<const Context>;

class Block;

QueryTreeNodePtr buildQueryTreeForShard(
    const PlannerContextPtr & planner_context,
    QueryTreeNodePtr query_tree_to_modify,
    bool allow_global_join_for_right_table,
    bool find_cross_join = false);

/** Replace every `ALIAS` column node with its defining expression, so the expression is evaluated on the shard that reads
  * the real table instead of the column being resolved there as if it were physical.
  *
  * Apply this to any query tree that is about to be shipped, before `buildQueryTreeForShard`: that function rebuilds a
  * shipped table expression from column names and types only, which drops an `ALIAS` column's resolved expression and
  * leaves the remote side asking storage for a column it does not have.
  *
  * When `enable_alias_marker` is on, each inlined expression is also wrapped in `__aliasMarker(expr, '<id>')`. The
  * marker preserves the identity of the logical column the expression was expanded from, so the initiator -- which
  * still sees the un-inlined column -- can match the shard header by name instead of by position.
  *
  * The column's logical name is kept as an alias on top-level projection items and on `JOIN USING` key sides only.
  * Aliasing an occurrence inside `WHERE` / `GROUP BY` / `ORDER BY` / `HAVING` / `JOIN ON` makes two same-named `ALIAS`
  * columns from different sources collide in one scope, and the shard then throws `MULTIPLE_EXPRESSIONS_FOR_ALIAS`.
  */
void inlineAliasColumns(QueryTreeNodePtr & query_tree_to_modify, const ContextPtr & context);

void rewriteJoinToGlobalJoin(QueryTreeNodePtr query_tree_to_modify, ContextPtr context, bool force_prefer_global_join = false);
void rewriteInToGlobalIn(QueryTreeNodePtr & query_tree_to_modify, ContextPtr context, bool rewrite_for_distributed = false);

/** When a Distributed/parallel-replicas query is executed up to `WithMergeableState`, the shard's query tree has its
  * `ALIAS` columns inlined into their defining expressions. If several projection (or sort/group/...) items expand to the
  * same expression, the shard's `ActionsDAG` deduplicates them into a single output column, so the shard header can have
  * fewer columns than the header the initiator expects (which keeps the columns distinct, since on the initiator the
  * `ALIAS` columns are not inlined).
  *
  * This helper reconstructs the initiator's `expected_header` from the shard's `shard_header` by fanning out each
  * collapsed shard column back to every expected column that maps onto it. The mapping is computed exactly: for every
  * expression node in `query_tree` we compute both its identifier-based action name (matching `expected_header`) and its
  * action name after inlining `ALIAS` columns (matching `shard_header`), and use this translation to resolve which shard
  * column feeds each expected column.
  *
  * Returns the conversion `ActionsDAG` (input = `shard_header`, output = `expected_header` names), or `std::nullopt` when
  * the situation is not a recognized projection collapse, in which case the caller should fall back to its default
  * reconciliation.
  */
std::optional<ActionsDAG> buildShardCollapseFanOut(
    const QueryTreeNodePtr & query_tree,
    const PlannerContextPtr & planner_context,
    const Block & shard_header,
    const Block & expected_header);

}
