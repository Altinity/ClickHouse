# CAS ref contiguous-chain — TLA+ phase (gate) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the model changes of spec `docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md` §9 — models green, every sabotage red — BEFORE any C++ code. This plan is the gate for the main implementation plan.

**Architecture:** Six tasks over `docs/superpowers/models/`: rewrite the writer/recovery core (`CaRefTableSnapshotLogCore`) and the GC-intake core (`CaRefDeltaIntakeCore`, gaining pool-wide blob state), add `CaRefCatalogCore` (new), rewrite `CaRefNsCleanupStaleLeaderCore` around catalog incarnations, extend `CaCasMountCore` with recovery-generation gating, and audit the remaining listing-adjacent models. Every model keeps the house conventions: green configs + `_sab_*` configs that MUST violate + a `run_*.sh` expected-verdict harness + a `*_RESULTS.md`.

**Tech Stack:** TLA+ / TLC (`tmp/tla2tools.jar`, invoked exactly as existing `run_*.sh` scripts do).

## Global Constraints

- Branch `cas-gc-rebuild`; commit after every task; **NEVER `git push`**.
- Commit messages: `ca: tla — <what>` + the standard trailer lines used by this session's previous commits.
- TLC invocation pattern (copy from `docs/superpowers/models/run_foldclamp.sh`): `java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp ../../../tmp/tla2tools.jar tlc2.TLC -metadir ../../../tmp/tlc-meta-<cfg> -workers auto -config <cfg>.cfg <Module>.tla`, log to `../../../tmp/tlc_<cfg>.log`, verdict = `grep "No error has been found"` (green) vs `grep -E "is violated|Error:"` (violation).
- Every NEW invariant must be seen RED at least once (its `_sab_` config) before the green run is trusted — record both outcomes in the RESULTS file.
- Model sizes: keep `MaxSeq <= 4` and other bounds small; if a TLC run exceeds ~10 min with `-workers auto`, shrink bounds, do not remove properties.
- The spec is the requirements source; where this plan and the spec §2/§5 disagree, the spec wins and the discrepancy is reported.

---

### Task 1: Rewrite `CaRefTableSnapshotLogCore` — INV-1/INV-2/INV-4 writer & recovery core

