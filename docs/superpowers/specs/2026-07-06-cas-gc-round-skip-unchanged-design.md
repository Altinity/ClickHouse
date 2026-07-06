# CAS: GC Round Skip-Unchanged (O(delta), not O(universe)) — Design

**Date:** 2026-07-06
**Status:** Approved for planning (TLA+ gate mandatory before code)
**Scope:** Phase 4 of the 2026-07-06 night-findings fix plan — **Lever A only**: skip the
per-round in-degree snapshot rebuild when a round would make no destructive decision. Lever B
(incremental / point-updatable in-degree store) is explicitly out of scope (separate spec).

Related:
- `docs/superpowers/specs/2026-07-06-cas-night-findings-fix-plan-design.md` (§Phase 4)
- BACKLOG `S3-BUDGET — idle GC …`, `S3-BUDGET/SCALABILITY — GC round duration is O(ref universe)`
- The failure mode the gate must prevent: BACKLOG `GC-CONCURRENT-LEADER-LEAK` (2026-06-27 —
  an unfolded owner-removal event); this design adds the mirror hazard (an unfolded owner-**addition**
  under graduation).

---

## 1. Problem {#problem}

A regular GC round's dominant cost is the **in-degree snapshot rebuild**, and it is O(universe),
not O(delta):

- `Gc::fold` merges each shard's edge deltas into the running blob in-degree via
  `foldDeltasIntoGeneration` (`CasBlobInDegree.cpp`). That is a streaming two-cursor merge over the
  **entire** prior generation's source-edge run (`PriorEdgeCursor` over all `prior_runs`) producing a
  **new** consolidated run. So every round **reads the whole prior generation and writes a whole new
  one** — ~2×O(all blob source-edges) S3 I/O — even when a single edge changed, and even on a fully
  idle pool (the same pass drives the retired/pending pipeline).
- Measured: ~1362 `CasGcGet` (generation-run block reads) + ~643 `CasBlobHead` (B148 retire
  HEAD-storm) per **idle** round (~2500 S3 ops, nothing changing, 161 such rounds in one soak);
  round duration 87 ms @ 400 parts → 92.6 s @ 10k tables → 398 s @ 100k parts.

