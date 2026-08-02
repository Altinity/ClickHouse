# T6a — frontier-attribution risk spike: verdict

## Verdict: BENIGN-TRANSIENT, and already structurally closed

The measured deficits (`3155/3157` and `11358/11369` namespaces proven, both with `0 anomaly(ies), 0
held namespace(s)`) were **post-LIST appends above the frozen tail**. That exit no longer exists as a
reachable path: the checkpoint-committed frontier introduced by `357cf7b963f`
(`ca: ref — LIST-independent recovery: exact checkpoint frontier, no hint-derived history`, 2026-08-02)
bounds the walk at `_ckpt.committed_through` and **proves** the frontier there instead of abandoning
it, one day after the 2026-07-31 measurement.

A healthy pool now converges to a complete frontier **within a single round**, not between rounds: a
record that arrives after the round's `LIST` is either at or below `committed_through` — in which case
the walk folds it and keeps going — or above it, in which case the walk stops at the ceiling with
`resolved_through == *committed_through`, which IS the proof. Neither outcome leaves a namespace
unproven.

T6 may proceed on its other predecessors.

## What the reproduction showed

Reproduction: a local server on the instrumented build, one CA-local pool (`object_storage_type =
local`, `metadata_type = content_addressed`) carrying 80 namespaces, `gc_enabled = 1`,
`gc_interval_sec = 1`, driven by four concurrent `clickhouse client` streams issuing 2000 `INSERT`s
each so that appends land while rounds walk.

- Server config: `tmp/t6a_srv/config.xml`; log `build/t6a_server3.log`.
- Table creation: `tmp/t6a_create.sql`; insert streams: `tmp/t6a_insert.sql` driven by `tmp/t6a_load.sh`.
- The named cheapest reproducer's shape (`05010_content_addressed_mounts_gc_health.sh`: one inline CA
  disk, `INSERT` + `TRUNCATE`, then a synchronous `SYSTEM CONTENT ADDRESSED GC RUN`) was also run
  directly against the same server.

**Every round reported a complete per-namespace tally.** Across all 935 suppression lines in
`build/t6a_server3.log` — 230 rounds on the 80-namespace pool and 705 on the single-namespace
`05010`-shaped pool — there is not one round where `frontier_proven` fell short of
`frontier_namespaces`:

```
$ grep -ao "([0-9]* of [0-9]* namespace(s) proven" build/t6a_server3.log | sort | uniq -c
    705 (1 of 1 namespace(s) proven
    230 (80 of 80 namespace(s) proven
```

```
$ grep -ac "unproven:" build/t6a_server3.log
0
$ grep -ac "frontier-probe budget" build/t6a_server3.log
0
```

The `05010`-shaped round, same log:

```
<Information> t6a_05010_pool/::ContentAddressedGC: CAS GC fold: destructive work SUPPRESSED this
pass — 0 anomaly(ies), 0 held namespace(s), frontier INCOMPLETE (1 of 1 namespace(s) proven; the
universe itself is not provable this stage).
```

It logs at Info, not Warning: `per_round_cause` is false, so the **only** remaining suppressor on that
pool is the Stage-A universe constant — which is precisely what T6 flips.

## The counterfactual: the old predicate would have fired

A zero deficit under the current code proves the deficit is gone; it does not by itself identify what
produced the historical one. To close that, a TEMPORARY probe counted namespaces whose walk read a
record strictly above `target.frozen_tail` — the pre-`357cf7b963f` break's exact predicate — and then
folded it anyway under the current ceiling. It was **removed before the commit**; see "Deviations".

```
$ grep -ac "T6A-TEMP" build/t6a_server3.log
935
$ grep -a "T6A-TEMP" build/t6a_server3.log | grep -av "T6A-TEMP: 0 namespace" | wc -l
12
2026.08.03 01:06:12 <Warning> t6a4_pool/::ContentAddressedGC: T6A-TEMP: 76 namespace(s) read a record strictly above the round's frozen tail
2026.08.03 01:06:17 <Warning> t6a4_pool/::ContentAddressedGC: T6A-TEMP: 42 namespace(s) read a record strictly above the round's frozen tail
2026.08.03 01:06:35 <Warning> t6a4_pool/::ContentAddressedGC: T6A-TEMP: 17 namespace(s) read a record strictly above the round's frozen tail
2026.08.03 01:06:41 <Warning> t6a4_pool/::ContentAddressedGC: T6A-TEMP: 18 namespace(s) read a record strictly above the round's frozen tail
```

Every round emitted the probe; 12 of them saw at least one such namespace, the largest reaching 76 of
the pool's 80. All 12 are on the 80-namespace pool under concurrent inserts. Under the pre-`357cf7b963f` walk every one of
those namespaces would have left the loop silently unproven — no anomaly, no hold — while under the
current walk all 80 are proven in the same rounds. That is the mechanism, measured.

## The attribution table

The instrumented build attributes every unproven namespace to exactly one bucket
(`Gc::FoldResult::FrontierDeficit`), so the buckets sum to `frontier_namespaces - frontier_proven` by
construction. Measured over every round above:

