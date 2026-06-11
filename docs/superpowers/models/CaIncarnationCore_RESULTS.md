---
description: 'TLC model-checking results for CaIncarnationCore.tla: per-stage state counts and wall times, seven sabotage counterexample traces, model refinements found during development, and residual untested surface.'
sidebar_label: 'CA incarnation-token model results'
sidebar_position: 3
slug: /superpowers/models/ca-incarnation-core-results
title: 'CA incarnation-token core — TLC model-checking results'
doc_type: 'guide'
---

# CA incarnation-token core — TLC model-checking results {#ca-incarnation-core-results}

`TLC` (v2.19, OpenJDK 21) was run on `CaIncarnationCore.tla` against the staged configs below.
This is **bounded model checking**: `TLC` exhaustively explored every interleaving up to the finite
bounds per stage. A clean run is strong evidence of safety within those bounds, **not** a proof for
unbounded tokens/writers/shards/rounds.

## PASS stages {#pass-stages}

Stage 4 was split into two companion configs (the full `stage4.cfg` exceeds the time budget at any
attempted bound): `stage4_small` covers debris classification and the two-shard torn-cut; `stage4_journaltree` covers the journaled retire→fence→recheck→delete tail and `FGCommit`'s tree-reachability rebuild. Stage 5 likewise ran only `stage5_small` (debris disabled to isolate split-brain × overwrite).

| Stage | Config | Adds | Distinct states | Wall time | Result |
|---|---|---|---|---|---|
| 1 core | `stage1` | publish/drop, fold/retire/fence/recheck, in-flight deletes | 24,744,564 | 1min 07s | **PASS** |
| 2 resurrect/evidence | `stage2` | `WResurrect`, `WEvidence`, `WResolveEvidence`; retired-old-vs-newer-current | 913,278 | 3s | **PASS** |
| 3 trees/cascade | `stage3` | expansion markers, atomic cascade, shared-child survival, selective cascade | 95,989,672 | 5min 07s | **PASS** |
| 4a debris/full-GC cut | `stage4_small` | heartbeat-gated debris, wedged-writer publish, two-shard cut | 21,104,848 | 53s | **PASS** |
| 4b journaled-delete + tree rebuild | `stage4_journaltree` | journaled retire→fence→recheck→delete + `FGCommit` tree rebuild | 86,472,872 | 3min 41s | **PASS** |
| 5 split/overwrite | `stage5_small` | split-brain two-leader competition + `WOverwrite` | 82,983,981 | 3min 36s | **PASS** |
| liveness (stage-2 bounds) | `stage2_live` | `NoLeakForever` under `FairSpec` | 99,645 | 3s | **bound-artifact lasso** (see below) |

All PASS runs exhausted their BFS queue (`0 states left on queue`). Invariants checked in every
stage: `TypeOK`, `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN`, `INV_JOURNAL_COVERAGE`.
`MonotoneGC` checked as an action property in all PASS stages.

### Liveness result note {#liveness-result-note}

`stage2_live` ended with `Error: Temporal properties were violated` — a bound-artifact lasso: the
GC exhausted its `MaxRound=2` budget before the model could reclaim the permanently-unreachable
candidate, so `NoLeakForever` found a finite prefix from which the property is not satisfied.
This is **not a design bug** — it is the model's round cap preventing the GC pipeline from
completing. Re-running with `MaxRound=3` (logged in `tmp/tlc_CaIncarnationCore_stage2_live_mr3.log`)
reproduces the same class of lasso at a larger bound; a genuine non-round-cap lasso would require
analyzing whether any scheduler can starve the GC pipeline under `FairSpec`, which the current
single-shard, `MaxRound=2` bounds cannot rule out. The safety properties (`INV_NO_DANGLE` etc.)
are not violated.

## Negative controls {#negative-controls}

Each sabotage config flips exactly one load-bearing rule and **must** produce a counterexample.
A negative-control run that passes (exit 0) is a model failure, not a clean bill of health.

