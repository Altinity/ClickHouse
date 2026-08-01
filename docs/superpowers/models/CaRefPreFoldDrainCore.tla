-------------------- MODULE CaRefPreFoldDrainCore --------------------
(* Pre-fold catalog-drain core.

   One `Removing` catalog row and one adopted ref-life row are enough to model the cross-object
   race. Actor A may issue the catalog CAS and then lose the GC lease. Actor B may take over while
   A's storage request is still in flight. The protocol makes B drain the proof carried by the
   authoritative parent seal, and conclusively resolve every eligible catalog CAS, before B may
   complete the hot stream LIST, take one fresh catalog cut, build the ref-walk plan, DEFER, fold,
   or REBUILD. Therefore no successor seal can invalidate A's proof before A's request either lands
   or loses its exact catalog token.

   No physical delete occurs here. The pre-fold drain mutates only the catalog; the model records
   only whether the old/new stream keys are LIST-visible, while the perpetual janitor and orphan
   sweeps own all byte reclamation. The same owner exports the fresh post-LIST cut as a token/value
   pair to a tiny ref-plan consumer boundary; the Delta-intake
   lifecycle is not copied here. The same-name rebirth abstraction records catalog `Creating`, `Live`
   publication, then the first stream `PUT`; retained predecessor bytes remain visible to every hot
   LIST but are not catalog authority. *)
EXTENDS Integers, FiniteSets

CONSTANTS
    SabotageFoldBypassesDrain,
    SabotageRebuildBypassesDrain,
    SabotageDeferBypassesDrain,
    SabotageContinueAfterUnknown,
    SabotageStaleDeleteAfterSuccessorHold,
    SabotageRebuildFromUnadoptedSeal,
    SabotageIntakeUsesPreDrainCut,
    SabotageIntakeUsesStaleToken,
    SabotageCutBeforeList,
    SabotageAbsentListedDefers

Actors == {"A", "B"}
Phases == {"idle", "parent", "observed", "issued", "uncertain", "resolved", "listed", "cut", "planned", "done"}
RowKinds == {"none", "unproved", "ready", "held"}
CatalogEntryKinds == {"none", "removing", "absent", "creating", "live"}
PhysicalEntryKinds == CatalogEntryKinds \cup {"live_stream"}

CatalogEntry(e) == IF e = "live_stream" THEN "live" ELSE e
Walkable(e) == e \in {"removing", "live"}

VARIABLES
    entry,                 \* catalog state; `live_stream` also records the successor's first stream PUT
    catalogToken,          \* full-object token; noise models an unrelated catalog-row mutation
    noiseDone,
    adoptedValid,          \* FALSE models missing/undecodable authoritative `gc/state`
    adoptedGeneration,
    adoptedRow,            \* the authoritative seal's row for this exact life
    authorityLossDone,
    leaseOwner,
    leaseSeq,
    phase,
    parentGeneration,
    parentRow,
    observedToken,
    observedEntry,
    cutEntry,
    cutToken,              \* full-catalog token paired with the sole fresh post-LIST cut
    intakeCut,             \* completed LIST plus the exact cut/token consumed by ref-plan
    advancedWithDebt,      \* sticky audit: DEFER/fold/REBUILD crossed an unresolved ready parent
    deletedWithoutCurrentProof, \* sticky audit: catalog CAS landed after authority/proof changed
    nonExactDelete         \* sticky audit: a catalog delete did not consume its exact observation

vars == << entry, catalogToken, noiseDone,
           adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
           leaseOwner, leaseSeq, phase, parentGeneration, parentRow,
           observedToken, observedEntry, cutEntry, cutToken, intakeCut,
           advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

