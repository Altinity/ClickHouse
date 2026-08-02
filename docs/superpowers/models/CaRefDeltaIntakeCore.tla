-------------------- MODULE CaRefDeltaIntakeCore --------------------
(* GC ref-intake core, POOL-WIDE — spec 2026-07-27-cas-ref-chain-complete-cut-design.md (v9),
   §1 (blob in-degree is pool-wide), §5 (fold rules, the destructive-round frontier proof, the
   durable hold), §7 (REBUILD preserves holds). TLA+ phase (gate), Task 2.

   Two namespaces (T1, T2) append contiguous ref records; ONE blob is shared between them. That is
   the whole point of this module: per-namespace cursor immobility is worthless when in-degree and
   deletion are pool-wide, so the oracle here is not "did the cursor pass an unfolded log" (that is
   `NoMissedFold`, retained) but "was an acked `+1` still unaccounted when the blob was deleted"
   (`NoAckedLoss`, new, and the reason the frontier proof and the hold exist).

   Task 5b's exact-frontier rewrite:

   - The LIST hint is CONSUMED NOWHERE for correctness. `HintReturn` may return any subset of the
     present logs, in any prefix of increasing id order, or nothing at all — omission is the NORM,
     not a sabotage. It may prioritize work and populate diagnostics, but it cannot select targets,
     define a stop, or detect a committed gap.
   - The fold advances by ARITHMETIC (`WalkStep`: point-read `cand[t] + 1`). Contiguity (INV-1) makes
     the exact `_ckpt.committed_through` sample finite. Epochs are crossed by consuming seals: a seal
     occupies a slot, folds as an applied no-op, and the walk continues over it (uniform consumption).
   - A round may run DESTRUCTIVE work (condemn, delete) only while holding a frontier proof for
     EVERY catalog-admitted life at that exact CTE, obtained in that round, and only if its own fold
     commit WON — a losing round adopted nothing, so it may destroy nothing.
   - An exact `GET` returning absent at or below CTE HOLDS the life durably at that offending position.
     The same CTE re-detects it every round even under total hint omission. A hold suppresses ALL
     destruction pool-wide and clears ONLY by folding through that exact position and adopting the
     result. REBUILD carries holds verbatim (§7).

   The store's exact point read is HONEST here (spec §1: point reads and conditional writes are the
   operations the store performed honestly even while its LIST lied) — with one bounded FAULT,
   `EnableHiddenHole`, which makes one above-cursor durable log invisible to point reads. That fault
   is not a protocol sabotage and not an admitted store behaviour; it is the corruption / deposed-
   leader-cleanup shape spec §5 names (a missing required committed key), and it is the ONLY way
   that shape is reachable at all under the honest writer/cleanup rules.
   `RevealLog` lets the fault be transient as well as permanent, so both the "hold clears by folding
   through" path and the "hold never clears, destruction suppressed forever" path are explored.

   Task 5b replaces the former expected-next 404 proof and listing/checkpoint-snapshot witnesses with
   exact `_ckpt.committed_through`. Every catalog-admitted life is a mandatory target, and any absent
   key at or below that frozen bound reasserts a hold independent of listing hints.

   Each Sabotage* toggle breaks exactly one load-bearing rule and must yield a counterexample.
   `HintSilent`, `HintComplete` and `EnableHiddenHole` are adversaries, not sabotages, and are
   enabled in GREEN configurations too. *)
EXTENDS Integers, FiniteSets

CONSTANTS
    T1, T2, MaxSeq,
    EdgeOf,                       \* [AllKeys -> {"add","rem","none"}]: each record's blob edge
    HintSilent,                   \* adversary: the LIST hint returns NOTHING, ever
    HintComplete,                 \* the LIST hint is an HONEST listing (no omission) — the control
    EnableHiddenHole,             \* fault: one above-cursor durable log is invisible to point reads
    SabotageRebuildDropsHold,     \* REBUILD rebuilds cursors but forgets holds (the r8 blocker)
    SabotageClearHoldOnAbsent,    \* a hold clears on a second absent probe instead of by folding
    SabotageDestroyUnderHold,     \* destruction ignores a carried hold
    SabotageDeleteIgnoresIndeg,   \* the pending delete skips its liveness re-check
    SabotageAdoptBeforeCommit,    \* cursor becomes visible to cleanup before the commit's win/lose
    SabotageCleanupIgnoresCursor, \* cleanup requires only snapshot coverage, not cursor coverage
    PlanOnly,                     \* isolate the Task 5 catalog-built-plan proof from the fold gate
    SabotageAdapterMints,         \* an input adapter inserts a ref-life row outside the catalog loop
    SabotageSkipCatalogTarget,    \* LIST omission lets `ScanComplete` skip a mandatory catalog life
    SabotageSkipHeldRetry         \* a carried hold may complete a round without an exact retry

ASSUME T1 # T2
ASSUME MaxSeq \in Nat /\ MaxSeq >= 2

Tables == {T1, T2}
Ids == 1..MaxSeq
PlanIds == 1..3
AllKeys == { [t |-> t, i |-> i] : t \in Tables, i \in Ids }