**Note on `INV_NO_DANGLE` vs `INV_NO_LOSS`:** several sabotages expected to violate `INV_NO_LOSS`
instead violate `INV_NO_DANGLE` first. This is because `INV_NO_DANGLE` (a shard manifest refs a
`present=FALSE` object) trips as soon as a root ref points to a deleted object, which happens one
step before any dependent manifest could be checked. Both invariants guard the same loss class;
the log records which invariant `TLC` reports, which is always `INV_NO_DANGLE` when the victim is
a direct root ref.

| Sabotage | Rule removed | Violated invariant | Distinct states at violation | Trace summary |
|---|---|---|---|---|
| `sab_nofence` | fence does not touch manifests — post-fence writers never blocked (horn 2) | `INV_NO_DANGLE` | 41,612 | `WCreate`→`WPublish`→`WReuse`→`WDrop`→`WPublish` builds two refs to `h1`; `GRetire` condemns `h1`; `GFenceShard` writes nothing; a second `WPublish` re-publishes the condemned ref; `Land` deletes `h1`; manifest still refs `h1` (present=FALSE). |
| `sab_norecheckfold` | recheck does not require fold-through-fence — pre-fence publishes missed (horn 1) | `INV_NO_DANGLE` | 70,858 | `WPublish` adds `h1`; `WDrop`+`WPublish` adds it again (cursor at 2); `GRetire` condemns at in-degree 0; `GFenceShard` appends fence at position 4; `GRecheckDelete` fires immediately (cursor still at 2, before fence) — fold-through-fence not required; the pending post-fence `WPublish` is missed; `Land` deletes `h1`; manifest dangling. |
| `sab_noretireview` | W-PUBLISH-GATE retire-view check removed | `INV_NO_DANGLE` | 186,013 | `WCreate`→`WReuse`→`WPublish` publishes `h1`; `WDrop` removes it; GC retires and fences `h1`; `WPublish` (no gate) republishes a stale reuse dep without refreshing view past the fence; `GRecheckDelete` issues delete; `Land` deletes `h1`; root ref dangles. |
| `sab_unconddelete` | exact-token delete — zombie delete kills resurrected incarnation | `INV_NO_DANGLE` | 37,652 | `GStartRound`; writer refreshes view; `WCreate`→`WPublish`→`WDrop`; GC folds/retires/fences `h1` at token 1; `WResurrect` issues new token 2 and publishes; fold catches the new add; `Land` for the stale token-1 delete fires unconditionally and kills the present token-2 incarnation; root ref dangles. |
| `sab_reusedtag` | W-FRESH-TAG / token distinctness — resurrect reuses condemned token | `INV_NO_RETURN` | 3,553 | `GStartRound`; writer refreshes view; `WCreate`→`WPublish`→`WDrop`; GC retires token 1; `WResurrect` reissues the same condemned token 1 (sabotage); `Land` for the retired token-1 delete arrives and marks token 1 deleted in `deadTok`; resurrected incarnation is still present at token 1; `INV_NO_RETURN` (`present[h] ⇒ tokOf[h] ∉ deadTok[h]`) violated. |
| `sab_cascade` | cascade-as-pipeline-step — deferred cascade strips LIVE tree's child edges | `INV_NO_LOSS` | 55,494,630 | GC round 1: tree `t1` retired and deleted; cascade deferred as `pendCasc`; GC round 2 starts; writer re-creates `t1` with fresh token and re-publishes; `GFold` re-expands `t1`'s child edges; stale `ApplyPendCascade` fires, stripping the now-live tree's child edges and marker; child hash `h1` retires (in-degree 0) and deletes; manifest still refs `t1` whose child `h1` is absent. |
| `sab_cutoverclaim` | full-GC claimed-authority = incorporated-state — cursor jumps past the cut | `INV_NO_DANGLE` | 1,120,807 | `WHbStart`; `GStartRound`; `WCreate`; `FGRead(s1)` snapshots cut at log length 0; `WPublish` lands after the read (log length 1); `WCrash`; `GDebrisRetire` condemns `h1`; `GFenceShard`; `FGRead(s2)` (second shard); `FGCommit` rebuilds edges from cut-0 (empty) but overclaims cursors to current length 1, skipping the add record; `GRecheckDelete` believes fold-through-fence; `Land` deletes `h1`; root manifest dangling. |