Init ==
    /\ entry = "removing"
    /\ catalogToken = 1
    /\ noiseDone = FALSE
    /\ adoptedValid = TRUE
    /\ adoptedGeneration = 1
    /\ adoptedRow = "ready"       \* matching positive terminal evidence, no durable hold
    /\ authorityLossDone = FALSE
    /\ leaseOwner = "none"
    /\ leaseSeq = 0
    /\ phase = [a \in Actors |-> "idle"]
    /\ parentGeneration = [a \in Actors |-> 0]
    /\ parentRow = [a \in Actors |-> "none"]
    /\ observedToken = [a \in Actors |-> 0]
    /\ observedEntry = [a \in Actors |-> "none"]
    /\ cutEntry = [a \in Actors |-> "none"]
    /\ cutToken = [a \in Actors |-> 0]
    /\ intakeCut = [a \in Actors |->
                       [catalog_token |-> 0,
                        entry |-> "none",
                        list_complete |-> FALSE,
                        listed_predecessor |-> FALSE,
                        listed_successor |-> FALSE,
                        plan_has_life |-> FALSE,
                        plan_has_predecessor |-> FALSE,
                        plan_has_successor |-> FALSE,
                        dead_debris_count |-> 0,
                        defer_for_dead_debris |-> FALSE]]
    /\ advancedWithDebt = FALSE
    /\ deletedWithoutCurrentProof = FALSE
    /\ nonExactDelete = FALSE

Current(a) == leaseOwner = a
Eligible(a) == parentRow[a] = "ready" /\ observedEntry[a] = "removing"
CurrentProof == adoptedValid /\ adoptedRow = "ready"

(* Acquiring a later lease does not erase an older actor or cancel a request it already issued. *)
Acquire(a) ==
    /\ a \in Actors
    /\ phase[a] = "idle"
    /\ leaseSeq < 2
    /\ leaseOwner' = a
    /\ leaseSeq' = leaseSeq + 1
    /\ phase' = [phase EXCEPT ![a] = "parent"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    parentGeneration, parentRow, observedToken, observedEntry, cutEntry,
                    cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* The parent is read only after lease acquisition. A missing/undecodable authority is not replaced
   by a seal found through LIST. *)
ReadAdoptedParent(a) ==
    /\ Current(a)
    /\ phase[a] = "parent"
    /\ adoptedValid
    /\ parentGeneration' = [parentGeneration EXCEPT ![a] = adoptedGeneration]
    /\ parentRow' = [parentRow EXCEPT ![a] = adoptedRow]
    /\ phase' = [phase EXCEPT ![a] = "observed"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, observedToken, observedEntry, cutEntry,
                    cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* The exact catalog observation is taken after the authoritative parent. *)
ReadDrainCatalog(a) ==
    /\ Current(a)
    /\ phase[a] = "observed"
    /\ observedToken' = [observedToken EXCEPT ![a] = catalogToken]
    /\ observedEntry' = [observedEntry EXCEPT ![a] = CatalogEntry(entry)]
    /\ phase' = [phase EXCEPT ![a] = IF parentRow[a] = "ready" /\ entry = "removing"
                                         THEN "issued" ELSE "resolved"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow, cutEntry,
                    cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* A mutation elsewhere in the shared catalog invalidates the full-object token without changing
   this exact row. The drain must re-read and retry; a token conflict alone is not completion. *)
CatalogNoise ==
    /\ ~noiseDone
    /\ entry = "removing"
    /\ catalogToken' = catalogToken + 1
    /\ noiseDone' = TRUE
    /\ UNCHANGED << entry, adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, phase, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* A successful exact CAS may return after its actor was deposed. Whether the CURRENT adopted seal
   still carries the proof is recorded at the mutation, not inferred from the actor's old sample. *)
DeleteSuccess(a) ==
    /\ phase[a] = "issued"
    /\ observedEntry[a] = "removing"
    /\ entry = observedEntry[a]
    /\ catalogToken = observedToken[a]
    /\ entry' = "absent"
    /\ catalogToken' = catalogToken + 1
    /\ phase' = [phase EXCEPT ![a] = "resolved"]
    /\ deletedWithoutCurrentProof' = (deletedWithoutCurrentProof \/ ~CurrentProof)
    /\ nonExactDelete' = (nonExactDelete \/
                           ~(entry = observedEntry[a] /\ catalogToken = observedToken[a]))
    /\ UNCHANGED << noiseDone, adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut, advancedWithDebt >>

(* An ambiguous response has two store outcomes. Neither permits progress until an exact re-read
   resolves it. *)
