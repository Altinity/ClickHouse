# CAS relink / write-release seam — TLA+ gate phase Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Discharge §12 of `docs/superpowers/specs/2026-07-29-cas-relink-reoffer-redesign.md` — build the refined model the redesign requires, run it, and answer the one question that can still stop the design: **does `_sab_stalecache` flip from RED to GREEN once the apply-pending marker is represented?** If it does not, the design does not ship.

**Architecture:** One new TLA+ module, `docs/superpowers/models/CaRelinkReofferCore.tla`, which REFINES `CaRelinkConfirmCore` rather than editing it (§12's disposition ruling: the v11 model is kept as the historical witness of the v11 protocol and is not touched, because its `_sab_*` reds are the evidence that v11's rules were each load-bearing). The refinement adds five things the v11 model cannot express: the apply-pending marker with a *separate reader-visible value* (`sApply` / `sApplySeen` — the model-level twin of seam §6, including the full wedge lifecycle of `CasRefLedger::resolveWedgeOnce`), an equal-namespace/different-`disk_name` mount pair (§12.5 row i, test row 17's B1 shape), a leader tenure that commits several durable chunks of *different kinds* (§12.5 row ii), a receiver whose acceptance is the *conjunction* of a certified answer and a returned identity (§4.2, §4.4), and a committed-relink fact that covers the landed-`Unresolved` publication as well as `Committed`. Around them, a 2×2 necessity matrix decides the gate, a cross-mount battery decides the validator's qualification, and the re-derived rule set decides that every retained rule is still load-bearing under the fence-first ordering. Cfg names are deliberately carried over from the v11 model so that the flip is greppable side by side: `CaRelinkConfirmCore_sab_stalecache` RED next to `CaRelinkReofferCore_sab_stalecache` GREEN.

**Tech Stack:** TLA+ / TLC 2 (`tmp/tla2tools.jar` → TLAToolbox 1.7.4, OpenJDK 21), invoked exactly as the existing `docs/superpowers/models/run_*.sh` harnesses do. No C++ in this phase.

## Global Constraints

- Branch `cas-gc-rebuild`; commit after every task; **NEVER `git push`**.
- Commit messages: `ca: tla — <what>` plus this session's standard trailer lines.
- **NEVER weaken an existing invariant, or an existing model, to make a new config pass.** `CaRelinkConfirmCore.tla` and all twelve `CaRelinkConfirmCore_*.cfg` files are **read-only for this whole plan** — the only file of that family this plan may touch is `CaRelinkConfirmCore_RESULTS.md`, and only to append a pointer section (Task 4). If a new config only passes after an invariant is narrowed, that is a FAIL to record, not an edit to make.
- **Every model transition must be traceable to a cited code site or to a spec sentence.** A sabotage that is red only because the model fabricated a state the code cannot reach is worthless — worse than absent, because it reads as evidence. Each sender-lane action in Task 1 carries its `CasRefLedger.cpp` citation in a comment, the wedge block carries an explicit outcome→action mapping for every arm of `resolveWedgeOnce`, and the reviewer's job at the end of Task 1 is to check every one of them.
- **Every cfg change is reviewed against its sabotage intent before it is run.** Each `_sab_*` cfg header states, in one sentence, which single rule it removes and which invariant that must break; a cfg whose observed red is a DIFFERENT invariant than the one named is a FAIL, not a pass — the runner asserts the invariant NAME, not the exit code.
- **A green is only evidence once the property it rests on has been seen RED — in the same task, not a later one.** Sabotages run before greens, in every task and in the runner's config order. Concretely: no green may list an invariant whose `_sab_*` config does not yet exist. `TypeOK` is not exempt (`_sab_typeprobe` is its control) and neither is `PromotedNeverDangles` (`_sab_publishafterconfirm` lives in Task 1 for exactly this reason).
- **Structural exclusions are recorded, not asserted.** Where a hazardous state is made unrepresentable by an action guard rather than excluded by an invariant, the guard's comment says so and RESULTS lists it under structural exclusions. An invariant over a state the transition relation cannot enter would be green for free, which is the opposite of evidence.
- **Every TLC run is logged with markers, and no two runs can collide.** The runner takes a `RUN_ID` (`date +%Y%m%dT%H%M%S`-plus-PID), puts every metadir under `tmp/tlc-meta-relinkreoffer/$RUN_ID/<cfg>` and every log under `tmp/tlc-runs/relinkreoffer/$RUN_ID/tlc_<cfg>.log`, and holds an `flock` on `tmp/.relinkreoffer.lock` for the whole battery so two invocations cannot interleave. Each invocation is bracketed by `=== TLC BEGIN <module>_<cfg> <ISO-8601> ===` / `=== TLC END <module>_<cfg> rc=<n> <ISO-8601> ===`. Stable convenience symlinks `tmp/tlc_CaRelinkReofferCore_<cfg>.log` point at the newest run; **RESULTS cites the per-run path, never the symlink**, so a row can always be re-read.
- TLC invocation pattern (derived from `docs/superpowers/models/run_refcatalog.sh`): `/usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp ../../../tmp/tla2tools.jar tlc2.TLC -metadir <per-run metadir> -workers "${TLC_WORKERS:-1}" -config <cfg>.cfg CaRelinkReofferCore.tla`. Verdict: `grep -q "No error has been found"` ⇒ green; `grep -q "is violated"` ⇒ violation, with the invariant name extracted by `grep -oE '(Invariant|Property) [A-Za-z_]+ is violated'`.
- `-workers 1` by default, **not** `-workers auto`, following `run_refcatalog.sh`: parallel BFS makes WHICH shortest counterexample TLC prints nondeterministic between identical runs, and every trace this plan narrates in RESULTS is a specific action sequence. Override with `TLC_WORKERS=auto` when only a verdict is wanted.
- **Every declared CONSTANT must be assigned in every `.cfg`.** Task 1 declares the COMPLETE constants block — all twenty-two, including dials only exercised in Tasks 2 and 3 — and Task 1's configs set the unexercised ones to their honest value. Adding a constant later would force an edit to every previously written cfg; do not do that. **v3 note: the constants block is unchanged from v2 and is deliberately not touched.** The wedge fix of Task 1 step 4 is a new VARIABLE plus action guards, not a new dial, precisely so that no cfg has to churn.
- Model sizes: `MaxId = 6`, `MaxRound = 5`, `MaxChunks = 2`, `Receivers = {r1}`. These bounds were checked against the minimum trace depths of every sabotage before this plan was written (the deepest need two journal ids and three GC rounds; the two-chunk witness needs about eight transitions; `_sab_publishafterconfirm` about ten). If a config exceeds ~10 min under `-workers 1`, shrink `MaxId` then `MaxRound` — **never drop a property**. Record the bound actually used in RESULTS.
- The spec is the requirements source. Where this plan and spec §12 disagree, the spec wins and the discrepancy is reported rather than silently resolved.
- **The gate can fail.** If Task 1 step 10 finds `_sab_stalecache` RED, STOP: do not start Task 2. Record the counterexample, write the RESULTS file with `RELINK TLA GATE: FAIL`, and report the design as refuted. That is a legitimate, successful outcome of this plan.

---

## File map

| File | Responsibility |
| --- | --- |
| `docs/superpowers/models/CaRelinkReofferCore.tla` | NEW. The refined model: sender lane with the marker, its reader-visible twin, and every arm of the wedge lifecycle; two same-pool mounts; the fence-first answer as an (answer, identity) pair; the four-state receiver; GC. |
| `docs/superpowers/models/CaRelinkReofferCore_*.cfg` | NEW, 31 configs: 14 sabotages, 7 greens, 10 witnesses. |
| `docs/superpowers/models/run_relinkreoffer.sh` | NEW. Expected-verdict harness in the `run_refcatalog.sh` shape, with per-run metadirs, per-run logs, an `flock` guard, and BEGIN/END markers. |
| `docs/superpowers/models/2026-07-29-relink-seam-tla-RESULTS.md` | NEW. The phase verdict: `RELINK TLA GATE: PASS|FAIL` plus the do-not-implement consequence. |
| `docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md` | APPEND ONLY (Task 4). One section pointing at the refinement and stating that the v11 `_sab_stalecache` RED is deliberately preserved. |

---

### Task 1: The refined module and the apply-marker 2×2 — THE GATE

This is the task that can stop the design. Everything else in the plan is conditional on its step 10.

**Files:**
- Create: `docs/superpowers/models/CaRelinkReofferCore.tla`
- Create: `..._sab_noapplypending.cfg`, `..._sab_noapplypending_window.cfg`, `..._sab_relaxedmarker.cfg`, `..._sab_typeprobe.cfg`, `..._sab_publishafterconfirm.cfg`, `..._v11_baseline.cfg`, `..._ctl_v11nomarker.cfg`, `..._sab_stalecache.cfg`, `..._witness_busylane.cfg`, `..._witness_midtenure.cfg`, `..._witness_proven.cfg`, `..._witness_delete.cfg`, `..._witness_corruptwindow.cfg`
- Create: `docs/superpowers/models/run_relinkreoffer.sh`
- Read first: `docs/superpowers/models/CaRelinkConfirmCore.tla` (all 436 lines), `docs/superpowers/models/run_refcatalog.sh`, spec §12.3, §5.1.1, §5.1.2, seam §6, and **`CasRefLedger::commitRefChunk` and `CasRefLedger::resolveWedgeOnce` in full, in the file, before writing any wedge transition.** The file is `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp`. The six sites the model is built against:
  - ≈`:3134-3145` — the `Unresolved` wedge install. Its comment is the contract: *"The marker deliberately STAYS `ApplyPending` on the success path: an `Unresolved` outcome is precisely 'an object that may be durable and is not applied' ... cleared only when the resolution installs the transaction or proves it never landed."*
  - ≈`:1745-1746` — `NoWedge`: the ordinary flush pays nothing for any of the resolution machinery.
  - ≈`:1758-1766` — `FloorReconciled`. `durableFloorCovers(wedge->txn_id)` is checked FIRST, before a candidate is built: the transaction is durable, the floor records it, the table is `Poisoned`, and the wedge is still installed. The wedge is retired **without installing the transaction**, and the `clearApplyPending` call there is documented as *"A no-op on a `Poisoned` table (the transition is terminal and this is a CAS)"* — so `Poisoned` is RETAINED. Its log line: *"The table stays POISONED: its cached view is missing that transaction until a recovery re-derives it from the log."*
  - ≈`:1919-1926` — `Rejected`: our own epoch seal occupies the slot ⇒ our bytes provably never landed ⇒ `wedge.reset()` + `clearApplyPending`.
  - ≈`:1927-1934` — `Corrupted`: a FOREIGN occupant ⇒ `clearApplyPending` and **KEEP the wedge**, with the comment *"This is the one state where a wedged lane is not `ApplyPending`: our bytes provably never landed, so no apply is owed."*
  - ≈`:1936-1950` — `StillWedged` / `Reason::StaleState`: the object is durable and this runtime cannot record it ⇒ `noteDurableIdNotApplied(wedge.txn_id)` raises the floor over the id, `poisonApplyState`, and *"The wedge is KEPT, so the next flush's floor reconciliation retires it instead of re-applying an id that now sits at the floor."*
  - ≈`:2050-2060` — the fence-closing reaction to `Corrupted`: *"the mount is fenced closed and a remount is scheduled; the lane is deliberately left wedged for inspection."* It runs AFTER the resolution, which is what creates the window Task 3's `_sab_nowedge` exploits.
  - ≈`:2062-2128` — the other `StillWedged` reasons (`FenceMoved`, `Superseded`, `WedgeReplaced`, `RefusedPreAttempt`, `ResolveFoundNothing`). Every one of them says *"the result is inert"* and the lane keeps whatever wedge it has.

**Interfaces:**
- Consumes: nothing (first task).
- Produces: the vocabulary every later task references — module name `CaRelinkReofferCore`; the 22 constants listed in step 2; operators `Validator(m, b)`, `HeldValidator`, `GateRefuses(m)`, `SenderAnswer(m)`, `SenderIdentity(m)`, `OfferIdentity(m)`, `RAccepts(ans, idn)`, `BlobOf(m)`, `NsOf(m)`, `AdoptedBlobs`; invariants `TypeOK`, `ConfirmedRelinkNeverDangles`, `PromotedNeverDangles`, `MarkerCoversDurableWindow`, `MarkerSeenMatchesMarker`, `NeverPublishedTwice`, `ChangedImpliesFenced`; witnesses `W_BusyLaneProven`, `W_MidTenureProven`, `W_ProvenCommitted`, `W_BlobDeleted`, `W_CorruptWindow`.

---

- [ ] **Step 1: Read the v11 model end to end, and write down the five things it cannot express.**

Read `CaRelinkConfirmCore.tla` in full, then the eight `CasRefLedger.cpp` sites above. Before writing any new TLA+, write these five sentences into a scratch note — they become the new module's header, and getting them wrong is how this task goes wrong:

1. `SenderDurable` (`:180-189`) makes a transaction durable while leaving `sCacheRef` un-updated, and the ONLY predicate that refuses in that state is rule 3's `quiescent == SabotageStaleCache \/ (~sPending /\ ~sLeader)` (`:269`). `sPoison` is set only by `SenderPoison` — the apply-THREW case — so **the v11 model has no representation of the code's `ApplyPending` marker at all**, and therefore none of its wedge-retained, wedge-resolved or floor-reconciled states.
2. The marker is a relaxed atomic written with no lock held, so **what a reader OBSERVES is a different value from what the writer STORED** — and the v11 model, having no marker, has neither.
3. `NsS` is a single fixed sender namespace and `Token` identifies its blobs globally, so **a cross-mount validator collision is unrepresentable.**
4. `SenderAdmit → SenderDurable → SenderApply` is one transaction per tenure and every transaction targets the tracked binding, so **`~sLeader` and "the marker is clear" coincide by construction** and cannot be told apart.
5. `RConfirm` records a single `Gate1Answer`, so **the certified ANSWER and the returned IDENTITY are one object** and §4.2's requirement that both independently agree is unrepresentable.

- [ ] **Step 2: Write the module's header, universe and named assumptions.**

Create `docs/superpowers/models/CaRelinkReofferCore.tla`. The header comment MUST contain, in this order: (a) what is under test — the re-offer confirm of spec `2026-07-29-cas-relink-reoffer-redesign.md` §4.2's five-step fence-first ordering; (b) the five sentences from step 1, stated as what this module adds; (c) the loud `_sab_stalecache` note below; (d) the named-assumptions block below verbatim; (e) the scope notes below verbatim. The wedge outcome→action mapping is a separate block written in step 4, immediately above the wedge actions.

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

The named assumptions, verbatim (§12.2, §12.5 iii, §12's `FreshCertifiedResponse`) — the `ASSUME` is a tripwire, not a proof: deleting an assumption without updating the count fails every config:

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
       confirm.  Request/response GENERATIONS are a transport property and stay out.  PARTIALLY
       MODELLED, and the split is exact: the OFFER-CONFUSION half IS represented -- `OfferIdentity`
       is an ungated resolve carrying NO certified answer, always available as a transport outcome,
       and `_sab_inferanswer` is red on it -- while the REPLAY half (a genuine earlier response,
       bit-for-bit valid) is not, because it needs generations.  DISCHARGED BY: relink spec §11
       row 16 (nonce echo) for the replay half and rows 15(a)/15(b) for the confusion half.  If any
       of those rows is weakened, this assumption goes with it. *)
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
     * §8 row S7 (marker synchronization) IS the row with model content, and it is carried by TWO
       invariants, not one: `MarkerCoversDurableWindow` for the INTERVAL ("the marker is set for the
       whole durable-but-unapplied window") and `MarkerSeenMatchesMarker` for the OBSERVATION ("a
       reader acquiring after the arm observes the armed value").  What stays code-level is the
       MECHANISM that makes the second true -- `state_mutex` at the call site supplying the
       happens-before (seam §6) -- because no untimed model can check a memory model.  A model can
       check the property the memory model must deliver, and this one does.
   Recovery COMPLETENESS -- that a recovered view is a complete replay of the durable log -- is not
   this answer's contract (§4.2, "Completeness is recovery's contract, not this answer's").  It is
   unrepresentable here by construction: `RecoverForAnswer` installs the durable view ATOMICALLY,
   which is also how §4.3's "abandoning before the install leaves nothing partial" is encoded.
   The wedge-resolution tenure is out of scope as a TENURE question (§12.5); its marker states are
   NOT out of scope and every arm of `resolveWedgeOnce` is mapped to an action or to a stated
   non-transition -- see the mapping block above the wedge actions. *)
```

Now the universe. **Unchanged from v2 — do not churn it:**

```tla
-------------------- MODULE CaRelinkReofferCore --------------------
EXTENDS Integers, FiniteSets

CONSTANTS
    Receivers,                     \* relink receivers (one suffices for this safety class)
    MaxId,                         \* bound on the pool-wide ref-transaction id counter
    MaxRound,                      \* bound on the number of GC rounds
    MaxChunks,                     \* durable chunk transactions ONE mount may commit in a behaviour
    SecondMount,                   \* a second same-pool mount: EQUAL root_namespace, DIFFERENT disk
    EqualNamespaces,               \* TRUE = the two mounts share a namespace string (§3's legal case)
    ModelColdTable,                \* TRUE = a mount may be evicted, so rule 2 is on the answer path
    TrackHistory,                  \* TRUE only in _witness_* cfgs: keeps history vars out of greens
    SabotageStaleCache,            \* drop v11 rule 3's pending/leader terms -- TRUE IS THE v12 DESIGN
    SabotageNoApplyPending,        \* the marker is NEVER ARMED (§12.3 step 2, first half)
    SabotageRelaxedMarker,         \* the arm STORES but does not PUBLISH: sApplySeen lags (seam §6)
    SabotageNoPoison,              \* the gate ignores apply_state = Poisoned
    SabotageNoWedge,               \* the gate ignores the wedge
    SabotageNoFence,               \* the gate ignores the mount fence (rule 1, hoisted first)
    SabotageNoRowExact,            \* rule 5 degenerates to "some binding is present"
    SabotageBareValidator,         \* validator = ManifestRef alone (B1's collision shape)
    SabotageNoDiskQual,            \* validator = namespace + ref, disk_name dropped (B1's fix)
    SabotageInferAnswer,           \* the receiver INFERS the answer from a matching identity (§4.1.3)
    SabotageSkipIdentity,          \* the receiver trusts `proven` and skips §4.4 condition 4
    SabotagePublishAfterConfirm,   \* invert the order: confirm+promote BEFORE the durable +1
    SabotageS2ByteFetch,           \* S2 byte-fetches instead of throwing retry-later
    SabotageTypeProbe              \* TypeOK's negative control: write an out-of-domain value

Token      == "m1"       \* the ManifestRef the offer minted; BOTH mounts may bind this same text
Other      == "m2"
NoBinding  == "none"
Absent     == "absent"   \* no identity on the response (§4.2 row 3: no binding => no validator)
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

ChunkKinds == {"tracked", "unrelated"}   \* what the in-flight chunk mutates (§12.5 ii)
ApplyStates == {"clean", "pending", "poisoned"}
Sources    == {"s_dA", "s_dB"} \cup Receivers
Namespaces == {NsA, NsB} \cup Receivers
Ids        == 0..MaxId
Rounds     == 0..MaxRound
Records    == [id: Ids, ns: Namespaces, blob: Blobs, src: Sources, op: {"add", "del"}]
```

- [ ] **Step 3: Write the variables and `Init`.**

One variable is new in v3: `sFloorCovers`, the model's `RefTableState::durableFloorCovers(wedge->txn_id)`. It is what makes the two-step stale resolution representable and the poisoned-to-clean install unrepresentable.

```tla
VARIABLES
    round, present, condemned, pendingDelete, folded, cursor, gcPhase,   \* GC
    journal, nextId,                                                     \* the durable ref log
    sDurableRef,      \* GROUND TRUTH: the namespace's durable binding, foreign writes included
    sCacheRef,        \* the mount's in-memory committed row -- what the answer reads
    sTarget, sKind, sPending, sLeader, sArmed,
    sApply,           \* the marker as STORED
    sApplySeen,       \* the marker as a `state_mutex` reader OBSERVES it (seam §6)
    sApplyOwed,       \* GHOST: this mount has an own DURABLE transaction not yet installed
    sFloorCovers,     \* the durable floor records the wedged txn as durable-but-not-applied
    sWedge, sForeign, sFence, sRecovered, sChunks, sTenureChunks,
    partMount,                                                           \* which mount holds the part
    rState, rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore,
    rAnsweredFenced, rPublishes, rUnresolved,
    sawBusyProven, sawMidTenureProven, sawProvenCommitted, sawLandedS2,
    sawChangedThenBytes, sawUnknown, sawColdRefused, sawCollision, sawCorruptWindow

gcVars     == << round, present, condemned, pendingDelete, folded, cursor, gcPhase >>
senderVars == << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sArmed, sApply,
                 sApplySeen, sApplyOwed, sFloorCovers, sWedge, sForeign, sFence, sRecovered,
                 sChunks, sTenureChunks >>
recvVars   == << rState, rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore,
                 rAnsweredFenced, rPublishes, rUnresolved >>
logVars    == << journal, nextId >>
histVars   == << sawBusyProven, sawMidTenureProven, sawProvenCommitted, sawLandedS2,
                 sawChangedThenBytes, sawUnknown, sawColdRefused, sawCollision,
                 sawCorruptWindow >>
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
    /\ sKind       = [m \in Mounts |-> "tracked"]
    /\ sPending    = [m \in Mounts |-> FALSE]
    /\ sLeader     = [m \in Mounts |-> FALSE]
    /\ sArmed      = [m \in Mounts |-> FALSE]
    /\ sApply      = [m \in Mounts |-> "clean"]
    /\ sApplySeen  = [m \in Mounts |-> "clean"]
    /\ sApplyOwed  = [m \in Mounts |-> FALSE]
    /\ sFloorCovers = [m \in Mounts |-> FALSE]
    /\ sWedge      = [m \in Mounts |-> FALSE]
    /\ sForeign    = [m \in Mounts |-> FALSE]
    /\ sFence      = [m \in Mounts |-> TRUE]
    /\ sRecovered  = [m \in Mounts |-> TRUE]
    /\ sChunks       = [m \in Mounts |-> 0]
    /\ sTenureChunks = [m \in Mounts |-> 0]
    /\ partMount = MountA
    /\ rState  = [r \in Receivers |-> "init"]
    /\ rAnswer = [r \in Receivers |-> "none"]
    /\ rIdentity = [r \in Receivers |-> Absent]
    /\ rAccepted  = [r \in Receivers |-> FALSE]
    /\ rCommitted = [r \in Receivers |-> FALSE]
    /\ rDurableBefore  = [r \in Receivers |-> FALSE]
    /\ rAnsweredFenced = [r \in Receivers |-> FALSE]
    /\ rPublishes  = [r \in Receivers |-> 0]
    /\ rUnresolved = [r \in Receivers |-> FALSE]
    /\ sawBusyProven = FALSE /\ sawMidTenureProven = FALSE /\ sawProvenCommitted = FALSE
    /\ sawLandedS2 = FALSE /\ sawChangedThenBytes = FALSE /\ sawUnknown = FALSE
    /\ sawColdRefused = FALSE /\ sawCollision = FALSE /\ sawCorruptWindow = FALSE
```

- [ ] **Step 4: Write the sender lane — chunk kinds, the marker's two values, and the FULL wedge lifecycle.**

Every action carries the code citation that licenses it. **The wedge block is the part a reviewer must check arm by arm against `resolveWedgeOnce`** — two earlier drafts of this plan got it wrong in two different ways, first by clearing the marker on an unresolved PUT and then by omitting the floor-reconciliation step and leaving a poisoned-to-clean install enabled. The mapping block below is the antidote: every outcome of the function has a line, including the ones that are deliberately NOT transitions.

```tla
(* ---- the sender's ref lane: ONE tenure, SEVERAL durable chunks of TWO kinds ------------------ *)

(* Admission.  `pending` and `leader_active` are exactly the two predicates v11 rule 3 read under
   `ref_queue_mutex` -- and exactly the two this design deletes.  The tenure OPENS here and does not
   close per chunk.  TWO KINDS, and the second is why a multi-chunk tenure is reachable at all: a
   "tracked" chunk mutates the binding under test and therefore requires it to still exist, while an
   "unrelated" chunk mutates some OTHER ref of the same namespace and requires nothing -- which is
   the ordinary case of a busy writer's lane (§2) and the case §12.5 ii asks the model to express. *)
SenderAdmit(m, nb, k) ==
    /\ sFence[m] /\ sRecovered[m]
    /\ ~sPending[m] /\ ~sWedge[m] /\ sApply[m] = "clean"
    /\ (k = "tracked") => (sDurableRef[m] = Token /\ nb # Token)
    /\ sChunks[m] < MaxChunks
    /\ nextId <= MaxId
    /\ sPending' = [sPending EXCEPT ![m] = TRUE]
    /\ sLeader'  = [sLeader  EXCEPT ![m] = TRUE]
    /\ sTarget'  = [sTarget  EXCEPT ![m] = IF k = "tracked" THEN nb ELSE sTarget[m]]
    /\ sKind'    = [sKind    EXCEPT ![m] = k]
    /\ sTenureChunks' = [sTenureChunks EXCEPT ![m] = IF sLeader[m] THEN sTenureChunks[m] ELSE 0]
    /\ UNCHANGED << sDurableRef, sCacheRef, sArmed, sApply, sApplySeen, sApplyOwed, sFloorCovers,
                    sWedge, sForeign, sFence, sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* THE ARM.  Seam §6: `armApplyPending` inside a `state_mutex` scope IMMEDIATELY before the PUT --
   "the last statement that still runs while nothing of this transaction can possibly be durable"
   (`CasRefLedger.cpp:2808`).  `sArmed` makes the ORDERING structural.
   TWO SABOTAGES, and they are different failures:
     * SabotageNoApplyPending -- the marker is not there at all: BOTH values stay clean.
     * SabotageRelaxedMarker  -- the marker IS stored but the store is not PUBLISHED to a reader
       taking `state_mutex` afterwards, which is exactly what the relaxed
       `compare_exchange_strong` at `:1681`, called at `:2808` with no lock held, permits.  The
       STORED value moves; the SEEN value lags.  These are not the same state machine, which is why
       both configs exist. *)
SenderArm(m) ==
    /\ sPending[m] /\ ~sArmed[m] /\ sApply[m] = "clean"
    /\ sArmed' = [sArmed EXCEPT ![m] = TRUE]
    /\ sApply' = [sApply EXCEPT ![m] = IF SabotageNoApplyPending THEN "clean" ELSE "pending"]
    /\ sApplySeen' = [sApplySeen EXCEPT ![m] =
           IF SabotageNoApplyPending    THEN "clean"
           ELSE IF SabotageRelaxedMarker THEN sApplySeen[m]   \* the store is not published
           ELSE                              "pending"]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sApplyOwed,
                    sFloorCovers, sWedge, sForeign, sFence, sRecovered, sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* The conditional PUT is acked: DURABLE, GC can fold it, the committed row still lags.  NO durable
   byte without a prior arm -- that guard is the ordering the whole design rests on.  A "tracked"
   chunk moves the binding and appends the removal GC will fold; an "unrelated" chunk is durable and
   owes an apply without touching the tracked binding or the blob universe. *)
SenderDurable(m) ==
    /\ sPending[m] /\ sArmed[m] /\ ~sWedge[m] /\ ~sApplyOwed[m]
    /\ nextId <= MaxId
    /\ IF sKind[m] = "tracked"
         THEN /\ sDurableRef[m] = Token
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> NsOf(m), blob |-> BlobOf(m),
                      src |-> EdgeOf(m), op |-> "del"] }
              /\ nextId' = nextId + 1
              /\ sDurableRef' = [sDurableRef EXCEPT ![m] = sTarget[m]]
         ELSE /\ UNCHANGED logVars
              /\ UNCHANGED sDurableRef
    /\ sApplyOwed' = [sApplyOwed EXCEPT ![m] = TRUE]
    /\ UNCHANGED << sCacheRef, sTarget, sKind, sPending, sLeader, sArmed, sApply, sApplySeen,
                    sFloorCovers, sWedge, sForeign, sFence, sRecovered, sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, histVars, partMount >>

(* THE INSTALL.  `clearApplyPending` is the last statement of the install region (`:2992`), in the
   same allocation-free scope as `rt->state.swap(*candidate)`: "'recorded' and 'no apply owed'
   become true together or not at all".  The TENURE SURVIVES -- `sLeader` untouched -- so the next
   chunk of the same tenure may open while the marker is clean and the view is complete.  That is
   the state v11 refused and v12 answers from. *)
SenderInstall(m) ==
    /\ sArmed[m] /\ sApplyOwed[m] /\ ~sWedge[m]
    /\ sCacheRef' = [sCacheRef EXCEPT ![m] =
           IF sKind[m] = "tracked" THEN sDurableRef[m] ELSE sCacheRef[m]]
    /\ sApply'     = [sApply     EXCEPT ![m] = "clean"]
    /\ sApplySeen' = [sApplySeen EXCEPT ![m] = "clean"]
    /\ sApplyOwed' = [sApplyOwed EXCEPT ![m] = FALSE]
    /\ sArmed'     = [sArmed     EXCEPT ![m] = FALSE]
    /\ sPending'   = [sPending   EXCEPT ![m] = FALSE]
    /\ sChunks'       = [sChunks       EXCEPT ![m] = sChunks[m] + 1]
    /\ sTenureChunks' = [sTenureChunks EXCEPT ![m] = sTenureChunks[m] + 1]
    /\ UNCHANGED << sDurableRef, sTarget, sKind, sLeader, sFloorCovers, sWedge, sForeign,
                    sFence, sRecovered >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

SenderCloseTenure(m) ==
    /\ sLeader[m] /\ ~sPending[m] /\ ~sArmed[m]
    /\ sLeader' = [sLeader EXCEPT ![m] = FALSE]
    /\ sTenureChunks' = [sTenureChunks EXCEPT ![m] = 0]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sArmed, sApply, sApplySeen,
                    sApplyOwed, sFloorCovers, sWedge, sForeign, sFence, sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* The in-memory apply THREW although the object is durable.  The tenure closes -- the lane looks
   perfectly quiescent -- and only the Poisoned arm of rule 4 can see it. *)
