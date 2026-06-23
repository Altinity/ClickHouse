---
description: 'TLC model-checking results for CaB140Dangle.tla: the Phase-1 refinement that expresses and reproduces the B140-dangle data-loss bug, the pinned producer trace, and the flag-off safety confirmation.'
sidebar_label: 'CA B140-dangle producer model'
sidebar_position: 4
slug: /superpowers/models/ca-b140-dangle-results
title: 'CA B140-dangle — Phase 1 producer model results'
doc_type: 'guide'
---

# CA B140-dangle — Phase 1 producer model results {#ca-b140-dangle-results}

Phase 1 of the B140-dangle fix (spec `docs/superpowers/specs/2026-06-17-ca-b140-dangle-fix-design.md`).
`CaB140Dangle.tla` is a FOCUSED, self-contained model whose only job is to EXPRESS and REPRODUCE
the B140-dangle data-loss bug. It is deliberately NOT the full `CaIncarnationCore`: it abstracts
away writers / heartbeats / registry / evidence and keeps exactly the machinery the producer needs
— a live root referencing a tree `T` whose child is a blob `B`, a PER-GENERATION GC snapshot store,
NON-ATOMIC tree expansion, a marker-retaining cascade strip, byte-equal cross-leader generation
adoption, and a lease-steal that orphans a generation. Editing the 1000-line core in place would have
required touching ~20 stage configs and every `UNCHANGED` tuple; a focused copy is safer and keeps
the producer legible.

## The refinements vs `CaIncarnationCore` {#refinements}

1. **Split, NON-atomic expansion.** The core's `GFold` sets a tree's child edges and its
   `markExpanded` marker ATOMICALLY. Here, under `EnableSplitExpand`, expansion is two sub-actions:
   `GAddTreeEdges(T)` (records the `T->B` edges) and `GMarkExpanded(T)` (sets the sticky marker) —
   so an interleaving can set the marker without (all) edges.
2. **Per-generation snapshots.** `snap` is `[0..MaxGen -> {marker, treeEdges, rootEdges, everEdged,
   cursor}]`, with `gcState = [snapGeneration, cursor]` as a SEPARATE durable pointer (the core had a
   single global `marker`/`treeEdges`/`cursor`). A round builds the NEXT generation and commits it
   to `gcState` only if it kept the lease.
3. **Byte-equal generation adoption.** `GAdoptGeneration` models the implementation's `putIfAbsent`
   byte-equal adoption / probe-upward: because the snap codec does NOT record the fold cursor as
   part of a generation's IDENTITY, two divergent-fold generations at the same NUMBER are
   byte-comparable, and a leader can adopt a generation that MIXES one fold's sticky marker with
   another fold's (stripped) edges.
4. **Marker-retaining cascade strip.** `GStripTree` (gated by `EnableInBuildStrip`) clears `T`'s
   `treeEdges` while KEEPING the sticky `marker` and `B` in `everEdged` — `GcSnap::stripTree` /
   the cascade strip, faithful to the marker being durable and not re-derived from the edge set.
5. **Lease-steal / orphaned generation.** `GStealLease` lets another leader grab the lease between
   retire/cascade and the closing `gc/state` CAS, leaving the victim's persisted generation orphaned
   (snap written, `gcState` not advanced).
6. **`INV_MARKER_EDGES`.** New invariant: in the current durable snap generation, every
   `markExpanded` tree that is still live (`InDeg > 0` from the LIVE manifest refs) has ALL its
   `tree->blob` child edges present.

An incarnation-token abstraction (`tok`, exact-token `GDeleteLand`) is included so the model can
distinguish a BENIGN relink-after-issue TOCTOU (the relink re-incarnates `T`, so a stale in-flight
delete `412`s) from the REAL B140 loss (the blob `B` is never re-incarnated, so its exact-token
delete succeeds). A fence/recheck-fold barrier on `GRetire`/`GIssueDelete` (`CurGen.cursor =
Len(log)` + in-degree recheck) models the core's `FoldedThroughFence` + `GRecheckDelete`, so the
simple stale-cursor dangle is excluded and the residual loss is attributable to GENERATION
incoherence, not a missing recheck.

## Run ledger {#run-ledger}

`TLC` (jar at `tmp/tla2tools.jar`), bounds `Leaders={L1,L2}`, `Trees={t1}`, `Blobs={b1}`,
`MaxGen=3`, `MaxLog=4` unless noted. All runs sub-second.

| Config | Flags | Result | Distinct states |
|---|---|---|---|
| `CaB140Dangle_safe` | all producer flags OFF | **PASS** (all invariants) | 3,292 (0 left on queue) |
| `CaB140Dangle_producer` | all ON | counterexample `INV_MARKER_EDGES` | 436 |
| `CaB140Dangle_blob` | all ON | counterexample `INV_BLOB_NOT_CONDEMNED` | 2,672 |
| `CaB140Dangle_loss` | all ON | counterexample `INV_NO_LOSS` | 6,979 |
| `CaB140Dangle_adopt` | strip OFF, adopt+steal ON | **PASS** at `MaxGen=3` | 52,766 (0 left on queue) |
| `safe` (stress) | OFF, `Blobs={b1,b2}` `MaxGen=4` `MaxLog=5` | **PASS** | 11,427 |
| `adopt` (stress) | strip OFF, `MaxGen=4` `MaxLog=5` | `INV_MARKER_EDGES` only; `INV_BLOB_NOT_CONDEMNED` **PASS** (300,777 states) | — |

## The pinned producer {#pinned-producer}

The shortest data-loss trace (`CaB140Dangle_loss`, 12 steps), single leader `L1`, one tree `t1`,
one blob `b1`:

1. `GAcquireLease(L1)` — `L1` takes the lease, builds generation 1.
2. `WUploadBlob(b1)` — child blob uploaded.
3. `WPublishTree(t1)` — `t1` published live (`refs={t1}`), journal `<<add t1>>`.
4. `GFold(L1)` — folds the `add t1` into gen 1: `rootEdges={t1}`, `everEdged={t1}`, cursor 1.
5. `GAddTreeEdges(L1,t1)` — records the edge `<<t1,b1>>` and adds `b1` to `everEdged`.
6. `GMarkExpanded(L1,t1)` — sets the sticky marker: gen 1 = `marker={t1}`, `treeEdges={<<t1,b1>>}`,
   `everEdged={t1,b1}`. (Coherent so far.)
7. `GStripTree(L1,t1)` — the cascade strip clears the edge but KEEPS the marker: gen 1 becomes
   `marker={t1}`, `treeEdges={}`, `rootEdges={t1}`, `everEdged={t1,b1}`. **`INV_MARKER_EDGES`
   already broken** — `t1` is live and marked but its `t1->b1` edge is gone.
8. `GCommitSnap(L1)` — commits gen 1 as the durable `gcState.snapGeneration=1`, cursor 1 =
   `Len(log)` (fully folded through the fence).
9. `GRetire(L1,b1)` — `b1` is present, `everEdged`, and `InDegGen(gen1,b1)=0` (no root ref to `b1`,
   no tree edge to `b1` — it was stripped), so `b1` is condemned. The fence/recheck barrier does NOT
   save it: the snap IS fully folded; it is INTERNALLY incoherent.
10. `GIssueDelete(L1,b1)` — recheck still computes in-degree 0; an EXACT-token (`tok[b1]=0`) delete
    of `b1` is issued.
11. `GDeleteLand(b1)` — exact-token hit (b1 was never re-incarnated): `present[b1]:=FALSE`. **A blob
    the live tree `t1` still references is gone — `INV_NO_LOSS` violated.**

The `CaB140Dangle_blob` counterexample is steps 1–10 (it fires `INV_BLOB_NOT_CONDEMNED` at the
issued exact-token delete, one step earlier).

## Does it match the hypothesis? {#hypothesis-match}

The spec hypothesized a **cross-generation incoherence**: a generation mixing `T`'s sticky
`markExpanded` marker with a DIFFERENT generation's stripped edges, enabled by the cursor-less
byte-equal codec — concretely via a lease handoff + relink + byte-equal adoption.

The model confirms the **structural root cause** exactly: the durable `gcState` generation carries
`markExpanded(T)` with `T` live and the `T->B` edge ABSENT but `B` still `everEdged`, so retire
computes `InDeg(B)=0` and the single exact-token delete site loses a live-referenced blob. It also
refines the producer attribution:

- **The marker-retaining strip is the load-bearing step.** `GStripTree` — a cascade strip that
  clears the edges but leaves the sticky marker and `B` in `everEdged` in the SAME durable
  generation — produces the data loss directly, and at the smallest bounds. This is the minimal
  pin.
- **Byte-equal cross-leader adoption is a confirmed but WEAKER arm.** With the in-build strip OFF
  (`CaB140Dangle_adopt`), adoption + lease-steal reach the STRUCTURAL incoherence
  (`INV_MARKER_EDGES` violated at `MaxGen=4`) — a marker-gen byte-equal-mixed into an edge-stripped
  gen — but did NOT reach an EFFECTIVE delete of the live tree's blob within the explored bounds
  (`INV_BLOB_NOT_CONDEMNED` exhausted, 300,777 states). So the cross-leader adoption mechanism is a
  real producer of the bad SNAPSHOT but the model could not, in these bounds, drive it all the way
  to a delete without the marker-retaining strip also being available.
- **The lease-steal (`GStealLease`) is modeled and available but not required** by the shortest
  producer; it broadens the orphaned-generation interleavings rather than being the trigger.

So the hypothesized lease-handoff / orphaned-generation / relink mechanism is **partially**
confirmed: the cursor-less byte-equal generation IS a structural enabler of the incoherent snapshot
(adoption reaches `INV_MARKER_EDGES`), but the concrete DATA-LOSS producer in this model is the
marker-retaining cascade strip within a single durable generation. The fix in Phase 2 (a fold
watermark that makes generations non-mixable, plus a fail-closed coherence guard) must therefore
close BOTH: the adoption mixing AND the marker-retaining strip leaving a live marked tree without
its child edge.

## Flag-off safety {#flag-off-safety}

`CaB140Dangle_safe` (all producer flags off — atomic expansion, coherent fold, no strip / steal /
adopt) PASSES all five invariants (`TypeOK`, `INV_MARKER_EDGES`, `INV_NO_LOSS`, `INV_NO_DANGLE`,
`INV_BLOB_NOT_CONDEMNED`) with the BFS queue exhausted, and continues to pass at a larger stress
bound (`Blobs={b1,b2}`, `MaxGen=4`, `MaxLog=5`). The refinement is inert when disabled.

## Residuals {#residuals}

- Single tree / single blob / single shard / one-level closure — same scope as the core's tree
  stages; nested subtrees and `snap_shards>1` are not modeled (the latter is forbidden today).
- The data-loss arm via PURE cross-leader byte-equal adoption (no in-build strip) was not reached as
  an effective delete within `MaxGen<=4`; larger bounds were not exhausted. The structural
  `INV_MARKER_EDGES` violation via that arm IS reached, which is sufficient to motivate the Phase-2
  watermark closing the adoption mixing.
- This is bounded model checking, not a proof; a clean `safe` run is strong evidence of inertness
  within the bounds, not unbounded.
