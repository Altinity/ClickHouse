# CAS relink / write-release seam — TLA+ gate phase Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Discharge §12 of `docs/superpowers/specs/2026-07-29-cas-relink-reoffer-redesign.md` — build the refined model the redesign requires, run it, and answer the one question that can still stop the design: **does `_sab_stalecache` flip from RED to GREEN once the apply-pending marker is represented?** If it does not, the design does not ship.

**Architecture:** One new TLA+ module, `docs/superpowers/models/CaRelinkReofferCore.tla`, which REFINES `CaRelinkConfirmCore` rather than editing it (§12's disposition ruling: the v11 model is kept as the historical witness of the v11 protocol and is not touched, because its `_sab_*` reds are the evidence that v11's rules were each load-bearing). The refinement adds three things the v11 model cannot express: the apply-pending marker (`sApply`, armed strictly before the durable PUT and cleared atomically with the install — the model-level twin of seam §6), an equal-namespace/different-`disk_name` mount pair (§12.5 row i, test row 17's B1 shape), and a leader tenure that commits several durable chunks (§12.5 row ii). Around them, a 2×2 necessity matrix decides the gate, a cross-mount battery decides the validator's qualification, and the re-derived rule set decides that every retained rule is still load-bearing under the fence-first ordering. Cfg names are deliberately carried over from the v11 model so that the flip is greppable side by side: `CaRelinkConfirmCore_sab_stalecache` RED next to `CaRelinkReofferCore_sab_stalecache` GREEN.

**Tech Stack:** TLA+ / TLC 2 (`tmp/tla2tools.jar` → TLAToolbox 1.7.4, OpenJDK 21), invoked exactly as the existing `docs/superpowers/models/run_*.sh` harnesses do. No C++ in this phase.

## Global Constraints

- Branch `cas-gc-rebuild`; commit after every task; **NEVER `git push`**.
- Commit messages: `ca: tla — <what>` plus this session's standard trailer lines.
- **NEVER weaken an existing invariant, or an existing model, to make a new config pass.** `CaRelinkConfirmCore.tla` and all twelve `CaRelinkConfirmCore_*.cfg` files are **read-only for this whole plan** — the only file of that family this plan may touch is `CaRelinkConfirmCore_RESULTS.md`, and only to append a pointer section (Task 4). If a new config only passes after an invariant is narrowed, that is a FAIL to record, not an edit to make.
- **Every cfg change is reviewed against its sabotage intent before it is run.** Each `_sab_*` cfg header states, in one sentence, which single rule it removes and which invariant that must break; a cfg whose observed red is a DIFFERENT invariant than the one named is a FAIL, not a pass — the runner asserts the invariant NAME, not the exit code.
- **Every TLC run is logged with markers.** The runner writes `../../../tmp/tlc_CaRelinkReofferCore_<cfg>.log` and brackets each invocation with `=== TLC BEGIN <module>_<cfg> <ISO-8601> ===` / `=== TLC END <module>_<cfg> rc=<n> <ISO-8601> ===`. Every row of the RESULTS table cites its log path.
- TLC invocation pattern (copied from `docs/superpowers/models/run_refcatalog.sh`): `/usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp ../../../tmp/tla2tools.jar tlc2.TLC -metadir ../../../tmp/tlc-meta-relinkreoffer-<cfg> -workers "${TLC_WORKERS:-1}" -config <cfg>.cfg CaRelinkReofferCore.tla`. Verdict: `grep -q "No error has been found"` ⇒ green; `grep -q "is violated"` ⇒ violation, with the invariant name extracted by `grep -oE '(Invariant|Property) [A-Za-z_]+ is violated'`.
- `-workers 1` by default, **not** `-workers auto`, following `run_refcatalog.sh`: parallel BFS makes WHICH shortest counterexample TLC prints nondeterministic between identical runs, and every trace this plan narrates in RESULTS is a specific action sequence. Override with `TLC_WORKERS=auto` when only a verdict is wanted.
- **Every declared CONSTANT must be assigned in every `.cfg`.** Task 1 therefore declares the COMPLETE constants block, including dials only exercised in Tasks 2 and 3, and Task 1's configs set the unexercised ones to their honest value. Adding a constant later would force an edit to every previously written cfg — do not do that.
- Model sizes: keep `MaxId <= 6`, `MaxRound <= 5`, `MaxChunks <= 2`, `Receivers = {r1}`. If a config exceeds ~10 min under `-workers 1`, shrink the bounds — **never drop a property**. Record the bound that was used in RESULTS.
- **A green is only evidence once the property it rests on has been seen RED.** Sabotages run before greens, in every task and in the runner's config order.
- The spec is the requirements source. Where this plan and spec §12 disagree, the spec wins and the discrepancy is reported rather than silently resolved.
- **The gate can fail.** If Task 1 step 8 finds `_sab_stalecache` RED, STOP: do not start Task 2. Record the counterexample, write the RESULTS file with `RELINK TLA GATE: FAIL`, and report the design as refuted. That is a legitimate, successful outcome of this plan.

---

## File map

| File | Responsibility |
| --- | --- |
| `docs/superpowers/models/CaRelinkReofferCore.tla` | NEW. The refined model: sender lane with the apply marker and chunked tenures, two same-pool mounts, the fence-first answer, the four-state receiver, GC. |
| `docs/superpowers/models/CaRelinkReofferCore_*.cfg` | NEW, 25 configs: 11 sabotages, 6 greens, 8 witnesses. |
| `docs/superpowers/models/run_relinkreoffer.sh` | NEW. Expected-verdict harness in the `run_refcatalog.sh` shape, with BEGIN/END markers. |
| `docs/superpowers/models/2026-07-29-relink-seam-tla-RESULTS.md` | NEW. The phase verdict: `RELINK TLA GATE: PASS|FAIL` plus the do-not-implement consequence. |
| `docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md` | APPEND ONLY (Task 4). One section pointing at the refinement and stating that the v11 `_sab_stalecache` RED is deliberately preserved. |

---

### Task 1: The refined module and the apply-marker 2×2 — THE GATE

This is the task that can stop the design. Everything else in the plan is conditional on its step 8.

**Files:**
- Create: `docs/superpowers/models/CaRelinkReofferCore.tla`
- Create: `docs/superpowers/models/CaRelinkReofferCore_sab_noapplypending.cfg`
- Create: `docs/superpowers/models/CaRelinkReofferCore_sab_noapplypending_window.cfg`
- Create: `docs/superpowers/models/CaRelinkReofferCore_v11_baseline.cfg`
- Create: `docs/superpowers/models/CaRelinkReofferCore_ctl_v11nomarker.cfg`
- Create: `docs/superpowers/models/CaRelinkReofferCore_sab_stalecache.cfg`
- Create: `docs/superpowers/models/CaRelinkReofferCore_witness_busylane.cfg`
- Create: `docs/superpowers/models/CaRelinkReofferCore_witness_midtenure.cfg`
- Create: `docs/superpowers/models/CaRelinkReofferCore_witness_proven.cfg`
- Create: `docs/superpowers/models/CaRelinkReofferCore_witness_delete.cfg`
- Create: `docs/superpowers/models/run_relinkreoffer.sh`
- Read first: `docs/superpowers/models/CaRelinkConfirmCore.tla` (all 436 lines), `docs/superpowers/models/run_refcatalog.sh`, spec §12.3, §5.1.1, §5.1.2, seam §6.

**Interfaces:**
- Consumes: nothing (first task).
- Produces: the vocabulary every later task references — module name `CaRelinkReofferCore`; constants `Receivers, MaxId, MaxRound, MaxChunks, SecondMount, EqualNamespaces, ModelColdTable, TrackHistory, SabotageStaleCache, SabotageNoApplyPending, SabotageNoPoison, SabotageNoWedge, SabotageNoFence, SabotageNoRowExact, SabotageBareValidator, SabotageNoDiskQual, SabotagePublishAfterConfirm, SabotageS2ByteFetch`; operators `Validator(m, b)`, `HeldValidator`, `GateRefuses(m)`, `Answer(m)`, `BlobOf(m)`, `NsOf(m)`, `AdoptedBlobs`; invariants `TypeOK`, `ConfirmedRelinkNeverDangles`, `PromotedNeverDangles`, `MarkerCoversDurableWindow`, `NeverPublishedTwice`, `ChangedImpliesFenced`; witnesses `W_BusyLaneProven`, `W_MidTenureCommit`, `W_ProvenCommitted`, `W_BlobDeleted`.

---

- [ ] **Step 1: Read the v11 model end to end, and write down the three things it cannot express.**

Read `docs/superpowers/models/CaRelinkConfirmCore.tla` in full. Before writing any new TLA+, write these three sentences into a scratch note (they become the new module's header, and getting them wrong is how this task goes wrong):

1. `SenderDurable` (`:180-189`) makes a transaction durable while leaving `sCacheRef` un-updated, and the ONLY predicate that refuses in that state is rule 3's `quiescent == SabotageStaleCache \/ (~sPending /\ ~sLeader)` (`:269`). `sPoison` is set only by `SenderPoison` — the apply-THREW case — so **the v11 model has no representation of the code's `ApplyPending` marker at all.**
2. `NsS` is a single fixed sender namespace and `Token` identifies its blobs globally, so **a cross-mount validator collision is unrepresentable.**
3. `SenderAdmit → SenderDurable → SenderApply` is one transaction per tenure, so **`~sLeader` and "the marker is clear" coincide by construction** and cannot be told apart.

- [ ] **Step 2: Write the module's header, universe and named assumptions.**

Create `docs/superpowers/models/CaRelinkReofferCore.tla`. The header comment MUST contain, in this order: (a) what is under test — the re-offer confirm of spec `2026-07-29-cas-relink-reoffer-redesign.md` §4.2's five-step fence-first ordering; (b) the three sentences from step 1, stated as what this module adds; (c) the loud note below about `_sab_stalecache`; (d) the named-assumptions block below verbatim; (e) the scope notes below verbatim.

The `_sab_stalecache` note, verbatim — a fresh reader who misses this will misread the entire battery:

```tla
(* *** `SabotageStaleCache = TRUE` IS THE v12 DESIGN, NOT A SABOTAGE. ***
   In `CaRelinkConfirmCore` that flag DROPPED gate 1 rule 3's `!pending.empty() || leader_active`
   terms and MUST produce a counterexample.  This design DELETES those terms (§4.2, §5.1), so here
   the same flag selects the shipped predicate.  The name is carried over deliberately: the whole
   evidence §12.3 asks for is that the SAME-NAMED config flipped verdict once the apply marker was
   represented -- `CaRelinkConfirmCore_sab_stalecache` RED beside
   `CaRelinkReofferCore_sab_stalecache` GREEN.  Renaming it would destroy the comparison. *)
```

The named assumptions, verbatim (§12.2, §12.5 iii, §12 `FreshCertifiedResponse`) — the `ASSUME` is a tripwire, not a proof: deleting an assumption without updating the count fails every config:

```tla
(* ---- THE THREE NAMED ASSUMPTIONS -- ASSUMED, NOT MODELLED ---------------------------------- *)
(* Each is named so that weakening the mechanism that discharges it breaks a NAMED thing rather
   than a silent one.  None of the three is checked here; all three are cited to where they ARE. *)
NamedAssumptions == {
    (* CommittedEdgesAreGcVisible (§12.2): a committed ref edge that is durable before a removal is
       appended is observed by every GC fold that observes the removal.  MODEL FORM: `GFold` folds
       ALL of `Avail` -- there is no `MaxHoles` dial in this module, because §12.1 reassigned
       listing completeness to the v9 chain models.  DISCHARGED BY: `CaRefDeltaIntakeCore_v9_safe`
       and `_v9_hintomission` GREEN plus `CaRefTableSnapshotLogCore_sab_scanistruth` RED
       (`2026-07-28-v9-phase-RESULTS.md`).  The historical refutation of the ASSUMPTION -- one
       incomplete page, once -- stays where it was found: `CaRelinkConfirmCore_sab_holeylist`,
       BACKLOG {#list-as-journal-dataloss-2026-07-25}. *)
    "CommittedEdgesAreGcVisible",
    (* UnresolvedPromoteNeverBytes (§12.5 iii): an unresolved promote never leads to a byte fetch.
       ASSUMED of the CODE.  DISCHARGED BY: relink spec §11 row 9.  Its CONSEQUENCE is modelled --
       `_sab_s2bytefetch` shows what breaks when the assumption is false -- so this module records
       why the assumption matters without pretending to establish it. *)
    "UnresolvedPromoteNeverBytes",
    (* FreshCertifiedResponse (§12): the response the receiver acts on was produced by the sender,
       in reply to THIS request, after T1 -- not replayed, and not an offer response mistaken for a
       confirm.  Request/response generations are a TRANSPORT property; teaching them to this model
       would grow its state space for no gain over a direct test.  DISCHARGED BY: relink spec §11
       row 16 (nonce echo) and row 15 subcases (a) and (b) (the confirm-only answer cookie, and
       normative `assertEOF`).  If either row is weakened, this assumption goes with it. *)
    "FreshCertifiedResponse" }

ASSUME Cardinality(NamedAssumptions) = 3
```

The scope notes, verbatim:

```tla
(* ---- WHAT THIS MODULE DELIBERATELY DOES NOT MODEL ------------------------------------------ *)
(* From the write-release seam (`2026-07-29-cas-part-write-release-seam.md`):
     * §3's emission at `~PartWriteTxn` and §4's `attempted` transmission mark are ACCOUNTING, not
       safety.  Seam §3.3 states the counter is an upper bound on residue, and seam §9 point 5
       states the contract "never changes what a consumer does on any exit" -- so neither has
       safety content a model can gate.  They are discharged by seam §8 rows S1-S6c and by relink
       rows 19-20, not here.
     * §8 row S7 (marker synchronization) IS the row with model content, and `sApply` is its
       model-level twin.  Exactly WHICH half of S7 this module asserts is settled in
       `2026-07-29-relink-seam-tla-RESULTS.md` -- see `MarkerCoversDurableWindow`.
   Recovery COMPLETENESS -- that a recovered view is a complete replay of the durable log -- is not
   this answer's contract (§4.2, "Completeness is recovery's contract, not this answer's").  It is
   unrepresentable here by construction: `RecoverForAnswer` installs `sDurableRef` ATOMICALLY,
   which is also how §4.3's "abandoning before the install leaves nothing partial" is encoded.
   The wedge-resolution tenure is out of scope: the wedge refusal is retained unchanged (§12.5). *)
```

Now the universe. Write it exactly as follows:

```tla
-------------------- MODULE CaRelinkReofferCore --------------------
EXTENDS Integers, FiniteSets

CONSTANTS
    Receivers,                     \* relink receivers (one suffices for this safety class)
    MaxId,                         \* bound on the pool-wide ref-transaction id counter
    MaxRound,                      \* bound on the number of GC rounds
    MaxChunks,                     \* durable chunk transactions ONE leader tenure may commit
    SecondMount,                   \* a second same-pool mount: EQUAL root_namespace, DIFFERENT disk
    EqualNamespaces,               \* TRUE = the two mounts share a namespace string (§3's legal case)
    ModelColdTable,                \* TRUE = a mount may be evicted, so rule 2 is on the answer path
    TrackHistory,                  \* TRUE only in _witness_* cfgs: keeps history vars out of greens
    SabotageStaleCache,            \* drop v11 rule 3's pending/leader terms -- TRUE IS THE v12 DESIGN
    SabotageNoApplyPending,        \* the marker is NEVER ARMED (§12.3 step 2, first half)
    SabotageNoPoison,              \* the gate ignores apply_state = Poisoned
    SabotageNoWedge,               \* the gate ignores the wedge
    SabotageNoFence,               \* the gate ignores the mount fence (rule 1, hoisted first)
    SabotageNoRowExact,            \* rule 5 degenerates to "some binding is present"
    SabotageBareValidator,         \* validator = ManifestRef alone (B1's collision shape)
    SabotageNoDiskQual,            \* validator = namespace + ref, disk_name dropped (B1's fix)
    SabotagePublishAfterConfirm,   \* invert the order: confirm+promote BEFORE the durable +1
    SabotageS2ByteFetch            \* S2 byte-fetches instead of throwing retry-later

Token      == "m1"       \* the ManifestRef the offer minted; BOTH mounts may bind this same text
Other      == "m2"
NoBinding  == "none"
Bindings   == {Token, Other, NoBinding}

MountA     == "dA"       \* the mount that made the offer
MountB     == "dB"       \* a second CA disk, SAME pool, SAME server_root_id, DIFFERENT disk_name
Mounts     == IF SecondMount THEN {MountA, MountB} ELSE {MountA}
NsA        == "ns_s"
NsB        == IF EqualNamespaces THEN "ns_s" ELSE "ns_t"
NsOf(m)    == IF m = MountA THEN NsA ELSE NsB

BlobA      == "bA"       \* the blob mount A's manifest `m1` owns -- the one the receiver ADOPTS
BlobB      == "bB"       \* the blob mount B's manifest `m1` owns -- a DIFFERENT object, same text
Blobs      == IF SecondMount THEN {BlobA, BlobB} ELSE {BlobA}
BlobOf(m)  == IF m = MountA THEN BlobA ELSE BlobB
AdoptedBlobs == {BlobA}
EdgeOf(m)  == IF m = MountA THEN "s_dA" ELSE "s_dB"

Sources    == {"s_dA", "s_dB"} \cup Receivers
Namespaces == {NsA, NsB} \cup Receivers
Ids        == 0..MaxId
Rounds     == 0..MaxRound
Records    == [id: Ids, ns: Namespaces, blob: Blobs, src: Sources, op: {"add", "del"}]
```

- [ ] **Step 3: Write the variables, the sender lane with the marker, and the fence-first answer.**

Append to the module. This is the load-bearing part of the whole plan — the arm/install ordering IS the refinement:

```tla
VARIABLES
    round, present, condemned, pendingDelete, folded, cursor, gcPhase,   \* GC
    journal, nextId,                                                     \* the durable ref log
    sDurableRef, sCacheRef, sTarget, sPending, sLeader, sArmed, sApply,
    sWedge, sFence, sRecovered, sChunks,                                 \* per-mount lane state
    partMount,                                                           \* which mount holds the part
    rState, rAnswer, rDurableBefore, rAnsweredFenced, rPublishes, rUnresolved,
    sawBusyProven, sawMidTenure, sawProvenCommitted, sawChangedThenBytes,
    sawUnknown, sawColdRefused, sawCollision                             \* history (TrackHistory)

gcVars     == << round, present, condemned, pendingDelete, folded, cursor, gcPhase >>
senderVars == << sDurableRef, sCacheRef, sTarget, sPending, sLeader, sArmed, sApply,
                 sWedge, sFence, sRecovered, sChunks >>
recvVars   == << rState, rAnswer, rDurableBefore, rAnsweredFenced, rPublishes, rUnresolved >>
logVars    == << journal, nextId >>
histVars   == << sawBusyProven, sawMidTenure, sawProvenCommitted, sawChangedThenBytes,
                 sawUnknown, sawColdRefused, sawCollision >>
vars       == << gcVars, senderVars, recvVars, logVars, histVars, partMount >>

Max(S)   == CHOOSE x \in S : \A y \in S : y <= x
Indeg(b) == Cardinality({ e \in folded : e.b = b })
H(cond)  == TrackHistory /\ cond    \* history is recorded ONLY in the witness cfgs

Init ==
    /\ round = 0
    /\ present = [b \in Blobs |-> TRUE]
    /\ condemned = {}
    /\ pendingDelete = {}
    (* History: each mount's binding of Token over ITS OWN blob is committed and already folded. *)
    /\ folded = { [b |-> BlobOf(m), src |-> EdgeOf(m)] : m \in Mounts }
    /\ cursor = [ns \in Namespaces |-> 0]
    /\ gcPhase = "idle"
    /\ journal = {}
    /\ nextId = 1
    /\ sDurableRef = [m \in Mounts |-> Token]
    /\ sCacheRef   = [m \in Mounts |-> Token]
    /\ sTarget     = [m \in Mounts |-> Token]
    /\ sPending    = [m \in Mounts |-> FALSE]
    /\ sLeader     = [m \in Mounts |-> FALSE]
    /\ sArmed      = [m \in Mounts |-> FALSE]
    /\ sApply      = [m \in Mounts |-> "clean"]
    /\ sWedge      = [m \in Mounts |-> FALSE]
    /\ sFence      = [m \in Mounts |-> TRUE]
    /\ sRecovered  = [m \in Mounts |-> TRUE]
    /\ sChunks     = [m \in Mounts |-> 0]
    /\ partMount = MountA
    /\ rState  = [r \in Receivers |-> "init"]
    /\ rAnswer = [r \in Receivers |-> "none"]
    /\ rDurableBefore  = [r \in Receivers |-> FALSE]
    /\ rAnsweredFenced = [r \in Receivers |-> FALSE]
    /\ rPublishes  = [r \in Receivers |-> 0]
    /\ rUnresolved = [r \in Receivers |-> FALSE]
    /\ sawBusyProven = FALSE /\ sawMidTenure = FALSE /\ sawProvenCommitted = FALSE
    /\ sawChangedThenBytes = FALSE /\ sawUnknown = FALSE /\ sawColdRefused = FALSE
    /\ sawCollision = FALSE

(* ---- the sender's ref lane: ONE tenure, SEVERAL durable chunks ------------------------------ *)

(* Admission.  `pending` and `leader_active` are exactly the two predicates v11 rule 3 read under
   `ref_queue_mutex` -- and exactly the two this design deletes.  The tenure OPENS here and does not
   close per chunk: `sChunks` counts the durable transactions it commits (§12.5 ii). *)
SenderAdmit(m, nb) ==
    /\ sFence[m] /\ sRecovered[m]
    /\ ~sPending[m] /\ ~sWedge[m] /\ sApply[m] = "clean"
    /\ sDurableRef[m] = Token
    /\ sChunks[m] < MaxChunks
    /\ nextId <= MaxId
    /\ sPending' = [sPending EXCEPT ![m] = TRUE]
    /\ sLeader'  = [sLeader  EXCEPT ![m] = TRUE]
    /\ sTarget'  = [sTarget  EXCEPT ![m] = nb]
    /\ UNCHANGED << sDurableRef, sCacheRef, sArmed, sApply, sWedge, sFence, sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* THE ARM.  Seam §6: `armApplyPending` inside a `state_mutex` scope IMMEDIATELY before the PUT --
   "the last statement that still runs while nothing of this transaction can possibly be durable".
   `sArmed` makes the ORDERING structural; `SabotageNoApplyPending` removes only the marker's VALUE,
   which is exactly what "drop the marker" means. *)
SenderArm(m) ==
    /\ sPending[m] /\ ~sArmed[m] /\ sApply[m] = "clean"
    /\ sArmed' = [sArmed EXCEPT ![m] = TRUE]
    /\ sApply' = [sApply EXCEPT ![m] = IF SabotageNoApplyPending THEN "clean" ELSE "pending"]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sPending, sLeader, sWedge, sFence,
                    sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* The conditional PUT is acked: DURABLE, GC can fold it, the committed row still lags.  NO durable
   byte without a prior arm -- that guard is the ordering the whole design rests on. *)
SenderDurable(m) ==
    /\ sPending[m] /\ sArmed[m]
    /\ sDurableRef[m] = Token
    /\ nextId <= MaxId
    /\ journal' = journal \cup
         { [id |-> nextId, ns |-> NsOf(m), blob |-> BlobOf(m), src |-> EdgeOf(m), op |-> "del"] }
    /\ nextId' = nextId + 1
    /\ sDurableRef' = [sDurableRef EXCEPT ![m] = sTarget[m]]
    /\ UNCHANGED << sCacheRef, sTarget, sPending, sLeader, sArmed, sApply, sWedge, sFence,
                    sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, histVars, partMount >>

(* THE INSTALL.  `clearApplyPending` is the last statement of the install region, in the same scope
   as `rt->state.swap(*candidate)`: "recorded" and "no apply owed" become true together or not at
   all.  The TENURE SURVIVES: `sLeader` is untouched, so the next chunk of the same tenure may open
   while `sApply = "clean"` -- the state v11 refused and v12 answers from. *)
SenderInstall(m) ==
    /\ sArmed[m] /\ sDurableRef[m] # sCacheRef[m] /\ ~sWedge[m]
    /\ sCacheRef' = [sCacheRef EXCEPT ![m] = sDurableRef[m]]
    /\ sApply'    = [sApply    EXCEPT ![m] = "clean"]
    /\ sArmed'    = [sArmed    EXCEPT ![m] = FALSE]
    /\ sPending'  = [sPending  EXCEPT ![m] = FALSE]
    /\ sChunks'   = [sChunks   EXCEPT ![m] = sChunks[m] + 1]
    /\ sawMidTenure' = sawMidTenure \/ H(sLeader[m] /\ sChunks[m] >= 1)
    /\ UNCHANGED << sDurableRef, sTarget, sLeader, sWedge, sFence, sRecovered >>
    /\ UNCHANGED << gcVars, recvVars, logVars, partMount >>
    /\ UNCHANGED << sawBusyProven, sawProvenCommitted, sawChangedThenBytes, sawUnknown,
                    sawColdRefused, sawCollision >>

SenderCloseTenure(m) ==
    /\ sLeader[m] /\ ~sPending[m] /\ ~sArmed[m]
    /\ sLeader' = [sLeader EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sPending, sArmed, sApply, sWedge, sFence,
                    sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* The in-memory apply THREW although the object is durable.  The tenure closes -- the lane looks
   perfectly quiescent -- and only the Poisoned arm of rule 4 can see it. *)
SenderPoison(m) ==
    /\ sArmed[m] /\ sDurableRef[m] # sCacheRef[m] /\ ~sWedge[m]
    /\ sApply'   = [sApply   EXCEPT ![m] = "poisoned"]
    /\ sArmed'   = [sArmed   EXCEPT ![m] = FALSE]
    /\ sPending' = [sPending EXCEPT ![m] = FALSE]
    /\ sLeader'  = [sLeader  EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sWedge, sFence, sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* The append came back WITHOUT a verdict.  Two arms, because only one is dangerous.  Both CLEAR
   the marker -- that is the shipped behaviour (§5.1.1: every other exit "proves nothing was sent
   ... or leaves the table wedged or Poisoned") and it is why the wedge check is separately
   load-bearing rather than redundant with the marker. *)
SenderWedgeLanded(m) ==
    /\ sPending[m] /\ sArmed[m] /\ sDurableRef[m] = Token /\ nextId <= MaxId
    /\ journal' = journal \cup
         { [id |-> nextId, ns |-> NsOf(m), blob |-> BlobOf(m), src |-> EdgeOf(m), op |-> "del"] }
    /\ nextId' = nextId + 1
    /\ sDurableRef' = [sDurableRef EXCEPT ![m] = sTarget[m]]
    /\ sWedge'   = [sWedge   EXCEPT ![m] = TRUE]
    /\ sApply'   = [sApply   EXCEPT ![m] = "clean"]
    /\ sArmed'   = [sArmed   EXCEPT ![m] = FALSE]
    /\ sPending' = [sPending EXCEPT ![m] = FALSE]
    /\ sLeader'  = [sLeader  EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sCacheRef, sTarget, sFence, sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, histVars, partMount >>

SenderWedgeNotLanded(m) ==
    /\ sPending[m] /\ sArmed[m]
    /\ sWedge'   = [sWedge   EXCEPT ![m] = TRUE]
    /\ sApply'   = [sApply   EXCEPT ![m] = "clean"]
    /\ sArmed'   = [sArmed   EXCEPT ![m] = FALSE]
    /\ sPending' = [sPending EXCEPT ![m] = FALSE]
    /\ sLeader'  = [sLeader  EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sFence, sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

FenceLoss(m) ==
    /\ sFence[m] /\ ~sPending[m] /\ ~sArmed[m]
    /\ sFence' = [sFence EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sPending, sLeader, sArmed, sApply,
                    sWedge, sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* The namespace's NEW writer removes the binding: durable, folded by GC, invisible to the deposed
   instance's committed row.  This is the ONLY way `sDurableRef` moves without an arm, which is why
   `MarkerCoversDurableWindow` excludes fenced-out mounts. *)
ForeignRemove(m) ==
    /\ ~sFence[m] /\ sDurableRef[m] = Token /\ nextId <= MaxId
    /\ journal' = journal \cup
         { [id |-> nextId, ns |-> NsOf(m), blob |-> BlobOf(m), src |-> EdgeOf(m), op |-> "del"] }
    /\ nextId' = nextId + 1
    /\ sDurableRef' = [sDurableRef EXCEPT ![m] = NoBinding]
    /\ UNCHANGED << sCacheRef, sTarget, sPending, sLeader, sArmed, sApply, sWedge, sFence,
                    sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, histVars, partMount >>

(* Rule 2's two arms.  Recovery INSTALLS ATOMICALLY (§4.3), so a half-recovered view is
   unrepresentable; the budget arm simply leaves the table cold and the answer is `unknown`. *)
EvictTable(m) ==
    /\ ModelColdTable /\ sRecovered[m]
    /\ ~sPending[m] /\ ~sArmed[m] /\ ~sLeader[m] /\ ~sWedge[m] /\ sApply[m] = "clean"
    /\ sRecovered' = [sRecovered EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sPending, sLeader, sArmed, sApply,
                    sWedge, sFence, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

RecoverForAnswer(m) ==
    /\ ~sRecovered[m] /\ ~sPending[m] /\ ~sArmed[m]
    /\ sRecovered' = [sRecovered EXCEPT ![m] = TRUE]
    /\ sCacheRef'  = [sCacheRef  EXCEPT ![m] = sDurableRef[m]]
    /\ UNCHANGED << sDurableRef, sTarget, sPending, sLeader, sArmed, sApply, sWedge, sFence,
                    sChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* ---- the validator and §4.2's fence-first answer -------------------------------------------- *)

(* The digest of §4.1 is modelled as the TUPLE it hashes, and record equality is digest equality.
   That is exact under §4.1's stated adversary model -- both endpoints are inside the interserver
   authentication boundary, so the threat is ACCIDENTAL collision at 128 bits, not forgery.
   `pool_uuid` and `ref_name` are omitted because they are constant across both mounts here (one
   pool, one table, one ref); they discriminate nothing this model can vary.  The sabotages remove
   fields from BOTH sides, because both sides run the same code -- an implementation that "forgot
   disk_name" forgets it symmetrically, and that symmetry is what makes the collision possible. *)
Validator(m, b) ==
    IF SabotageBareValidator   THEN [ref |-> b]
    ELSE IF SabotageNoDiskQual THEN [ns |-> NsOf(m), ref |-> b]
    ELSE                            [ns |-> NsOf(m), disk |-> m, ref |-> b]

HeldValidator == Validator(MountA, Token)   \* what the receiver adopted from the offer, at T0

(* §4.2's ordering, evaluated in this order.  Rule 1 (fence) is FIRST, which is the reorder. *)
GateRefuses(m) ==
    \/ ~(SabotageNoFence \/ sFence[m])                              \* 1. mount fence -- HOISTED
    \/ ~sRecovered[m]                                               \* 2. residency and recovery
    \/ ~(SabotageNoWedge \/ ~sWedge[m])                             \* 3. wedge
    \/ ~(sApply[m] = "clean" \/ (SabotageNoPoison /\ sApply[m] = "poisoned"))   \* 4. apply state
    \/ ~(SabotageStaleCache \/ (~sPending[m] /\ ~sLeader[m]))       \* v11 rule 3 -- TRUE = v12

Answer(m) ==
    IF GateRefuses(m) THEN "unknown"
    ELSE IF sCacheRef[m] = NoBinding THEN "unknown"       \* no binding => no validator (§4.2 row 3)
    ELSE IF SabotageNoRowExact THEN "proven"              \* rule 5 degenerates to presence
    ELSE IF Validator(m, sCacheRef[m]) = HeldValidator THEN "proven"
    ELSE "changed"
```

- [ ] **Step 4: Write the receiver, GC, `Next`, and the Task-1 invariants and witnesses.**

Append. Task 1 needs only the minimum receiver that can reach `committed`; Task 3 adds S0/S2 and the byte-fetch arms:

```tla
(* ---- the receiver (§6; S0 and S2 arrive in Task 3) ----------------------------------------- *)

(* T1: stage and `precommitAdd`.  On return the +1 is DURABLE, over the blobs of the manifest the
   receiver ADOPTED -- mount A's. *)
RStage(r) ==
    /\ \/ rState[r] = "init"
       \/ (SabotagePublishAfterConfirm /\ rState[r] = "S3")
    /\ ~(\E rec \in journal : rec.ns = r /\ rec.op = "add")
    /\ nextId <= MaxId
    /\ journal' = journal \cup
         { [id |-> nextId, ns |-> r, blob |-> BlobA, src |-> r, op |-> "add"] }
    /\ nextId' = nextId + 1
    /\ rState' = [rState EXCEPT ![r] = IF rState[r] = "init" THEN "S1" ELSE "S3"]
    /\ UNCHANGED << rAnswer, rDurableBefore, rAnsweredFenced, rPublishes, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, histVars, partMount >>

(* T2: the re-offer, answered by whichever mount holds the part NOW (§3: the request carries `part`
   and `endpoint`, so the sender routes it exactly as the offer did -- a `MOVE ... TO DISK` between
   offer and confirm is answered by a mount that never made the offer). *)
RConfirm(r) ==
    /\ \/ rState[r] = "S1"
       \/ (SabotagePublishAfterConfirm /\ rState[r] = "init")
    /\ LET m == partMount
           ans == Answer(m) IN
       /\ rAnswer' = [rAnswer EXCEPT ![r] = ans]
       /\ rAnsweredFenced' = [rAnsweredFenced EXCEPT ![r] = sFence[m]]
       /\ sawBusyProven' = sawBusyProven \/ H(ans = "proven" /\ (sPending[m] \/ sLeader[m]))
       /\ sawUnknown'    = sawUnknown    \/ H(ans = "unknown")
       /\ sawCollision'  = sawCollision  \/
            H(ans = "proven" /\ m # MountA /\ sCacheRef[m] = Token /\ NsOf(m) = NsOf(MountA))
    /\ rDurableBefore' = [rDurableBefore EXCEPT ![r] = (rState[r] = "S1")]
    /\ rState' = [rState EXCEPT ![r] = "answered"]
    /\ UNCHANGED << rPublishes, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, partMount >>
    /\ UNCHANGED << sawMidTenure, sawProvenCommitted, sawChangedThenBytes, sawColdRefused >>

RPromoteCommit(r) ==
    /\ rState[r] = "answered" /\ rAnswer[r] = "proven"
    /\ rState' = [rState EXCEPT ![r] = "S3"]
    /\ rPublishes' = [rPublishes EXCEPT ![r] = rPublishes[r] + 1]
    /\ sawProvenCommitted' = sawProvenCommitted \/ H(rDurableBefore[r])
    /\ UNCHANGED << rAnswer, rDurableBefore, rAnsweredFenced, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, partMount >>
    /\ UNCHANGED << sawBusyProven, sawMidTenure, sawChangedThenBytes, sawUnknown,
                    sawColdRefused, sawCollision >>

(* Anything but `proven` aborts, and the abort RELEASES the receiver's protection (a durable -1).
   Task 3 splits this into the `changed` byte-fetch arm and the `unknown` retry-later arm. *)
RAbort(r) ==
    /\ rState[r] = "answered" /\ rAnswer[r] # "proven"
    /\ rState' = [rState EXCEPT ![r] = "done_retry"]
    /\ IF \E rec \in journal : rec.ns = r /\ rec.op = "add"
         THEN /\ nextId <= MaxId
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> r, blob |-> BlobA, src |-> r, op |-> "del"] }
              /\ nextId' = nextId + 1
         ELSE UNCHANGED logVars
    /\ UNCHANGED << rAnswer, rDurableBefore, rAnsweredFenced, rPublishes, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, histVars, partMount >>

(* ---- GC: fold, then three-phase graduation with SPARING on positive in-degree ---------------- *)

ApplyOne(F, rec) ==
    IF rec.op = "add" THEN F \cup { [b |-> rec.blob, src |-> rec.src] }
                      ELSE F \ { [b |-> rec.blob, src |-> rec.src] }
RECURSIVE ApplyOrdered(_, _)
ApplyOrdered(F, S) ==
    IF S = {} THEN F
    ELSE LET m == CHOOSE x \in S : \A y \in S : x.id <= y.id
         IN ApplyOrdered(ApplyOne(F, m), S \ {m})

Avail == { rec \in journal : rec.id > cursor[rec.ns] }

(* CommittedEdgesAreGcVisible in model form: the fold observes ALL of `Avail`.  There is no
   `MaxHoles` dial here -- §12.1 reassigned listing completeness to the v9 chain models. *)
GFold ==
    /\ gcPhase = "idle" /\ round < MaxRound
    /\ folded' = ApplyOrdered(folded, Avail)
    /\ cursor' = [ ns \in Namespaces |->
                     LET seen == { rec.id : rec \in { x \in Avail : x.ns = ns } }
                     IN IF seen = {} THEN cursor[ns] ELSE Max(seen) ]
    /\ gcPhase' = "folded"
    /\ UNCHANGED << round, present, condemned, pendingDelete >>
    /\ UNCHANGED << senderVars, recvVars, logVars, histVars, partMount >>

GSettle ==
    /\ gcPhase = "folded"
    /\ LET live  == { b \in Blobs : Indeg(b) > 0 }
           kills == { b \in pendingDelete : present[b] /\ Indeg(b) = 0 }
           grads == { b \in (condemned \ pendingDelete) : present[b] /\ Indeg(b) = 0 }
           newly == { b \in Blobs : present[b] /\ Indeg(b) = 0 /\ b \notin condemned }
       IN /\ present'       = [ b \in Blobs |-> IF b \in kills THEN FALSE ELSE present[b] ]
          /\ condemned'     = ((condemned \ live) \ kills) \cup newly
          /\ pendingDelete' = ((pendingDelete \ live) \ kills) \cup grads
    /\ round' = round + 1
    /\ gcPhase' = "idle"
    /\ UNCHANGED << folded, cursor >>
    /\ UNCHANGED << senderVars, recvVars, logVars, histVars, partMount >>

NoOp == UNCHANGED vars     \* house pattern; every cfg also sets CHECK_DEADLOCK FALSE

Next ==
    \/ \E m \in Mounts :
         \/ \E nb \in {Other, NoBinding} : SenderAdmit(m, nb)
         \/ SenderArm(m) \/ SenderDurable(m) \/ SenderInstall(m) \/ SenderCloseTenure(m)
         \/ SenderPoison(m) \/ SenderWedgeLanded(m) \/ SenderWedgeNotLanded(m)
         \/ FenceLoss(m) \/ ForeignRemove(m) \/ EvictTable(m) \/ RecoverForAnswer(m)
    \/ \E r \in Receivers : RStage(r) \/ RConfirm(r) \/ RPromoteCommit(r) \/ RAbort(r)
    \/ GFold \/ GSettle
    \/ NoOp

Spec == Init /\ [][Next]_vars

(* ---- invariants ----------------------------------------------------------------------------- *)

TypeOK ==
    /\ round \in Rounds
    /\ present \in [Blobs -> BOOLEAN]
    /\ pendingDelete \subseteq condemned /\ condemned \subseteq Blobs
    /\ \A e \in folded : e.b \in Blobs /\ e.src \in Sources
    /\ cursor \in [Namespaces -> Ids]
    /\ gcPhase \in {"idle", "folded"}
    /\ journal \subseteq Records
    /\ nextId \in 1..(MaxId + 1)
    /\ sDurableRef \in [Mounts -> Bindings] /\ sCacheRef \in [Mounts -> Bindings]
    /\ sTarget \in [Mounts -> Bindings]
    /\ sApply \in [Mounts -> {"clean", "pending", "poisoned"}]
    /\ sChunks \in [Mounts -> 0..MaxChunks]
    /\ partMount \in Mounts
    /\ rAnswer \in [Receivers -> {"none", "proven", "changed", "unknown"}]
    /\ rPublishes \in [Receivers -> 0..2]

LiveBlobs == { b \in Blobs : present[b] }

(* THE THEOREM.  A relink promoted on a `proven` whose activation (+1) was durable BEFORE the
   confirm never references a blob that has been physically deleted. *)
ConfirmedRelinkNeverDangles ==
    \A r \in Receivers :
        (rState[r] = "S3" /\ rAnswer[r] = "proven" /\ rDurableBefore[r])
            => AdoptedBlobs \subseteq LiveBlobs

(* The antecedent-free form: broken by inverting the order, which leaves the guarded theorem
   vacuously satisfied. *)
PromotedNeverDangles ==
    \A r \in Receivers : rState[r] = "S3" => AdoptedBlobs \subseteq LiveBlobs

(* §5.1.2's REQUIREMENT on the seam fix, stated as a model property: a mount's own durable write is
   never invisible to a reader while the marker says Clean.  Scoped to a FENCED, UNWEDGED mount --
   `ForeignRemove` moves a deposed mount's durable binding with no arm of its own (rule 1 covers
   it), and the wedge path clears the marker by design (rule 3 covers it). *)
MarkerCoversDurableWindow ==
    \A m \in Mounts :
        (sFence[m] /\ ~sWedge[m] /\ sDurableRef[m] # sCacheRef[m]) => (sApply[m] # "clean")

(* ---- witnesses (negated reachability; a TLC violation means the state IS reachable) --------- *)

(* THE FLIP'S NON-VACUITY, and the model-level statement of §2's availability fix: a `proven` is
   actually given while the lane is BUSY -- the exact state v11 rule 3 refused.  Without this,
   `_sab_stalecache` GREEN could be green because the state is unreachable. *)
W_BusyLaneProven == ~sawBusyProven

(* §12.5 ii: a SECOND durable chunk of the SAME tenure commits, so the flip is not an artefact of
   one transaction per tenure. *)
W_MidTenureCommit == ~sawMidTenure

(* NON-VACUITY OF THE THEOREM: its antecedent (S3 + proven + activation durable first). *)
W_ProvenCommitted == ~sawProvenCommitted

(* NON-VACUITY OF THE CONSEQUENT: GC really does physically delete. *)
W_BlobDeleted == \A b \in Blobs : present[b]

=============================================================================
```

- [ ] **Step 5: Write the runner with expected verdicts and markers.**

Create `docs/superpowers/models/run_relinkreoffer.sh`, `chmod +x`. Copy the structure of `run_refcatalog.sh` exactly — same result extraction, same `violation:<NAME>` assertion, same `ALL EXPECTATIONS MET` / exit code — with these differences: `MODULE=CaRelinkReofferCore`, metadir prefix `tlc-meta-relinkreoffer-`, and the marker lines. The invocation block must be:

```bash
  rm -rf "$meta"
  { echo "=== TLC BEGIN ${MODULE}_${name} $(date -Is) ==="; } > "$log"
  start=$SECONDS
  /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
    -metadir "$meta" -workers "${TLC_WORKERS:-1}" -config "$cfg" "$MODULE.tla" >>"$log" 2>&1
  rc=$?
  elapsed=$((SECONDS - start))
  { echo "=== TLC END ${MODULE}_${name} rc=${rc} $(date -Is) ==="; } >> "$log"
```

Its header comment lists every config with its expectation and the one-line reason, in the `run_refcatalog.sh` style. Its `CONFIGS` array for THIS task is exactly:

```bash
CONFIGS=(
  "sab_noapplypending        violation  ConfirmedRelinkNeverDangles"
  "sab_noapplypending_window violation  MarkerCoversDurableWindow"
  "v11_baseline              green      -"
  "ctl_v11nomarker           green      -"
  "sab_stalecache            green      -"
  "witness_busylane          violation  W_BusyLaneProven"
  "witness_midtenure         violation  W_MidTenureCommit"
  "witness_proven            violation  W_ProvenCommitted"
  "witness_delete            violation  W_BlobDeleted"
)
```

Tasks 2 and 3 extend this array; the sabotage-before-green ordering is preserved as they do.

- [ ] **Step 6: Write the two failing-first configs.**

`CaRelinkReofferCore_sab_noapplypending.cfg` — the header states the sabotage intent in one sentence, then:

```
\* SABOTAGE (§12.3 step 2, first half): the apply-pending marker is NEVER ARMED, while the design's
\* deletion of v11 rule 3's lane-quiescence terms stands.  TLC MUST report a
\* ConfirmedRelinkNeverDangles counterexample: the confirm reads a committed row that lags a DURABLE
\* removal, exactly the v11 _sab_stalecache trace, and this is the guard doing real work.
SPECIFICATION Spec
CONSTANTS
    Receivers = {r1}
    MaxId = 6
    MaxRound = 5
    MaxChunks = 2
    SecondMount = FALSE
    EqualNamespaces = TRUE
    ModelColdTable = FALSE
    TrackHistory = FALSE
    SabotageStaleCache = TRUE
    SabotageNoApplyPending = TRUE
    SabotageNoPoison = FALSE
    SabotageNoWedge = FALSE
    SabotageNoFence = FALSE
    SabotageNoRowExact = FALSE
    SabotageBareValidator = FALSE
    SabotageNoDiskQual = FALSE
    SabotagePublishAfterConfirm = FALSE
    SabotageS2ByteFetch = FALSE
INVARIANTS
    ConfirmedRelinkNeverDangles
CHECK_DEADLOCK FALSE
```

`CaRelinkReofferCore_sab_noapplypending_window.cfg` — identical constants, and:

```
\* SABOTAGE, SECOND CONSEQUENCE of the same toggle: §5.1.2 states what this design REQUIRES of the
\* seam's marker fix -- "a reader holding `state_mutex` must not be able to observe `Clean` while a
\* transaction of that table is between its arm and its install".  With the marker unarmed that
\* requirement is false, so TLC MUST report a MarkerCoversDurableWindow counterexample.  This is the
\* direct statement of the requirement; the sibling cfg is its safety consequence.
INVARIANTS
    MarkerCoversDurableWindow
```

- [ ] **Step 7: Run the two sabotages FIRST. Both MUST be red.**

Run:
```bash
bash /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models/run_relinkreoffer.sh
```
Expected at this point: `sab_noapplypending` → `violation:ConfirmedRelinkNeverDangles` PASS; `sab_noapplypending_window` → `violation:MarkerCoversDurableWindow` PASS; the seven not-yet-written configs error out (`error`) — that is expected and is why they are written next.

If either sabotage comes back GREEN, the model does not represent the hazard and the rest of the task is meaningless: the arm/durable ordering or the gate's rule-4 term is wrong. Fix the MODEL (never the invariant) and re-run. Copy the counterexample trace of `sab_noapplypending` out of `tmp/tlc_CaRelinkReofferCore_sab_noapplypending.log` into the scratch note — it is quoted in RESULTS.

- [ ] **Step 8: Write the three 2×2 configs and run THE GATE.**

Three configs, same constants as step 6 except the two cells below, and the invariant lists shown:

| cfg | `SabotageStaleCache` | `SabotageNoApplyPending` | `INVARIANTS` | expected |
| --- | --- | --- | --- | --- |
| `v11_baseline` | `FALSE` | `FALSE` | `TypeOK` `ConfirmedRelinkNeverDangles` `PromotedNeverDangles` `MarkerCoversDurableWindow` | green |
| `ctl_v11nomarker` | `FALSE` | `TRUE` | `TypeOK` `ConfirmedRelinkNeverDangles` `PromotedNeverDangles` | green |
| `sab_stalecache` | `TRUE` | `FALSE` | `TypeOK` `ConfirmedRelinkNeverDangles` `PromotedNeverDangles` `MarkerCoversDurableWindow` | **green — THE FLIP** |

`ctl_v11nomarker` deliberately omits `MarkerCoversDurableWindow`: with the marker unarmed that invariant is FALSE by construction, and step 7 already proved it. Omitting it here is scoping, not weakening — record the sentence in the cfg header.

Each cfg's header states its cell of the matrix. `_sab_stalecache`'s header must carry:

```
\* *** THE GATE (spec §12.3 step 3). ***  v11 rule 3's lane-quiescence terms are DROPPED -- which is
\* what this design ships -- and the apply-pending marker is INTACT.  In `CaRelinkConfirmCore` the
\* same-named cfg MUST be RED; here it MUST be GREEN, and that flip is the proof that the terms this
\* design deletes were redundant rather than load-bearing.  If TLC reports a violation, the design is
\* wrong and MUST NOT BE IMPLEMENTED.
```

Run the harness again. Expected: three greens.

**THE VERDICT.** If `sab_stalecache` is GREEN → the gate's first half is met; continue. If it is RED → **STOP**. Do not start Task 2. Preserve `tmp/tlc_CaRelinkReofferCore_sab_stalecache.log`, jump to Task 5, write the RESULTS file with `RELINK TLA GATE: FAIL`, name the violated invariant and quote the counterexample, and report the design as refuted per §1 gate 2 and §12.3 step 3.

- [ ] **Step 9: Write the four witnesses and prove the flip is not vacuous.**

Four configs. All four use the DESIGN's settings — `SabotageStaleCache = TRUE`, `SabotageNoApplyPending = FALSE` — because a witness run under different settings proves nothing about the config it is meant to de-vacuum. All four set `TrackHistory = TRUE`. Each carries exactly one negated invariant:

| cfg | `INVARIANT` | what its violation proves |
| --- | --- | --- |
| `witness_busylane` | `W_BusyLaneProven` | a `proven` answer is actually given while `sPending \/ sLeader` — the availability state v11 refused is reachable and answerable. **This is the one that makes `_sab_stalecache` GREEN mean something.** |
| `witness_midtenure` | `W_MidTenureCommit` | a second durable chunk of the SAME tenure commits (§12.5 ii) |
| `witness_proven` | `W_ProvenCommitted` | the theorem's antecedent is reachable |
| `witness_delete` | `W_BlobDeleted` | GC physically deletes, so the consequent is not trivially true |

Run the harness. Expected: nine rows, `ALL EXPECTATIONS MET`.

If `witness_busylane` comes back GREEN, `_sab_stalecache`'s green is vacuous — the busy-lane `proven` is unreachable — and the gate is NOT met. Treat that exactly as a RED `_sab_stalecache`: stop and report. Likewise `witness_midtenure` GREEN means `MaxChunks`/the tenure encoding is wrong and the flip is the one-transaction artefact §12.5 ii warns about; fix the model and re-run steps 7–9 in order.

- [ ] **Step 10: Commit.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add docs/superpowers/models/CaRelinkReofferCore.tla \
        docs/superpowers/models/CaRelinkReofferCore_sab_noapplypending.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_noapplypending_window.cfg \
        docs/superpowers/models/CaRelinkReofferCore_v11_baseline.cfg \
        docs/superpowers/models/CaRelinkReofferCore_ctl_v11nomarker.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_stalecache.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_busylane.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_midtenure.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_proven.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_delete.cfg \
        docs/superpowers/models/run_relinkreoffer.sh
git commit -m "ca: tla — CaRelinkReofferCore: the apply-pending refinement and the 2x2 gate (_sab_stalecache FLIPS GREEN)"
```

---

### Task 2: The cross-mount collision — equal namespace, different disk

§12.5 row i, verbatim: *"MODEL IT, and specifically: two mounts with EQUAL `root_namespace` and DIFFERENT `disk_name` ... Without that shape a model can pass while qualifying by namespace only."*

**Files:**
- Modify: `docs/superpowers/models/CaRelinkReofferCore.tla` (add `PartMove`, the collision witness, wire `partMount` into `Next`)
- Create: `docs/superpowers/models/CaRelinkReofferCore_sab_barevalidator.cfg`
- Create: `docs/superpowers/models/CaRelinkReofferCore_sab_nodiskqualification.cfg`
- Create: `docs/superpowers/models/CaRelinkReofferCore_ctl_distinctns.cfg`
- Create: `docs/superpowers/models/CaRelinkReofferCore_witness_collisionreached.cfg`
- Modify: `docs/superpowers/models/run_relinkreoffer.sh` (extend `CONFIGS`)
- Read first: spec §3, §4.1 (the validator paragraph), §11 row 17, §12.5 row i.

**Interfaces:**
- Consumes: Task 1's `Validator(m, b)`, `HeldValidator`, `NsOf(m)`, `BlobOf(m)`, `partMount`, `sawCollision`, `SecondMount`, `EqualNamespaces`, `SabotageBareValidator`, `SabotageNoDiskQual`.
- Produces: `W_CollisionReached`; the two reds §12.5 row i names by name.

---

- [ ] **Step 1: Add the part move and the collision witness to the module.**

Insert after `RecoverForAnswer`:

```tla
(* `MOVE ... TO DISK` between offer and confirm.  The offer was answered by mount A; after this the
   confirm routes to mount B -- "a part that moves between same-pool disks can be answered for by a
   mount that never made the offer" (§3).  This is test row 17's driver. *)
PartMove ==
    /\ SecondMount /\ partMount = MountA
    /\ partMount' = MountB
    /\ UNCHANGED << gcVars, senderVars, recvVars, logVars, histVars >>
```

Add `\/ PartMove` to `Next`. Add the witness beside the others:

```tla
(* §12.5 i's non-vacuity: the collision STATE is reachable -- a `proven` emitted by a mount that is
   NOT the offering mount, holding the SAME ManifestRef text, under an EQUAL namespace string.
   Without this, `_sab_barevalidator` and `_sab_nodiskqualification` could be red for some other
   reason and `_ctl_distinctns` could be green for free. *)
W_CollisionReached == ~sawCollision
```

Note that `sawCollision` is already set in `RConfirm` (Task 1 step 4) — no change there.

- [ ] **Step 2: Write the two must-red configs.**

Both use `SecondMount = TRUE`, `EqualNamespaces = TRUE`, `SabotageStaleCache = TRUE` (the design), `SabotageNoApplyPending = FALSE`, `TrackHistory = FALSE`, `ModelColdTable = FALSE`, `Receivers = {r1}`, `MaxId = 6`, `MaxRound = 5`, `MaxChunks = 2`, every other sabotage `FALSE`, `INVARIANTS ConfirmedRelinkNeverDangles`, `CHECK_DEADLOCK FALSE`.

`sab_barevalidator` sets `SabotageBareValidator = TRUE`; its header:

```
\* SABOTAGE (§12.5 i): the validator is the bare ManifestRef, with no qualification at all -- the
\* B1 collision shape.  `CasTypes.h:131-134`: "Two namespaces may legally carry the same ManifestRef
\* tuple without addressing the same object."  TLC MUST report a ConfirmedRelinkNeverDangles
\* counterexample: mount A's binding is removed and its blob deleted; the part MOVES to mount B,
\* which still binds the same ManifestRef TEXT over a DIFFERENT object; the bare comparison matches
\* and the receiver promotes over blobs nobody is protecting.
```

`sab_nodiskqualification` sets `SabotageNoDiskQual = TRUE`; its header:

```
\* SABOTAGE (§12.5 i) -- *** THE ONE THAT PINS B1'S ACTUAL FIX. ***  The validator KEEPS the
\* namespace and DROPS `disk_name`.  §3: a namespace is `<server_root_id>/store/<u3>/<uuid>@cas@`,
\* so two content-addressed disks with the same `server_root_id`, in one pool, serving one table
\* produce the IDENTICAL namespace string -- which is why `resolveContentAddressedConfirm` demands
\* exactly one matching mount today.  With EqualNamespaces = TRUE the namespace discriminates
\* NOTHING, so TLC MUST report a ConfirmedRelinkNeverDangles counterexample.  A model without this
\* shape passes while qualifying by namespace only; `_ctl_distinctns` is the control that proves the
\* shape is what makes this red.
```

- [ ] **Step 3: Run both. Both MUST be red — this is the failing-first step.**

Add the two rows to the runner's `CONFIGS` array, immediately after `sab_noapplypending_window`:

```bash
  "sab_barevalidator        violation  ConfirmedRelinkNeverDangles"
  "sab_nodiskqualification  violation  ConfirmedRelinkNeverDangles"
```

Run `bash docs/superpowers/models/run_relinkreoffer.sh`. Expected: both `violation:ConfirmedRelinkNeverDangles`.

If `sab_nodiskqualification` is GREEN, the collision is unreachable and the model is not yet expressing §12.5 row i. The likely cause, in order of probability: `PartMove` is not in `Next`; mount B's row does not hold `Token` (check `Init`'s `sDurableRef`/`sCacheRef` over `Mounts`); `MaxId` is too small for the trace (A's removal + GC + the receiver's `+1` = at least three ids). Fix the model, re-run — do not touch the invariant.

- [ ] **Step 4: Write the control that proves the equal-namespace shape is load-bearing.**

`ctl_distinctns.cfg`: identical to `sab_nodiskqualification.cfg` except `EqualNamespaces = FALSE`, and `INVARIANTS TypeOK ConfirmedRelinkNeverDangles PromotedNeverDangles`. Header:

```
\* CONTROL for _sab_nodiskqualification.  The SAME sabotage -- `disk_name` dropped from the
\* validator -- with the two mounts carrying DIFFERENT namespace strings.  Expected GREEN: the
\* namespace alone separates them, so nothing collides.  That is the whole point of §12.5 i's
\* "specifically": the red next door is caused by the EQUAL-namespace configuration, not by the
\* second mount existing.  A model that ran only this configuration would pass while the wire is
\* unsafe.
```

- [ ] **Step 5: Write the collision witness.**

`witness_collisionreached.cfg`: `SecondMount = TRUE`, `EqualNamespaces = TRUE`, `TrackHistory = TRUE`, all sabotages `FALSE` **except** `SabotageStaleCache = TRUE` and `SabotageNoDiskQual = TRUE` — the witness must run under the settings whose red it de-vacuums. `INVARIANT W_CollisionReached`.

- [ ] **Step 6: Run the full battery so far.**

Extend `CONFIGS` with `"ctl_distinctns green -"` (after the greens) and `"witness_collisionreached violation W_CollisionReached"` (with the witnesses). Run. Expected: 13 rows, `ALL EXPECTATIONS MET`.

- [ ] **Step 7: Commit.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add docs/superpowers/models/CaRelinkReofferCore.tla \
        docs/superpowers/models/CaRelinkReofferCore_sab_barevalidator.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_nodiskqualification.cfg \
        docs/superpowers/models/CaRelinkReofferCore_ctl_distinctns.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_collisionreached.cfg \
        docs/superpowers/models/run_relinkreoffer.sh
git commit -m "ca: tla — cross-mount collision: equal-namespace/different-disk pair, _sab_barevalidator and _sab_nodiskqualification red, _ctl_distinctns green"
```

---

### Task 3: Re-derivation against the new rule set, and the four-state receiver

§12.4: the fence moves first, `No` becomes an authoritative outcome with its own successor action, the model must show the byte fetch following a `No` cannot publish twice, and the `_witness_confirmno` / `_witness_confirmunknown` witnesses must be re-derived because the states they prove reachable are no longer the same states.

**Files:**
- Modify: `docs/superpowers/models/CaRelinkReofferCore.tla` (split `RAbort`; add S0/S2; add `NeverPublishedTwice`, `ChangedImpliesFenced`, three witnesses)
- Create: `..._sab_nofence.cfg`, `..._sab_nofence_changed.cfg`, `..._sab_nopoison.cfg`, `..._sab_nowedge.cfg`, `..._sab_norowexact.cfg`, `..._sab_publishafterconfirm.cfg`, `..._sab_s2bytefetch.cfg`, `..._v12_design_full.cfg`, `..._v12_coldanswer.cfg`, `..._witness_changed.cfg`, `..._witness_unknown.cfg`, `..._witness_budgetunknown.cfg`
- Modify: `docs/superpowers/models/run_relinkreoffer.sh`
- Read first: spec §4.2, §4.4, §6.1–§6.4, §12.4.

**Interfaces:**
- Consumes: everything from Tasks 1–2.
- Produces: `NeverPublishedTwice`, `ChangedImpliesFenced`, `W_ChangedThenBytes`, `W_UnknownRefusal`, `W_ColdRefused`; and `_v12_design_full`, the single green in which the whole universe (two mounts, marker, chunked tenure, four-state receiver) is checked together.

---

- [ ] **Step 1: Replace `RAbort` with the three answer-specific successors, and add S0/S2.**

Delete `RAbort` and insert:

```tla
(* §4.4: `changed` with a present, different identity => abort the prepared relink, then FETCH THE
   BYTES FROM THE SAME SENDER.  Today that is forbidden; under fence-first there is no doubt left
   to protect against, because rule 1 established the answering mount held its fence. *)
RChangedThenBytes(r) ==
    /\ rState[r] = "answered" /\ rAnswer[r] = "changed"
    /\ rState' = [rState EXCEPT ![r] = "done_bytes"]
    /\ rPublishes' = [rPublishes EXCEPT ![r] = rPublishes[r] + 1]
    /\ sawChangedThenBytes' = sawChangedThenBytes \/ H(TRUE)
    /\ IF \E rec \in journal : rec.ns = r /\ rec.op = "add"
         THEN /\ nextId <= MaxId
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> r, blob |-> BlobA, src |-> r, op |-> "del"] }
              /\ nextId' = nextId + 1
         ELSE UNCHANGED logVars
    /\ UNCHANGED << rAnswer, rDurableBefore, rAnsweredFenced, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, partMount >>
    /\ UNCHANGED << sawBusyProven, sawMidTenure, sawProvenCommitted, sawUnknown,
                    sawColdRefused, sawCollision >>

(* §4.4 and §6.2: everything else is one outcome -- abort, then throw the retry-later NETWORK_ERROR.
   NO byte re-request. *)
RUnknownThenRetry(r) ==
    /\ rState[r] = "answered" /\ rAnswer[r] = "unknown"
    /\ rState' = [rState EXCEPT ![r] = "done_retry"]
    /\ IF \E rec \in journal : rec.ns = r /\ rec.op = "add"
         THEN /\ nextId <= MaxId
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> r, blob |-> BlobA, src |-> r, op |-> "del"] }
              /\ nextId' = nextId + 1
         ELSE UNCHANGED logVars
    /\ UNCHANGED << rAnswer, rDurableBefore, rAnsweredFenced, rPublishes, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, histVars, partMount >>

(* §6.2: `promote` has THREE outcomes.  `MechanismFallbackAllowed` was rejected BEFORE its ref-log
   append, so "nothing was committed" is PROVEN -- state S0, and its action is a byte fetch. *)
RPromoteFallback(r) ==
    /\ rState[r] = "answered" /\ rAnswer[r] = "proven"
    /\ rState' = [rState EXCEPT ![r] = "S0"]
    /\ UNCHANGED << rAnswer, rDurableBefore, rAnsweredFenced, rPublishes, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, histVars, partMount >>

S0Bytes(r) ==
    /\ rState[r] = "S0"
    /\ rState' = [rState EXCEPT ![r] = "done_bytes"]
    /\ rPublishes' = [rPublishes EXCEPT ![r] = rPublishes[r] + 1]
    /\ UNCHANGED << rAnswer, rDurableBefore, rAnsweredFenced, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, histVars, partMount >>

(* §6.3: `Unresolved` -- the ref-log append was attempted and came back without a verdict, so the
   receiver's ref MAY be committed.  Modelled as exactly that: an ambiguity resolved LATER, either
   way, by an action the receiver does not control. *)
RPromoteUnresolved(r) ==
    /\ rState[r] = "answered" /\ rAnswer[r] = "proven"
    /\ rState' = [rState EXCEPT ![r] = "S2"]
    /\ rUnresolved' = [rUnresolved EXCEPT ![r] = TRUE]
    /\ UNCHANGED << rAnswer, rDurableBefore, rAnsweredFenced, rPublishes >>
    /\ UNCHANGED << gcVars, senderVars, logVars, histVars, partMount >>

S2ResolveLanded(r) ==       \* the append had in fact landed: the ref IS committed
    /\ rUnresolved[r]
    /\ rUnresolved' = [rUnresolved EXCEPT ![r] = FALSE]
    /\ rPublishes'  = [rPublishes  EXCEPT ![r] = rPublishes[r] + 1]
    /\ UNCHANGED << rState, rAnswer, rDurableBefore, rAnsweredFenced >>
    /\ UNCHANGED << gcVars, senderVars, logVars, histVars, partMount >>

S2ResolveNotLanded(r) ==    \* it had not: the precommit is released
    /\ rUnresolved[r]
    /\ rUnresolved' = [rUnresolved EXCEPT ![r] = FALSE]
    /\ IF \E rec \in journal : rec.ns = r /\ rec.op = "add"
         THEN /\ nextId <= MaxId
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> r, blob |-> BlobA, src |-> r, op |-> "del"] }
              /\ nextId' = nextId + 1
         ELSE UNCHANGED logVars
    /\ UNCHANGED << rState, rAnswer, rDurableBefore, rAnsweredFenced, rPublishes >>
    /\ UNCHANGED << gcVars, senderVars, histVars, partMount >>

(* §6.3's action: throw retry-later, NEVER a byte fetch.  "This is the one state where a byte fetch
   would be a defect, because it would publish the part a second time over a relink that may already
   be committed."  `SabotageS2ByteFetch` does exactly that, and must break NeverPublishedTwice --
   the necessity half of the named assumption `UnresolvedPromoteNeverBytes`. *)
S2Retry(r) ==
    /\ rState[r] = "S2"
    /\ IF SabotageS2ByteFetch
         THEN /\ rState' = [rState EXCEPT ![r] = "done_bytes"]
              /\ rPublishes' = [rPublishes EXCEPT ![r] = rPublishes[r] + 1]
         ELSE /\ rState' = [rState EXCEPT ![r] = "done_retry"]
              /\ UNCHANGED rPublishes
    /\ UNCHANGED << rAnswer, rDurableBefore, rAnsweredFenced, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, histVars, partMount >>
```

Extend the receiver disjunct of `Next` to:

```tla
    \/ \E r \in Receivers :
         \/ RStage(r) \/ RConfirm(r)
         \/ RPromoteCommit(r) \/ RPromoteFallback(r) \/ RPromoteUnresolved(r)
         \/ RChangedThenBytes(r) \/ RUnknownThenRetry(r)
         \/ S0Bytes(r) \/ S2Retry(r) \/ S2ResolveLanded(r) \/ S2ResolveNotLanded(r)
```

Add to `RConfirm`'s history conjunct: `sawColdRefused' = sawColdRefused \/ H(ans = "unknown" /\ ~sRecovered[partMount])`, and remove `sawColdRefused` from that action's `UNCHANGED` list.

- [ ] **Step 2: Add the two new invariants and the three re-derived witnesses.**

```tla
(* §12.4: "the model must show that the byte fetch following a `No` cannot publish twice".  Counted,
   not asserted structurally: a relink promote that COMMITS publishes once; an ambiguous promote
   that turns out to have landed publishes once; a byte fetch publishes once.  Two is the defect. *)
NeverPublishedTwice == \A r \in Receivers : rPublishes[r] <= 1

(* THE FENCE-FIRST PAYOFF.  An authoritative `No` is what authorizes a same-sender byte fetch, and
   only a mount that HELD ITS FENCE may emit one -- "a fenced-out mount is not this namespace's
   writer and its answer is not an answer" (§4.2).  With the fence evaluated LAST, as v11 did, a
   fenced-out instance's row comparison could speak first. *)
ChangedImpliesFenced ==
    \A r \in Receivers : (rAnswer[r] = "changed") => rAnsweredFenced[r]

(* Re-derived from v11's `_witness_confirmno`: the state it proves reachable is no longer the same
   state.  The `changed` answer fires AND its successor byte fetch runs -- the arm that did not
   exist before the reorder. *)
W_ChangedThenBytes == ~sawChangedThenBytes

(* Re-derived from v11's `_witness_confirmunknown`: `unknown` now folds a DIFFERENT rule set --
   the fence is hoisted out of it and the lane-quiescence terms are gone. *)
W_UnknownRefusal == ~sawUnknown

(* §12.4's revisit of rule 2: recovery is MANDATORY on the answer path, so the cold refusal is now
   on the path every cold answer takes rather than one nobody takes.  Its budget arm must be
   reachable. *)
W_ColdRefused == ~sawColdRefused
```

- [ ] **Step 3: Write the seven must-red configs and run them FIRST.**

Base constants for all seven: `Receivers = {r1}`, `MaxId = 6`, `MaxRound = 5`, `MaxChunks = 2`, `TrackHistory = FALSE`, `ModelColdTable = FALSE`, `SabotageStaleCache = TRUE` (the design — every retained rule is re-derived against the SHIPPED gate, not against v11's), `SabotageNoApplyPending = FALSE`, everything else `FALSE` except the row's own toggle.

| cfg | `SecondMount` | toggle | `INVARIANTS` | one-line intent for the cfg header |
| --- | --- | --- | --- | --- |
| `sab_nofence` | `FALSE` | `SabotageNoFence = TRUE` | `ConfirmedRelinkNeverDangles` | rule 1, hoisted first, still load-bearing: a deposed instance answers `proven` about a namespace somebody else now writes |
| `sab_nofence_changed` | `FALSE` | `SabotageNoFence = TRUE` | `ChangedImpliesFenced` | the SECOND consequence of the same toggle: a fenced-out mount emits an authoritative `No`, which is what authorizes a same-sender byte fetch |
| `sab_nopoison` | `FALSE` | `SabotageNoPoison = TRUE` | `ConfirmedRelinkNeverDangles` | rule 4's Poisoned arm: after a poisoned apply the lane IS quiescent and the marker is NOT pending, so only this arm sees the permanently stale row |
| `sab_nowedge` | `FALSE` | `SabotageNoWedge = TRUE` | `ConfirmedRelinkNeverDangles` | rule 3 (wedge), retained by the design: the wedge path CLEARS the marker while the write may have landed, so the marker cannot cover it |
| `sab_norowexact` | `FALSE` | `SabotageNoRowExact = TRUE` | `ConfirmedRelinkNeverDangles` | rule 5's exactness (v11's `_sab_nogate1`, re-derived): presence instead of validator equality is an ABA |
| `sab_publishafterconfirm` | `FALSE` | `SabotagePublishAfterConfirm = TRUE` | `PromotedNeverDangles` | the ORDER is inverted; the guarded theorem stays vacuously satisfied, so the antecedent-free form is what must break. §core-idea survives verbatim and this is its check |
| `sab_s2bytefetch` | `FALSE` | `SabotageS2ByteFetch = TRUE` | `NeverPublishedTwice` | §6.3: S2 is the one state where a byte fetch double-publishes. The necessity half of the named assumption `UnresolvedPromoteNeverBytes` |

Add all seven to `CONFIGS` (in the sabotage block, before the greens) and run. Expected: seven violations, each matching its named invariant. A red on a DIFFERENT invariant than the one named is a FAIL per the Global Constraints — investigate the trace, do not relabel the cfg.

- [ ] **Step 4: Write the two remaining greens.**

`v12_design_full.cfg` — **the config in which the whole universe is checked together**, so no property is only ever checked in isolation:

```
\* THE DESIGN, WHOLE.  Two mounts (equal namespace, different disk), the apply marker armed and
\* cleared, a tenure that commits several chunks, the four-state receiver, and v11 rule 3's terms
\* deleted -- every mechanism of this design switched on at once.  Every other green in this battery
\* isolates one thing; this one is the only place where they are checked TOGETHER, which is what
\* rules out a property that holds only when its neighbours are off.
SPECIFICATION Spec
CONSTANTS
    Receivers = {r1}
    MaxId = 6
    MaxRound = 5
    MaxChunks = 2
    SecondMount = TRUE
    EqualNamespaces = TRUE
    ModelColdTable = FALSE
    TrackHistory = FALSE
    SabotageStaleCache = TRUE
    SabotageNoApplyPending = FALSE
    SabotageNoPoison = FALSE
    SabotageNoWedge = FALSE
    SabotageNoFence = FALSE
    SabotageNoRowExact = FALSE
    SabotageBareValidator = FALSE
    SabotageNoDiskQual = FALSE
    SabotagePublishAfterConfirm = FALSE
    SabotageS2ByteFetch = FALSE
INVARIANTS
    TypeOK
    ConfirmedRelinkNeverDangles
    PromotedNeverDangles
    MarkerCoversDurableWindow
    NeverPublishedTwice
    ChangedImpliesFenced
CHECK_DEADLOCK FALSE
```

`v12_coldanswer.cfg` — identical except `SecondMount = FALSE` and `ModelColdTable = TRUE`, same six invariants. Header: rule 2 is now on the answer path (§4.3's mandatory recovery, §12.4's revisit); this config runs eviction and peer-initiated recovery, and green means the atomic install leaves nothing partial for the gate to read.

Run. Expected: both green. If `v12_design_full` exceeds ~10 min at `-workers 1`, drop `MaxId` to 5 first, then `MaxRound` to 4 — and record the bound actually used in RESULTS. Never drop an invariant to make it finish.

- [ ] **Step 5: Write the three re-derived witnesses.**

All three: `TrackHistory = TRUE`, `SabotageStaleCache = TRUE`, everything else honest.

| cfg | extra constants | `INVARIANT` |
| --- | --- | --- |
| `witness_changed` | `SecondMount = TRUE`, `EqualNamespaces = TRUE` | `W_ChangedThenBytes` |
| `witness_unknown` | `SecondMount = FALSE` | `W_UnknownRefusal` |
| `witness_budgetunknown` | `SecondMount = FALSE`, `ModelColdTable = TRUE` | `W_ColdRefused` |

`witness_changed` needs the second mount: with one mount and the design's mint-tightening the only way to a `changed` is a repoint, which is reachable, but the cross-mount route is the one row 17 exercises and the one that must be shown live. Each cfg header states, in one sentence, which v11 witness it re-derives and why the state is not the same one.

- [ ] **Step 6: Run the whole battery and confirm 25/25.**

Final `CONFIGS` array — sabotages first, then greens, then witnesses:

```bash
CONFIGS=(
  "sab_noapplypending        violation  ConfirmedRelinkNeverDangles"
  "sab_noapplypending_window violation  MarkerCoversDurableWindow"
  "sab_nofence               violation  ConfirmedRelinkNeverDangles"
  "sab_nofence_changed       violation  ChangedImpliesFenced"
  "sab_nopoison              violation  ConfirmedRelinkNeverDangles"
  "sab_nowedge               violation  ConfirmedRelinkNeverDangles"
  "sab_norowexact            violation  ConfirmedRelinkNeverDangles"
  "sab_barevalidator         violation  ConfirmedRelinkNeverDangles"
  "sab_nodiskqualification   violation  ConfirmedRelinkNeverDangles"
  "sab_publishafterconfirm   violation  PromotedNeverDangles"
  "sab_s2bytefetch           violation  NeverPublishedTwice"
  "v11_baseline              green      -"
  "ctl_v11nomarker           green      -"
  "sab_stalecache            green      -"
  "ctl_distinctns            green      -"
  "v12_design_full           green      -"
  "v12_coldanswer            green      -"
  "witness_busylane          violation  W_BusyLaneProven"
  "witness_midtenure         violation  W_MidTenureCommit"
  "witness_proven            violation  W_ProvenCommitted"
  "witness_delete            violation  W_BlobDeleted"
  "witness_collisionreached  violation  W_CollisionReached"
  "witness_changed           violation  W_ChangedThenBytes"
  "witness_unknown           violation  W_UnknownRefusal"
  "witness_budgetunknown     violation  W_ColdRefused"
)
```

Run `bash docs/superpowers/models/run_relinkreoffer.sh`. Expected: 25 rows, `ALL EXPECTATIONS MET`, exit 0.

- [ ] **Step 7: Commit.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add docs/superpowers/models/CaRelinkReofferCore.tla \
        docs/superpowers/models/CaRelinkReofferCore_sab_nofence.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_nofence_changed.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_nopoison.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_nowedge.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_norowexact.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_publishafterconfirm.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_s2bytefetch.cfg \
        docs/superpowers/models/CaRelinkReofferCore_v12_design_full.cfg \
        docs/superpowers/models/CaRelinkReofferCore_v12_coldanswer.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_changed.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_unknown.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_budgetunknown.cfg \
        docs/superpowers/models/run_relinkreoffer.sh
git commit -m "ca: tla — re-derivation against the new rule set: fence-first, authoritative No, four-state receiver, NeverPublishedTwice"
```

---

### Task 4: The S7 ruling, the v11 continuity note, and the clean end-to-end run

**Files:**
- Modify: `docs/superpowers/models/CaRelinkReofferCore.tla` (the S7 verdict, written into `MarkerCoversDurableWindow`'s comment block)
- Modify: `docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md` (**append only** — one new section)
- Read first: seam §6, seam §8 row S7, spec §5.1.2.
- **Read-only, must not change:** `CaRelinkConfirmCore.tla` and every `CaRelinkConfirmCore_*.cfg`.

**Interfaces:**
- Consumes: Task 1's `MarkerCoversDurableWindow` and `_sab_noapplypending_window` result.
- Produces: the S7 verdict paragraph and the v11 cross-reference that Task 5's RESULTS file quotes.

---

- [ ] **Step 1: Run the S7 expressibility experiment.**

The question this step must answer explicitly: **is seam §8 row S7 — "a reader taking `state_mutex` between the arm and the install observes `ApplyPending`, never a stale `Clean`" — expressible as an assertion in this model, or is it code-level-only?**

Do not answer it from the armchair. Run the experiment:

1. Note what is already established: `MarkerCoversDurableWindow` is checked in `v11_baseline`, `sab_stalecache`, `v12_design_full` and `v12_coldanswer` (green), and `_sab_noapplypending_window` breaks it (red). So the INTERVAL half of S7 — that the marker is set for the whole durable-but-unapplied window of a fenced, unwedged mount — is already asserted and already seen red.
2. Attempt the OBSERVATION half. Write a scratch module copy under the scratchpad directory (never beside the tracked module) that adds `SabotageRelaxedMarker`: `SenderArm` sets `sApply' = "pending"` as usual but the gate reads a value that lags — encode it as the worst case the relaxed store permits, i.e. the arm is invisible to a `state_mutex` reader.
3. Run it with the design's settings and `INVARIANTS ConfirmedRelinkNeverDangles`. Compare its counterexample, state for state, with `tmp/tlc_CaRelinkReofferCore_sab_noapplypending.log`'s.

**Decision rule, applied to what the run actually shows:**

- **If the two traces are the same state sequence** (the expected outcome — an arm that no reader can observe and an arm that never happened are the same state machine, because nothing else in the model reads `sApply`): the scratch module is DISCARDED, no `_sab_relaxedmarker` config is added, and the verdict is **S7 is code-level-only for its observation half**. Duplicate evidence is not evidence, and adding a second config that produces an identical counterexample would inflate the battery while proving nothing new.
- **If the traces differ**, keep the config: name it `_sab_relaxedmarker`, add it to the runner expecting `violation:ConfirmedRelinkNeverDangles`, and record in RESULTS exactly which state distinguishes it from `_sab_noapplypending` — because that state is then a real, separately-modelled hazard and the reason must be legible.

- [ ] **Step 2: Write the verdict into the module, at `MarkerCoversDurableWindow`.**

Whichever branch step 1 took, the module must carry the answer where the property lives — not only in RESULTS, per §5.1.2's own rule that "a gate whose justification sits in another document is a gate nobody re-checks". Prepend to `MarkerCoversDurableWindow`'s comment (this is the text for the expected branch; if step 1 took the other branch, state that instead, with the distinguishing state named):

```tla
(* *** SEAM §8 ROW S7 -- WHAT THIS ASSERTS AND WHAT IT DOES NOT. ***
   S7 is: "a reader taking `state_mutex` between the arm and the install observes `ApplyPending`,
   never a stale `Clean`."  It has two halves and this model can carry only one of them.
     * THE INTERVAL half IS asserted, and this is it: for a fenced, unwedged mount, no state exists
       in which the mount's own durable write is not yet installed while the marker reads clean.
       Seen RED by `_sab_noapplypending_window`, so it is not asserted for free.  This is exactly
       what §5.1.2 says this design REQUIRES of the seam fix.
     * THE OBSERVATION half is NOT expressible here, and the reason is a property of TLA+ rather
       than of the design: actions are atomic and state is globally visible, so there is no window
       between a write and a reader's view of it -- the very window S7 is about exists only in the
       C++ memory model, where `armApplyPending`'s relaxed `compare_exchange_strong` is called with
       no lock held (`CasRefLedger.cpp:1681`, called at `:2808`).  A `_sab_relaxedmarker` config was
       WRITTEN AND RUN, and rejected as duplicate evidence: nothing but the gate reads `sApply`, so
       an unobservable arm and an absent arm are the same state machine and produce the same
       counterexample as `_sab_noapplypending`.
   S7's observation half is therefore CODE-LEVEL-ONLY, discharged by the seam's mutual exclusion
   argument (seam §6: arm under `state_mutex`, read under `state_mutex`, lock at the CALL SITE
   because `forceWedgeForTest` at `:1396` already holds it) and by seam §8 row S7's test.  Recorded
   here so the omission is a decision rather than an oversight -- and so that a later weakening of
   the seam's lock placement breaks a STATED requirement rather than an invisible one. *)
```

- [ ] **Step 3: Append the continuity section to `CaRelinkConfirmCore_RESULTS.md`.**

Append one section, `## The v12 refinement, and why this file's `_sab_stalecache` stays RED {#v12-refinement}`, carrying exactly these four facts:

1. `CaRelinkConfirmCore.tla` and its twelve configs are UNCHANGED and will stay unchanged — §12's disposition: the model is the historical witness of the v11 protocol, and rewriting it would destroy the record that v11's rules were each load-bearing.
2. The redesign's model is `CaRelinkReofferCore.tla`, and its results live in `2026-07-29-relink-seam-tla-RESULTS.md`.
3. **The flip, stated as a side-by-side line, because it is the whole evidence:** `CaRelinkConfirmCore_sab_stalecache` = **RED** (`ConfirmedRelinkNeverDangles` violated) is the v11 record; `CaRelinkReofferCore_sab_stalecache` = **GREEN** is the v12 result. The difference between the two runs is one thing — the second model represents the apply-pending marker, armed strictly before the durable PUT and cleared atomically with the install.
4. `_sab_holeylist` keeps its meaning unchanged: it is the historical witness of BACKLOG `{#list-as-journal-dataloss-2026-07-25}`, and §12.1 reassigns dangle-freedom's listing half to the v9 chain models — which is why `CaRelinkReofferCore` has no `MaxHoles` dial and names `CommittedEdgesAreGcVisible` instead.

- [ ] **Step 4: Verify the v11 family is untouched.**

Run:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git status --short docs/superpowers/models/CaRelinkConfirmCore.tla docs/superpowers/models/CaRelinkConfirmCore_*.cfg
```
Expected: **no output at all**. Any modification listed here is a Global Constraints violation — revert it before continuing.

- [ ] **Step 5: Re-run the whole battery from a clean metadir state.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
rm -rf tmp/tlc-meta-relinkreoffer-*
bash docs/superpowers/models/run_relinkreoffer.sh | tee tmp/relinkreoffer_battery.txt
```
Expected: 25 rows, `ALL EXPECTATIONS MET`, exit 0. Keep `tmp/relinkreoffer_battery.txt` — Task 5 pastes it verbatim.

Then confirm the v11 battery still behaves as recorded, since the flip is a claim about a PAIR of runs:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models
./run_relinkconfirm.sh CaRelinkConfirmCore_sab_stalecache
./run_relinkconfirm.sh CaRelinkConfirmCore_main
```
Expected: the first reports `Invariant ConfirmedRelinkNeverDangles is violated`; the second reports `Model checking completed. No error has been found.` Record both output lines.

- [ ] **Step 6: Commit.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add docs/superpowers/models/CaRelinkReofferCore.tla \
        docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md
git commit -m "ca: tla — seam S7 ruling (interval half asserted, observation half code-level-only) + v11 continuity note"
```

---

### Task 5: The RESULTS document and the gate verdict

**Files:**
- Create: `docs/superpowers/models/2026-07-29-relink-seam-tla-RESULTS.md`
- Read first: `docs/superpowers/models/2026-07-28-v9-phase-RESULTS.md` (the front-matter, `## Gate` section shape and the reds-breakdown paragraph are the template), `docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md`.

**Interfaces:**
- Consumes: every task's logs, `tmp/relinkreoffer_battery.txt`, and Task 4's S7 verdict.
- Produces: the greppable verdict line the relink implementation plan's Task 0 checks before any C++ is written.

---

- [ ] **Step 1: Write the front matter and the gate section.**

Front matter in the house shape (`description`, `sidebar_label`, `sidebar_position: 3`, `slug: /superpowers/models/2026-07-29-relink-seam-tla-results`, `title`, `doc_type: 'reference'`).

Then, first section, the verdict — **one line, greppable, nothing else on it**:

```markdown
## Gate {#gate}

> **`RELINK TLA GATE: PASS`**
```

with `PASS` **only if** every green is green and every red is red against the invariant NAME it was required to break. Anything else is `RELINK TLA GATE: FAIL` with the failing config named on the next line. Immediately under it, the consequence, verbatim:

```markdown
**What this verdict decides.** Spec §12.3 step 3 and §1 gate 2:

> If the refined `_sab_stalecache` does not pass, the design is wrong and must not be implemented.

- **PASS** means the two terms this design deletes from `CasRefLedger::confirmExactRef` rule 3 —
  `!rt.pending.empty()` and `rt.leader_active` — are REDUNDANT with the apply-pending marker, and
  deleting them is safe **conditionally on the marker being made synchronized first**
  (`2026-07-29-cas-part-write-release-seam.md` §6). The marker fix is a PREREQUISITE, not a
  companion: `_sab_noapplypending` is red, so shipping the deletion without the marker is a
  demonstrated dangle. The order is: seam §6 lands, then the relink deletion.
- **FAIL** means **DO NOT IMPLEMENT** `2026-07-29-cas-relink-reoffer-redesign.md`. No C++ of that
  design is written, no `confirmExactRef` rule is deleted, and the spec returns to design with the
  counterexample below as the input. The seam document is unaffected either way — it is
  relink-independent and stands on its own (seam §intro).
```

- [ ] **Step 2: Write the flip section — the single most important table in the file.**

```markdown
## The flip {#the-flip}

| model | cfg | rule 3's lane-quiescence terms | apply-pending marker | verdict |
|---|---|---|---|---|
| `CaRelinkConfirmCore` (v11, unchanged) | `_sab_stalecache` | dropped | **not represented** | **RED** — `ConfirmedRelinkNeverDangles` |
| `CaRelinkReofferCore` (v12) | `_v11_baseline` | present | armed | green |
| `CaRelinkReofferCore` (v12) | `_ctl_v11nomarker` | present | **not armed** | green |
| `CaRelinkReofferCore` (v12) | **`_sab_stalecache`** | **dropped** | **armed** | **GREEN — THE FLIP** |
| `CaRelinkReofferCore` (v12) | `_sab_noapplypending` | dropped | **not armed** | **RED** — `ConfirmedRelinkNeverDangles` |
```

Under it, three paragraphs: (1) what the 2×2 establishes — each guard is individually sufficient, at least one is necessary, and the v12 substitution is real rather than coincidental; (2) why the green is not vacuous — quote `_witness_busylane`'s and `_witness_midtenure`'s violations, since without them the green could be an unreachable-state artefact or a one-transaction-per-tenure artefact (§12.5 ii); (3) the exact counterexample trace of `_sab_noapplypending`, copied out of its log, annotated action by action.

- [ ] **Step 3: Write the full battery table.**

One row per config: `cfg | expected | observed | invariant | states (gen/distinct) | depth | seconds | log path`. Twenty-five rows, taken from `tmp/relinkreoffer_battery.txt` and from each `tmp/tlc_CaRelinkReofferCore_*.log` — real TLC numbers, never estimates. Paste the runner's own table verbatim in a fenced block beneath it, including its `ALL EXPECTATIONS MET` line, and state the bounds actually used (`MaxId`, `MaxRound`, `MaxChunks`) and any config where they had to be shrunk.

Follow the v9 phase file's honesty convention and break the reds down by class rather than lumping them: sabotage-class (a load-bearing rule removed), reachability witnesses (where the violation IS the evidence), and controls.

- [ ] **Step 4: Write the four obligation sections, one per §12 clause.**

- **§12.3 — the required refinement.** The 2×2, cross-referenced to §the-flip.
- **§12.5 i — cross-mount collision.** `_sab_barevalidator` RED, `_sab_nodiskqualification` RED, `_ctl_distinctns` GREEN, `_witness_collisionreached` reachable. State the control's meaning in one sentence: the red next door is caused by the EQUAL-namespace configuration, which is what §12.5 i's "specifically" demands, and a model that ran only distinct namespaces would pass while the wire is unsafe.
- **§12.5 ii — chunk-boundary tenure.** `MaxChunks = 2`, `_witness_midtenure` reachable, and the sentence that matters: without it, `_sab_stalecache`'s green would be an artefact of the model having only one transaction per tenure.
- **§12.4 — re-derivation.** Fence hoisted first (`ChangedImpliesFenced`, `_sab_nofence_changed` RED); `No` given its own successor (`RChangedThenBytes`) and shown unable to publish twice (`NeverPublishedTwice`, `_sab_s2bytefetch` RED); the two v11 witnesses re-derived (`_witness_changed`, `_witness_unknown`) with one line on why the state each proves reachable is no longer the same state; rule 2 revisited (`_v12_coldanswer` green, `_witness_budgetunknown` reachable).

- [ ] **Step 5: Write the assumptions and scope sections.**

- **The three named assumptions**, each with its discharge mechanism and the sentence that makes weakening it visible: `CommittedEdgesAreGcVisible` (v9 chain models — name the exact configs), `UnresolvedPromoteNeverBytes` (spec §11 row 9, with `_sab_s2bytefetch` as the in-model necessity half), `FreshCertifiedResponse` (spec §11 rows 15a, 15b, 16). State plainly that if any of those test rows is weakened, the assumption goes with it — which is the point of naming them.
- **Seam §8 row S7**, with Task 4's verdict: the interval half is asserted (`MarkerCoversDurableWindow`, red under `_sab_noapplypending_window`); the observation half is code-level-only, with the reason and the record that a `_sab_relaxedmarker` config was written, run and rejected as duplicate evidence.
- **What the seam contributes and what it does not:** §3's emission point and §4's `attempted` mark are accounting with no safety content to gate (seam §3.3, §9 point 5), discharged by seam §8 rows S1–S6c and relink rows 19–20.
- **What this model dropped from v11 and why:** the `MaxHoles` / `NsNoise` holey-list machinery (§12.1 reassignment) — recorded as a deliberate deletion with `_sab_holeylist` named as where the finding still lives.

- [ ] **Step 6: Verify the verdict line is greppable, then commit.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
grep -n "RELINK TLA GATE:" docs/superpowers/models/2026-07-29-relink-seam-tla-RESULTS.md
```
Expected: exactly one match, reading `> **\`RELINK TLA GATE: PASS\`**` (or `FAIL`). More than one match means the string appears in prose too — rephrase the prose, keep the verdict line unique.

```bash
git add docs/superpowers/models/2026-07-29-relink-seam-tla-RESULTS.md
git commit -m "ca: tla — relink/seam gate RESULTS: RELINK TLA GATE verdict + the do-not-implement consequence"
```

---

## Self-review notes (done at write time)

**Spec coverage.** §12.1 (listing reassigned — no `MaxHoles` dial, `CommittedEdgesAreGcVisible` named instead, Task 1 step 2 + Task 5 step 5). §12.2 (`CommittedEdgesAreGcVisible` named with discharge, Task 1 step 2). §12.3 (the 2×2, Task 1 steps 6–9 — the gate). §12.4 (fence-first, authoritative `No`, no double publish, witnesses re-derived, rule 2 revisited — Task 3). §12.5 i (equal-namespace/different-disk mounts, both reds, plus a control the spec did not ask for and that the "specifically" clause implies — Task 2). §12.5 ii (chunked tenure + its witness — Task 1 steps 3, 9). §12.5 iii (`UnresolvedPromoteNeverBytes` named, with `_sab_s2bytefetch` as the necessity half — Task 3). §12's `FreshCertifiedResponse` (named, Task 1 step 2). Seam §8 S7 (Task 4 steps 1–2). Seam §3/§4 (explicitly out of scope with the reason, Task 1 step 2 + Task 5 step 5). §12's disposition ruling that `CaRelinkConfirmCore` is not edited in place (Global Constraints + Task 4 steps 3–4).

**Two places where this plan goes beyond the spec, deliberately, and says so.** (1) `_ctl_v11nomarker` and `_ctl_distinctns` — controls the spec does not name, added because a flip with no control is a coincidence and §12.5 i's "Without that shape a model can pass" implies the shape must be shown to be what bites. (2) `_sab_s2bytefetch` — the spec ASSUMES `UnresolvedPromoteNeverBytes` rather than modelling it; this plan keeps the assumption exactly as assumed and models only its CONSEQUENCE, which is what makes the assumption's weight visible instead of merely stated.

**Placeholder scan.** Every cfg's full constants block is given once (Task 1 step 6) and every later cfg is specified as a delta against it with the exact toggles and the exact `INVARIANTS` list. Every module fragment is real TLA+, not a sketch. The one step whose outcome is not known in advance (Task 4 step 1) carries a decision rule with both branches spelled out, not a TODO. The one place the plan says "state it in one sentence" is a cfg header comment, where the content is fully determined by the table row beside it.

**Type consistency.** `sApply` is three-valued (`clean`/`pending`/`poisoned`) everywhere, matching `RefApplyState`. `rState` values `init`/`S1`/`answered`/`S0`/`S2`/`S3`/`done_bytes`/`done_retry` are used identically in Tasks 1 and 3 — `RStage` writes `S1`, `RConfirm` writes `answered`, and both `ConfirmedRelinkNeverDangles` and `PromotedNeverDangles` test `S3`. `rAnswer` values are `none`/`proven`/`changed`/`unknown` — the wire vocabulary of §4.2, not v11's `yes`/`no`/`unknown`; `TypeOK` and every witness use those spellings. `SabotageNoApplyPending` acts in `SenderArm` only and never appears in `GateRefuses` — that separation is what makes `_sab_noapplypending_window` a distinct consequence rather than a duplicate. `MarkerCoversDurableWindow`'s `~sWedge` and `sFence` guards are matched by `SenderWedgeLanded` clearing the marker and by `ForeignRemove` requiring `~sFence`; without either guard the invariant would be red in the honest model for reasons that have nothing to do with the marker.

### Critical Files for Implementation

- /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models/CaRelinkConfirmCore.tla
- /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-29-cas-relink-reoffer-redesign.md
- /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-29-cas-part-write-release-seam.md
- /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models/run_refcatalog.sh
- /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models/2026-07-28-v9-phase-RESULTS.md