DeleteUnknownLanded(a) ==
    /\ phase[a] = "issued"
    /\ observedEntry[a] = "removing"
    /\ entry = observedEntry[a]
    /\ catalogToken = observedToken[a]
    /\ entry' = "absent"
    /\ catalogToken' = catalogToken + 1
    /\ phase' = [phase EXCEPT ![a] = "uncertain"]
    /\ deletedWithoutCurrentProof' = (deletedWithoutCurrentProof \/ ~CurrentProof)
    /\ nonExactDelete' = (nonExactDelete \/
                           ~(entry = observedEntry[a] /\ catalogToken = observedToken[a]))
    /\ UNCHANGED << noiseDone, adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut, advancedWithDebt >>

DeleteUnknownNotLanded(a) ==
    /\ phase[a] = "issued"
    /\ phase' = [phase EXCEPT ![a] = "uncertain"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* A definite token conflict is still followed by the same exact resolution path. *)
DeleteConflict(a) ==
    /\ phase[a] = "issued"
    /\ \/ catalogToken # observedToken[a]
       \/ entry # observedEntry[a]
    /\ phase' = [phase EXCEPT ![a] = "uncertain"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

ResolveAbsent(a) ==
    /\ phase[a] = "uncertain"
    /\ entry = "absent"
    (* The resolution itself performs a complete catalog GET. An actor that has lost the lease cannot
       issue another read, so this one-row core records that completed observation and resolves; the
       all-row companion proves that a globally continuing drain returns to `scan` before deciding. *)
    /\ phase' = [phase EXCEPT ![a] = "resolved"]
    /\ observedToken' = [observedToken EXCEPT ![a] = catalogToken]
    /\ observedEntry' = [observedEntry EXCEPT ![a] = entry]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow, cutEntry,
                    cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* The same complete `Removing` row under a new full-catalog token remains eligible. Return through
   the observation phase, rather than treating this row-local retry as a completed whole drain. *)
ResolveSameRemoving(a) ==
    /\ phase[a] = "uncertain"
    /\ entry = "removing"
    /\ observedToken' = [observedToken EXCEPT ![a] = catalogToken]
    /\ observedEntry' = [observedEntry EXCEPT ![a] = entry]
    /\ phase' = [phase EXCEPT ![a] = "observed"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow, cutEntry,
                    cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* A same-name successor is a different exact catalog row. `Creating` precedes every life object;
   `Live` publication precedes the first stream PUT. The physical state is folded into `entry` only
   to keep this focused owner small; `CatalogEntry` removes that physical bit from catalog cuts. *)
BeginSameNameRebirth ==
    /\ entry = "absent"
    /\ entry' = "creating"
    /\ catalogToken' = catalogToken + 1
    /\ UNCHANGED << noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, phase, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

PublishRebornLive ==
    /\ entry = "creating"
    /\ entry' = "live"
    /\ catalogToken' = catalogToken + 1
    /\ UNCHANGED << noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, phase, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

PutRebornStream ==
    /\ entry = "live"
    /\ entry' = "live_stream"
    /\ UNCHANGED << catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, phase, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* A stale request that now sees a different same-name life resolves; it cannot act on that row. *)
ResolveDifferentLife(a) ==
    /\ phase[a] = "uncertain"
    /\ CatalogEntry(entry) \in {"creating", "live"}
    /\ observedToken' = [observedToken EXCEPT ![a] = catalogToken]
    /\ observedEntry' = [observedEntry EXCEPT ![a] = CatalogEntry(entry)]
    /\ phase' = [phase EXCEPT ![a] = "resolved"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow, cutEntry,
                    cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* The old predecessor stream is deliberately retained and always returned. The successor key may
   be omitted by a weak LIST, but can be returned only after its post-Live stream PUT. *)
CompleteHotList(a) ==
    /\ Current(a)
    /\ phase[a] = "resolved"
    /\ \E includeSuccessor \in BOOLEAN :
         /\ includeSuccessor => entry = "live_stream"
         /\ intakeCut' = [intakeCut EXCEPT
                              ![a].list_complete = TRUE,
                              ![a].listed_predecessor = TRUE,
                              ![a].listed_successor = includeSuccessor]
    /\ phase' = [phase EXCEPT ![a] = "listed"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* This is the barrier: the one fresh cut exists only after drain resolution and completed LIST. *)