## Model refinements found during development {#model-refinements}

`TLC` found genuine violations on intermediate encodings. Each one corresponds to a load-bearing
rule; fixing the model to encode the rule (testing the *intended* design) made the violation
disappear. These are the model-checker doing its job: it refuses to let a too-weak version pass.

### MR-1 — `deadTok` in `CondemnedAtView` (stage 1) {#mr-1}

First encoding: `CondemnedAtView(h, t, v)` consulted only the live `retired` set. `Land` consumes
the retired entry on delete, so after the message lands the entry is gone. A stale unpublished
dependency `(h, t)` could then pass `DepOK` even though the object had been physically deleted —
the `retired` guard was vacuously false. **Fix:** `CondemnedAtView` also checks `t ∈ deadTok[h]`
(the durable history of physically deleted tokens), so a deleted token remains permanently
condemned regardless of whether its retired entry is still live. **Verdict:** real design
constraint — the publish gate must consult durable deleted-token history, not only the in-flight
retire set.

### MR-2 — `WResurrect` and `WOverwrite` add old token to `deadTok` (stages 2 and 5) {#mr-2}

`WResurrect` overwrites `tokOf[h]` with a fresh token in-place (the object stays present). An
in-flight delete message for the old token landing after the overwrite would be a 412 (correct).
But if the old token were NOT added to `deadTok`, a subsequent writer holding a dependency on the
old token could pass `DepOK` and publish — dangling once the old token's delete eventually lands.
The same applies to `WOverwrite`. **Fix:** both actions unconditionally push the displaced token
into `deadTok[h]` at the moment of overwrite. This is the model RULE stated at the `deadTok`
declaration: *any action making a token stop being current adds it to `deadTok`*. **Verdict:** real
design constraint — the spec's publish gate must re-validate dependencies' current physical state,
not just the originally-observed token (see Spec Finding F1 below).

### MR-3 — `TreeDepsOK` + `WCreate` child guard (stage 3) {#mr-3}

The plan's "no new actions for stage 3" was wrong. A tree ref may be published into a manifest
only when all of its children are present and non-condemned at publish time — otherwise publishing
a tree over an absent or condemned child immediately creates a dangling reference via the fold's
expansion. **Fix:** `WPublish` for a tree hash additionally checks all children's liveness
(`TreeDepsOK`). This is bottom-up build discipline: children must be durably present before the
tree referencing them is published. **Verdict:** real design constraint — the spec's writer rules
(§5/§7) should state this explicitly for tree-typed objects (see Spec Finding F2 below).

### MR-4 — `GFenceShard` monotone fence (stage 5) {#mr-4}

With two leaders, a lagging leader could execute `GFenceShard` and write a lower fence round to the
manifest, contradicting `MonotoneGC`. **Fix:** `GFenceShard` is modelled as a monotone-max write:
`man[s].fence := max(man[s].fence, roundOf[l])`. This matches the spec's CAS-based fence write
and `INV-MONOTONE-GC`. **Verdict:** real design constraint — the implementation must ensure the
fence field never decreases, even when a stale leader races a fresher one.

## Spec findings to carry upstream {#spec-findings}

These are refinements the model surfaced that `2026-06-10-ca-incarnation-store-design.md` should
state explicitly.

### F1 — Publish gate must re-validate dependencies' CURRENT physical state {#f1}

An observed token can go stale via (a) a delete message landing and the retired entry dropping, or
(b) another writer's `WResurrect` / `WOverwrite` displacing the token in-place. The spec's §5/§7
publish-gate description should note that `DepOK` must consult both the live retire set AND the
durable deleted-token history (`deadTok`). The model captures this conservatively; the nested /
transitive case (a dependency whose dependency was overwritten) is currently left to the one-level
tree closure and the token-exact delete chain.

