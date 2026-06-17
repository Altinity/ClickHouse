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

## B91 refresh (2026-06-12) — amended protocol, full re-run {#b91-refresh}

The model was refreshed onto the 2026-06-12 protocol amendments (namespace registry + manifest
creation; evidence staleness) and the ENTIRE suite re-run. The historical tables below record the
original runs; this section is authoritative for the current model.

**Semantic changes** (all flag-gated; old configs keep identical reachable behavior — stage-1's
pre-edit re-run reproduced its 24,744,564 distinct states bit-for-bit before `ViewableRound`
landed):

- `EnableRegistry`: `reg` bundle (registered set, manifest-exists flags, registry fence,
  REGISTRATION-TIME floors, per-leader fence universe), `WRegister`, `GFenceRegistry` (ends the
  round's retiring; captures the FENCE-TIME universe), `GFenceShard` minting + universe-bound,
  `WPublish` floor `PubFloor = max(man.fence, reg.floor)`, `INV_MAN_EXISTS`.
- `EnableEvStale`: `wEv` evidence with recorded view round, `WEvObserve` re-observation, `EvOK`
  gate (no hash hit + fresh), dep-set `TreeDepsOK` (no global presence oracle), `WEvidence`
  requires reachability (live-root is literal).
- `ViewableRound` (unconditional): a view refresh claims round R only after R's retiring ended —
  the implementation's `gc/state.round` coverage property. Shrinks all stage state counts.

**Three fixes machine-derived during the refresh:**

1. **C++ fence-universe hole** (commit `724eb5363ff`): `Gc::fence` fenced the FOLD-TIME registry
   universe; a namespace registered between the fold's registry read and the registry-fence CAS
   fell between both horns. Fixed to the committed registry-fence universe;
   `sab_foldtimeuniverse` is this exact hole as a permanent negative control.
2. **Model ordering, implementation-faithful**: the registry fence must END the round's retiring
   (R2 strictly before R3 step 0) and view rounds must carry the coverage property
   (`ViewableRound`) — both found as TLC counterexamples against the first model draft, both
   already true of the C++ (sequential round; `gc/state.round` advances at R2's CAS).
3. **C++ blind `adoptTree`** (commit `a247e29c125`): whole-root adoption recorded tokenless
   evidence without observing the object — a reclaimed detached tree could be re-attached as a
   dangling ref past every gate. Fixed to cold-reuse (`observeAndAdmit`); spec `W-EVIDENCE` SCOPE
   amended.

**Re-run ledger (2026-06-12, all queues exhausted on PASS):**

| Config | Result | Distinct states | Wall |
|---|---|---|---|
| `stage1` | **PASS** | 20,931,058 | 1min 08s |
| `stage2` | **PASS** | 155,142 | 2s |
| `stage3` | **PASS** | 52,770,710 | 3min 41s |
| `stage4_small` | **PASS** | 16,844,184 | 55s |
| `stage4_journaltree` | **PASS** | 35,576,464 | 1min 49s |
| `stage5_small` | **PASS** | 64,359,811 | 3min 29s |
| `reval_stage2` | **PASS** | 155,142 | 1s |
| `stage6_registry` (new) | **PASS** (incl. `INV_MAN_EXISTS`, `MonotoneRegistry`) | 3,700,390 | 12s |
| `stage6_evstale` (new) | **PASS** | 10,143,569 | 36s |
| `stage6_cross_smoke` (new, review follow-up) | **PASS** (incl. `INV_MAN_EXISTS`, `MonotoneRegistry`) | 439,696 | 2s |
| `stage2_live` | bound-artifact lasso (expected, unchanged class) | 78,761 | 3s |
| `sab_nofence` | counterexample `INV_NO_DANGLE` ✓ | 32,014 | 1s |
| `sab_norecheckfold` | counterexample `INV_NO_DANGLE` ✓ | 57,108 | 1s |
| `sab_noretireview` | counterexample `INV_NO_DANGLE` ✓ | 159,414 | 1s |
| `sab_unconddelete` | counterexample `INV_NO_DANGLE` ✓ | 28,957 | 1s |
| `sab_reusedtag` | counterexample `INV_NO_RETURN` ✓ | 5,997 | 0s |
| `sab_cascade` | counterexample `INV_NO_LOSS` ✓ | 38,719,755 | 2min 22s |
| `sab_cutoverclaim` | counterexample `INV_NO_DANGLE` ✓ | 793,197 | 2s |
| `sab_noreobserve` | counterexample `INV_NO_DANGLE` ✓ | 28,777 | 0s |
| `sab_noregistry` (new) | counterexample `INV_NO_DANGLE` ✓ | 36,347 | 0s |
| `sab_foldtimeuniverse` (new) | counterexample `INV_NO_DANGLE` ✓ | 29,911 | 0s |
| `sab_noevreobserve` (new) | counterexample `INV_NO_LOSS` ✓ | 626,806 | 2s |

State counts shrink vs the historical tables because `ViewableRound` prunes unimplementable early
views (a refresh during a round's retiring no longer claims that round). The
`sab_foldtimeuniverse` trace reproduces the C++ hole shape exactly: `WRegister` lands between
`GStartRound`'s (sabotaged) universe capture and `GFenceRegistry`; the late namespace's publish is
never fenced or recheck-folded and the exact-token delete dangles its ref.

**External review follow-ups (2026-06-12, accepted with the refresh):** `MonotoneRegistry` action
property added and checked in registry-enabled configs (append-only `reg.ns`, monotone
`reg.fence`/`reg.man`, immutable registration floors; `reg.univ`/`reg.done` are per-round work
state, deliberately not monotone); the three mixed `/\`-`\/` guards parenthesized for
reviewability (`EvOK` freshness, `WAbandon` enabling condition, `WPublish` W-REGISTER line —
semantics unchanged, stage1 reproduces 20,931,058 states exactly); `stage6_cross_smoke` added
(registry × evidence small-bounds interaction check — not a proof of the combined space). The
reviewer's `ViewableRound`-under-split-leaders nuance (global `gcRound` vs per-leader/durable
round) is folded into the B104 registry × split-brain residual.

**B91 residuals:** registry × split-brain and registry × evidence at FULL bounds not run (state
space; `stage6_cross_smoke` covers the small-bounds interaction); one model shard = one namespace
(per-namespace `root_shards > 1` not modeled); the model's hit checks read the live `retired`
set, weaker than the implementation's frozen LIST snapshot (conservative direction);
`CaIncarnationProofCore.tla` (Apalache induction) predates the amendments and is STALE until
re-derived — the amendments add proof obligations (permanent registration floors, fence-time
universe, view-round coverage, evidence freshness/re-observation, dep-covered tree children).

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

## Apalache induction results {#apalache-induction-results}

Tool: **Apalache 0.58.0** (build `711dce6`, Z3 bundled), proof core `CaIncarnationProofCore.tla`.
Induction bounds: `|Writers|=2`, `|Shards|=1`, `|Hashes|=2`, `MaxToken=3`, `MaxRound=2`,
`MaxLog=4` (the `CInit` constant initializer). Harness: `StateShape`/`IndInvInit` idiom —
function-domain-pinned `Gen` assignments so `TypeBounds` reads land on defined entries; `IndInvInit
== StateShape /\ IndInv`.

### Summary {#apalache-summary}

| Check | Command | Outcome | Wall time |
|---|---|---|---|
| Base case (`Init => IndInv`) | `--init=Init --inv=IndInv --length=0` | **NoError** | 1s |
| Step check (`IndInv => IndInv'`) | `--init=IndInvInit --inv=IndInv --length=1` | **NoError** | 45s |
| Step check (reproduced) | same | **NoError** | 72s |

`IndInv` (19 conjuncts) is **INDUCTIVE** at the stated bounds after 12 CTI iterations.
Soundness rests on the base+step pair, not on the TLC pre-filter.

### Final IndInv — 19 conjuncts {#indinv-conjuncts}

The conjuncts are grouped by origin; each is a named operator in `CaIncarnationProofCore.tla`.

**9 v1 candidates (Task 4 — TLC-prefiltered before any SMT; 2 needed correction):**

| Conjunct | One-line meaning | TLC pre-filter note |
|---|---|---|
| `TypeBounds` | domains the unbounded Apalache Int/Seq types don't carry | green |
| `NoDangle` | every manifest ref points to a `present` object — TARGET | green |
| `NoReturn` | a present object's token is never in `obsoleteTok` — TARGET | green |
| `InflightHeld` | in-flight `(h,t)` implies entry held OR `t` displaced (`tokOf[h] # t`) | **corrected**: v1 had no disjunct; TLC pre-filter found the spared-branch orphan case (spec refinement `8eda0a5ba76` — the IN-FLIGHT DISJUNCTION) |
| `RetiredCurrentOrDead` | a retired token is still current or is in `obsoleteTok` | green |
| `InflightVsRefs` | an in-flight-deletable token is never a referenced current token | green |
| `SendImpliesFenced` | a delete is sent only after every shard's fence covers its entry's round, OR the token is displaced | **corrected**: same disjunctive form for the same orphan case |
| `RefsFromLog` | manifest refs = full-log fold (logical pre-compaction manifest) | green |
| `SnapFromPrefix` | snap edge-set = cursor-prefix fold | green |

**4 early CTI facts (CTIs #1–4, before the orphan detour):**

| Conjunct | One-line meaning | CTI # |
|---|---|---|
| `InflightAllocated` | `d.t < nextTok[d.h]` — in-flight token was allocated | #1 |
| `PhaseRoundActive` | active GC phase implies `gcRound >= 1` | #2 |
| `AbsentObsolete` | absent key has `tokOf=0` or `tokOf ∈ obsoleteTok` | #3 |
| `RootEdgesTyped` | `rootEdges ⊆ Shards × Hashes` (type doesn't restrict domain) | #4 |

**`InflightCurrentUnreferenced` — the irredundant heart conjunct (CTIs #5–6 detour):**

| Conjunct | One-line meaning |
|---|---|
| `InflightCurrentUnreferenced` | an in-flight delete on a STILL-CURRENT token has no folded root edge and no unfolded add in any journal suffix beyond the cursor |

CTI #6 was a vacuity trap: a candidate fact (`tokOf[d.h]=d.t => InDeg(d.h)=0`) passed the
single-hash TLC pre-filter vacuously because `GFold` of a stale add can re-snap `h2` — impossible
at `Hashes={h1}` but reachable at `Hashes={h1,h2}`. Pre-filter policy adjusted: inter-hash facts
additionally require a bounded two-hash TLC run (see below).

**5 fence-discipline conjuncts (CTIs #7–11, pulled in by `InflightCurrentUnreferenced`'s preservation):**

| Conjunct | One-line meaning | CTI # |
|---|---|---|
| `FenceCoverage` | `fencedSet` shards have `man[s].fence >= gcRound`; `"fenced"` phase iff all shards fenced | #7 |
| `FencePosRecord` | `fencePos[s] > 0` implies the record at that position is a fence entry | #8 |
| `FenceLeRound` | no shard's fence is strictly ahead of the active GC round | #9 |
| `RetiredCoveredNoPostFenceAdd` | for a retired-current entry covered by a shard's fence, that shard has no add of the entry's key after its latest fence record | #10 |
| `RetiringFenceBelow` | during `"retiring"` phase every shard's fence is strictly below the active round | #11 |

### Full CTI journal {#cti-journal}

| # | Violated conjunct | CTI summary | Fact added |
|---|---|---|---|
| 1 | `InflightHeld` | phantom `inflight [h, nextTok[h]]`; `WOverwrite` makes it current | `InflightAllocated` |
| 2 | `TypeBounds` (`e.r` range) | `gcPhase="retiring"` at `gcRound=0`; `GRetire` stamps `r=0` | `PhaseRoundActive` |
| 3 | `RetiredCurrentOrDead` | absent `h` with stale nonzero `tokOf` not in `obsoleteTok`; `WCreate` advances | `AbsentObsolete` |
| 4 | `InflightHeld` | junk fresh-shard `rootEdge` inflates in-degree; spared-branch orphan | `RootEdgesTyped` |
| 5 | `InflightHeld` | spared branch drops retired entry whose `[h,t]` is in flight and still current | `InflightCurrentUnreferenced` (after #6 detour) |
| 6 | (rejected candidate) | `GFold` of stale add re-snaps `h2`; single-hash TLC passed a FALSE fact vacuously | candidate reverted; pre-filter policy adjusted |
| 7 | `InflightVsRefs` | fenced with `fence=0 < gcRound`; ungated post-fence add; stale-snap condemn | `FenceCoverage` |
| 8 | `InflightVsRefs` | `fencedSet={s}` with `fencePos=0`, NO fence record; trivial `FoldedThroughFence` | `FencePosRecord` |
| 9 | `InflightVsRefs` | future fence (`fence=2` at `gcRound=1`) satisfies coverage; stale snap | `FenceLeRound` |
| 10 | `InflightVsRefs` | unfolded POST-fence adds of retired-current `h2`; condemned off stale snap | `RetiredCoveredNoPostFenceAdd` |
| 11 | `RetiredCoveredNoPostFenceAdd` | `GRetire` in `"retiring"` with `fence=gcRound`: new entry born covered | `RetiringFenceBelow` |
| 12 | — | **PASS** | — |

### Negative controls {#apalache-negative-controls}

| Control | What was tested | Outcome | Finding |
|---|---|---|---|
| `IndInv => NoDangle` | implication (length=0) | **NoError** | `NoDangle` is a conjunct — trivially implied |
| `IndInv => NoReturn` | implication (length=0) | **NoError** | `NoReturn` is a conjunct — trivially implied |
| drop `InflightCurrentUnreferenced` (`IndInv_NoICU`) | step check on weakened invariant | **Error** (CTI) | irredundant — the spared-branch orphan pattern breaks `InflightHeld`; ICU is the heart conjunct |
| drop `InflightHeld` (`IndInv_NoHeld`) | step check on weakened invariant | **NoError** | documented redundancy: `IndInv_NoHeld => InflightHeld` confirmed by implication check; after strengthening, `InflightHeld` is a corollary of `InflightCurrentUnreferenced` + the fence-discipline family |
| drop `InflightVsRefs` (`IndInv_NoRefs`) | step check on weakened invariant | **NoError** | documented redundancy: `IndInv_NoRefs => InflightVsRefs` confirmed by implication check; same reason |
| gate control `WPublishNoReval` via `--next=NextNoReval` | step check with re-observation conjunct removed from `WPublish` | **Error** (CTI breaking `NoDangle`) | machine-checked F1 witness: stale dep on deleted object (`present=FALSE`, `tokOf=0` vs dep token 2) passes the weakened gate and publishes `h1` into `man.refs`; `W-REVALIDATE` is what carries F1 in the inductive argument |
| `FalseInv` satisfiability | `--init=IndInv --inv=FalseInv --length=0` | **Error** (counterexample produced) | `IndInv` is satisfiable — the induction is not vacuously true |

### Pre-filter policy and state counts {#prefilter-policy}

Every conjunct was TLC-pre-filtered before any Apalache SMT run.

| Run | Scope | Distinct states | Result |
|---|---|---|---|
| Single-hash TLC exhaustive (`Hashes={h1}`) | complete BFS, depth 30 | **694,265** | green throughout all 12 iterations |
| Two-hash bounded TLC (`CaIncarnationProofCore_tlc2h.cfg`, `Hashes={h1,h2}`) | BFS through level 23, 600s cap | **254.8M** | 0 violations at cap |

Policy: per-hash facts are exhaustively pre-filtered at `Hashes={h1}`. Inter-hash facts (anything
coupling `inflight`/`retired` on one hash with edges/journal records of another hash) additionally
require the bounded two-hash run. Induction soundness does NOT rest on the pre-filter — a false
lemma necessarily fails the Apalache base or step check; the pre-filter is an efficiency and
diagnosis device.

### Honest scope {#honest-scope}

The induction quantifies over ALL states satisfying `IndInv` at FIXED constants (`|Writers|=2`,
`|Shards|=1`, `|Hashes|=2`, `MaxToken=3`, `MaxRound=2`, `MaxLog=4`) — unbounded depth, bounded
constants. This is the middle rung: stronger than `TLC` bounded-checking (no depth limit) but
weaker than a parametric proof (constants are not symbolic). Constant-parametric generality is
TLAPS territory; `IndInv` and the CTI journal above are the prepared input for that effort.

Proof-core residuals (recorded scope exclusions): single leader (split-brain covered by the TLC
main model), no trees/debris/evidence, logical pre-compaction manifest (`RefsFromLog` — physical
trimming covered by `INV_JOURNAL_COVERAGE` in the TLC model), uniform journal record encoding
(the main model's heterogeneous `AddRec ∪ FenceRec` is a uniform `{op: Str, hs: Set(HASH)}` in
the proof core).

## P9 (2026-06-17): `GForget` — prune absent zero-in-degree nodes from `everEdged` {#p9-gforget}

To eliminate the regular-GC `retire` 404-HEAD storm, the implementation removes a deleted node from
the in-degree snapshot's `known` set (the model's `everEdged`). The model gains one action,
`GForget(h)`, enabled when `~present[h] /\ h \in everEdged /\ InDeg(h)=0`, with effect
`everEdged' = everEdged \ {h}`. It is added to `Next` unconditionally — a single always-enabled
action conservatively covers BOTH implementation prune sites (the cascade's delete-time prune and
the retire observe loop's HEAD-404 prune): if the invariants hold when the prune may lag the delete
arbitrarily, they hold when it fires atomically at the delete.

**Frame argument (why safety is preserved by construction).** `GForget` modifies ONLY `everEdged`.
None of the safety invariants reference `everEdged`: `INV_NO_DANGLE` (refs⊆present), `INV_NO_LOSS`
(reachable⊆present), `INV_NO_RETURN` (present⇒tok∉deadTok), `INV_JOURNAL_COVERAGE`
(trimBase≤cursor), `MonotoneGC`, `MonotoneRegistry`, `TypeOK`. So `GForget` preserves every safety
invariant independent of scope. The only property reading `everEdged` is the liveness `NoLeakForever`.

**Results.**

| run | cfg | property set | states | time | result |
|---|---|---|---|---|---|
| safety, no trees, 1 hash | `stage2` | TypeOK + 3 INV + MonotoneGC | 206,696 | 3s | **no error** |
| safety, no trees, 2 hashes | `stage1` | TypeOK + 3 INV + MonotoneGC | 44,879,094 | 2m17s | **no error** |
| safety, trees, 4 hashes | `stage3` | TypeOK + 3 INV + MonotoneGC | 65,564,923 | 4m27s | **no error** |
| liveness | `stage2_live` | `NoLeakForever` | 81,555 | 3s | bound-artifact lasso (SEE NOTE) |
| sabotage sanity | `sab_unconddelete` | INV set | 33,208 | 1s | **still violates `INV_NO_DANGLE`** (not masked) |

**Liveness note (no regression).** `stage2_live` reports the SAME `MaxRound=2` bound-artifact lasso
documented above (a permanently-unreachable candidate not reclaimed within the round budget). It is
NOT introduced by `GForget`: running `stage2_live` against the model with `GForget` removed produces
the identical counterexample (768,100 states, exit 13) — the `GForget` step does not even appear in
the trace. The pre-existing round-cap interpretation stands unchanged.

**`stage4`/`stage6` not re-run exhaustively.** Adding an always-enabled `GForget` after every delete
multiplies interleavings; `stage4` (2 shards + debris) did not converge in practical time. Given the
frame argument (no safety invariant reads `everEdged`), the safety verdict is independent of the
larger scope, and `stage1`/`stage2`/`stage3` exhaustively confirm it across the no-tree and tree
shapes. The `GForget` delta is, by inspection, scope-independent for safety.