(* ---- the shared blob's edges (spec §1: in-degree is POOL-WIDE) ----

   The r7-1 scenario, laid out as constants so the counterexample is not an accident of the search:
   the blob starts with one already-folded edge (`BaseIndeg`, namespace T2's earlier life), T2's
   record `RemId` REMOVES it, and T1's record `AddId` is the acknowledged `+1` that a hint omission
   or a dropped hold can hide. `AddId > 1` deliberately: proving T1's frontier requires walking
   through a record BELOW the add, which is the position the hidden-hole fault attacks.

   `EdgeRoles` is the value every configuration binds to the CONSTANT `EdgeOf` (`EdgeOf <-
   EdgeRoles`): a cfg cannot spell a function literal, and fixing the roles in the module keeps the
   scenario identical across configurations, which is what makes the sabotage/green pairs
   controlled experiments. A configuration wanting different roles overrides to another operator —
   but note what the roles are load-bearing FOR: `NoAckedLoss` below is the strong form ("no acked
   add exists at all when the blob is deleted"), which is only the honest statement of the damage
   because no `rem` in this key space ever retracts the `add`. An override that pairs a removal with
   the add makes deletion legitimate and turns that invariant into a false alarm; the second ASSUME
   below is the guard that catches the specific case of over-many removals, not this one. Such an
   override must weaken `NoAckedLoss` to the "acked but unfolded" form in the same change. *)
AddId == 2
RemId == 1
BaseIndeg == 1
EdgeRoles ==
    [ k \in AllKeys |-> IF      k.t = T1 /\ k.i = AddId THEN "add"
                        ELSE IF k.t = T2 /\ k.i = RemId THEN "rem"
                        ELSE "none" ]

ASSUME EdgeOf \in [AllKeys -> {"add", "rem", "none"}]
(* Non-negative in-degree by construction: at most `BaseIndeg` removals exist in the whole key
   space, so no fold order can drive the count below zero. TypeOK asserts the consequence. *)
ASSUME Cardinality({ k \in AllKeys : EdgeOf[k] = "rem" }) <= BaseIndeg

VARIABLES
    durable,       \* SUBSET AllKeys: present _log objects (deletable by Cleanup)
    everDurable,   \* SUBSET AllKeys: every key ever made durable (monotonic; the acked oracle)
    hidden,        \* SUBSET AllKeys: FAULT — durable logs an exact GET currently fails to return
    sealed,        \* [Tables -> SUBSET Ids]: slots occupied by an EpochSeal (folds as a no-op)
    pendingId,     \* [Tables -> 0..MaxSeq]: in-flight append id, 0 = no unresolved append (wedge)
    snap,          \* [Tables -> 0..MaxSeq]: abstract published snapshot coverage (_ckpt.checkpoint)
    committedThrough, \* [Tables -> 0..MaxSeq]: exact `_ckpt.committed_through` authority
    cursor,        \* [Tables -> 0..MaxSeq]: durable, adopted last_folded_ref_id
    cand,          \* [Tables -> 0..MaxSeq]: this round's candidate cursor (the arithmetic walk head)
    csnap,         \* [Tables -> 0..MaxSeq]: cursor snapshotted at this round's BeginRound
    maxSeen,       \* [Tables -> 0..MaxSeq]: greatest id this round's non-authoritative hint returned
    frontier,      \* [Tables -> BOOLEAN]: this round reached the exact CTE for the catalog life
    roundCte,      \* [Tables -> 0..MaxSeq]: exact CTE sample frozen at this round's `BeginRound`
    targetResolved,\* [Tables -> BOOLEAN]: target reached CTE or exact-read a required missing key
    heldAtStart,   \* [Tables -> BOOLEAN]: carried holds this round must retry independent of LIST
    delta,         \* SUBSET AllKeys: keys walked this round above csnap (this round's candidate fold)
    folded,        \* SUBSET AllKeys: durably adopted deltas (the in-degree fold input)
    gcPhase,       \* "idle" | "scanning" | "complete"
    roundOutcome,  \* "none" | "won" | "lost" -- outcome of the current/last round's commit
    dupFlag,       \* sticky ghost: a commit tried to adopt a key already in folded (I2 oracle)
    hold,          \* [Tables -> BOOLEAN]: the namespace is held on an impossible shape
    holdPos,       \* [Tables -> 0..(MaxSeq+1)]: the hold's offending position (0 = not held)
    holdDebt,      \* [Tables -> 0..(MaxSeq+1)]: ghost — a hold's position released WITHOUT folding
                   \* through it. Sabotage-only; the debt is what makes the two hold sabotages'
                   \* counterexamples consequential rather than tautological: the flag below is set
                   \* by DESTRUCTION happening while a debt is outstanding, not by the drop itself.
    condemned,     \* the shared blob is condemned (in-degree reached zero)
    deleted,       \* the shared blob's bytes are gone
    rsc,           \* 0..1: rounds since condemnation (the two-phase pacing floor)
    destroyedUnderHold, \* sticky ghost: a destructive step ran while a hold was live or in debt
    (* ---- Task 5: catalog-cut-only ref-life plan ---- *)
    planCut,            \* [PlanIds -> {"creating", "live", "removing", "absent"}]
    gcRefLives,         \* SUBSET PlanIds: ordinary fold's `ref_lives` keys
    rebuildRefLives,    \* SUBSET PlanIds: REBUILD's `ref_lives` keys from the same constructor
    planBuilt,          \* BOOLEAN: both constructors have consumed the immutable cut
    enriched            \* [adapter -> SUBSET PlanIds]: metadata attached to existing rows only

storeVars == << durable, everDurable, hidden, sealed >>
laneVars  == << pendingId, snap, committedThrough >>
roundVars == << cand, csnap, maxSeen, frontier, roundCte, targetResolved, heldAtStart,
                delta, gcPhase, roundOutcome >>
foldVars  == << cursor, folded, dupFlag >>
holdVars  == << hold, holdPos, holdDebt >>
blobVars  == << condemned, deleted, rsc, destroyedUnderHold >>
Adapters == {"parent", "list_hint", "hold", "checkpoint_tail"}
planVars == << planCut, gcRefLives, rebuildRefLives, planBuilt, enriched >>
legacyVars == << storeVars, laneVars, roundVars, foldVars, holdVars, blobVars >>

vars == << legacyVars, planVars >>

MaxOf(S) == CHOOSE x \in S : \A y \in S : x >= y
MaxOr0(S) == IF S = {} THEN 0 ELSE MaxOf(S)

IdsOf(S, t) == { k.i : k \in { kk \in S : kk.t = t } }
DurableIds(t) == IdsOf(durable, t)
EverIds(t) == IdsOf(everDurable, t)

(* The bounded model's opaque catalog life ids. Id 3 is deliberately non-walkable, so the adapter
   sabotage still has a `Creating` id to try to mint while T1/T2 represent the admitted rows. *)
LifeOf == [t \in Tables |-> IF t = T1 THEN 1 ELSE 2]
WalkablePlanIds == {i \in PlanIds : planCut[i] \in {"live", "removing"}}
CatalogTargets == {t \in Tables : LifeOf[t] \in gcRefLives}

(* INV-1: the next id is derived from STATE — greatest applied id (a seal applies too) plus one.
   `everDurable`, not `durable`: cleanup deleting a covered log must not free its id for reuse. *)
MaxDurable(t) == MaxOr0(EverIds(t) \cup sealed[t])

(* An exact point read. Honest except for the `hidden` fault; a seal occupies its slot and answers
   the read (spec §5: epochs are crossed by consuming seals). *)
Present(t, i) ==
    \/ ([t |-> t, i |-> i] \in durable /\ [t |-> t, i |-> i] \notin hidden)
    \/ i \in sealed[t]

(* Pool-wide in-degree of the shared blob over the ADOPTED fold — never over `delta`, which a losing
   commit throws away. *)
Indeg == BaseIndeg + Cardinality({ k \in folded : EdgeOf[k] = "add" })
                   - Cardinality({ k \in folded : EdgeOf[k] = "rem" })

AnyHold == \E t \in Tables : hold[t]
AnyDebt == \E t \in Tables : holdDebt[t] # 0
Encumbered == AnyHold \/ AnyDebt

Init ==
    /\ durable = {}
    /\ everDurable = {}
    /\ hidden = {}
    /\ sealed = [t \in Tables |-> {}]
    /\ pendingId = [t \in Tables |-> 0]
    /\ snap = [t \in Tables |-> 0]
    /\ committedThrough = [t \in Tables |-> 0]
    /\ cursor = [t \in Tables |-> 0]
    /\ cand = [t \in Tables |-> 0]
    /\ csnap = [t \in Tables |-> 0]
    /\ maxSeen = [t \in Tables |-> 0]
    /\ frontier = [t \in Tables |-> FALSE]
    /\ roundCte = [t \in Tables |-> 0]
    /\ targetResolved = [t \in Tables |-> FALSE]
    /\ heldAtStart = [t \in Tables |-> FALSE]
    /\ delta = {}
    /\ folded = {}
    /\ gcPhase = "idle"
    /\ roundOutcome = "none"
    /\ dupFlag = FALSE
    /\ hold = [t \in Tables |-> FALSE]
    /\ holdPos = [t \in Tables |-> 0]
    /\ holdDebt = [t \in Tables |-> 0]
    /\ condemned = FALSE
    /\ deleted = FALSE
    /\ rsc = 0
    /\ destroyedUnderHold = FALSE
    /\ planCut = [i \in PlanIds |-> IF i = 1 THEN "live"
                              ELSE IF i = 2 THEN "removing"
                              ELSE "creating"]
    /\ gcRefLives = {}
    /\ rebuildRefLives = {}
    /\ planBuilt = FALSE
    /\ enriched = [a \in Adapters |-> {}]

(* ---- writer actions (INV-1: never sabotaged here; the ambiguity split is Task 1's subject) ---- *)

(* At most one unresolved append per namespace (the wedge), at the state-derived next id. *)
WAppendStart(t) ==
    /\ pendingId[t] = 0
    /\ MaxDurable(t) + 1 <= MaxSeq
    /\ pendingId' = [pendingId EXCEPT ![t] = MaxDurable(t) + 1]
    /\ UNCHANGED << storeVars, snap, committedThrough, roundVars, foldVars, holdVars, blobVars >>

(* Resolution by success. Spec §5 temporal lemma, writer-side closure (r9-2 variant (a)): a writer
   adding a ref to a blob whose meta says `Condemned` — or that a round already deleted — does not
   inherit the corpse; it REMATERIALIZES from source, a fresh re-upload the old exact-token delete
   cannot touch. Modelled at the moment the add becomes durable, which is the moment the acked `+1`
   starts to matter. Without this the late-arrival window would be a false counterexample; with it,
   the window that remains is the one the frontier proof has to close.

   The successor-seal-as-conclusive-rejection rule of INV-1 is NOT modelled here and cannot be: a
   seal is only placed at `MaxDurable(t) + 1` with the lane idle (`EpochSeal` requires
   `pendingId[t] = 0`), and an in-flight id is derived the same way, so the pending slot is never a
   sealed slot in this module. The wedge meeting a successor's seal needs a writer-local view of the
   store, which is `CaRefTableSnapshotLogCore`'s `WResolveSealRejected` (task 1). *)
WAppendDurable(t) ==
    /\ pendingId[t] # 0
    /\ LET key == [t |-> t, i |-> pendingId[t]] IN
         /\ durable' = durable \cup {key}
         /\ everDurable' = everDurable \cup {key}
         /\ condemned' = IF EdgeOf[key] = "add" THEN FALSE ELSE condemned
         /\ deleted' = IF EdgeOf[key] = "add" THEN FALSE ELSE deleted
    /\ pendingId' = [pendingId EXCEPT ![t] = 0]
    /\ committedThrough' = [committedThrough EXCEPT ![t] = pendingId[t]]
    /\ UNCHANGED << hidden, sealed, snap, roundVars, foldVars, holdVars, rsc, destroyedUnderHold >>

(* Resolution WITHOUT durability: nothing landed, so the slot is reusable and NO GAP is created —
   the next `WAppendStart` derives the same id again. Contiguity is why the fold can decide absence
   arithmetically at all. *)
WAppendAbandon(t) ==
    /\ pendingId[t] # 0
    /\ pendingId' = [pendingId EXCEPT ![t] = 0]
    /\ UNCHANGED << storeVars, snap, committedThrough, roundVars, foldVars, holdVars, blobVars >>

(* INV-2: a writer-epoch transition is closed in-band — the successor occupies the frontier slot.
   The occupant is a real object: the fold's walk READS it and continues (uniform consumption), and
   it applies as a no-op, contributing no edge. *)
EpochSeal(t) ==
    /\ pendingId[t] = 0
    /\ MaxDurable(t) + 1 <= MaxSeq
    /\ sealed' = [sealed EXCEPT ![t] = @ \cup {MaxDurable(t) + 1}]
    /\ committedThrough' = [committedThrough EXCEPT ![t] = MaxDurable(t) + 1]
    /\ UNCHANGED << durable, everDurable, hidden, pendingId, snap,
                    roundVars, foldVars, holdVars, blobVars >>

(* Abstract snapshot publication (`_ckpt.checkpoint`): may cover any present id of the table. *)
WRaiseSnap(t) ==
    /\ \E i \in DurableIds(t) :
         /\ i > snap[t]
         /\ snap' = [snap EXCEPT ![t] = i]
    /\ UNCHANGED << storeVars, pendingId, committedThrough, roundVars, foldVars, holdVars, blobVars >>

(* ---- the fault: an above-cursor log the store stops answering for ----

   Bounded to one key at a time, above the cursor (a covered log below the cursor is already
   folded, so its loss is a cleanup question, not an accounting one) and only where a HIGHER
   durable id exists — i.e. only in the shape the protocol can in principle detect. *)
HideLog(t, i) ==
    /\ EnableHiddenHole
    /\ hidden = {}
    /\ [t |-> t, i |-> i] \in durable
    /\ i > cursor[t]
    /\ \E j \in DurableIds(t) : j > i
    /\ hidden' = { [t |-> t, i |-> i] }
    /\ UNCHANGED << durable, everDurable, sealed, laneVars, roundVars, foldVars, holdVars, blobVars >>

RevealLog ==
    /\ hidden # {}
    /\ hidden' = {}
    /\ UNCHANGED << durable, everDurable, sealed, laneVars, roundVars, foldVars, holdVars, blobVars >>

(* ---- GC round: hint, arithmetic walk, frontier proof, fold commit ---- *)

BeginRound ==
    /\ gcPhase = "idle"
    /\ planBuilt
    /\ gcPhase' = "scanning"
    /\ csnap' = cursor
    /\ cand' = cursor
    (* The round's ONE non-authoritative enumeration. `HintComplete` is only a completeness control;
       neither arm affects targets, CTE, committed-gap detection, or the destructive frontier. *)
    /\ maxSeen' = [t \in Tables |-> IF HintComplete THEN MaxOr0(DurableIds(t)) ELSE 0]
    /\ roundCte' = committedThrough
    /\ targetResolved' = [t \in Tables |-> t \in CatalogTargets /\ cursor[t] = committedThrough[t]]
    /\ heldAtStart' = hold
    /\ frontier' = [t \in Tables |-> t \in CatalogTargets /\ cursor[t] = committedThrough[t]]
    /\ delta' = {}
    /\ roundOutcome' = "none"
    /\ rsc' = IF condemned THEN 1 ELSE 0          \* the two-phase pacing tick
    /\ UNCHANGED << storeVars, laneVars, foldVars, holdVars, condemned, deleted, destroyedUnderHold >>

(* ONE strict hint enumeration per round (spec §5) — and it is a ZERO-TRUST hint. It may return any
   present log with an id above the last it returned, and it may stop at any time, including
   immediately: `ScanComplete` has no exhaustion guard, because there is nothing to exhaust. The
   hint feeds NOTHING into `delta`; folding a hinted key directly would fold a record whose
   predecessors are unproven. `maxSeen` remains diagnostic state and an input only to the isolated
   skipped-target sabotage. *)
HintReturn(t) ==
    /\ ~HintSilent /\ ~HintComplete
    /\ gcPhase = "scanning"
    /\ \E i \in DurableIds(t) :
         /\ i > maxSeen[t]
         /\ maxSeen' = [maxSeen EXCEPT ![t] = i]
    /\ UNCHANGED << storeVars, laneVars, cand, csnap, frontier, roundCte, targetResolved,
                    heldAtStart, delta, gcPhase, roundOutcome,
                    foldVars, holdVars, blobVars >>

(* The fold's real advance: point-read the expected next id. A seal advances the walk without
   producing a delta record; a log above the round's starting cursor becomes a candidate. *)
WalkStep(t) ==
    /\ gcPhase = "scanning"
    /\ t \in CatalogTargets
    /\ LET nxt == cand[t] + 1
           key == [t |-> t, i |-> nxt]
       IN /\ nxt <= MaxSeq
          /\ nxt <= roundCte[t]
          /\ Present(t, nxt)
          /\ cand' = [cand EXCEPT ![t] = nxt]
          /\ delta' = IF key \in durable /\ nxt > csnap[t] THEN delta \cup {key} ELSE delta
          /\ targetResolved' = [targetResolved EXCEPT ![t] = (nxt = roundCte[t])]
          /\ frontier' = [frontier EXCEPT ![t] = (nxt = roundCte[t])]
    /\ UNCHANGED << storeVars, laneVars, csnap, maxSeen, roundCte, heldAtStart,
                    gcPhase, roundOutcome, foldVars, holdVars, blobVars >>

(* A missing exact key AT OR BELOW the same-round CTE is corruption, independent of LIST. It resolves
   the target only as HELD, never as a frontier proof. A carried hold therefore exact-retries its
   offending key even when the next hint is silent, and the missing read reasserts the same hold. *)
ProbeAbsent(t) ==
    /\ gcPhase = "scanning"
    /\ t \in CatalogTargets
    /\ LET nxt == cand[t] + 1 IN
       /\ nxt <= roundCte[t]
       /\ ~Present(t, nxt)
       /\ targetResolved' = [targetResolved EXCEPT ![t] = TRUE]
       /\ IF SabotageClearHoldOnAbsent /\ hold[t]
          THEN /\ hold' = [hold EXCEPT ![t] = FALSE]
               /\ holdPos' = [holdPos EXCEPT ![t] = 0]
               /\ holdDebt' = [holdDebt EXCEPT ![t] = holdPos[t]]
               /\ frontier' = [frontier EXCEPT ![t] = TRUE]
          ELSE /\ hold' = [hold EXCEPT ![t] = TRUE]
               /\ holdPos' = [holdPos EXCEPT ![t] = nxt]
               /\ frontier' = [frontier EXCEPT ![t] = FALSE]
               /\ UNCHANGED holdDebt
    /\ UNCHANGED << storeVars, laneVars, cand, csnap, maxSeen, roundCte, heldAtStart,
                    delta, gcPhase, roundOutcome, foldVars, blobVars >>

(* Honestly the cursor stays untouched until the commit actually wins. SabotageAdoptBeforeCommit
   makes the candidate cursor live here, before the win/lose is decided. *)
ScanComplete ==
    /\ gcPhase = "scanning"
    /\ \A t \in CatalogTargets :
         \/ targetResolved[t]
         \/ (SabotageSkipCatalogTarget /\ maxSeen[t] = 0)
         \/ (SabotageSkipHeldRetry /\ heldAtStart[t])
    /\ gcPhase' = "complete"
    /\ cursor' = IF SabotageAdoptBeforeCommit THEN cand ELSE cursor
    /\ UNCHANGED << storeVars, laneVars, cand, csnap, maxSeen, frontier, roundCte,
                    targetResolved, heldAtStart, delta, roundOutcome,
                    folded, dupFlag, holdVars, blobVars >>

(* The round's single `gc/state` CAS. Cursor adoption, the fold and hold clearing are ONE atomic
   step: spec §5 clears a hold by folding through its offending position AND ADOPTING that result,
   so a hold cannot be released by a round whose commit is about to lose. *)
FoldCommitWin ==
    /\ gcPhase = "complete"
    /\ cursor' = cand
    /\ dupFlag' = (dupFlag \/ ((delta \cap folded) # {}))
    /\ folded' = folded \cup delta
    /\ LET adopted == folded \cup delta
           cleared == { t \in Tables : hold[t] /\ [t |-> t, i |-> holdPos[t]] \in adopted }
           settled == { t \in Tables : holdDebt[t] # 0
                                       /\ [t |-> t, i |-> holdDebt[t]] \in adopted }
       IN /\ hold' = [t \in Tables |-> hold[t] /\ t \notin cleared]
          /\ holdPos' = [t \in Tables |-> IF t \in cleared THEN 0 ELSE holdPos[t]]
          /\ holdDebt' = [t \in Tables |-> IF t \in settled THEN 0 ELSE holdDebt[t]]
    /\ roundOutcome' = "won"
    /\ gcPhase' = "idle"
    /\ UNCHANGED << storeVars, laneVars, cand, csnap, maxSeen, frontier, roundCte,
                    targetResolved, heldAtStart, delta, blobVars >>

(* A losing commit adopts nothing: cursor, fold and holds carry over from before this round (I3).
   Checked against `csnap` rather than "cursor before this action" because under
   SabotageAdoptBeforeCommit the cursor may already have been bumped at ScanComplete. *)
FoldCommitLose ==
    /\ gcPhase = "complete"
    /\ roundOutcome' = "lost"
    /\ gcPhase' = "idle"
    /\ UNCHANGED << storeVars, laneVars, cand, csnap, maxSeen, frontier, roundCte,
                    targetResolved, heldAtStart, delta,
                    foldVars, holdVars, blobVars >>

(* ---- ref-object cleanup: storage housekeeping only, no fold-account effect ---- *)

(* Honestly requires BOTH: the adopted cursor covers the id (its delta cannot be lost) AND a
   published snapshot covers it. SabotageCleanupIgnoresCursor drops the cursor requirement, so
   cleanup may delete a log the fold has never accounted. Exact CTE now detects that as a committed
   gap and holds; the sabotage remains as the green regression for that conversion. *)
Cleanup(t, i) ==
    /\ [t |-> t, i |-> i] \in durable
    /\ i <= snap[t]
    /\ (SabotageCleanupIgnoresCursor \/ i <= cursor[t])
    /\ durable' = durable \ { [t |-> t, i |-> i] }
    /\ hidden' = hidden \ { [t |-> t, i |-> i] }
    /\ UNCHANGED << everDurable, sealed, laneVars, roundVars, foldVars, holdVars, blobVars >>

(* ---- destructive work: the frontier proof, the hold, two-phase pacing ---- *)

(* Destruction is the post-commit tail of a round that WON its CAS: `roundOutcome = "won"` with the
   round's proofs (`frontier`) still in hand, reset by the next BeginRound. A losing round destroys
   nothing — it adopted nothing, so it accounted nothing. *)
Proven == \A t \in CatalogTargets : frontier[t]
DestructiveGate ==
    /\ gcPhase = "idle"
    /\ roundOutcome = "won"
    /\ (Proven \/ SabotageDestroyUnderHold)
    /\ (~AnyHold \/ SabotageDestroyUnderHold)

Condemn ==
    /\ DestructiveGate
    /\ Indeg = 0
    /\ ~condemned /\ ~deleted
    /\ condemned' = TRUE
    /\ rsc' = 0
    /\ destroyedUnderHold' = (destroyedUnderHold \/ Encumbered)
    /\ UNCHANGED << storeVars, laneVars, roundVars, foldVars, holdVars, deleted >>

(* Two-phase pacing (`rsc >= 1`) plus the liveness re-check. The re-check is the SECOND half of
   spec §5's temporal lemma, and `_sab_deleteignoresindeg` shows the first half does not cover the
   whole window: an acked `+1` landing after its namespace's probe but BEFORE condemnation is
   invisible to that round (so in-degree reaches zero) and is folded normally by the NEXT round (so
   in-degree is back above zero when the delete runs). Writer-side rematerialization cannot help —
   the blob was LIVE when the writer added its ref. Only re-reading in-degree here stops it. *)
Delete ==
    /\ DestructiveGate
    /\ condemned /\ ~deleted
    /\ rsc >= 1
    /\ (Indeg = 0 \/ SabotageDeleteIgnoresIndeg)
    /\ deleted' = TRUE
    /\ destroyedUnderHold' = (destroyedUnderHold \/ Encumbered)
    /\ UNCHANGED << storeVars, laneVars, roundVars, foldVars, holdVars, condemned, rsc >>

(* ---- REBUILD (spec §7) ----

   Honest REBUILD rebuilds cursors and edges from catalog + `_ckpt` + arithmetic tails, condemns
   nothing and CARRIES EVERY HOLD VERBATIM — on this model's state that is the identity, so it is
   modelled only through its sabotaged form. `SabotageRebuildDropsHold` makes that forbidden release
   executable; `HoldReleaseRequiresFold` observes the resulting debt immediately. Exact CTE would
   re-detect the gap later, but that does not make dropping durable control state legal.

   A rebuild is its own operation, not a step inside a fold round: it discards the round's
   destructive authorization (`roundOutcome`) and its frontier proofs. Without that, REBUILD
   inherited the authorization of whatever round it interrupted. *)
Rebuild ==
    /\ SabotageRebuildDropsHold
    /\ gcPhase = "idle"
    /\ AnyHold
    /\ hold' = [t \in Tables |-> FALSE]
    /\ holdPos' = [t \in Tables |-> 0]
    /\ holdDebt' = [t \in Tables |-> IF hold[t] THEN holdPos[t] ELSE holdDebt[t]]
    /\ roundOutcome' = "none"
    /\ frontier' = [t \in Tables |-> FALSE]
    /\ UNCHANGED << storeVars, laneVars, cand, csnap, maxSeen, roundCte, targetResolved,
                    heldAtStart, delta, gcPhase,
                    foldVars, blobVars >>

(* Self-loop so bounded counters exhausting is not a TLC deadlock (house pattern). *)
NoOp == UNCHANGED vars

(* Task 5's sole ref-life row producer. Both ordinary GC and REBUILD first materialize exactly the
   `Live`/`Removing` ids of one immutable catalog cut. The four input adapters can attach coverage,
   hints, holds, or checkpoint/tail observations only after this set is frozen. *)
BuildRefWalkPlans ==
    /\ ~planBuilt
    /\ gcRefLives' = WalkablePlanIds
    /\ rebuildRefLives' = WalkablePlanIds
    /\ planBuilt' = TRUE
    /\ UNCHANGED << planCut, enriched >>
    /\ UNCHANGED legacyVars

AdapterEnrich(a, i) ==
    /\ PlanOnly
    /\ planBuilt
    /\ IF SabotageAdapterMints /\ i \notin gcRefLives
       THEN /\ gcRefLives' = gcRefLives \cup {i}
            /\ rebuildRefLives' = rebuildRefLives \cup {i}
       ELSE /\ i \in (gcRefLives \cap rebuildRefLives)
            /\ UNCHANGED << gcRefLives, rebuildRefLives >>
    /\ enriched' = [enriched EXCEPT ![a] = @ \cup {i}]
    /\ UNCHANGED << planCut, planBuilt >>
    /\ UNCHANGED legacyVars

PlanNoOp ==
    /\ PlanOnly
    /\ UNCHANGED vars

LegacyNext ==
    \/ \E t \in Tables :
         \/ WAppendStart(t) \/ WAppendDurable(t) \/ WAppendAbandon(t)
         \/ EpochSeal(t) \/ WRaiseSnap(t)
         \/ HintReturn(t) \/ WalkStep(t) \/ ProbeAbsent(t)
    \/ \E t \in Tables, i \in Ids : Cleanup(t, i) \/ HideLog(t, i)
    \/ RevealLog
    \/ BeginRound \/ ScanComplete \/ FoldCommitWin \/ FoldCommitLose
    \/ Condemn \/ Delete \/ Rebuild
    \/ NoOp

Next ==
    \/ /\ ~PlanOnly
       /\ LegacyNext
       /\ UNCHANGED planVars
    \/ BuildRefWalkPlans
    \/ \E a \in Adapters, i \in PlanIds : AdapterEnrich(a, i)
    \/ PlanNoOp

Spec == Init /\ [][Next]_vars

(* ---- invariants ---- *)

TypeOK ==
    /\ durable \subseteq AllKeys
    /\ everDurable \subseteq AllKeys
    /\ durable \subseteq everDurable
    /\ hidden \subseteq durable
    /\ sealed \in [Tables -> SUBSET Ids]
    /\ \A t \in Tables : EverIds(t) \cap sealed[t] = {}   \* no slot is both a log and a seal
    /\ delta \subseteq AllKeys
    /\ folded \subseteq AllKeys
    /\ pendingId \in [Tables -> 0..MaxSeq]
    /\ snap \in [Tables -> 0..MaxSeq]
    /\ committedThrough \in [Tables -> 0..MaxSeq]
    /\ cursor \in [Tables -> 0..MaxSeq]
    /\ cand \in [Tables -> 0..MaxSeq]
    /\ csnap \in [Tables -> 0..MaxSeq]
    /\ maxSeen \in [Tables -> 0..MaxSeq]
    /\ frontier \in [Tables -> BOOLEAN]
    /\ roundCte \in [Tables -> 0..MaxSeq]
    /\ targetResolved \in [Tables -> BOOLEAN]
    /\ heldAtStart \in [Tables -> BOOLEAN]
    /\ gcPhase \in {"idle", "scanning", "complete"}
    /\ roundOutcome \in {"none", "won", "lost"}
    /\ dupFlag \in BOOLEAN
    /\ hold \in [Tables -> BOOLEAN]
    /\ holdPos \in [Tables -> 0..(MaxSeq + 1)]
    /\ holdDebt \in [Tables -> 0..(MaxSeq + 1)]
    /\ \A t \in Tables : hold[t] <=> (holdPos[t] # 0)
    /\ condemned \in BOOLEAN
    /\ deleted \in BOOLEAN
    /\ rsc \in 0..1
    /\ destroyedUnderHold \in BOOLEAN
    /\ planCut \in [PlanIds -> {"creating", "live", "removing", "absent"}]
    /\ gcRefLives \subseteq PlanIds
    /\ rebuildRefLives \subseteq PlanIds
    /\ planBuilt \in BOOLEAN
    /\ enriched \in [Adapters -> SUBSET PlanIds]
    /\ Indeg >= 0                    \* the cfg's EdgeOf keeps honest in-degree non-negative

(* (I1) The adopted cursor never passes a durable-but-unfolded log, even if that log was later
   deleted -- everDurable, not durable, is the oracle. *)
NoMissedFold == \A k \in everDurable : (k.i <= cursor[k.t]) => (k \in folded)

(* (I4, the pool-wide one) The shared blob's bytes are never deleted while an acknowledged `+1`
   names it. This model's `EdgeOf` gives the blob exactly one removal — T2's, matching the
   pre-existing base edge — so an acked add is never legitimately retracted: once one is durable,
   deleting the blob IS the data loss, whether or not the fold happens to have accounted it.

   Stronger than "acked but unfolded", deliberately. The weaker form cannot see the case where a
   `+1` landed after condemnation and WAS folded, yet a pending exact-token delete still fired —
   which is the whole subject of the delete-time liveness re-check. This is the property the
   frontier proof, the hold and the two-phase pacing exist to buy, and the one no per-namespace
   invariant can see. *)
NoAckedLoss == deleted => (\A k \in everDurable : EdgeOf[k] # "add")

(* (I5) No destructive step ever ran while a namespace was ENCUMBERED — held, or carrying a debt: a
   hold whose offending position was released without folding through it (spec §5: a hold "clears
   ONLY by folding through that position and adopting the result ... never by observing another
   absent"; §7: REBUILD carries every hold verbatim).

   Stated over a sticky ghost rather than the shape `(\E t : hold[t]) => ~deleted`, because that
   shape is not the property: a hold SET after a legitimate deletion would falsify it, and a hold
   DROPPED before an illegitimate one — which is exactly the r8 blocker — would satisfy it. What
   must never happen is the destructive STEP, taken while the namespace's accounting is open. The
   debt is carried rather than flagged at the drop so that the two hold sabotages must actually
   reach a destructive step to be caught, instead of being red by definition. *)
HoldSuppresses == ~destroyedUnderHold

(* No hold may be released before its offending position is folded and adopted. The debt ghost is
   introduced only by the two isolated release sabotages. *)
HoldReleaseRequiresFold == ~AnyDebt

(* (I2) Every key is adopted into folded[] at most once. *)
ExactlyOnce == ~dupFlag

(* (I3) A losing commit's round adopts nothing: if this round's outcome is "lost", the cursor
   still equals the value captured at BeginRound. *)
LosingCommitAdoptsNothing == (roundOutcome = "lost") => (cursor = csnap)

(* Exactness is the Task 5 proof boundary, stated over the cut rather than the later mutable catalog.
   It excludes `Creating` and absent ids even if every adapter names them. REBUILD's equality proves
   it calls the same constructor rather than growing a second admission predicate. *)
PlanKeySetExact ==
    planBuilt => (gcRefLives = WalkablePlanIds)

RebuildPlanKeySetExact ==
    planBuilt => (rebuildRefLives = WalkablePlanIds)

AdaptersEnrichOnly ==
    \A a \in Adapters : enriched[a] \subseteq (gcRefLives \cap rebuildRefLives)

(* Task 5b: the round cannot complete until every catalog-admitted target has either reached the
   frozen CTE or exact-read a missing required key and reasserted its hold. *)
EveryCatalogTargetAttempted ==
    (gcPhase = "complete") => (\A t \in CatalogTargets : targetResolved[t])

(* A carried hold is a mandatory retry even when LIST omits the life. *)
EveryCarriedHoldRetried ==
    (gcPhase = "complete") =>
        (\A t \in CatalogTargets : heldAtStart[t] => targetResolved[t])

(* The checkpoint authority is exact and monotone: it names every acknowledged slot, including
   seals, and the immutable same-round sample cannot move backwards relative to durable authority. *)
CteExact == \A t \in Tables : committedThrough[t] = MaxDurable(t)
RoundCteNoRegression == \A t \in Tables : roundCte[t] <= committedThrough[t]

WITNESS_PLAN_BUILT == ~planBuilt

=============================================================================