TakeFreshCut(a) ==
    /\ Current(a)
    /\ phase[a] = "listed"
    /\ cutEntry' = [cutEntry EXCEPT ![a] = CatalogEntry(entry)]
    /\ cutToken' = [cutToken EXCEPT ![a] = catalogToken]
    /\ phase' = [phase EXCEPT ![a] = "cut"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* Load-bearing ordering sabotage: the old pre-amendment cut is taken before the hot LIST. *)
TakeFreshCutBeforeList(a) ==
    /\ SabotageCutBeforeList
    /\ Current(a)
    /\ phase[a] = "resolved"
    /\ cutEntry' = [cutEntry EXCEPT ![a] = CatalogEntry(entry)]
    /\ cutToken' = [cutToken EXCEPT ![a] = catalogToken]
    /\ phase' = [phase EXCEPT ![a] = "cut"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

BuildRefWalkPlan(a) ==
    /\ Current(a)
    /\ phase[a] = "cut"
    /\ intakeCut' = [intakeCut EXCEPT
                         ![a].catalog_token = cutToken[a],
                         ![a].entry = cutEntry[a],
                         ![a].plan_has_life = Walkable(cutEntry[a]),
                         ![a].plan_has_predecessor = (cutEntry[a] = "removing"),
                         ![a].plan_has_successor = (cutEntry[a] = "live"),
                         ![a].dead_debris_count =
                             IF intakeCut[a].listed_predecessor /\ cutEntry[a] # "removing" THEN 1 ELSE 0,
                         ![a].defer_for_dead_debris = FALSE]
    /\ phase' = [phase EXCEPT ![a] = "planned"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* Rejected classifier: a listed id absent from the later cut is called unknown and forces DEFER. *)
BuildRefWalkPlanDeferringDeadDebris(a) ==
    /\ SabotageAbsentListedDefers
    /\ Current(a)
    /\ phase[a] = "cut"
    /\ intakeCut[a].listed_predecessor
    /\ cutEntry[a] # "removing"
    /\ intakeCut' = [intakeCut EXCEPT
                         ![a].catalog_token = cutToken[a],
                         ![a].entry = cutEntry[a],
                         ![a].plan_has_life = Walkable(cutEntry[a]),
                         ![a].plan_has_predecessor = FALSE,
                         ![a].plan_has_successor = (cutEntry[a] = "live"),
                         ![a].dead_debris_count = 1,
                         ![a].defer_for_dead_debris = TRUE]
    /\ phase' = [phase EXCEPT ![a] = "planned"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

Defer(a) ==
    /\ Current(a)
    /\ phase[a] = "planned"
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

AdoptFromCut(a) ==
    /\ Current(a)
    /\ phase[a] = "planned"
    /\ ~intakeCut[a].defer_for_dead_debris
    /\ \E nextRow \in IF Walkable(cutEntry[a]) THEN {"unproved", "ready", "held"}
                     ELSE {"none"} :
         /\ adoptedRow' = nextRow
         /\ adoptedGeneration' = adoptedGeneration + 1
    /\ adoptedValid' = TRUE
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ UNCHANGED << entry, catalogToken, noiseDone, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* Three isolated omissions. Their sticky ghost makes the ordering claim executable even for
   DEFER, which publishes no successor seal and would otherwise leave no state witness. *)
FoldBypassDrain(a) ==
    /\ SabotageFoldBypassesDrain
    /\ Current(a)
    /\ phase[a] \in {"issued", "uncertain"}
    /\ parentRow[a] = "ready"
    /\ entry = "removing"
    /\ adoptedGeneration' = adoptedGeneration + 1
    /\ adoptedRow' = "held"
    /\ adoptedValid' = TRUE
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ advancedWithDebt' = TRUE
    /\ UNCHANGED << entry, catalogToken, noiseDone, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut,
                    deletedWithoutCurrentProof, nonExactDelete >>

