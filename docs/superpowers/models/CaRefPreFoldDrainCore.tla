-------------------- MODULE CaRefPreFoldDrainCore --------------------
(* Pre-fold catalog-drain core.

   One `Removing` catalog row and one adopted ref-life row are enough to model the cross-object
   race. Actor A may issue the catalog CAS and then lose the GC lease. Actor B may take over while
   A's storage request is still in flight. The protocol makes B drain the proof carried by the
   authoritative parent seal, and conclusively resolve every eligible catalog CAS, before B may
   take a fresh catalog cut, DEFER, fold, or REBUILD. Therefore no successor seal can invalidate
   A's proof before A's request either lands or loses its exact catalog token.

   Physical objects deliberately do not occur here. The pre-fold drain mutates only the catalog;
   the perpetual janitor and orphan sweeps own all byte reclamation. *)
EXTENDS Integers, FiniteSets

CONSTANTS
    SabotageFoldBypassesDrain,
    SabotageRebuildBypassesDrain,
    SabotageDeferBypassesDrain,
    SabotageContinueAfterUnknown,
    SabotageStaleDeleteAfterSuccessorHold,
    SabotageRebuildFromUnadoptedSeal

Actors == {"A", "B"}
Phases == {"idle", "parent", "observed", "issued", "uncertain", "resolved", "cut", "done"}
RowKinds == {"none", "unproved", "ready", "held"}
EntryKinds == {"none", "removing", "absent"}

VARIABLES
    entry,                 \* `cas/ref_catalog`: the one exact row, or absence
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
    advancedWithDebt,      \* sticky audit: DEFER/fold/REBUILD crossed an unresolved ready parent
    deletedWithoutCurrentProof, \* sticky audit: catalog CAS landed after authority/proof changed
    nonExactDelete         \* sticky audit: a catalog delete did not consume its exact observation

vars == << entry, catalogToken, noiseDone,
           adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
           leaseOwner, leaseSeq, phase, parentGeneration, parentRow,
           observedToken, observedEntry, cutEntry,
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
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* The exact catalog observation is taken after the authoritative parent. *)
ReadDrainCatalog(a) ==
    /\ Current(a)
    /\ phase[a] = "observed"
    /\ observedToken' = [observedToken EXCEPT ![a] = catalogToken]
    /\ observedEntry' = [observedEntry EXCEPT ![a] = entry]
    /\ phase' = [phase EXCEPT ![a] = IF parentRow[a] = "ready" /\ entry = "removing"
                                         THEN "issued" ELSE "resolved"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow, cutEntry,
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
                    observedToken, observedEntry, cutEntry,
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
                    observedToken, observedEntry, cutEntry, advancedWithDebt >>

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
                    observedToken, observedEntry, cutEntry, advancedWithDebt >>

DeleteUnknownNotLanded(a) ==
    /\ phase[a] = "issued"
    /\ phase' = [phase EXCEPT ![a] = "uncertain"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry,
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
                    observedToken, observedEntry, cutEntry,
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
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

(* This is the barrier: no fresh cut exists before resolution. *)
TakeFreshCut(a) ==
    /\ Current(a)
    /\ phase[a] = "resolved"
    /\ cutEntry' = [cutEntry EXCEPT ![a] = entry]
    /\ phase' = [phase EXCEPT ![a] = "cut"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

Defer(a) ==
    /\ Current(a)
    /\ phase[a] = "cut"
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry,
                    advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete >>

AdoptFromCut(a) ==
    /\ Current(a)
    /\ phase[a] = "cut"
    /\ \E nextRow \in IF cutEntry[a] = "absent" THEN {"none"}
                     ELSE {"unproved", "ready", "held"} :
         /\ adoptedRow' = nextRow
         /\ adoptedGeneration' = adoptedGeneration + 1
    /\ adoptedValid' = TRUE
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ UNCHANGED << entry, catalogToken, noiseDone, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry, cutEntry,
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
                    observedToken, observedEntry, cutEntry,
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
                    observedToken, observedEntry, cutEntry,
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
                    observedToken, observedEntry, cutEntry,
                    deletedWithoutCurrentProof, nonExactDelete >>

ContinueAfterUnknown(a) ==
    /\ SabotageContinueAfterUnknown
    /\ Current(a)
    /\ phase[a] = "uncertain"
    /\ parentRow[a] = "ready"
    /\ entry = "removing"
    /\ cutEntry' = [cutEntry EXCEPT ![a] = entry]
    /\ phase' = [phase EXCEPT ![a] = "cut"]
    /\ advancedWithDebt' = TRUE
    /\ UNCHANGED << entry, catalogToken, noiseDone,
                    adoptedValid, adoptedGeneration, adoptedRow, authorityLossDone,
                    leaseOwner, leaseSeq, parentGeneration, parentRow,
                    observedToken, observedEntry,
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
                    observedToken, observedEntry, cutEntry,
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
                    observedToken, observedEntry, cutEntry,
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
                    observedToken, observedEntry, cutEntry,
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
                    observedToken, observedEntry, cutEntry,
                    advancedWithDebt, nonExactDelete >>

NoOp == UNCHANGED vars

Next ==
    \/ \E a \in Actors :
         \/ Acquire(a) \/ ReadAdoptedParent(a) \/ ReadDrainCatalog(a)
         \/ DeleteSuccess(a) \/ DeleteUnknownLanded(a) \/ DeleteUnknownNotLanded(a)
         \/ DeleteConflict(a) \/ ResolveAbsent(a) \/ ResolveSameRemoving(a)
         \/ TakeFreshCut(a) \/ Defer(a) \/ AdoptFromCut(a)
         \/ FoldBypassDrain(a) \/ RebuildBypassDrain(a) \/ DeferBypassDrain(a)
         \/ ContinueAfterUnknown(a) \/ AdoptHeldOverDeposedRequest(a)
         \/ LoseAuthority(a) \/ RebuildAuthorityOnly(a) \/ RebuildFromUnadoptedSealDeletes(a)
    \/ CatalogNoise
    \/ NoOp

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ entry \in {"removing", "absent"}
    /\ catalogToken \in 1..3
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
    /\ observedToken \in [Actors -> 0..3]
    /\ observedEntry \in [Actors -> EntryKinds]
    /\ cutEntry \in [Actors -> EntryKinds]
    /\ advancedWithDebt \in BOOLEAN
    /\ deletedWithoutCurrentProof \in BOOLEAN
    /\ nonExactDelete \in BOOLEAN

DrainBeforeDecision == ~advancedWithDebt
DeleteUsesCurrentAdoptedProof == ~deletedWithoutCurrentProof
ExactCatalogCAS == ~nonExactDelete

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

=============================================================================