SenderPoison(m) ==
    /\ sArmed[m] /\ sApplyOwed[m] /\ ~sWedge[m]
    /\ sApply'     = [sApply     EXCEPT ![m] = "poisoned"]
    /\ sApplySeen' = [sApplySeen EXCEPT ![m] = "poisoned"]
    /\ sArmed'   = [sArmed   EXCEPT ![m] = FALSE]
    /\ sPending' = [sPending EXCEPT ![m] = FALSE]
    /\ sLeader'  = [sLeader  EXCEPT ![m] = FALSE]
    /\ sTenureChunks' = [sTenureChunks EXCEPT ![m] = 0]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sApplyOwed, sFloorCovers, sWedge,
                    sForeign, sFence, sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* ============================================================================================ *)
(* ---- THE WEDGE LIFECYCLE: every arm of `CasRefLedger::resolveWedgeOnce`, mapped ------------- *)
(* ============================================================================================ *)
(*
   OUTCOME (WedgeResolution / Reason)          SITE           MODEL
   ------------------------------------------  -------------  ------------------------------------
   the Unresolved PUT that CREATES the wedge   :3134-3145     SenderUnresolvedLanded /
     -- the marker deliberately STAYS pending                   SenderUnresolvedNotLanded
   NoWedge                                     :1745-1746     NO ACTION -- the absence of `sWedge`;
                                                                the ordinary flush pays nothing
   FloorReconciled                             :1758-1766     FloorReconcile -- wedge RETIRED, the
     -- floor already records the txn; the                       txn NOT installed, Poisoned
        `clearApplyPending` there is a                           RETAINED, `sApplyOwed` RETAINED
        documented no-op on a Poisoned table
   Rejected (own epoch seal at the slot)       :1919-1926     WedgeResolveRejected -- wedge reset,
     -- proven non-durable, no apply owed                        marker clean
   Corrupted (FOREIGN occupant)                :1927-1934     WedgeResolveCorrupted -- marker
     -- the ONE state where a wedged lane        + reaction       cleared, wedge KEPT ...
        is not ApplyPending                      :2050-2060     ... then CorruptFenceReaction, a
                                                                SEPARATE step: fence closed
   StillWedged / Reason::StaleState             :1936-1950     WedgeResolveStale -- floor raised
     -- durable, cannot be recorded here:                        over the id (sFloorCovers),
        floor + poison, wedge KEPT for the                       poisoned, wedge KEPT
        next flush's floor reconciliation
   adoption install (Created, or Occupied       :1953+         WedgeResolveInstall -- state.swap,
        with our own bytes)                                      wedge cleared, marker clean.
                                                                GUARDED by ~sFloorCovers and
                                                                sApply # "poisoned": see below
   StillWedged / FenceMoved, Superseded,        :2062-2128     NO ACTION -- every one of these is
        WedgeReplaced, RefusedPreAttempt,                        INERT ("the result is inert and the
        ResolveFoundNothing                                      lane keeps whatever wedge it has"),
                                                                i.e. UNCHANGED vars, which `NoOp`
                                                                already supplies.  Modelled as
                                                                DELAY, not as a transition.
*)

(* UNRESOLVED (`:3134-3145`).  The PUT outcome is unknown, the wedge is installed, and *** the
   MARKER STAYS `ApplyPending` ***: its own comment says an `Unresolved` outcome is "precisely 'an
   object that may be durable and is not applied'", cleared only when the resolution installs the
   transaction or proves it never landed.  The ledger's state is IDENTICAL in both arms below -- only
   the ground truth differs, which is the whole point of a wedge. *)
SenderUnresolvedLanded(m) ==
    /\ sPending[m] /\ sArmed[m] /\ ~sWedge[m] /\ ~sApplyOwed[m]
    /\ sKind[m] = "tracked" /\ sDurableRef[m] = Token /\ nextId <= MaxId
    /\ journal' = journal \cup
         { [id |-> nextId, ns |-> NsOf(m), blob |-> BlobOf(m), src |-> EdgeOf(m), op |-> "del"] }
    /\ nextId' = nextId + 1
    /\ sDurableRef' = [sDurableRef EXCEPT ![m] = sTarget[m]]
    /\ sApplyOwed'  = [sApplyOwed  EXCEPT ![m] = TRUE]
    /\ sWedge'   = [sWedge   EXCEPT ![m] = TRUE]
    /\ sPending' = [sPending EXCEPT ![m] = FALSE]
    /\ sLeader'  = [sLeader  EXCEPT ![m] = FALSE]
    /\ sTenureChunks' = [sTenureChunks EXCEPT ![m] = 0]
    /\ UNCHANGED << sCacheRef, sTarget, sKind, sArmed, sApply, sApplySeen, sFloorCovers, sForeign,
                    sFence, sRecovered, sChunks >>          \* sApply STAYS "pending" -- by contract
    /\ UNCHANGED << gcVars, recvVars, histVars, partMount >>

SenderUnresolvedNotLanded(m) ==
    /\ sPending[m] /\ sArmed[m] /\ ~sWedge[m] /\ ~sApplyOwed[m]
    /\ sWedge'   = [sWedge   EXCEPT ![m] = TRUE]
    /\ sPending' = [sPending EXCEPT ![m] = FALSE]
    /\ sLeader'  = [sLeader  EXCEPT ![m] = FALSE]
    /\ sTenureChunks' = [sTenureChunks EXCEPT ![m] = 0]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sArmed, sApply, sApplySeen,
                    sApplyOwed, sFloorCovers, sForeign, sFence, sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* ADOPTION INSTALL (`:1953+`): durable, and the table has not advanced under the resolution, so it
   can be recorded.  *** TWO GUARDS, AND BOTH ARE STRUCTURAL EXCLUSIONS, NOT DECORATION. ***
     * `~sFloorCovers[m]` -- `durableFloorCovers` is tested FIRST in the real function (`:1758`),
       before a candidate is even built, so a floor-covered wedge can only take the
       `FloorReconciled` path.  An install here would re-apply an id that now sits AT the floor,
       which `applyTxnInPlace` refuses as non-contiguous -- "every later flush would throw in the
       same place and the lane would stay wedged for the runtime's life".
     * `sApply[m] # "poisoned"` -- a poisoned cache is by definition missing a durable transaction
       and may never be silently advanced to clean.  An earlier draft of this model guarded this
       action on `sWedge /\ sApplyOwed` ALONE, which left a poisoned-to-clean install enabled
       immediately after `WedgeResolveStale`: an impossible transition presented as a reachable one.
   Neither guard has an invariant of its own, deliberately: they make the states UNREPRESENTABLE, and
   an invariant over a state the transition relation cannot enter would be green for free.  They are
   recorded as structural exclusions in `2026-07-29-relink-seam-tla-RESULTS.md`. *)
