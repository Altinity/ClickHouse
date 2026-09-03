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

Grep pattern: `grep -E 'DLPR|OSC_STAGE|CLUSTER_ALT' /var/log/clickhouse-server/clickhouse-server.log`

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
    §3.2), **`-arm64-v7`** (current: `IStorageCluster.cpp` CTE-serialization fix,
    pushed last, awaiting re-validation).
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

- [ ] Re-run the full 23-query benchmark suite against `arm64-v7` with
      `object_storage_cluster_bypass_join_wrap=1` set for the whole suite (as it
      was for the run that surfaced the regression) — confirm q17 still
      fast/correct **and** q2/q4/q13/q16/q21 now succeed (CTE body should be
      inlined instead of erroring on `Unknown table expression identifier`).
      Check `CLUSTER_ALT` log lines: since `ignoring_join` turned out to be
      `true` for q2 as well as q17 (§3.2), all six queries should now show
      `experimental_allow_join=true` — the differentiator is no longer that log
      line, it's whether the CTE queries now succeed end-to-end.
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
- [ ] Remove the `DLPR`/`OSC_STAGE`/`CLUSTER_ALT` diagnostic logging before any
      production-quality version.
- [ ] Nothing in this experiment has been committed to git — all changes listed
      above are uncommitted working-tree modifications on branch
      `fix/antalya-26.6/query-plan-aggregation-perf`.

---

## 10. Full diff (as of `arm64-v7`)

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
diff --git a/src/Storages/IStorageCluster.cpp b/src/Storages/IStorageCluster.cpp
index 02389f92cf4..97f0b3a96a5 100644
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
diff --git a/src/Storages/ObjectStorage/StorageObjectStorageCluster.cpp b/src/Storages/ObjectStorage/StorageObjectStorageCluster.cpp
index 9eefd709aba..ff3a80601b5 100644
--- a/src/Storages/ObjectStorage/StorageObjectStorageCluster.cpp
+++ b/src/Storages/ObjectStorage/StorageObjectStorageCluster.cpp
@@ -740,6 +740,18 @@ String StorageObjectStorageCluster::getClusterName(ContextPtr context) const
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
| **`iceberg-join-bypass-arm64-v7`** | + `IStorageCluster::read()` CTE fix (`queryNodeToDistributedSelectQuery()` for `query_to_send` when the experimental flag is set) | **current, awaiting re-validation of q2/q4/q13/q16/q21 + q17 on the live cluster** |
| `bin-runner-amd64` / `bin-runner-arm64` | generic binary-URL-driven runner image, no baked binary | available, unused so far for actual testing |
