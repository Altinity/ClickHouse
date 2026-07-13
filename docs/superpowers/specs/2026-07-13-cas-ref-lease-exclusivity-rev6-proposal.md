---
description: 'Proposal: solve ref-writer exclusivity at the mount-lease boundary; remove the grace window and publish-path replay from the ref protocol'
sidebar_label: 'CAS Ref Lease Exclusivity (rev.6 proposal)'
sidebar_position: 20260713
slug: /superpowers/specs/cas-ref-lease-exclusivity-rev6-proposal
title: 'CAS Ref Protocol rev.6 Proposal: Lease-Boundary Exclusivity'
doc_type: 'reference'
---

# CAS Ref Protocol rev.6 Proposal: Lease-Boundary Exclusivity {#cas-ref-lease-exclusivity-rev6}

**Date:** 2026-07-13
**Status:** PROPOSAL — awaiting user review; amends
[CAS Ref Table Snapshot and Log Design](2026-07-11-cas-ref-table-snapshot-log-design.md) (rev.5)
**Origin:** user design discussion during the task-3 soak, 2026-07-13. The user's directive:
exclusivity is solved once at the lease boundary; the ref protocol assumes a single writer by
construction and sheds every "someone else may still be writing" complication.

## Why rev.5's grace window must go {#why-grace-must-go}

