---
description: 'TLC evidence for the condemn-marker gate: PR1 configuration, sabotage rows and the regression battery'
sidebar_label: 'CAS condemn-marker TLC results'
sidebar_position: 5
slug: /superpowers/models/CaGcCondemnMarkerGate-results
title: 'CAS condemn-marker TLC results'
doc_type: 'reference'
---

# `CaGcCondemnMarkerGate` results {#cagccondemnmarkergate-results}

## Verdict {#verdict}

The PR1 configuration (gate always reads, no registry, retry stamps the current round, job deletes only at a round up to the entry's) is green in every required green row. Each sabotage row (`rv_bug`, `f1_bug`, `pm_g1_late_resolved_entry`, `pm_g1_nogateround_noregistry`, `pm_g1_nojob`, `pm_g1_norestamp_live`) violates the invariant named for it. Violation rows differ in count between runs because TLC stops at the first counterexample, which depends on worker scheduling.

Three violation rows are accepted residuals, not sabotage: `pm_g1_late_resolved_stale` and `pm_g1_late_gaveup` (a late Clean request that still matches the marker, spec sections 10.1 and 10.2 of `docs/superpowers/specs/2026-09-30-cas-gc-meta-delete-names-its-condemnation-design.md`) and `pm_g1_extdrop_noregistry` (an externally dropped edge, same spec, section 10.1). They show only that the model can find the loss PR1 does not close. `pm_g1_late_resolved_version` is green: it is the same schedule family with `EtagIsContent` FALSE (GCS), where the late request does not match recreated bytes.

## Checker and finite scope {#checker-and-finite-scope}

- TLC `2026.07.18.145032`, revision `30cc360`; `tla2tools.jar` SHA-256 `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`.
- 8 workers, `nice -n 10`, `-Xmx8g`, deadlock checking on. PR1 rows use `MaxGen 3`, `MaxRounds 7`, `TwoWriters`; regression rows keep their own values.
- Not shown: the read-ahead, a destructive request landing after a PR1 read, an old build's pending entries, and GC's own ambiguous writes. `GSettleGraduate` makes the gate read, the graduation and the round commit one step.
- Not shown: the code's refuse-and-retry after a failed marker read when the marker already guards the entry (`GSettleCarryRetry` refuses only when `GateOK` is false), more than one hash, more than one retired entry, more than two writer slots, more than one `GStaleAttempt` and one late request per behaviour, and liveness. The meta value is its bytes, so under `EtagIsContent` it stands for S3's content etag.

## Switches {#switches}

| Constant | PR1 value | Meaning |
|---|---|---|
| `GateOnMarker`, `GateAlwaysReads`, `GateRoundCheck`, `JobRoundCheck`, `Restamp` | `TRUE` | the gate reads the marker; round check; job round check; re-stamp |
| `RetryStampsCurrent` | `TRUE` | the gate's retry write stamps the refusing round, not the entry's |
| `SplitMetaDelete`, `SplitPublish`, `RealCandidate`, `Respawn` | `TRUE` | the code's shape |
| `Registry`, `RegRound` | `FALSE` | no in-process registry |
| `PersistConf` | either | the carried bit; `GateAlwaysReads` ignores it |
| `WriterLeavesMarker`, `DeleteReads`, `GateTouched`, `ForgetOnTouch`, `ExternalDrop` | `FALSE` | rejected candidates |
| `OutstandingReconcile` | `"none"` | `"resolved"` / `"gaveup"`: a reconcile leaves one Clean If-Match request in flight |
| `EtagIsContent` | `TRUE` | S3: equal bytes match; `FALSE`: only an unreplaced version matches (GCS) |
| `TrackRetry` | `FALSE` | records a completed retry write for `NoWedgedCarry` |
| `MarkerPrefetch` | `FALSE` | `TRUE`: the gate decides on a copy of the marker taken after the cut and before the gate (the graduation read-ahead of `CasGc.cpp`, `GraduationReadHints`); `GObserveMeta` takes the copy, `SeenMeta` is what `GateOK` tests. Issue #2486 |

Fault switches: `AsyncMarker`, `Prefetch`, `ReconcileFault`, `StaleMarkers`, `StaleAttempts`, `SuppressRounds`. `ExactAbsentPath` makes publication only into a key with neither body nor meta.

## Invariants {#invariants}

```tla
NoDangle == \A w \in Writers : committed[w] => body.present

MarkerPreservedPending == (ret.st = "pending" /\ TokCurrent) => Cond
MarkerPreservedConfirmed == (ret.st = "condemned" /\ GateConfirmed /\ TokCurrent) => Cond

JobWouldDelete(c) == ~JobRoundCheck \/ (Cond /\ meta.cr <= c)
JobSparesPendingGuard == \A c \in mdq : ~(ret.st = "pending" /\ TokCurrent /\ JobWouldDelete(c))

NoWedgedCarry == ~(ret.st = "condemned" /\ ret.retried /\ Cond /\ meta.cr < ret.cr)
```

`NoDangle` compares content presence, not tokens. `MarkerPreservedConfirmed` is vacuous under `GateAlwaysReads` and is kept for the regression cfgs. `NoWedgedCarry` checks the re-stamp, not eventual graduation.

## Required runs {#required-runs}

All cfgs are committed next to the module; `run_condemnmarker.sh` asserts the invariant by name. Counts come from the battery run before `Next` was split into one disjunct per action (a change that does not alter the state graph). Afterwards `bug`, `fix`, `pm_g1_noregistry`, `rv_bug` and `pm_g1_nojob` were rerun; the green counts are identical (`fix` 845, `pm_g1_noregistry` 199,936). `pm_g1_nojob` was rerun after its cfg lost the `StaleMarkers` switch and is the only row with a different count here (38,082 distinct at the first counterexample). The last two rows are the earlier sabotage cfgs rerun on the new module. The trace column was read from the counterexamples as state diffs.

| Cfg | Expected | Observed | Distinct | Seconds | Trace |
|---|---|---|---:|---:|---|
| `pm_g1_noregistry` | green | `green` | 199,936 | 1 | - |
| `pm_g1_all_noregistry` | green | `green` | 6,089,253 | 21 | - |
| `rv_g1_noregistry` | green | `green` | 536,095 | 3 | - |
| `pm_g1_f1` | green | `green` | 4,578,945 | 15 | - |
| `pm_g1_live` | green | `green` | 386,063 | 2 | - |
| `pm_g1_late_resolved` | green | `green` | 675,800 | 2 | - |
| `pm_g1_late_resolved_entry` | violation | `violation:NoDangle` | 380,644 | 1 | the carried entry is cr2 and the retry stamps the entry's round; a reconcile resolves Clean and leaves a request expecting Condemned(cr2); the retry recreates identical bytes Condemned(cr2); the entry graduates on it, then the late request lands (equal bytes) as Clean and the redelete removes the body under a landed edge. |
| `pm_g1_late_resolved_version` | green | `green` | 13,862,134 | 47 | - |
| `pm_g1_mpf_noregistry` | green | `green` | 370,480 | 2 | `pm_g1_noregistry` with `MarkerPrefetch`: the read-ahead copy decides, no loss |
| `rv_g1_mpf_noregistry` | green | `green` | 1,913,847 | 9 | `rv_g1_noregistry` with `MarkerPrefetch` |
| `pm_g1_mpf_late_resolved` | green | `green` | 1,147,984 | 4 | `pm_g1_late_resolved` with `MarkerPrefetch` |
| `pm_g1_late_resolved_stale` | violation | `violation:NoDangle` | 542,881 | 2 | residual. A stale marker write Condemned(cr1) is the version the reconcile read; the reconcile resolves Clean, its request stays in flight; the round-1 condemn's own marker job (not a retry) recreates identical bytes Condemned(cr1); the entry graduates, a second writer's edge lands, the late request lands Clean and the body is deleted under a landed edge. |
| `pm_g1_late_gaveup` | violation | `violation:NoDangle` | 298,083 | 1 | residual. The reconcile faults and gives up with a request in flight expecting Condemned(cr2); the entry graduates to pending on that unchanged marker; the late request lands Clean; the redelete removes the body under a landed edge. |
| `pm_g1_nogateround_noregistry` | violation | `violation:NoDangle` | 4,010,406 | 12 | gate without the round check accepts a stale Condemned(cr1) for an entry of round 4; the stale delete job removes then a writer's Clean lands; entry deletes the body under a landed edge. |
| `pm_g1_nojob` | violation | `violation:JobSparesPendingGuard` | 38,082 | 1 | `JobRoundCheck` off, the only constant that differs from `pm_g1_noregistry` (its invariant list is shorter); a queued job captured at round 1 would delete the Condemned(cr4) marker guarding the pending entry (the invariant fires one step before the delete). |
| `pm_g1_norestamp_live` | violation | `violation:NoWedgedCarry` | 21,959 | 0 | `Restamp` off; a writer republishes the body over Condemned(cr1) with a faulted reconcile; the entry is superseded at round 2 and its marker job leaves Condemned(cr1) in place; the retry write at round 3 completes (`retried`) and the marker is still older than the entry's round 2. |
| `pm_g1_extdrop_noregistry` | violation | `violation:NoDangle` | 46,592 | 1 | residual. The first writer's edge is dropped externally while its reconcile is owed; a second writer's edge lands; the first writer's owed reconcile then writes Clean over Condemned(cr2) and the entry deletes the body under a landed edge. |
| `pm_g1_mpf_extdrop` | violation | `violation:NoDangle` | 85,254 | 1 | the same residual with `MarkerPrefetch`: the read-ahead adds no loss class. Every green row above stays green with the flag (`*_mpf_*`); the only violation it reaches is the externally dropped edge, which the marker read at the gate does not close either. |
| `rv_bug` | violation | `violation:NoDangle` | 14,238 | 1 | a publisher's Clean overwrites the entry's Condemned(cr1) marker; the in-process confirmation lets the entry graduate; body deleted while a writer's edge landed. |
| `f1_bug` | violation | `violation:NoDangle` | 1,299,275 | 3 | stale job deletes the marker, a new writer publishes over the absent marker and cleans, the entry (cr4) graduates on the old in-process confirmation and deletes the body under a landed edge. |

## Regression {#regression}

### Reproducible with committed cfgs {#regression-committed}

Promoted cfgs against the last committed RESULTS (`4dd5f1ac11b`). The module was replaced, not extended: the earlier module had different writer actions, so the counts moved while every result stayed the same.

| Cfg | Last committed distinct | Now distinct | Result |
|---|---:|---:|---|
| `fix` | 808 | 845 | `green` |
| `fix_pconf` | 768 | 803 | `green` |
| `fix_prefetch` | 1,337 | 1,385 | `green` |
| `fix_both` | 1,285 | 1,331 | `green` |
| `fix_both_2w` | 20,574 | 22,171 | `green` |

### Recorded counts, cfgs not committed {#regression-recorded}

Earlier battery rerun with the four constants at their inert values. Its cfgs are not committed, and several names (`fix`, `fix_both`, `rv_bug`, `f1_bug`) denote different configurations from the committed cfgs of the same name, so these rows are recorded counts, not reproducible from this directory. Green rows match in distinct states. Violation rows are first-counterexample counts and vary between runs. Rows that were not green before (`pm_g7_all`, `pm_g1w1_extdrop`, `pm_g7_big`, `pm_g1_big`) keep the same result as before.

| Cfg | Before: result | Before: distinct | After: result | After: distinct |
|---|---|---:|---|---:|
| `atomic_regress` | `green` | 20,802 | `green` | 20,802 |
| `bug_gateround` | `violation:NoDangle` | 52,084 | `violation:NoDangle` | 51,468 |
| `bug_jobdelay` | `violation:NoDangle` | 25,886 | `violation:NoDangle` | 25,397 |
| `bug_restamp` | `violation:NoDangle` | 55,888 | `violation:NoDangle` | 54,907 |
| `bug_stale` | `violation:NoDangle` | 114,328 | `violation:NoDangle` | 119,983 |
| `fix_both` | `green` | 104,271 | `green` | 104,271 |
| `fix` | `green` | 52,645 | `green` | 52,645 |
| `fix_splitpub` | `violation:NoDangle` | 34,382 | `violation:NoDangle` | 34,979 |
| `fix_stale_big` | `green` | 2,591,915 | `green` | 2,591,915 |
| `fix_stale` | `green` | 719,379 | `green` | 719,379 |
| `job_only` | `green` | 40,197 | `green` | 40,197 |
| `job_only_stale` | `violation:NoDangle` | 294,797 | `violation:NoDangle` | 308,988 |
| `nogate_stale` | `violation:NoDangle` | 311,874 | `violation:NoDangle` | 296,958 |
| `norestamp_stale` | `violation:NoDangle` | 281,131 | `violation:NoDangle` | 301,088 |
| `old_bug_both_2w` | `violation:NoDangle` | 7,830 | `violation:NoDangle` | 4,298 |
| `old_bug_both` | `violation:NoDangle` | 1,068 | `violation:NoDangle` | 847 |
| `old_bug` | `violation:NoDangle` | 455 | `violation:NoDangle` | 455 |
| `old_fix_both_2w` | `green` | 22,171 | `green` | 22,171 |
| `old_fix_both` | `green` | 1,331 | `green` | 1,331 |
| `old_fix` | `green` | 845 | `green` | 845 |
| `old_fix_pconf` | `green` | 803 | `green` | 803 |
| `old_fix_prefetch` | `green` | 1,385 | `green` | 1,385 |
| `rv_bug` | `violation:NoDangle` | 17,866 | `violation:NoDangle` | 9,981 |
| `rv_bug_mp` | `violation:MarkerPreservedPending` | 3,177 | `violation:MarkerPreservedPending` | 2,471 |
| `rv_bug_async` | `violation:NoDangle` | 14,165 | `violation:NoDangle` | 10,372 |
| `f1_bug` | `violation:NoDangle` | 1,333,161 | `violation:NoDangle` | 1,280,796 |
| `f1_bug_mp` | `violation:MarkerPreservedPending` | 321,419 | `violation:MarkerPreservedPending` | 382,988 |
| `pm_bug` | `violation:NoDangle` | 90,777 | `violation:NoDangle` | 89,130 |
| `pm_bug_all` | `violation:NoDangle` | 1,641,510 | `violation:NoDangle` | 1,613,975 |
| `pm_overapprox` | `violation:NoDangle` | 61,647 | `violation:NoDangle` | 68,444 |
| `pm_g1_nogateround` | `violation:NoDangle` | 35,332,696 | `violation:NoDangle` | 44,130,658 |
| `pm_g1_extdrop` | `violation:NoDangle` | 73,907 | `violation:NoDangle` | 58,910 |
| `pm_g1_wit` | `violation:WitNoDeleteAfterRefusal` | 11,400 | `violation:WitNoDeleteAfterRefusal` | 12,760 |
| `pm_g7_mpc` | `violation:MarkerPreservedConfirmed` | 5,511 | `violation:MarkerPreservedConfirmed` | 3,551 |
| `pm_g7_naive` | `violation:NoDangle` | 164,032 | `violation:NoDangle` | 133,738 |
| `pm_w1_stale` | `violation:NoDangle` | 9,526,734 | `violation:NoDangle` | 4,130,740 |
| `pm_g2_all` | `violation:NoDangle` | 1,890,991 | `violation:NoDangle` | 2,966,783 |
| `pm_g1w1_extdrop_stale` | `violation:NoDangle` | 6,626,988 | `violation:NoDangle` | 3,361,495 |
| `rv_g1` | `green` | 1,951,931 | `green` | 1,951,931 |
| `f1_fix` | `green` | 37,162,840 | `green` | 37,162,840 |
| `pm_nosplit` | `green` | 37,162,840 | `green` | 37,162,840 |
| `pm_g1` | `green` | 241,547 | `green` | 241,547 |
| `pm_g1_all` | `green` | 108,547,117 | `green` | 108,547,117 |
| `pm_g1_noreg` | `green` | 108,547,117 | `green` | 108,547,117 |
| `pm_g1_overapprox` | `green` | 57,644,865 | `green` | 57,644,865 |
| `pm_g1_norestamp` | `green` | 79,572,691 | `green` | 79,572,691 |
| `pm_g7_all` | `violation:MarkerPreservedPending` | 180,871 | `violation:MarkerPreservedPending` | 121,690 |
| `pm_w1` | `green` | 1,532,050 | `green` | 1,532,050 |
| `pm_g1w1_extdrop` | `violation:MarkerPreservedPending` | 57,061 | `violation:MarkerPreservedPending` | 53,780 |
| `f1_fix_big` | `green` | 241,031,093 | `green` | 241,031,093 |
| `pm_g1_big` | `timeout` | not recorded | `timeout` | about 388,000,000 at the 1,200 s timeout |
| `pm_g7_big` | `violation:MarkerPreservedPending` | 261,366 | `violation:MarkerPreservedPending` | 148,701 |

## Big run {#big-run}

`CaGcCondemnMarkerGate_pm_g1_big_noregistry.cfg`: `green`, 25,807,236 distinct states, 90 s (cap 7200 s). It is not part of `run_condemnmarker.sh`; run it with TLC directly. The count was observed before `Next` was split per action and was not rerun after it.