WedgeResolveInstall(m) ==
    /\ sWedge[m] /\ sApplyOwed[m]
    /\ ~sFloorCovers[m]
    /\ sApply[m] # "poisoned"
    /\ sCacheRef'  = [sCacheRef  EXCEPT ![m] = sDurableRef[m]]
    /\ sApply'     = [sApply     EXCEPT ![m] = "clean"]
    /\ sApplySeen' = [sApplySeen EXCEPT ![m] = "clean"]
    /\ sApplyOwed' = [sApplyOwed EXCEPT ![m] = FALSE]
    /\ sWedge'     = [sWedge     EXCEPT ![m] = FALSE]
    /\ sArmed'     = [sArmed     EXCEPT ![m] = FALSE]
    /\ sChunks'    = [sChunks    EXCEPT ![m] = sChunks[m] + 1]
    /\ UNCHANGED << sDurableRef, sTarget, sKind, sPending, sLeader, sFloorCovers, sForeign,
                    sFence, sRecovered, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* REJECTED (`:1919-1926`): our own epoch seal occupies the slot, so our bytes PROVABLY never
   landed.  "no apply is owed" -- the wedge resets and the marker clears together.  Reached only
   when the floor does NOT cover the id, because that test comes first. *)
WedgeResolveRejected(m) ==
    /\ sWedge[m] /\ ~sApplyOwed[m] /\ ~sForeign[m] /\ ~sFloorCovers[m]
    /\ sWedge'     = [sWedge     EXCEPT ![m] = FALSE]
    /\ sApply'     = [sApply     EXCEPT ![m] = "clean"]
    /\ sApplySeen' = [sApplySeen EXCEPT ![m] = "clean"]
    /\ sArmed'     = [sArmed     EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sApplyOwed,
                    sFloorCovers, sForeign, sFence, sRecovered, sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* CORRUPTED (`:1927-1934`) -- *** THE ONE STATE WHERE A WEDGED LANE IS NOT ApplyPending. ***  A
   FOREIGN object at a key that mount-lease exclusivity says is exclusively ours.  The code clears
   the marker ("our bytes provably never landed, so no apply is owed") and deliberately KEEPS the
   wedge for inspection.  Two consequences the model must carry, and both matter:
     * exclusivity is BREACHED, so this namespace has another writer whose durable removals this
       mount's committed row cannot see -- `sForeign` is what enables `ForeignRemove` below;
     * the fence-closing reaction is a SEPARATE, later step (`:2050-2060`), so there is a window in
       which the lane is fence-live, marker-clean and wedged.  Rule 3 is the ONLY guard in it.
   This window is `_sab_nowedge`'s counterexample and `_witness_corruptwindow`'s subject. *)
WedgeResolveCorrupted(m) ==
    /\ sWedge[m] /\ ~sApplyOwed[m] /\ ~sForeign[m] /\ ~sFloorCovers[m]
    /\ sForeign'   = [sForeign   EXCEPT ![m] = TRUE]
    /\ sApply'     = [sApply     EXCEPT ![m] = "clean"]
    /\ sApplySeen' = [sApplySeen EXCEPT ![m] = "clean"]
    /\ sArmed'     = [sArmed     EXCEPT ![m] = FALSE]
    /\ sawCorruptWindow' = sawCorruptWindow \/ H(sFence[m])
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sApplyOwed,
                    sFloorCovers, sWedge, sFence, sRecovered, sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, partMount >>       \* wedge KEPT
    /\ UNCHANGED << sawBusyProven, sawMidTenureProven, sawProvenCommitted, sawLandedS2,
                    sawChangedThenBytes, sawUnknown, sawColdRefused, sawCollision >>

(* The reaction (`:2050-2060`): the mount is fenced closed and a remount is scheduled. *)
CorruptFenceReaction(m) ==
    /\ sForeign[m] /\ sFence[m]
    /\ sFence' = [sFence EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sArmed, sApply,
                    sApplySeen, sApplyOwed, sFloorCovers, sWedge, sForeign, sRecovered,
                    sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* STALE STATE (`:1936-1950`), STEP ONE OF TWO.  The object is proven durable and this runtime
   cannot record it, "which is exactly what the floor and `Poisoned` are for":
   `noteDurableIdNotApplied` raises the durable floor over the wedged id, `poisonApplyState` poisons
   the table, and *** the WEDGE IS KEPT *** -- "so the next flush's floor reconciliation retires it
   instead of re-applying an id that now sits at the floor".  `sApplyOwed` stays TRUE, which is the
   truth: the transaction is durable and this cache does not contain it. *)
WedgeResolveStale(m) ==
    /\ sWedge[m] /\ sApplyOwed[m] /\ ~sFloorCovers[m]
    /\ sFloorCovers' = [sFloorCovers EXCEPT ![m] = TRUE]
    /\ sApply'       = [sApply       EXCEPT ![m] = "poisoned"]
    /\ sApplySeen'   = [sApplySeen   EXCEPT ![m] = "poisoned"]
    /\ sArmed'       = [sArmed       EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sApplyOwed,
                    sWedge, sForeign, sFence, sRecovered, sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* FLOOR RECONCILIATION (`:1758-1766`), STEP TWO OF TWO -- the transition an earlier draft of this
   model omitted entirely.  The NEXT invocation sees `durableFloorCovers(wedge->txn_id)` and takes
   this path before any candidate is built: the wedge is RETIRED and the transaction is NOT
   installed.  Three things stay exactly as they were, and each is a code fact rather than a
   modelling convenience:
     * the cached row is NOT advanced -- "nothing is installed on this path";
     * `Poisoned` is RETAINED -- the `clearApplyPending` call there is documented as "A no-op on a
       `Poisoned` table (the transition is terminal and this is a CAS)";
     * `sApplyOwed` is RETAINED -- "The table stays POISONED: its cached view is missing that
       transaction until a recovery re-derives it from the log."
   So `MarkerCoversDurableWindow` holds through both steps without any exclusion, and the lane is
   stopped (every admission requires a clean marker) until a recovery re-derives the cache. *)
FloorReconcile(m) ==
    /\ sWedge[m] /\ sFloorCovers[m]
    /\ sWedge' = [sWedge EXCEPT ![m] = FALSE]
    /\ sArmed' = [sArmed EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sApply, sApplySeen,
                    sApplyOwed, sFloorCovers, sForeign, sFence, sRecovered, sChunks,
                    sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* ---- fence loss, the foreign writer, and rule 2's two arms ----------------------------------- *)

FenceLoss(m) ==
    /\ sFence[m] /\ ~sPending[m] /\ ~sArmed[m]
    /\ sFence' = [sFence EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sArmed, sApply,
                    sApplySeen, sApplyOwed, sFloorCovers, sWedge, sForeign, sRecovered,
                    sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* Another writer removes the binding: durable, folded by GC, invisible to this mount's committed
   row.  Enabled by EITHER of the two ways exclusivity can fail -- a lost fence (this instance was
   deposed) or a proven foreign occupant (exclusivity was breached while the fence still reads
   live).  It never touches `sApplyOwed`, because it is not this mount's transaction -- which is
   precisely why `MarkerCoversDurableWindow` needs no exclusion clause. *)
ForeignRemove(m) ==
    /\ (~sFence[m] \/ sForeign[m])
    /\ sDurableRef[m] = Token /\ nextId <= MaxId
    /\ journal' = journal \cup
         { [id |-> nextId, ns |-> NsOf(m), blob |-> BlobOf(m), src |-> EdgeOf(m), op |-> "del"] }
    /\ nextId' = nextId + 1
    /\ sDurableRef' = [sDurableRef EXCEPT ![m] = NoBinding]
    /\ UNCHANGED << sCacheRef, sTarget, sKind, sPending, sLeader, sArmed, sApply, sApplySeen,
                    sApplyOwed, sFloorCovers, sWedge, sForeign, sFence, sRecovered, sChunks,
                    sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, histVars, partMount >>

(* Eviction requires no IN-FLIGHT transaction, NOT a clean marker: a poisoned cache is exactly the
   one you want evicted, and eviction plus recovery is how the code's "until a recovery re-derives
   it from the log" is reached.  Without this the poisoned/floor-reconciled lane would be a dead end
   whenever `ModelColdTable = FALSE`; with `ModelColdTable = FALSE` it still is, and that is
   accepted -- a dead end is a state, not an error, and `NoOp` keeps TLC from reporting a
   deadlock. *)
EvictTable(m) ==
    /\ ModelColdTable /\ sRecovered[m]
    /\ ~sPending[m] /\ ~sArmed[m] /\ ~sLeader[m] /\ ~sWedge[m] /\ sApply[m] # "pending"
    /\ sRecovered' = [sRecovered EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sArmed, sApply,
                    sApplySeen, sApplyOwed, sFloorCovers, sWedge, sForeign, sFence, sChunks,
                    sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* §4.3: recovery installs its result ATOMICALLY, so abandoning before the install leaves nothing
   partial and a half-recovered view is unrepresentable.  It is also the ONLY exit from the poisoned
   state, and it clears the marker, the owed apply and the floor record together -- "a recovery
   re-derives the cache from the log". *)
RecoverForAnswer(m) ==
    /\ ~sRecovered[m] /\ ~sPending[m] /\ ~sArmed[m] /\ ~sWedge[m]
    /\ sRecovered' = [sRecovered EXCEPT ![m] = TRUE]
    /\ sCacheRef'  = [sCacheRef  EXCEPT ![m] = sDurableRef[m]]
    /\ sApply'     = [sApply     EXCEPT ![m] = "clean"]
    /\ sApplySeen' = [sApplySeen EXCEPT ![m] = "clean"]
    /\ sApplyOwed' = [sApplyOwed EXCEPT ![m] = FALSE]
    /\ sFloorCovers' = [sFloorCovers EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sTarget, sKind, sPending, sLeader, sArmed, sWedge, sForeign,
                    sFence, sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>
```

- [ ] **Step 5: Write the validator, and the answer as an (answer, identity) PAIR.**

**Unchanged from v2 — the reviewer verified every cited symbol here; do not churn it.**

```tla
(* ---- the validator ---------------------------------------------------------------------------- *)

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