**Files:**
- Modify: `docs/superpowers/models/CaRefTableSnapshotLogCore.tla` (full rewrite of the writer/reader halves; keep module name)
- Create: `docs/superpowers/models/CaRefTableSnapshotLogCore_v9_safe.cfg`, `..._v9_flip_latepred.cfg`, `..._sab_reuseafterambiguous.cfg`, `..._sab_scanistruth.cfg`, `..._sab_cleanupaboveckpt.cfg`, `..._sab_staleckptcorruption.cfg`
- Modify: `docs/superpowers/models/run_refsnaplog.sh` (create if absent — follow `run_foldclamp.sh` pattern)
- Create: `docs/superpowers/models/CaRefTableSnapshotLogCore_RESULTS.md`
- Delete configs: `..._latepred.cfg`, `..._sab_deletebeforesnapshot.cfg`, `..._sab_remountkeepsoldepoch.cfg`, `..._sab_vanishiscorruption.cfg`, `..._sab_recreatebeforecompleted.cfg`, `..._rev6_*.cfg` (superseded; their RESULTS notes migrate to the new RESULTS file's "history" section)

**Interfaces:**
- Consumes: nothing (first task).
- Produces: the vocabulary later tasks reference: `pendingSlot` (at-most-one in-flight id), `sealedIds` (EpochSeal-occupied ids), `ckpt` (record `[base |-> Nat, seal |-> Nat]`), invariant names `INV_DENSE`, `INV_RECOVERY`, `INV_NO_GHOST`, `INV_NOFAIL`.

The current module (453 lines) models: ordered scan + pick-greatest-snapshot recovery, cleanup gated on scanned snapshots, LatePredecessorPut as EXPECTED-FAIL. The v9 rewrite keeps `op`/`writtenEver`/`logs`/`snaps`/`snapCov`/`FoldIds` and the reader phase machine, and changes the protocol around them.

- [ ] **Step 1: Rewrite the module.** Key deltas (write these exactly; keep everything else from the current module that still compiles against them):

Replace the allocator and failure actions (INV-1). `nextId` is DELETED; ids derive from state; `pendingSlot` (0 = none) is the single in-flight id; `ambiguousEver` marks slots that had an ambiguous attempt:

```tla
VARIABLES pendingSlot, ambiguousEver, sealedIds   \* new; delete nextId
MaxWritten == IF writtenEver \cup sealedIds = {} THEN 0 ELSE MaxOf(writtenEver \cup sealedIds)
NextSlot == MaxWritten + 1

WAppendStart(o) ==            \* o \in {"birth","mut","remove"}
    /\ ReaderInactive /\ pendingSlot = 0 /\ NextSlot <= MaxSeq
    /\ opGuard(o)             \* birth: WState.life="empty"; mut: ="live"; remove: ="live"
    /\ pendingSlot' = NextSlot
    /\ pendingOp' = o
    /\ UNCHANGED ...

WResolveDurable ==
    /\ pendingSlot # 0
    /\ pendingSlot \notin sealedIds                 \* a successor seal is a conclusive rejection
    /\ op' = [op EXCEPT ![pendingSlot] = pendingOp]
    /\ writtenEver' = writtenEver \cup {pendingSlot}
    /\ logs' = logs \cup {pendingSlot}
    /\ pendingSlot' = 0
    /\ UNCHANGED ...

WResolveConclusiveReject ==   \* every-attempt rule: only a PROVEN-never-applied attempt frees the id
    /\ pendingSlot # 0
    /\ (pendingSlot \notin ambiguousEver \/ SabotageReuseAfterAmbiguous)
    /\ pendingSlot' = 0       \* the id is REUSED by the next WAppendStart: no gap
    /\ UNCHANGED ...

WAttemptAmbiguous ==          \* an attempt was sent, outcome unknown: the lane stays wedged
    /\ pendingSlot # 0
    /\ ambiguousEver' = ambiguousEver \cup {pendingSlot}
    /\ UNCHANGED ...
```

Add the recovery CAS-walk and the seal (INV-2). Recovery replaces the pick-greatest-snapshot scan: base comes from `ckpt` (point read, honest), the tail is walked by arithmetic, and the walk ends by occupying the next slot:

```tla
VARIABLE ckpt                 \* [base |-> 0..MaxSeq, seal |-> 0..MaxSeq]; 0 = none
RSealSlot ==                  \* slot-occupy Created: close the epoch at the frontier
    /\ rPhase = "walk"
    /\ rWalkPos = MaxWritten                        \* walked through every durable id
    /\ NextSlot <= MaxSeq
    /\ IF pendingSlot = NextSlot /\ pendingSlot # 0
       THEN sealedIds' = sealedIds \cup {NextSlot}  \* the ghost's only slot is now taken
       ELSE sealedIds' = sealedIds \cup {NextSlot}
    /\ ckpt' = [ckpt EXCEPT !.seal = NextSlot]
    /\ rPhase' = "done"
RAdoptStraggler ==            \* slot-occupy Occupied: the in-flight PUT landed first — adopt it
    /\ rPhase = "walk" /\ rWalkPos = MaxWritten
    /\ pendingSlot = NextSlot /\ pendingSlot # 0
    /\ op' = [op EXCEPT ![pendingSlot] = pendingOp]
    /\ writtenEver' = writtenEver \cup {pendingSlot}
    /\ logs' = logs \cup {pendingSlot}
    /\ pendingSlot' = 0
    /\ UNCHANGED << rPhase, ... >>                  \* the walk continues over the adopted id
RWalkStep ==                  \* arithmetic advance: expected next = rWalkPos+1, membership in
    /\ rPhase = "walk"        \* logs is the honest point read; the HINT plays no part
    /\ rWalkPos < MaxWritten
    /\ (rWalkPos + 1 \in writtenEver \/ rWalkPos + 1 \in sealedIds)   \* density: no gaps exist
    /\ (rWalkPos + 1 \in logs \/ rWalkPos + 1 \in sealedIds \/ rWalkPos + 1 <= ckpt.base)
    /\ rWalkPos' = rWalkPos + 1
```

`LatePredecessorPut` FLIPS: keep the action, gate `LatePred`, but its guard now requires its exact slot to be free — `L = pendingSlot`-shaped stragglers only, and `L \notin sealedIds`. With the walk+seal above, the `_v9_flip_latepred.cfg` config (LatePred = TRUE, sabotages FALSE) must be GREEN: TLC explores the action, and `INV_RECOVERY`/`INV_NO_GHOST` hold because a landed straggler is adopted and an unlanded one is fenced by the seal:

```tla
INV_NO_GHOST == \A i \in writtenEver : i \notin sealedIds     \* nothing lands in a sealed slot
INV_DENSE == \A i \in (writtenEver \cup sealedIds) : \A j \in 1..i : j \in (writtenEver \cup sealedIds) \/ j = pendingSlot
```

`_ckpt` gates cleanup and recovery base (INV-4): `GcCleanupLog` guard becomes `L <= ckpt.base` (sabotage `SabotageCleanupAboveCkpt` drops it → must break `INV_RECOVERY`); snapshot publication CASes `ckpt.base` upward; the reader's vanish three-way: on missing base, reread `ckpt` — if it advanced, restart; if unchanged, `rPhase' = "failed"` ONLY under `SabotageStaleCkptCorruption`... invert per spec: honest = unchanged-token missing base IS corruption (`stuck-corrupt` phase, and the invariant asserts it is unreachable in honest runs because deletion is gated strictly below `ckpt.base`); the sabotage `SabotageStaleCkptCorruption` makes cleanup race the base away to prove the gate is load-bearing.

The hint demotion sabotage `SabotageScanIsTruth`: the reader takes `rSeenLogs` from the scan as the fold set (old behavior) instead of the arithmetic walk — with a scan that may omit keys (keep `ReaderScanStep` but ALLOW skipping: drop the "must be minimum" constraint unconditionally — omission is the modeled norm now), `INV_RECOVERY` must VIOLATE, which is the observed `0x1430c/d` defect as a model counterexample.

- [ ] **Step 2: Write the six configs.** `_v9_safe`: all sabotages FALSE, `LatePred = FALSE`, invariants `TypeOK INV_RECOVERY INV_NOFAIL INV_DENSE INV_NO_GHOST`; `MaxSeq = 4`, `MaxRestarts = 2`. `_v9_flip_latepred`: `LatePred = TRUE`, same invariants, expect GREEN (the flip). Each `_sab_*`: exactly one sabotage TRUE, expect violation; name the violated invariant in a comment at the top of the cfg.

- [ ] **Step 3: Run the sabotages FIRST** (each must be red): `bash docs/superpowers/models/run_refsnaplog.sh` after writing it with expectations `_v9_safe green`, `_v9_flip_latepred green`, four `_sab_* violation`. Expected first run: sabotages VIOLATE (paste the violated invariant name from each log into the RESULTS file), greens PASS.

- [ ] **Step 4: `CaRefTableSnapshotLogCore_RESULTS.md`:** table of configs × expected × observed × violated-invariant; a "history" section naming the deleted rev.4/rev.6 configs and WHY (superseded by v9 semantics; `LatePredecessorPut` flipped from expected-fail to green — cite spec §2 INV-2); state-space sizes and runtimes.

- [ ] **Step 5: Commit** `ca: tla — CaRefTableSnapshotLogCore v9 rewrite (contiguity, slot-occupy seal, ckpt; LatePredecessorPut flipped green)`.

---

### Task 2: Rewrite `CaRefDeltaIntakeCore` — pool-wide fold, frontier proof, durable hold

**Files:**
- Modify: `docs/superpowers/models/CaRefDeltaIntakeCore.tla` (full rewrite; keep module name)
- Create: configs `..._v9_safe.cfg`, `..._v9_hintomission.cfg`, `..._sab_skipquietprobe.cfg`, `..._sab_rebuilddropshold.cfg`, `..._sab_clearholdonabsent.cfg`, `..._sab_cleanupignorescursor.cfg` (retained from today), and delete `..._latepred.cfg`, `..._sab_resumeskip.cfg`, `..._sab_adoptbeforecommit.cfg` if their subject is retained — KEEP `_sab_adoptbeforecommit` (cursor-adoption atomicity is unchanged and still load-bearing); `_sab_resumeskip`'s subject (pagination honesty) is DELETED BY DESIGN — record in RESULTS that hint omission is now the modeled NORM (`_v9_hintomission` green replaces it).
- Modify/Create: `docs/superpowers/models/run_deltaintake.sh`
- Create: `docs/superpowers/models/CaRefDeltaIntakeCore_RESULTS.md`

**Interfaces:**
- Consumes: Task 1's vocabulary conceptually (`pendingId` wedge stays; contiguity via state-derived ids).
- Produces: invariant names later tasks and the main plan cite: `NoMissedFold`, `NoAckedLoss`, `HoldSuppresses`, `ExactlyOnce`, `LosingCommitAdoptsNothing`.

- [ ] **Step 1: Rewrite.** Keep: two tables, `pendingId` wedge, `cursor/cand/csnap`, `FoldCommitWin/Lose` atomicity, `Cleanup` cursor∧snap gate, `dupFlag`. Change:

Allocator (contiguity): delete `nextId`; `WAppendStart(t)` sets `pendingId[t]` to `MaxDurable(t)+1` where `MaxDurable(t) == IF DurableIds(t) \cup SealedIds(t) = {} THEN 0 ELSE Max(...)`; `WAppendAbandon` requires the attempt conclusively rejected (model as before — the ambiguity split is Task 1's subject; here abandon = slot reusable, no gap).

The scan becomes a HINT: `PageStep` may skip any key (drop the minimum constraint unconditionally) and feeds ONLY `delta`-candidates; the fold's cursor advance happens through a NEW arithmetic action:

```tla
WalkStep(t) ==                    \* the fold's real advance: point-read by expected id
    /\ gcPhase = "scanning"
    /\ LET nxt == cand[t] + 1 IN
       /\ [t |-> t, i |-> nxt] \in durable \/ nxt \in sealed[t]
       /\ cand' = [cand EXCEPT ![t] = nxt]
       /\ delta' = IF [t |-> t, i |-> nxt] \in durable /\ nxt > csnap[t]
                   THEN delta \cup {[t |-> t, i |-> nxt]} ELSE delta
```

Pool-wide blob state (the r7-1 scenario): each key carries a fixed edge role via a CONSTANT function `EdgeOf \in [AllKeys -> {"add","rem","none"}]` chosen in the cfg so that T1 holds a late `add` for the one shared blob and T2 holds its `rem`. Fold maintains `indeg == Cardinality({k \in folded : EdgeOf[k]="add"}) - Cardinality({k \in folded : EdgeOf[k]="rem"})` (guard the cfg's EdgeOf so indeg never goes negative in honest runs); pipeline:

```tla
VARIABLES condemned, deleted, hold      \* hold \in [Tables -> BOOLEAN]
FrontierProof(t) == ([t |-> t, i |-> cursor[t] + 1] \notin durable) \/ probedWalked[t]
Condemn == /\ gcPhase = "idle" /\ indeg = 0 /\ ~condemned /\ ~deleted
           /\ (\A t \in Tables : FrontierProof(t)) \/ SabotageSkipQuietProbe
           /\ \A t \in Tables : ~hold[t]
           /\ condemned' = TRUE
Delete ==  /\ condemned /\ ~deleted /\ roundsSinceCondemn >= 1     \* two-phase pacing
           /\ (\A t \in Tables : ~hold[t]) \/ SabotageRebuildDropsHold
           /\ deleted' = TRUE
NoAckedLoss == deleted => (\A k \in everDurable : EdgeOf[k] = "add" => k \in folded)
```

Holds: `HoldSet(t)` on an impossible shape (model trigger: a probe finds `cursor[t]+1` absent while some durable id `> cursor[t]+1` exists for `t` — the below-witness 404); `HoldClear(t)` requires folding THROUGH the offending id (`[t, cursor[t]+1] \in folded'`) — sabotage `SabotageClearHoldOnAbsent` clears on a second absent probe instead → must violate `NoAckedLoss`. `SabotageRebuildDropsHold`: a `Rebuild` action resets `hold` to all-FALSE while keeping cursors → must violate `NoAckedLoss` via the r8-blocker scenario. `HoldSuppresses == (\E t : hold[t]) => ~condemned'` is expressed as the guard above; the invariant form: `(\E t \in Tables : hold[t]) => ~deleted`.

`LatePredecessorPut` is DELETED from this module (Task 1 owns the flip; this module's contiguity makes the old action unrepresentable — record in RESULTS).

- [ ] **Step 2: Configs.** `_v9_safe` (all FALSE; invariants `TypeOK NoMissedFold ExactlyOnce LosingCommitAdoptsNothing NoAckedLoss`); `_v9_hintomission` — identical but with a cfg comment stating the scan constraint is already permissive; green proves omission-tolerance; `_sab_skipquietprobe` / `_sab_rebuilddropshold` / `_sab_clearholdonabsent` / `_sab_adoptbeforecommit` / `_sab_cleanupignorescursor` — one toggle each, expect violation, name the invariant.

- [ ] **Step 3: Run sabotages first, then greens** via `run_deltaintake.sh` (same harness pattern; expectations per Step 2).

- [ ] **Step 4: RESULTS.md** — configs table + the two design notes: `_sab_resumeskip` retired (pagination honesty is no longer load-bearing — omission is the norm), `latepred` moved to Task 1's flip.

- [ ] **Step 5: Commit** `ca: tla — CaRefDeltaIntakeCore v9 rewrite (arithmetic walk, frontier proof, durable hold, shared-blob NoAckedLoss)`.

---

### Task 3: New `CaRefCatalogCore` — catalog lifecycle, incarnations, `_ckpt` ordering

**Files:**
- Create: `docs/superpowers/models/CaRefCatalogCore.tla`
- Create: configs `..._safe.cfg`, `..._churn.cfg`, `..._sab_janitoreatsnewborn.cfg`, `..._sab_reconcilelivecreator.cfg`, `..._sab_entrybeforeckptdelete.cfg`, `..._sab_sameincarnationrebirth.cfg`
- Create: `docs/superpowers/models/run_refcatalog.sh`, `docs/superpowers/models/CaRefCatalogCore_RESULTS.md`

**Interfaces:**
- Consumes: nothing mechanical; vocabulary from `CaIncarnationCore` (read its header for naming style only).
- Produces: invariant names `INV_NO_ALIAS`, `INV_NEWBORN_SAFE`, `INV_BOUNDED_CATALOG`, `INV_RECONCILE_SAFE` for the main plan's tests.

- [ ] **Step 1: Write the module.** One logical name, incarnations `1..MaxInc`; state:

```tla
VARIABLES
    entry,        \* [state |-> {"absent","creating","live","removing"}, inc |-> 0..MaxInc]
    creatorAlive, \* BOOLEAN: the Creating creator's fence is still live
    objects,      \* SUBSET (1..MaxInc): incarnations owning surviving ref-layer objects
    ckptOf,       \* SUBSET (1..MaxInc): incarnations whose _ckpt object exists
    nextInc,      \* next incarnation to mint
    aliased,      \* ghost: a new life read/was affected by an old life's object
    newbornEaten  \* ghost: the janitor deleted a creating-incarnation object
```

Actions: `Create` (absent→creating, mint `nextInc`, `creatorAlive' = TRUE) → `CkptCreate` (`ckptOf ∪ {inc}`) → `GoLive`; `CreatorDies` (creatorAlive' = FALSE); `ReconcileCreating` — guard `entry.state="creating" ∧ (¬creatorAlive ∨ SabotageReconcileLiveCreator)` → absent (+ `newbornEaten' = newbornEaten ∨ creatorAlive`); `Drop` (live→removing) → `TerminalFoldAndCleanup` (best-effort: nondeterministically leaves `objects` remnants of `inc`) → `CkptDelete` then `EntryDelete` — sabotage `SabotageEntryBeforeCkptDelete` swaps the order and a successor `Create` while the old `_ckpt` survives sets `aliased` if the new life's `CkptCreate` collides (model: collides iff same incarnation — with `SabotageSameIncarnationRebirth`, `Create` REUSES the dead incarnation number instead of minting → any surviving old object of that incarnation sets `aliased` on the new life's first read action `ReadOwn`); `Janitor` — deletes any object whose incarnation is NOT the catalog entry's current `inc` — honest guard excludes nothing else; `SabotageJanitorEatsNewborn` widens it to also delete `creating`-incarnation objects → `newbornEaten`.