| Bucket | Count | Exit it names | Would it be silent? |
| --- | --- | --- | --- |
| `no_catalog_entry` | 0 | the namespace has no `Live`/`Removing` catalog row this round | no — records an anomaly |
| `checkpoint_unusable` | 0 | `chooseRecoveryGrounding` produced nothing (absent or invalid `_ckpt`) | no — hold, or anomaly when there is no position to hold at |
| `checkpoint_frontier_empty` | 0 | an empty checkpoint frontier under a nonzero sealed cursor | no — records an anomaly |
| `committed_below_cursor` | 0 | `committed_through` precedes the sealed cursor | no — records an anomaly |
| `held` | 0 | an effective hold, this round's or carried | no — the hold set is a gate term of its own |
| `append_above_frozen_tail` | 0 | the frozen-tail edge | yes, if reachable — see below |
| `probe_budget` | 0 | `gc_frontier_probe_budget` ran out before the namespace was probed | no — its own `LOG_WARNING`, and `frontier_unprobed_budget` |
| `fold_aborted` | 0 | the whole ref fold aborted, discarding its proofs | no — the abort records an anomaly |
| `unattributed` | 0 | an exit that left a namespace unproven without naming itself | yes — which is why the bucket exists |

Every bucket except `append_above_frozen_tail` and `unattributed` carries an anomaly, a hold, or its
own warning, so none of them can produce the observed "0 anomalies, 0 holds" shape. That is what makes
the frozen-tail edge the only candidate, and the counterfactual above confirms it fired.

## Two dead arms, reported not fixed

The ceiling check at the top of the walk (`if (*grounding->committed_through < *expected)`, the first
statement of `while (expected)`) breaks out before any body `GET`. `grounding` is not reassigned in the
loop and `expected` is not reassigned on any path that falls through to the sites below, so reaching
either of these guarantees `*expected <= *grounding->committed_through`:

- the frozen-tail break's second conjunct, `*grounding->committed_through < *expected`, is therefore
  always false — the break is unreachable. Its comment still says the frozen tail is "what makes the
  round's work finite at round start however fast the writer appends". The round IS still finite, but
  because `committed_through` is a round-start snapshot, not because of this break.
- the absent-record arm's `else frontier_proven = true` (under `if (*expected <=
  *grounding->committed_through)`) is unreachable for the same reason: the condition is always true, so
  a missing record below the ceiling always holds.

Neither is a safety defect — an unreachable branch that would have refused a proof cannot grant one,
and the surviving path is the stricter one. Both are dead code plus a comment that describes a bound
the code no longer implements. **Not fixed here**, per the brief; a separate task owns it.

## What T8's soak must show

The convergence claim above is within-round, so the soak criterion is stronger than "drains
eventually":

1. **Every** round on a healthy pool reports `frontier_proven == frontier_namespaces`. Not "converges
   over N rounds" — a single round with a deficit on a pool with no anomalies and no holds falsifies
   the verdict.
2. `unattributed` is zero on every round. A nonzero value means the bucket enumeration has stopped
   being exhaustive and the attribution above no longer covers the code.
3. `probe_budget` is zero, or, if not, `gc_frontier_probe_budget` is raised until it is — an exhausted
   budget is a configuration answer, not a defect, but it suppresses destruction exactly as a defect
   would.
4. The pending-reclaim backlog drains to zero and STAYS there across rounds, rather than draining once.

## Instrumentation kept

`Gc::FoldResult::FrontierDeficit` and `Gc::FoldResult::FrontierUnproven` (declared in `CasGc.h`,
counted in `Gc::fold`) are kept as a permanent observability improvement. The suppression warning now
appends `; unproven: <bucket>=<count>, ...` whenever the deficit is nonzero, so an operator reading
"N of M proven" is told which cause produced the deficit — the causes want opposite responses (a hold
is something to chase, an exhausted probe budget is something to raise), and the warning previously
distinguished none of them. Rounds with no deficit print no such clause, so the healthy-pool log is
unchanged.

## Deviations

- **A temporary counterfactual probe was added and then removed.** It counted namespaces reading above
  the frozen tail, to turn "the old predicate would have fired" from inference into measurement. It
  logged at Warning under a `T6A-TEMP` prefix. It is a counter for a code path that no longer exists,
  so it is not permanent-quality observability; it was fully removed and the tree rebuilt clean before
  the commit. Its output is quoted above and remains in `build/t6a_server3.log`.
- **The reproduction did not exhibit a deficit**, so the attribution table's non-zero column is empty.
  The buckets are a fence against a future deficit, not a measurement of a live one. What is measured
  is (a) zero deficit across every round of the reproduction under a workload that (b) fires the
  historical predicate in 12 of them, and
  times.
- The brief offered `tests/clickhouse-test` as one way to drive the reproduction; a directly driven
  local server was used instead, because the deficit needs sustained concurrent appends across many
  rounds and many namespaces, which the single-namespace stateless test does not produce. The
  `05010`-shaped workload was additionally run against the same server for faithfulness.
