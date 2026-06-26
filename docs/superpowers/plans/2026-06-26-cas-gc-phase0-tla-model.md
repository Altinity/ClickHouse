---
description: "TLA+ model and format skeleton — the R0 safety gate for the CAS GC redesign."
sidebar_label: "GC redesign — Phase 0 (TLA+)"
sidebar_position: 2
slug: /superpowers/plans/2026-06-26-cas-gc-phase0-tla-model
title: "Phase 0 — TLA+ Model & Format Skeleton — Implementation Plan"
doc_type: reference
---

# Phase 0 — TLA+ Model & Format Skeleton — Implementation Plan {#phase-0-tla-model-format-skeleton-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax. This is the **R0 gate** for the whole redesign: no code task in any later phase may begin until this model's suite is green. Read `2026-06-26-cas-gc-redesign-overview.md` first.

**Goal:** Author `CaGcRootLocalPartManifestCore.tla` + its `.cfg` suite so that the root-local part-manifest GC protocol is proved safe (`INV_NO_DANGLE`/`INV_NO_LOSS`/`INV_NO_RETURN`) and every one of the 22 negative controls produces the expected counterexample — before any production code is written.

**Architecture:** A TLC model branched from `CaIncarnationCore.tla` (the proved delete-protocol core) plus the precommit rules of `CaBuildRootPrecommit.tla`. It drops the content-addressed-tree/cascade machinery (no `treeEdges`, `marker`, `pendCasc`, `Children`) and replaces it with unique `ManifestId`s, single ownership, owner transitions, and blob-only in-degree. The model is the executable form of the spec's §Safety Invariants and §Negative Controls.

**Tech Stack:** TLA+ / TLC (`tla2tools.jar` v2.19 at `tmp/tla2tools.jar`, OpenJDK 21). No C++ in this phase.

## Global Constraints {#global-constraints}

- **Branch:** commit on `cas-gc-part-manifest-impl` (off `codex-gc-proposal-2026-06-26`). Never `master`. **New commits only — never amend/rebase.** Trailers on every commit:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
  ```
- **R0:** `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN` must be *proved by this model*. This phase IS the gate.
- **Prose style:** literal names in backticks; write a function as `f` not `f()`; "ASan" not "ASAN"; "exception" not "crash".
- **TLA+ run mechanics** (from `docs/superpowers/models/`):
  ```bash
  cd docs/superpowers/models
  java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp ../../../tmp/tla2tools.jar tlc2.TLC \
       -metadir ../../../tmp/tlc-meta -workers auto -config <Cfg>.cfg CaGcRootLocalPartManifestCore.tla
  ```
  - **HOLD config PASS** = exit 0 + log line `Model checking completed. No error has been found.`
  - **`_sab_*` / `_buggy` config CORRECT** = nonzero exit + `Error: Invariant <NAME> is violated.` (or `Temporal properties were violated.`). A `_sab_*` config that PASSES is a **gate failure**.
  - Bound runs with `TLC_JAVA_OPTS=-Xmx48g` when a stage's state space is large.

## Resolved Open Questions consumed here {#resolved-open-questions-consumed-here}

- The model defines the *protocol* objects only (identities, owner transitions, blob in-degree, generation seal coverage). On-wire encodings (OQ1/3/5/7 — `PartManifestProto` fields, block-run details, thresholds) are NOT modeled; they are Phase-1a concerns. The model DOES define the **fields the `GenerationSeal` must cover** (the `SabotageCutOverclaim` defense), so its coverage is provable.
- `_manifests` placement (OQ2) and sweep-eligibility (OQ6) are modeled abstractly: a manifest object belongs to exactly one `(namespace, build-prefix)` and a build-prefix has a boolean `sweepEligible` derived from a durable watermark fact, never a frozen-seq heuristic.

## File Structure {#file-structure}

- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla` — the model.
- Create: `docs/superpowers/models/run_gc_partmanifest.sh` — wrapper (mirrors `run_tlc.sh` but hardcodes the new module).
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_stage{0,1,2,3,4}.cfg` — positive stages (must HOLD).
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_live.cfg` — liveness (`FairSpec`, must HOLD).
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_<rule>.cfg` × 22 — negative controls (must FAIL with the named invariant).
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_RESULTS.md` — the green-suite ledger.

## Modeled Vocabulary (canonical names — later phases reference these) {#modeled-vocabulary-canonical-names-later-phases-reference-these}