What is **already** O(delta) and is NOT the problem: `LIST(cas/refs/)` discovery is O(shards) with a
small constant (flat prefix, streamed pagination — the old "O(N²) over roots/" is fixed, ROADMAP
gate #16 closed); and the per-shard fold **read** already skips unchanged shards via token-diff
(`computeDiscoverDecisions` / `DiscoverDecision::Skip`). The snapshot rebuild is the remaining
O(universe) monster, and no LIST optimization touches it.

## 2. Goal & non-goals {#goal}

**Goal:** a round that would make **no destructive decision** does **not** rebuild the in-degree
snapshot — it re-adopts the already-sealed generation. Idle and small-delta rounds drop from
~2×O(universe) snapshot I/O to O(shards + servers). Correctness (no dangle, no leak) is preserved
exactly.

**Non-goals:**
- Lever B: making the snapshot rebuild itself O(delta) (point-updatable in-degree). Separate spec.
- Removing the `LIST(cas/refs/)` sweep (O(shards); a cheaper global change-signal is Lever B).
- The B148 retire HEAD-storm as a standalone fix (it is *incidentally* eliminated on non-folding
  rounds because those rounds do not retire; the stored-token retire remains its own ROADMAP item).

## 3. Core idea {#core-idea}

Split every round into a cheap **decision** phase and the expensive **fold** phase, and let the
decision phase choose FOLD or DEFER:

```
DEFER (re-adopt the sealed generation, do NOT touch the snapshot)  ⟺
      (changed-shard count < fold_threshold)
  AND (no destructive decision is due this round)
  AND (rounds-since-last-fold < fold_max_defer_rounds)
otherwise FOLD (rebuild the snapshot, folding ALL accumulated deltas).
```

Key properties:

- **The accumulator is the journals themselves.** DEFER simply does not advance the sealed
  `(snap_generation, snap_attempt)`. On the next round the token-diff is still taken against that
  same (older) sealed generation, so changed shards **accumulate naturally**; when a FOLD finally
  runs it reads every changed shard's journal from the sealed cursor and folds the whole accumulated
  delta in one batch. **No new persisted state.**
- **The decision uses only cheap signals** (all computed BEFORE the snapshot is touched):
  - *changed-shard set / count* — from `LIST(cas/refs/)` + token-diff (`computeDiscoverDecisions`),
    O(shards). This is known WITHOUT reading any journal.
  - *destructive-decision-due* — from the current retired list (`condemn_round` per pending entry)
    vs. the current heartbeat floor `min_ack`, plus whether any owner-removal exact-token body
    delete would fire. O(retired-entries) + O(servers). No journal read.
  - *defer window* — `rounds_since_last_fold`, tracked in `gc/state`. O(1).
- **Content (+1 vs −1) is NOT needed at decision time.** From LIST we know a shard *changed*, not
  *how*. That is sufficient, because a DEFER round makes no destructive decision (see §4).

## 4. Safety {#safety}

DEFER means the snapshot is **stale** (missing the deferred deltas). The two directions are
asymmetric:

- **Deferred `−1` (owner removed) is conservative-safe.** An unfolded removal leaves the snapshot
  in-degree **higher** than reality, so GC will not condemn a blob that actually dropped to zero.
  Worst case: reclaim is *delayed* (a new candidate is not detected until the deferred FOLD). Never
  a correctness violation. This is the exact 2026-06-27 leak mode, and DEFER cannot re-introduce it
  because deferral is bounded (§4.3) — every removal is eventually folded.
- **Deferred `+1` (owner added) is dangerous IFF a destructive decision runs on the stale
  snapshot.** A blob marked `delete_pending` at round K graduates to a physical delete once
  `min_ack > K` **and** its snapshot in-degree is still 0. If an unfolded `+1` re-referenced it, the
  stale snapshot says 0 while reality is 1 → **deleting a live-referenced blob = dangling.**

### 4.1 The invariant that closes the +1 hazard {#invariant}

> **No destructive decision is ever made on a not-fully-folded snapshot.**

Operationally: **any round that would delete anything forces a FOLD** (of all accumulated deltas)
before the decision. Because the FOLD reads the changed shards' journals — including the protective
`+1` — the graduation check runs on an up-to-date in-degree, and a re-referenced blob's graduation
is cancelled (`in-degree recovered → entry dropped`, the existing path at `CasGc.cpp:257-270`).
Between destructive decisions, DEFER accumulates freely.

"Destructive decision" enumerated (a round with ANY of these forces FOLD):
1. **Pending-delete graduation** — any retired `delete_pending` entry whose `condemn_round < min_ack`
   (i.e., due to graduate to a physical blob delete this round).
2. **Owner-removal body delete** — a folded `−1` that removes a manifest owner triggers an
   exact-token manifest-body delete (`mf_cleanup` / recheck). (This can only arise from folding, so
   a DEFER round — which does not fold — never triggers it; it is listed for completeness so the FOLD
   path is understood to own all deletes.)
3. Any other exact-token delete the round would issue (trim of a reclaimed shard, etc. — the plan
   enumerates these against the code and the FOLD path owns them).

Because a DEFER round folds nothing and deletes nothing, it is destructive-decision-free by
construction; the only question is whether a decision is DUE (signal #1), which is cheap to check.

### 4.2 New condemnations under DEFER {#new-condemnations}

A blob newly reaching zero in-degree (a new condemnation candidate) is detected only by folding.
Under DEFER it is simply detected later (at the next FOLD). This delays reclaim by at most the
defer window; it is never unsafe (an un-condemned blob is retained, not deleted).

### 4.3 Bounded deferral (liveness) {#liveness}

DEFER must be bounded so removals fold and reclaim makes progress:
- `gc_fold_max_defer_rounds` (§6, default 8) forces a FOLD regardless of delta size — the bound that
  matters only when batching is enabled (`gc_fold_threshold > 1`).
- A due destructive decision forces a FOLD (§4.1).
So every accumulated delta is folded within a bounded number of rounds; reclaim latency is bounded.

### 4.4 Concurrent leaders {#concurrent-leaders}

The round's `gc/state` advance is lease-guarded (attempt-scoped generation adoption, unchanged). A
DEFER round writes nothing to `gc/state` (§5), so it changes nothing durable — a deposed leader's
DEFER is inert. A FOLD round is the same single lease-guarded CAS as today. Leader/lease mutual
exclusion is **below this model's abstraction** (it is proven by `CaGcLeaseCore` + the attempt-scoped
generation adoption, unchanged here) — the `CaGcRoundDeferCore` gate models the DEFER/FOLD/graduate
hazard, which is leader-agnostic: the +1-over-delete failure exists even for a single leader, so
proving it single-leader is sufficient.

## 5. State-machine changes {#state-machine}

**No `gc/state` schema change.** The defer counter is **leader-local, in-memory**
(`rounds_since_last_fold`), not persisted:

- A **DEFER round is a pure no-op**: it does the cheap decision phase (LIST + token-diff, retired-vs-
  `min_ack` check, `min_ack` from heartbeats), decides DEFER, and **writes nothing to `gc/state`** —
  no new generation artifact, no retire, no delete. `(snap_generation, snap_attempt)` stays pinned by
  virtue of not being rewritten. The leader increments its in-memory counter.
- A **FOLD round** is unchanged from today except that the accumulated delta since the *sealed*
  cursor may span several rounds' worth of journal (the sealed cursor simply hasn't moved). It resets
  the in-memory counter.
