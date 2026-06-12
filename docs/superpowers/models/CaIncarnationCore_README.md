---
description: 'TLA+ bounded model-checking runbook for CaIncarnationCore.tla — the incarnation-token CA store GC model (spec 2026-06-10-ca-incarnation-store-design.md §12 + Appendix A). Covers staged configs, flag table, what is and is not modeled, and how to reproduce the runs.'
sidebar_label: 'CA incarnation-token model runbook'
sidebar_position: 2
slug: /superpowers/models/ca-incarnation-core-readme
title: 'CA incarnation-token core — TLA+ model runbook'
doc_type: 'guide'
---

# CA incarnation-token core — TLA+ model runbook {#ca-incarnation-core-readme}

A TLA+ specification (`CaIncarnationCore.tla`) of the incarnation-token content-addressed (CA)
store GC, model-checked with `TLC` to exhaustively hunt safety violations within finite bounds.
The source design is `docs/superpowers/specs/2026-06-10-ca-incarnation-store-design.md` §12 +
Appendix A. This model replaces the superseded `CaGcCore.tla` (the EBR/epoch/generation design,
now kept as historical record; see `README.md`).

This is **bounded model checking, not a proof**: `TLC` exhausts all interleavings up to the finite
bounds below. A clean check is strong evidence of safety within those bounds, not a theorem for
unbounded tokens/writers/shards.

## What is modeled {#what-is-modeled}

One key per content hash (`Hashes`); tokens are naturals allocated per key by `nextTok` — the
spec's `W-FRESH-TAG` and backend token-distinctness hold **by construction** in the model
(monotone per-key allocator). Actions:

- **`WCreate`** — allocates a fresh token, sets `present[h] = TRUE`, records `(h, tok)` in the
  writer's dependency set. Tree creates (`h ∈ TreeHashes`) are additionally guarded on all
  children being present and non-condemned at publish time (`TreeDepsOK`, see refinement note).
- **`WReuse`** — cold reuse: observes the current token; records a token-bearing dependency.
- **`WResurrect`** — resurrects a condemned current incarnation with a **fresh** token
  (`W-FRESH-TAG`). The old token is unconditionally added to `deadTok[h]` at the overwrite point —
  this is the model RULE: *any action that makes a token stop being current adds it to `deadTok`*.
- **`WEvidence` / `WResolveEvidence`** — tokenless live-root evidence dependency (carry-forward /
  fetch-by-reference); escalates to a token-bearing dependency when the retire-view is clean.
  Under `EnableEvStale` (B91, 2026-06-12) the evidence carries the view round it was RECORDED at
  (the amended `W-EVIDENCE` staleness input), requires the object to be REACHABLE when observed
  (live-root is literal — the implementation witnesses it by reading the live source tree), and
  the publish gate (`EvOK`) admits it only fresh (`r ≥ wView`) with no hash hit; stale members
  must be re-observed via **`WEvObserve`** (present ⇒ token-bearing dep, absent ⇒ dropped).