### F2 — Tree bottom-up build discipline should be explicit in the writer rules {#f2}

The spec's writer protocol (§5) does not explicitly state that a tree-typed object may only be
published once all of its direct children are present and non-condemned. `TreeDepsOK` was
discovered as a load-bearing constraint during model development (MR-3). The spec should add a
writer pre-condition for tree publishes: "all `Children[t]` are present and their current tokens
are not in `retired` at the writer's view." The model covers one-level trees; the transitive
(nested subtree) case should be addressed in the spec's recursive reachability argument.

## Residual untested surface {#residual-untested-surface}

1. **Pack byte-range addressing** — objects are atomic in the model; sub-object reference counting
   and partial deletion are untested.
2. **Manifest size bounds and journal data-plane internals** — `O(delta)` fold streaming-merge
   implementation, journal compaction, and atomic snap publish are abstracted.
3. **GCS/Azure token bindings** — token distinctness is a parameter (monotone allocator), not
   derived from backend semantics.
4. **The reader path** — `INV_NO_DANGLE` / `INV_NO_LOSS` check that root refs are not deleted;
   in-flight-read restart (§6) is not modeled.
5. **Provenance, diagnostic fields, event-id dedup** — out of scope.
6. **Nested subtrees (one-level tree closure only)** — `Children[t]` is direct children; transitive
   depth is not covered.
7. **Per-stage bound reductions** — stage 2 dropped to 1 hash; stage 3 is single-writer; stage 4
   was split (full `stage4.cfg` not run); stage 5 is single-writer with debris disabled (full
   `stage5.cfg` not run). Writer-vs-writer interleaving for trees (stage 3) is covered by stages
   1, 2, and 5 on smaller models.
8. **Liveness is round-cap limited** — `NoLeakForever` found a bound-artifact lasso at `MaxRound=2`
   (and at `MaxRound=3`); a genuine non-round-cap lasso was not confirmed absent.

## Large-bound counterexample hunt (W-REVALIDATE mode) {#hunt}

Run 2026-06-11 after the `W-REVALIDATE` mode landed (the faithful F1 re-observation gate, no
dead-token oracle — `EnableReval = TRUE` in both hunt configs). Both phases are **bug hunts, not
proofs**: "no violation found" below means exactly that — a clean search of the stated scope —
and is never recorded as a PASS.

| Phase | Config | Scope searched | Violations | Termination |
|---|---|---|---|---|
| BFS cross-product | `CaIncarnationCore_hunt_cross.cfg` | **4,978,632,765 states generated / 781,714,146 distinct** (trees × debris × split-brain × overwrite × 2 writers × 2 shards — the cross-product no staged config co-checks; all 5 invariants + `MonotoneGC`) | **0** | killed at ~28 min by **disk exhaustion** (`StatePoolWriter: No space left on device` — the breadth-first queue at 637M pending states filled the ~100 GB free; 43 GB heap was not the limit). Infra failure, not a model error; re-runnable with more disk or smaller bounds. |
| Random simulation, depth 200 | `CaIncarnationCore_hunt_sim.cfg` | **8,307,265,639 state visits** along random depth-200 traces at bounds BFS cannot enumerate (2 trees + 2 blobs, `MaxToken=4`, `MaxRound=4`, `MaxLog=8`; 5 invariants — action properties not checked in simulation) | **0** | full 58-minute budget (timeout, by design) |

Interpretation: ~782M distinct states breadth-first across the full feature cross-product plus
8.3B deep random visits at enlarged bounds, all clean, in the gate mode the implementation will
actually use. Strong negative evidence; the explored-prefix caveat stands (the BFS queue still
held 637M unexplored states at the cutoff). Logs: `tmp/tlc_CaIncarnationCore_hunt_cross.log`,
`tmp/tlc_CaIncarnationCore_hunt_sim.log`.