RebuildBypassDrain(a) ==
    /\ SabotageRebuildBypassesDrain
    /\ Current(a)
    /\ phase[a] \in {"issued", "uncertain"}
    /\ parentRow[a] = "ready"
    /\ entry = "removing"
    /\ adoptedGeneration' = adoptedGeneration + 1
    /\ adoptedRow' = "held"
    /\ adoptedValid' = TRUE
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ advancedWithDebt' = TRUE
    /\ UNCHANGED << entry, catalogToken, noiseDone, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut,
                    deletedWithoutCurrentProof, nonExactDelete >>

DeferBypassDrain(a) ==
    /\ SabotageDeferBypassesDrain
    /\ Current(a)
    /\ phase[a] \in {"issued", "uncertain"}
    /\ parentRow[a] = "ready"
    /\ entry = "removing"
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ advancedWithDebt' = TRUE
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut,
                    deletedWithoutCurrentProof, nonExactDelete >>

ContinueAfterUnknown(a) ==
    /\ SabotageContinueAfterUnknown
    /\ Current(a)
    /\ phase[a] = "uncertain"
    /\ parentRow[a] = "ready"
    /\ entry = "removing"
    /\ cutEntry' = [cutEntry EXCEPT ![a] = entry]
    /\ cutToken' = [cutToken EXCEPT ![a] = catalogToken]
    /\ phase' = [phase EXCEPT ![a] = "cut"]
    /\ advancedWithDebt' = TRUE
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, intakeCut,
                    deletedWithoutCurrentProof, nonExactDelete >>

(* Exact reproduction of the rejected post-adoption finalizer: A has an issued request from ready
   S1; B publishes held S2 without draining; A's old exact catalog token still matches and lands. *)
AdoptHeldOverDeposedRequest(a) ==
    /\ SabotageStaleDeleteAfterSuccessorHold
    /\ Current(a)
    /\ \E old \in Actors \ {a} : phase[old] = "issued"
    /\ entry = "removing"
    /\ adoptedGeneration' = adoptedGeneration + 1
    /\ adoptedRow' = "held"
    /\ adoptedValid' = TRUE
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ UNCHANGED << entry, catalogToken, noiseDone, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* Missing authority may be rebuilt, but that invocation returns after restoring authority. It does
   not infer deletion authority from a seal discovered by enumeration. *)
LoseAuthority(a) ==
    /\ Current(a)
    /\ phase[a] = "parent"
    /\ ~authorityLossDone
    /\ \A other \in Actors \ {a} : phase[other] \notin {"issued", "uncertain"}
    /\ adoptedValid' = FALSE
    /\ authorityLossDone' = TRUE
    /\ UNCHANGED << entry, catalogToken, noiseDone, adoptedGeneration, adoptedRow,
                    leaseOwner, leaseSeq, phase, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

RebuildAuthorityOnly(a) ==
    /\ Current(a)
    /\ phase[a] = "parent"
    /\ ~adoptedValid
    /\ adoptedValid' = TRUE
    /\ adoptedGeneration' = adoptedGeneration + 1
    /\ adoptedRow' = IF entry = "removing" THEN "ready" ELSE "none"
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ UNCHANGED << entry, catalogToken, noiseDone, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

RebuildFromUnadoptedSealDeletes(a) ==
    /\ SabotageRebuildFromUnadoptedSeal
    /\ Current(a)
    /\ phase[a] = "parent"
    /\ ~adoptedValid
    /\ entry = "removing"
    /\ entry' = "absent"
    /\ catalogToken' = catalogToken + 1
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ deletedWithoutCurrentProof' = TRUE
    /\ UNCHANGED << noiseDone, adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken, intakeCut,
                    advancedWithDebt, nonExactDelete >>

(* Composition sabotage: the drain and fresh-cut transition are honest, but the ref-plan consumer
   is wired to the earlier drain observation. This isolates cut provenance from lifecycle ordering. *)