- **`WRegister` / `GFenceRegistry`** — under `EnableRegistry` (B91, 2026-06-12): a namespace must
  be CAS-appended to the registry before any publish (`W-REGISTER`); the registering read takes
  the current registry fence as the namespace's publish-gate floor (`reg.floor`). The GC fences
  the registry FIRST (`GFenceRegistry`, ending the round's retiring — the view-coverage ordering)
  and derives the shard-fence universe from the COMMITTED registry (fence-time, never fold-time);
  `GFenceShard` MINTS absent manifests. `ViewableRound` gives view rounds the COVERAGE PROPERTY
  (a refresh claims round R only after R's retiring ended — `gc/state.round` semantics).
- **`WOverwrite`** — anonymous environment churn (unconditional same-content re-PUT by anyone);
  same RULE: the displaced token joins `deadTok`. No dependency recorded.
- **`WPublish`** — one atomic successful CAS guarded on the CURRENT manifest (CAS linearization).
  Gate: `wView[w] ≥ man[s].fence` AND `DepOK(w)` (no condemned/hit dependency at the writer's
  view). `SabotageNoRetireView` removes the gate.
- **`WDrop`** — removes a root ref; append-only journal entry.
- **`WAbandon`** — crash/abort before publish; uploads remain (debris in stage 4).
- **`GStartRound` / `GEndRound`** — GC round lifecycle; `gcRound` monotone.
- **`GFold`** — folds one journal record into the snap edge-set; expansion-marker rule for trees.
- **`GRetire`** — retires a journal-known, present, in-degree-0 candidate at its current token.
- **`GFenceShard`** — bumps `man[s].fence` and records a fence entry in the journal (CAS-modelled
  as a monotone-max write in stage 5 to prevent a lagging leader lowering the durable fence);
  `SabotageNoFence` skips the manifest write entirely.
- **`GRecheckDelete`** — recheck after fold-through-fence: spared entries drop from `retired`;
  condemned entries stay in `retired` until the delete message lands; `Land` is the confirmed
  outcome. `SabotageNoRecheckFold` drops the `FoldedThroughFence` pre-condition.
- **`Land`** — exact-token delete message lands (412 = no-op). The retired entry drops here, not
  at send time. For trees, cascade (strip child edges + expansion marker) is **atomic with the
  landing** — the round-pipeline rule. `SabotageCascadeRace` defers the strip.
  The landing also adds the deleted token to `deadTok[h]`.
- **`Trim`** — advances `trimBase[s]` below the folded cursor (`INV_JOURNAL_COVERAGE`).
- Stage-4: `WHbStart`, `WHbRenew`, `Wedge`, `WCrash`, `GObserveHb`, `GDebrisRetire`,
  `FGRead`, `FGCommit` — heartbeat-gated debris classification and exact-cut full-GC walk.
  `GFenceShard` is additionally CAS-modelled as monotone max in stage 5 (`MonotoneGC` property).

**Key modeling rules:**

- `deadTok[h]` is a durable history set: any action making a token non-current adds it (delete
  land, `WResurrect`, `WOverwrite`). `INV_NO_RETURN` checks `present[h] ⇒ tokOf[h] ∉ deadTok[h]`.
- `TreeDepsOK` (bottom-up build discipline): a tree ref may be published only when its children
  are present and non-condemned (see refinement note in `CaIncarnationCore_RESULTS.md`).
- `GFenceShard` is monotone-fence: under split-brain the fence CAS is modelled as a monotone-max
  write (prevents a lagging leader lowering the durable fence; matches the spec's `INV-MONOTONE-GC`).
- Retire entries are checked against the live `retired` set; dropped entries are safe to miss
  (deleted/absent ⇒ reuse becomes a fresh create; replaced/spared ⇒ current token is not
  condemned) — mirrors the spec's entry-lifecycle argument.

## Flags table {#flags-table}

| Constant | Effect |
|---|---|
| `EnableResurrect`, `EnableDebris`, `EnableOverwrite` | gate actions (stage 2 / 4 / 5 resp.) |
| `EnableTrees` | documentation only — non-empty `TreeHashes` activates tree machinery |
| `EnableSplit` | documentation only — `\|Leaders\| = 2` activates split-brain leader competition |
| `EnableReval` | W-REVALIDATE gate mode: no dead-token oracle; re-observation conjunct |
| `EnableRegistry` | B91: namespace registry + manifest creation (`WRegister`, `GFenceRegistry`, minting); registry-enabled configs also check the `MonotoneRegistry` action property (append-only `reg.ns`, monotone `reg.fence`/`reg.man`, immutable registration floors) |
| `EnableEvStale` | B91: evidence staleness + dep-set tree-child validation (`wEv`, `WEvObserve`, `EvOK`) |
| `Sabotage*` | negative controls; exactly one `TRUE` per sabotage config |

## How to run {#how-to-run}

```bash
# from docs/superpowers/models; jar at ../../../tmp/tla2tools.jar (v2.19, OpenJDK 21)
./run_tlc.sh CaIncarnationCore_stage1.cfg
./run_tlc.sh CaIncarnationCore_stage2.cfg
./run_tlc.sh CaIncarnationCore_stage3.cfg
./run_tlc.sh CaIncarnationCore_stage4_small.cfg
./run_tlc.sh CaIncarnationCore_stage4_journaltree.cfg
./run_tlc.sh CaIncarnationCore_stage5_small.cfg
./run_tlc.sh CaIncarnationCore_stage2_live.cfg     # temporal: bound-limited (see RESULTS)
# B91 stages (2026-06-12 amendments):
./run_tlc.sh CaIncarnationCore_stage6_registry.cfg
./run_tlc.sh CaIncarnationCore_stage6_evstale.cfg
# negative controls — these MUST fail with an invariant violation:
for c in nofence norecheckfold noretireview unconddelete reusedtag cascade cutoverclaim \
         noreobserve noregistry foldtimeuniverse noevreobserve; do
  ./run_tlc.sh CaIncarnationCore_sab_$c.cfg && echo "UNEXPECTED PASS: $c"
done
```

`stage4.cfg` and `stage5.cfg` exist but were NOT run — they exceed the time budget; see the
residual section below.

## Stage table {#stage-table}

| Stage | Config | Adds | Bounds | Expected |
|---|---|---|---|---|
| 1 core | `CaIncarnationCore_stage1.cfg` | publish/drop, fold/retire/fence/recheck, in-flight deletes | 2 writers, 2 hashes, `MaxToken=3`, `MaxRound=2`, `MaxLog=6`, 1 leader, 1 shard | PASS |
| 2 resurrect/evidence | `CaIncarnationCore_stage2.cfg` | `WResurrect`, `WEvidence`, `WResolveEvidence`; retired-old-vs-newer-current | 1 hash (bounds reduced — 2 hashes explode under `EnableResurrect`), `MaxLog=4` | PASS |
| 3 trees/cascade | `CaIncarnationCore_stage3.cfg` | tree expansion markers, atomic cascade; two asymmetric trees (`t1` full, `t2` single-child): shared-child survival, selective cascade | single writer (bounds reduced — tree×resurrect explode multi-writer), 4 hashes, `MaxToken=2`, `MaxLog=5` | PASS |
| 4a debris/full-GC cut | `CaIncarnationCore_stage4_small.cfg` | heartbeat-gated debris, wedged-writer publish, two-shard full-GC exact-cut walk | 1 writer, 1 hash (no trees), `MaxToken=2`, `MaxRound=1`, `MaxLog=2`, 2 shards | PASS |
| 4b journaled-delete + tree rebuild | `CaIncarnationCore_stage4_journaltree.cfg` | full GC + `MaxLog=3` (covers journaled retire→fence→recheck→delete tail) + tree-reachability rebuild in `FGCommit` | 1 writer, 1 shard, 2 hashes (`t1` tree), `MaxLog=3` | PASS |
| 5 split/overwrite | `CaIncarnationCore_stage5_small.cfg` | split-brain two-leader competition + `WOverwrite` (anonymous churn) | 1 writer, 2 leaders, 2 hashes, `MaxLog=4` (debris OFF to isolate split×overwrite) | PASS |
| 6a registry | `CaIncarnationCore_stage6_registry.cfg` | namespace registry + manifest creation (B91): `WRegister`, `GFenceRegistry` (fence-time universe), minting, registration floors; reval mode | 1 writer, 2 shards (= 2 namespaces), 1 hash, `MaxToken=3`, `MaxRound=2`, `MaxLog=4` | PASS |
| 6b evidence staleness | `CaIncarnationCore_stage6_evstale.cfg` | amended `W-EVIDENCE` (B91): recorded-round evidence, `WEvObserve`, dep-set tree-child validation (no global presence oracle); reval mode | 1 writer, 1 shard, 3 hashes (`t1` tree), `MaxToken=2`, `MaxRound=2`, `MaxLog=5` | PASS |
| 6c cross smoke | `CaIncarnationCore_stage6_cross_smoke.cfg` | registry × evidence-staleness interaction smoke (NOT a proof of the combined space — B104 residual): `PubFloor` + `EvOK` in one gate, minting under a tree publish riding evidence deps | 1 writer, 1 shard, 2 hashes (`t1` tree), `MaxToken=2`, `MaxRound=2`, `MaxLog=4` | PASS |
| liveness | `CaIncarnationCore_stage2_live.cfg` | `NoLeakForever` under `FairSpec` | stage-2 bounds, `MaxRound=2` | bound-artifact lasso (see RESULTS) |

### Sabotage configs (all MUST fail) {#sabotage-configs}

| Config | Rule removed | Expected violation |
|---|---|---|
| `CaIncarnationCore_sab_nofence.cfg` | fence does not touch manifests (horn 2) | `INV_NO_DANGLE` |
| `CaIncarnationCore_sab_norecheckfold.cfg` | recheck does not require fold-through-fence (horn 1) | `INV_NO_DANGLE` |
| `CaIncarnationCore_sab_noretireview.cfg` | publish gate removed (W-PUBLISH-GATE) | `INV_NO_DANGLE` |
| `CaIncarnationCore_sab_unconddelete.cfg` | exact-token delete removed (zombie kills resurrected) | `INV_NO_DANGLE` |
| `CaIncarnationCore_sab_reusedtag.cfg` | W-FRESH-TAG / token distinctness | `INV_NO_RETURN` |
| `CaIncarnationCore_sab_cascade.cfg` | cascade-as-pipeline-step ordering | `INV_NO_LOSS` |
| `CaIncarnationCore_sab_cutoverclaim.cfg` | full-GC claimed-authority = incorporated-state | `INV_NO_DANGLE` |
| `CaIncarnationCore_sab_noreobserve.cfg` | W-REVALIDATE re-observation conjunct (reval mode) | `INV_NO_DANGLE` |
| `CaIncarnationCore_sab_noregistry.cfg` | W-REGISTER skipped: publish into unregistered namespaces, no floor (B91) | `INV_NO_DANGLE` |
| `CaIncarnationCore_sab_foldtimeuniverse.cfg` | GC fences the FOLD-TIME universe — the C++ hole fixed 2026-06-12 (B91) | `INV_NO_DANGLE` |
| `CaIncarnationCore_sab_noevreobserve.cfg` | gate admits STALE evidence without re-observation (B91) | `INV_NO_LOSS` |

## What is deliberately NOT modeled {#not-modeled}

The model abstracts aggressively. These are out of scope and remain untested:

1. **Pack byte-range addressing.** Objects within a pack are addressed by offset range; the model
   treats each `h ∈ Hashes` as an atomic opaque object. Sub-object reference counting and partial
   deletion are not modeled.
2. **Manifest size bounds and journal data-plane internals.** Journal append-only semantics,
   compaction, and atomic publish of the snap are abstracted; the `O(delta)` fold's streaming-merge
   implementation is not checked.
3. **GCS/Azure token bindings.** Token distinctness is a **parameter** (enforced by construction
   in the model via a per-key monotone allocator), not derived from cloud-backend semantics. The
   cloud backends' CAS / conditional-write contracts are assumed, not modeled.
4. **The reader path.** Reads are not modeled. `INV_NO_DANGLE` and `INV_NO_LOSS` check that live
   refs' bytes are not deleted; the in-flight-read restart rule (§6) is out of scope.
5. **Provenance and diagnostic fields.** Object envelopes, event-id dedup, and audit trails are
   not modeled.
6. **Nested subtrees (one-level tree closure only).** `Children[t]` maps a tree to its direct
   children. The model does NOT model transitive subtrees; the one-level closure is a stated
   residual. The spec's recursive reachability requires a separate argument for deeper nesting.
7. **Per-stage reduced bounds and dropped feature combinations:**
   - Stage 2 drops from 2 hashes to 1 (resurrect×2-hash explodes).
   - Stage 3 is single-writer (tree×resurrect×multi-writer explodes); writer-vs-writer interleaving
     is covered by stages 1, 2, and 5.
   - Stage 4 was split into two configs: `stage4_small` (debris + two-shard cut, no trees,
     `MaxLog=2`) and `stage4_journaltree` (journaled delete tail + tree rebuild, single shard,
     `MaxLog=3`). The full `stage4.cfg` was NOT run (exceeds time budget at any attempted bound).
   - Stage 5 disables debris to isolate split-brain × overwrite; debris × split was not run.
     The full `stage5.cfg` was NOT run (exceeds time budget).
8. **Liveness is bound-limited.** `NoLeakForever` is checked on stage-2 bounds with `MaxRound=2`.
   The temporal-property counterexample is a round-cap lasso (GC exhausted its round budget before
   reclaiming the candidate), not a genuine leak; see `CaIncarnationCore_RESULTS.md`.
9. **B91 stage-6 reductions.** One model shard = one namespace (`root_shards > 1` per namespace is
   not modeled — the implementation fences namespaces × all shards uniformly). Registry × split-brain
   and registry × evidence-staleness combined configs are not run (state space); each amendment is
   checked against the full single-leader round machinery. The retire view's hit check reads the
   LIVE `retired` set, weaker than the implementation's frozen LIST snapshot (the model explores
   MORE writer-hostile states — conservative direction).

## Results {#results}

See `CaIncarnationCore_RESULTS.md` for per-stage state counts, wall times, and the seven
negative-control counterexample traces. See also the model-refinement findings that `TLC` surfaced
during development (each was a real encoding gap, fixed spec-faithfully).

## Apalache induction (proof core) {#apalache-induction}

A trimmed, fully type-annotated proof module `CaIncarnationProofCore.tla` (single leader,
`W-REVALIDATE` gate only, token-only dependencies — no trees, no debris, no split-brain, no
evidence) was driven through Apalache's one-step induction recipe to find a strengthened
**inductive invariant** `IndInv`.

**What induction buys (state honestly):** the step check quantifies over ALL states satisfying
`IndInv` — not just reachable ones — so a green induction is evidence for every execution at the
fixed constant sizes (`|Writers|=2`, `|Shards|=1`, `|Hashes|=2`, `MaxToken=3`, `MaxRound=2`,
`MaxLog=4`), regardless of trace depth. Constants remain finite; parametric generality requires
TLAPS, for which `IndInv` and the CTI journal are the prepared input. This is the middle rung
between TLC bounded-checking and full parametric proof.

### Install Apalache {#install-apalache}

```bash
mkdir -p <repo>/tmp/apalache
cd <repo>/tmp/apalache
curl -fL -o apalache.tgz https://github.com/apalache-mc/apalache/releases/latest/download/apalache.tgz
tar xzf apalache.tgz --strip-components=1
bin/apalache-mc version   # must print 0.58.0 or newer
```

Java 21 (`/usr/bin/java`) is required. All runs use `docs/superpowers/models/run_apalache.sh`
(wrapper that logs to `tmp/apa_<label>.log` and prints a summary tail).

### Running the induction checks {#running-induction}

All commands run from `docs/superpowers/models/`.

```bash
# Typecheck (Snowcat): must be clean before any SMT run
./run_apalache.sh typecheck typecheck CaIncarnationProofCore.tla

# Base case: Init satisfies IndInv
./run_apalache.sh base check --cinit=CInit --init=Init --inv=IndInv --length=0 CaIncarnationProofCore.tla

# Step check: IndInv is inductive (the main result)
./run_apalache.sh step check --cinit=CInit --init=IndInvInit --inv=IndInv --length=1 CaIncarnationProofCore.tla
```

Expected: typecheck exit 0; base `NoError`; step `NoError` (45–73s wall).
`IndInvInit == StateShape /\ IndInv` — the `StateShape` initializer assigns every variable via
function spaces and `Gen`, then `IndInv` constrains; needed because Apalache's assignment-finder
requires a top-level assignment and `IndInv` alone carries only element-wise bounds.

### Negative controls {#negative-controls-apalache}

```bash
# Implication: IndInv => NoDangle and IndInv => NoReturn (both must be NoError)
./run_apalache.sh impl1 check --cinit=CInit --init=IndInv --inv=NoDangle --length=0 CaIncarnationProofCore.tla
./run_apalache.sh impl2 check --cinit=CInit --init=IndInv --inv=NoReturn  --length=0 CaIncarnationProofCore.tla

# Drop InflightCurrentUnreferenced: MUST FAIL (the irredundant heart conjunct)
./run_apalache.sh ctlicu check --cinit=CInit --init=IndInv_NoICUInit --inv=IndInv_NoICU --length=1 CaIncarnationProofCore.tla

# Drop InflightHeld / InflightVsRefs: each PASSES (implied by remaining conjuncts — documented redundancy)
./run_apalache.sh ctlheld check --cinit=CInit --init=IndInv_NoHeldInit --inv=IndInv_NoHeld --length=1 CaIncarnationProofCore.tla
./run_apalache.sh ctlrefs check --cinit=CInit --init=IndInv_NoRefsInit --inv=IndInv_NoRefs --length=1 CaIncarnationProofCore.tla

# Gate control: WPublish without re-observation conjunct — MUST FAIL on NoDangle (F1 witness)
./run_apalache.sh ctlreval check --cinit=CInit --init=IndInv --next=NextNoReval --inv=IndInv --length=1 CaIncarnationProofCore.tla

# Satisfiability: an IndInv state exists (FalseInv control must produce a counterexample)
./run_apalache.sh sat check --cinit=CInit --init=IndInv --inv=FalseInv --length=0 CaIncarnationProofCore.tla
```

`IndInv_NoHeld`, `IndInv_NoRefs`, `IndInv_NoICU`, `WPublishNoReval`, `NextNoReval`, and `FalseInv`
are defined in the proof core as inert negative-control annotations (not referenced from the TLC
config). Full results and the CTI journal are in `CaIncarnationCore_RESULTS.md`.