The rev.5 containment for the [Late Predecessor PUT](2026-07-11-cas-ref-table-snapshot-log-design.md#late-predecessor-put)
is `snapshot_min_log_age_ms` (5 s): snapshot coverage lags the tail so a late-materializing
predecessor log stays above coverage, where the orphan-manifest sweep and the pre-delete recheck can
still read it. Live measurement (task-3 soak, 2 h, 6 workers) and code archaeology show three
defects, one of them semantic:

1. **Cost.** The 5 s grace floor keeps `tail_since_snapshot` permanently above the count threshold
   (~23 own txn/s per node × 5 s ≈ 115 entries > 64), so the trigger degenerates and one FULL state
   re-encode + `_snap` PUT runs per publish latency: measured ~20–26 GB/h per node against a ~4 GB
   pool, plus a full `base+tail` replay with a per-entry `RefTableState` copy under `state_mutex`
   per attempt (`CasStore.cpp:1985`).
2. **Divergent observers.** A log that materializes after this epoch's recovery LIST can never enter
   the live state (`applyRefLogTxn` strict-increase on `greatest_applied`), never enters
   `tail_since_snapshot` (no steady-state re-LIST), and therefore never enters any snapshot this
   writer publishes. Its only readers are cold folds (`GC`, `fsck`, sweep, recheck) in the window
   before a covering snapshot lands. During that window the durable fold-visible truth and the
   writer's in-memory truth disagree — observed in the wild as the `delete_pending retired entry
   recovered in-degree N — sparing` warnings. After coverage, the log becomes permanently invisible
   to everyone. The write is dropped either way (contract-clean: its author never received an ACK);
   grace only makes the drop non-deterministic and timing-dependent, and breaks snapshot
   byte-determinism (rev.5 admits this).
3. **Misplaced cost.** The residual hazard is rare and cold (a mount handover); the tax is paid on
   the hot path (every publish, every read-trigger evaluation).

## The rev.6 model {#rev6-model}

One sentence: **a writable mount begins only after the predecessor's writes provably cannot appear,
and the first snapshot published at mount seals everything the recovery LIST saw; from that moment
the protocol is single-writer by construction.**

### 1. Handover wait (`materialization_grace_ms`, T_mat) {#handover-wait}

Client-side attempt lifetimes are already bounded below the lease TTL
(`validateCasRequestBudget`: `attempt_timeout_ms + lease_safety_margin_ms < mount_lease_ttl_ms`, a
writable mount refuses to open otherwise). The one hazard no client timeout bounds is
**server-side late materialization**: an S3-accepted PUT whose client abandoned it can commit inside
the store later; process death kills the client, not the request already queued in the server.

Rule: before performing the recovery LIST, a mount whose predecessor ended **uncleanly** (lease
expired or fenced) waits until `predecessor_lease_end + materialization_grace_ms`. `T_mat` is a
generous configured allowance for the store to finish or drop accepted requests (default proposal:
30 000 ms; operators with a contractual server-side request bound may lower it).

**Clean-release fast path:** a clean unmount drains all lanes and releases the lease with a
`released_clean` marker; drained lanes + no wedge imply no in-flight conditional PUT can exist, so a
successor observing `released_clean` skips the wait entirely. Ordinary same-node restarts (clean
stop) therefore pay nothing; only crash/fence handovers pay `T_mat` — and they already pay the lease
TTL wait today.

### 2. Immediate recovery snapshot = the seal {#recovery-snapshot-seal}

At mount, after the recovery LIST and replay, if the listed predecessor region is non-empty the
writer **immediately publishes the recovery snapshot** (state as of the greatest listed txn id),
before acknowledging the mount as writable. Phase 1's objection to a per-table `_seal` object does
not apply: this adds no new object kind and no extra request class — it is the first `_snap` PUT,
merely moved from "lazily, after 64 appends" to "eagerly, at mount".

Effect: the existing coverage rule ("a log at or below the newest snapshot id is already covered")
becomes active from the first instant of the new epoch, for **every** observer uniformly — fold,
sweep, recheck, and future recoveries all read "snapshot + logs above it". A predecessor log that
materializes after the recovery LIST (i.e. after the T_mat window it had no right to outlive) is
deterministically invisible to all observers from birth. No divergence window exists at all.

Soundness of sealing at the listed maximum: the wedge discipline forbids a writer from allocating
ids past an own unresolved PUT, so at most ONE conditional log PUT per namespace can be in flight at
predecessor death, and its id is strictly above the last id whose outcome the predecessor resolved.
Either it materialized before the successor's LIST (then it is inside the seal and folded) or it
lands later strictly above the last listed id of its epoch — inside the sealed region, uniformly
ignored. There are no fillable holes below the seal. (Implementation must re-verify this invariant
against the lane code before relying on it.)

A namespace born in the current epoch has no predecessor region; its first snapshot may stay lazy.

### 3. Steady state: publish from the live state {#publish-from-live}

With no cross-epoch straggler able to appear (T_mat + seal), `snapshot_min_log_age_ms` is deleted
along with the whole grace machinery. Snapshot publication becomes the user's original scheme:

```text
after applying txn N under the flush leader (single-writer, no concurrency by construction):
  if aged-trigger fires: copy the live state once, hand it to the async publisher
  continue with N+1 immediately
publisher: encode + conditional PUT of _snap/N off the hot path
```

- Trigger: `count(txns above newest_snapshot_id) > snapshot_log_count_threshold` (raised 64 → 256
  per user; snapshot bytes are threshold-independent, the read-side fold pays ~8× more per-log GETs
  — accepted) or the existing bytes threshold.
- No replay, no `snapshot_base_state` reconstruction, no per-entry state copies, no `state_mutex`
  hold across encode. One state copy per publish, taken at a txn boundary.
- The publish-path `CasRefLatePredecessorObserved` counter and the grace-window selection loop are
  removed. `CasConditionalWriteFenceLostPostWrite` (post-write fence verify) STAYS — it measures a
  different thing (own stalled-thread containment).

### 4. What stays, explicitly {#what-stays}

Everything below is about the writer's own past self, not about "someone else", and survives rev.6:

- **`fence_ok` pre-attempt + post-write verify on every conditional log PUT.** A lease cannot stop
  an already-running thread of this process (whole-process freeze/thaw, cgroup-throttled beat,
  externally fenced live mount — the P3.1/S13 class observed in our own soak). Costs zero extra
  requests.
- **Resolve-before-reissue + wedge discipline** for the writer's own ambiguous PUTs (also
  load-bearing for the seal-soundness argument above).
- **T11 monotonic adoption guard** — the writer's own async publishes may still finish out of order.
- **Writer-side linearization, batching lanes, request budget validation** — unchanged.

## Spec-amendment checklist (rev.6, upon approval) {#amendment-checklist}

1. §Late Predecessor PUT: rewrite — the window is closed by construction (T_mat + recovery-snapshot
   seal); delete grace-age containment prose; keep the honest caveat that T_mat is an operational
   bound on store behavior, with the seal making any violation deterministic-invisible rather than
   silently divergent.
2. §Startup And Recovery: add the unclean-handover wait, the `released_clean` fast path, and the
   eager recovery-snapshot publish (mount is writable only after it commits).
3. §Snapshot Publication: replace the grace/replay algorithm with copy-once-from-live at the aged
   trigger; threshold default 256.
4. §S3 Request Budget / timeout-retry RFC: extend the tuning invariant to
   `unclean handover wait >= all client timeouts + T_mat`.
5. §Failure Handling: recovery-snapshot PUT failure at mount = mount fails to open writable
   (fail-closed; retry mount).
6. TLA+: extend the ref model — replace the `LatePredecessorPut` counterexample demonstration with
   an invariant `NoDivergentFold` (all observers agree on the folded set at every step) under the
   new mount rule; model the in-flight predecessor PUT as a message that can deliver at any time
   before `T_mat` and never after; keep the stalled-own-thread action to re-verify `fence_ok`.
7. Config: add `materialization_grace_ms`, delete `snapshot_min_log_age_ms`, default
   `snapshot_log_count_threshold` 64 → 256.

## Open questions for review {#open-questions}

1. Is `T_mat = 30 s` an acceptable unclean-handover penalty for production (it stacks on the lease
   TTL wait)? Note rustfs/MinIO/real-S3 differ in server-side request lifetimes.
2. Should the eager recovery snapshot also run on clean mounts with a large inherited tail (cost:
   one snapshot PUT; benefit: bounded recovery for the NEXT mount), or stay strictly
   unclean-handover-only?
3. The wedge/single-in-flight invariant the seal leans on: acceptable to hard-require (chassert +
   release fail-closed) at the lane level?
4. Interim sequencing: the mechanical patch (aged trigger + copy-once replay + threshold 256,
   grace semantics preserved) lands right after the task-3 soak regardless of this proposal —
   confirm.