Invariants:

```tla
INV_NO_ALIAS == ~aliased
INV_NEWBORN_SAFE == ~newbornEaten
INV_BOUNDED_CATALOG == TRUE   \* structural in a 1-name model; the churn cfg's bound is the check
INV_RECONCILE_SAFE == (entry.state = "creating") => (ckptOf # {} \/ creatorAlive \/ entry.inc = 0)
```

`_churn.cfg` sets `MaxInc = 3` and TLC's state constraint to allow ≥ 3 full create→drop→recreate cycles: green proves the lifecycle terminates each life and the entry count never exceeds one live+one removing (assert via `TypeOK`-style bound), the model analogue of the user's create/drop-per-second scenario.

- [ ] **Step 2–3: sabotages first (each red, note the invariant), then `_safe` and `_churn` green** via `run_refcatalog.sh`.
- [ ] **Step 4: RESULTS.md** (configs table; note that incarnation-scoped inertness is what lets `EntryDelete` skip any physical-empty proof — cite spec INV-3).
- [ ] **Step 5: Commit** `ca: tla — CaRefCatalogCore (lifecycles, incarnation inertness, janitor safety, churn bound)`.

---

### Task 4: Rewrite `CaRefNsCleanupStaleLeaderCore` around catalog incarnations

**Files:**
- Modify: `docs/superpowers/models/CaRefNsCleanupStaleLeaderCore.tla` (129 lines today — read fully first; keep its stale-leader straggler actions)
- Configs: keep/adapt its existing cfg set; add `..._sab_noincarnation.cfg`
- Modify: its run script (find by `grep -l NsCleanup docs/superpowers/models/run_*.sh`; create `run_nscleanup.sh` if none)
- Create/append: `CaRefNsCleanupStaleLeaderCore_RESULTS.md`

