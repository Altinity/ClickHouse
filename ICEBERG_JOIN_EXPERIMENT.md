# Iceberg JOIN + partial-aggregation pushdown experiment

Branch: `fix/antalya-26.6/query-plan-aggregation-perf` (all changes described here are
**uncommitted** working-tree modifications on top of `296cce3c390`)

Status as of last update: minimal patch validated end-to-end on q17 (correctness +
topology + ~37% hot-runtime improvement); a real regression in unrelated benchmark
queries was found and fixed with a narrower condition; that narrower fix is built,
pushed, and awaiting re-validation against the full benchmark suite.

---

## 1. Goal

Test whether ClickHouse's parallel-replicas execution over an Iceberg DataLake table
can push a `JOIN` (against a small dimension/lookup table from the same catalog) plus
partial `GROUP BY` down onto the parallel-replica workers, instead of pulling all
probe-side rows back to the initiator and joining/aggregating there.

Benchmark: IcebergBench q17 ("geo enrichment") —

```sql
SELECT
  dst_city,
  CASE
    WHEN (CASE WHEN g.lat IS NOT NULL AND g.lng IS NOT NULL THEN g.lat ELSE e.slc_latitude END) IS NOT NULL
     AND (CASE WHEN g.lat IS NOT NULL AND g.lng IS NOT NULL THEN g.lng ELSE e.slc_longitude END) IS NOT NULL
    THEN concat(...) ELSE NULL
  END AS src_geo_location,
  count(*) AS events
FROM ice.`billion-rows_t17175.event_page` e
LEFT JOIN ice.`billion-rows.geo_location_lookup` g
  ON e.src_city = g.city_ascii
 AND e.src_region = g.state_ascii
 AND (e.src_country = g.iso2 OR e.src_country = 'unknown')
WHERE e.ns_tenant_id = 17175
  AND e.timestamp >= '2025-02-05 00:00:00'
  AND e.timestamp < '2025-02-13 00:00:00'
  AND e.dst_country <> ''
GROUP BY dst_city, <same CASE expression>
ORDER BY events DESC, dst_city, src_geo_location
LIMIT 200
```

Fact table `event_page`: ~238,000,777 rows after filtering. Lookup table
`geo_location_lookup`: ~17 rows. Both tables are resolved through the same DataLake
catalog database (`ice`).

### Baseline numbers (before this experiment)

| Scenario | Hot runtime |
|---|---|
| Original q17 (JOIN executed on initiator) | ~6.97 s |
| Manual `clusterAllReplicas()` + disjoint timestamp ranges proof-of-concept (JOIN + partial agg forced onto 3 workers by hand) | ~4.7 s |
| StarRocks (reference) | ~2.15 s |

`JoinProbeTableRowCount` on the original query was ~238,000,777 — all on the
initiator. The manual proof validated that pushing JOIN + partial aggregation onto
the 3 workers is both correct (checksum-verified) and faster.

### Correctness oracle

```sql
SELECT
    count() AS rows,
    groupBitXor(sipHash64(dst_city, coalesce(src_geo_location, '__NULL__'), events)) AS xor_hash,
    sum(sipHash64(dst_city, coalesce(src_geo_location, '__NULL__'), events)) AS sum_hash
FROM ( <q17> );
```

Expected: `rows=200, xor_hash=16681162019044056089, sum_hash=11731770707781500759`.

---

## 2. Result: it works, ~4.4s hot

After the fixes below, running the **unmodified** q17 (just with extra `SETTINGS`)
through the normal parallel-replicas execution path:

- Checksum matches the oracle exactly.
- `EXPLAIN PLAN` on the initiator collapses to `MergingAggregated` directly over
  `ReadFromCluster` — the `Join`/`Aggregating` steps that used to sit on the
  initiator are gone, pushed onto the workers (matches the manual proof's shape).
- `EXPLAIN PIPELINE distributed=1` shows `ReadFromCluster` fanning out to 3 `Remote`
  branches, each independently unmarshalling merged/aggregated blocks, converging
  into one `MergingAggregatedTransform`.
- Per-node `system.query_log` `ProfileEvents['JoinProbeTableRowCount']`, gathered via
  `clusterAllReplicas('vig-test', system.query_log)`, confirmed across **5
  independent runs** that the initiator's own row always shows `join_probe_rows=0`
  (it no longer probes the JOIN itself), and the 3 workers' `join_probe_rows` sum to
  exactly `238,000,777` each time (e.g. one run: `65,217,067 + 69,905,138 +
  102,878,572 = 238,000,777`). `join_result_rows == join_probe_rows` on every worker
  (correct `LEFT JOIN`, no row loss).
- Hot runtime: **~4.4 s** — down from the `~6.97 s` baseline (~37% faster), and
  slightly faster than the manual `~4.7 s` proof-of-concept, consistent with the
  hypothesis that native task-based file scheduling balances work better than a
  hand-picked timestamp split.

---

## 3. What actually blocked it (three separate issues, not one)

The task's original hypothesis was a single guard in `PlannerJoinTree.cpp`. In
practice there were three independent blockers found by iterating against a live
3-node cluster (`vig-test`, via the ClickHouse Kubernetes Operator), each diagnosed
from a real error/plan captured on the cluster, not from static reading alone.

### 3.1 `PlannerJoinTree.cpp`: `should_wrap_left_table` (original hypothesis, confirmed real but not sufficient alone)

`buildJoinTreeQueryPlan()` wraps the leftmost table expression in a subquery
whenever the query has multiple tables and the leftmost storage is `IStorageCluster`
-derived. This is a safety guard: for a generic `s3Cluster()`/`hdfsCluster()`
source, the JOIN's other tables might not exist on the remote nodes, so the full
query must not be forwarded. For a DataLake-catalog-backed table, both sides *are*
resolvable on every cluster node, so the guard is unnecessarily conservative here.

Fix: gate the wrap on a new opt-in setting, `object_storage_cluster_bypass_join_wrap`
(default `false`).

This alone did **not** change the plan — see 3.2.

### 3.2 `Analyzer/Resolve/QueryAnalyzer.cpp`: the actual root cause

`TableFunctionsWithClusterAlternativesVisitor::shouldReplaceWithClusterAlternatives()`:

```cpp
bool shouldReplaceWithClusterAlternatives() const
{
    return subquery_count <= 1 && !has_join && ((table_count + table_function_count) == 1 || (table_function_count == 0));
}
```

`!has_join` unconditionally excludes **any** query containing a JOIN. And in
`QueryAnalyzer::resolveQuery()`:

```cpp
if (!table_function_visitor.shouldReplaceWithClusterAlternatives())
    query_node_typed.getMutableContext()->setSetting("parallel_replicas_for_cluster_engines", false);
```

This **forcibly resets** `parallel_replicas_for_cluster_engines` to `false` for the
whole query at analysis time, whenever a JOIN is present — regardless of what the
query's own `SETTINGS` clause requested. This happens upstream of everything in
`PlannerJoinTree.cpp` / `IStorageCluster.cpp` / `DatabaseDataLake.cpp`, which is why
setting `parallel_replicas_for_cluster_engines=1` explicitly in `SETTINGS` had no
effect, and why a standalone `SELECT getSetting('parallel_replicas_for_cluster_engines')`
returned `true` while `DatabaseDataLake::tryGetTableImpl()`'s own diagnostic log
showed `parallel_cluster_engines=false` for the very same query — two different
code paths, only one of which sees the analyzer's override.

Found via a diagnostic `LOG_WARNING` (`DLPR` tag) added to
`DatabaseDataLake::tryGetTableImpl()`, tracing
`cluster_for_pr` / `parallel_cluster_engines` / `can_task_pr` / `is_distributed` /
`query_kind` / `can_use_pr` / `final_cluster` for every DataLake table resolution,
and a second one (`OSC_STAGE` tag) in
`StorageObjectStorageCluster::getQueryProcessingStage()`.

**First fix attempt** (too broad): bypass the *entire* `shouldReplaceWithClusterAlternatives()`
result when the experimental setting is on. This worked for q17 but **regressed
unrelated benchmark queries** with CTEs (`q2`, `q4`, `q13`, `q16` — nested
`appinfo_d` CTE; `q21` — multiple dependent CTEs including `policy_matches`), all
failing with `Unknown table expression identifier 'appinfo_d' in scope`
(`code: 60`) once the bypass setting was enabled cluster-wide for a full benchmark
run. The `subquery_count`/`table_count`/`table_function_count` restrictions in the
original check are protecting something real (very likely: `parallel_replicas_for_cluster_engines`
also gates a "silently rewrite this table/table-function reference to its Cluster
equivalent" transformation elsewhere in the analyzer — see the comment in
`StorageURLCluster.cpp` — and leaving that rewrite enabled for a query with CTEs can
desynchronize how a CTE alias gets bound across its multiple references).

**Narrow fix** (pushed as `arm64-v6`, superseded -- see below): only lift the
`has_join` prohibition, keep every other restriction:

```cpp
// TableFunctionsWithClusterAlternativesVisitor.h
bool shouldReplaceWithClusterAlternatives() const
{
    return !has_join && shouldReplaceWithClusterAlternativesIgnoringJoin();
}

bool shouldReplaceWithClusterAlternativesIgnoringJoin() const
{
    return subquery_count <= 1 && ((table_count + table_function_count) == 1 || (table_function_count == 0));
}
```

```cpp
// QueryAnalyzer.cpp
const bool experimental_allow_join = query_node_typed.getMutableContext()->getSettingsRef()[Setting::object_storage_cluster_bypass_join_wrap]
    && table_function_visitor.shouldReplaceWithClusterAlternativesIgnoringJoin();

if (!table_function_visitor.shouldReplaceWithClusterAlternatives() && !experimental_allow_join)
    query_node_typed.getMutableContext()->setSetting("parallel_replicas_for_cluster_engines", false);
```

For q17: `subqueries=0, tables=2, table_functions=0, has_join=true` →
`shouldReplaceWithClusterAlternativesIgnoringJoin()=true` → bypass applies.

For q2/q4/q13/q16/q21 (CTEs): `shouldReplaceWithClusterAlternativesIgnoringJoin()`
should evaluate to `false` (subquery/table-count restrictions still apply) → bypass
does **not** apply → `parallel_replicas_for_cluster_engines` still gets forced off
exactly as before this experiment → back to their old, working (non-regressed)
behavior.

Added a third diagnostic (`CLUSTER_ALT` tag) logging
`tables`/`table_functions`/`subqueries`/`has_join`/`normal`/`ignoring_join`/`experimental_allow_join`
for every query, to directly confirm the split lands where predicted.

**Re-validation result (superseded the "narrow" framing above):** the `arm64-v6`
image still failed q2 with the exact same `Unknown table expression identifier
'appinfo_d'` error. `shouldReplaceWithClusterAlternativesIgnoringJoin()` does
**not** exclude q2 the way it was assumed to:

```cpp
bool shouldReplaceWithClusterAlternativesIgnoringJoin() const
{
    return subquery_count <= 1 && ((table_count + table_function_count) == 1 || (table_function_count == 0));
}
```

For q2, `table_function_count == 0` (`ice.appev`/`ice.appinfo` are `TableNode`s,
not table functions), which makes the right-hand side of the `||` unconditionally
`true` regardless of `table_count` — and q2's resolved tree apparently also has
`subquery_count <= 1`. So `ignoring_join` evaluates `true` for q2 just like it
does for q17, `experimental_allow_join` is `true`, and
`parallel_replicas_for_cluster_engines` is **not** forced off. The
`subquery_count`/`table_count` restrictions in the original check were never
actually gating "does this query have a CTE" for the DataLake-table case — they
were incidentally protecting other shapes (generic `s3Cluster()`/table-function
queries), not this one.

**Conclusion, and change of plan:** stop trying to keep q2 on its old
(non-distributed) path via this check — the check can't distinguish q2/q4/q13/q16
from q17 the way originally assumed, and even if it could, we *want* q2 to be
distributed too (it's a JOIN over the same catalog, same as q17). Fix the actual
failure instead: `appinfo_d` is a CTE, and `query_info.query` (built by
`queryNodeToSelectQuery(query_tree)` with the default
`set_subquery_cte_name=true`) serializes a CTE reference as its bare name, which
only resolves at the top-level query — not on a remote node. ClickHouse already
has `queryNodeToDistributedSelectQuery()` (`src/Planner/Utils.cpp`) for exactly
this: it clones the query tree, strips CTE names so subqueries are always
serialized by body, and is already used by
`IStorageCluster::updateQueryWithJoinToSendIfNeeded()` for the `LOCAL`/`GLOBAL`
join modes. It was just never applied on the `ALLOW`-mode path (the default),
which is why `IStorageCluster::read()` was sending the raw
`queryNodeToSelectQuery()` output with the dangling CTE name.

**Fix (pushed as `arm64-v7`)**, in `src/Storages/IStorageCluster.cpp`,
immediately after `query_to_send` is initialized in `read()`:

```cpp
ASTPtr query_to_send = query_info.query;

if (settings[Setting::object_storage_cluster_bypass_join_wrap] && query_info.query_tree)
    query_to_send = queryNodeToDistributedSelectQuery(query_info.query_tree);
```

Gated on the same experimental setting so default (flag-off) behavior is
unchanged. Compiled clean (no warnings) against `build/` before cross-compiling.
**Not yet re-validated against the live cluster** — q2/q4/q13/q16/q21 need to be
re-run against `arm64-v7` to confirm the CTE body now gets inlined and the
queries succeed, and q17 needs to be re-confirmed still correct/fast on this
image.

### 3.3 `PlannerJoinTree.cpp`: RHS (non-leftmost) table wrap — reverted, was a wrong turn

Once 3.1+3.2 got the JOIN reaching a worker, it failed there with
`NOT_FOUND_COLUMN_IN_BLOCK` (`Not found column __table2.lat in block __table1.lat,
__table1.lng, __table1.city_ascii, ...`). Root-caused (with a correction from
review, see below) to the wrap-subquery mechanism for the non-leftmost (RHS) table:

```cpp
bool is_remote = planner_context->getTableExpressionDataOrThrow(table_expression).isRemote();
```

`g`'s storage `isRemote()` is unconditionally `true` for any `IStorageCluster`
(`IStorageCluster.h:56` — `bool isRemote() const final { return true; }`),
independent of whether a cluster name actually resolves — so `g` gets wrapped in a
subquery, planned under its own fresh `GlobalPlannerContext`
(`buildQueryPlanForTableExpression`, `wrap_read_columns_in_subquery` branch).

**First attempted fix** (wrong): bypass this wrap too, same experimental flag. This
produced correctly-labeled `__table2.lat`/`__table2.lng` (fixing the original
mislabeling), but a **new** failure appeared: `Columns [__table2.iso2] are not found
in blocks [...]` — the JOIN-key-only columns (`city_ascii`, `state_ascii`, `iso2`,
needed only for the `ON` condition, not the final output) vanished entirely once
the wrap was skipped. **Conclusion: the wrap was doing real, necessary
required-column preservation** for the RHS — without it, whatever machinery
computes "which columns does `g` need to expose" (going through
`IStorageCluster::read()`'s query-rewriting, since `g`'s storage is still
`IStorageCluster`-typed even with an empty resolved cluster name) doesn't correctly
account for JOIN-condition-only column usage when handed the full multi-table query
directly, but does when handed a pre-flattened `SELECT <needed columns> FROM g`
subquery.

Fix: **reverted** — restored the original unconditional
`is_remote = planner_context->getTableExpressionDataOrThrow(table_expression).isRemote();`.
The RHS wrap always stays active regardless of the experimental setting. This
matches the task's original instruction to not touch this guard unless proven
necessary — it turned out not to be part of the actual fix.

(A related false lead along the way: setting `object_storage_cluster='vig-test'` as
a blanket **query-level** setting — as opposed to relying on the natural
`cluster_for_parallel_replicas` → `DatabaseDataLake` per-table resolution — made
`getClusterName()` resolve non-empty for the RHS too, since that setting is read
unconditionally ahead of any `isDistributed()`-gated fallback
(`StorageObjectStorageCluster::getClusterName()`). That's a distinct, real
recursion risk — `IStorageCluster::read()` builds a `ReadFromCluster` step
whenever `cluster_name_from_settings` is non-empty, with **no gating on
`processed_stage`** — but it turned out not to be what actually triggered the
`NOT_FOUND_COLUMN_IN_BLOCK` errors above; removing `object_storage_cluster` and
relying on the natural per-table `cluster_for_parallel_replicas` resolution, plus
fixing 3.2, was the combination that actually worked.)

### 3.4 q21: JOIN's driving table is one CTE scope below the outer query — `PlannerJoinTree.cpp`'s guard can't see it, but ClickHouse's MergeTree parallel-replicas candidate-finder already can

Unlike q2/q4/q13/q16 (a CTE referenced from the JOIN's RHS), q21 has its driving
table nested *inside* a CTE that is itself the JOIN's LHS:

```
outer q21 query (GROUP BY derived_action)
└─ LEFT JOIN
    ├─ transaction_event  (CTE: SELECT transaction_id FROM ice....txnlog WHERE ...)
    └─ alert_events        (CTE: SELECT ... FROM ice....event_alert LEFT JOIN policy_matches ...)
```

`PlannerJoinTree.cpp`'s `should_wrap_left_table` guard (§3.1) only inspects the
*current* JOIN's leftmost table expression — for the outer query, that's the
`transaction_event` `QueryNode`, not the `TableNode` for `txnlog` underneath it.
So even with `object_storage_cluster_bypass_join_wrap=1`, the outer JOIN is never
recognized as "leftmost table is `IStorageCluster`" and never gets sent whole;
`txnlog` just gets planned as an ordinary local subquery
(`Join / ReadFromObjectStorage`, no `ReadFromCluster` anywhere), confirmed by
running a flattened (CTE-names-inlined-by-hand) version of q21 for diagnosis.
(That flattened rewrite was only used to isolate the cause — **q21's actual SQL
is never to be rewritten**; any fix has to work on the query as originally
written.)

Confirmed via code reading (not yet re-validated live) that ClickHouse already
has a traversal that solves exactly this "see through CTEs to the driving table"
problem — for MergeTree, not (yet) for `IStorageCluster`: `src/Planner/findParallelReplicasQuery.cpp`.

- `getSupportingParallelReplicasQueries()` walks `QUERY` → join tree → `TABLE`
  through arbitrarily nested CTEs/subqueries and `LEFT`/`INNER ALL`/qualifying
  `RIGHT` `JOIN`s (this is the exact q21 shape: outer `QUERY` → `JOIN` → LHS
  `QUERY` (`transaction_event`) → `TABLE` (`txnlog`)), stopping only when the
  reached `TABLE` is or isn't eligible.
- `findQueryForParallelReplicas()` then refines that candidate list against the
  actual (dummy-storage) `QueryPlan` shape to find the largest subquery
  distributable up to `WithMergeableState`.
- `findTableForParallelReplicas()` separately walks the same JOIN/CTE structure
  to find the single leftmost eligible `TableNode` (`txnlog` for q21).
- `buildQueryPlanForParallelReplicas()` (used when
  `GlobalPlannerContext::parallel_replicas_node == &query_node`, checked in
  `Planner.cpp`) already does `queryNodeToDistributedSelectQuery()` on the whole
  candidate subtree — the same CTE-inlining fix as §3.2b, for free.

The one and only gate keeping all of this MergeTree-only is
`isTableNodeEligibleForParallelReplicas()`:

```cpp
if (!storage->isMergeTree() && !typeid_cast<const StorageDummy *>(storage.get()))
    return false;
```

**What was *not* done**: `buildQueryPlanForParallelReplicas()`'s execution path
(`ClusterProxy::executeQueryWithParallelReplicas`) is MergeTree/task-based
parallel-replicas machinery (coordinator-driven part assignment) — not correct
for an `IStorageCluster` driver, which needs `IStorageCluster::read()`'s own
`ReadFromCluster` path instead. So `isTableNodeEligibleForParallelReplicas()`
itself was deliberately **not** relaxed — doing so would flow straight into
`GlobalPlannerContext::parallel_replicas_node`/`parallel_replicas_table` at
`Planner::Planner()` construction time, for *every* query in the session while
the experimental setting is on, and could get `buildQueryPlanForParallelReplicas()`
invoked for real against an Iceberg table with no safety net.

**What was done instead (diagnostic only, pushed as `arm64-v8`)**: a
self-contained *copy* of `getSupportingParallelReplicasQueries()` +
`findTableForParallelReplicas()` (the internal recursive one), with eligibility
relaxed to `dynamic_cast<const IStorageCluster *>(storage.get()) != nullptr`
instead of `isMergeTree()`, used only to log — never wired into
`GlobalPlannerContext` or execution:

- `src/Planner/findParallelReplicasQuery.cpp`: added
  `getSupportingObjectStorageClusterQueriesForDiagnostic()`,
  `findObjectStorageClusterDriverTableForDiagnostic()`, and
  `logObjectStorageClusterParallelReplicasCandidate()` (exported via
  `findQueryForParallelReplicas.h`), no-op unless
  `object_storage_cluster_bypass_join_wrap` is set.
- `src/Planner/Planner.cpp`: call it from the end of `Planner`'s constructor
  body (after `planner_context` is built, so on a fully-resolved query tree),
  logging `LOG_WARNING` tag `PR_CLUSTER_DIAG`: number of nested candidate
  queries found, whether the *outermost* one equals the whole query passed in
  (i.e. whether the whole query — CTEs and all — is a valid distributable
  candidate down to the driver table), and the driver table's full name (or
  `<none>`).

Compiled clean (no warnings) against `build/`, including a full local relink
with no undefined symbols.

**First live result (`arm64-v8`), and why it wasn't conclusive on its own:** q21
produced exactly one `PR_CLUSTER_DIAG` line:

```
PR_CLUSTER_DIAG candidates=1 outermost_candidate_is_top_query=true
driver_table=ice.`billion-rows_t17175.txnlog`
```

This proves the traversal *does* reach `txnlog` as the `IStorageCluster` driver
— but `candidates=1` is one fewer than expected for the true outer q21
(`outer query` → `transaction_event` → `txnlog` should be 2 candidates), and
`Planner`'s constructor runs once per recursively-planned subquery too (see
`buildJoinTreeQueryPlan()`'s `Planner subquery_planner(...)`), each with its own
already-narrow view of the tree. So the one line captured could equally have
been the *recursive* Planner instance for `transaction_event` alone (which
trivially sees `candidates=1`, `outermost_candidate_is_top_query=true` for
*itself*), not the true outer q21 invocation — `outermost_candidate_is_top_query`
means "top of this Planner invocation's own query tree," not "the original
top-level query."

**Fix (pushed as `arm64-v9`):** `SelectQueryOptions` already carries
`subquery_depth`/`is_subquery` (set by `.subquery()` for every recursively
planned subquery). Added both to the log line, and restricted the diagnostic to
only fire for the outermost (`is_subquery == false`) `Planner` invocation —
still purely a logging change, no execution impact:

```cpp
if (select_query_options.is_subquery)
    return;
...
LOG_WARNING(getLogger("ParallelReplicasClusterDiag"),
    "PR_CLUSTER_DIAG depth={} is_subquery={} candidates={} outermost_candidate_is_top_query={} driver_table={}",
    select_query_options.subquery_depth, select_query_options.is_subquery, ...);
```

`logObjectStorageClusterParallelReplicasCandidate()` now takes
`select_query_options` as a third argument (was `query_tree_node, context`;
now `query_tree_node, context, select_query_options`).

The other diagnostics from the same `v8` run corroborate that nothing was
actually distributed (as expected — this is log-only): `CLUSTER_ALT` for one of
q21's scopes still showed `subqueries=2 has_join=true ... normal=false
ignoring_join=false experimental_allow_join=false` (the current §3.2b analyzer
gate still rejects that scope — expected, since none of this diagnostic touches
the real eligibility/gating), a nested scope showed `is_distributed=true
can_use_pr=false final_cluster=''` (no accidental recursive cluster dispatch),
and all `OSC_STAGE` lines showed `resolved_cluster=''` (every actual read stayed
local, i.e. q21 executed exactly as it did before any of this diagnostic was
added).

Compiled clean (no warnings) against `build/`. **Not yet re-run against the
live cluster with the depth/is_subquery fix.** Expected, if the hypothesis is
right, for q21: exactly one `PR_CLUSTER_DIAG depth=0 is_subquery=false` line,
with `candidates=2` (outer query + `transaction_event`) and
`driver_table=ice.\`billion-rows_t17175.txnlog\``. If that holds, the next step
(not yet started) is a real `IStorageCluster`-flavored sibling of
`buildQueryPlanForParallelReplicas()` — clone the candidate subtree, rewrite it
with `queryNodeToDistributedSelectQuery()` (as already happens for
`LOCAL`/`GLOBAL` join modes and now, via §3.2b, for `ALLOW`), and hand it to
`IStorageCluster`'s own `ReadFromCluster` path instead of
`ClusterProxy::executeQueryWithParallelReplicas` — without touching q21's SQL
at all.

### 3.5 First whole-query dispatch prototype (`arm64-v10`) — real execution wiring, not yet validated live, four known gaps

Confirmed live (`v9`) that the whole outer q21 query, CTEs included, is a valid
distributable candidate down to `txnlog` (§3.4's `PR_CLUSTER_DIAG depth=0
is_subquery=false candidates=1 outermost_candidate_is_top_query=true
driver_table=ice.\`billion-rows_t17175.txnlog\``). Built the execution side
described at the end of §3.4:

- `src/Planner/findParallelReplicasQuery.cpp` / `.h`: two new exported
  functions, mirroring `findQueryForParallelReplicas()`/
  `buildQueryPlanForParallelReplicas()` but for an `IStorageCluster` driver:
  - `findObjectStorageClusterWholeQueryDriver(query_node, context)` — reuses
    the same relaxed-eligibility traversal proven live in §3.4 (promoted from
    diagnostic-only to real use), gated on the experimental setting, and on
    the driver **not** being the JOIN's immediate leftmost table expression
    (that shape is q17's — left entirely on the existing, already-proven §3.1
    mechanism, untouched).
  - `buildQueryPlanForObjectStorageCluster(query_node, driver_table_node,
    planner_context)` — clones the whole candidate query, serializes it with
    `queryNodeToDistributedSelectQuery()`, and calls the driver storage's own
    `read()` (i.e. `IStorageCluster::read()`) with that whole query as
    `query_info.query`/`query_tree`, `WithMergeableState` as the target stage.
- `src/Planner/Planner.cpp`: new `else if` branch at the same dispatch point
  as the MergeTree `parallel_replicas_node == &query_node` check, calling the
  two functions above instead of `buildJoinTreeQueryPlan()` when they apply.
- `src/Storages/IStorageCluster.cpp`: `RestoreQualifiedNamesVisitor` (which
  assumes the outer query's first table expression *is* this storage) is now
  skipped when that first table expression is a subquery instead — true only
  for a query dispatched through the new whole-query path, never for any
  existing `IStorageCluster` caller (s3Cluster, hdfsCluster, the §3.1 leftmost
  case), so this is provably a no-op everywhere else.

Compiled clean (isolated `.o` checks + a full local `build/` relink with no
undefined symbols). Smoke-tested locally with the experimental setting off
(default): a `WITH ... AS (...) SELECT ... JOIN` query over `numbers()`
executes identically to before — confirms the new code path is inert unless
opted in. Pushed as `arm64-v10`. **Not yet run against the live cluster.**

**Review feedback on this first cut (four open issues, none fixed yet —
deliberately checking before iterating further, since each live rebuild is
expensive and guessing wrong wastes a cycle):**

1. **The q17-vs-q21 split is benchmark-shaped, not semantic.** Whether the
   driver sits directly under the JOIN or one CTE-scope below it shouldn't
   determine which distributed-execution mechanism ClickHouse uses — this
   exclusion exists only so the new, unproven path can't regress q17's
   already-proven one while both coexist. Should collapse into one rule
   ("find the outermost safely-distributable query, find its driver, dispatch
   it") once the new path is trusted enough to subsume §3.1 entirely.
2. **Doesn't reuse the *whole* parallel-replicas eligibility algorithm.**
   `findObjectStorageClusterWholeQueryDriver()` only reuses the structural
   traversal (`getSupportingParallelReplicasQueries()`-equivalent) — not
   `findQueryForParallelReplicas()`'s dummy-storage-plan walk, which checks
   that every step between the candidate and the driver is `Expression`/
   `Filter`/`Join`/mergeable-`Sorting` and stops at anything needing initiator
   finalization. Fine for q21's simple shape; for an arbitrary query this
   could pick a candidate containing a planner operation the new dispatch
   path was never designed to absorb. The real fix builds the same
   dummy-storage plan and walks it, the way `findQueryForParallelReplicas()`
   already does, rather than trusting the structural candidate alone.
3. **`SelectQueryInfo` and `column_names` are fabricated too minimally.**
   `buildQueryPlanForObjectStorageCluster()` constructs a near-empty
   `SelectQueryInfo` (just `query`/`query_tree`/`planner_context`) instead of
   cloning the enclosing `select_query_info` (which already carries storage
   limits, filters, etc.) and overriding only what needs to change. Likewise
   `column_names` uses the driver's *entire physical column list*
   (`storage_snapshot->getColumns(AllPhysical)`) instead of the columns
   actually required, pulled from the driver's own `TableExpressionData`. Both
   happen to be harmless for q21 specifically but are exactly the kind of
   shortcut that passes one benchmark query and breaks something else
   (virtual columns, subcolumns, dropped storage limits) later.
4. **The cluster may never actually resolve for the nested driver — likely
   blocker, needs an empirical check before anything else.**
   `StorageObjectStorageCluster::getClusterName()` returns either the
   `object_storage_cluster` session setting (deliberately unset, §5) or the
   value baked in when the storage was *constructed*
   (`getOriginalClusterName()`); the new dispatch reads from the driver's
   already-built `StoragePtr` as-is and has no way to influence that. Per
   `DatabaseDataLake::tryGetTableImpl()`:
   ```cpp
   const auto can_use_parallel_replicas = !parallel_replicas_cluster_name.empty()
       && query_settings[Setting::parallel_replicas_for_cluster_engines]
       && context_->canUseTaskBasedParallelReplicas()
       && !context_->isDistributed();
   ```
   Hypothesis (from re-reading the code, not yet confirmed live): each nested
   `QueryNode` gets its own context via `Context::createCopy(...)`
   (`Planner.cpp`), created *after* `QueryAnalyzer::resolveQuery()`'s
   `CLUSTER_ALT` decision for the enclosing scope has already run (the
   "disable cache during join tree resolution" comment sits immediately after
   that block, before join-tree/CTE resolution). For q21's outer scope,
   `subquery_count=2` fails the `subquery_count <= 1` check regardless of
   `has_join`/`experimental_allow_join` (§3.2), forcing
   `parallel_replicas_for_cluster_engines=false` on the outer context *before*
   `transaction_event`/`txnlog` are ever resolved. If that `false` propagates
   into the child context `txnlog` resolves under, `DatabaseDataLake`
   constructs it with an empty cluster name regardless of anything the new
   dispatch code does downstream -- fully consistent with all three `v9`
   `OSC_STAGE ... resolved_cluster=''` lines (`txnlog` included). If confirmed,
   the fix belongs in `QueryAnalyzer.cpp`/
   `TableFunctionsWithClusterAlternativesVisitor` (§3.2) -- teach it to also
   skip forcing `parallel_replicas_for_cluster_engines=false` when a
   whole-query `IStorageCluster` driver is reachable through nested CTEs
   (reusing the same traversal as §3.4/`findObjectStorageClusterWholeQueryDriver`),
   not just in the dispatch code added here.
   **Next action: run q21 against `arm64-v10` and check the `DLPR`/`OSC_STAGE`
   lines for `txnlog` specifically** -- confirms or refutes this before any
   further code changes.

### 3.6 Issue #4 confirmed live; fixed without touching `QueryAnalyzer.cpp` (`arm64-v11`)

Ran q21 against `arm64-v10`. Confirmed exactly the §3.5 issue #4 hypothesis:

```
CLUSTER_ALT tables=0 table_functions=0 subqueries=2 has_join=true normal=false ignoring_join=false experimental_allow_join=false
DLPR table=billion-rows_t17175.event_alert ... parallel_cluster_engines=false ... can_use_pr=false final_cluster=''
CLUSTER_ALT tables=0 table_functions=0 subqueries=1 has_join=false normal=true ignoring_join=true experimental_allow_join=true
DLPR table=billion-rows_t17175.event_alert ... parallel_cluster_engines=true ... can_use_pr=false final_cluster=''
PR_CLUSTER_DIAG depth=0 is_subquery=false candidates=1 outermost_candidate_is_top_query=true driver_table=ice.`billion-rows_t17175.txnlog`
OSC_STAGE table=ice.`billion-rows_t17175.txnlog` ... resolved_cluster='' ...
OSC_STAGE table=ice.`billion-rows_t17175.event_alert` ... resolved_cluster='' ...
OSC_STAGE table=ice.`billion-rows_t17175.event_alert` ... resolved_cluster='' ...
```

`resolved_cluster=''` for `txnlog` too, confirming the whole-query dispatch (once entered) still falls back to a local read, exactly as predicted.

**Pushback on the proposed fix direction (correct, and changed the plan):**
touching `QueryAnalyzer.cpp` to also skip forcing `parallel_replicas_for_cluster_engines=false`
when a whole-query driver is reachable would be wrong even if it "worked" — it operates at the
scope of the *entire query*, so it would make **every** DataLake table resolved under that scope
cluster-aware, not just the chosen driver (`txnlog` → cluster, but `event_alert`/`policy_matches`
→ cluster too, when the correct shape is `txnlog` → distributed driver, everything else → ordinary
per-worker local reads). That's a planner-level decision about one specific table, not a property
of the whole analyzer scope, and relaxing the scope-wide gate is exactly the kind of change that
"passes this benchmark and breaks something else" the review flagged for issues #2/#3 too.

**A second, more fundamental problem, found in the same review:** even with a resolved cluster
name, `StorageObjectStorageCluster::updateQueryForDistributedEngineIfNeeded()` — the function that
rewrites a plain `ice.txnlog` reference into the task-iterator-consuming `icebergS3(...)`-with-
`object_storage_cluster`-setting form that workers need to do *partitioned* distributed reading
(as opposed to redundantly scanning the whole table on every worker) — unconditionally operated on
`tables->children[0]`, the outer query's *first* table expression. For q21 that's `transaction_event`
(a subquery), not `txnlog`, so this rewrite silently no-oped (`return false` at the
`!table_expression->database_and_table_name` check) regardless of cluster-name resolution. Simply
fixing cluster-name resolution without fixing this would have been actively dangerous: `ReadFromCluster`
would dispatch the (unrewritten) whole query to every worker, and each worker would independently
execute an *ordinary, non-distributed* `ice.txnlog` read — i.e. `worker 0/1/2 → full txnlog` instead
of `worker N → txnlog's assigned tasks`, silently multiplying the result rather than partitioning it.
A textbook case for the fail-close principle: better to keep falling back to local (as `v10` did) than
to "succeed" with wrong data.

**Fix (pushed as `arm64-v11`), two independent, narrowly-scoped changes — neither touches
`QueryAnalyzer.cpp` or any per-scope setting:**

1. `src/Planner/findParallelReplicasQuery.cpp` (`buildQueryPlanForObjectStorageCluster`): instead of
   relying on the driver's already-baked-in cluster name, explicitly read
   `cluster_for_parallel_replicas` from the query's own settings and set it as the
   `object_storage_cluster` setting on a **context copy scoped only to this one `read()` call**:
   ```cpp
   const auto & driver_cluster_name = context->getSettingsRef()[Setting::cluster_for_parallel_replicas].value;
   if (driver_cluster_name.empty())
       throw Exception(ErrorCodes::LOGICAL_ERROR,
           "object_storage_cluster_bypass_join_wrap whole-query dispatch requires cluster_for_parallel_replicas to be set");

   auto driver_context = Context::createCopy(context);
   driver_context->setSetting("object_storage_cluster", Field(driver_cluster_name));
   ```
   Only `txnlog`'s `read()` call sees this; `event_alert`/`policy_matches` are resolved independently,
   per worker, from the query text — unaffected, exactly the shape the review asked for. Fails loudly
   (`LOGICAL_ERROR`) rather than silently falling back if the setting is missing, since silently
   executing this dispatch un-clusterized would be the same wrong-data risk described above.
2. `src/Storages/ObjectStorage/StorageObjectStorageCluster.cpp`
   (`updateQueryForDistributedEngineIfNeeded`): added `findTableExpressionForStorageId()`, which
   searches the whole query -- recursing into subqueries -- for the `ASTTableExpression` whose
   `database_and_table_name` matches *this storage's own* `StorageID`, instead of assuming
   `tables->children[0]`. Falls back to the old position-0 logic defensively if the search finds
   nothing. This is a **strict generalization**: for every existing caller (s3Cluster, hdfsCluster,
   the §3.1 leftmost-table case), the storage genuinely *is* at position 0, so the search finds it
   there as the first and only match — provably a no-op for all of them. Not
   Iceberg-JOIN-experiment-specific.

Compiled clean (no warnings), full local `build/` relink with no undefined symbols, local
flag-off sanity check (CTE+JOIN query unaffected). Built and pushed `arm64-v11`. **Not yet run
against the live cluster.**

### 3.7 Redesign: exact QueryTree replacement (`StorageDistributed` pattern), four live bugs found and fixed, q21 now correct; new hot-latency regression found on q2/q4/q13 (`arm64-v12` through `arm64-v17`)

External review (independent of the person driving this session) pushed back hard on `v11`'s
AST-level `findTableExpressionForStorageId()` + `RestoreQualifiedNamesVisitor`-skip approach:
both are string/position-based workarounds sitting on top of an already-serialized AST, ambiguous
for self-joins, and not the pattern `StorageDistributed::buildQueryTreeDistributed()` already uses
for exactly this problem (replace one exact resolved table expression inside an arbitrary
`QueryTree`, wherever it is nested, via `IQueryTreeNode::cloneAndReplace()` keyed on node identity,
not name/position). Traced `StorageDistributed`'s mechanism in full (its `remote_table_function`
branch, `cloneAndReplace()`'s automatic weak-pointer rebinding of `ColumnNode::column_source`, and
`StorageDistributed::read()`'s `SelectQueryInfo modified_query_info = query_info;` preservation
pattern) and rebuilt the whole-query dispatch on top of it instead:

- `StorageObjectStorageCluster::buildClusterTableFunctionAST()` (new): builds a throwaway
  single-table `SELECT ... FROM <this storage>` query and runs it through the *existing*,
  already-proven `updateQueryToSendIfNeeded()`/`updateQueryForDistributedEngineIfNeeded()` rewrite
  (reused, not duplicated) to produce a standalone `icebergS3Cluster('vig-test', <args>,
  structure=..., format=...)` AST fragment — the exact shape already proven for q17 — without
  mutating any existing query. Takes the cluster name explicitly (not `getClusterName(context)`),
  since a driver nested under a CTE has an empty resolved cluster name of its own.
  `updateQueryForDistributedEngineIfNeeded()` was reverted to its original unconditional
  `tables->children[0]` lookup — `findTableExpressionForStorageId()` was deleted entirely, no
  longer needed by any caller.
- `findParallelReplicasQuery.cpp`'s `buildQueryPlanForObjectStorageCluster()`: rewritten to build
  the replacement `TableFunctionNode` via `buildQueryTree()` + `QueryAnalysisPass` (mirroring
  `buildQueryTreeDistributed()`'s `remote_table_function` branch almost verbatim), then
  `query_tree->cloneAndReplace(driver_table_expression, table_function_node)` — one call, exact
  node identity, correct for self-joins by construction. Copies the *real* enclosing
  `SelectQueryInfo` instead of fabricating one.
- `IStorageCluster::readPreparedClusterQuery()` (new): a narrower entry point that skips straight
  to `getClusterImpl()` + `ReadFromCluster` construction, for a caller that has already built the
  complete, correctly-qualified query. `read()`'s own `first_table_expression->subquery` /
  `RestoreQualifiedNamesVisitor`-skip special case was reverted — no longer needed, since
  `cloneAndReplace()`'s structural correctness makes that visitor's string-based rewrite
  unnecessary for this path (and it would be wrong to keep applying to it).

**Four live bugs found and fixed, one per rebuild/run cycle** (each confirmed with an actual
exception or checksum before moving to the next):

1. **`Table expression ... is not registered in planner context`** (`v12`→`v14` fix): after
   `cloneAndReplace()`, `query_info.table_expression` was still set to the *old* (now-replaced)
   driver `TableNode`, but `query_info.planner_context` was the fresh context built from the
   *replaced* tree — which only knows about the new `TableFunctionNode`. Fixed by setting
   `query_info.table_expression = table_function_node` (the exact node actually registered in the
   new planner context) instead. `driver_storage`/`driver_storage_snapshot` (used for
   `getTaskIteratorExtension()`) are separate parameters, unaffected.
2. **Same-error-message-shape but different cause, `NOT_FOUND_COLUMN_IN_BLOCK` /
   `icebergS3Cluster(...) AS __table2 is not registered in planner context`** (`v14`→`v15` fix):
   fixing #1 only moved the failure to the *replacement* node, because
   `getSampleBlockAndPlannerContext()`'s `only_analyze=true` interpreter never actually plans
   `prepared_query_to_send`'s replaced table expression as its own table read (that only happens
   later, per-worker) — so *no* planner context, old or new, will ever have a
   `TableExpressionData` entry for it. Root realization: for this prepared `ReadFromCluster`,
   `query_info.table_expression`/`planner_context` don't have their normal per-table meaning at
   all — this step represents dispatch of the *entire* remote query, not a single-table read. Fixed
   in `IStorageCluster::readPreparedClusterQuery()` by capturing `external_tables` from
   `query_info.planner_context` first (unchanged), then clearing both fields before constructing
   `ReadFromCluster` — `SelectQueryInfo::buildNodeNameToInputNodeColumn()` (the only consumer,
   called from `SourceStepWithFilter::applyFilters()`) is guarded by `if (planner_context)` and
   falls back to `{}`, the same degraded-but-safe path `SourceStepWithFilterBase::applyFilters()`
   always uses for storages with no `QueryTree`/planner context at all.
3. **Correctness regression on `q2`/`q4`/`q13`/`q16`**: full-suite validation (never done before
   this point — see §9's own open item) surfaced `Not found column __tableN.appinfo_app in block`
   for every query shaped `LEFT JOIN(direct IStorageCluster TableNode, QueryNode/CTE)` — e.g. q2's
   `event_app LEFT JOIN appinfo_d` (`appinfo_d` a CTE). Root cause: `object_storage_cluster_bypass_join_wrap`'s
   leftmost-table bypass (§3.1) was gated only on "is there a JOIN and is the setting on" — not on
   what the *other* side of the JOIN actually is. For q17 the RHS is a direct `TableNode`
   (`IStorageCluster::read()`'s AST-rewrite/`RestoreQualifiedNamesVisitor` machinery can serialize
   that safely); for q2/q4/q13/q16 the RHS is a `QueryNode` (a CTE/subquery — a query boundary that
   machinery cannot safely serialize). Fixed with a capability check,
   `canBypassClusterJoinWrapForTableExpressions()` in `PlannerJoinTree.cpp`: walks the same
   `table_expressions_stack` already used to find the leftmost table, and requires every *other*
   entry to not be a `QUERY`/`UNION` node (structural `JOIN`/`CROSS_JOIN`/`ARRAY_JOIN` nodes in the
   flattened stack are fine). Keyed purely on `QueryTreeNodeType`, not query/table/CTE names. When
   the RHS fails this check, `should_wrap_left_table` falls back to exactly its pre-experiment
   value — q2/q4/q13/q16 get their original wrapped/local topology back. Pushed as `arm64-v16`.
4. **Same error persisted identically after fix #3** (`v16`→`v17` fix) — traced with the exact
   live query text this time rather than reasoning abstractly. The *actual* culprit was a second,
   independent bug in the same mechanism: `findObjectStorageClusterWholeQueryDriver()` runs for
   *every* `Planner` instance, including the recursive one built for `appinfo_d`'s own CTE body
   (`SELECT * FROM (SELECT ... FROM appinfo WHERE ...) t WHERE rn = 1`) — a plain chain of
   derived-table subqueries around one table, with **no JOIN anywhere in it**. The existing
   "immediate leftmost" exclusion only rejects a scope whose own join tree is directly a bare
   `TABLE`/`TABLE_FUNCTION`; it says nothing about a `QUERY`-only passthrough chain. So the
   traversal walked straight through `appinfo_d → t → appinfo`, "succeeded" with `appinfo` as
   driver, and dispatched *just that CTE body* to the cluster independently via
   `icebergS3Cluster(...)` — producing a sub-plan whose internal column identifiers
   (`__table3`/`__table4`) didn't match what the outer q2 JOIN expected once it tried to read the
   result. Fixed by adding a `saw_join` out-parameter to
   `getSupportingObjectStorageClusterQueriesForDiagnostic()` (set only in the `JOIN` case, not
   `ARRAY_JOIN`/`CROSS_JOIN`), and requiring it in `findObjectStorageClusterWholeQueryDriver()` —
   this whole mechanism exists to bypass a JOIN-wrap guard, so it must never fire for a candidate
   with no JOIN anywhere in its path to the driver. Pushed as `arm64-v17`.

**Result as of `arm64-v17`, full 23-query IcebergBench suite, `object_storage_cluster_bypass_join_wrap=1`
set for the whole run:**

- **All 23 queries `ok`** — no errors, no correctness mismatches.
- `q17`/`q21` show the intended speedup vs. the published pre-experiment baseline
  ([`BENCHMARK.md`](https://github.com/Altinity/IcebergBench/blob/main/BENCHMARK.md), Antalya 26.6
  column): q17 hot `6.972s → 3.829s` (~45% faster), q21 hot `18.203s → 8.930s` (~51% faster).
- **New finding, not yet root-caused: a hot-latency regression on `q2`/`q4`/`q13`** (not `q16`,
  which came back slightly *faster* than baseline despite being the same CTE shape): q2 hot
  `1.002s → 3.285s` (3.3×), q4 hot `1.623s → 6.510s` (4.0×), q13 hot `1.176s → 4.290s` (3.6×).
  Correctness is fine (checksums stable across runs); this is purely a latency regression.
  Working hypothesis (**not yet confirmed with log evidence**): `object_storage_cluster_bypass_join_wrap=1`
  also loosens a *pre-existing, unrelated* analyzer check
  (`TableFunctionsWithClusterAlternativesVisitor::shouldReplaceWithClusterAlternativesIgnoringJoin()`,
  §3.2 — predates today's whole-query-dispatch work) that normally forces
  `parallel_replicas_for_cluster_engines` off for any query containing a JOIN. With it not forced
  off, some *other* table inside these queries — suspect: `appinfo` inside `appinfo_d`, which
  computes `row_number() OVER (...)` and must fully materialize on one node regardless — may now be
  going through a distributed/task-iterator cluster read on its own, independent of and unrelated
  to the JOIN-pushdown mechanism this experiment is actually about, paying coordination overhead
  for a scan pattern that has no fan-out benefit. **Next action, before any further code change:**
  confirm via the existing `DLPR`/`OSC_STAGE` log lines for a q2 run whether `appinfo` shows
  `resolved_cluster='vig-test'` (confirms the hypothesis) or `resolved_cluster=''` (refutes it,
  need to look elsewhere).

Known gaps carried forward from earlier sections, still open, not solved by this redesign (see
§3.5 issues #2/#3, restated in current form): `column_names` passed to `readPreparedClusterQuery()`
is the driver's full `AllPhysical` column list rather than the exact required set, and
`query_info.filter_actions_dag` is never populated for the driver, so
`StorageObjectStorageCluster::getTaskIteratorExtension()` cannot prune Iceberg files/partitions for
the driver at the initiator (workers still apply the query's own `WHERE` correctly — this is a
performance gap, not a correctness one). Root cause: `collectTableExpressionData()` deliberately
does not descend into subquery/CTE scopes, and this whole-query dispatch intentionally bypasses the
recursive per-CTE `Planner` invocation that would otherwise populate that data. Also not ported:
the full MergeTree dummy-plan eligibility walk (`findQueryForParallelReplicas()`'s
`StorageDummy`-based per-step validation) — deliberately deferred, since it encodes MergeTree-
specific semantics (the non-driver side of a JOIN may not be independently readable on every
replica) that don't hold for DataLake/object storage (every worker can independently and safely
re-read/re-aggregate the non-driver side from the shared catalog).

---

## 4. New setting

```
object_storage_cluster_bypass_join_wrap  (Bool, default false)
```

Declared in `src/Core/Settings.cpp`, registered in
`src/Core/SettingsChangesHistory.cpp` under the
`26.6.2.20001.altinityantalya` version block. Off by default — behavior is
unchanged unless a query opts in via `SETTINGS object_storage_cluster_bypass_join_wrap = 1`.

Currently gates three things (see §3):
1. `PlannerJoinTree.cpp`'s leftmost-table subquery-wrap guard.
2. `QueryAnalyzer.cpp`'s `has_join` prohibition inside
   `TableFunctionsWithClusterAlternativesVisitor`.
3. `IStorageCluster.cpp`'s choice of AST serialization for `query_to_send` in
   `read()` — `queryNodeToDistributedSelectQuery()` (CTE-safe) instead of the
   default `query_info.query` (CTE-name-only) when the flag is on.

Does **not** gate the RHS wrap (reverted, always active).

---

## 5. Settings required for the A/B (current, validated shape)

```sql
SETTINGS
    enable_parallel_replicas = 1,
    cluster_for_parallel_replicas = 'vig-test',
    parallel_replicas_for_cluster_engines = 1,
    object_storage_cluster_bypass_join_wrap = 1,
    filesystem_cache_name = 's3_disk_cache',
    use_page_cache_for_object_storage = 1,
    remote_read_min_bytes_for_seek = 1048576
```

Notably **not** used (found to be actively harmful / a wrong turn):
`object_storage_cluster = 'vig-test'` — this blanket query-level setting
overrides `StorageObjectStorageCluster::getClusterName()` for *every* table in the
query (both `e` and `g`), unconditionally ahead of the `isDistributed()`-gated
DataLake fallback, defeating the natural per-table differentiation that
`cluster_for_parallel_replicas` alone provides (only the leftmost table gets
clusterized; the RHS correctly stays local because by the time it resolves,
`context->isDistributed()` is already `true` from the left table's resolution).

`parallel_replicas_for_cluster_engines = 1` is required explicitly in `SETTINGS`
even though its declared default is `true` — see §3.2, without the `QueryAnalyzer.cpp`
fix this gets silently forced back to `false` by the analyzer for any JOIN query
regardless of what's requested.

---

## 6. Diagnostics added (all behind no flag — always log when hit; harmless overhead)

| Log tag | File | What it shows |
|---|---|---|
| `DLPR` | `DatabaseDataLake.cpp` (`tryGetTableImpl`) | Per-table-resolution: `cluster_for_pr`, `parallel_cluster_engines`, `can_task_pr`, `is_distributed`, `query_kind`, `can_use_pr`, `final_cluster` |
| `OSC_STAGE` | `StorageObjectStorageCluster.cpp` (`getQueryProcessingStage`) | `original_cluster`, `resolved_cluster`, `cluster_supported`, `query_kind`, `to_stage` |
| `CLUSTER_ALT` | `QueryAnalyzer.cpp` (`resolveQuery`) | `tables`, `table_functions`, `subqueries`, `has_join`, `normal` (original check), `ignoring_join` (has_join-only-lifted check), `experimental_allow_join` |
| `PR_CLUSTER_DIAG` | `findParallelReplicasQuery.cpp` (`logObjectStorageClusterParallelReplicasCandidate`, called from `Planner`'s constructor) | Only logged when `object_storage_cluster_bypass_join_wrap=1`, and only for the outermost (non-subquery) `Planner` invocation: `depth`/`is_subquery` (from `SelectQueryOptions`, always `0`/`false` since v9's fix — logged anyway as a sanity check), `candidates` (nested candidate-query count), `outermost_candidate_is_top_query` (whether the *whole* query being planned — CTEs included — is a valid distributable candidate down to the driver table), `driver_table` (full name of the leftmost `IStorageCluster` table found through the CTE/JOIN structure, or `<none>`). See §3.4. |

Grep pattern: `grep -E 'DLPR|OSC_STAGE|CLUSTER_ALT|PR_CLUSTER_DIAG' /var/log/clickhouse-server/clickhouse-server.log`

These are left in place (not stripped) in the currently-pushed images; they should
be removed before any production-quality version of this patch, once the fix design
is finalized.

---

## 7. Build/deploy notes

- Cross-compiled natively for arm64 via `build_arm64/` (`-DCMAKE_TOOLCHAIN_FILE=cmake/linux/toolchain-aarch64.cmake`)
  — the actual IcebergBench cluster (`vig-test`, via the ClickHouse Kubernetes
  Operator) runs on arm64. A plain `docker build --platform` cannot cross-compile;
  it either runs the wrong-arch binary or QEMU-emulates compilation of this whole
  codebase, impractically slow. amd64 built for local iteration.
- Hit and worked around an unrelated pre-existing local-build issue: the checked-in
  Rust-symbol-stripping step (`cmake/strip_rust_symbols.sh`, applied to the `prql`
  and `polyglot` crates via `ld.lld -r` partial relink) produces `stripped.o`
  objects that trip `DW.ref.rust_eh_personality` under `--gc-sections` in this
  local toolchain/lld version. Worked around by building with
  `-DENABLE_PRQL=OFF -DENABLE_POLYGLOT=OFF` (unrelated to Iceberg/JOIN
  functionality, not present in the actual deployed image's feature set either way
  since only `clickhouse-server`/`clickhouse-client` type functionality matters
  here).
- Hit a second, unrelated pre-existing local-build issue while relinking for
  `arm64-v7`: `utils/check-large-objects.sh` (invoked as part of the `clickhouse`
  link step) failed the build because a Rust codegen-unit object for chdig's
  `perfetto_protos` dependency (`51,324,664` bytes) came in just over the
  `50,000,000`-byte-per-translation-unit limit — the actual `clang++` link had
  already succeeded by that point, only the post-link size-check step failed.
  Not related to anything in this experiment's diff (only `IStorageCluster.cpp`
  changed for `v7`) and not present in `v6`'s build, so most likely Rust
  codegen-unit-splitting nondeterminism pushing an already-borderline object
  over the line. Worked around, as the script itself suggests, by reconfiguring
  with `cmake -DCHECK_LARGE_OBJECT_SIZES=0 .` in `build_arm64/` and relinking.
- Each build was stripped (`llvm-strip-21` / equivalent) before
  packaging: ~5.2 GB unstripped (debug info) → ~615 MB stripped.
- Packaged into two kinds of images, both pushed to
  `docker.io/nerflongshotuv/clickhouse-test`:
  - **`iceberg-join-bypass-*`** (one-shot: bakes a specific binary in). Iterated
    through several tags as fixes landed: `-amd64`, `-arm64`, `-arm64-v2` (RHS-wrap
    fix, reverted), `-arm64-v3` (RHS-wrap reverted back), `-arm64-v4` (diagnostics
    added), `-arm64-v5` (root cause 3.2 fix, first/broad version), `-arm64-v6`
    (narrow fix for 3.2 — turned out not to actually change q2's eligibility, see
    §3.2), `-arm64-v7` (`IStorageCluster.cpp` CTE-serialization fix, §3.2b),
    `-arm64-v8` (adds the `PR_CLUSTER_DIAG` q21 diagnostic, §3.4 — first live
    run was ambiguous about outermost- vs recursive-Planner invocation),
    `-arm64-v9` (`PR_CLUSTER_DIAG` restricted to the outermost Planner
    invocation + `depth`/`is_subquery` fields, §3.4 — confirmed live that the
    whole q21 query is a valid candidate down to `txnlog`),
    `-arm64-v10` (first whole-query execution-dispatch prototype, §3.5 — real
    `IStorageCluster::read()` wiring for a nested driver; confirmed live to
    still fall back to a local read, per issue #4), `-arm64-v11` (explicit
    per-call cluster-name context + `findTableExpressionForStorageId`
    driver-targeted query rewrite, §3.6 — fixes issue #4 without touching
    `QueryAnalyzer.cpp`), `-arm64-v12` (§3.7 redesign: exact QueryTree
    `cloneAndReplace()` replacing the AST-search approach, `buildClusterTableFunctionAST()`,
    `readPreparedClusterQuery()` — first cut, not yet run live), `-arm64-v13`
    (narrowed whole-query-driver eligibility to `StorageObjectStorageCluster`
    specifically, §3.7), `-arm64-v14` (§3.7 bug #1 fix: `query_info.table_expression`
    pointed at the exact node registered in the new planner context), `-arm64-v15`
    (§3.7 bug #2 fix: clear `query_info.table_expression`/`planner_context` before
    constructing `ReadFromCluster` — this dispatch has no per-table planner-context
    meaning), `-arm64-v16` (§3.7 bug #3 fix: capability-gated JOIN-wrap bypass,
    `canBypassClusterJoinWrapForTableExpressions()`), **`-arm64-v17`** (current: §3.7
    bug #4 fix, `saw_join` gate on `findObjectStorageClusterWholeQueryDriver()` —
    first run with **all 23 IcebergBench queries correct**; new hot-latency
    regression found on q2/q4/q13, not yet root-caused).
  - **`bin-runner-{amd64,arm64}`**: a small (~130 MB) generic image
    (`/home/nerflongshot/Projects/Altinity/dev/clickhouse-bin-runner/`) that
    downloads the actual `clickhouse` binary at container start from
    `CLICKHOUSE_BINARY_URL` (env var / ConfigMap), caches it, wires up the
    standard multi-call symlinks, then execs into the normal ClickHouse
    entrypoint. Lets future variants be swapped via env var + pod restart instead
    of a new image build/push per iteration. Verified locally (download + symlink
    wiring + full HTTP server startup) before pushing.

---

## 8. Validated so far

- [x] Checksum match against the known-good oracle (`rows=200`,
      matching `xor_hash`/`sum_hash`) — multiple runs.
- [x] `EXPLAIN PLAN` / `EXPLAIN PIPELINE distributed=1` topology matches the target
      shape (JOIN + partial agg on workers, initiator only merges).
- [x] Per-node `JoinProbeTableRowCount` split confirmed across 5 independent runs,
      summing exactly to `238,000,777`, initiator always `0`.
- [x] Hot runtime ~4.4 s (vs ~6.97 s baseline, ~4.7 s manual proof).
- [x] Root cause of the `q2`/`q4`/`q13`/`q16`/`q21` CTE regression identified:
      not actually blocked by `has_join` (the `arm64-v6` "narrow" check doesn't
      exclude q2 either — see §3.2), but by `IStorageCluster::read()` sending a
      bare CTE-name reference (`appinfo_d`) to the remote node instead of the
      CTE's body. Fixed via `queryNodeToDistributedSelectQuery()`, built, and
      pushed (`arm64-v7`).

## 9. Not yet validated / open

- [ ] Re-run the full 23-query benchmark suite against `arm64-v9` with
      `object_storage_cluster_bypass_join_wrap=1` set for the whole suite (as it
      was for the run that surfaced the regression) — confirm q17 still
      fast/correct **and** q2/q4/q13/q16 now succeed (CTE body should be inlined
      instead of erroring on `Unknown table expression identifier`, per §3.2b).
      Check `CLUSTER_ALT` log lines: since `ignoring_join` turned out to be
      `true` for q2 as well as q17 (§3.2), all should now show
      `experimental_allow_join=true` — the differentiator is no longer that log
      line, it's whether the CTE queries now succeed end-to-end.
- [x] q21 candidate/driver discovery confirmed live on `v9`:
      `PR_CLUSTER_DIAG depth=0 is_subquery=false candidates=1
      outermost_candidate_is_top_query=true
      driver_table=ice.\`billion-rows_t17175.txnlog\`` — the whole outer q21
      query, CTEs included, is a valid distributable candidate down to
      `txnlog` (`candidates=1` rather than the naively-expected `2` is fine;
      the load-bearing fields are `outermost_candidate_is_top_query` and
      `driver_table`, see §3.4/§3.5).
- [x] Issue #4 (`v10`'s driver cluster name never resolving) confirmed live
      exactly as hypothesized: `OSC_STAGE table=...txnlog ... resolved_cluster=''`.
      Fixed in `v11` (§3.6) **without** touching `QueryAnalyzer.cpp` — per
      review pushback, relaxing the analyzer's scope-wide
      `parallel_replicas_for_cluster_engines` gate would have made every
      DataLake table under q21 cluster-aware, not just the chosen driver.
      Instead: (a) `buildQueryPlanForObjectStorageCluster()` now sets
      `object_storage_cluster` explicitly on a context copy scoped to just the
      driver's `read()` call, reading the cluster name straight from
      `cluster_for_parallel_replicas`; (b) a second, independently-discovered
      problem — `StorageObjectStorageCluster::updateQueryForDistributedEngineIfNeeded()`
      only ever looked at the outer query's first table expression to decide
      which table to rewrite into the task-iterator-consuming
      `icebergS3(...)`-with-`object_storage_cluster` form, so even a resolved
      cluster name wouldn't have made `txnlog` (nested under `transaction_event`)
      get that rewrite — meaning workers would each have redundantly scanned
      the *whole* `txnlog` table instead of their assigned partition, a silent
      correctness bug, not just a fallback. Fixed by making that function
      search the whole query (recursing into subqueries) for its own
      `StorageID` instead of assuming position 0 — a strict generalization,
      provably a no-op for every other existing caller.
- [ ] q21 real execution: `arm64-v11` is built and pushed but **not yet run
      live**. This is the next thing to check — does q21 now actually
      distribute (`ReadFromCluster` in the plan, `OSC_STAGE ... resolved_cluster='vig-test'`
      for `txnlog`) and produce correct results?
- [ ] Once q21 actually executes correctly end-to-end: fix remaining review
      issues #2 (reuse the full dummy-plan-walk eligibility check from
      `findQueryForParallelReplicas()`, not just the structural traversal) and
      #3 (clone `select_query_info` instead of fabricating a near-empty one;
      pull `column_names` from the driver's `TableExpressionData` instead of
      all physical columns) before trusting this beyond q21. Issue #1
      (collapse the q17/q21 split into one rule) is lower priority — deferred
      until the new path is validated enough to consider replacing §3.1
      rather than coexisting with it.
- [ ] Multiple hot-run timing samples (median, not one lucky run) for a more
      reliable performance number.
- [ ] Design of the narrow, non-flag production-quality condition (per the
      original task's ask): auto-detect "left storage is a DataLake-catalog
      `StorageObjectStorageCluster`, created from a shared catalog, all other table
      expressions in the JOIN resolvable on every cluster node,
      `object_storage_cluster_join_mode == ALLOW`" instead of a blanket opt-in
      setting. Not started — deferred until the full-suite re-validation lands.
  - Given §3.2's finding, this condition needs to cover *both* the
    `PlannerJoinTree.cpp` wrap guard and the `QueryAnalyzer.cpp`
    `TableFunctionsWithClusterAlternativesVisitor` check, not just the former.
  - If §3.4's hypothesis for q21 pans out, this condition is a third,
    structurally different code path (parallel-replicas candidate-finder-based,
    not `PlannerJoinTree.cpp`-wrap-based) and needs its own eligibility design,
    not just a flag.
- [ ] Remove the `DLPR`/`OSC_STAGE`/`CLUSTER_ALT`/`PR_CLUSTER_DIAG` diagnostic
      logging before any production-quality version.
- [x] The state described through §3.2b (root cause + CTE fixes, no q21 work
      yet) was committed to git as a single PoC commit
      (`fix/antalya-26.6/query-plan-aggregation-perf`, not pushed to the
      remote). Everything from §3.4 through §3.7 (`PR_CLUSTER_DIAG` diagnostic,
      the §3.5 whole-query dispatch prototype, the §3.6 cluster-name/driver-
      rewrite fixes, and the §3.7 exact-QueryTree-replacement redesign +
      four live bug fixes) is captured in a second PoC commit on the same
      branch, per §3.7.
- [x] q21 real execution, full topology: confirmed live on `arm64-v17` —
      `MergingAggregated -> ReadFromCluster`, checksum stable
      (`55aface52f64`), hot `8.930s` vs. the published pre-experiment baseline's
      `18.203s` (~51% faster). See §3.7.
- [x] Full 23-query IcebergBench suite re-run against `arm64-v17` with
      `object_storage_cluster_bypass_join_wrap=1` set for the whole run — all
      23 `ok`, no correctness mismatches. This closes the very first open item
      in this section (re-run the full suite), just four `arm64-v*` tags later
      than originally hoped, and against `v17` rather than `v9` since three
      more live bugs turned up along the way (§3.7).
- [ ] **New, not yet root-caused: hot-latency regression on q2/q4/q13** (q16,
      the fourth query with the same CTE shape, is *not* regressed — came back
      slightly faster than baseline). q2 `1.002s → 3.285s` (3.3×), q4
      `1.623s → 6.510s` (4.0×), q13 `1.176s → 4.290s` (3.6×), vs. the published
      Antalya 26.6 baseline in
      [`BENCHMARK.md`](https://github.com/Altinity/IcebergBench/blob/main/BENCHMARK.md).
      Working hypothesis (§3.7): `object_storage_cluster_bypass_join_wrap=1`'s
      side effect on the pre-existing, unrelated `QueryAnalyzer.cpp` §3.2 check
      may be letting some other table inside these queries (suspect: `appinfo`
      inside q2's `appinfo_d`, a window-function CTE that must fully
      materialize on one node regardless) go through a distributed/task-
      iterator read it gets no fan-out benefit from. Not yet confirmed with
      `DLPR`/`OSC_STAGE` log evidence — next action before any further code
      change.
- [ ] Review issues #2 (reuse the full dummy-plan-walk eligibility check from
      `findQueryForParallelReplicas()`, not just the structural traversal) and
      #3 (pull `column_names`/`filter_actions_dag` from the driver's
      `TableExpressionData` instead of `AllPhysical` + unset) are unchanged
      from §3.5/§3.7 — still open, still believed correctness-safe
      (workers apply the real `WHERE`) but not minimal (no initiator-side
      Iceberg file/partition pruning for the driver). Likely candidate to
      revisit once the q2/q4/q13 regression is understood, since a missing
      filter DAG could plausibly compound with it.

---

## 10. Full diff (as of `arm64-v11` — superseded by §3.7's redesign; run `git diff` on
## `fix/antalya-26.6/query-plan-aggregation-perf` for the current state)

```diff
diff --git a/src/Analyzer/Resolve/QueryAnalyzer.cpp b/src/Analyzer/Resolve/QueryAnalyzer.cpp
index e2cf9848905..956d8ae54ff 100644
--- a/src/Analyzer/Resolve/QueryAnalyzer.cpp
+++ b/src/Analyzer/Resolve/QueryAnalyzer.cpp
@@ -38,6 +38,7 @@
 
 #include <Common/FieldVisitorToString.h>
 #include <Common/quoteString.h>
+#include <Common/logger_useful.h>
 #include <Core/Settings.h>
 
 #include <Parsers/ASTSelectQuery.h>
@@ -104,6 +105,7 @@ namespace Setting
     extern const SettingsString implicit_table_at_top_level;
     extern const SettingsBool parallel_replicas_for_cluster_engines;
     extern const SettingsBool enable_identifier_resolve_cache;
+    extern const SettingsBool object_storage_cluster_bypass_join_wrap;
 }
 
 
@@ -6212,7 +6214,33 @@ void QueryAnalyzer::resolveQuery(const QueryTreeNodePtr & query_node, Identifier
 
     TableFunctionsWithClusterAlternativesVisitor table_function_visitor;
     table_function_visitor.visit(query_node);
-    if (!table_function_visitor.shouldReplaceWithClusterAlternatives())
+
+    /// EXPERIMENTAL (see object_storage_cluster_bypass_join_wrap in PlannerJoinTree.cpp): root cause found for
+    /// the Iceberg-JOIN-pushdown experiment -- TableFunctionsWithClusterAlternativesVisitor unconditionally
+    /// excludes any query containing a JOIN (has_join) from being eligible for cluster alternatives, forcibly
+    /// resetting parallel_replicas_for_cluster_engines to false regardless of what the query's own SETTINGS
+    /// clause requested. That happens here, upstream of everything in PlannerJoinTree.cpp/IStorageCluster.cpp/
+    /// DatabaseDataLake.cpp.
+    ///
+    /// A blanket bypass of the whole check (ignoring subquery_count/table_count/table_function_count too, not
+    /// just has_join) regressed unrelated benchmark queries with CTEs (e.g. `Unknown table expression
+    /// identifier 'appinfo_d' in scope`) -- those other restrictions are protecting real things. So this only
+    /// lifts the has_join prohibition specifically, keeping every other restriction intact.
+    const bool experimental_allow_join = query_node_typed.getMutableContext()->getSettingsRef()[Setting::object_storage_cluster_bypass_join_wrap]
+        && table_function_visitor.shouldReplaceWithClusterAlternativesIgnoringJoin();
+
+    LOG_WARNING(
+        getLogger("QueryAnalyzer"),
+        "CLUSTER_ALT tables={} table_functions={} subqueries={} has_join={} normal={} ignoring_join={} experimental_allow_join={}",
+        table_function_visitor.getTableCount(),
+        table_function_visitor.getTableFunctionCount(),
+        table_function_visitor.getSubqueryCount(),
+        table_function_visitor.hasJoin(),
+        table_function_visitor.shouldReplaceWithClusterAlternatives(),
+        table_function_visitor.shouldReplaceWithClusterAlternativesIgnoringJoin(),
+        experimental_allow_join);
+
+    if (!table_function_visitor.shouldReplaceWithClusterAlternatives() && !experimental_allow_join)
         query_node_typed.getMutableContext()->setSetting("parallel_replicas_for_cluster_engines", false);
 
     /// Disable cache during join tree resolution - table expressions aren't fully initialized yet,
diff --git a/src/Analyzer/Resolve/TableFunctionsWithClusterAlternativesVisitor.h b/src/Analyzer/Resolve/TableFunctionsWithClusterAlternativesVisitor.h
index 6d898e33493..19ff3f822cd 100644
--- a/src/Analyzer/Resolve/TableFunctionsWithClusterAlternativesVisitor.h
+++ b/src/Analyzer/Resolve/TableFunctionsWithClusterAlternativesVisitor.h
@@ -27,9 +27,22 @@ public:
 
     bool shouldReplaceWithClusterAlternatives() const
     {
-        return subquery_count <= 1 && !has_join && ((table_count + table_function_count) == 1 || (table_function_count == 0));
+        return !has_join && shouldReplaceWithClusterAlternativesIgnoringJoin();
     }
 
+    /// EXPERIMENTAL (see object_storage_cluster_bypass_join_wrap in PlannerJoinTree.cpp): same restrictions
+    /// as shouldReplaceWithClusterAlternatives() except the has_join prohibition, for the narrow opt-in case
+    /// of a direct DataLake-catalog JOIN where every table expression is resolvable on all cluster nodes.
+    bool shouldReplaceWithClusterAlternativesIgnoringJoin() const
+    {
+        return subquery_count <= 1 && ((table_count + table_function_count) == 1 || (table_function_count == 0));
+    }
+
+    size_t getTableCount() const { return table_count; }
+    size_t getTableFunctionCount() const { return table_function_count; }
+    size_t getSubqueryCount() const { return subquery_count; }
+    bool hasJoin() const { return has_join; }
+
 private:
     size_t table_count = 0;
     size_t table_function_count = 0;
diff --git a/src/Core/Settings.cpp b/src/Core/Settings.cpp
index 9a5021a06b1..f199dd6b12f 100644
--- a/src/Core/Settings.cpp
+++ b/src/Core/Settings.cpp
@@ -2138,6 +2138,22 @@ Possible values:
 - `local` — Replaces the database and table in the subquery with local ones for the destination server (shard), leaving the normal `IN`/`JOIN.`
 - `global` — Replaces the `IN`/`JOIN` query with `GLOBAL IN`/`GLOBAL JOIN.` Right table executes first and is added to the secondary query as temporay table.
 - `allow` — Default value. Allows the use of these types of subqueries.
+)", 0) \
+    DECLARE(Bool, object_storage_cluster_bypass_join_wrap, false, R"(
+Experimental. When enabled, disables the safety guard in the query planner that
+normally wraps a leftmost `IStorageCluster` table expression (e.g. `s3Cluster`,
+`icebergCluster`, or a DataLake table converted to `StorageObjectStorageCluster`
+under parallel replicas) into a subquery whenever the query has multiple table
+expressions (i.e. a JOIN). With this setting enabled, such a source may receive
+the full JOIN query and, together with `object_storage_cluster_join_mode='allow'`,
+execute the JOIN and partial aggregation on the remote workers instead of pulling
+all probe-side rows back to the initiator.
+
+This is only safe when every table expression in the query is resolvable on all
+remote nodes of the cluster (e.g. tables backed by the same shared catalog). It is
+not safe in general for `IStorageCluster` sources whose remote nodes may not have
+access to the other tables referenced by the JOIN, which is why the guard exists
+and is enabled by default (`object_storage_cluster_bypass_join_wrap=false`).
 )", 0) \
     \
     DECLARE(UInt64, max_concurrent_queries_for_all_users, 0, R"(
diff --git a/src/Core/SettingsChangesHistory.cpp b/src/Core/SettingsChangesHistory.cpp
index f1b2bab3ac3..a105b1c10f9 100644
--- a/src/Core/SettingsChangesHistory.cpp
+++ b/src/Core/SettingsChangesHistory.cpp
@@ -42,6 +42,7 @@ const VersionToSettingsChangesMap & getSettingsChangesHistory()
         addSettingsChanges(settings_changes_history, "26.6.2.20001.altinityantalya",
         {
             {"use_puffin_files_cache", false, true, "Enables cache of parsed Puffin file content such as deletion vectors."},
+            {"object_storage_cluster_bypass_join_wrap", false, false, "Experimental setting to allow IStorageCluster sources (e.g. Iceberg StorageObjectStorageCluster) to receive the full JOIN query instead of being wrapped in a subquery, so JOIN + partial aggregation can run on parallel-replica workers."},
         });
 
         addSettingsChanges(settings_changes_history, "26.6",
diff --git a/src/Databases/DataLake/DatabaseDataLake.cpp b/src/Databases/DataLake/DatabaseDataLake.cpp
index ba2fc89c779..811c853ef4d 100644
--- a/src/Databases/DataLake/DatabaseDataLake.cpp
+++ b/src/Databases/DataLake/DatabaseDataLake.cpp
@@ -751,6 +751,22 @@ StoragePtr DatabaseDataLake::tryGetTableImpl(const String & name, ContextPtr con
     if (cluster_name.empty() && can_use_parallel_replicas && !is_secondary_query)
         cluster_name = parallel_replicas_cluster_name;
 
+    /// EXPERIMENTAL diagnostic (see object_storage_cluster_bypass_join_wrap in PlannerJoinTree.cpp): trace
+    /// why can_use_parallel_replicas may end up false for a DataLake table under a JOIN.
+    LOG_WARNING(
+        log,
+        "DLPR table={} cluster_for_pr='{}' parallel_cluster_engines={} "
+        "can_task_pr={} is_distributed={} query_kind={} "
+        "can_use_pr={} final_cluster='{}'",
+        name,
+        parallel_replicas_cluster_name,
+        static_cast<bool>(query_settings[Setting::parallel_replicas_for_cluster_engines]),
+        context_->canUseTaskBasedParallelReplicas(),
+        context_->isDistributed(),
+        static_cast<int>(context_->getClientInfo().query_kind),
+        can_use_parallel_replicas,
+        cluster_name);
+
     auto storage_cluster = std::make_shared<StorageObjectStorageCluster>(
         cluster_name,
         configuration,
diff --git a/src/Planner/Planner.cpp b/src/Planner/Planner.cpp
index b36bc3b00cd..a017e19d55f 100644
--- a/src/Planner/Planner.cpp
+++ b/src/Planner/Planner.cpp
@@ -1919,6 +1919,14 @@ Planner::Planner(const QueryTreeNodePtr & query_tree_,
             findTableUnionForParallelReplicas(query_tree, select_query_options),
             collectFiltersForAnalysis(query_tree, select_query_options, post_filter_))))
 {
+    /// EXPERIMENTAL, DIAGNOSTIC ONLY (see object_storage_cluster_bypass_join_wrap): no-op unless that setting
+    /// is enabled; purely observes (via LOG_WARNING) whether the parallel-replicas candidate traversal above
+    /// -- run again here with eligibility relaxed to IStorageCluster -- would have found a distributable
+    /// whole-query candidate and driver table the same way it does for MergeTree. Does not affect
+    /// planner_context or query execution. Restricted to the outermost (non-subquery) Planner invocation
+    /// inside the function itself, since this constructor also runs for each recursively-planned subquery.
+    if (!select_query_options.only_analyze)
+        logObjectStorageClusterParallelReplicasCandidate(query_tree, planner_context->getQueryContext(), select_query_options);
 }
 
 Planner::Planner(const QueryTreeNodePtr & query_tree_,
@@ -2297,6 +2305,17 @@ void Planner::buildPlanForQueryNode()
     {
         join_tree_query_plan = buildQueryPlanForParallelReplicas(query_node, planner_context, select_query_info.storage_limits);
     }
+    else if (const TableNode * object_storage_cluster_driver = select_query_options.only_analyze
+                 ? nullptr
+                 : findObjectStorageClusterWholeQueryDriver(query_node, query_context))
+    {
+        /// EXPERIMENTAL PROTOTYPE (see object_storage_cluster_bypass_join_wrap, §3.4 in
+        /// ICEBERG_JOIN_EXPERIMENT.md): whole-query dispatch for an IStorageCluster driver nested below a
+        /// CTE/subquery (e.g. IcebergBench q21) that PlannerJoinTree.cpp's existing leftmost-table bypass
+        /// (§3.1) can't reach -- analogous to the MergeTree branch above, but routed through the driver's own
+        /// IStorageCluster::read()/ReadFromCluster path rather than ClusterProxy::executeQueryWithParallelReplicas.
+        join_tree_query_plan = buildQueryPlanForObjectStorageCluster(query_node, *object_storage_cluster_driver, planner_context);
+    }
     else
     {
         auto top_level_identifiers = collectTopLevelColumnIdentifiers(query_tree, planner_context);
diff --git a/src/Planner/PlannerJoinTree.cpp b/src/Planner/PlannerJoinTree.cpp
index dd4ef0a462a..cf5329f4e59 100644
--- a/src/Planner/PlannerJoinTree.cpp
+++ b/src/Planner/PlannerJoinTree.cpp
@@ -119,6 +119,7 @@ namespace Setting
     extern const SettingsFloat max_streams_to_max_threads_ratio;
     extern const SettingsMaxThreads max_threads;
     extern const SettingsUInt64 max_threads_min_free_memory_per_thread;
+    extern const SettingsBool object_storage_cluster_bypass_join_wrap;
     extern const SettingsBool optimize_sorting_by_input_stream_properties;
     extern const SettingsBool optimize_trivial_count_query;
     extern const SettingsUInt64 parallel_replicas_count;
@@ -2108,7 +2109,15 @@ JoinTreeQueryPlan buildJoinTreeQueryPlan(const QueryTreeNodePtr & query_node,
     bool should_wrap_left_table = false;
     bool has_multiple_tables = table_expressions_stack.size() > 1;
 
-    if (has_multiple_tables)
+    /// EXPERIMENTAL A/B SETTING (not for production): bypasses the IStorageCluster wrapping guard
+    /// so object-storage-cluster sources (e.g. StorageObjectStorageCluster / Iceberg) can receive
+    /// the full JOIN query and execute JOIN + partial aggregation on the workers instead of the
+    /// initiator. See object_storage_cluster_join_mode in IStorageCluster.cpp for the remote-side
+    /// handling. Off by default; enable per-query with SETTINGS object_storage_cluster_bypass_join_wrap=1.
+    bool experimental_bypass_cluster_join_wrap =
+        planner_context->getQueryContext()->getSettingsRef()[Setting::object_storage_cluster_bypass_join_wrap];
+
+    if (has_multiple_tables && !experimental_bypass_cluster_join_wrap)
     {
         // Get the actual storage to check its type
         auto * table_node = left_table_expression->as<TableNode>();
@@ -2220,6 +2229,14 @@ JoinTreeQueryPlan buildJoinTreeQueryPlan(const QueryTreeNodePtr & query_node,
 
             /** If table expression is remote and it is not left most table expression, we wrap read columns from such
               * table expression in subquery.
+              *
+              * REVERTED experimental bypass here (see git history): the wrap turned out to be doing useful
+              * required-column preservation for the RHS lookup table. Bypassing it dropped JOIN-only key
+              * columns (e.g. city_ascii/state_ascii/iso2) once the RHS's read routed through
+              * IStorageCluster::read()'s cluster-forwarding branch (see IStorageCluster.cpp:453 --
+              * unconditional on cluster_name being non-empty, independent of processed_stage). The actual
+              * fix belongs upstream of this wrap decision: make sure the RHS's resolved cluster name stays
+              * empty (readFallBackToPure(), not ReadFromCluster) rather than skipping this wrap.
               */
             bool is_remote = planner_context->getTableExpressionDataOrThrow(table_expression).isRemote();
             query_plans_stack.push_back(buildQueryPlanForTableExpression(
diff --git a/src/Planner/findParallelReplicasQuery.cpp b/src/Planner/findParallelReplicasQuery.cpp
index cea8427c3e8..5ceb10efe5d 100644
--- a/src/Planner/findParallelReplicasQuery.cpp
+++ b/src/Planner/findParallelReplicasQuery.cpp
@@ -4,6 +4,7 @@
 #include <Analyzer/QueryNode.h>
 #include <Analyzer/TableNode.h>
 #include <Analyzer/UnionNode.h>
+#include <Common/logger_useful.h>
 #include <Core/Settings.h>
 #include <Interpreters/ClusterProxy/SelectStreamFactory.h>
 #include <Interpreters/ClusterProxy/executeQuery.h>
@@ -17,12 +18,16 @@
 #include <Processors/QueryPlan/JoinStep.h>
 #include <Processors/QueryPlan/JoinStepLogical.h>
 #include <Processors/QueryPlan/SortingStep.h>
+#include <Storages/ColumnsDescription.h>
+#include <Storages/IStorageCluster.h>
 #include <Storages/MergeTree/MergeTreeData.h>
+#include <Storages/SelectQueryInfo.h>
 #include <Storages/StorageDummy.h>
 #include <Storages/StorageMaterializedView.h>
 #include <Storages/StorageView.h>
 #include <Storages/buildQueryTreeForShard.h>
 #include <Storages/removeGroupingFunctionSpecializations.h>
+#include <stack>
 
 namespace DB
 {
@@ -33,6 +38,9 @@ namespace Setting
     extern const SettingsBool parallel_replicas_allow_materialized_views;
     extern const SettingsBool serialize_query_plan;
     extern const SettingsBool parallel_replicas_allow_view_over_mergetree;
+    extern const SettingsBool object_storage_cluster_bypass_join_wrap;
+    extern const SettingsNonZeroUInt64 max_block_size;
+    extern const SettingsString cluster_for_parallel_replicas;
 }
 
 namespace ErrorCodes
@@ -522,6 +530,207 @@ const TableNode * findTableForParallelReplicas(const QueryTreeNodePtr & query_tr
     return findTableForParallelReplicas(query_tree_node.get(), context);
 }
 
+namespace
+{
+
+/// DIAGNOSTIC ONLY (see object_storage_cluster_bypass_join_wrap in PlannerJoinTree.cpp / IStorageCluster.cpp):
+/// self-contained copies of getSupportingParallelReplicasQueries()/findTableForParallelReplicas() above, with
+/// eligibility relaxed from "MergeTree" to "IStorageCluster-derived" (e.g. Iceberg StorageObjectStorageCluster).
+/// Used purely to observe -- via logging -- whether the existing whole-query/leftmost-table candidate-finding
+/// traversal (which already sees through CTEs/subqueries, unlike PlannerJoinTree's buildJoinTreeQueryPlan) would
+/// identify a multi-CTE query like IcebergBench's q21 as a distributable candidate and its leftmost table as the
+/// driver, the way it already does for MergeTree. Deliberately NOT wired into GlobalPlannerContext / real
+/// execution -- buildQueryPlanForParallelReplicas() below is MergeTree/task-based-parallel-replicas machinery
+/// (ClusterProxy::executeQueryWithParallelReplicas) and would not be correct for an IStorageCluster driver.
+
+bool isObjectStorageClusterTable(const IQueryTreeNode & table_node_untyped)
+{
+    const auto & table_node = table_node_untyped.as<const TableNode &>();
+    return dynamic_cast<const IStorageCluster *>(table_node.getStorage().get()) != nullptr;
+}
+
+std::vector<const QueryNode *> getSupportingObjectStorageClusterQueriesForDiagnostic(const IQueryTreeNode * query_tree_node)
+{
+    std::vector<const QueryNode *> res;
+
+    while (query_tree_node)
+    {
+        switch (query_tree_node->getNodeType())
+        {
+            case QueryTreeNodeType::TABLE:
+            {
+                if (isObjectStorageClusterTable(*query_tree_node))
+                    return res;
+                return {};
+            }
+            case QueryTreeNodeType::TABLE_FUNCTION:
+            {
+                return {};
+            }
+            case QueryTreeNodeType::QUERY:
+            {
+                const auto & query_node_to_process = query_tree_node->as<QueryNode &>();
+                query_tree_node = query_node_to_process.getJoinTree().get();
+                res.push_back(&query_node_to_process);
+                break;
+            }
+            case QueryTreeNodeType::UNION:
+            {
+                const auto & union_node = query_tree_node->as<UnionNode &>();
+                const auto & union_queries = union_node.getQueries().getNodes();
+                if (union_queries.empty())
+                    return {};
+                query_tree_node = union_queries.front().get();
+                break;
+            }
+            case QueryTreeNodeType::ARRAY_JOIN:
+            {
+                const auto & array_join_node = query_tree_node->as<ArrayJoinNode &>();
+                query_tree_node = array_join_node.getTableExpression().get();
+                break;
+            }
+            case QueryTreeNodeType::CROSS_JOIN:
+            {
+                return {};
+            }
+            case QueryTreeNodeType::JOIN:
+            {
+                const auto & join_node = query_tree_node->as<JoinNode &>();
+                const auto join_kind = join_node.getKind();
+                const auto join_strictness = join_node.getStrictness();
+                std::unordered_set<QueryTreeNodeType> supported_table_expression_types
+                    = {QueryTreeNodeType::TABLE, QueryTreeNodeType::QUERY, QueryTreeNodeType::UNION};
+
+                if (join_kind == JoinKind::Left || (join_kind == JoinKind::Inner && join_strictness == JoinStrictness::All))
+                    query_tree_node = join_node.getLeftTableExpression().get();
+                else if (join_kind == JoinKind::Right && join_strictness != JoinStrictness::RightAny
+                    && supported_table_expression_types.contains(join_node.getLeftTableExpression()->getNodeType()))
+                    query_tree_node = join_node.getRightTableExpression().get();
+                else
+                    return {};
+                break;
+            }
+            default:
+                return {};
+        }
+    }
+
+    return res;
+}
+
+const TableNode * findObjectStorageClusterDriverTableForDiagnostic(const IQueryTreeNode * query_tree_node)
+{
+    std::stack<const IQueryTreeNode *> join_nodes;
+    while (query_tree_node || !join_nodes.empty())
+    {
+        if (!query_tree_node)
+        {
+            query_tree_node = join_nodes.top();
+            join_nodes.pop();
+        }
+
+        switch (query_tree_node->getNodeType())
+        {
+            case QueryTreeNodeType::TABLE:
+            {
+                if (isObjectStorageClusterTable(*query_tree_node))
+                    return &query_tree_node->as<const TableNode &>();
+                query_tree_node = nullptr;
+                break;
+            }
+            case QueryTreeNodeType::TABLE_FUNCTION:
+            {
+                query_tree_node = nullptr;
+                break;
+            }
+            case QueryTreeNodeType::QUERY:
+            {
+                const auto & query_node_to_process = query_tree_node->as<QueryNode &>();
+                query_tree_node = query_node_to_process.getJoinTree().get();
+                break;
+            }
+            case QueryTreeNodeType::UNION:
+            {
+                const auto & union_node = query_tree_node->as<UnionNode &>();
+                const auto & union_queries = union_node.getQueries().getNodes();
+                query_tree_node = nullptr;
+                if (!union_queries.empty())
+                    query_tree_node = union_queries.front().get();
+                break;
+            }
+            case QueryTreeNodeType::ARRAY_JOIN:
+            {
+                const auto & array_join_node = query_tree_node->as<ArrayJoinNode &>();
+                query_tree_node = array_join_node.getTableExpression().get();
+                break;
+            }
+            case QueryTreeNodeType::CROSS_JOIN:
+            {
+                return nullptr;
+            }
+            case QueryTreeNodeType::JOIN:
+            {
+                const auto & join_node = query_tree_node->as<JoinNode &>();
+                const auto join_kind = join_node.getKind();
+
+                if (join_kind == JoinKind::Left || (join_kind == JoinKind::Inner && join_node.getStrictness() == JoinStrictness::All))
+                {
+                    query_tree_node = join_node.getLeftTableExpression().get();
+                    join_nodes.push(join_node.getRightTableExpression().get());
+                }
+                else if (join_kind == JoinKind::Right)
+                {
+                    query_tree_node = join_node.getRightTableExpression().get();
+                    join_nodes.push(join_node.getLeftTableExpression().get());
+                }
+                else
+                {
+                    return nullptr;
+                }
+                break;
+            }
+            default:
+                return nullptr;
+        }
+    }
+
+    return nullptr;
+}
+
+}
+
+void logObjectStorageClusterParallelReplicasCandidate(
+    const QueryTreeNodePtr & query_tree_node, const ContextPtr & context, const SelectQueryOptions & select_query_options)
+{
+    if (!context->getSettingsRef()[Setting::object_storage_cluster_bypass_join_wrap])
+        return;
+
+    /// This constructor also runs for the recursive per-subquery Planner instances that plan each table
+    /// expression independently (see buildJoinTreeQueryPlan()'s `Planner subquery_planner(...)`), not only for
+    /// the outermost query. Only the outermost query's traversal is meaningful for "does the whole query,
+    /// CTEs included, reach the driver table" -- a recursive subquery's own Planner only ever sees its own
+    /// already-flattened-out subtree, so it trivially reports candidates=1 for itself.
+    if (select_query_options.is_subquery)
+        return;
+
+    auto * query_node = query_tree_node->as<QueryNode>();
+    auto * union_node = query_tree_node->as<UnionNode>();
+    if (!query_node && !union_node)
+        return;
+
+    auto candidates = getSupportingObjectStorageClusterQueriesForDiagnostic(query_tree_node.get());
+    const TableNode * driver_table = findObjectStorageClusterDriverTableForDiagnostic(query_tree_node.get());
+
+    LOG_WARNING(
+        getLogger("ParallelReplicasClusterDiag"),
+        "PR_CLUSTER_DIAG depth={} is_subquery={} candidates={} outermost_candidate_is_top_query={} driver_table={}",
+        select_query_options.subquery_depth,
+        select_query_options.is_subquery,
+        candidates.size(),
+        (!candidates.empty() && candidates.front() == query_tree_node.get()),
+        driver_table ? driver_table->getStorageID().getFullTableName() : "<none>");
+}
+
 /// Walk the query tree looking for a UNION node whose every child query
 /// ultimately reads from a table eligible for parallel replicas.
 /// Returns the first such UNION node, or nullptr if none found.
@@ -661,4 +870,141 @@ JoinTreeQueryPlan buildQueryPlanForParallelReplicas(
     return {std::move(query_plan), std::move(processed_stage), {}, {}, {}};
 }
 
+namespace
+{
+
+/// True when query_node's JOIN tree, walked leftmost-first through any number of JOINs, reaches a TABLE or
+/// TABLE_FUNCTION directly -- i.e. the driving table (if any) is exactly the shape PlannerJoinTree.cpp's
+/// existing should_wrap_left_table bypass (see IStorageCluster-JOIN-pushdown experiment, PlannerJoinTree.cpp
+/// §3.1) already handles. Used to make sure the whole-query dispatch below only takes over for the shape it
+/// doesn't handle -- the driver nested at least one CTE/subquery level down (e.g. q21) -- and never second-
+/// guesses the already-validated flat-JOIN case (e.g. q17).
+bool isObjectStorageClusterDriverImmediateLeftmostTableExpression(const QueryNode & query_node)
+{
+    const IQueryTreeNode * join_tree_node = query_node.getJoinTree().get();
+    if (!join_tree_node)
+        return false;
+
+    while (join_tree_node->getNodeType() == QueryTreeNodeType::JOIN)
+        join_tree_node = join_tree_node->as<const JoinNode &>().getLeftTableExpression().get();
+
+    auto node_type = join_tree_node->getNodeType();
+    return node_type == QueryTreeNodeType::TABLE || node_type == QueryTreeNodeType::TABLE_FUNCTION;
+}
+
+}
+
+/// EXPERIMENTAL PROTOTYPE (see object_storage_cluster_bypass_join_wrap): returns the driving IStorageCluster
+/// table for query_node's whole-query dispatch (see buildQueryPlanForObjectStorageCluster below), or nullptr
+/// if this query isn't eligible. Reuses the exact traversal already proven live against IcebergBench q21 by
+/// logObjectStorageClusterParallelReplicasCandidate() (§3.4) -- the diagnostic functions above are now also
+/// used for real here, not just for logging.
+const TableNode * findObjectStorageClusterWholeQueryDriver(const QueryNode & query_node, const ContextPtr & context)
+{
+    if (!context->getSettingsRef()[Setting::object_storage_cluster_bypass_join_wrap])
+        return nullptr;
+
+    if (isObjectStorageClusterDriverImmediateLeftmostTableExpression(query_node))
+        return nullptr;
+
+    auto candidates = getSupportingObjectStorageClusterQueriesForDiagnostic(&query_node);
+    if (candidates.empty() || candidates.front() != &query_node)
+        return nullptr;
+
+    return findObjectStorageClusterDriverTableForDiagnostic(&query_node);
+}
+
+/// EXPERIMENTAL PROTOTYPE (see object_storage_cluster_bypass_join_wrap): sibling of
+/// buildQueryPlanForParallelReplicas() above for an IStorageCluster driver (e.g. Iceberg
+/// StorageObjectStorageCluster) instead of MergeTree. Unlike the MergeTree path, does NOT go through
+/// ClusterProxy::executeQueryWithParallelReplicas (task/part-range-based coordinator, MergeTree-specific);
+/// instead it hands the whole (CTE-inlined) candidate query straight to the driver storage's own
+/// IStorageCluster::read() / ReadFromCluster path -- the same mechanism already proven correct for q17 (see
+/// §2), just invoked here with query_node possibly several CTE/subquery scopes above the driver table (q21's
+/// shape, see §3.4), rather than only when the driver is the immediate JOIN leftmost table
+/// (PlannerJoinTree.cpp's existing bypass, §3.1).
+///
+/// column_names/storage_snapshot passed to read() are the driver's own (used only for
+/// storage_snapshot->check() and SourceStepWithFilter bookkeeping there -- the real output header comes from
+/// re-analyzing the whole query, same as the already-proven q17 ALLOW-mode path); query_info.query/query_tree
+/// are the whole candidate query, serialized via queryNodeToDistributedSelectQuery() so every CTE (including
+/// ones nested further, like q21's policy_matches) is inlined by body rather than referenced by name.
+JoinTreeQueryPlan buildQueryPlanForObjectStorageCluster(
+    const QueryNode & query_node,
+    const TableNode & driver_table_node,
+    const PlannerContextPtr & planner_context)
+{
+    auto processed_stage = QueryProcessingStage::WithMergeableState;
+    auto context = planner_context->getQueryContext();
+
+    QueryTreeNodePtr modified_query_tree = query_node.clone();
+
+    auto initial_header = InterpreterSelectQueryAnalyzer::getSampleBlock(
+        modified_query_tree, context, SelectQueryOptions(processed_stage).analyze());
+
+    auto modified_query_tree_for_ast = modified_query_tree->clone();
+    removeGroupingFunctionSpecializations(modified_query_tree_for_ast);
+    ASTPtr modified_query_ast = queryNodeToDistributedSelectQuery(modified_query_tree_for_ast);
+
+    auto storage = driver_table_node.getStorage();
+    auto storage_snapshot = driver_table_node.getStorageSnapshot();
+    Names column_names = storage_snapshot->getColumns(GetColumnsOptions(GetColumnsOptions::AllPhysical)).getNames();
+
+    SelectQueryInfo query_info;
+    query_info.query = modified_query_ast;
+    query_info.query_tree = modified_query_tree;
+    query_info.planner_context = planner_context;
+
+    /// EXPERIMENTAL (see object_storage_cluster_bypass_join_wrap, §4 in ICEBERG_JOIN_EXPERIMENT.md):
+    /// driver_table_node's StoragePtr was already constructed earlier during analysis, and
+    /// StorageObjectStorageCluster::getClusterName() only ever returns either the `object_storage_cluster`
+    /// session setting or the cluster name baked in at construction time (getOriginalClusterName()) -- which
+    /// may well be empty here, since parallel_replicas_for_cluster_engines could already have been forced
+    /// false for the driver's own resolution scope by an ancestor query's analyzer decision (§3.2), long
+    /// before this whole-query dispatch ever ran. Rather than relaxing that analyzer decision for the whole
+    /// scope (which would make every DataLake table under the query cluster-aware, not just the chosen
+    /// driver), explicitly set `object_storage_cluster` on a context copy scoped to just this read() call, so
+    /// only the driver is dispatched to the cluster -- every other table in the query resolves normally,
+    /// per-worker, from the query text itself.
+    const auto & driver_cluster_name = context->getSettingsRef()[Setting::cluster_for_parallel_replicas].value;
+    if (driver_cluster_name.empty())
+        throw Exception(ErrorCodes::LOGICAL_ERROR,
+            "object_storage_cluster_bypass_join_wrap whole-query dispatch requires cluster_for_parallel_replicas to be set");
+
+    auto driver_context = Context::createCopy(context);
+    driver_context->setSetting("object_storage_cluster", Field(driver_cluster_name));
+
+    QueryPlan query_plan;
+    storage->read(
+        query_plan,
+        column_names,
+        storage_snapshot,
+        query_info,
+        driver_context,
+        processed_stage,
+        context->getSettingsRef()[Setting::max_block_size],
+        /*num_streams*/ 1);
+
+    if (query_plan.isInitialized())
+    {
+        /// Same position-based rename as buildQueryPlanForParallelReplicas() above: the driver's own read()
+        /// resolves the whole query independently, so column names/aliases in its returned header need not
+        /// match the outer query_node's own naming.
+        auto converting = ActionsDAG::makeConvertingActions(
+            query_plan.getCurrentHeader()->getColumnsWithTypeAndName(),
+            initial_header->getColumnsWithTypeAndName(),
+            ActionsDAG::MatchColumnsMode::Position,
+            context,
+            false /*ignore_constant_values*/,
+            false /*add_cast_columns*/,
+            nullptr /*new_names*/);
+
+        auto step = std::make_unique<ExpressionStep>(query_plan.getCurrentHeader(), std::move(converting));
+        step->setStepDescription("Convert object storage cluster whole-query names");
+        query_plan.addStep(std::move(step));
+    }
+
+    return {std::move(query_plan), processed_stage, {}, {}, {}};
+}
+
 }
diff --git a/src/Planner/findQueryForParallelReplicas.h b/src/Planner/findQueryForParallelReplicas.h
index a74459b22ee..874fe0044cc 100644
--- a/src/Planner/findQueryForParallelReplicas.h
+++ b/src/Planner/findQueryForParallelReplicas.h
@@ -33,6 +33,16 @@ bool isTableNodeEligibleForParallelReplicas(const TableNode & table_node, const
 /// Used for views with UNION ALL where each branch reads from a separate MergeTree table.
 const UnionNode * findTableUnionForParallelReplicas(const QueryTreeNodePtr & query_tree_node, const SelectQueryOptions & select_query_options);
 
+/// EXPERIMENTAL, DIAGNOSTIC ONLY (see object_storage_cluster_bypass_join_wrap): logs (LOG_WARNING,
+/// "ParallelReplicasClusterDiag" / "PR_CLUSTER_DIAG") whether the query tree, with eligibility relaxed from
+/// MergeTree to any IStorageCluster-derived storage, would be recognized by the same whole-query/leftmost-table
+/// candidate traversal used for MergeTree parallel replicas -- including seeing through CTEs/subqueries. Does
+/// not affect query execution; no-op unless the experimental setting is enabled. `select_query_options` is only
+/// used for the diagnostic's `depth`/`is_subquery` log fields, to distinguish the outermost query's Planner
+/// invocation from the recursive per-subquery Planner invocations that also run through this constructor.
+void logObjectStorageClusterParallelReplicasCandidate(
+    const QueryTreeNodePtr & query_tree_node, const ContextPtr & context, const SelectQueryOptions & select_query_options);
+
 struct JoinTreeQueryPlan;
 
 class PlannerContext;
@@ -48,4 +58,20 @@ JoinTreeQueryPlan buildQueryPlanForParallelReplicas(
     const PlannerContextPtr & planner_context,
     std::shared_ptr<const StorageLimitsList> storage_limits);
 
+/// EXPERIMENTAL PROTOTYPE (see object_storage_cluster_bypass_join_wrap): returns the IStorageCluster table
+/// (e.g. Iceberg StorageObjectStorageCluster) that drives query_node's whole-query distributed dispatch, or
+/// nullptr if query_node isn't eligible (setting off, no such candidate, or the driver is already handled by
+/// PlannerJoinTree.cpp's existing leftmost-table bypass). See §3.4 in ICEBERG_JOIN_EXPERIMENT.md.
+const TableNode * findObjectStorageClusterWholeQueryDriver(const QueryNode & query_node, const ContextPtr & context);
+
+/// EXPERIMENTAL PROTOTYPE (see object_storage_cluster_bypass_join_wrap): sibling of
+/// buildQueryPlanForParallelReplicas() for an IStorageCluster driver found by
+/// findObjectStorageClusterWholeQueryDriver() -- dispatches the whole (CTE-inlined) query straight to the
+/// driver's own IStorageCluster::read()/ReadFromCluster path instead of MergeTree's
+/// ClusterProxy::executeQueryWithParallelReplicas. See §3.4 in ICEBERG_JOIN_EXPERIMENT.md.
+JoinTreeQueryPlan buildQueryPlanForObjectStorageCluster(
+    const QueryNode & query_node,
+    const TableNode & driver_table_node,
+    const PlannerContextPtr & planner_context);
+
 }
diff --git a/src/Storages/IStorageCluster.cpp b/src/Storages/IStorageCluster.cpp
index 02389f92cf4..0de3ebf5d61 100644
--- a/src/Storages/IStorageCluster.cpp
+++ b/src/Storages/IStorageCluster.cpp
@@ -64,6 +64,7 @@ namespace Setting
     extern const SettingsBool object_storage_remote_initiator;
     extern const SettingsString object_storage_remote_initiator_cluster;
     extern const SettingsObjectStorageClusterJoinMode object_storage_cluster_join_mode;
+    extern const SettingsBool object_storage_cluster_bypass_join_wrap;
 }
 
 namespace ErrorCodes
@@ -375,6 +376,16 @@ void IStorageCluster::read(
     const auto & settings = context->getSettingsRef();
     ASTPtr query_to_send = query_info.query;
 
+    /// EXPERIMENTAL (see object_storage_cluster_bypass_join_wrap in PlannerJoinTree.cpp): when the JOIN
+    /// leftmost-table wrap is bypassed, query_info.query (built via queryNodeToSelectQuery() with
+    /// set_subquery_cte_name=true) may reference a CTE like `appinfo_d` by name only -- CTEs are only defined at
+    /// the top-level query and are not sent to the remote node, causing "Unknown table expression identifier"
+    /// errors there. queryNodeToDistributedSelectQuery() (already used below by
+    /// updateQueryWithJoinToSendIfNeeded() for the LOCAL/GLOBAL join modes) forces every CTE subquery to be
+    /// serialized by its body instead of by name, which is what the remote node needs.
+    if (settings[Setting::object_storage_cluster_bypass_join_wrap] && query_info.query_tree)
+        query_to_send = queryNodeToDistributedSelectQuery(query_info.query_tree);
+
     if (cluster_name_from_settings.empty())
     {
         if (settings[Setting::object_storage_remote_initiator])
@@ -452,11 +463,27 @@ void IStorageCluster::read(
 
     auto cluster = getClusterImpl(context, cluster_name_from_settings, isObjectStorage() ? settings[Setting::object_storage_max_nodes] : 0);
 
-    RestoreQualifiedNamesVisitor::Data data;
-    data.distributed_table = DatabaseAndTableWithAlias(*getTableExpression(query_to_send->as<ASTSelectQuery &>(), 0));
-    data.remote_table.database = context->getCurrentDatabase();
-    data.remote_table.table = getName();
-    RestoreQualifiedNamesVisitor(data).visit(query_to_send);
+    /// EXPERIMENTAL (see object_storage_cluster_bypass_join_wrap in PlannerJoinTree.cpp / §3.4 in
+    /// ICEBERG_JOIN_EXPERIMENT.md): the whole-query dispatch (buildQueryPlanForObjectStorageCluster) may send
+    /// a query_to_send whose first table expression is a subquery -- the driver table is nested below a CTE,
+    /// not this storage's own direct FROM/JOIN reference (e.g. q21's `transaction_event AS te`, not `txnlog`
+    /// itself). RestoreQualifiedNamesVisitor's rewrite only makes sense when the first table expression *is*
+    /// this storage (the shape every other IStorageCluster::read() caller -- s3Cluster, hdfsCluster, and the
+    /// PlannerJoinTree.cpp §3.1 leftmost-table case -- always produces): it rewrites identifiers qualified
+    /// with that table's local alias to be qualified with this storage's own remote name instead. Applied to
+    /// a subquery alias, it would incorrectly rewrite identifiers like `te.transaction_id` as if `te` were a
+    /// direct reference to this storage. Skip it in that case -- every name in query_to_send is already fully
+    /// catalog-qualified by queryNodeToDistributedSelectQuery(), so there is no local-alias-to-remote-name gap
+    /// to bridge for a nested driver.
+    auto * first_table_expression = getTableExpression(query_to_send->as<ASTSelectQuery &>(), 0);
+    if (!first_table_expression->subquery)
+    {
+        RestoreQualifiedNamesVisitor::Data data;
+        data.distributed_table = DatabaseAndTableWithAlias(*first_table_expression);
+        data.remote_table.database = context->getCurrentDatabase();
+        data.remote_table.table = getName();
+        RestoreQualifiedNamesVisitor(data).visit(query_to_send);
+    }
     AddDefaultDatabaseVisitor visitor(context, context->getCurrentDatabase(),
                                       /* only_replace_current_database_function_= */false,
                                       /* only_replace_in_join_= */true);
diff --git a/src/Storages/ObjectStorage/StorageObjectStorageCluster.cpp b/src/Storages/ObjectStorage/StorageObjectStorageCluster.cpp
index 9eefd709aba..248cacfd879 100644
--- a/src/Storages/ObjectStorage/StorageObjectStorageCluster.cpp
+++ b/src/Storages/ObjectStorage/StorageObjectStorageCluster.cpp
@@ -12,6 +12,8 @@
 #include <Core/Settings.h>
 #include <Formats/FormatFactory.h>
 #include <Parsers/ASTSelectQuery.h>
+#include <Parsers/ASTSelectWithUnionQuery.h>
+#include <Parsers/ASTSubquery.h>
 #include <Parsers/ASTTablesInSelectQuery.h>
 #include <Parsers/ASTIdentifier.h>
 #include <Parsers/ASTLiteral.h>
@@ -298,6 +300,78 @@ std::optional<UInt64> StorageObjectStorageCluster::totalBytes(ContextPtr query_c
     return configuration->totalBytes(query_context);
 }
 
+namespace
+{
+
+/// EXPERIMENTAL (see object_storage_cluster_bypass_join_wrap in PlannerJoinTree.cpp / §4 in
+/// ICEBERG_JOIN_EXPERIMENT.md): updateQueryForDistributedEngineIfNeeded() used to always take the query's
+/// first table expression (tables->children[0]) as "the table this storage's read() is being called for".
+/// That holds for every existing caller (s3Cluster, hdfsCluster, and PlannerJoinTree.cpp's §3.1 leftmost-table
+/// bypass), but not for the whole-query dispatch (buildQueryPlanForObjectStorageCluster, §3.5), where the
+/// driver can be nested inside a CTE/subquery and isn't the outer query's own first table expression (e.g.
+/// IcebergBench q21's txnlog, nested inside the transaction_event CTE). Search the whole query (recursing into
+/// subqueries) for the table expression that actually refers to this storage, instead of assuming position 0
+/// -- this also correctly finds it at position 0 for every existing caller, so it's a strict generalization,
+/// not an Iceberg-JOIN-experiment-specific special case.
+ASTTableExpression * findTableExpressionForStorageId(const ASTPtr & node, const StorageID & storage_id)
+{
+    if (!node)
+        return nullptr;
+
+    if (const auto * select_query = node->as<ASTSelectQuery>())
+    {
+        auto tables_ast = select_query->tables();
+        if (!tables_ast)
+            return nullptr;
+
+        auto & tables = tables_ast->as<ASTTablesInSelectQuery &>();
+        for (auto & child : tables.children)
+        {
+            auto * element = child->as<ASTTablesInSelectQueryElement>();
+            if (!element || !element->table_expression)
+                continue;
+
+            auto * table_expression = element->table_expression->as<ASTTableExpression>();
+            if (!table_expression)
+                continue;
+
+            if (table_expression->database_and_table_name)
+            {
+                const auto & identifier = table_expression->database_and_table_name->as<const ASTTableIdentifier &>();
+                const auto identifier_table_id = identifier.getTableId();
+                if (identifier_table_id.table_name == storage_id.table_name
+                    && (identifier_table_id.database_name.empty() || identifier_table_id.database_name == storage_id.database_name))
+                    return table_expression;
+            }
+
+            if (table_expression->subquery)
+            {
+                if (auto * found = findTableExpressionForStorageId(table_expression->subquery, storage_id))
+                    return found;
+            }
+        }
+        return nullptr;
+    }
+
+    if (const auto * subquery = node->as<ASTSubquery>())
+        return subquery->children.empty() ? nullptr : findTableExpressionForStorageId(subquery->children[0], storage_id);
+
+    if (const auto * union_query = node->as<ASTSelectWithUnionQuery>())
+    {
+        if (!union_query->list_of_selects)
+            return nullptr;
+        for (const auto & child : union_query->list_of_selects->children)
+        {
+            if (auto * found = findTableExpressionForStorageId(child, storage_id))
+                return found;
+        }
+    }
+
+    return nullptr;
+}
+
+}
+
 bool StorageObjectStorageCluster::updateQueryForDistributedEngineIfNeeded(ASTPtr & query, ContextPtr context, bool make_cluster_function)
 {
     // Change table engine on table function for distributed request
@@ -319,7 +393,12 @@ bool StorageObjectStorageCluster::updateQueryForDistributedEngineIfNeeded(ASTPtr
             "Expected SELECT query from table with engine {}, got '{}'",
             configuration->getEngineName(), query->formatForLogging());
 
-    auto * table_expression = tables->children[0]->as<ASTTablesInSelectQueryElement>()->table_expression->as<ASTTableExpression>();
+    /// Find the table expression that actually refers to this storage, wherever it sits in the query (see
+    /// findTableExpressionForStorageId() above); fall back to the old position-0 assumption defensively, in
+    /// case the search misses some table-expression shape it doesn't yet handle.
+    auto * table_expression = findTableExpressionForStorageId(query, getStorageID());
+    if (!table_expression)
+        table_expression = tables->children[0]->as<ASTTablesInSelectQueryElement>()->table_expression->as<ASTTableExpression>();
 
     if (!table_expression)
         return false;
@@ -740,6 +819,18 @@ String StorageObjectStorageCluster::getClusterName(ContextPtr context) const
 QueryProcessingStage::Enum StorageObjectStorageCluster::getQueryProcessingStage(
     ContextPtr context, QueryProcessingStage::Enum to_stage, const StorageSnapshotPtr & storage_snapshot, SelectQueryInfo & query_info) const
 {
+    /// EXPERIMENTAL diagnostic (see object_storage_cluster_bypass_join_wrap in PlannerJoinTree.cpp).
+    LOG_WARNING(
+        getLogger("StorageObjectStorageCluster"),
+        "OSC_STAGE table={} original_cluster='{}' resolved_cluster='{}' "
+        "cluster_supported={} query_kind={} to_stage={}",
+        getStorageID().getFullTableName(),
+        getOriginalClusterName(),
+        getClusterName(context),
+        isClusterSupported(),
+        static_cast<int>(context->getClientInfo().query_kind),
+        QueryProcessingStage::toString(to_stage));
+
     if (!isClusterSupported())
         return QueryProcessingStage::Enum::FetchColumns;
 
```

---

## 11. Docker images (all pushed to `docker.io/nerflongshotuv/clickhouse-test`)

| Tag | Contents | Status |
|---|---|---|
| `iceberg-join-bypass-amd64` | Left-wrap bypass only, RHS wrap bypassed (buggy) | superseded |
| `iceberg-join-bypass-arm64` | same as above, arm64 | superseded |
| `iceberg-join-bypass-arm64-v2` | RHS-wrap bypass fix attempt | superseded, reverted |
| `iceberg-join-bypass-arm64-v3` | RHS wrap reverted back to original | superseded |
| `iceberg-join-bypass-arm64-v4` | + `DLPR`/`OSC_STAGE` diagnostics | superseded |
| `iceberg-join-bypass-arm64-v5` | + §3.2 fix (broad version, not yet narrow) | superseded |
| `iceberg-join-bypass-arm64-v6` | + §3.2 narrow fix, + `CLUSTER_ALT` diagnostic | superseded — narrow fix didn't actually exclude q2 (§3.2), and CTE serialization bug was still unfixed |
| `iceberg-join-bypass-arm64-v7` | + `IStorageCluster::read()` CTE fix (`queryNodeToDistributedSelectQuery()` for `query_to_send` when the experimental flag is set) | superseded — didn't yet address q21 (§3.4: driving table is one CTE scope below the JOIN, `PlannerJoinTree.cpp`'s guard never sees it) |
| `iceberg-join-bypass-arm64-v8` | + `PR_CLUSTER_DIAG` diagnostic (§3.4): logs, without touching execution, whether ClickHouse's MergeTree parallel-replicas candidate-finder would (if relaxed to `IStorageCluster`) recognize q21's whole query + CTEs as a distributable candidate down to `txnlog` | superseded — first live run's `candidates=1` line was ambiguous between the outermost Planner invocation and a recursive per-subquery one |
| `iceberg-join-bypass-arm64-v9` | + `depth`/`is_subquery` fields on `PR_CLUSTER_DIAG`, restricted to the outermost (non-subquery) `Planner` invocation only | superseded — confirmed candidate/driver discovery live for q21, diagnostic-only, no real execution |
| `iceberg-join-bypass-arm64-v10` | + first whole-query execution-dispatch prototype (§3.5): `findObjectStorageClusterWholeQueryDriver`/`buildQueryPlanForObjectStorageCluster` in `findParallelReplicasQuery.cpp`, wired into `Planner.cpp`'s dispatch; `IStorageCluster.cpp` qualified-names-restoration skip for a subquery-rooted first table expression | superseded — confirmed live that issue #4 (empty driver cluster name) reproduces, falls back to local |
| **`iceberg-join-bypass-arm64-v11`** | + §3.6: explicit per-call `object_storage_cluster` context override in `buildQueryPlanForObjectStorageCluster()` (reads `cluster_for_parallel_replicas` directly, doesn't touch `QueryAnalyzer.cpp`); `StorageObjectStorageCluster::updateQueryForDistributedEngineIfNeeded()` now finds its own table expression anywhere in the query (recursing into subqueries) instead of assuming position 0 | **current, fixes issue #4 (cluster resolution) and the deeper task-iterator-rewrite gap it uncovered, not yet run live** |
| `bin-runner-amd64` / `bin-runner-arm64` | generic binary-URL-driven runner image, no baked binary | available, unused so far for actual testing |

---

## 12. Redesign: reuse the stock parallel-replicas candidate machinery (`v17` → `v18`)

Review of `v17` (§3.7) found the design too ad hoc: three separate gates spread across
`PlannerJoinTree.cpp`/`QueryAnalyzer.cpp`/`findParallelReplicasQuery.cpp`, each patched
reactively per benchmark query shape (`should_wrap_left_table` bypass,
`canBypassClusterJoinWrapForTableExpressions`, `saw_join`,
`isObjectStorageClusterDriverImmediateLeftmostTableExpression`), plus a bespoke duplicate
traversal (`getSupportingObjectStorageClusterQueriesForDiagnostic`/
`findObjectStorageClusterDriverTableNode`) shadowing ClickHouse's own
`getSupportingParallelReplicasQueries`/`findTableForParallelReplicas`. Root cause: ClickHouse
already decides "should this table be a distributed parallel-replicas driver" in two places for
`MergeTree`, and this experiment never reused either:

1. **Planner-level, per-table, post-analysis** — `GlobalPlannerContext::parallel_replicas_node`/
   `parallel_replicas_table`, populated once at `Planner::Planner()` construction time by
   `findQueryForParallelReplicas()`/`findTableForParallelReplicas()`, both gated by the single
   eligibility predicate `isTableNodeEligibleForParallelReplicas()`. Handles a driver reachable
   only through a CTE/subquery boundary (q21's shape) — `findQueryForParallelReplicas()`
   deliberately returns `nullptr` when the eligible table is already in the *current* query
   scope (`stack.back() == query_tree_node.get()`), because that case is handled by mechanism 2.
2. **`PlannerJoinTree.cpp`, per-JOIN-tree, join-shape-based** —
   `parallelReplicasEnabledForStorage()` (storage eligibility) +
   `allowParallelReplicasForJoinTree()` (join-kind/strictness shape check: LEFT/INNER-ALL follow
   left, qualifying RIGHT follow right, reject CROSS JOIN and non-simple RIGHT JOIN shapes),
   consumed inside `buildQueryPlanForTableExpression()` to swap a plain `ReadFromMergeTree` step
   for `ClusterProxy::executeQueryWithParallelReplicas()`. Handles a driver that is the immediate
   leftmost/rightmost table of the JOIN (q17's shape).

Because `DatabaseDataLake`/`StorageObjectStorageCluster` instead decided cluster-dispatch
*at table-resolution time* via a *query-scope-wide* analyzer flag
(`TableFunctionsWithClusterAlternativesVisitor`'s `has_join` check forcing
`parallel_replicas_for_cluster_engines` off), neither mechanism above ever got a chance to pick
an object-storage-cluster driver on its own — every fix in §3 was a patch trying to stop *other*
tables from being incidentally swept up by that scope-wide flag, not a way of picking the driver.
This is the most likely cause of the still-open q2/q4/q13 hot-latency regression (§9): relaxing
`has_join` lets `DatabaseDataLake`'s pre-existing `can_use_parallel_replicas` fallback bake a
cluster name into *any* DataLake table under a JOIN scope, not just the one actually meant to be
distributed — e.g. `event_app` in q2, which then fans out over the network for no benefit versus
its already-fast single-node read, while `appinfo_d`'s window-function CTE is unrelated and never
should have been touched at all.

### 12.1 Finalized architecture

**Candidate selection** (two entry points, both reusing/generalizing the exact stock
`MergeTree` machinery via a shared, pluggable storage-eligibility predicate — no new traversal):

```
q17 (direct leftmost/rightmost driver)                q21 (driver nested under CTE/subquery)
        PlannerJoinTree.cpp                              findParallelReplicasQuery.cpp
 parallelReplicasEnabledForStorage()                 findQueryForParallelReplicas()
 allowParallelReplicasForJoinTree()                   findTableForParallelReplicas()
        (bound to isObjectStorageClusterDriverEligible)  (via isTableNodeEligibleForParallelReplicas())
                       \                                        /
                        \                                      /
                         v                                    v
                    buildQueryPlanForObjectStorageCluster()  (single execution backend)
```

- `isObjectStorageClusterDriverEligible(storage, context)` (new, `findParallelReplicasQuery.cpp`,
  exported via `findQueryForParallelReplicas.h`): true iff `storage` is a
  `StorageObjectStorageCluster` and the query's settings opt in
  (`object_storage_cluster_bypass_join_wrap`, `parallel_replicas_for_cluster_engines`,
  non-empty `cluster_for_parallel_replicas`). This is the *single* place the opt-in setting is
  checked now (previously checked independently in three files).
- `isTableNodeEligibleForParallelReplicas()` delegates to it first, before its existing
  `MergeTree`-only checks — this alone makes `findQueryForParallelReplicas()`/
  `findTableForParallelReplicas()` recognize an object-storage-cluster driver nested under a
  CTE/subquery, with zero changes to either function's traversal logic.
- `allowParallelReplicasForJoinTree()` is refactored to take the storage-eligibility check as a
  parameter (`ParallelReplicasStorageEligibility`, a `std::function<bool(const StoragePtr&)>`)
  instead of calling `parallelReplicasEnabledForStorage()` directly. The existing `MergeTree`
  call site binds `parallelReplicasEnabledForStorage`, byte-for-byte unchanged behavior; a new
  call site in `buildJoinTreeQueryPlan()` binds `isObjectStorageClusterDriverEligible`, replacing
  `canBypassClusterJoinWrapForTableExpressions()`'s syntactic guard with the same join-shape
  check MergeTree's own dispatch already trusts (rejects CROSS JOIN, non-simple RIGHT JOIN
  shapes, a `VIEW` leftmost table, etc., for free).

**Execution** (one shared backend, unchanged in essence from `v17`'s redesign, just
re-parameterized): `buildQueryPlanForObjectStorageCluster(query_tree, driver_table_node,
select_query_info, planner_context)` — takes the driver as `const TableNode &` rather than a
`QueryTreeNodePtr`, since `IQueryTreeNode::cloneAndReplace()`'s `ReplacementMap` is keyed by raw
pointer (`std::unordered_map<const IQueryTreeNode *, QueryTreeNodePtr>`) — no shared-ptr
ownership of the driver node is actually needed, which is what let this same function be called
directly from both `Planner::buildPlanForQueryNode()` (mechanism 1, with
`GlobalPlannerContext::parallel_replicas_table`, a raw `const TableNode *`) and
`buildJoinTreeQueryPlan()` (mechanism 2, with the leftmost `TableNode` reference) with no
duplicated logic. Internally: `StorageObjectStorageCluster::buildClusterTableFunctionAST()` →
`buildQueryTree()`/`QueryAnalysisPass` → `IQueryTreeNode::cloneAndReplace()` (mirroring
`StorageDistributed::buildQueryTreeDistributed()`'s `remote_table_function` branch) →
`queryNodeToDistributedSelectQuery()` → `IStorageCluster::readPreparedClusterQuery()` →
`ReadFromCluster`. The cluster identity is embedded as a literal argument in the driver's own
`icebergS3Cluster('vig-test', ...)` table-function AST — never as a propagated `Context`
setting — so no other table in the same query text (on the initiator *or* a worker) can pick up
cluster-dispatch behavior it wasn't explicitly selected for.

For q17 specifically: `buildJoinTreeQueryPlan()`'s leftmost-table branch calls
`buildQueryPlanForObjectStorageCluster()` directly in place of
`buildQueryPlanForTableExpression()` when eligible — bypassing `storage->read()` entirely for
that table expression, so `parallelReplicasEnabledForStorage()`'s `MergeTree`-only
`ReadFromMergeTree`-swap block is never reached (not just gated false — genuinely not in the
call path). The existing stock short-circuit
`if (left_table_expression_query_plan.stage != QueryProcessingStage::FetchColumns) return
left_table_expression_query_plan;` then makes the leftmost table's plan own the whole JOIN tree,
exactly the way `Distributed`/`Merge`/parallel-replicas storages already do — no second RHS
planning path needed.

### 12.2 Removed entirely (not just reverted-and-reintroduced elsewhere)

- `Analyzer/Resolve/QueryAnalyzer.cpp` / `TableFunctionsWithClusterAlternativesVisitor.h` — the
  `has_join` relaxation and `CLUSTER_ALT` diagnostic. Reverted to stock byte-for-byte
  (`git checkout 296cce3c390 -- <path>`). No longer needed: cluster dispatch is injected
  explicitly, only for the one selected driver, at execution time — no table's storage object
  needs a baked-in cluster name from this analyzer-level path anymore.
- `PlannerJoinTree.cpp`'s `canBypassClusterJoinWrapForTableExpressions()` — superseded by
  `allowParallelReplicasForJoinTree()` bound to `isObjectStorageClusterDriverEligible`.
- `findParallelReplicasQuery.cpp`'s diagnostic-only duplicate traversal
  (`isObjectStorageClusterTable`, `getSupportingObjectStorageClusterQueriesForDiagnostic`,
  `findObjectStorageClusterDriverTableNode`, `logObjectStorageClusterParallelReplicasCandidate`),
  `isObjectStorageClusterDriverImmediateLeftmostTableExpression`, and
  `findObjectStorageClusterWholeQueryDriver` (including its `saw_join` guard) — all superseded by
  reusing `getSupportingParallelReplicasQueries()`/`findQueryForParallelReplicas()`/
  `findTableForParallelReplicas()` directly. The `saw_join` guard in particular is no longer
  needed: it existed only because the old duplicate traversal ran unguarded against the
  *recursive per-CTE* `Planner` instance for `appinfo_d`'s own body; the stock dummy-plan-walk
  inside `findQueryForParallelReplicas()` already rejects a lone window-function-over-table
  candidate on its own terms (a `WindowStep` isn't Expression/Filter/Join/mergeable-Sorting, and
  with no enclosing JOIN `inside_join` stays false, so it returns `nullptr` outright) — this may
  also be what naturally fixes the q2/q4/q13 regression, since it was never really about the JOIN
  shape at all, see §12.1's root-cause paragraph.
- `Planner.cpp`'s `logObjectStorageClusterParallelReplicasCandidate()` call in the constructor —
  no longer needed once `GlobalPlannerContext::parallel_replicas_node`/`parallel_replicas_table`
  genuinely reflect object-storage-cluster eligibility.
- The considered-and-rejected approach of injecting `object_storage_cluster` on a
  `Context::createCopy()` passed into `storage->read()` for the driver (an earlier draft of this
  redesign, before implementation): that context is what gets serialized to the remote workers,
  so the setting would travel with the query and let a worker's own resolution of an *unrelated*
  DataLake table in the same JOIN (e.g. the RHS lookup table) pick up the same cluster name and
  recursively fan out — reproducing the same scope-wide leakage this whole redesign exists to
  eliminate, just via a different setting. `buildClusterTableFunctionAST()`'s `Context::createCopy`
  is safe by contrast because it's used only to steer local AST construction and is discarded
  before any query ever leaves the initiator.

### 12.3 Kept unchanged

- `IStorageCluster::read()`'s `queryNodeToDistributedSelectQuery()` CTE-serialization fix (§3.2b)
  — genuinely orthogonal to everything above, still needed whenever a query dispatched through
  this path has a CTE anywhere in it.
- `StorageObjectStorageCluster::buildClusterTableFunctionAST()`,
  `IStorageCluster::readPreparedClusterQuery()` — the execution backend from `v17`'s redesign,
  unchanged in mechanism, just re-parameterized (`buildQueryPlanForObjectStorageCluster()` now
  takes `const TableNode &` instead of `QueryTreeNodePtr`, see §12.1).
- The RHS-wrap revert (§3.3) — still correct, untouched.
- Diagnostics `DLPR` (`DatabaseDataLake.cpp`) and `OSC_STAGE` (`StorageObjectStorageCluster.cpp`)
  — kept for one more live validation pass (confirm no cluster-name leakage to non-driver tables
  now that `QueryAnalyzer.cpp` is reverted); `CLUSTER_ALT` and `PR_CLUSTER_DIAG` removed along
  with the code they were observing.

### 12.4 Known gap, unchanged from `v17`

Neither candidate-selection mechanism evaluates the *cost* of redundantly recomputing the
non-driver side of the JOIN on every worker — `allowParallelReplicasForJoinTree()`/
`getSupportingParallelReplicasQueries()` check structural pushability (JOIN kind, whether a step
needs initiator finalization), not cardinality or expense. This happens to exclude `appinfo_d`
(a window function is structurally non-passthrough) but would not exclude a structurally-flat
yet expensive non-driver branch. True cost-based gating remains an open follow-up, not attempted
here — same status as the original review's issue #2 (the full MergeTree dummy-plan-walk was
always structural, never cost-based, either).

### 12.5 Build/validate plan

Implemented directly against `fix/antalya-26.6/query-plan-aggregation-perf` (not yet committed —
see git status once the arm64 build/image lands). Same validation loop as §7-§9: cross-compile
for `arm64` via `build_arm64/`, package into a new `iceberg-join-bypass-arm64-v18` image, push to
`docker.io/nerflongshotuv/clickhouse-test`, then re-run the full 23-query IcebergBench suite with
`object_storage_cluster_bypass_join_wrap=1` — expect q17/q21 to reproduce `v17`'s speedups via the
new shared path, and (the main open question) q2/q4/q13 to no longer regress now that
`QueryAnalyzer.cpp` is untouched and no non-driver table can pick up a cluster name it wasn't
explicitly given.

### 12.6 Bug found before any live run: `isObjectStorageClusterDriverEligible()` checked a setting the
kept stock analyzer code permanently zeroes for any JOIN query (`v18` → `v19`)

`v18` (§12.1-12.5) was built and pushed, but never actually run live -- caught by re-reading the
interaction with the *kept* stock `QueryAnalyzer.cpp` code before testing. `TableFunctionsWithClusterAlternativesVisitor`'s
`has_join` check (untouched, deliberately kept stock -- §12.2) still does exactly what it always did:

```cpp
if (!table_function_visitor.shouldReplaceWithClusterAlternatives())
    query_node_typed.getMutableContext()->setSetting("parallel_replicas_for_cluster_engines", false);
```

`shouldReplaceWithClusterAlternatives()` is `false` for any query with `has_join=true` -- i.e. for
both q17 and q21, unconditionally, before the `Planner` ever runs. `v18`'s
`isObjectStorageClusterDriverEligible()` (`findParallelReplicasQuery.cpp`) checked
`settings[Setting::parallel_replicas_for_cluster_engines]` as part of its gate -- reading that same,
now-forced-`false` setting off the same query-node context. Net effect: with `v18`'s code, **no
`SETTINGS` clause could ever make the new mechanism fire** for a JOIN query -- the eligibility check
always saw `parallel_replicas_for_cluster_engines=false`, regardless of what the query's own
`SETTINGS` requested. This was caught by tracing the setting through before testing, not live.

**Fix (`v19`)**: dropped the `parallel_replicas_for_cluster_engines` check from
`isObjectStorageClusterDriverEligible()` entirely. It was never a meaningful signal for this
mechanism to begin with -- `object_storage_cluster_bypass_join_wrap` (untouched by any analyzer
logic, always reflects exactly what the query's own `SETTINGS` requested) is the correct, sole
opt-in gate, together with a non-empty `cluster_for_parallel_replicas` (needed later to actually
pick the cluster). Confirmed this doesn't weaken any other gate: `Planner.cpp`'s dispatch condition
(`canUseTaskBasedParallelReplicas()`) and `findQueryForParallelReplicas()`'s own gate
(`canUseParallelReplicasOnInitiator()`) check `allow_experimental_parallel_reading_from_replicas`/
`parallel_replicas_mode`/`max_parallel_replicas`/`automatic_parallel_replicas_mode` -- none of which
the `has_join` guard touches. `DatabaseDataLake`'s own pre-existing `parallel_replicas_for_cluster_engines`-driven
fallback (for a plain, non-JOIN DataLake table) is untouched and still governed by that setting as
before -- only the new mechanism's own eligibility check stopped depending on it.

Compiled clean (native + `arm64` sanity checks), rebuilt `build_arm64/`, repackaged and pushed as
`iceberg-join-bypass-arm64-v19`. **Not yet run live** -- this is the next actual test.

**Settings to test with** (same shape as §5, `parallel_replicas_for_cluster_engines` no longer
load-bearing for this mechanism specifically but left set since it still governs other DataLake
tables' normal behavior):

```sql
SETTINGS
    enable_parallel_replicas = 1,
    cluster_for_parallel_replicas = 'vig-test',
    object_storage_cluster_bypass_join_wrap = 1,
    parallel_replicas_for_cluster_engines = 1,
    filesystem_cache_name = 's3_disk_cache',
    use_page_cache_for_object_storage = 1,
    remote_read_min_bytes_for_seek = 1048576
```

### 12.7 First live run against `v19`: `Code: 306. TOO_DEEP_RECURSION` on q21, two more bugs found (`v19` → `v20`)

q21 (run against `v19` with the §12.6 settings) failed immediately with:

```
Code: 306. DB::Exception: Stack size too large. Stack address: 0xffff4dd60000, frame address:
0xffff4e25fd50, stack size: 5243568, maximum stack size: 10485760. (TOO_DEEP_RECURSION)
```

**Bug 1 (the actual cause): mechanism B's new dispatch branch in `buildJoinTreeQueryPlan()` never
checked `select_query_options.only_analyze`.** `buildQueryPlanForObjectStorageCluster()` internally
calls `InterpreterSelectQueryAnalyzer::getSampleBlock()`/`getSampleBlockAndPlannerContext()` on
`query_tree->clone()` purely to compute output headers -- each spins up its own fresh, `only_analyze=true`
`Planner` that re-enters `buildJoinTreeQueryPlan()` on the *same, still-unreplaced* tree (the driver
TableNode hasn't been swapped for the cluster table function yet at the point `getSampleBlock()` is
called). Without an `only_analyze` guard, that recursive analyze-only invocation sees the identical
eligible JOIN shape and calls `buildQueryPlanForObjectStorageCluster()` again, which calls
`getSampleBlock()` again -- unbounded recursion, confirmed as the `TOO_DEEP_RECURSION` above.
Mechanism A got this guard *for free*, because `findQueryForParallelReplicas()`/
`findTableForParallelReplicas()` both early-return `nullptr` under `only_analyze` (so
`GlobalPlannerContext::parallel_replicas_node`/`parallel_replicas_table` are never populated during an
analyze-only Planner construction); mechanism B's new branch had no equivalent check. This
specifically hit q21 because `alert_events`'s own CTE body (`event_alert LEFT JOIN policy_matches`)
is *itself* an eligible immediate-leftmost JOIN, and gets recursively (re-)analyzed while the outer
q21 dispatch (mechanism A, driver `txnlog`) computes its own headers -- but the same bug would equally
hit q17 on its own first analyze pass; it just hadn't been tested yet when this was found.

**Fix**: added `!select_query_options.only_analyze &&` to the mechanism B dispatch condition in
`buildJoinTreeQueryPlan()`, mirroring the `select_query_options.only_analyze ? nullptr : ...` guard
the original (`v17`) leftmost-driver prototype used at its own call site.

**Bug 2 (found while fixing bug 1, not yet observed live -- fixed proactively): `isObjectStorageClusterDriverEligible()`
had no protection against firing on an already-distributed execution.** Once the outer query's driver
is dispatched via `ReadFromCluster`, each worker receives and independently analyzes/executes the
*whole* prepared query text (CTEs inlined by body via `queryNodeToDistributedSelectQuery()`). For
q21 that text still contains `alert_events`'s own `event_alert LEFT JOIN policy_matches` -- a second,
independently eligible JOIN. Settings (`object_storage_cluster_bypass_join_wrap`,
`cluster_for_parallel_replicas`) propagate to the worker like any other setting, so without a guard
the worker's own analysis of that embedded JOIN would find `event_alert` eligible too and try to
dispatch *it* via `buildQueryPlanForObjectStorageCluster()` again -- recursive re-fanout from within
an already-distributed execution, not just redundant work. `DatabaseDataLake::tryGetTableImpl()`
already guards its own (unrelated) parallel-replicas fallback against exactly this with
`!context_->isDistributed() && query_kind != SECONDARY_QUERY`; added the identical guard to
`isObjectStorageClusterDriverEligible()`. A worker executing a query it received via `ReadFromCluster`
is always `isDistributed()==true`/`query_kind==SECONDARY_QUERY`, so this confines whole-query dispatch
to the initiator's own top-level analysis, matching the pre-existing pattern instead of inventing a
new one.

Both fixes compiled clean (native + full relink, `arm64` full rebuild), repackaged and pushed as
`iceberg-join-bypass-arm64-v20`. **Not yet re-run live** -- q21 (and q17, not yet tested at all) are
the next actual test, same settings as §12.6.

### 12.8 Second live run against `v20`: crash gone, but wrong topology (q21) and no distribution at all
(q17) -- two more bugs, both root-caused precisely, not guessed (`v20` → `v21`)

q21 no longer crashed, but `EXPLAIN` showed `Aggregating → Join → [ReadFromCluster (txnlog),
Aggregating → Join → [ReadFromObjectStorage, ReadFromObjectStorage]]` -- only `txnlog` distributed,
`alert_events` still fully local, instead of the target `MergingAggregated → ReadFromCluster`. q17
regressed further: **no** `ReadFromCluster` at all, fully local on both sides.

**Bug 3 (q21's wrong topology): `Planner.cpp`'s dispatch used `GlobalPlannerContext::parallel_replicas_table`
to pick the execution backend, but that field is `nullptr` on a normal initiator by design.** Diagnosed
precisely by the reviewer (not guessed): the public `findTableForParallelReplicas()` overload is gated by
`serialize_query_plan || context->canUseParallelReplicasOnFollower()` -- a gate meant for
`PlannerJoinTree.cpp`'s View/MaterializedView follower-recursion-safety check
(`no_tables_or_another_table_chosen_for_reading_with_parallel_replicas_mode`), which is only meaningful on a
*follower*. `canUseParallelReplicasOnFollower()` is `canUseTaskBasedParallelReplicas() && collaborate_with_initiator`,
always false on the initiator itself -- so `parallel_replicas_table` is always `nullptr` there, and
`Planner.cpp`'s `if (driver_table && dynamic_cast<StorageObjectStorageCluster*>(...))` always fell through
to the `else` branch (`buildQueryPlanForParallelReplicas()`, the MergeTree/`ClusterProxy::executeQueryWithParallelReplicas`
backend). That backend didn't outright fail for `txnlog` because it re-derives its own driver internally via
the *private*, ungated `findTableForParallelReplicas(modified_query_tree.get(), context)` overload (which
now finds `txnlog` fine, since `isTableNodeEligibleForParallelReplicas()` accepts `StorageObjectStorageCluster`)
-- but it dispatches through the wrong coordinator (MergeTree part-range assignment, not the object-storage
task iterator) and runs `rewriteJoinToGlobalJoin()`/`buildQueryTreeForShard()` (MergeTree-shard-rewriting),
which is why `alert_events` ended up wrapped as an ordinary local `GLOBAL JOIN` instead of being absorbed
into the same dispatched query text.

**Fix**: added a new, purpose-built initiator-safe lookup, `findParallelReplicasCandidateDriver(candidate)`
(`findParallelReplicasQuery.cpp`/`.h`) -- explicitly *not* a modification of the public
`findTableForParallelReplicas()` (its follower-only gate is correct and load-bearing for
`PlannerJoinTree.cpp`'s existing use; touching it would have been exactly the kind of scope creep this
redesign is trying to avoid). It reuses the same private, unguarded `findTableForParallelReplicas(IQueryTreeNode*,
ContextPtr&)` overload `buildQueryPlanForParallelReplicas()` itself already relies on for MergeTree, called
with `candidate` -- the *exact* `QueryNode` `findQueryForParallelReplicas()` selected
(`GlobalPlannerContext::parallel_replicas_node`) -- rather than independently re-deriving from the whole root
query tree. Since `getSupportingParallelReplicasQueries()`'s traversal is deterministic and structural, a
walk from `candidate` reaches the identical driver a walk from the root would, but deriving it directly from
the actual selected candidate keeps the relationship correct by construction rather than by two traversals
happening to agree. `GlobalPlannerContext` gained a new field, `parallel_replicas_candidate_driver`
(5th constructor parameter, defaulted to `nullptr` so none of the ~13 other call sites needed touching),
computed once in `Planner::Planner()`'s constructor alongside `parallel_replicas_node` (both derived from a
single `findQueryForParallelReplicas()` call, avoiding recomputation/evaluation-order hazards). `Planner.cpp`'s
dispatch now reads `parallel_replicas_candidate_driver` instead of `parallel_replicas_table`.

**Bug 4 (q17 stopped distributing entirely): the `!context->isDistributed()` guard added in §12.7 (bug 2) was
based on a wrong assumption about what that flag means in this codebase.** Traced precisely via
`QueryAnalyzer.cpp:5817-5827`:
```cpp
auto & mutable_context = query_node.getMutableContext();
if (!mutable_context->isDistributed())
{
    bool is_distributed = false;
    if (auto * table_node = join_tree_node->as<TableNode>())
        is_distributed = table_node->getStorage()->isRemote();
    ...
    mutable_context->setDistributed(is_distributed);
}
```
`IStorageCluster::isRemote()` is unconditionally `true`. So the moment *any* query scope's own join tree
resolves a remote-capable table, that scope's *own* context gets `isDistributed()=true` -- purely
structurally, during ordinary analysis, regardless of initiator vs. worker. q17's own top-level scope gets
this set the instant `event_page` is resolved. This is not the "am I a worker executing an already-dispatched
sub-query" signal `DatabaseDataLake.cpp`'s analogous check relies on -- that check works there only because
of *when* it runs (during a table's own resolution, before the flag is set for it), a timing distinction
that doesn't hold when read later, at Planner-construction time, after analysis has already flipped it for
the whole scope. The actual "am I a worker" signal, `query_kind == SECONDARY_QUERY`, was already present
alongside it and is unaffected by this bug.

**Fix**: removed `!context->isDistributed()` from `isObjectStorageClusterDriverEligible()`, keeping only
`query_kind != SECONDARY_QUERY` (the correct, narrower signal for the worker-side recursive-refanout concern
§12.7 bug 2 was actually about).

Both fixes compiled clean (native sanity + full relink in progress). Pushed as `iceberg-join-bypass-arm64-v21`
once the `arm64` build completes. **Not yet re-run live.**

**Success criteria for this build, per reviewer guidance** -- explicitly *not* "q17/q21 pass ⇒ done":
1. q17 → `ReadFromCluster` / ~4s, q21 → `MergingAggregated → ReadFromCluster` / ~9s. This only proves the
   plumbing (candidate selection + execution backend wiring) is now behavior-preserving relative to `v17`'s
   validated shape -- it does **not** yet prove the general refactor is correct.
2. Immediately after, re-check q2/q4/q13/q16. The direct-driver path
   (`allowParallelReplicasForJoinTree()` bound to `isObjectStorageClusterDriverEligible()`) only checks that
   the *left* storage is eligible for a `LEFT`/`INNER ALL` join -- it does not, and structurally cannot by
   itself, distinguish q17's shape (`event_page LEFT JOIN` a 17-row lookup) from q2's shape (`event_app LEFT
   JOIN appinfo_d`, an expensive window-function CTE) the same way `findQueryForParallelReplicas()`'s
   dummy-plan-walk incidentally does for q21's driver-selection path (§12.2's `saw_join` removal note). If
   q2/q4/q13 regress again here, that is not a plumbing bug to patch with another syntactic guard -- it is
   the same open, genuinely unsolved cost/eligibility question flagged in §12.4, now confirmed to affect the
   direct-driver path too, and the next step would be designing an actual cost signal, not another guard.

### 12.9 Third live run against `v21`: q17 works, q2/q4/q13/q16 don't regress; q21 has a new,
different bug (`v21` → `v22`, diagnostic-only)

Confirmed live: q17 → `ReadFromCluster`, correct topology (§12.8 bugs 3 and 4 both fixed). q2/q4/q13/q16
show no regression this round -- real validation that at least for these specific benchmark queries the
direct-driver path's lack of a cost signal (§12.8's stated risk) has not (yet) bitten in practice.

q21 now fails differently -- no crash, `ReadFromCluster` does appear in the plan (topology looks right), but
execution fails:
```
Code: 10. DB::Exception: Not found column __table4.transaction_id in block. There are only columns:
__table1.transaction_id, max(if(equals(__table1.alert_type, 'Malware'_String), coalesce(__table2.policy_action,
__table1.action), __table1.action)): While executing Remote. (NOT_FOUND_COLUMN_IN_BLOCK)
```

Not guessed at: reproduced the *structural* shape locally first (`clickhouse local`, plain `Memory` tables,
no cluster involved) to isolate whether this is a general AST/analyzer bug in
`queryNodeToDistributedSelectQuery()`'s CTE-inlining for a "JOIN whose RHS is itself a JOIN against a CTE
that re-references the same underlying table twice" shape (q21's actual structure: `alert_events` joins
`event_alert AS a` against `policy_matches`, which itself independently reads `event_alert` again) --
**both the original `WITH ... AS (...)` form and a manually flattened form (every CTE reference replaced by
its own subquery body, mirroring `set_subquery_cte_name=false` serialization) execute correctly locally**,
producing identical, correct results. This rules out a general AST-flattening/self-reference bug in the
CTE-inlining logic itself -- the bug is specific to something in the actual distributed dispatch (serialize
→ wire → re-parse-and-execute-on-worker), not reproducible with a pure in-process round trip.

**Not yet fixed -- added a temporary diagnostic instead of guessing**: `buildQueryPlanForObjectStorageCluster()`
now logs the exact dispatched query text (`LOG_WARNING`, tag `OSC_DISPATCH`) right after
`queryNodeToDistributedSelectQuery()` produces it, so the next run can compare the *actual* serialized text
against the locally-tested reproduction above and pin down where they diverge (most likely candidate,
not yet confirmed: something specific to the driver being a genuine multi-node dispatching table function
(`icebergS3Cluster(...)`) embedded as one operand of a JOIN whose *other* operand is itself another JOIN,
executed by each of 3 initiator-selected workers independently -- an interaction between nested distributed
dispatch and the alias/column-position matching `ReadFromCluster`/`RemoteQueryExecutor` do when reconciling
returned blocks, not exercised by q17's simpler flat-RHS shape). Compiled clean, pushed as
`iceberg-join-bypass-arm64-v22`. **Next action: re-run q21 against `v22` and capture the `OSC_DISPATCH` log
line from `clickhouse-server.log`** (`grep OSC_DISPATCH`) before attempting any fix.

### 12.10 Root cause found: `findQueryForParallelReplicas()`'s MergeTree-tuned candidate-narrowing policy
is too conservative for an object-storage-cluster driver (`v22` → `v23`)

The `OSC_DISPATCH` log plus the full `EXPLAIN` from `v21`/`v22` (reviewer's analysis, cross-checked against
source, not taken on faith) showed the actual topology:
```
Join
  Expression -> ReadFromCluster                                    (transaction_event, driver = txnlog)
  Expression -> MergingAggregated -> Expression (Convert object storage
                cluster whole-query names) -> ReadFromCluster       (alert_events, driver = event_alert)
```
i.e. `transaction_event` and `alert_events` were being dispatched as **two independent**
`buildQueryPlanForObjectStorageCluster()` calls, joined locally on the initiator -- not the target `outer
q21 query -> MergingAggregated -> ReadFromCluster` whole-query dispatch. That directly explains the
`NOT_FOUND_COLUMN_IN_BLOCK __table4` error too: each independently-dispatched candidate gets its own,
separately-numbered `__tableN` identifiers, and nothing reconciles the two once `alert_events`'s
independently-computed result is joined against the outer scope's expectation of it.

Traced precisely against `findQueryForParallelReplicas()`'s own dummy-plan walk (`findParallelReplicasQuery.cpp`,
the internal 3-arg overload, unchanged in this repository until this fix) for q21's exact shape:
- `getSupportingParallelReplicasQueries()` from the outer q21 query walks `outer -> JOIN(left) -> transaction_event
  -> txnlog`, giving `stack = [outer_query, transaction_event]`.
- The dummy-plan walk processes innermost-first: `transaction_event`'s own scope (`SELECT transaction_id FROM
  txnlog WHERE ...`) is a clean Expression/Filter chain -> `res = transaction_event`.
- Processing the outer query next: its own plan is `Aggregating -> ... -> Join(txnlog-side, alert_events-side)`.
  Walking this hits the outer `Aggregating` (needs finalization, `inside_join=false` at that point, so the
  walk *keeps going* rather than stopping), then the `Join` step (pushes both children with `inside_join=true`),
  then reaches `alert_events`'s own `Aggregating` (its `GROUP BY a.transaction_id`) with `inside_join=true`.
  `can_distribute_full_node` ends up `false`, `currently_inside_join` ends up `true`.
- The function's own return logic: `if (!res) return nullptr; return currently_inside_join ? res : subquery_node;`
  -- since `currently_inside_join=true` and `res=transaction_event`, it returns the **narrower**
  `transaction_event`, not the outer query. The function's own comment on this exact branch says why:
  *"If we were inside JOIN we cannot offload the whole subquery to replicas because at least one side of the
  JOIN needs to be finalized on the initiator."*

That assumption -- a JOIN's non-driver branch needing its own finalization means the branch can't be
delegated to a replica -- is specifically true for MergeTree (a shard cannot assume it independently holds
the data needed to correctly finalize the non-driver side) and specifically **false** for a DataLake/
object-storage JOIN, where every worker has full access to the same shared catalog and can safely,
redundantly recompute the non-driver branch (`alert_events`, GROUP BY included) in full. This exact
distinction was already flagged, deliberately unresolved, in the original `v17` prototype notes (review
issue #2: *"a DataLake-specific eligibility policy needs its own design, not a blind port"*) and in this
redesign's own §12.4 -- reusing `findQueryForParallelReplicas()` wholesale (rather than just its structural
half) fixed the `appinfo_d` false-positive (§12.2) but reintroduced this false-negative for q21, a genuine,
previously-anticipated tension between MergeTree's and DataLake's semantics, not a plumbing bug.

**Fix**: added a 4th parameter, `narrow_candidate_on_non_passthrough_join_branch`, to the internal 3-arg
`findQueryForParallelReplicas(stack, mapping, settings, ...)` walk, controlling exactly the
`currently_inside_join ? res : subquery_node` decision above -- `true` (MergeTree's call site, unchanged
behavior) keeps falling back to the narrower candidate; `false` (used when the eventual driver is a
`StorageObjectStorageCluster`) instead accepts the wider candidate regardless. The *other*, unconditional
rejection path just above it (`if (!res) return nullptr;`, which is what correctly excludes q2's `appinfo_d`
-- a window function with no enclosing JOIN at all, rejected before this parameter is ever consulted) is
untouched by this change, so the q2/q4/q13/q16 protection is unaffected. The public `findQueryForParallelReplicas()`
overload determines which policy to use by finding the eventual driver's storage *before* running the dummy-plan
walk -- a second, cheap, side-effect-free call to the same private, structural `findTableForParallelReplicas()`
traversal already used elsewhere in this redesign (no dummy-table substitution, no nested `Planner`
construction) -- and passes the corresponding boolean through.

**Known, deliberately out of scope for this fix**: the relaxation applies uniformly to *any* non-passthrough
step found inside a JOIN once the driver is object-storage-cluster-kind, not specifically to the *non-driver*
side. A (currently hypothetical, not exercised by q17/q21) shape where the *driver's own* path additionally
passed through a JOIN with an unabsorbed step before reaching the actual table could be wrongly accepted
under this policy. Neither test query has this shape; distinguishing "which side of the join" would need
the dummy-plan walk to track branch identity, not just an `inside_join` boolean -- left as a follow-up, not
attempted here, matching this document's established practice of flagging known gaps rather than
guessing at unexercised cases.

Compiled clean (native + `arm64` full rebuilds). Pushed as `iceberg-join-bypass-arm64-v23` once the `arm64`
build completes. **Not yet re-run live.** Expected: q21 -> `outer query MergingAggregated -> ReadFromCluster`,
single dispatch, matching `v17`'s validated `~9s` shape; q17/q2/q4/q13/q16 unaffected (this change only
alters the policy consulted when the eventual driver is object-storage-cluster-kind *and* the outer query
was already being considered as a mechanism-A candidate at all -- q17's mechanism-B path and q2/q4/q13/q16's
rejection-before-policy-matters path are both untouched). The `OSC_DISPATCH` diagnostic (§12.9) is left in
place for this next validation pass, to directly confirm the dispatched text is now the single, whole outer
query rather than two independent candidates.

### 12.11 `v23` rejected before testing: too broad, fixed by carrying branch-role through the traversal
(`v22`/`v23` → `v24`)

§12.10's fix (`v23`) was rejected on review *before* being adopted, for a correctness reason its own
"known, deliberately out of scope" paragraph had already flagged but underestimated: it relaxed narrowing
for *any* non-passthrough step found inside a JOIN once the driver is object-storage-cluster-kind, not
specifically the non-driver side. Counter-example: `(SELECT k, sum(v) FROM object_storage_driver GROUP BY
k) LEFT JOIN rhs` -- here the `GROUP BY` sits on the *driver's own* (partitioned) path, and blindly widening
the candidate would let each worker aggregate only its own file-range slice before the JOIN, which is not
generally equivalent to aggregating first. `v23` was pushed before this was caught, but should not be used
-- the actual property needed is "the finalization-requiring step is on a JOIN branch proven to be the
*non-driver* side, not merely the driver's own storage kind."

**Two further gaps found in the first sketch of the branch-aware fix**, before any of it was written, by
tracing the exact code (not assumption):

1. **Order-dependence.** The existing `currently_inside_join = inside_join;` assignment inside the walk is
   overwritten on *every* non-passthrough step found, and the walk does not stop at the first one -- q21's
   own candidate check visits at least three (outer `Aggregating`, `alert_events`'s own `GROUP BY`,
   `policy_matches`'s own `GROUP BY`). A branch-role bit tracked the same overwriting way would let whichever
   failure is visited *last* silently override an earlier, differently-classified one. Fixed by keeping
   `currently_inside_join`'s existing last-write semantics **completely untouched** (so MergeTree's decision
   is provably byte-identical to before) and adding a **separate, purely cumulative** flag,
   `saw_unsafe_join_branch_failure` (set once, on any inside-join failure *not* on a recognized non-driver
   branch, never reset), consulted only when the driver is object-storage-cluster-kind.
2. **Plan-node identity mismatch.** `Planner.cpp:2753` (`query_node_to_plan_step_mapping[&query_node] =
   query_plan.getRootNode();`) confirms `mapping[alert_events]` records the root of `alert_events`'s own,
   independently-built plan (e.g. its `Aggregating` step) -- captured *before* the outer query wraps it with
   its own "Pre Join Actions" `Expression` step(s) when integrating it as a JOIN operand (the same shape
   visible in the real initiator `EXPLAIN`: `Join → Expression (Right Pre Join Actions) → ... →
   MergingAggregated`). So a JOIN's *immediate* child in the walk is that wrapper, not the mapped node one
   or more levels down -- pointer-equality against immediate JOIN children alone would miss it. Rather than a
   per-JOIN subtree search, the walk already visits every one of those intermediate wrapper steps one at a
   time via its existing single-child traversal (and `ExpressionStep` is already classified as passthrough,
   so it doesn't itself trigger a failure) -- so checking set membership at *every* visited node, not just
   immediate JOIN children, finds the mapped root correctly at whatever depth it sits, in O(1) per node
   instead of O(subtree) per JOIN.

**Fix, replacing `v23`'s single boolean:**
- `getSupportingParallelReplicasQueries()` gained an optional out-parameter,
  `std::vector<const IQueryTreeNode *> * non_driver_branch_roots`, populated at the exact point the function
  already decides which JOIN child to follow toward the driver -- pushing the *other* child (previously
  discarded) when non-null. Zero behavior change for existing callers (parameter defaults to `nullptr`).
- The public `findQueryForParallelReplicas()` overload calls this (only when the driver is
  object-storage-cluster-kind, via the same early `findTableForParallelReplicas()` lookup from `v23`) on
  `updated_query_tree` specifically -- not the original tree -- so the collected pointers share identity
  with `mapping` (both built from the same dummy-substituted clone). QUERY-type siblings are then resolved
  through `mapping` once, up front, into `non_driver_plan_roots` (`std::unordered_set<const QueryPlan::Node
  *>`); a bare-table non-driver side (q17's `geo_location_lookup`) is skipped, having no internal steps to
  protect.
- The internal 3-arg walk's `Frame` gained one sticky bit, `on_non_driver_branch`, checked and propagated at
  *every* visited node (inherited from the parent frame, or newly set on a `non_driver_plan_roots` match) --
  addressing gap 2. The failure site now also sets `saw_unsafe_join_branch_failure` cumulatively when
  `inside_join && !on_non_driver_branch` -- addressing gap 1. The final decision:
  `relax_for_object_storage_driver ? saw_unsafe_join_branch_failure : currently_inside_join` -- for
  MergeTree (`relax_for_object_storage_driver=false`, `non_driver_plan_roots` never populated), this reads
  `currently_inside_join` exactly as stock code always has; only the object-storage path uses the new,
  order-independent, branch-aware signal.

Verified against the counter-example from the rejected `v23`: `GROUP BY` on the driver's own (LEFT/followed)
side means the *right* side gets recorded as the non-driver sibling, not the aggregating branch --
`on_non_driver_branch` stays false at that `Aggregating` step, `saw_unsafe_join_branch_failure` still gets
set, narrowing still applies, exactly as required.

Added a temporary diagnostic (`LOG_WARNING`, tag `PR_CANDIDATE_CLASSIFY`, object-storage-driver candidates
only) logging candidate-stack size, how many non-driver branch roots were found/resolved, and whether the
final selection is the outermost/innermost/null candidate -- to directly confirm q21's classification
without needing to infer it from `EXPLAIN` alone. Compiled clean (native + `arm64` full rebuilds). Pushed as
`iceberg-join-bypass-arm64-v24`.

### 12.12 `v24` live: `selected_is_null=true` -- a third, unrelated bug in the dummy-substitution mechanism
itself (`v24` → `v25`, diagnostic-only)

`PR_CANDIDATE_CLASSIFY` from the actual `v24` run: `stack_size=2 non_driver_branch_roots=1
non_driver_plan_roots=1 selected_is_outermost=false selected_is_innermost=false selected_is_null=true`.
`res` is **null**, not narrow (`transaction_event`) and not wide (outer query) -- §12.11's fix for gaps 1
and 2 both worked exactly as designed (`non_driver_branch_roots`/`non_driver_plan_roots` both correctly
populated), but the *result itself* regressed to nothing at all, and the `OSC_DISPATCH` text (`alert_events`
alone, `icebergS3Cluster(event_alert) LEFT JOIN policy_matches`) confirms mechanism A found no candidate,
letting `buildJoinTreeQueryPlan()` run normally for the outer query and mechanism B independently pick up
`alert_events`'s own `event_alert` as an unrelated immediate-leftmost driver -- a different code path than
anything touched in §12.10/§12.11.

Traced (not yet confirmed live) to a third, structurally separate bug in the pre-existing dummy-substitution
mechanism itself, unrelated to branch-role tracking: `getSupportingParallelReplicasQueries()`, called on
`updated_query_tree` (dummy-substituted), reaches `transaction_event`'s own driving table -- now a
`StorageDummy` standing in for the real `StorageObjectStorageCluster` `txnlog` -- and if
`canUseTableForParallelReplicas()` (→ `isTableNodeEligibleForParallelReplicas()`) rejects that `StorageDummy`,
the function returns `{}` (discarding everything accumulated, including `outer_query`/`transaction_event`)
regardless of anything already recorded via the `non_driver_branch_roots` out-parameter (a side effect,
independent of the return value) -- exactly matching `new_stack` ending up empty, the internal walker's
`while (!stack.empty())` loop never running, and `res` staying `nullptr`.

Whether `isObjectStorageClusterDriverEligible()`'s `StorageObjectStorageCluster`-only `dynamic_cast` (which a
generic `StorageDummy` can never satisfy) is *actually* what rejects it, or whether the existing MergeTree/
`StorageDummy` fallback path (`storage->isMergeTree() || typeid_cast<StorageDummy*>`, then
`supportsReplication()`) still saves it as it apparently did before this session's changes (`ReadFromCluster`
did appear for `txnlog` in every prior test), is not yet certain from static reading alone --
`StorageObjectStorage::supportsReplication()` returns `configuration->isDataLakeConfiguration()` (true for
Iceberg) and `ReplaceTableNodeToDummyVisitor` passes the real storage's `supportsReplication()` into the
`StorageDummy` constructor, which by that reasoning *should* still pass the stock MergeTree/StorageDummy
check -- but that reasoning contradicts the observed `new_stack` failure, so something in it is wrong.
Rather than guess further, added a second temporary diagnostic, `PR_TABLE_ELIGIBILITY_FAIL` (fires from
`getSupportingParallelReplicasQueries()`'s own `TABLE` case whenever `canUseTableForParallelReplicas()`
rejects a table, logging `is_storage_dummy`, `is_merge_tree`, `supports_replication`, and the
`parallel_replicas_for_non_replicated_merge_tree` setting), plus `new_stack_size` added directly to
`PR_CANDIDATE_CLASSIFY` (previously only `stack.size()`, the *original*-tree stack, was logged -- not
`new_stack.size()`, the dummy-tree one actually driving the walk). Compiled clean (native + `arm64` full
rebuilds). Pushed as `iceberg-join-bypass-arm64-v25`. **Not yet re-run live** -- next action is to capture
both new log lines from a fresh q21 run before writing any further fix.

### 12.13 Root cause confirmed with certainty: `StorageDummy` erases the type `isObjectStorageClusterDriverEligible()` depends on (`v25` → `v26`)

`v25` live: `PR_TABLE_ELIGIBILITY_FAIL table=ice.\`billion-rows_t17175.txnlog\` is_storage_dummy=true
is_merge_tree=false supports_replication=false parallel_replicas_for_non_replicated_merge_tree=false`,
immediately followed by `PR_CANDIDATE_CLASSIFY ... new_stack_size=0 ... selected_is_null=true`. No more
ambiguity: the dummy-substituted `txnlog` fails the eligibility check inside
`getSupportingParallelReplicasQueries()`, which discards the *entire* candidate stack (`{}`), including
everything already accumulated (`outer_query`, `transaction_event`) -- independent of §12.11's
`non_driver_branch_roots`/`non_driver_plan_roots` bookkeeping, which had already succeeded (both `=1`) by
that point. With `new_stack` empty, the internal walker's `while (!stack.empty())` loop never runs and `res`
stays `nullptr` -- `parallel_replicas_node` ends up unset entirely, `buildJoinTreeQueryPlan()` runs normally
for the outer query, and `alert_events` gets independently picked up by mechanism B -- a different code path
than anything §12.10/§12.11 touched, which is why those fixes, though individually correct (confirmed by
the same log line), could never have been observed taking effect.

**Root cause, structural, not tunable**: `ReplaceTableNodeToDummyVisitor` (used only for the disposable
plan built to walk plan-step *shapes*, never executed) replaces every table with a generic `StorageDummy` --
by design, since the dummy-tree analysis only needs each table's columns/`StorageID`, not its real storage
class. But `isObjectStorageClusterDriverEligible()`'s `dynamic_cast<StorageObjectStorageCluster *>` can
never succeed against a `StorageDummy`, no matter what the original storage was -- the type information the
whole ObjectStorage eligibility path depends on is erased by the very substitution mechanism eligibility is
later re-checked against. `StorageDummy`'s own MergeTree/replication fallback doesn't save it either,
because `StorageObjectStorage::supportsReplication()` (`return configuration->isDataLakeConfiguration();`)
reports the underlying catalog's own replication semantics -- for this Iceberg source, `false` -- not "is
this a valid parallel-replicas candidate," a question `StorageDummy`'s constructor has no other way to be
told the answer to. Static reasoning about whether this fallback would or wouldn't save it (§12.12) was
inconclusive precisely because the property being checked (`isDataLakeConfiguration()`) has nothing to do
with the property actually needed -- confirmed empirically rather than resolved by further reading.

**Fix**: `isObjectStorageClusterDriverEligible()`'s signature changed from `(const StoragePtr &, const
ContextPtr &)` to `(const IStorage &, const ContextPtr &)` -- a pure widening, no behavior change at either
existing call site (`isTableNodeEligibleForParallelReplicas()`, `PlannerJoinTree.cpp`'s
`allowParallelReplicasForJoinTree()` predicate), both updated to dereference their `StoragePtr` instead.
This lets `ReplaceTableNodeToDummyVisitor::enterImpl()` -- which already holds `const IStorage & storage`,
the *real* storage, at the exact point before it gets replaced -- call it directly, with no `shared_ptr`
needed. The dummy's `supportsReplication()` constructor argument (previously just
`storage.supportsReplication()`) becomes `storage.supportsReplication() ||
isObjectStorageClusterDriverEligible(storage, getContext())`: eligibility is decided once, against the real
storage, before replacement, and folded into the one signal `StorageDummy` can still carry forward --
`isTableNodeEligibleForParallelReplicas()`'s existing MergeTree/`StorageDummy` fallback branch (`if
(!storage->isMergeTree() && !typeid_cast<StorageDummy*>(storage.get())) return false;` then the
`supportsReplication()` check) then passes for exactly the tables that were already proven eligible, without
touching that branch's logic or its behavior for genuine MergeTree/`StorageDummy` cases at all.

This is a **fourth**, entirely independent bug from the three already fixed in this document (§12.8's
`only_analyze` recursion and `isDistributed()` misunderstanding; §12.10/§12.11's candidate-narrowing policy
and its two tracking gaps) -- all of which turned out, in retrospect, to have been correctly implemented but
untestable, because this one silently zeroed out mechanism A's candidate stack before any of that logic
ever got exercised for an object-storage-cluster driver reachable through a CTE. Compiled clean (native +
`arm64` full rebuilds). Pushed as `iceberg-join-bypass-arm64-v26`.

**Confirmed live: q21 works.** This closes out the redesign's core correctness goal -- both q17 (direct
leftmost driver, mechanism B) and q21 (driver nested under a CTE, mechanism A) now dispatch through the same
shared `buildQueryPlanForObjectStorageCluster()` execution backend, matching the architecture agreed in §12.1,
via the stock parallel-replicas candidate-selection machinery generalized with a pluggable storage-eligibility
policy rather than a duplicated traversal.

### 12.14 Next steps, not yet done

- [ ] Re-check q2/q4/q13/q16 against `v26` specifically (they showed no regression against `v20`/`v21`,
      before §12.10-§12.13's candidate-selection changes landed -- those changes only affect
      `findQueryForParallelReplicas()`'s internal walk for an object-storage-cluster driver, gated the same
      way, so no reason to expect a new regression, but not yet re-confirmed against this exact binary).

### 12.15 `v26`'s re-check (§12.14's own open item) found a fifth bug: mechanism B has no boundary against a
JOIN nested under a *derived-table* subquery, not just a CTE (`v26` → `v27`)

Running the actual re-check flagged as still outstanding in §12.14 (not a fresh regression -- the first time
this exact binary shape had been exercised against the full 23-query suite) surfaced a new failure, q16 only:

```
Code: 10. DB::Exception: Not found column __table7.appinfo_ccl in block. There are only columns:
__table2.appinfo_ccl, __table1.event_timestamp...
```

q2/q4/q13/q21/q17 all still correct against `v26` -- confirming §12.14's own prediction that §12.10-§12.13
didn't regress them. q16 is structurally different from q2/q4/q13 despite sharing the same `appinfo_d` CTE
(comment in `q16_looker_pivot.sql`: "base is q4's bytes-by-day-x-CCL"): q16 wraps that same `base` JOIN (`{appev}
e LEFT JOIN appinfo_d a`) in **four more levels of derived-table subqueries** (`ww`/`xx`/`yy`/`zz`, Looker's
pivot machinery -- stacked `DENSE_RANK`/`RANK`/`MIN() OVER` window functions), where q2/q4/q13 select from
`base` directly with no extra wrapping.

**Root-caused precisely, not by pattern-matching against q21's earlier bug**: traced which mechanism actually
fires. Mechanism A (`findQueryForParallelReplicas()`) does **not** engage for q16 at all -- `base` gets its own
recursive `Planner subquery_planner` (`PlannerJoinTree.cpp:1512`, one per derived-table `FROM`-clause subquery),
and from that nested Planner's own candidate search, `base` is both the outermost *and* innermost stack entry
(`getSupportingParallelReplicasQueries()` on `base`'s own scope returns just `[base]`). It's popped first,
`mapping[base]` is `base`'s own `Aggregating` (its `GROUP BY`) step, which needs finalization, and since `res`
is still `nullptr` on this very first iteration, `findParallelReplicasQuery.cpp`'s `if (!res) return nullptr;`
fires immediately -- mechanism A is a dead end for this shape, never even reaching the "narrow vs wide candidate"
question §12.10-§12.13 fixed for q21.

**Mechanism B** (`PlannerJoinTree.cpp`'s `dispatched_as_object_storage_cluster`, §12.1) is what actually fires,
and it has no check on whether the JOIN's owning `query_node` (`base`) is the query's true top level or merely
nested under further wrapping. It dispatches `base` as a **standalone unit** via
`buildQueryPlanForObjectStorageCluster()` -> `queryNodeToDistributedSelectQuery()`, which gets independently
re-numbered when the worker re-analyzes it from scratch -- `__table1`(`appev`)/`__table2`(`appinfo_d`), scoped
only to `base`'s own two tables. That's exactly the observed "There are only columns: `__table2.appinfo_ccl`,
`__table1.event_timestamp`". Something in the *already-built* outer plan -- from the single, original,
whole-query analysis pass that ran once before any nested `subquery_planner` instances existed -- still expects
`appinfo_d`'s column at its original position in that global numbering, `__table7`. Same bug *family* as q21's
original double-dispatch (§12.9: two independently-renumbered candidates never reconciled), different trigger:
there it was two independent whole-scope dispatches; here it's one dispatch whose scope is narrower than a
reference that survives around it. q17 never exposed this because its JOIN sits at the query's true top level --
no enclosing subquery scope to go stale against.

**Fix**: added `!select_query_options.is_subquery` to `dispatched_as_object_storage_cluster`'s condition
(`PlannerJoinTree.cpp`, ~L2153). `select_query_options.is_subquery` is set at exactly the point (`subquery()`,
`PlannerJoinTree.cpp:1511`) where a derived-table subquery gets its own nested `Planner subquery_planner` --
already available as a parameter at this call site, no new plumbing needed. This is a conservative capability
boundary, not a q16-specific patch: whole-query object-storage-cluster dispatch via mechanism B is validated
only for a JOIN at the query's actual top level (q17); a driver nested under a *CTE* (not a derived-table
subquery) inside the top-level query is mechanism A's territory and unaffected (q21). A driver nested under a
plain derived-table subquery now falls through to ordinary local planning, exactly as it did before this
session's changes -- narrower than the general goal, but honest about what's actually been proven correct.
Nested mechanism-B pushdown (reconciling `__tableN` identity across the nested-Planner serialization boundary)
remains a real, deliberately out-of-scope follow-up, not attempted here.

Compiled clean, no new warnings (native + `arm64`, incremental -- only `PlannerJoinTree.cpp` recompiled).
Packaged (`llvm-strip-21`, ~5.2 GB → ~615 MB, same as every prior build) and pushed as
`iceberg-join-bypass-arm64-v27`. **Not yet re-run live** -- next action is the full suite (q16 correctness,
q2/q4/q13 still correct, q17 ~4s, q21 ~9-10s) against this exact image.

### 12.16 Next steps, not yet done

- [ ] Run the full 23-query suite against `v27` live; confirm q16 now succeeds and q2/q4/q13/q17/q21 are
      unaffected (success criteria above).
- [ ] Multiple hot-run timing samples for q17 and q21 (median, not one run) to confirm the speedups still
      match `v17`'s validated shape (q17 ~3.8s, q21 ~9s) now that the mechanism is unified.
- [ ] Remove the temporary diagnostics once the above is confirmed: `OSC_DISPATCH`, `PR_CANDIDATE_CLASSIFY`,
      `PR_TABLE_ELIGIBILITY_FAIL` (all added this session), plus the older `DLPR`/`OSC_STAGE` (kept from the
      `v17` redesign for this validation pass, §12.2).
- [ ] Commit the working tree (currently uncommitted since the `v17` redesign began -- §12) once the above
      validation is complete, if requested.
- [ ] The known, deliberately-unsolved gaps remain open, unchanged by this session's fixes: no cost-based
      eligibility signal (§12.4/§12.8's stated risk for q2/q4/q13-shaped regressions in general, not just
      the two tested this session), and `buildQueryPlanForObjectStorageCluster()`'s `column_names`/
      `filter_actions_dag` still use `AllPhysical`/unset rather than the driver's exact required columns
      (§3.7's original "known gap", carried forward unchanged through this redesign).