- **Leader change is safe by construction:** a new leader starts `rounds_since_last_fold = 0`, so it
  can only fold *sooner*, never later — it never inherits a stale "I can keep deferring" budget. This
  is why the counter needs no persistence.

## 6. Configuration {#config}

- `gc_fold_threshold` (changed-shard count): default **1** — fold as soon as anything changed
  (batching OFF, exactly today's behavior; only zero-delta idle rounds ever DEFER). > 1 enables
  small-delta batching.
- `gc_fold_max_defer_rounds`: the liveness bound for batching mode — forces a FOLD after this many
  consecutive DEFER rounds even if still below threshold. Default **8**. **Inert at
  `gc_fold_threshold = 1`**: a zero-delta DEFER round has nothing to fold, so unbounded deferral of
  *nothing* is vacuously safe (`EventuallyFolded` holds with no unfolded edges); the bound only
  matters once `threshold > 1` lets non-zero deltas accumulate.
- Optional (plan may add): threshold by edge count instead of shard count, which costs a cheap
  O(changed-shards) journal read at decision time — still ≪ the O(universe) snapshot merge. Default
  off (shard-count trigger needs no journal read).

## 7. TLA+ gate (mandatory, before any code) {#tla}

Extend/author a model (candidate `CaGcRoundDeferCore`) over the fold/graduate/defer interleaving:

- **Actions:** `FoldRound` (fold accumulated delta, may condemn / graduate / delete), `DeferRound`
  (re-adopt, no snapshot change, no delete), `WriterAddEdge` (+1), `WriterRemoveEdge` (−1),
  `AckAdvance` (min_ack rises). (Single implicit leader — leader/lease mutual exclusion is below the
  model's abstraction, as in `CaGcAckFloorCore`; the +1/defer hazard is leader-agnostic.)
- **Invariants:**
  - `NoOverDelete` — a blob is never physically deleted while a live (folded-or-unfolded) `+1`
    reference exists (the +1 hazard). Equivalent to: every graduation is immediately preceded by a
    fold that covers all pending edges for that blob.
  - `NoLeak` / `EventuallyFolded` — every owner-removal edge is folded within a bounded number of
    rounds (deferral is bounded); no permanent skip (the 2026-06-27 mode).
- **Witness:** a small-delta round DEFERS and a later round FOLDS the accumulated delta and reclaims.
- **Sabotage configs (must produce counterexamples):**
  - `sab_graduate_on_stale` — allow graduation on a DEFER (stale) snapshot → must violate
    `NoOverDelete` (proves the force-fold-before-graduation rule is load-bearing).
  - `sab_unbounded_defer` — remove the defer bound → must violate `EventuallyFolded`/reclaim
    liveness.

Gate is GREEN before implementation. The plan's phase 0 is the TLA+ task.

## 8. Testing {#testing}

Unit (build/, `unit_tests_dbms --gtest_filter='CasGc*'`, SANITIZE=OFF):
- **Idle round re-adopts:** a round with zero changed shards and no due graduation makes zero
  generation-run reads/writes (assert via an instrumented backend op-count or the `CasGc*`
  ProfileEvents) and leaves `(snap_generation, snap_attempt)` pinned.
- **Small-delta defers then batch-folds:** with `gc_fold_threshold > 1`, N sub-threshold rounds
  DEFER (snapshot untouched), then a threshold-crossing (or `max_defer_rounds`) round folds the
  accumulated delta once; final in-degree equals the unbatched result (byte/þvalue-equal).
- **Force-fold before graduation (the +1 guard):** condemn a blob; while deferred, re-reference it
  (+1); advance `min_ack` so it is due to graduate; assert the round FORCE-FOLDS and the graduation
  is cancelled (blob survives, `dangling == 0`) — this test FAILS if graduation runs on the stale
  snapshot.
- **Bounded deferral:** `max_defer_rounds` forces a fold even below threshold.
- **gc_shards > 1** coverage (the reducer path).

Harness / live:
- The S03 idle-cost card gains an **ops/round budget assertion** (idle round S3 ops drop from ~2500
  to near-zero snapshot I/O).
- Re-run S03 / S05 / S08 on the fixed binary: idle rounds cheap; a small-delta round on the 10k-table
  pool far below 92.6 s; `dangling == 0` throughout.

## 9. Acceptance {#acceptance}

- Idle-pool round: no generation-run read/write; S3 ops ≈ O(shards + servers), not ~2500.
- Small-delta round on a large universe: amortized snapshot rebuild (one fold per batch), far below
  the current per-round 92.6 s @ 10k / 398 s @ 100k.
- `default` config (`gc_fold_threshold = 1`, no due-graduation deferral only) is behavior-identical
  to today except idle rounds skip the redundant snapshot rebuild.
- TLA+ gate GREEN (`NoOverDelete` + `EventuallyFolded`; both sabotage configs counterexample).
- Unit + live: `dangling == 0` preserved everywhere.