**Interfaces:**
- Consumes: Task 3's incarnation semantics (conceptually).
- Produces: the structural-inertness witness the main plan cites for deleting `_cleanup` gating on the ref layer.

- [ ] **Step 1:** Read the module; replace its `_cleanup`-marker/`Completed` recreation gate with: recreation mints a fresh incarnation (Task 3 semantics inlined: an `inc` component on every object and on the live table), stale-leader physical deletes target OLD-incarnation objects only by construction of their guard (they captured `inc` before deposition). The old model's hazard invariant ("straggler deletes new-life object") becomes STRUCTURAL: honest config green; `_sab_noincarnation.cfg` (recreation reuses the incarnation) must violate it — that is the model-level proof that incarnations, not physical-empty polling, carry rebirth safety.
- [ ] **Step 2–4:** sabotage first (red), greens, RESULTS note ("`Completed`-marker gate deleted for the REF layer per spec §3; the FILE layer keeps it — register R1"), run script expectations updated.
- [ ] **Step 5: Commit** `ca: tla — CaRefNsCleanupStaleLeaderCore rewrite (incarnation-structural rebirth safety)`.

---

### Task 5: Extend `CaCasMountCore` — recovery generations and the wedge-vs-seal race

**Files:**
- Modify: `docs/superpowers/models/CaCasMountCore.tla` (1027 lines — read its header comment, variable block, and `CaCasMountCore_RESULTS.md` FIRST; extend, do not restructure)
- Create: `docs/superpowers/models/CaCasMountCore_sab_staleinstall.cfg`, `..._sab_wedgeretryoldgen.cfg`; a green `..._v9_recoverygen.cfg`
- Modify: its run script; append to `CaCasMountCore_RESULTS.md`