IntakeUsesPreDrainCut(a) ==
    /\ SabotageIntakeUsesPreDrainCut
    /\ Current(a)
    /\ phase[a] = "cut"
    /\ observedToken[a] # 0
    /\ intakeCut' = [intakeCut EXCEPT
                         ![a].catalog_token = observedToken[a],
                         ![a].entry = observedEntry[a],
                         ![a].plan_has_life = Walkable(observedEntry[a]),
                         ![a].plan_has_predecessor = (observedEntry[a] = "removing"),
                         ![a].plan_has_successor = FALSE,
                         ![a].dead_debris_count = 0,
                         ![a].defer_for_dead_debris = FALSE]
    /\ phase' = [phase EXCEPT ![a] = "planned"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* Token-only composition sabotage at the real plan/adoption seam. Adoption and the plan consume the
   fresh row value, but intake carries the earlier full-catalog token for that same value. *)
AdoptFromCutWithStaleToken(a) ==
    /\ SabotageIntakeUsesStaleToken
    /\ Current(a)
    /\ phase[a] = "cut"
    /\ observedToken[a] # 0
    /\ observedToken[a] # cutToken[a]
    /\ intakeCut' = [intakeCut EXCEPT
                         ![a].catalog_token = observedToken[a],
                         ![a].entry = cutEntry[a],
                         ![a].plan_has_life = Walkable(cutEntry[a]),
                         ![a].plan_has_predecessor = (cutEntry[a] = "removing"),
                         ![a].plan_has_successor = (cutEntry[a] = "live"),
                         ![a].dead_debris_count =
                             IF intakeCut[a].listed_predecessor /\ cutEntry[a] # "removing" THEN 1 ELSE 0,
                         ![a].defer_for_dead_debris = FALSE]
    /\ phase' = [phase EXCEPT ![a] = "planned"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry, cutToken,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

NoOp == UNCHANGED vars

Next ==
    \/ \E a \in Actors :
         \/ Acquire(a) \/ ReadAdoptedParent(a) \/ ReadDrainCatalog(a)
         \/ DeleteSuccess(a) \/ DeleteUnknownLanded(a) \/ DeleteUnknownNotLanded(a)
         \/ DeleteConflict(a) \/ ResolveAbsent(a) \/ ResolveSameRemoving(a) \/ ResolveDifferentLife(a)
         \/ CompleteHotList(a) \/ TakeFreshCut(a) \/ TakeFreshCutBeforeList(a)
         \/ BuildRefWalkPlan(a) \/ BuildRefWalkPlanDeferringDeadDebris(a)
         \/ Defer(a) \/ AdoptFromCut(a)
         \/ FoldBypassDrain(a) \/ RebuildBypassDrain(a) \/ DeferBypassDrain(a)
         \/ ContinueAfterUnknown(a) \/ AdoptHeldOverDeposedRequest(a)
         \/ LoseAuthority(a) \/ RebuildAuthorityOnly(a) \/ RebuildFromUnadoptedSealDeletes(a)
         \/ IntakeUsesPreDrainCut(a) \/ AdoptFromCutWithStaleToken(a)
    \/ CatalogNoise \/ BeginSameNameRebirth \/ PublishRebornLive \/ PutRebornStream
    \/ NoOp

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ entry \in PhysicalEntryKinds
    /\ catalogToken \in 1..5
    /\ noiseDone \in BOOLEAN
    /\ adoptedValid \in BOOLEAN
    /\ adoptedGeneration \in 1..3
    /\ adoptedRow \in RowKinds
    /\ authorityLossDone \in BOOLEAN
    /\ leaseOwner \in Actors \cup {"none"}
    /\ leaseSeq \in 0..2
    /\ phase \in [Actors -> Phases]
    /\ parentGeneration \in [Actors -> 0..3]
    /\ parentRow \in [Actors -> RowKinds]
    /\ observedToken \in [Actors -> 0..5]
    /\ observedEntry \in [Actors -> CatalogEntryKinds]
    /\ cutEntry \in [Actors -> CatalogEntryKinds]
    /\ cutToken \in [Actors -> 0..5]
    /\ intakeCut \in [Actors ->
                        [catalog_token : 0..5,
                         entry : CatalogEntryKinds,
                         list_complete : BOOLEAN,
                         listed_predecessor : BOOLEAN,
                         listed_successor : BOOLEAN,
                         plan_has_life : BOOLEAN,
                         plan_has_predecessor : BOOLEAN,
                         plan_has_successor : BOOLEAN,
                         dead_debris_count : 0..1,
                         defer_for_dead_debris : BOOLEAN]]
    /\ advancedWithDebt \in BOOLEAN
    /\ deletedWithoutCurrentProof \in BOOLEAN
    /\ nonExactDelete \in BOOLEAN