(* ---- §4.2's fence-first ordering, and the TWO INDEPENDENT response fields -------------------- *)

(* Rule 1 (fence) is FIRST, which is the reorder.  Rule 4 reads the marker a `state_mutex` reader
   OBSERVES -- `sApplySeen`, not `sApply` -- which is what makes seam §6 a property this model can
   check rather than a comment. *)
GateRefuses(m) ==
    \/ ~(SabotageNoFence \/ sFence[m])                              \* 1. mount fence -- HOISTED
    \/ ~sRecovered[m]                                               \* 2. residency and recovery
    \/ ~(SabotageNoWedge \/ ~sWedge[m])                             \* 3. wedge
    \/ ~(sApplySeen[m] = "clean"                                    \* 4. apply state, AS OBSERVED
         \/ (SabotageNoPoison /\ sApplySeen[m] = "poisoned"))
    \/ ~(SabotageStaleCache \/ (~sPending[m] /\ ~sLeader[m]))       \* v11 rule 3 -- TRUE = v12

RowPresent(m) == sCacheRef[m] # NoBinding

(* THE TWO FIELDS ARE COMPUTED SEPARATELY, because §4.2 requires them to agree INDEPENDENTLY:
   "`proven` with a non-matching identity is a contradiction, not a `Yes` ... Each is separately
   sufficient to fail closed."  A model that derived one from the other could not express that. *)
SenderIdentity(m) == IF GateRefuses(m) \/ ~RowPresent(m) THEN Absent
                                                         ELSE Validator(m, sCacheRef[m])

SenderAnswer(m) ==
    IF GateRefuses(m) \/ ~RowPresent(m) THEN "unknown"
    ELSE IF SabotageNoRowExact THEN "proven"
    ELSE IF Validator(m, sCacheRef[m]) = HeldValidator THEN "proven"
    ELSE "changed"

(* §4.1.3: an OFFER response is an UNGATED resolve that carries NO certified answer at all.  It is
   always available as a transport outcome, because a stripped mode parameter is something the wire
   genuinely permits (§11 row 15) -- and its identity may well EQUAL the held validator, which is
   exactly why the explicit answer cookie exists. *)
OfferIdentity(m) == IF RowPresent(m) THEN Validator(m, sCacheRef[m]) ELSE Absent

(* §4.4 conditions 3 and 4.  Conditions 1 and 2 -- empty body, nonce echo -- are
   `FreshCertifiedResponse`'s territory and are not modelled. *)
RAccepts(ans, idn) ==
    IF SabotageInferAnswer        THEN idn = HeldValidator         \* infer the answer: §4.1.3 (1)
    ELSE IF SabotageSkipIdentity  THEN ans = "proven"              \* skip condition 4
    ELSE                               ans = "proven" /\ idn = HeldValidator
```

- [ ] **Step 6: Write the receiver, GC, `Next`, and Task 1's invariants and witnesses.**

Task 1 needs only the minimum receiver that can reach a committed relink; Task 3 adds S0/S2 and the byte-fetch arms. The receiver, GC, `Next` and witness blocks below are **unchanged from v2** apart from `sFloorCovers` in `TypeOK` and the three new wedge actions in `Next`.

```tla
(* ---- the receiver (§6; S0 and S2 arrive in Task 3) ----------------------------------------- *)

RStage(r) ==
    /\ \/ rState[r] = "init"
       \/ (SabotagePublishAfterConfirm /\ rState[r] = "S3")
    /\ ~(\E rec \in journal : rec.ns = r /\ rec.op = "add")
    /\ nextId <= MaxId
    /\ journal' = journal \cup
         { [id |-> nextId, ns |-> r, blob |-> BlobA, src |-> r, op |-> "add"] }
    /\ nextId' = nextId + 1
    /\ rState' = [rState EXCEPT ![r] = IF rState[r] = "init" THEN "S1" ELSE "S3"]
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore, rAnsweredFenced,
                    rPublishes, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, histVars, partMount >>

(* T2: the re-offer, answered by whichever mount holds the part NOW (§3: the request carries `part`
   and `endpoint`, so the sender routes it exactly as the offer did).  TWO response shapes: the
   gated CONFIRM pair, and the ungated OFFER (§4.1.3's stripped-mode outcome, which carries no
   certified answer).  The receiver's acceptance is `RAccepts` over BOTH fields. *)
RConfirmResponse(r, ans, idn) ==
    /\ rAnswer'   = [rAnswer   EXCEPT ![r] = ans]
    /\ rIdentity' = [rIdentity EXCEPT ![r] = idn]
    /\ rAccepted' = [rAccepted EXCEPT ![r] = RAccepts(ans, idn)]
    /\ rAnsweredFenced' = [rAnsweredFenced EXCEPT ![r] = sFence[partMount]]
    /\ rDurableBefore'  = [rDurableBefore  EXCEPT ![r] = (rState[r] = "S1")]
    /\ rState' = [rState EXCEPT ![r] = "answered"]
    /\ UNCHANGED << rCommitted, rPublishes, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, partMount >>

RConfirm(r) ==
    /\ \/ rState[r] = "S1"
       \/ (SabotagePublishAfterConfirm /\ rState[r] = "init")
    /\ LET m == partMount IN
       \/ (* the CONFIRM response: the gated pair *)
          /\ RConfirmResponse(r, SenderAnswer(m), SenderIdentity(m))
          /\ sawBusyProven' = sawBusyProven \/
               H(SenderAnswer(m) = "proven" /\ (sPending[m] \/ sLeader[m]))
          /\ sawMidTenureProven' = sawMidTenureProven \/
               H(SenderAnswer(m) = "proven" /\ sLeader[m] /\ sTenureChunks[m] >= 1)
          /\ sawUnknown' = sawUnknown \/ H(SenderAnswer(m) = "unknown" /\ GateRefuses(m))
          /\ sawColdRefused' = sawColdRefused \/ H(SenderAnswer(m) = "unknown" /\ ~sRecovered[m])
          /\ sawCollision' = sawCollision \/
               H(SenderAnswer(m) = "proven" /\ m # MountA /\ sCacheRef[m] = Token
                 /\ NsOf(m) = NsOf(MountA))
          /\ UNCHANGED << sawProvenCommitted, sawLandedS2, sawChangedThenBytes, sawCorruptWindow >>
       \/ (* the OFFER response: ungated, and carrying NO certified answer (§4.1.3, §11 row 15) *)
          /\ RConfirmResponse(r, Absent, OfferIdentity(partMount))
          /\ UNCHANGED histVars

RPromoteCommit(r) ==
    /\ rState[r] = "answered" /\ rAccepted[r]
    /\ rState' = [rState EXCEPT ![r] = "S3"]
    /\ rCommitted'  = [rCommitted  EXCEPT ![r] = TRUE]
    /\ rPublishes'  = [rPublishes  EXCEPT ![r] = rPublishes[r] + 1]
    /\ sawProvenCommitted' = sawProvenCommitted \/ H(rDurableBefore[r])
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rDurableBefore, rAnsweredFenced, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, partMount >>
    /\ UNCHANGED << sawBusyProven, sawMidTenureProven, sawLandedS2, sawChangedThenBytes,
                    sawUnknown, sawColdRefused, sawCollision, sawCorruptWindow >>

(* Anything not accepted aborts, and the abort RELEASES the receiver's protection (a durable -1).
   Task 3 splits this into the `changed` byte-fetch arm and the `unknown` retry-later arm. *)
RAbort(r) ==
    /\ rState[r] = "answered" /\ ~rAccepted[r]
    /\ rState' = [rState EXCEPT ![r] = "done_retry"]
    /\ IF \E rec \in journal : rec.ns = r /\ rec.op = "add"
         THEN /\ nextId <= MaxId
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> r, blob |-> BlobA, src |-> r, op |-> "del"] }
              /\ nextId' = nextId + 1
         ELSE UNCHANGED logVars
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore, rAnsweredFenced,
                    rPublishes, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, histVars, partMount >>

(* TypeOK's NEGATIVE CONTROL.  Writes a value outside `rPublishes`' declared domain, so `TypeOK`
   must break -- which is what discharges the "green only after red" rule for the type invariant
   itself.  It is enabled by nothing else and appears in exactly one cfg. *)
TypeProbe(r) ==
    /\ SabotageTypeProbe
    /\ rPublishes' = [rPublishes EXCEPT ![r] = 3]
    /\ UNCHANGED << rState, rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore,
                    rAnsweredFenced, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, histVars, partMount >>

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

NoOp == UNCHANGED vars     \* house pattern; every cfg also sets CHECK_DEADLOCK FALSE.  It is ALSO
                           \* the model of every INERT `StillWedged` resolution (`:2062-2128`).

Next ==
    \/ \E m \in Mounts :
         \/ \E nb \in {Other, NoBinding} : \E k \in ChunkKinds : SenderAdmit(m, nb, k)
         \/ SenderArm(m) \/ SenderDurable(m) \/ SenderInstall(m) \/ SenderCloseTenure(m)
         \/ SenderPoison(m)
         \/ SenderUnresolvedLanded(m) \/ SenderUnresolvedNotLanded(m)
         \/ WedgeResolveInstall(m) \/ WedgeResolveRejected(m) \/ WedgeResolveCorrupted(m)
         \/ WedgeResolveStale(m) \/ FloorReconcile(m) \/ CorruptFenceReaction(m)
         \/ FenceLoss(m) \/ ForeignRemove(m) \/ EvictTable(m) \/ RecoverForAnswer(m)
    \/ \E r \in Receivers : RStage(r) \/ RConfirm(r) \/ RPromoteCommit(r) \/ RAbort(r)
                            \/ TypeProbe(r)
    \/ GFold \/ GSettle
    \/ NoOp

Spec == Init /\ [][Next]_vars

(* ---- invariants ----------------------------------------------------------------------------- *)

(* COMPLETE over every variable, deliberately: a type invariant that omits a variable stops
   protecting against a wrong `EXCEPT` or a dropped `UNCHANGED` in exactly the actions this model
   was written to get right.  `_sab_typeprobe` is its negative control. *)
TypeOK ==
    /\ round \in Rounds
    /\ present \in [Blobs -> BOOLEAN]
    /\ pendingDelete \subseteq condemned /\ condemned \subseteq Blobs
    /\ \A e \in folded : e.b \in Blobs /\ e.src \in Sources
    /\ cursor \in [Namespaces -> Ids]
    /\ gcPhase \in {"idle", "folded"}
    /\ journal \subseteq Records
    /\ nextId \in 1..(MaxId + 1)
    /\ sDurableRef \in [Mounts -> Bindings]
    /\ sCacheRef   \in [Mounts -> Bindings]
    /\ sTarget     \in [Mounts -> Bindings]
    /\ sKind       \in [Mounts -> ChunkKinds]
    /\ sPending    \in [Mounts -> BOOLEAN]
    /\ sLeader     \in [Mounts -> BOOLEAN]
    /\ sArmed      \in [Mounts -> BOOLEAN]
    /\ sApply      \in [Mounts -> ApplyStates]
    /\ sApplySeen  \in [Mounts -> ApplyStates]
    /\ sApplyOwed  \in [Mounts -> BOOLEAN]
    /\ sFloorCovers \in [Mounts -> BOOLEAN]
    /\ sWedge      \in [Mounts -> BOOLEAN]
    /\ sForeign    \in [Mounts -> BOOLEAN]
    /\ sFence      \in [Mounts -> BOOLEAN]
    /\ sRecovered  \in [Mounts -> BOOLEAN]
    /\ sChunks       \in [Mounts -> 0..MaxChunks]
    /\ sTenureChunks \in [Mounts -> 0..MaxChunks]
    /\ partMount \in Mounts
    /\ rState \in [Receivers -> {"init", "S1", "answered", "S0", "S2", "S3",
                                 "done_bytes", "done_retry"}]
    /\ rAnswer \in [Receivers -> {"none", "proven", "changed", "unknown", Absent}]
    /\ rIdentity \in [Receivers -> {Absent} \cup
                        { Validator(m, b) : m \in Mounts, b \in Bindings }]
    /\ rAccepted  \in [Receivers -> BOOLEAN]
    /\ rCommitted \in [Receivers -> BOOLEAN]
    /\ rDurableBefore  \in [Receivers -> BOOLEAN]
    /\ rAnsweredFenced \in [Receivers -> BOOLEAN]
    /\ rPublishes  \in [Receivers -> 0..2]
    /\ rUnresolved \in [Receivers -> BOOLEAN]
    /\ \A v \in {sawBusyProven, sawMidTenureProven, sawProvenCommitted, sawLandedS2,
                 sawChangedThenBytes, sawUnknown, sawColdRefused, sawCollision,
                 sawCorruptWindow} : v \in BOOLEAN

LiveBlobs == { b \in Blobs : present[b] }

(* THE THEOREM.  A relink COMMITTED on an ACCEPTED certificate whose activation (+1) was durable
   BEFORE the confirm never references a physically deleted blob.  Two deliberate choices:
     * the antecedent is `rAccepted`, not `rAnswer = "proven"` -- a sabotage that makes the receiver
       accept the WRONG thing must not escape the theorem by never producing the word `proven`;
     * `rCommitted` is not `rState = "S3"` -- a landed `Unresolved` promote (Task 3) publishes the
       relink while the receiver's own classification stays S2, and that publication is squarely
       inside what this theorem is about. *)
ConfirmedRelinkNeverDangles ==
    \A r \in Receivers :
        (rCommitted[r] /\ rAccepted[r] /\ rDurableBefore[r])
            => AdoptedBlobs \subseteq LiveBlobs

(* The antecedent-free form: broken by inverting the order, which leaves the guarded theorem
   vacuously satisfied.  Its red is `_sab_publishafterconfirm`, which lives in THIS task because a
   green may not list an invariant whose sabotage does not yet exist. *)
PromotedNeverDangles ==
    \A r \in Receivers : rCommitted[r] => AdoptedBlobs \subseteq LiveBlobs

(* SEAM §8 ROW S7, HALF ONE -- THE INTERVAL.  §5.1.2's requirement, stated as a model property:
   while an apply is OWED, the marker is not clean.  Note what it needs and does NOT need: the
   subject is this mount's OWN durable transaction (`sApplyOwed`), so no exclusion for the wedge and
   none for the fence is required, and none is added.  It holds through BOTH steps of the stale
   resolution -- `WedgeResolveStale` poisons while keeping the apply owed, and `FloorReconcile`
   retires the wedge while retaining both. *)
MarkerCoversDurableWindow ==
    \A m \in Mounts : sApplyOwed[m] => (sApply[m] # "clean")

(* SEAM §8 ROW S7, HALF TWO -- THE OBSERVATION.  Seam §6: "a reader acquiring AFTER the arm
   necessarily observes `ApplyPending`".  With the arm and the read both under `state_mutex`, the
   mutex supplies the happens-before and the two values coincide.  This is what the fix must
   deliver; `_sab_relaxedmarker` is the world in which it does not. *)
MarkerSeenMatchesMarker == \A m \in Mounts : sApplySeen[m] = sApply[m]

(* ---- witnesses (negated reachability; a TLC violation means the state IS reachable) --------- *)

(* THE FLIP'S NON-VACUITY, and the model-level statement of §2's availability fix: a `proven` is
   actually given while the lane is BUSY -- the exact state v11 rule 3 refused. *)
W_BusyLaneProven == ~sawBusyProven

(* §12.5 ii, and STRONGER than "two chunks exist": a `proven` is given mid-tenure, with the tenure
   OPEN and a chunk of THAT tenure already installed. *)
W_MidTenureProven == ~sawMidTenureProven

W_ProvenCommitted == ~sawProvenCommitted    \* non-vacuity of the theorem's antecedent
W_BlobDeleted == \A b \in Blobs : present[b] \* non-vacuity of the consequent: GC really deletes

(* `CasRefLedger.cpp:1927-1934` + `:2050-2060`: the fence-live, marker-clean, WEDGED window between
   a Corrupted resolution and the fence-closing reaction really exists.  Task 3's `_sab_nowedge`
   depends on it, so it is proven reachable rather than assumed. *)
W_CorruptWindow == ~sawCorruptWindow

=============================================================================
```

- [ ] **Step 7: Write the runner with expected verdicts, per-run isolation and markers.**

Create `docs/superpowers/models/run_relinkreoffer.sh`, `chmod +x`. Copy the structure of `run_refcatalog.sh` — same result extraction, same `violation:<NAME>` assertion, same `ALL EXPECTATIONS MET` / exit code — with these differences: `MODULE=CaRelinkReofferCore`, and the isolation/marker block below, which exists because two concurrent batteries must not be able to delete each other's metadir or overwrite each other's log:

```bash
RUN_ID="${RUN_ID:-$(date +%Y%m%dT%H%M%S)-$$}"
LOCK=../../../tmp/.relinkreoffer.lock
: > "$LOCK" 2>/dev/null || true
exec 9>"$LOCK"
flock -n 9 || { echo "another relinkreoffer battery is running (lock: $LOCK)" >&2; exit 4; }
RUNDIR=../../../tmp/tlc-runs/relinkreoffer/$RUN_ID
METAROOT=../../../tmp/tlc-meta-relinkreoffer/$RUN_ID
mkdir -p "$RUNDIR" "$METAROOT"
echo "RUN_ID=$RUN_ID  logs=$RUNDIR"
```

and, per config:

```bash
  log="$RUNDIR/tlc_${name}.log"
  meta="$METAROOT/$name"
  { echo "=== TLC BEGIN ${MODULE}_${name} $(date -Is) ==="; } > "$log"
  start=$SECONDS
  /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
    -metadir "$meta" -workers "${TLC_WORKERS:-1}" -config "$cfg" "$MODULE.tla" >>"$log" 2>&1
  rc=$?
  elapsed=$((SECONDS - start))
  { echo "=== TLC END ${MODULE}_${name} rc=${rc} $(date -Is) ==="; } >> "$log"
  ln -sfn "tlc-runs/relinkreoffer/$RUN_ID/tlc_${name}.log" \
          "../../../tmp/tlc_${MODULE}_${name}.log"
```

The stable symlink is a convenience for interactive work; **RESULTS must cite `$RUNDIR/tlc_<cfg>.log`**, never the symlink, so a recorded row can always be re-read. The header comment lists every config with its expectation and the one-line reason, in the `run_refcatalog.sh` style, and states the `-workers 1` rationale.

Its `CONFIGS` array for THIS task is exactly:

```bash
CONFIGS=(
  "sab_typeprobe             violation  TypeOK"
  "sab_noapplypending        violation  ConfirmedRelinkNeverDangles"
  "sab_noapplypending_window violation  MarkerCoversDurableWindow"
  "sab_relaxedmarker         violation  MarkerSeenMatchesMarker"
  "sab_publishafterconfirm   violation  PromotedNeverDangles"
  "v11_baseline              green      -"
  "ctl_v11nomarker           green      -"
  "sab_stalecache            green      -"
  "witness_busylane          violation  W_BusyLaneProven"
  "witness_midtenure         violation  W_MidTenureProven"
  "witness_proven            violation  W_ProvenCommitted"
  "witness_delete            violation  W_BlobDeleted"
  "witness_corruptwindow     violation  W_CorruptWindow"
)
```

Tasks 2 and 3 extend this array; the sabotage-before-green ordering is preserved as they do.

- [ ] **Step 8: Write the five failing-first configs.**

The full constants block, given once — every later cfg in this plan is a delta against it:

```
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
    SabotageNoApplyPending = FALSE
    SabotageRelaxedMarker = FALSE
    SabotageNoPoison = FALSE
    SabotageNoWedge = FALSE
    SabotageNoFence = FALSE
    SabotageNoRowExact = FALSE
    SabotageBareValidator = FALSE
    SabotageNoDiskQual = FALSE
    SabotageInferAnswer = FALSE
    SabotageSkipIdentity = FALSE
    SabotagePublishAfterConfirm = FALSE
    SabotageS2ByteFetch = FALSE
    SabotageTypeProbe = FALSE
CHECK_DEADLOCK FALSE
```

| cfg | delta | `INVARIANTS` | header states |
| --- | --- | --- | --- |
| `sab_typeprobe` | `SabotageTypeProbe = TRUE` | `TypeOK` | `TypeOK`'s negative control: an action writes `rPublishes := 3`, outside its declared `0..2`. Its red is what makes every green's `TypeOK` evidence rather than decoration, and it is why type invariants need no exemption from the green-only-after-red rule. |
| `sab_noapplypending` | `SabotageNoApplyPending = TRUE` | `ConfirmedRelinkNeverDangles` | §12.3 step 2, first half: the marker is never armed while the design's deletion of v11 rule 3's lane-quiescence terms stands. TLC MUST report a counterexample — the confirm reads a committed row that lags a DURABLE removal, exactly the v11 `_sab_stalecache` trace. This is the guard doing real work. |
| `sab_noapplypending_window` | `SabotageNoApplyPending = TRUE` | `MarkerCoversDurableWindow` | The same toggle's SECOND consequence, and the direct statement of §5.1.2's requirement on the seam fix. Seam §8 row S7, interval half. |
| `sab_relaxedmarker` | `SabotageRelaxedMarker = TRUE` | `MarkerSeenMatchesMarker` | Seam §8 row S7, OBSERVATION half: the arm stores `ApplyPending` and the store is not published, which is what a relaxed `compare_exchange_strong` called with no lock held permits (`CasRefLedger.cpp:1681` called at `:2808`). **Distinct from `_sab_noapplypending` by construction, not by trace comparison:** there the marker's stored value never moves, here it does, and the two configs break DIFFERENT invariants. |
| `sab_publishafterconfirm` | `SabotagePublishAfterConfirm = TRUE` | `PromotedNeverDangles` | The design's ORDER is inverted — confirm and promote BEFORE the receiver's `+1` is durable. The guarded theorem stays vacuously satisfied (`rDurableBefore` is FALSE), so the antecedent-free form is what must break. **It lives in Task 1, not Task 3, because Task 1's and Task 2's greens list `PromotedNeverDangles` and a green may not rest on an invariant whose red does not yet exist.** The alternative — dropping the invariant from the earlier greens and adding it in Task 3 — was rejected because it would run the 2×2 gate without the antecedent-free theorem, weakening the very cell that decides the design. §core-idea survives verbatim and this is its check. |

- [ ] **Step 9: Run the five sabotages FIRST. All five MUST be red.**

```bash
bash /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models/run_relinkreoffer.sh
```
Expected: five `violation:<the named invariant>` PASS rows; the eight not-yet-written configs report `error` — expected, and why they are written next.

Read each counterexample and check it against the code, not just against the expectation:
- `sab_noapplypending`'s trace must go `Admit(tracked) → Arm → Durable → (GFold → GSettle)×3 → Stage → Confirm → PromoteCommit` — note the alternation: consecutive `GSettle`s are disabled by `gcPhase`, so each graduation phase costs a fold and a settle. The blob is deleted BEFORE the receiver's `+1` and the answer comes from the stale row. Minimum depth ≈ 12.
- `sab_relaxedmarker`'s trace must reach a state with `sApply = "pending"` and `sApplySeen = "clean"` — depth 2 or 3. If it reports both values clean, the sabotage is wired into the wrong branch of `SenderArm`.
- `sab_publishafterconfirm`'s trace must confirm from `"init"` and stage from `"S3"`. Minimum depth ≈ 10.

If any sabotage comes back GREEN, fix the MODEL (never the invariant) and re-run.

- [ ] **Step 10: Write the three 2×2 configs and run THE GATE.**

Three configs, deltas against step 8's block:

| cfg | `SabotageStaleCache` | `SabotageNoApplyPending` | `INVARIANTS` | expected |
| --- | --- | --- | --- | --- |
| `v11_baseline` | `FALSE` | `FALSE` | `TypeOK` `ConfirmedRelinkNeverDangles` `PromotedNeverDangles` `MarkerCoversDurableWindow` `MarkerSeenMatchesMarker` | green |
| `ctl_v11nomarker` | `FALSE` | `TRUE` | `TypeOK` `ConfirmedRelinkNeverDangles` `PromotedNeverDangles` | green |
| `sab_stalecache` | `TRUE` | `FALSE` | `TypeOK` `ConfirmedRelinkNeverDangles` `PromotedNeverDangles` `MarkerCoversDurableWindow` `MarkerSeenMatchesMarker` | **green — THE FLIP** |

`ctl_v11nomarker` omits both marker invariants: with the marker unarmed they are FALSE by construction, and step 9 already proved the first one red. Omitting them here is scoping, not weakening — put that sentence in the cfg header. Its green is not theorem-vacuous: `Stage → Confirm → PromoteCommit` makes `rDurableBefore` true, and the v11 terms refuse throughout the durable-but-unapplied window because `SenderInstall` preserves `sLeader` until `SenderCloseTenure`.

`_sab_stalecache`'s header must carry:

```
\* *** THE GATE (spec §12.3 step 3). ***  v11 rule 3's lane-quiescence terms are DROPPED -- which is
\* what this design ships -- and the apply-pending marker is INTACT.  In `CaRelinkConfirmCore` the
\* same-named cfg MUST be RED; here it MUST be GREEN, and that flip is the proof that the terms this
\* design deletes were redundant rather than load-bearing.  If TLC reports a violation, the design is
\* wrong and MUST NOT BE IMPLEMENTED.
```

Run the harness again. Expected: three greens.

**THE VERDICT.** GREEN → the gate's first half is met; continue. RED → **STOP**. Do not start Task 2. Preserve the run's log directory, jump to Task 5, write RESULTS with `RELINK TLA GATE: FAIL`, name the violated invariant, quote the counterexample, and report the design as refuted per §1 gate 2 and §12.3 step 3.

- [ ] **Step 11: Write the five witnesses and prove the flip is not vacuous.**

All five use the DESIGN's settings (`SabotageStaleCache = TRUE`, `SabotageNoApplyPending = FALSE`, `SabotageRelaxedMarker = FALSE`) — a witness run under different settings proves nothing about the config it de-vacuums — and all five set `TrackHistory = TRUE`. One negated invariant each:

| cfg | `INVARIANT` | what its violation proves |
| --- | --- | --- |
| `witness_busylane` | `W_BusyLaneProven` | a `proven` is actually given while `sPending \/ sLeader`. **This is what makes `_sab_stalecache` GREEN mean something.** |
| `witness_midtenure` | `W_MidTenureProven` | a `proven` is given with the tenure OPEN and a chunk of that tenure already installed — §12.5 ii, and reachable only because `SenderAdmit` has an `"unrelated"` kind |
| `witness_proven` | `W_ProvenCommitted` | the theorem's antecedent is reachable |
| `witness_delete` | `W_BlobDeleted` | GC physically deletes, so the consequent is not trivially true |
| `witness_corruptwindow` | `W_CorruptWindow` | the fence-live, marker-clean, wedged window between a Corrupted resolution and the fence-closing reaction is reachable |

Run the harness. Expected: 13 rows, `ALL EXPECTATIONS MET`.

Two witnesses are gate-critical, and a GREEN on either is to be treated exactly as a RED `_sab_stalecache` — stop and report:
- `witness_busylane` green ⇒ the busy-lane `proven` is unreachable, so the flip's green is vacuous.
- `witness_midtenure` green ⇒ the multi-chunk tenure is unreachable and the flip is the one-transaction artefact §12.5 ii warns about. Most likely causes, in order: `SenderAdmit`'s `"tracked"` guard leaking onto the `"unrelated"` kind; `sTenureChunks` not reset by `SenderCloseTenure`; `sTenureChunks` not incremented by `SenderInstall`. Fix the model and re-run steps 9–11 in order.

- [ ] **Step 12: Audit every sender action against its citation, then commit.**

Before committing, walk the **eighteen** actions of `Next`'s mount disjunction — `SenderAdmit`, `SenderArm`, `SenderDurable`, `SenderInstall`, `SenderCloseTenure`, `SenderPoison`, `SenderUnresolvedLanded`, `SenderUnresolvedNotLanded`, `WedgeResolveInstall`, `WedgeResolveRejected`, `WedgeResolveCorrupted`, `WedgeResolveStale`, `FloorReconcile`, `CorruptFenceReaction`, `FenceLoss`, `ForeignRemove`, `EvictTable`, `RecoverForAnswer` — and confirm each matches the code site named in its comment. Then check the mapping block covers every arm of `resolveWedgeOnce`, including the two that map to NO action.

Six of these have been caught wrong by review at least once; verify them individually:

1. `SenderUnresolvedLanded` and `SenderUnresolvedNotLanded` leave `sApply` at `"pending"` (`:3134-3145`).
2. `WedgeResolveCorrupted` clears the marker and KEEPS `sWedge` (`:1927-1934`).
3. `CorruptFenceReaction` is a SEPARATE action from it (`:2050-2060`).
4. `WedgeResolveStale` sets `sFloorCovers`, poisons, and KEEPS `sWedge` (`:1936-1950`).
5. `FloorReconcile` exists, retires the wedge, installs NOTHING, and retains both `Poisoned` and `sApplyOwed` (`:1758-1766`).
6. `WedgeResolveInstall` is guarded by `~sFloorCovers[m]` AND `sApply[m] # "poisoned"`, so a poisoned-to-clean install is unrepresentable.

Finally, `grep -c 'sFloorCovers' CaRelinkReofferCore.tla` and confirm every action either writes it or lists it in an `UNCHANGED` — TLC will report a missing assignment, but an accidental `UNCHANGED` omission that happens to be in a tuple will not be caught for you.

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add docs/superpowers/models/CaRelinkReofferCore.tla \
        docs/superpowers/models/CaRelinkReofferCore_sab_typeprobe.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_noapplypending.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_noapplypending_window.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_relaxedmarker.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_publishafterconfirm.cfg \
        docs/superpowers/models/CaRelinkReofferCore_v11_baseline.cfg \
        docs/superpowers/models/CaRelinkReofferCore_ctl_v11nomarker.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_stalecache.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_busylane.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_midtenure.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_proven.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_delete.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_corruptwindow.cfg \
        docs/superpowers/models/run_relinkreoffer.sh
git commit -m "ca: tla — CaRelinkReofferCore: apply-marker refinement (stored vs observed, full wedge lifecycle) and the 2x2 gate (_sab_stalecache FLIPS GREEN)"
```

---

### Task 2: The cross-mount collision, and the answer/identity split

§12.5 row i, verbatim: *"MODEL IT, and specifically: two mounts with EQUAL `root_namespace` and DIFFERENT `disk_name` ... Without that shape a model can pass while qualifying by namespace only."* Plus §4.2's requirement that the certified answer and the returned identity agree independently — which lives here because it is the same subject: what the validator is, and who compares it.

**Files:**
- Modify: `docs/superpowers/models/CaRelinkReofferCore.tla` (add `PartMove` and `W_CollisionReached`; wire `PartMove` into `Next`)
- Create: `..._sab_barevalidator.cfg`, `..._sab_nodiskqualification.cfg`, `..._sab_inferanswer.cfg`, `..._ctl_distinctns.cfg`, `..._ctl_skipidentity.cfg`, `..._witness_collisionreached.cfg`
- Modify: `docs/superpowers/models/run_relinkreoffer.sh`
- Read first: spec §3, §4.1 (the validator paragraph), §4.1.3, §4.2's answer table, §4.4, §11 rows 15 and 17, §12.5 row i.

**Interfaces:**
- Consumes: Task 1's `Validator(m, b)`, `HeldValidator`, `NsOf(m)`, `BlobOf(m)`, `partMount`, `SenderAnswer(m)`, `SenderIdentity(m)`, `OfferIdentity(m)`, `RAccepts(ans, idn)`, `sawCollision`, `SecondMount`, `EqualNamespaces`, `SabotageBareValidator`, `SabotageNoDiskQual`, `SabotageInferAnswer`, `SabotageSkipIdentity`.
- Produces: `W_CollisionReached`; the two reds §12.5 row i names by name; the answer/identity scoping result.

---

- [ ] **Step 1: Add the part move and the collision witness.**

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

Add `\/ PartMove` to `Next`, and beside the other witnesses:

```tla
(* §12.5 i's non-vacuity: the collision STATE is reachable -- a `proven` emitted by a mount that is
   NOT the offering mount, holding the SAME ManifestRef text, under an EQUAL namespace string.
   Without this, the two validator reds could be red for some other reason and `_ctl_distinctns`
   could be green for free. *)
W_CollisionReached == ~sawCollision
```

- [ ] **Step 2: Write the three must-red configs.**

Deltas against Task 1 step 8's block. `INVARIANTS ConfirmedRelinkNeverDangles` in all three. **The mount setting differs between them, and deliberately:** the two validator sabotages need the second mount because a cross-mount collision is what they are about; `_sab_inferanswer` needs no second mount at all, because an ungated offer response is a single-mount hazard, and running it with one mount keeps its state space small.

`sab_barevalidator` — `SecondMount = TRUE`, `EqualNamespaces = TRUE`, `SabotageBareValidator = TRUE`:

```
\* SABOTAGE (§12.5 i): the validator is the bare ManifestRef, with no qualification at all -- the
\* B1 collision shape.  `CasTypes.h:131-134`: "Two namespaces may legally carry the same ManifestRef
\* tuple without addressing the same object."  TLC MUST report a ConfirmedRelinkNeverDangles
\* counterexample: mount A's binding is removed and its blob deleted; the part MOVES to mount B,
\* which still binds the same ManifestRef TEXT over a DIFFERENT object; the bare comparison matches
\* and the receiver promotes over blobs nobody is protecting.
```

`sab_nodiskqualification` — `SecondMount = TRUE`, `EqualNamespaces = TRUE`, `SabotageNoDiskQual = TRUE`:

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

`sab_inferanswer` — `SecondMount = FALSE`, `SabotageInferAnswer = TRUE`:

```
\* SABOTAGE (§4.1.3 defence 1): the receiver INFERS the answer from a matching identity instead of
\* requiring the explicit `content_addressed_answer` cookie.  "The offer response carries the same
\* validator, and the offer path is not gated" -- `getRelinkOffer` applies none of §4.2's fence,
\* wedge or apply-state checks, because an offer is a proposal, not a certificate.  So a stripped
\* mode parameter (§11 row 15) makes the sender reply with an ordinary OFFER whose identity may
\* equal the held validator, and a receiver comparing validators alone promotes on an UNGATED
\* resolve.  ONE MOUNT is enough: this hazard is about the offer path being ungated, not about
\* mounts.  TLC MUST report a ConfirmedRelinkNeverDangles counterexample.
```

- [ ] **Step 3: Run all three. All three MUST be red — this is the failing-first step.**

Add to `CONFIGS`, immediately after `sab_relaxedmarker`:

```bash
  "sab_barevalidator        violation  ConfirmedRelinkNeverDangles"
  "sab_nodiskqualification  violation  ConfirmedRelinkNeverDangles"
  "sab_inferanswer          violation  ConfirmedRelinkNeverDangles"
```

Run. Expected: three `violation:ConfirmedRelinkNeverDangles`.

If `sab_nodiskqualification` is GREEN the collision is unreachable: check, in this order, that `PartMove` is in `Next`; that mount B's row holds `Token` (`Init` over `Mounts`); that `MaxId` admits the trace (A's removal and the receiver's `+1` — two ids and three GC rounds suffice). If `sab_inferanswer` is GREEN, the offer-response arm of `RConfirm` is unreachable — most likely the disjunction collapsed because both arms were written into one conjunct list. Fix the model; do not touch the invariant.

- [ ] **Step 4: Write the two controls, whose GREEN is the result.**

`ctl_distinctns` — identical to `sab_nodiskqualification` except `EqualNamespaces = FALSE`; `INVARIANTS TypeOK ConfirmedRelinkNeverDangles PromotedNeverDangles`:

```
\* CONTROL for _sab_nodiskqualification.  The SAME sabotage -- `disk_name` dropped -- with the two
\* mounts carrying DIFFERENT namespace strings.  Expected GREEN: the namespace alone separates them,
\* so nothing collides.  That is the whole point of §12.5 i's "specifically": the red next door is
\* caused by the EQUAL-namespace configuration, not by the second mount existing.  This control
\* proves only the restricted claim -- namespace qualification suffices WHEN namespaces differ.
```

`ctl_skipidentity` — `SabotageSkipIdentity = TRUE`, `SecondMount = TRUE`, `EqualNamespaces = TRUE`; same three invariants:

```
\* CONTROL, and *** GREEN IS THE RESULT, NOT A DISAPPOINTMENT. ***  The receiver trusts `proven` and
\* SKIPS §4.4 condition 4 (the identity comparison).  Expected GREEN, and the green is the finding:
\* under a faithful sender the certified answer and the identity match are computed from the SAME
\* committed row, so they are one fact and the receiver's own comparison is redundant.  What it
\* defends is a response that did not come from THIS confirm -- a replay, or an offer mistaken for a
\* confirm -- and that is `FreshCertifiedResponse`'s scope (§11 rows 15, 16), discharged by test
\* rather than by this model.  Recorded as a MEASURED scoping result so that nobody later reads
\* §4.2's "each is separately sufficient to fail closed" as an unmodelled gap: half of it IS
\* modelled (`_sab_inferanswer`, red), and this config is why the other half is not.
\* NOTE: the fields are still DECOUPLED in the model -- sender answer, returned identity and
\* receiver acceptance are three separate objects -- which is what makes this measurement possible
\* at all.  Do not re-fuse them.
```

- [ ] **Step 5: Write the collision witness.**

`witness_collisionreached`: `SecondMount = TRUE`, `EqualNamespaces = TRUE`, `TrackHistory = TRUE`, `SabotageStaleCache = TRUE`, `SabotageNoDiskQual = TRUE` — the witness must run under the settings whose red it de-vacuums. `INVARIANT W_CollisionReached`.

- [ ] **Step 6: Run the battery so far.**

Extend `CONFIGS` with `"ctl_distinctns green -"` and `"ctl_skipidentity green -"` among the greens, and `"witness_collisionreached violation W_CollisionReached"` among the witnesses. Run. Expected: 19 rows, `ALL EXPECTATIONS MET`.

- [ ] **Step 7: Commit.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add docs/superpowers/models/CaRelinkReofferCore.tla \
        docs/superpowers/models/CaRelinkReofferCore_sab_barevalidator.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_nodiskqualification.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_inferanswer.cfg \
        docs/superpowers/models/CaRelinkReofferCore_ctl_distinctns.cfg \
        docs/superpowers/models/CaRelinkReofferCore_ctl_skipidentity.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_collisionreached.cfg \
        docs/superpowers/models/run_relinkreoffer.sh
git commit -m "ca: tla — cross-mount collision (equal-ns/different-disk) + the answer/identity split: _sab_nodiskqualification and _sab_inferanswer red, both controls green"
```

---

### Task 3: Re-derivation against the new rule set, and the four-state receiver

§12.4: the fence moves first, `No` becomes an authoritative outcome with its own successor action, the model must show the byte fetch following a `No` cannot publish twice, and the `_witness_confirmno` / `_witness_confirmunknown` witnesses must be re-derived because the states they prove reachable are no longer the same states.

**Files:**
- Modify: `docs/superpowers/models/CaRelinkReofferCore.tla` (split `RAbort`; add S0/S2 with a landed-`Unresolved` publication; add `NeverPublishedTwice`, `ChangedImpliesFenced` and four witnesses)
- Create: `..._sab_nofence.cfg`, `..._sab_nofence_changed.cfg`, `..._sab_nopoison.cfg`, `..._sab_nowedge.cfg`, `..._sab_norowexact.cfg`, `..._sab_s2bytefetch.cfg`, `..._v12_design_full.cfg`, `..._v12_coldanswer.cfg`, `..._witness_changed.cfg`, `..._witness_unknown.cfg`, `..._witness_budgetunknown.cfg`, `..._witness_landeds2.cfg`
- Modify: `docs/superpowers/models/run_relinkreoffer.sh`
- Read first: spec §4.2, §4.4, §6.1–§6.4, §12.4.

**Interfaces:**
- Consumes: everything from Tasks 1–2.
- Produces: `NeverPublishedTwice`, `ChangedImpliesFenced`, `W_ChangedThenBytes`, `W_UnknownRefusal`, `W_ColdRefused`, `W_LandedS2`; and `_v12_design_full`, the single green in which the whole universe is checked together.

---

- [ ] **Step 1: Replace `RAbort` with the answer-specific successors, and add S0/S2.**

Delete `RAbort` and insert:

```tla
(* §4.4: `changed` with a present, different identity => abort the prepared relink, then FETCH THE
   BYTES FROM THE SAME SENDER.  Today that is forbidden; under fence-first there is no doubt left to
   protect against, because rule 1 established the answering mount held its fence. *)
RChangedThenBytes(r) ==
    /\ rState[r] = "answered" /\ ~rAccepted[r] /\ rAnswer[r] = "changed"
    /\ rState' = [rState EXCEPT ![r] = "done_bytes"]
    /\ rPublishes' = [rPublishes EXCEPT ![r] = rPublishes[r] + 1]
    /\ sawChangedThenBytes' = sawChangedThenBytes \/ H(TRUE)
    /\ IF \E rec \in journal : rec.ns = r /\ rec.op = "add"
         THEN /\ nextId <= MaxId
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> r, blob |-> BlobA, src |-> r, op |-> "del"] }
              /\ nextId' = nextId + 1
         ELSE UNCHANGED logVars
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore, rAnsweredFenced,
                    rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, partMount >>
    /\ UNCHANGED << sawBusyProven, sawMidTenureProven, sawProvenCommitted, sawLandedS2,
                    sawUnknown, sawColdRefused, sawCollision, sawCorruptWindow >>

(* §4.4 and §6.2: everything else is one outcome -- abort, then throw the retry-later
   NETWORK_ERROR.  NO byte re-request. *)
RUnknownThenRetry(r) ==
    /\ rState[r] = "answered" /\ ~rAccepted[r] /\ rAnswer[r] # "changed"
    /\ rState' = [rState EXCEPT ![r] = "done_retry"]
    /\ IF \E rec \in journal : rec.ns = r /\ rec.op = "add"
         THEN /\ nextId <= MaxId
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> r, blob |-> BlobA, src |-> r, op |-> "del"] }
              /\ nextId' = nextId + 1
         ELSE UNCHANGED logVars
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore, rAnsweredFenced,
                    rPublishes, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, histVars, partMount >>

(* §6.2: `promote` has THREE outcomes.  `MechanismFallbackAllowed` was rejected BEFORE its ref-log
   append, so "nothing was committed" is PROVEN -- state S0, whose action is a byte fetch. *)
RPromoteFallback(r) ==
    /\ rState[r] = "answered" /\ rAccepted[r]
    /\ rState' = [rState EXCEPT ![r] = "S0"]
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore, rAnsweredFenced,
                    rPublishes, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, histVars, partMount >>

S0Bytes(r) ==
    /\ rState[r] = "S0"
    /\ rState' = [rState EXCEPT ![r] = "done_bytes"]
    /\ rPublishes' = [rPublishes EXCEPT ![r] = rPublishes[r] + 1]
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore, rAnsweredFenced,
                    rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, histVars, partMount >>

(* §6.3: `Unresolved` -- the ref-log append was attempted and came back without a verdict, so the
   receiver's ref MAY be committed.  Modelled as exactly that: an ambiguity resolved LATER, either
   way, by something the receiver does not control. *)
RPromoteUnresolved(r) ==
    /\ rState[r] = "answered" /\ rAccepted[r]
    /\ rState' = [rState EXCEPT ![r] = "S2"]
    /\ rUnresolved' = [rUnresolved EXCEPT ![r] = TRUE]
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore, rAnsweredFenced,
                    rPublishes >>
    /\ UNCHANGED << gcVars, senderVars, logVars, histVars, partMount >>

(* THE APPEND HAD LANDED: the relink IS committed, even though the receiver's own classification is
   still S2.  `rCommitted` is what both dangle theorems quantify over, which is why this
   publication cannot escape them -- an earlier draft of this model tested `rState = "S3"` and let
   exactly this trace through. *)
S2ResolveLanded(r) ==
    /\ rUnresolved[r]
    /\ rUnresolved' = [rUnresolved EXCEPT ![r] = FALSE]
    /\ rCommitted'  = [rCommitted  EXCEPT ![r] = TRUE]
    /\ rPublishes'  = [rPublishes  EXCEPT ![r] = rPublishes[r] + 1]
    /\ sawLandedS2' = sawLandedS2 \/ H(TRUE)
    /\ UNCHANGED << rState, rAnswer, rIdentity, rAccepted, rDurableBefore, rAnsweredFenced >>
    /\ UNCHANGED << gcVars, senderVars, logVars, partMount >>
    /\ UNCHANGED << sawBusyProven, sawMidTenureProven, sawProvenCommitted, sawChangedThenBytes,
                    sawUnknown, sawColdRefused, sawCollision, sawCorruptWindow >>

S2ResolveNotLanded(r) ==
    /\ rUnresolved[r]
    /\ rUnresolved' = [rUnresolved EXCEPT ![r] = FALSE]
    /\ IF \E rec \in journal : rec.ns = r /\ rec.op = "add"
         THEN /\ nextId <= MaxId
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> r, blob |-> BlobA, src |-> r, op |-> "del"] }
              /\ nextId' = nextId + 1
         ELSE UNCHANGED logVars
    /\ UNCHANGED << rState, rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore,
                    rAnsweredFenced, rPublishes >>
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
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore, rAnsweredFenced,
                    rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, histVars, partMount >>
```

Extend the receiver disjunct of `Next`:

```tla
    \/ \E r \in Receivers :
         \/ RStage(r) \/ RConfirm(r) \/ TypeProbe(r)
         \/ RPromoteCommit(r) \/ RPromoteFallback(r) \/ RPromoteUnresolved(r)
         \/ RChangedThenBytes(r) \/ RUnknownThenRetry(r)
         \/ S0Bytes(r) \/ S2Retry(r) \/ S2ResolveLanded(r) \/ S2ResolveNotLanded(r)
```

- [ ] **Step 2: Add the two new invariants and the four witnesses.**

```tla
(* §12.4: "the model must show that the byte fetch following a `No` cannot publish twice".  Counted,
   not asserted structurally: a relink promote that COMMITS publishes once; an ambiguous promote that
   turns out to have landed publishes once; a byte fetch publishes once.  Two is the defect. *)
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

(* Re-derived from v11's `_witness_confirmunknown`: `unknown` now folds a DIFFERENT rule set -- the
   fence is hoisted out of it and the lane-quiescence terms are gone.  The flag is set only for a
   GATE refusal, not for the offer-response arm, so the witness stays precise. *)
W_UnknownRefusal == ~sawUnknown

(* §12.4's revisit of rule 2: recovery is MANDATORY on the answer path, so the cold refusal is now
   on the path every cold answer takes rather than one nobody takes. *)
W_ColdRefused == ~sawColdRefused

(* The landed-Unresolved publication -- the trace both dangle theorems used to miss -- is reachable,
   so extending them to `rCommitted` is not a vacuous strengthening. *)
W_LandedS2 == ~sawLandedS2
```

- [ ] **Step 3: Write the six must-red configs and run them FIRST.**

Deltas against Task 1 step 8's block, i.e. `SabotageStaleCache = TRUE` (every retained rule is re-derived against the SHIPPED gate, not against v11's) and everything else honest except the row's own toggle. `_sab_publishafterconfirm` is NOT here — it landed in Task 1, where the invariant it protects is first listed by a green.

| cfg | delta | `INVARIANTS` | one-line intent for the cfg header |
| --- | --- | --- | --- |
| `sab_nofence` | `SabotageNoFence = TRUE` | `ConfirmedRelinkNeverDangles` | rule 1, hoisted first, still load-bearing: a deposed instance answers `proven` about a namespace somebody else now writes |
| `sab_nofence_changed` | `SabotageNoFence = TRUE` | `ChangedImpliesFenced` | the SECOND consequence of the same toggle: a fenced-out mount emits an authoritative `No`, which is what authorizes a same-sender byte fetch |
| `sab_nopoison` | `SabotageNoPoison = TRUE` | `ConfirmedRelinkNeverDangles` | rule 4's Poisoned arm: after a poisoned apply the lane IS quiescent and the marker is not pending, so only this arm sees the permanently stale row. **Two routes reach it and both are real — `SenderPoison` (the apply threw) and `WedgeResolveStale` (durable, unrecordable, floor-raised); note which one the trace takes.** |
| `sab_nowedge` | `SabotageNoWedge = TRUE` | `ConfirmedRelinkNeverDangles` | rule 3, and its counterexample must run through the state `CasRefLedger.cpp:1927-1934` singles out: a Corrupted resolution has cleared the marker and KEPT the wedge, the fence-closing reaction has not run yet, and the foreign occupant that proved exclusivity was breached is free to remove the binding. In that window rule 3 is the only guard. **Check the trace: if it does not contain `WedgeResolveCorrupted`, the red is coming from somewhere else and the sabotage is not testing what it claims.** |
| `sab_norowexact` | `SabotageNoRowExact = TRUE` | `ConfirmedRelinkNeverDangles` | rule 5's exactness (v11's `_sab_nogate1`, re-derived): presence instead of validator equality is an ABA |
| `sab_s2bytefetch` | `SabotageS2ByteFetch = TRUE` | `NeverPublishedTwice` | §6.3: S2 is the one state where a byte fetch double-publishes. The necessity half of `UnresolvedPromoteNeverBytes` |

Add all six to `CONFIGS` in the sabotage block and run. Expected: six violations, each matching its named invariant. A red on a DIFFERENT invariant is a FAIL per the Global Constraints — investigate the trace, do not relabel the cfg.

- [ ] **Step 4: Write the two remaining greens.**

`v12_design_full` — **the config in which the whole universe is checked together**, so no property is only ever checked in isolation. Deltas: `SecondMount = TRUE`, `EqualNamespaces = TRUE`, `SabotageStaleCache = TRUE`, everything else honest. `INVARIANTS`: `TypeOK` `ConfirmedRelinkNeverDangles` `PromotedNeverDangles` `MarkerCoversDurableWindow` `MarkerSeenMatchesMarker` `NeverPublishedTwice` `ChangedImpliesFenced`. Header:

```
\* THE DESIGN, WHOLE.  Two mounts (equal namespace, different disk), the marker armed, published and
\* cleared, the full wedge lifecycle including both steps of the stale resolution, a tenure that
\* commits several chunks of two kinds, the four-state receiver with the decoupled answer/identity
\* pair, and v11 rule 3's terms deleted -- every mechanism switched on at once.  Every other green
\* isolates one thing; this is the only place they are checked TOGETHER, which is what rules out a
\* property that holds only when its neighbours are off.
```

`v12_coldanswer` — identical except `SecondMount = FALSE` and `ModelColdTable = TRUE`, same seven invariants. Header: rule 2 is now on the answer path (§4.3's mandatory recovery, §12.4's revisit); this config runs eviction and peer-initiated recovery, and it is also the only place where the poisoned/floor-reconciled lane has an exit — `EvictTable` then `RecoverForAnswer`, which is the model of "until a recovery re-derives it from the log".

Run. Expected: both green. If `v12_design_full` exceeds ~10 min at `-workers 1`, drop `MaxId` to 5, then `MaxRound` to 4 — and record the bound in RESULTS. Never drop an invariant to make it finish.

- [ ] **Step 5: Write the four witnesses.**

All four: `TrackHistory = TRUE`, `SabotageStaleCache = TRUE`, everything else honest.

| cfg | extra deltas | `INVARIANT` |
| --- | --- | --- |
| `witness_changed` | `SecondMount = TRUE`, `EqualNamespaces = TRUE` | `W_ChangedThenBytes` |
| `witness_unknown` | — | `W_UnknownRefusal` |
| `witness_budgetunknown` | `ModelColdTable = TRUE` | `W_ColdRefused` |
| `witness_landeds2` | — | `W_LandedS2` |

`witness_changed` uses the second mount because the cross-mount route is the one row 17 exercises and the one that must be shown live. Each header states, in one sentence, which v11 witness it re-derives (or which theorem extension it de-vacuums) and why the state is not the same one.

- [ ] **Step 6: Run the whole battery and confirm 31/31.**

Final `CONFIGS` array — sabotages first, then greens, then witnesses:

```bash
CONFIGS=(
  "sab_typeprobe             violation  TypeOK"
  "sab_noapplypending        violation  ConfirmedRelinkNeverDangles"
  "sab_noapplypending_window violation  MarkerCoversDurableWindow"
  "sab_relaxedmarker         violation  MarkerSeenMatchesMarker"
  "sab_publishafterconfirm   violation  PromotedNeverDangles"
  "sab_nofence               violation  ConfirmedRelinkNeverDangles"
  "sab_nofence_changed       violation  ChangedImpliesFenced"
  "sab_nopoison              violation  ConfirmedRelinkNeverDangles"
  "sab_nowedge               violation  ConfirmedRelinkNeverDangles"
  "sab_norowexact            violation  ConfirmedRelinkNeverDangles"
  "sab_barevalidator         violation  ConfirmedRelinkNeverDangles"
  "sab_nodiskqualification   violation  ConfirmedRelinkNeverDangles"
  "sab_inferanswer           violation  ConfirmedRelinkNeverDangles"
  "sab_s2bytefetch           violation  NeverPublishedTwice"
  "v11_baseline              green      -"
  "ctl_v11nomarker           green      -"
  "sab_stalecache            green      -"
  "ctl_distinctns            green      -"
  "ctl_skipidentity          green      -"
  "v12_design_full           green      -"
  "v12_coldanswer            green      -"
  "witness_busylane          violation  W_BusyLaneProven"
  "witness_midtenure         violation  W_MidTenureProven"
  "witness_proven            violation  W_ProvenCommitted"
  "witness_delete            violation  W_BlobDeleted"
  "witness_corruptwindow     violation  W_CorruptWindow"
  "witness_collisionreached  violation  W_CollisionReached"
  "witness_changed           violation  W_ChangedThenBytes"
  "witness_unknown           violation  W_UnknownRefusal"
  "witness_budgetunknown     violation  W_ColdRefused"
  "witness_landeds2          violation  W_LandedS2"
)
```

Run. Expected: 31 rows — 14 red, 7 green, 10 witness-red — `ALL EXPECTATIONS MET`, exit 0.

- [ ] **Step 7: Commit.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add docs/superpowers/models/CaRelinkReofferCore.tla \
        docs/superpowers/models/CaRelinkReofferCore_sab_nofence.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_nofence_changed.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_nopoison.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_nowedge.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_norowexact.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_s2bytefetch.cfg \
        docs/superpowers/models/CaRelinkReofferCore_v12_design_full.cfg \
        docs/superpowers/models/CaRelinkReofferCore_v12_coldanswer.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_changed.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_unknown.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_budgetunknown.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_landeds2.cfg \
        docs/superpowers/models/run_relinkreoffer.sh
git commit -m "ca: tla — re-derivation against the new rule set: fence-first, authoritative No, four-state receiver, landed-S2 inside the theorems, NeverPublishedTwice"
```

---

### Task 4: The S7 ruling, the v11 continuity note, and the clean end-to-end run

**Files:**
- Modify: `docs/superpowers/models/CaRelinkReofferCore.tla` (the S7 verdict, written beside the two marker invariants)
- Modify: `docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md` (**append only** — one new section)
- Read first: seam §6, seam §8 row S7, spec §5.1.2.
- **Read-only, must not change:** `CaRelinkConfirmCore.tla` and every `CaRelinkConfirmCore_*.cfg`.

**Interfaces:**
- Consumes: Task 1's `MarkerCoversDurableWindow`, `MarkerSeenMatchesMarker`, `_sab_noapplypending_window` and `_sab_relaxedmarker` results.
- Produces: the S7 verdict paragraph and the v11 cross-reference that Task 5's RESULTS file quotes.

---

- [ ] **Step 1: Settle the S7 ruling on the evidence, and record why the equivalence question is closed structurally rather than by trace comparison.**

The question: **is seam §8 row S7 — "a reader taking `state_mutex` between the arm and the install observes `ApplyPending`, never a stale `Clean`" — expressible as an assertion in this model, or is it code-level-only?**

The answer is settled by the two invariants and two configs the model already carries, and the step's job is to confirm each fact from its log rather than assume it:

1. **The INTERVAL half is asserted.** `MarkerCoversDurableWindow` is green in `v11_baseline`, `sab_stalecache`, `v12_design_full`, `v12_coldanswer`; `_sab_noapplypending_window` breaks it. Confirm both from their logs. Confirm also that it holds through both steps of the stale resolution — the greens are the evidence, and `WedgeResolveStale`/`FloorReconcile` are the actions that would break it if either had cleared the marker or the owed apply.
2. **The OBSERVATION half is asserted too.** `MarkerSeenMatchesMarker` is green in the same greens; `_sab_relaxedmarker` breaks it. Confirm both from their logs. This is the property the seam's mutex placement must DELIVER, stated over the reader-visible value the model carries explicitly.
3. **`_sab_relaxedmarker` is retained, and the reason it is not duplicate evidence is structural, not empirical.** An earlier draft of this plan proposed discarding it if its counterexample matched `_sab_noapplypending`'s. That rule is unsound twice over and must not be used: (a) the two configs cannot produce identical state sequences, because `sApply` moves to `"pending"` in one and stays `"clean"` in the other by construction; (b) even an identical shortest ACTION trace at these bounds would not establish projection equivalence, nor rule out divergence at greater depth — a shortest counterexample is a fact about one bound, not a theorem about the state spaces. The sound argument is the one available for free: **the two configs are asserted against DIFFERENT invariants**, so neither can stand in for the other, and no comparison is needed.
4. **What remains code-level-only is the MECHANISM, not the property.** No untimed model can check a memory model. Seam §6's mutual-exclusion argument (arm under `state_mutex`, read under `state_mutex`, lock at the CALL SITE because `forceWedgeForTest` at `:1396` already holds it) plus seam §8 row S7's test are what establish that `sApplySeen = sApply` holds in the code. The model states the obligation; the seam discharges it.

- [ ] **Step 2: Write the verdict into the module, beside the two marker invariants.**

§5.1.2's own rule is that a gate whose justification sits in another document is a gate nobody re-checks, so the ruling lives in the module, not only in RESULTS. Insert after `MarkerSeenMatchesMarker`:

```tla
(* *** SEAM §8 ROW S7 -- THE RULING. ***
   S7 is: "a reader taking `state_mutex` between the arm and the install observes `ApplyPending`,
   never a stale `Clean`."  BOTH of its halves are asserted here, by two invariants and two reds:
     * INTERVAL   -- `MarkerCoversDurableWindow`, red under `_sab_noapplypending_window`.  This is
                     verbatim what §5.1.2 says this design REQUIRES of the seam fix.
     * OBSERVATION-- `MarkerSeenMatchesMarker`, red under `_sab_relaxedmarker`.  The reader-visible
                     value is a separate variable precisely so that the relaxed store's hazard is a
                     modelled state rather than a comment.
   The two configs are NOT duplicate evidence, and the argument is structural: they are asserted
   against DIFFERENT invariants, so neither can substitute for the other.  (A trace-comparison rule
   was considered and rejected as unsound -- the stored marker's value differs between them by
   construction, so identical state sequences are impossible, and identical shortest traces at one
   bound would prove nothing about the state spaces.)
   WHAT STAYS CODE-LEVEL is the MECHANISM, not the property: no untimed model can check a memory
   model.  That `sApplySeen = sApply` actually holds in the code is established by seam §6 -- arm
   under `state_mutex`, read under `state_mutex`, lock at the CALL SITE, since `forceWedgeForTest`
   (`CasRefLedger.cpp:1396`) already holds it and a self-locking `armApplyPending` would deadlock
   there -- together with seam §8 row S7's test.  The model states the obligation; the seam
   discharges it.  If the seam's lock placement is ever weakened, `MarkerSeenMatchesMarker` is the
   named thing it breaks. *)
```

- [ ] **Step 3: Append the continuity section to `CaRelinkConfirmCore_RESULTS.md`.**

Append one section, `## The v12 refinement, and why this file's _sab_stalecache stays RED {#v12-refinement}`, carrying exactly these five facts:

1. `CaRelinkConfirmCore.tla` and its twelve configs are UNCHANGED and will stay unchanged — §12's disposition: the model is the historical witness of the v11 protocol, and rewriting it would destroy the record that v11's rules were each load-bearing.
2. The redesign's model is `CaRelinkReofferCore.tla`; its results live in `2026-07-29-relink-seam-tla-RESULTS.md`.
3. **The flip, as a side-by-side line, because it is the whole evidence:** `CaRelinkConfirmCore_sab_stalecache` = **RED** (`ConfirmedRelinkNeverDangles` violated) is the v11 record; `CaRelinkReofferCore_sab_stalecache` = **GREEN** is the v12 result. The difference between the two runs is one thing — the second model represents the apply-pending marker: armed strictly before the durable PUT, published to a `state_mutex` reader, cleared atomically with the install, retained across an unresolved PUT, and cleared on exactly the one wedge arm where `CasRefLedger.cpp` clears it.
4. `_sab_holeylist` keeps its meaning unchanged: the historical witness of BACKLOG `{#list-as-journal-dataloss-2026-07-25}`. §12.1 reassigns dangle-freedom's listing half to the v9 chain models, which is why `CaRelinkReofferCore` has no `MaxHoles` dial and names `CommittedEdgesAreGcVisible` instead.
5. One sentence naming what the refinement found that the v11 model could not have: the wedge's marker retention (`:3134-3145`), its single clean-marker exception (`:1927-1934`), and the two-step stale resolution (`:1936-1950` then `:1758-1766`, where the wedge is retired without installing and `Poisoned` survives) are states v11 had no variable for. `_sab_nowedge`'s counterexample runs through the second of them.

- [ ] **Step 4: Verify the v11 family is untouched.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git status --short docs/superpowers/models/CaRelinkConfirmCore.tla docs/superpowers/models/CaRelinkConfirmCore_*.cfg
```
Expected: **no output at all.** Any modification listed here is a Global Constraints violation — revert it before continuing.

- [ ] **Step 5: Re-run the whole battery under a fresh `RUN_ID`.**

Per-run metadirs make the state clean by construction, so there is no wildcard to delete:

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models
RUN_ID="gate-final-$(date +%Y%m%dT%H%M%S)" bash ./run_relinkreoffer.sh \
  | tee ../../../tmp/relinkreoffer_battery_final.txt
```
Expected: 31 rows, `ALL EXPECTATIONS MET`, exit 0. The first line prints the `RUN_ID` and the log directory — **record both; Task 5 cites that directory per row.** Keep `tmp/relinkreoffer_battery_final.txt` verbatim.

Then confirm the v11 battery still behaves as recorded, since the flip is a claim about a PAIR of runs:

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models
./run_relinkconfirm.sh CaRelinkConfirmCore_sab_stalecache
./run_relinkconfirm.sh CaRelinkConfirmCore_main
```
Expected: the first reports `Invariant ConfirmedRelinkNeverDangles is violated`; the second reports `Model checking completed. No error has been found.` Record both output lines verbatim — they are the left-hand column of the flip table.

- [ ] **Step 6: Commit.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add docs/superpowers/models/CaRelinkReofferCore.tla \
        docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md
git commit -m "ca: tla — seam S7 ruling (both halves asserted; the mechanism stays code-level) + v11 continuity note"
```

---

### Task 5: The RESULTS document and the gate verdict

**Files:**
- Create: `docs/superpowers/models/2026-07-29-relink-seam-tla-RESULTS.md`
- Read first: `docs/superpowers/models/2026-07-28-v9-phase-RESULTS.md` (front matter, `## Gate` section shape, and the reds-breakdown paragraph are the template), `docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md`.

**Interfaces:**
- Consumes: Task 4 step 5's `RUN_ID` and log directory, `tmp/relinkreoffer_battery_final.txt`, the two v11 verdict lines, and Task 4's S7 ruling.
- Produces: the greppable verdict line the relink implementation plan's Task 0 checks before any C++ is written.

---

- [ ] **Step 1: Write the front matter and the verdict line.**

Front matter in the house shape: `description`, `sidebar_label: 'Relink/seam TLA+ gate'`, `sidebar_position: 3`, `slug: /superpowers/models/2026-07-29-relink-seam-tla-results`, `title`, `doc_type: 'reference'`.

First section, the verdict — **one line, greppable, nothing else on it**:

```markdown
## Gate {#gate}

> **`RELINK TLA GATE: PASS`**
```

`PASS` **only if** every green is green and every red is red against the invariant NAME it was required to break. Anything else is `RELINK TLA GATE: FAIL` with the failing config named on the next line.

- [ ] **Step 2: Write the consequence block, directly under the verdict.**

Verbatim:

```markdown
**What this verdict decides.** Spec §12.3 step 3 and §1 gate 2:

> If the refined `_sab_stalecache` does not pass, the design is wrong and must not be implemented.

- **PASS** means the two terms this design deletes from `CasRefLedger::confirmExactRef` rule 3 —
  `!rt.pending.empty()` and `rt.leader_active` — are REDUNDANT with the apply-pending marker, and
  deleting them is safe **conditionally on the marker being made synchronized first**
  (`2026-07-29-cas-part-write-release-seam.md` §6). The marker fix is a PREREQUISITE, not a
  companion, and the battery says so twice: `_sab_noapplypending` is red (no marker at all) and
  `_sab_relaxedmarker` is red (a marker whose store is not published to a `state_mutex` reader).
  Shipping the deletion against today's relaxed, unlocked arm is the second of those configs. The
  order is: seam §6 lands, then the relink deletion.
- **FAIL** means **DO NOT IMPLEMENT** `2026-07-29-cas-relink-reoffer-redesign.md`. No C++ of that
  design is written, no `confirmExactRef` rule is deleted, and the spec returns to design with the
  counterexample below as its input. The seam document is unaffected either way — it is
  relink-independent and stands on its own (seam §intro).
```

- [ ] **Step 3: Write the flip section — the single most important table in the file.**

```markdown
## The flip {#the-flip}

| model | cfg | rule 3's lane-quiescence terms | apply-pending marker | verdict |
|---|---|---|---|---|
| `CaRelinkConfirmCore` (v11, unchanged) | `_sab_stalecache` | dropped | **not represented** | **RED** — `ConfirmedRelinkNeverDangles` |
| `CaRelinkReofferCore` (v12) | `_v11_baseline` | present | armed + published | green |
| `CaRelinkReofferCore` (v12) | `_ctl_v11nomarker` | present | **not armed** | green |
| `CaRelinkReofferCore` (v12) | **`_sab_stalecache`** | **dropped** | **armed + published** | **GREEN — THE FLIP** |
| `CaRelinkReofferCore` (v12) | `_sab_noapplypending` | dropped | **not armed** | **RED** — `ConfirmedRelinkNeverDangles` |
| `CaRelinkReofferCore` (v12) | `_sab_relaxedmarker` | dropped | armed, **not published** | **RED** — `MarkerSeenMatchesMarker` |
```

Then three paragraphs, each with its evidence rather than its claim:

1. **What the matrix establishes.** Each guard is individually sufficient (rows 2 and 3), at least one is necessary (row 5), and the v12 substitution is a real exchange rather than a coincidence. Row 6 is what makes "synchronized" a load-bearing word instead of an adjective.
2. **Why the green is not vacuous.** Quote `_witness_busylane`'s and `_witness_midtenure`'s violations with their depths: without the first, the busy-lane `proven` is unreachable and the green is an artefact of an empty state; without the second, the green is the one-transaction-per-tenure artefact §12.5 ii warns about. Name the mechanism that made the second reachable — `SenderAdmit`'s `"unrelated"` chunk kind — and say plainly that an earlier draft of the model had no such kind and therefore no reachable second chunk.
3. **The counterexample.** Paste `_sab_noapplypending`'s trace from `$RUNDIR/tlc_sab_noapplypending.log`, annotated action by action, and state its depth.

- [ ] **Step 4: Write the full battery table and the reds breakdown.**

One row per config: `cfg | expected | observed | invariant | states (gen/distinct) | depth | seconds | log`. Thirty-one rows, from `tmp/relinkreoffer_battery_final.txt` and each `$RUNDIR/tlc_<cfg>.log` — real TLC numbers, never estimates. **The log column cites the per-run path under `tmp/tlc-runs/relinkreoffer/$RUN_ID/`, never the convenience symlink**, so every row can be re-read after later runs. Name the `RUN_ID` once above the table.

Paste the runner's own table verbatim in a fenced block beneath it, including its `ALL EXPECTATIONS MET` line, and state the bounds actually used (`MaxId`, `MaxRound`, `MaxChunks`) plus any config where they had to be shrunk and why.

Follow the v9 phase file's honesty convention — break the reds down by class instead of lumping them:

```markdown
- **11 sabotage-class** — one load-bearing rule removed per config.
- **1 type negative control** — `_sab_typeprobe`, which is what makes every green's `TypeOK` a
  checked property rather than a listed one. It exists because "a green is only evidence once the
  property has been seen red" applies to type invariants too, and the alternative was an exemption
  clause.
- **2 marker-shape reds** — `_sab_noapplypending` (the marker is absent) and `_sab_relaxedmarker`
  (the marker is present but unpublished). Asserted against different invariants, which is why
  neither substitutes for the other.
- **10 reachability witnesses** — negated reachability, where the violation IS the evidence.
```

- [ ] **Step 5: Write the four obligation sections, one per §12 clause.**

- **§12.3 — the required refinement.** The 2×2 plus the two marker-shape reds; cross-reference §the-flip.
- **§12.5 i — cross-mount collision.** `_sab_barevalidator` RED, `_sab_nodiskqualification` RED, `_ctl_distinctns` GREEN, `_witness_collisionreached` reachable. State the control's meaning in one sentence: the red next door is caused by the EQUAL-namespace configuration, which is what §12.5 i's "specifically" demands, and a model that ran only distinct namespaces would pass while the wire is unsafe.
- **§12.5 ii — chunk-boundary tenure.** `MaxChunks = 2`, the `"tracked"`/`"unrelated"` chunk kinds, `_witness_midtenure` reachable at `proven /\ sLeader /\ sTenureChunks >= 1`, and the sentence that matters: without it, `_sab_stalecache`'s green would be an artefact of one transaction per tenure. Record that `sTenureChunks` resets on every tenure close — including the poison and both unresolved arms — so the witness proves *same tenure* rather than *two chunks somewhere*.
- **§12.4 — re-derivation.** Fence hoisted first (`ChangedImpliesFenced`, `_sab_nofence_changed` RED); `No` given its own successor (`RChangedThenBytes`) and shown unable to publish twice (`NeverPublishedTwice`, `_sab_s2bytefetch` RED); the answer/identity pair decoupled (`_sab_inferanswer` RED, `_ctl_skipidentity` GREEN-as-result); the landed-`Unresolved` publication brought inside both theorems via `rCommitted` (`_witness_landeds2` reachable); the two v11 witnesses re-derived (`_witness_changed`, `_witness_unknown`) with one line each on why the state is no longer the same one; rule 2 revisited (`_v12_coldanswer` green, `_witness_budgetunknown` reachable); the publish-then-confirm order still load-bearing (`_sab_publishafterconfirm` RED, and one line on why it sits in Task 1 rather than Task 3).

- [ ] **Step 6: Write the assumptions and seam sections.**

- **The three named assumptions**, each with its discharge mechanism and the sentence that makes weakening it visible: `CommittedEdgesAreGcVisible` (v9 chain models — name the exact configs); `UnresolvedPromoteNeverBytes` (spec §11 row 9, with `_sab_s2bytefetch` as the in-model necessity half); `FreshCertifiedResponse` (rows 15a/15b for the offer-confusion half, which IS partly modelled via `OfferIdentity` and `_sab_inferanswer`; row 16 for the replay half, which is not). State plainly that if any of those rows is weakened, the assumption goes with it — which is the point of naming them.
- **Seam §8 row S7**, with Task 4's ruling: both halves asserted, by `MarkerCoversDurableWindow` and `MarkerSeenMatchesMarker`; the mechanism (mutex-supplied happens-before) stays code-level because no untimed model checks a memory model; the two configs are non-duplicate structurally, and the trace-comparison rule an earlier draft proposed is recorded as rejected-as-unsound with its one-line reason.
- **What the seam contributes and what it does not:** §3's emission point and §4's `attempted` mark are accounting with no safety content to gate (seam §3.3, §9 point 5), discharged by seam §8 rows S1–S6c and relink rows 19–20.

- [ ] **Step 7: Write the structural-exclusions section.**

This section exists because the Global Constraints require a guard that makes a state unrepresentable to be *recorded* rather than asserted — otherwise a reader finds a guard with no evidence beside it and cannot tell whether it was checked or forgotten.

```markdown
## Structural exclusions — guards, not invariants {#structural-exclusions}

Three hazards are excluded by an action guard rather than by an invariant. Each is listed with the
code fact that licenses it and with the reason an invariant would have been worse than useless: an
assertion over a state the transition relation cannot enter is green for free, which is the opposite
of evidence.

| Excluded state | Guard | Code fact |
|---|---|---|
| A floor-covered wedge is re-applied | `WedgeResolveInstall`'s `~sFloorCovers[m]` | `durableFloorCovers` is tested FIRST (`CasRefLedger.cpp:1758`), before a candidate is built, so a floor-covered wedge can only take the `FloorReconciled` path. Re-applying an id AT the floor is what `applyTxnInPlace` refuses as non-contiguous — "every later flush would throw in the same place and the lane would stay wedged for the runtime's life" |
| A poisoned cache is advanced to clean | `WedgeResolveInstall`'s `sApply[m] # "poisoned"` | a `Poisoned` table is by definition missing a durable transaction; only a recovery may re-derive it (`:1758-1766`'s log line). An earlier draft of this model guarded the action on `sWedge /\ sApplyOwed` alone, which left this transition enabled immediately after `WedgeResolveStale` |
| A durable byte with no prior arm | `SenderDurable`'s `sArmed[m]` | seam §6 / `:2808`: the arm is "the last statement that still runs while nothing of this transaction can possibly be durable". Making the ordering a guard rather than an invariant is what lets `SabotageNoApplyPending` remove the marker's VALUE without removing the ORDERING, which is what "drop the marker" means |

Two arms of `resolveWedgeOnce` are likewise modelled as NON-transitions, and for the same kind of
reason: `NoWedge` (`:1745-1746`) is the absence of `sWedge`, and every inert `StillWedged` reason
(`:2062-2128`, each of which says "the result is inert and the lane keeps whatever wedge it has") is
`UNCHANGED vars`, which `NoOp` already supplies. They are delay, not state change. The full
outcome→action mapping is in the module, above the wedge actions.
```

- [ ] **Step 8: Write the "what this model dropped, and what it found" section.**

Two short subsections.

**Dropped from v11, deliberately:** the `MaxHoles` / `NsNoise` holey-list machinery, per §12.1's reassignment, with `_sab_holeylist` named as where that finding still lives and `CommittedEdgesAreGcVisible` named as what replaced it here.

**Found by the refinement, and not findable in v11:** the wedge lifecycle as a modelling result in its own right. The v11 model had no marker variable, so it could not represent the `Unresolved` retention (`:3134-3145`), the single clean-marker exception (`:1927-1934`) with its separate fence-closing reaction (`:2050-2060`), or the two-step stale resolution (`:1936-1950` then `:1758-1766`). Record all three of these plainly, because each was a real defect in a draft of this model and the record is what stops the next draft repeating it:

1. A draft cleared the marker on the unresolved PUT — which would have made `_sab_nowedge` red for a state the code cannot reach.
2. A later draft omitted `FloorReconcile` entirely and left `WedgeResolveInstall` enabled on `sWedge /\ sApplyOwed`, admitting a poisoned-to-clean install.
3. Both drafts carried a blanket `~sWedge` exclusion in `MarkerCoversDurableWindow`, which was hiding (1) rather than describing the code. Once the invariant's subject became this mount's own owed apply, **no exclusion is needed at all** — not for the wedge and not for the fence — and it holds through both steps of the stale resolution.

- [ ] **Step 9: Verify the verdict line is unique, then commit.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
grep -c "RELINK TLA GATE:" docs/superpowers/models/2026-07-29-relink-seam-tla-RESULTS.md
```
Expected: `1`. More than one means the string appears in prose too — rephrase the prose; the verdict line stays unique so a grep can never return an ambiguous answer.

```bash
git add docs/superpowers/models/2026-07-29-relink-seam-tla-RESULTS.md
git commit -m "ca: tla — relink/seam gate RESULTS: RELINK TLA GATE verdict + the do-not-implement consequence"
```

---

## Self-review notes (done at write time; revised after codex rounds 1 and 2)

**Round-2 findings and where each is now addressed.**

| # | Finding | Resolution |
| --- | --- | --- |
| B1 (narrowed, was NOT-ADDRESSED) | The stale-state path was modelled as a single step, and `WedgeResolveInstall` stayed enabled on `sWedge /\ sApplyOwed` alone, admitting a poisoned-to-clean install | Task 1 step 4 now models the **two-step** stale resolution against the code: `WedgeResolveStale` (`:1936-1950`) raises the floor via a new `sFloorCovers` variable, poisons, and RETAINS the wedge; `FloorReconcile` (`:1758-1766`) retires the wedge, installs NOTHING, and retains both `Poisoned` and `sApplyOwed` — because the `clearApplyPending` there is documented as a no-op on a Poisoned table. `WedgeResolveInstall` is guarded by `~sFloorCovers[m]` AND `sApply[m] # "poisoned"`, recorded as structural exclusions in RESULTS step 7 rather than as invariants. `WedgeResolveRejected` and `WedgeResolveCorrupted` gain `~sFloorCovers[m]`, since the floor test runs first. An **outcome→action mapping block** now sits above the wedge actions with one line per arm of `resolveWedgeOnce`, including the two arms that map to no transition. `EvictTable` relaxed to `sApply # "pending"` and `RecoverForAnswer` extended to clear the marker, the owed apply and the floor record, so the poisoned lane has the exit the code describes ("until a recovery re-derives it from the log") instead of being a dead end. |
| MAJOR (new) | `PromotedNeverDangles` listed by Task 1's and Task 2's greens while its red first existed in Task 3 | `_sab_publishafterconfirm` **moved into Task 1** (config count 12→13 there, 13→12 in Task 3; total unchanged at 31). Reason stated in the cfg header and in Global Constraints: dropping the invariant from the earlier greens instead would run the 2×2 gate without the antecedent-free theorem, weakening the very cell that decides the design. The constraint itself is now worded as "in the same task, not a later one", with the concrete rule "no green may list an invariant whose `_sab_*` config does not yet exist". |
| m1 | Task 2's prose said all three must-red configs use `SecondMount = TRUE`, contradicting `_sab_inferanswer` | Task 2 step 2's preamble now states the split and why: the two validator sabotages need the second mount because a cross-mount collision is what they are about; `_sab_inferanswer` needs none, because an ungated offer response is a single-mount hazard. Its cfg header says so too. |
| m2 | The `_sab_noapplypending` trace suffix was written `GFold → GSettle×3` | Corrected to `(GFold → GSettle)×3` in Task 1 step 9, with the reason inline: consecutive settles are disabled by `gcPhase`, so each graduation phase costs a fold and a settle. |
| m3 | Step 12 said "twelve sender-lane actions" | Corrected to **eighteen**, enumerated by name, with the six review-caught ones listed as individual verification items and a `grep -c 'sFloorCovers'` check added for the new variable's `UNCHANGED` coverage. |

**Round-1 findings, all confirmed addressed by round 2 and unchanged in v3:** B2 (chunk kinds, per-tenure counting, strengthened witness — verified by hand-traced minimal trace), M3 (`rCommitted` covering landed S2), M4 (answer/identity decoupled; `_sab_inferanswer` red, `_ctl_skipidentity` green-as-result), M5 (`sApplySeen`, `MarkerSeenMatchesMarker`, `_sab_relaxedmarker` retained on a structural argument), m6 (`TypeOK` complete + `_sab_typeprobe`), m7 (`RUN_ID`, per-run metadirs and logs, `flock`, per-run paths in RESULTS).

**Deliberately byte-stable in v3.** The 22-constant block, the base cfg block, the validator and answer/identity section (step 5), the receiver, GC and witness blocks, the runner's isolation and marker code, and Tasks 4–5's structure are unchanged from v2 except where a finding required an edit. The only additions to previously-reviewed text are `sFloorCovers` in the variable list, `senderVars`, `Init`, `TypeOK` and every action's `UNCHANGED`, plus the three new wedge actions in `Next`.

**Spec coverage.** §12.1 (listing reassigned — no `MaxHoles` dial; `CommittedEdgesAreGcVisible` named instead). §12.2 (that assumption named with discharge). §12.3 (the 2×2 plus the two marker-shape reds — the gate). §12.4 (fence-first, authoritative `No`, no double publish, landed-S2 coverage, publish-then-confirm order, witnesses re-derived, rule 2 revisited). §12.5 i (equal-namespace/different-disk mounts, both named reds, plus the control the "specifically" clause implies). §12.5 ii (chunk kinds, per-tenure counting, strengthened witness). §12.5 iii (`UnresolvedPromoteNeverBytes` named, with `_sab_s2bytefetch` as the necessity half). §12's `FreshCertifiedResponse` (named, with its modelled and unmodelled halves stated). Seam §8 S7 (both halves asserted; mechanism code-level). Seam §3/§4 (out of scope with the reason). §12's disposition ruling (v11 read-only, enforced by a `git status` check).

**Three places this plan goes beyond the spec, deliberately, and says so.** (1) `_ctl_v11nomarker`, `_ctl_distinctns` and `_ctl_skipidentity` — controls the spec does not name, because a flip with no control is a coincidence and a green with no scoping statement is a silent gap. (2) `_sab_s2bytefetch` — the spec ASSUMES `UnresolvedPromoteNeverBytes`; this plan keeps the assumption assumed and models only its consequence. (3) The wedge lifecycle in full — §12 does not ask for it, but two drafts proved that modelling rule 3 without it produces a sabotage that is red for an unreachable state, which is worse than no sabotage.

**Placeholder scan.** Every cfg's full constants block appears once (Task 1 step 8); every later cfg is a delta with exact toggles and an exact `INVARIANTS` list. Every module fragment is real TLA+. Nothing is deferred: the S7 question is answered on evidence the battery already produces, and the one previously open decision rule was removed as unsound rather than left branching.

**Type consistency.** `sApply` and `sApplySeen` share the domain `ApplyStates`; the gate reads `sApplySeen`, the interval invariant reads `sApply`, and only `SenderArm` may separate them. `sApplyOwed` is the sole subject of `MarkerCoversDurableWindow` and is written by exactly four actions (`SenderDurable` and `SenderUnresolvedLanded` set it; `SenderInstall`, `WedgeResolveInstall` clear it) plus `RecoverForAnswer`, which is the poisoned lane's only exit. `sFloorCovers` is written by exactly two (`WedgeResolveStale` sets it, `RecoverForAnswer` clears it) and read as a guard by four. `rCommitted` is written by exactly two (`RPromoteCommit`, `S2ResolveLanded`) and is the antecedent of both theorems. `rAccepted` — not `rAnswer = "proven"` — is the theorem's certificate antecedent, so a sabotage that makes the receiver accept the wrong thing cannot escape by never producing the word `proven`. `rAnswer`'s domain includes `Absent` for the offer-response arm, and `TypeOK` lists it. Every `rState` value used in Tasks 1 and 3 appears in `TypeOK`'s enumeration.

### Critical Files for Implementation

- /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models/CaRelinkConfirmCore.tla
- /home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
- /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-29-cas-relink-reoffer-redesign.md
- /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-29-cas-part-write-release-seam.md
- /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models/run_refcatalog.sh