**Interfaces:**
- Consumes: the module's existing fence/epoch variables (adapt names to what the module defines — its RESULTS file documents them).
- Produces: the generation-gating witness the main plan cites for `slot-occupy`/`_ckpt`/install code changes.

- [ ] **Step 1:** Add to the module: a `recGen` captured by a recovery-start action from the module's existing fence-generation counter; an `Install` action guarded `recGen = <current fence generation> ∧ ¬superseded`; a `WedgeRetry` action performing one conditional create guarded by its captured generation; a successor `SealSlot` that conclusively rejects an old-generation retry. Two sabotages: `SabotageStaleInstall` drops the install guard (an old recovery installs after a remount → violate the module's existing exclusivity/no-two-writers invariant — reuse it, do not invent a parallel one); `SabotageWedgeRetryOldGen` lets the retry run under the NEW generation unchecked (must violate the same or the no-double-apply property — pick whichever existing invariant the counterexample actually trips, and name it in the cfg comment).
- [ ] **Step 2–4:** sabotages red first, green config, RESULTS append with runtimes.
- [ ] **Step 5: Commit** `ca: tla — CaCasMountCore recovery-generation gating (stale install / old-gen wedge retry sabotages)`.

---

### Task 6: Audit remaining models + phase summary

**Files:**
- Read: `docs/superpowers/models/CaErasureProof.tla`, `CaDiskLifecycle.tla`, `CaGcAckFloorCore.tla`, `CaGcAckFloorZombie.tla`, `CaGcCondemnMarkerGate.tla`, `CaRelinkConfirmCore.tla` (its `_sab_holeylist` section)
- Create: `docs/superpowers/models/2026-07-28-v9-phase-RESULTS.md` (the phase summary)
- Modify: `CaRelinkConfirmCore`'s RESULTS file (or create) — the holeylist-witness note