DrainBeforeDecision == ~advancedWithDebt
DeleteUsesCurrentAdoptedProof == ~deletedWithoutCurrentProof
ExactCatalogCAS == ~nonExactDelete

(* The one authoritative round cut is post-LIST. A pre-LIST cut is not safe classification input. *)
FreshCutFollowsCompletedHotList ==
    \A a \in Actors : cutToken[a] # 0 => intakeCut[a].list_complete

(* A retained predecessor returned by LIST but absent from the later cut is counted and dropped. It
   neither enters the plan nor sets a dead-debris DEFER reason; a reborn `live` row is independent. *)
DeadListedPredecessorIsInert ==
    \A a \in Actors :
        phase[a] \in {"planned", "done"}
        /\ intakeCut[a].listed_predecessor
        /\ cutEntry[a] # "removing"
        => /\ intakeCut[a].dead_debris_count = 1
           /\ ~intakeCut[a].plan_has_predecessor
           /\ ~intakeCut[a].defer_for_dead_debris

(* The consumer must receive the same immutable full-catalog observation produced after the drain.
   Equality includes the token, so an earlier cut with the same row value is still stale. *)
IntakeConsumesFreshPostDrainCut ==
    \A a \in Actors :
        intakeCut[a].catalog_token # 0 =>
            /\ intakeCut[a].catalog_token = cutToken[a]
            /\ intakeCut[a].entry = cutEntry[a]
            /\ intakeCut[a].plan_has_life = Walkable(intakeCut[a].entry)
            /\ intakeCut[a].plan_has_predecessor = (intakeCut[a].entry = "removing")
            /\ intakeCut[a].plan_has_successor = (intakeCut[a].entry = "live")

(* Negated non-vacuity witness: A's exact request loses to B's completed drain, B adopts the
   catalog-absent successor, and A conclusively observes its conflict. *)
WITNESS_TAKEOVER_CONVERGES ==
    ~(entry = "absent"
      /\ leaseOwner = "B"
      /\ phase["B"] = "done"
      /\ adoptedRow = "none"
      /\ phase["A"] = "resolved"
      /\ observedEntry["A"] = "absent"    \* A resolved its stale request by re-read
      /\ observedEntry["B"] = "removing") \* B, not A, consumed the exact row

(* Negated non-vacuity witness for the composition boundary: a real exact deletion advances the
   catalog token, and the cut consumed by the plan omits the drained row. *)
WITNESS_DRAINED_ROW_ABSENT_FROM_INTAKE ==
    ~(\E a \in Actors :
        /\ phase[a] = "done"
        /\ entry = "absent"
        /\ observedEntry[a] = "removing"
        /\ cutToken[a] > observedToken[a]
        /\ intakeCut[a].catalog_token = cutToken[a]
        /\ intakeCut[a].entry = "absent"
        /\ ~intakeCut[a].plan_has_life)

(* Negated convergence witness: the catalog row is deleted, the same name is reborn through
   Creating/Live/stream PUT, the completed LIST still returns predecessor debris, and the later cut
   admits only the successor. The actor adopts instead of perpetually deferring on the old bytes. *)
WITNESS_REBIRTH_WITH_RETAINED_DEBRIS_ADOPTS ==
    ~(\E a \in Actors :
        /\ phase[a] = "done"
        /\ entry = "live_stream"
        /\ intakeCut[a].list_complete
        /\ intakeCut[a].listed_predecessor
        /\ intakeCut[a].listed_successor
        /\ cutEntry[a] = "live"
        /\ intakeCut[a].dead_debris_count = 1
        /\ ~intakeCut[a].plan_has_predecessor
        /\ intakeCut[a].plan_has_successor
        /\ ~intakeCut[a].defer_for_dead_debris
        /\ adoptedGeneration > parentGeneration[a]
        /\ adoptedRow \in {"unproved", "ready", "held"})

=============================================================================
