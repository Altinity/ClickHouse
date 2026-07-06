# Scenario card author's API reference

A card is a `Scenario` subclass in `scenarios/cards/`. The framework constructs the run context and
result, resets the pool, runs `run(self, ctx, result)`, finalizes the verdict, and writes reports.
Cards must only **populate `result`** and drive the cluster via the helpers below — never call
`result.finalize()`, write reports, or touch RUN_HISTORY/BACKLOG (the runner does that).

See `cards/s01_s02_huge_blob.py` for a worked example.

## Scenario base (`framework.base`)

```python
from ..framework.base import Scenario, register

@register
class S0X(Scenario):
    name = "S0X"; title = "..."; priority = "P0"   # required
    # optional flags:
    abandons = False            # deliberately leaves unreachable objects -> relaxes leftover check
    expect_exception = False    # negative test: an `exception` CA-log row is allowed
    compose_variant = None      # None | "gc_shards2"
    needs_infra = None          # set a string reason if the scenario can't run on the 2-replica+RustFS
                                # compose -> the runner marks it inconclusive and never runs run()
    param_table = {             # scale -> params; "dev" is the fast default, ci/full are larger
        "dev": {...}, "ci": {...}, "full": {...},
    }
    def run(self, ctx, result):
        ...
```

## RunContext `ctx`
- `ctx.cluster` — `soak.cluster.Cluster`: `.node1`, `.node2`, `.nodes()`. A `Node` has
  `.query(sql) -> str` (TabSeparated), `.command(sql)`, `.scalar(sql) -> str`, `.container`, `.ping()`.
- `ctx.params` — resolved param dict for this run. `ctx.seed`, `ctx.duration_s`, `ctx.scale`.
- `ctx.log(msg)`; `ctx.path(name) -> Path`; `ctx.subdir(name)`; `ctx.write_json(name, obj)`;
  `ctx.write_text(name, text)`.
- `ctx.extra["since_event_time"]` — server `now()` captured at run start; pass to log queries to scope
  to this run (the helpers do this for you).

## Result + Verdict (`framework.report`)
- `result.add(verdict)`; `result.note_anomaly(text)`; `result.observations[k]=v`;
  `result.timings[k]=v`.
- `Verdict.check(name, expected, observed, ok, note="")` — pass/fail from a bool.
- `Verdict.inconclusive(name, expected, reason)` — data unavailable; NEVER silently pass.
- `Verdict.skipped(name, reason)` — explicitly not evaluated.
- `Verdict(name, expected, observed, status, note)` — status in `pass`/`fail`/`inconclusive`/`skipped`.

## Common card helpers (`cards._common`)
- `standard_end(ctx, result, tables, table_filter=None, abandons=False, expect_exception=False, optimize=True)`
  — quiesce → settle fsck → forced GC to fixpoint → final fsck+dryrun → collect observations → dump
  raw extracts → run the common hard assertions. Call this at the end of (almost) every positive card.
  `tables` is the small list of tables to SYNC/OPTIMIZE; for many-namespace cards pass a short list
  plus a `table_filter` SQL fragment (e.g. `"table LIKE 's05_%'"`).
- `record_peak_memory(result, sampler, budget_bytes=None, label=...)` — peak RSS verdict.
- `assert_replicas_agree(result, cluster, query, name=...)` — both replicas return the same value.
- `counters_window(ctx)` — returns `finish()`; call `finish()` after the workload for a CA-counter
  delta dict `{"_total": {counter: delta}, "<container>": {...}}`.
- `blob_count(ctx)` — current physical blob-object count (None if the pool probe failed).

## SQL / workload (`framework.sql`)
- `create_ca_table(node, name, columns=, order_by=, partition_by=None, ttl=None, engine=None,
  extra_settings=None, wide=True, replica_path=None)` — `storage_policy='ca'`, ReplicatedMergeTree by
  default with a shared zk path derived from the name.
- `drop_table_both(cluster, name)`, `drop_all_ca_tables(cluster)`, `list_ca_tables(node)`.
- `insert_random(node, table, rows=, payload_bytes=, extra_cols_select="", op_id=0, settings=None,
  timeout=)` — `payload` column = `payload_bytes` of incompressible `randomString` (so the column
  `.bin` ≈ rows × bytes). Table needs `(id UInt64, payload String, ...)`.
- `insert_values(node, table, values_sql, timeout=, settings=)` — `INSERT INTO t <values_sql>`,
  retry-wrapped. Use `SELECT ... FROM numbers(N)` with `repeat(...)` for DETERMINISTIC content
  (needed for dedup tests; `randomString` is non-deterministic).
- `replicas_agree(cluster, query) -> (bool, {container: value})`; `table_checksum_query(table)`;
  `parts_summary(node, table) -> {active, inactive, rows, bytes_on_disk}`.

## Observability (`framework.observe`)
- `pool_shape(timeout_s=)` -> `{prefix:{objects,bytes}, "_total":{...}, "_ok":bool}` for prefixes
  `blobs/roots/_manifests/refs/_files/gc/_pool_meta/other`.
- `cluster_events_snapshot(cluster)` / `cluster_events_delta(before, after)` — `Cas*`/`DiskS3*`/`S3*`.
- `gc_log_all(cluster, since)` -> `{per_node, summary{failed,not_a_leader,success,deleted_total,...}}`.
- `ca_event_counts_all(cluster, since)` -> `{per_node, bad_total}` (bad = read_missing/dangling_access/
  corrupt_dangle/corrupt_decode/snap_journal_incoherent/exception).
- `server_memory(node)`, `cluster_memory(cluster)`, `container_samples()`, `object_lifetime(node,
  object_hash=, token=)`.

## GC + lifecycle (`framework.gc`, `framework.lifecycle`)
- `gc.gc_round(node, disk="ca")`, `gc.gc_drive_round(cluster)` (single leader — never both, to avoid the concurrent-leader reclaim leak),
  `gc.forced_gc_to_fixpoint(cluster, unreachable_fn) -> (residual, history)`.
- `lifecycle.fsck_summary()`, `lifecycle.fsck_detail()`, `lifecycle.dryrun()`,
  `lifecycle.unreachable_probe()` (-> 0-arg callable), `lifecycle.quiesce_cluster(cluster, tables, ...)`.

## Sampler (`framework.sampler`)
```python
from ..framework import sampler as sampler_mod
smp = sampler_mod.MetricsSampler(sampler_mod.open_db(ctx.path("metrics.sqlite")), ctx.cluster,
                                 interval_s=5.0, pool_every=4, phase_fn=lambda: "workload", log_fn=ctx.log)
with smp:                       # or smp.start()/smp.stop()
    ...workload...
smp.peak_mem_resident           # {container: peak_rss_bytes}
smp.peak_scratch_bytes()        # {container: peak_scratch_bytes}
```

## Conventions
- Conservative dev-scale defaults; keep ci/full scale knobs in `param_table`.
- Never silently skip an assertion — emit `Verdict.inconclusive`/`Verdict.skipped` with a reason.
- Negative cards: set `expect_exception=True`, assert the statement fails with the expected code, and
  prove no live ref points at missing content (fsck dangling==0) + partial uploads reclaimable.
- Table names: prefix with the scenario id (e.g. `s09_wide`) so multiple cards never collide and a
  `table_filter` can scope quiescence.