CONSTANTS: `Namespaces, Writers, Leaders, Blobs, ManifestInstances, Refs, Builds, Paths, MaxToken, MaxRound, MaxLog`, the `Enable*` flags (`EnablePrecommit, EnableMissingBody, EnableOrphanSweep, EnableMutablePayload`), and one `Sabotage*` per negative control (see [Negative Controls](#negative-controls-tasks)).

VARIABLES (roles): `present`/`tokOf`/`nextTok`/`deadTok` (blob objects + token history, as in `CaIncarnationCore`); `mBody` (`[ManifestId -> BOOLEAN]` manifest body present); `mEntries` (`[ManifestId -> [Paths -> Blobs ∪ {NoBlob}]]` the per-path blob refs a manifest body names); `mRef`/`mNs` (`[ManifestId -> ...]` the body's self-described ref/namespace, for `RefMatchesBody`/`ManifestNamespaceMatches`); `owner` (`[ManifestId -> {committed ref | precommit build | none}]`); `journal` (`[Namespaces -> Seq(OwnerTransition)]`); `mActiveEdges` (`[ManifestId -> [Paths -> Blobs ∪ {NoBlob}]]` the per-path edges actually emitted at activation — the `ManifestActivationMatchesEdges` oracle); `blobIndeg` (`[Blobs -> Nat]`) / `blobEdges` (`⊆ ManifestIds×Paths`, the folded active source edges); `seal` (per-generation `GenerationSeal` coverage); the GC pipeline vars carried from the core (`gcRound, gcPhase, roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView`); `mfCleanup` (part-manifest cleanup work keyed by `ManifestId`); `sweepEligible` (`[BuildPrefix -> BOOLEAN]`).

Actions composed into `Next`: `WStageManifest, WPrecommitAdd, WUploadBlob, WPromote, WPublishCommitted, WDropRef, WRepoint, WAbandonPrecommit, WMutableUpdate, WRepublishCrossNs, GStartRound, GFoldTransition, GRetireBlob, GRetireManifest, GFenceRegistry, GFenceShard, GRecheckDelete, GDeleteManifest, GOrphanSweep, Land, Trim`. `Spec == Init /\ [][Next]_vars`; `FairSpec` adds `WF_vars` on the GC pipeline + `Land` + `GOrphanSweep`.

Invariants/properties (the spec's §Safety Invariants): `TypeOK, INV_NO_DANGLE, INV_NO_LOSS, INV_NO_RETURN, INV_JOURNAL_COVERAGE, NoManifestIdReuse, SingleManifestOwner, CommittedManifestBodyRequired, PrecommitMayReferenceMissingManifest, RefMatchesBody, ManifestNamespaceMatches, MutablePayloadNotReachability, ManifestActivationMatchesEdges, CommittedNoMissingBlob, PrecommitMayReferenceMissingBlob, BlobInDegreeMatchesActiveManifests, NoCommittedDangle`; properties `MonotoneGC, MonotoneRegistry`; liveness `OrphanManifestDebrisDrains, NoLeakForever` (under `FairSpec`); non-vacuity witnesses `W_*`.

---

## Tasks {#tasks}

### Task 1: Module scaffold, wrapper, and trivial holding stage {#task-1-module-scaffold-wrapper-and-trivial-holding-stage}

**Files:**
- Create: `docs/superpowers/models/run_gc_partmanifest.sh`
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla`
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_stage0.cfg`

- [ ] **Step 1: Write the wrapper script** (mirrors `run_tlc.sh`, hardcodes the new module)

```bash
#!/usr/bin/env bash
# Usage: ./run_gc_partmanifest.sh <Cfg-basename-without-.cfg>
set -u
CFG="${1:?usage: run_gc_partmanifest.sh <cfg-basename>}"
LOG="../../../tmp/tlc_${CFG}.log"
shift || true
java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp ../../../tmp/tla2tools.jar tlc2.TLC \
     -metadir ../../../tmp/tlc-meta -workers auto -config "${CFG}.cfg" "$@" \
     CaGcRootLocalPartManifestCore.tla 2>&1 | tee "$LOG" | \
     grep -E "Model checking completed|Error:|violated|states generated|distinct states|Finished in"
RC=${PIPESTATUS[0]}
echo "exit=$RC log=$LOG"
exit "$RC"
```
Then `chmod +x run_gc_partmanifest.sh`.

- [ ] **Step 2: Write the module skeleton** with state types but a near-empty `Next`, so `stage0` holds trivially.

```tla
-------------------- MODULE CaGcRootLocalPartManifestCore --------------------
(* Root-local part-manifest GC core — spec: 2026-06-26-cas-gc-streaming-sharded-redesign-design.md.
   Branch of CaIncarnationCore.tla: blobs keep incarnation tokens + exact-token delete + fence/recheck;
   trees are REPLACED by unique single-owner ManifestIds with owner transitions and blob-only in-degree.
   Sabotage* flags each break exactly one load-bearing rule and MUST yield a counterexample. *)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Namespaces, Writers, Leaders, Blobs, ManifestInstances, Refs, Builds, Paths,
    MaxToken, MaxRound, MaxLog,
    EnablePrecommit, EnableMissingBody, EnableOrphanSweep, EnableMutablePayload,
    \* one per negative control (Task 8-10); all FALSE in positive stages:
    SabotageReuseManifestId, SabotageTwoOwners, SabotageSplitPromote,
    SabotageMissingBodyActivated, SabotageCommitSkipBlobReval, SabotagePrecommitlessProtect,
    SabotageNoOrphanSweep, SabotageWholesalePrefixDelete, SabotageFrozenSeqAuthority,
    SabotageMissingCommittedEmpty, SabotageDeleteBodyBeforeDecrements, SabotageCutOverclaim,
    SabotageRoundVisibilityEarly, SabotageNoFence, SabotageTrimUnincorporated,
    SabotageUncondDelete, SabotageReusedTag, SabotageBareNonce, SabotageKeyByRefNotId,
    SabotageAcceptNamespaceMismatch, SabotageAcceptRefMismatch,
    SabotageMutableAsReachability, SabotagePromoteAfterMissingBody

\* A ManifestId is (namespace, manifest_instance_id). manifest_instance_id is drawn from
\* ManifestInstances and is NEVER reused once visible (NoManifestIdReuse). Two namespaces may
\* hold the same instance id; they are DIFFERENT ManifestIds (the SabotageKeyByRefNotId hazard).
ManifestIds == Namespaces \X ManifestInstances
Toks == 1..MaxToken
None == "none"
NoBlob == "noblob"   \* per-path sentinel: that path has no blob entry (inline / absent)
\* Source edges are keyed by (ManifestId, path), NOT by a SUBSET Blobs. A manifest referencing the
\* same blob at two paths therefore contributes in-degree 2 — multiplicity that SUBSET Blobs cannot
\* express and that controls #18 (KeyByRefNotId), #21 (MutableAsReachability), and
\* MutablePayloadNotReachability require. mEntries[m] and mActiveEdges[m] are [Paths -> Blobs \cup {NoBlob}].
BlobsOf(g) == { g[p] : p \in {q \in Paths : g[q] \in Blobs} }   \* set of blobs a per-path map references
SrcEdges(m) == { <<m, p>> : p \in {q \in Paths : mActiveEdges[m][q] \in Blobs} }  \* active source edges of m

VARIABLES
    present, tokOf, nextTok, deadTok,         \* blob objects (as in CaIncarnationCore)
    mBody, mEntries, mRef, mNs,               \* manifest body: present?, PER-PATH blob refs [Paths->Blobs∪{NoBlob}], self-ref, self-ns
    owner, mActiveEdges,                       \* structural owner; per-path edges actually emitted at activation [Paths->Blobs∪{NoBlob}]
    journal,                                   \* [Namespaces -> Seq(OwnerTransition)]
    blobIndeg, blobEdges, everEdged,           \* folded blob in-degree; blobEdges ⊆ ManifestIds×Paths (folded active source edges); journal-known
    seal,                                      \* per-round GenerationSeal coverage record
    gcRound, gcPhase, roundOf, fencePos, cursor, trimBase, fenceVersion,
    retired, inflight, wView,                  \* GC pipeline (carried from core)
    mfCleanup, sweepEligible                   \* part-manifest cleanup work; orphan-sweep eligibility

vars == << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
           journal, blobIndeg, blobEdges, everEdged, seal, gcRound, gcPhase, roundOf, fencePos,
           cursor, trimBase, fenceVersion, retired, inflight, wView, mfCleanup, sweepEligible >>

\* ---- helpers (filled across tasks) ----
NoOp == UNCHANGED vars

Init ==
    /\ present = [b \in Blobs |-> FALSE]
    /\ tokOf = [b \in Blobs |-> 0]
    /\ nextTok = [b \in Blobs |-> 1]
    /\ deadTok = [b \in Blobs |-> {}]
    /\ mBody = [m \in ManifestIds |-> FALSE]
    /\ mEntries = [m \in ManifestIds |-> [p \in Paths |-> NoBlob]]
    /\ mRef = [m \in ManifestIds |-> m]      \* body self-ref equals id until sabotaged
    /\ mNs = [m \in ManifestIds |-> m[1]]    \* body self-ns equals owning ns until sabotaged
    /\ owner = [m \in ManifestIds |-> None]
    /\ mActiveEdges = [m \in ManifestIds |-> [p \in Paths |-> NoBlob]]
    /\ journal = [n \in Namespaces |-> << >>]
    /\ blobIndeg = [b \in Blobs |-> 0]
    /\ blobEdges = {}
    /\ everEdged = {}
    /\ seal = [r \in 0..MaxRound |-> [covered |-> {}, cut |-> [n \in Namespaces |-> 0]]]
    /\ gcRound = 0
    /\ gcPhase = [l \in Leaders |-> "idle"]
    /\ roundOf = [l \in Leaders |-> 0]
    /\ fencePos = [n \in Namespaces |-> 0]
    /\ cursor = [n \in Namespaces |-> 0]
    /\ trimBase = [n \in Namespaces |-> 0]
    /\ fenceVersion = [r \in 0..MaxRound |-> [n \in Namespaces |-> 0]]
    /\ retired = {}
    /\ inflight = {}
    /\ wView = [w \in Writers |-> 0]
    /\ mfCleanup = {}
    /\ sweepEligible = [n \in Namespaces |-> FALSE]

TypeOK ==
    /\ present \in [Blobs -> BOOLEAN]
    /\ tokOf \in [Blobs -> 0..MaxToken]
    /\ owner \in [ManifestIds -> Refs \cup Builds \cup {None}]
    /\ mBody \in [ManifestIds -> BOOLEAN]
    /\ mEntries \in [ManifestIds -> [Paths -> Blobs \cup {NoBlob}]]
    /\ blobEdges \in SUBSET (ManifestIds \X Paths)
    /\ blobIndeg \in [Blobs -> 0..(Cardinality(ManifestIds) * Cardinality(Blobs))]
    /\ cursor \in [Namespaces -> 0..MaxLog]
    /\ trimBase \in [Namespaces -> 0..MaxLog]

INV_JOURNAL_COVERAGE == \A n \in Namespaces : trimBase[n] <= cursor[n]

StateConstraint ==
    /\ \A n \in Namespaces : Len(journal[n]) <= MaxLog
    /\ Cardinality(inflight) <= 2

Next == NoOp           \* replaced incrementally in Tasks 2-7

Spec == Init /\ [][Next]_vars
=============================================================================
```

- [ ] **Step 3: Write `stage0.cfg`** (must HOLD — only `TypeOK` + coverage)

```
SPECIFICATION Spec
CONSTANTS
    Namespaces = {n1}
    Writers = {w1}
    Leaders = {L1}
    Blobs = {b1}
    ManifestInstances = {m1}
    Refs = {r1}
    Builds = {bd1}
    MaxToken = 2
    MaxRound = 1
    MaxLog = 3
    EnablePrecommit = FALSE
    EnableMissingBody = FALSE
    EnableOrphanSweep = FALSE
    EnableMutablePayload = FALSE
    SabotageReuseManifestId = FALSE
    SabotageTwoOwners = FALSE
    SabotageSplitPromote = FALSE
    SabotageMissingBodyActivated = FALSE
    SabotageCommitSkipBlobReval = FALSE
    SabotagePrecommitlessProtect = FALSE
    SabotageNoOrphanSweep = FALSE
    SabotageWholesalePrefixDelete = FALSE
    SabotageFrozenSeqAuthority = FALSE
    SabotageMissingCommittedEmpty = FALSE
    SabotageDeleteBodyBeforeDecrements = FALSE
    SabotageCutOverclaim = FALSE
    SabotageRoundVisibilityEarly = FALSE
    SabotageNoFence = FALSE
    SabotageTrimUnincorporated = FALSE
    SabotageUncondDelete = FALSE
    SabotageReusedTag = FALSE
    SabotageBareNonce = FALSE
    SabotageKeyByRefNotId = FALSE
    SabotageAcceptNamespaceMismatch = FALSE
    SabotageAcceptRefMismatch = FALSE
    SabotageMutableAsReachability = FALSE
    SabotagePromoteAfterMissingBody = FALSE
CONSTRAINT StateConstraint
INVARIANT TypeOK
INVARIANT INV_JOURNAL_COVERAGE
```

- [ ] **Step 4: Run stage0 — must HOLD**

Run: `cd docs/superpowers/models && ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_stage0`
Expected: `Model checking completed. No error has been found.` and `exit=0`.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/models/CaGcRootLocalPartManifestCore.tla \
        docs/superpowers/models/run_gc_partmanifest.sh \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_stage0.cfg
git commit -m "CA GC phase0: TLA+ module scaffold + holding stage0"
```

---

### Task 2: Manifest identity, body validation, and no-reuse {#task-2-manifest-identity-body-validation-and-no-reuse}

**Files:**
- Modify: `CaGcRootLocalPartManifestCore.tla`
- Create: `CaGcRootLocalPartManifestCore_stage1.cfg`

**Interfaces produced:** `WStageManifest` action; `RefMatchesBody`, `ManifestNamespaceMatches`, `NoManifestIdReuse`, `INV_NO_RETURN` (blob-token form).

- [ ] **Step 1: Add the staging action and validation helpers.** Add to the helpers section and extend `Next`.

```tla
\* A blob token stops being current when displaced or deleted (INV_NO_RETURN oracle).
CondemnedTok(b, t) == t \in deadTok[b]

\* RefMatchesBody / ManifestNamespaceMatches: the body self-describes its ref + ns; a sabotage may
\* publish a manifest whose body disagrees. A read/fold that accepts a mismatch is unsafe.
BodyValid(m) == mRef[m] = m /\ mNs[m] = m[1]

\* visibleIds: ids that have ever been bound (body staged or owner set). NoManifestIdReuse forbids
\* re-binding a visible id to a new body lineage.
WStageManifest(m, f) ==
    /\ owner[m] = None /\ ~mBody[m]
    /\ m[2] \notin everEdged                    \* fresh instance id (never-reused); SabotageReuseManifestId drops this
       \/ SabotageReuseManifestId
    /\ mBody' = [mBody EXCEPT ![m] = TRUE]
    /\ mEntries' = [mEntries EXCEPT ![m] = f]
    /\ mRef' = [mRef EXCEPT ![m] = IF SabotageAcceptRefMismatch THEN CHOOSE x \in ManifestIds : x # m ELSE m]
    /\ mNs' = [mNs EXCEPT ![m] = IF SabotageAcceptNamespaceMismatch THEN CHOOSE n \in Namespaces : n # m[1] ELSE m[1]]
    /\ everEdged' = everEdged \cup {m[2]}
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, owner, mActiveEdges, journal, blobIndeg,
                    blobEdges, seal, gcRound, gcPhase, roundOf, fencePos, cursor, trimBase,
                    fenceVersion, retired, inflight, wView, mfCleanup, sweepEligible >>
```
Replace `Next == NoOp` with:
```tla
Next ==
    \/ \E m \in ManifestIds, f \in [Paths -> Blobs \cup {NoBlob}] : WStageManifest(m, f)
```

- [ ] **Step 2: Add the invariants.**

```tla
NoManifestIdReuse ==
    \* once a body is staged for an instance id in a namespace, no DIFFERENT body lineage rebinds it
    \A m \in ManifestIds : mBody[m] => (mRef[m] = m \/ SabotageAcceptRefMismatch)
RefMatchesBody == \A m \in ManifestIds : (mBody[m] /\ owner[m] # None) => mRef[m] = m
ManifestNamespaceMatches == \A m \in ManifestIds : (mBody[m] /\ owner[m] # None) => mNs[m] = m[1]
INV_NO_RETURN == \A b \in Blobs : present[b] => tokOf[b] \notin deadTok[b]
```

- [ ] **Step 2b: Run stage0 again — still HOLDs** (regression).

Run: `./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_stage0`
Expected: `No error`.

- [ ] **Step 3: Create `stage1.cfg`** — copy `stage0.cfg`, then add the new invariants and widen the manifest space:
```
    ManifestInstances = {m1, m2}
    ...
INVARIANT NoManifestIdReuse
INVARIANT RefMatchesBody
INVARIANT ManifestNamespaceMatches
INVARIANT INV_NO_RETURN
```
(keep all `Sabotage* = FALSE`, all `Enable* = FALSE`.)

- [ ] **Step 4: Run stage1 — must HOLD.**

Run: `./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_stage1`
Expected: `No error`, `exit=0`.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/models/CaGcRootLocalPartManifestCore.tla docs/superpowers/models/CaGcRootLocalPartManifestCore_stage1.cfg
git commit -m "CA GC phase0: manifest identity + body validation + no-reuse (stage1)"
```

---

### Task 3: Owner transitions, committed vs precommit, promotion {#task-3-owner-transitions-committed-vs-precommit-promotion}

**Files:**
- Modify: `CaGcRootLocalPartManifestCore.tla`
- Create: `CaGcRootLocalPartManifestCore_stage2.cfg`

**Interfaces produced:** `WPrecommitAdd, WUploadBlob, WPromote, WPublishCommitted, WDropRef, WRepoint, WAbandonPrecommit`; `SingleManifestOwner, CommittedManifestBodyRequired, PrecommitMayReferenceMissingManifest, CommittedNoMissingBlob, PrecommitMayReferenceMissingBlob, INV_NO_DANGLE, INV_NO_LOSS, NoCommittedDangle`.

- [ ] **Step 1: Add owner-transition actions.** Model the spec's §Build And Precommit Protocol. Each action appends an `OwnerTransition` record `[ver, ref, old, new]` to `journal[ns]`. Key rules:
  - `WPrecommitAdd(m, bld)`: requires `EnablePrecommit`; sets `owner[m] = bld`; precommit **may** have `~mBody[m]` only when `EnableMissingBody` (the missing-body intent); records `mActiveEdges[m] = IF mBody[m] /\ BodyValid(m) THEN mEntries[m] ELSE [p \in Paths |-> NoBlob]` unless `SabotageMissingBodyActivated` forces `mEntries[m]` even when body absent.
  - `WUploadBlob(b)`: mint a fresh token `nextTok[b]` (never the condemned one unless `SabotageReusedTag`); `present[b]=TRUE`.
  - `WPromote(m, bld, ref)`: the **atomic** owner move. Single transition `owner[m]: bld -> ref`. Fail-closed gate: requires `mBody[m] /\ BodyValid(m) /\ (BlobsOf(mEntries[m]) \subseteq {b \in Blobs : present[b]})` unless `SabotageCommitSkipBlobReval`. If activation was missing-body, emit committed `+` edges here (set `mActiveEdges[m]=mEntries[m]`) unless `SabotagePromoteAfterMissingBody` keeps it a pure move. `SabotageSplitPromote` splits into remove-then-add with an interleaving gap.
  - `WPublishCommitted(m, ref)`: direct committed publish (no precommit). Same fail-closed body+blob gate as promote.
  - `WDropRef(m)` / `WRepoint(mOld, mNew, ref)` / `WAbandonPrecommit(m)`: owner removals/replacements; each emits the paired old/new transition and queues `mfCleanup` for the removed id.

  (Write each action in full following the `CaIncarnationCore` `WPublish`/`WDrop` style; gate them on the relevant `Enable*`. Add each to `Next`.)

- [ ] **Step 2: Add the ownership/dangle invariants.**
```tla
SingleManifestOwner ==
    SabotageTwoOwners \/ (\A m1, m2 \in ManifestIds :
        (owner[m1] # None /\ owner[m1] = owner[m2] /\ m1 # m2) => owner[m1] \in Builds)
CommittedManifestBodyRequired ==
    \A m \in ManifestIds : (owner[m] \in Refs) => (mBody[m] /\ BodyValid(m))
PrecommitMayReferenceMissingManifest == TRUE   \* witnessed reachable, not an invariant to hold
CommittedNoMissingBlob ==
    \A m \in ManifestIds : (owner[m] \in Refs) => (\A b \in BlobsOf(mActiveEdges[m]) : present[b] /\ ~CondemnedTok(b, tokOf[b]))
NoCommittedDangle ==
    \A m \in ManifestIds : (owner[m] \in Refs) => (mBody[m] /\ \A b \in BlobsOf(mEntries[m]) : present[b])
INV_NO_DANGLE == NoCommittedDangle
ReachableBlobs == UNION { BlobsOf(mEntries[m]) : m \in {x \in ManifestIds : owner[x] \in Refs /\ mBody[x]} }
INV_NO_LOSS == \A b \in ReachableBlobs : present[b]
```

- [ ] **Step 3: Create `stage2.cfg`** — copy `stage1.cfg`, set `EnablePrecommit = TRUE`, `EnableMissingBody = TRUE`, widen `Blobs = {b1, b2}`, add the new invariants `SingleManifestOwner`, `CommittedManifestBodyRequired`, `CommittedNoMissingBlob`, `NoCommittedDangle`, `INV_NO_DANGLE`, `INV_NO_LOSS`. Keep all `Sabotage* = FALSE`.

- [ ] **Step 4: Run stage2 — must HOLD.**

Run: `./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_stage2`
Expected: `No error`. If it finds a trace, the model (not the config) is wrong — fix the action gates until it holds, because all sabotage flags are off.

- [ ] **Step 5: Commit**
```bash
git add docs/superpowers/models/CaGcRootLocalPartManifestCore.tla docs/superpowers/models/CaGcRootLocalPartManifestCore_stage2.cfg
git commit -m "CA GC phase0: owner transitions + committed/precommit + promotion (stage2)"
```

---

### Task 4: GC pipeline — fold, retire, fence, recheck, exact-token delete, trim {#task-4-gc-pipeline-fold-retire-fence-recheck-exact-token-delete-trim}

**Files:**
- Modify: `CaGcRootLocalPartManifestCore.tla`
- Create: `CaGcRootLocalPartManifestCore_stage3.cfg`

**Interfaces produced:** `GStartRound, GFoldTransition, GRetireBlob, GFenceRegistry, GFenceShard, GRecheckDelete, Land, Trim`; `ViewableRound`, `BlobInDegreeMatchesActiveManifests`, `FoldedThroughFence`, `seal` coverage, `MonotoneGC`.

- [ ] **Step 1: Add the GC pipeline.** Port the proved tail of `CaIncarnationCore` (`GStartRound/GFenceRegistry/GFenceShard/GRecheckDelete/Land/Trim`) but replace tree expansion with blob-delta folding:
  - `GFoldTransition(n)`: consume the next unfolded `journal[n]` record at `cursor[n]`. For an `old` id, require `mBody[old] /\ BodyValid(old)` (read the body while present), remove `SrcEdges(old)` from `blobEdges` (the `-1` deltas) and decrement `blobIndeg` accordingly, queue `mfCleanup`. For a `new` committed id, require `mBody[new] /\ BodyValid(new)` (`SabotageMissingCommittedEmpty` drops this and treats it as `{}`), set `mActiveEdges[new]=mEntries[new]`, add `SrcEdges(new)` to `blobEdges` (the `+1` deltas), and increment `blobIndeg`. For a `new` precommit id with absent body, emit nothing (record missing-body). Advance `cursor[n]`; update `seal[gcRound].cut[n]`. `SabotageCutOverclaim` jumps `cursor[n]` to `Len(journal[n])` while edges came from the cut.
  - `GRetireBlob`: a blob with `blobIndeg[b] = 0 /\ present[b]` is retired with its exact token (per-candidate; `SabotageReusedTag`/`SabotageUncondDelete` belong to `Land`).
  - `GFenceRegistry` then `GFenceShard(n)`: as in the core; `SabotageNoFence` skips the manifest fence write.
  - `GRecheckDelete`: requires `FoldedThroughFence` (`SabotageNoRecheckFold` drops it); deletes a blob only if `blobIndeg[b]=0` still and token matches; spares otherwise.
  - `Land`: exact-token blob delete message lands; `SabotageUncondDelete` ignores the token match; on any token stopping being current, add it to `deadTok[b]`.
  - `Trim(n)`: only below `cursor[n]`; `SabotageTrimUnincorporated` trims below an unfolded transition.
```tla
FoldedThroughFence == \A n \in Namespaces : cursor[n] >= fencePos[n]
ViewableRound == ...   \* port from CaIncarnationCore: round R visible to writers only after all
                       \* round-R retired sets AND mfCleanup bundles are durable; SabotageRoundVisibilityEarly breaks it
BlobInDegreeMatchesActiveManifests ==
    \A b \in Blobs :
        blobIndeg[b] = Cardinality({ e \in blobEdges : mActiveEdges[e[1]][e[2]] = b })
\* Every folded source edge belongs to a still-active manifest (the link fence+recheck protect):
FoldedEdgesAreActive == \A e \in blobEdges : owner[e[1]] # None
MonotoneGC == [][ gcRound' >= gcRound ]_vars
```

- [ ] **Step 2: Create `stage3.cfg`** — copy `stage2.cfg`, widen `MaxRound = 2`, `Leaders = {L1}`, add invariants `BlobInDegreeMatchesActiveManifests` and `FoldedEdgesAreActive` (list `INVARIANT FoldedEdgesAreActive` in the cfg — it pins that every folded source edge still belongs to an active manifest, the structural complement to the in-degree accounting), `INV_NO_RETURN`, and `PROPERTY MonotoneGC`. Keep all `Sabotage* = FALSE`.

- [ ] **Step 3: Run stage3 — must HOLD.**

Run: `TLC_JAVA_OPTS=-Xmx16g ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_stage3`
Expected: `No error`.

- [ ] **Step 4: Commit**
```bash
git add docs/superpowers/models/CaGcRootLocalPartManifestCore.tla docs/superpowers/models/CaGcRootLocalPartManifestCore_stage3.cfg
git commit -m "CA GC phase0: GC pipeline fold/retire/fence/recheck/delete/trim (stage3)"
```

---

### Task 5: Part-manifest cleanup ordering + orphan pre-precommit sweep {#task-5-part-manifest-cleanup-ordering-orphan-pre-precommit-sweep}

**Files:**
- Modify: `CaGcRootLocalPartManifestCore.tla`
- Create: `CaGcRootLocalPartManifestCore_stage4.cfg`
- Create: `CaGcRootLocalPartManifestCore_live.cfg`

**Interfaces produced:** `GRetireManifest, GDeleteManifest, GOrphanSweep`; `OrphanManifestDebrisDrains`, `NoLeakForever`, `MutablePayloadNotReachability`, `ManifestActivationMatchesEdges`, `WMutableUpdate`.

- [ ] **Step 1: Add manifest cleanup + sweep + mutable update.**
  - `GRetireManifest` / `GDeleteManifest(m)`: a manifest body is deleted (exact-token) only after its owner-removal blob decrements are sealed into the generation. `SabotageDeleteBodyBeforeDecrements` deletes the body before the `-` deltas are durable.
  - `GOrphanSweep(n)`: requires `EnableOrphanSweep`; deletes a staged-but-unowned manifest body in namespace `n` only if its build-prefix `sweepEligible` AND its id is absent from the sealed owner view; emits no blob deltas. `SabotageNoOrphanSweep` disables it (leak). `SabotageWholesalePrefixDelete` deletes the whole eligible prefix regardless of owner view. `SabotageFrozenSeqAuthority` sets `sweepEligible` from a frozen-seq heuristic instead of the durable watermark fact.
  - `WMutableUpdate(m)`: requires `EnableMutablePayload`; changes only mutable per-ref payload — no owner transition, no blob delta, no id change. `SabotageMutableAsReachability` mints a new id / emits blob deltas.
```tla
MutablePayloadNotReachability == TRUE   \* enforced by WMutableUpdate touching none of owner/blobEdges/ManifestIds;
                                        \* SabotageMutableAsReachability violates INV_NO_LOSS by spurious decrement
ManifestActivationMatchesEdges ==
    \A m \in ManifestIds : (owner[m] # None) => (mActiveEdges[m] \subseteq mEntries[m] \/ ~mBody[m])
OrphanManifestDebrisDrains ==        \* liveness: under FairSpec a staged-unowned eligible body is eventually deleted
    \A m \in ManifestIds :
        (mBody[m] /\ owner[m] = None /\ sweepEligible[m[1]]) ~> (~mBody[m])
NoLeakForever == <>[](\A b \in Blobs : (blobIndeg[b] = 0 /\ present[b]) => FALSE)  \* refine: eventually no live zero-indeg present blob
FairSpec == Spec /\ WF_vars(\E n \in Namespaces : GFoldTransition(n))
                 /\ WF_vars(\E l \in Leaders : GRecheckDelete(l))
                 /\ WF_vars(\E d \in inflight : Land(d))
                 /\ WF_vars(\E n \in Namespaces : GOrphanSweep(n))
```

- [ ] **Step 2: Create `stage4.cfg`** (safety, all enables on, all sab off): copy `stage3.cfg`; set `EnableOrphanSweep = TRUE`, `EnableMutablePayload = TRUE`; add invariants `ManifestActivationMatchesEdges`. Run — must HOLD.

Run: `TLC_JAVA_OPTS=-Xmx24g ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_stage4`
Expected: `No error`.

- [ ] **Step 3: Create `live.cfg`** (liveness): copy `stage4.cfg`; replace `SPECIFICATION Spec` with `SPECIFICATION FairSpec`; remove safety `INVARIANT` lines; add `PROPERTY OrphanManifestDebrisDrains` and `PROPERTY NoLeakForever`; add `CHECK_DEADLOCK FALSE`. Run — must HOLD.

Run: `TLC_JAVA_OPTS=-Xmx24g ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_live`
Expected: `No error`.

- [ ] **Step 4: Commit**
```bash
git add docs/superpowers/models/CaGcRootLocalPartManifestCore.tla \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_stage4.cfg \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_live.cfg
git commit -m "CA GC phase0: manifest cleanup ordering + orphan sweep + liveness (stage4 + live)"
```

---

### Negative Controls (Tasks 6–8) {#negative-controls-tasks-6-8}

Each control is a `Sabotage* = TRUE` config that **must FAIL** with the named invariant. The model already contains the `IF Sabotage... THEN ...` branches added in Tasks 2–5. Each cfg = a copy of the smallest positive stage that exercises the rule, with exactly one `Sabotage* = TRUE` and the single targeted `INVARIANT` line (narrowing speeds the counterexample, per the `CaIncarnationCore` convention). The 22 controls map one-to-one to spec §Negative Controls.

| # | Sabotage flag | Spec control | Base stage | Targeted INVARIANT (must be violated) |
|---|---|---|---|---|
| 1 | `SabotageReuseManifestId` | reuse a `ManifestId` for a byte-identical future manifest | stage3 | `INV_NO_LOSS` |
| 2 | `SabotageTwoOwners` | two owners for one `ManifestId` (sharing) | stage2 | `INV_NO_LOSS` |
| 3 | `SabotageSplitPromote` | `PromotePrecommit` as two CAS with a gap | stage2 | `INV_NO_DANGLE` |
| 4 | `SabotageMissingBodyActivated` | treat missing precommit body as activated | stage3 | `INV_NO_LOSS` |
| 5 | `SabotageCommitSkipBlobReval` | committed publish skips blob revalidation | stage2 | `INV_NO_DANGLE` |
| 6 | `SabotagePrecommitlessProtect` | precommitless upload treated as protected | stage3 | `INV_NO_DANGLE` |
| 7 | `SabotageNoOrphanSweep` | omit pre-precommit debris sweep | live | `OrphanManifestDebrisDrains` |
| 8 | `SabotageWholesalePrefixDelete` | wholesale dead-prefix delete | stage4 | `INV_NO_DANGLE` |
| 9 | `SabotageFrozenSeqAuthority` | frozen-seq/judged-dead as deletion authority | stage4 | `INV_NO_DANGLE` |
| 10 | `SabotageMissingCommittedEmpty` | missing committed body treated as empty | stage3 | `INV_NO_LOSS` |
| 11 | `SabotageDeleteBodyBeforeDecrements` | delete body before decrements durable | stage3 | `NoLeakForever` (live) |
| 12 | `SabotageCutOverclaim` | cursor past unsealed deltas | stage3 | `INV_NO_DANGLE` |
| 13 | `SabotageRoundVisibilityEarly` | round visible after one shard's retired set | stage3 | `INV_NO_DANGLE` |
| 14 | `SabotageNoFence` | skip global fence for a racing publish | stage3 | `INV_NO_DANGLE` |
| 15 | `SabotageTrimUnincorporated` | trim below unincorporated transition | stage3 | `INV_JOURNAL_COVERAGE` |
| 16a | `SabotageUncondDelete` | non-exact delete | stage3 | `INV_NO_DANGLE` |
| 16b | `SabotageReusedTag` | reuse blob tokens | stage3 | `INV_NO_RETURN` |
| 17 | `SabotageBareNonce` | carry bare instance-id (no ref) | stage2 | `INV_NO_LOSS` |
| 18 | `SabotageKeyByRefNotId` | key edges/cleanup by ref not id | stage2 (2 namespaces) | `INV_NO_LOSS` |
| 19 | `SabotageAcceptNamespaceMismatch` | accept body ns ≠ owning ns | stage2 | `INV_NO_DANGLE` |
| 20 | `SabotageAcceptRefMismatch` | accept body ref ≠ journal ref | stage2 | `INV_NO_LOSS` |
| 21 | `SabotageMutableAsReachability` | mutable update mints id / emits deltas | stage4 | `INV_NO_LOSS` |
| 22 | `SabotagePromoteAfterMissingBody` | promote-as-move after missing-body | stage3 | `INV_NO_LOSS` |

### Task 6: Negative controls 1–8 {#task-6-negative-controls-1-8}

**Files:** Create `CaGcRootLocalPartManifestCore_sab_{reusemanifestid,twoowners,splitpromote,missingbodyactivated,commitskipblobreval,precommitlessprotect,noorphansweep,wholesaleprefixdelete}.cfg`

- [ ] **Step 1: Write the 8 cfgs** per the table (base stage's constants, one `Sabotage*=TRUE`, single targeted `INVARIANT`; the `noorphansweep` cfg uses the `live` base with `SPECIFICATION FairSpec`, `PROPERTY OrphanManifestDebrisDrains`, `CHECK_DEADLOCK FALSE`).
- [ ] **Step 2: Run each — every one MUST FAIL** with its targeted invariant.
```bash
for c in reusemanifestid twoowners splitpromote missingbodyactivated commitskipblobreval precommitlessprotect noorphansweep wholesaleprefixdelete ; do
  ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_sab_$c && echo "UNEXPECTED PASS: $c"
done
```
Expected: each prints `Error: Invariant <NAME> is violated.` (or the temporal-property violation for `noorphansweep`) and a nonzero `exit=`; **no** `UNEXPECTED PASS` line appears.
- [ ] **Step 3: Commit** `git commit -m "CA GC phase0: negative controls 1-8 (sabotage configs reproduce violations)"`.

### Task 7: Negative controls 9–15 {#task-7-negative-controls-9-15}

**Files:** Create `CaGcRootLocalPartManifestCore_sab_{frozenseqauthority,missingcommittedempty,deletebodybeforedecrements,cutoverclaim,roundvisibilityearly,nofence,trimunincorporated}.cfg`

- [ ] **Step 1: Write the 7 cfgs** per the table.
- [ ] **Step 2: Run each — every one MUST FAIL** (loop as in Task 6, names `frozenseqauthority missingcommittedempty deletebodybeforedecrements cutoverclaim roundvisibilityearly nofence trimunincorporated`). `deletebodybeforedecrements` uses the `live` base + `PROPERTY NoLeakForever`.
- [ ] **Step 3: Commit** `git commit -m "CA GC phase0: negative controls 9-15"`.

### Task 8: Negative controls 16–22 {#task-8-negative-controls-16-22}

**Files:** Create `CaGcRootLocalPartManifestCore_sab_{unconddelete,reusedtag,barenonce,keybyrefnotid,acceptnamespacemismatch,acceptrefmismatch,mutableasreachability,promoteaftermissingbody}.cfg`

- [ ] **Step 1: Write the 8 cfgs** per the table (`keybyrefnotid` uses `Namespaces = {n1, n2}` and `ManifestInstances = {m1}` so both namespaces share an instance id).
- [ ] **Step 2: Run each — every one MUST FAIL** (loop, names `unconddelete reusedtag barenonce keybyrefnotid acceptnamespacemismatch acceptrefmismatch mutableasreachability promoteaftermissingbody`).
- [ ] **Step 3: Commit** `git commit -m "CA GC phase0: negative controls 16-22 (all 22 controls reproduce)"`.

---

### Task 9: Non-vacuity witnesses {#task-9-non-vacuity-witnesses}

**Files:**
- Modify: `CaGcRootLocalPartManifestCore.tla` (add negated reachability probes)
- Create: `CaGcRootLocalPartManifestCore_witness_{precommitmissingbody,promoteaftermissingbodyaddsedges,orphandeleted}.cfg`

**Why:** prove the dangerous-but-safe interleavings are actually reachable (so the positive stages are not vacuously true).

- [ ] **Step 1: Add witnesses** (each is `INVARIANT W_x` where `W_x == ~(reachable interesting state)`, so TLC must report it *violated*):
```tla
W_PrecommitMissingBodyReached == ~(\E m \in ManifestIds : owner[m] \in Builds /\ ~mBody[m])
W_PromoteAfterMissingBodyAddsEdges == ~(\E m \in ManifestIds : owner[m] \in Refs /\ Cardinality(mActiveEdges[m]) > 0
                                          /\ mActiveEdges[m] = mEntries[m])
W_OrphanDeleted == ~(\E m \in ManifestIds : ~mBody[m] /\ owner[m] = None /\ m[2] \in everEdged)
```
- [ ] **Step 2: Write the 3 witness cfgs** (fixed positive constants; single `INVARIANT W_*` line).
- [ ] **Step 3: Run each — every one MUST FAIL** (the witness is violated ⇒ the state is reachable). An `UNEXPECTED PASS` means the model can never reach that state — a modeling bug to fix.
- [ ] **Step 4: Commit** `git commit -m "CA GC phase0: non-vacuity witnesses"`.

---

### Task 10: Green-suite ledger + gate confirmation {#task-10-green-suite-ledger-gate-confirmation}

**Files:**
- Create: `CaGcRootLocalPartManifestCore_RESULTS.md`

- [ ] **Step 1: Run the entire suite** and capture state counts + wall time per config:
```bash
cd docs/superpowers/models
for s in stage0 stage1 stage2 stage3 stage4 live ; do ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_$s ; done
for c in reusemanifestid twoowners splitpromote missingbodyactivated commitskipblobreval precommitlessprotect \
         noorphansweep wholesaleprefixdelete frozenseqauthority missingcommittedempty deletebodybeforedecrements \
         cutoverclaim roundvisibilityearly nofence trimunincorporated unconddelete reusedtag barenonce keybyrefnotid \
         acceptnamespacemismatch acceptrefmismatch mutableasreachability promoteaftermissingbody ; do
  ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_sab_$c && echo "UNEXPECTED PASS: $c"
done
```
- [ ] **Step 2: Write `RESULTS.md`** as a ledger table: one row per config with `result` (HOLD / VIOLATED `<Inv>`), `states generated`, `distinct states`, `wall time`. Mark the suite **GREEN** iff every stage/live/witness row is correct and all 22 `_sab_*` rows show their expected violation with **no** `UNEXPECTED PASS`. Use the analyze-via-subagent rule for any long log.
- [ ] **Step 3: Verify the gate** — confirm: (a) `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN` HOLD in stage3/stage4; (b) all 22 controls reproduce; (c) liveness HOLDs; (d) witnesses are reachable.
- [ ] **Step 4: Commit** `git commit -m "CA GC phase0: green-suite ledger — R0 gate satisfied"`.

---

## Self-Review {#self-review}

- **Spec coverage:** every entry in spec §Safety Invariants is a named invariant (Tasks 2–5); every entry in spec §Negative Controls is one row of the controls table (Tasks 6–8); §Backpressure/format encodings are explicitly out of scope here (Phase 1a). ✓
- **Gate definition matches the overview:** stages/`live`/witnesses HOLD; 22 `_sab_*` VIOLATE; no `UNEXPECTED PASS`. ✓
- **No placeholders:** the wrapper, the module skeleton, `stage0`/`stage1` cfgs, and one representative sabotage branch per action are shown in full; the remaining cfgs are fully specified by the controls table (base stage + the single flag + the single invariant), which is data, not a vague instruction. The implementer reads `CaIncarnationCore.tla` for the action-writing idiom (referenced in Task 3/4). ✓

**Gate for the rest of the redesign:** Phases 1a–1d may not start until this suite is GREEN.