**Interfaces:**
- Consumes: all prior tasks' RESULTS files.
- Produces: the gate verdict the main plan's Task 0 checks.

- [ ] **Step 1:** For each audited model, grep for listing-derived guards (`LIST`, `scan`, `enumerat`, `snaps`, `logs` used as discovery) and record in the summary: either "no LIST-trust encoded" or the exact action + whether v9 changes its premise. `CaRelinkConfirmCore` is NOT rewritten: its subject (publish-confirm relink) is unchanged; add the note that its `_sab_holeylist` config remains the historical defect witness and that the fold-side flip lives in `CaRefDeltaIntakeCore_v9_hintomission` (green) + `CaRefTableSnapshotLogCore_sab_scanistruth` (red) — together the regression pair.
- [ ] **Step 2:** `2026-07-28-v9-phase-RESULTS.md`: one table — every model touched, every config, expected vs observed, runtimes; a GATE line: `TLA PHASE: PASS` only if every green is green and every `_sab_` is red; anything else = `FAIL` with the failing config named.
- [ ] **Step 3:** Run ALL run scripts end-to-end once more from a clean `tmp/tlc-meta-*` state; paste the harness outputs into the summary.
- [ ] **Step 4: Commit** `ca: tla — v9 phase audit + gate summary (all greens green, all sabotages red)`.

---

## Self-review notes (done at write time)

- Spec coverage: INV-1 (Task 1 allocator + Task 2 contiguity), INV-2 (Task 1 seal/adopt + flip), INV-3 (Task 3 + Task 4), INV-4 (Task 1 ckpt gates), frontier proof + hold + REBUILD-hold (Task 2), recovery generations (Task 5), regression witness (Task 6). The temporal lemma's writer-side arms (Condemned-meta rematerialization, tokenless relink ordering) are NOT modeled here — they are existing-code properties verified by the main plan's C++ tests; recorded in the phase summary as a deliberate scope note.
- Placeholders: none — every step names exact actions/invariants/configs or instructs reading a named file first where adaptation to existing names is required (Tasks 4/5, justified: extend-don't-restructure).
- Type consistency: invariant names cross-referenced between Tasks 1/2 and Task 6's summary.
