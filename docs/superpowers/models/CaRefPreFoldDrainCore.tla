-------------------- MODULE CaRefPreFoldDrainCore --------------------
(* Pre-fold catalog-drain core.

   One adopted predecessor ref-life row and one current catalog row are enough to model the
   cross-object race. Actor A may issue the catalog CAS and then lose the GC lease. Actor B may take
   over while A's storage request is still in flight. The protocol makes B drain the proof carried by
   the authoritative parent seal, and conclusively resolve every eligible catalog CAS, before B may
   complete the hot stream LIST, take one fresh catalog cut, build the ref-walk plan, DEFER, fold, or
   REBUILD. Therefore no successor seal can invalidate A's proof before A's request either lands or
   loses its exact catalog token.

   No physical delete occurs here. The pre-fold drain mutates only the catalog; the model records only
   whether the successor stream key is LIST-visible, while predecessor bytes remain visible and the
   perpetual janitor and orphan sweeps own all byte reclamation. Catalog observations carry opaque
   life identity separately from `Creating`/`Live`/`Removing` state, and plan membership is a set of
   life ids. The same owner exports the fresh post-LIST cut as a token/value pair to a tiny ref-plan
   consumer boundary; the Delta-intake lifecycle is not copied here. The same-name rebirth abstraction
   records a fresh opaque life id, catalog `Creating`, catalog `Live`, the first stream `PUT`, and a
   legal transition to `Removing`. *)
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
    SabotageAbsentListedDefers,
    SabotagePredecessorDeletesSuccessor

Actors == {"A", "B"}
Phases == {"idle", "parent", "observed", "issued", "uncertain", "conflict", "resolved",
           "earlycut", "listed", "listedearly", "cut", "planned", "done"}
RowKinds == {"none", "unproved", "ready", "held"}
LifeIds == {"predecessor_life", "successor_life"}
CatalogLifeIds == LifeIds \cup {"none"}
CatalogStates == {"absent", "creating", "live", "removing"}

PredecessorLife == "predecessor_life"
SuccessorLife == "successor_life"
Walkable(state) == state \in {"live", "removing"}
PlanLives(life, state) == IF Walkable(state) THEN {life} ELSE {}

CatalogRowValid(life, state) == (life = "none") <=> (state = "absent")

VARIABLES
    catalogLifeId,
    catalogState,
    successorStreamPresent,
    catalogToken,
    noiseDone,
    adoptedValid,
    adoptedGeneration,
    adoptedLifeId,
    adoptedRow,
    authorityLossDone,
    leaseOwner,
    leaseSeq,
    phase,
    parentGeneration,
    parentLifeId,
    parentRow,
    observedToken,
    observedLifeId,
    observedState,
    cutLifeId,
    cutState,
    cutToken,
    cutAfterList,
    intakeCut,
    advancedWithDebt,
    deletedWithoutCurrentProof,
    nonExactDelete,
    predecessorProofDeletedSuccessor

vars ==
    << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
       adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
       leaseOwner, leaseSeq, phase, parentGeneration, parentLifeId, parentRow,
       observedToken, observedLifeId, observedState,
       cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
       advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
       predecessorProofDeletedSuccessor >>

Init ==
    /\ catalogLifeId = PredecessorLife
    /\ catalogState = "removing"
    /\ successorStreamPresent = FALSE
    /\ catalogToken = 1
    /\ noiseDone = FALSE
    /\ adoptedValid = TRUE
    /\ adoptedGeneration = 1
    /\ adoptedLifeId = PredecessorLife
    /\ adoptedRow = "ready"
    /\ authorityLossDone = FALSE
    /\ leaseOwner = "none"
    /\ leaseSeq = 0
    /\ phase = [a \in Actors |-> "idle"]
    /\ parentGeneration = [a \in Actors |-> 0]
    /\ parentLifeId = [a \in Actors |-> "none"]
    /\ parentRow = [a \in Actors |-> "none"]
    /\ observedToken = [a \in Actors |-> 0]
    /\ observedLifeId = [a \in Actors |-> "none"]
    /\ observedState = [a \in Actors |-> "absent"]
    /\ cutLifeId = [a \in Actors |-> "none"]
    /\ cutState = [a \in Actors |-> "absent"]
    /\ cutToken = [a \in Actors |-> 0]
    /\ cutAfterList = [a \in Actors |-> FALSE]
    /\ intakeCut = [a \in Actors |->
                       [catalog_token |-> 0,
                        life_id |-> "none",
                        state |-> "absent",
                        list_complete |-> FALSE,
                        listed_predecessor |-> FALSE,
                        listed_successor |-> FALSE,
                        plan_lives |-> {},
                        dead_debris_count |-> 0,
                        defer_for_dead_debris |-> FALSE]]
    /\ advancedWithDebt = FALSE
    /\ deletedWithoutCurrentProof = FALSE
    /\ nonExactDelete = FALSE
    /\ predecessorProofDeletedSuccessor = FALSE

Current(a) == leaseOwner = a
CurrentProofFor(life) ==
    adoptedValid /\ adoptedLifeId = life /\ adoptedRow = "ready"

PlanRecord(a, token, life, state, planLives, deferForDebris) ==
    [catalog_token |-> token,
     life_id |-> life,
     state |-> state,
     list_complete |-> intakeCut[a].list_complete,
     listed_predecessor |-> intakeCut[a].listed_predecessor,
     listed_successor |-> intakeCut[a].listed_successor,
     plan_lives |-> planLives,
     dead_debris_count |->
         IF intakeCut[a].listed_predecessor /\ life # PredecessorLife THEN 1 ELSE 0,
     defer_for_dead_debris |-> deferForDebris]

Acquire(a) ==
    /\ a \in Actors
    /\ phase[a] = "idle"
    /\ leaseSeq < 2
    /\ leaseOwner' = a
    /\ leaseSeq' = leaseSeq + 1
    /\ phase' = [phase EXCEPT ![a] = "parent"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

ReadAdoptedParent(a) ==
    /\ Current(a)
    /\ phase[a] = "parent"
    /\ adoptedValid
    /\ parentGeneration' = [parentGeneration EXCEPT ![a] = adoptedGeneration]
    /\ parentLifeId' = [parentLifeId EXCEPT ![a] = adoptedLifeId]
    /\ parentRow' = [parentRow EXCEPT ![a] = adoptedRow]
    /\ phase' = [phase EXCEPT ![a] = "observed"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

ReadDrainCatalog(a) ==
    /\ Current(a)
    /\ phase[a] = "observed"
    /\ observedToken' = [observedToken EXCEPT ![a] = catalogToken]
    /\ observedLifeId' = [observedLifeId EXCEPT ![a] = catalogLifeId]
    /\ observedState' = [observedState EXCEPT ![a] = catalogState]
    /\ phase' = [phase EXCEPT
                     ![a] = IF parentRow[a] = "ready"
                                  /\ parentLifeId[a] = catalogLifeId
                                  /\ catalogState = "removing"
                              THEN "issued" ELSE "resolved"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

CatalogNoise ==
    /\ ~noiseDone
    /\ catalogLifeId = PredecessorLife
    /\ catalogState = "removing"
    /\ catalogToken' = catalogToken + 1
    /\ noiseDone' = TRUE
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, phase, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

DeleteSuccess(a) ==
    /\ phase[a] = "issued"
    /\ observedState[a] = "removing"
    /\ catalogLifeId = observedLifeId[a]
    /\ catalogState = observedState[a]
    /\ catalogToken = observedToken[a]
    /\ catalogLifeId' = "none"
    /\ catalogState' = "absent"
    /\ catalogToken' = catalogToken + 1
    /\ phase' = [phase EXCEPT ![a] = "resolved"]
    /\ deletedWithoutCurrentProof' =
         (deletedWithoutCurrentProof \/ ~CurrentProofFor(observedLifeId[a]))
    /\ nonExactDelete' =
         (nonExactDelete
          \/ ~(catalogLifeId = observedLifeId[a]
               /\ catalogState = observedState[a]
               /\ catalogToken = observedToken[a]))
    /\ predecessorProofDeletedSuccessor' =
         (predecessorProofDeletedSuccessor
          \/ (catalogLifeId = SuccessorLife /\ parentLifeId[a] = PredecessorLife))
    /\ UNCHANGED
         << successorStreamPresent, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut, advancedWithDebt >>

DeleteUnknownLanded(a) ==
    /\ phase[a] = "issued"
    /\ observedState[a] = "removing"
    /\ catalogLifeId = observedLifeId[a]
    /\ catalogState = observedState[a]
    /\ catalogToken = observedToken[a]
    /\ catalogLifeId' = "none"
    /\ catalogState' = "absent"
    /\ catalogToken' = catalogToken + 1
    /\ phase' = [phase EXCEPT ![a] = "uncertain"]
    /\ deletedWithoutCurrentProof' =
         (deletedWithoutCurrentProof \/ ~CurrentProofFor(observedLifeId[a]))
    /\ nonExactDelete' =
         (nonExactDelete
          \/ ~(catalogLifeId = observedLifeId[a]
               /\ catalogState = observedState[a]
               /\ catalogToken = observedToken[a]))
    /\ predecessorProofDeletedSuccessor' =
         (predecessorProofDeletedSuccessor
          \/ (catalogLifeId = SuccessorLife /\ parentLifeId[a] = PredecessorLife))
    /\ UNCHANGED
         << successorStreamPresent, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut, advancedWithDebt >>

DeleteUnknownNotLanded(a) ==
    /\ phase[a] = "issued"
    /\ phase' = [phase EXCEPT ![a] = "uncertain"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

DeleteConflict(a) ==
    /\ phase[a] = "issued"
    /\ \/ catalogLifeId # observedLifeId[a]
       \/ catalogState # observedState[a]
       \/ catalogToken # observedToken[a]
    /\ phase' = [phase EXCEPT ![a] = "conflict"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

ResolveAbsent(a) ==
    /\ phase[a] \in {"uncertain", "conflict"}
    /\ catalogLifeId = "none"
    /\ catalogState = "absent"
    /\ phase' = [phase EXCEPT ![a] = "resolved"]
    /\ observedToken' = [observedToken EXCEPT ![a] = catalogToken]
    /\ observedLifeId' = [observedLifeId EXCEPT ![a] = catalogLifeId]
    /\ observedState' = [observedState EXCEPT ![a] = catalogState]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

ResolveSameRemoving(a) ==
    /\ phase[a] \in {"uncertain", "conflict"}
    /\ catalogLifeId = observedLifeId[a]
    /\ catalogState = "removing"
    /\ observedToken' = [observedToken EXCEPT ![a] = catalogToken]
    /\ observedLifeId' = [observedLifeId EXCEPT ![a] = catalogLifeId]
    /\ observedState' = [observedState EXCEPT ![a] = catalogState]
    /\ phase' = [phase EXCEPT ![a] = "observed"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

(* Keep the stale predecessor observation for the non-vacuity witness. A different opaque life id
   resolves the request without acting on that current row. *)
ResolveDifferentLife(a) ==
    /\ phase[a] \in {"uncertain", "conflict"}
    /\ catalogLifeId \in LifeIds
    /\ catalogLifeId # observedLifeId[a]
    /\ phase' = [phase EXCEPT ![a] = "resolved"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

BeginSameNameRebirth ==
    /\ catalogLifeId = "none"
    /\ catalogState = "absent"
    /\ ~successorStreamPresent
    /\ catalogLifeId' = SuccessorLife
    /\ catalogState' = "creating"
    /\ catalogToken' = catalogToken + 1
    /\ UNCHANGED
         << successorStreamPresent, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, phase, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

PublishRebornLive ==
    /\ catalogLifeId = SuccessorLife
    /\ catalogState = "creating"
    /\ catalogState' = "live"
    /\ catalogToken' = catalogToken + 1
    /\ UNCHANGED
         << catalogLifeId, successorStreamPresent, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, phase, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

PutRebornStream ==
    /\ catalogLifeId = SuccessorLife
    /\ catalogState = "live"
    /\ ~successorStreamPresent
    /\ successorStreamPresent' = TRUE
    /\ UNCHANGED
         << catalogLifeId, catalogState, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, phase, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

BeginRebornRemoval ==
    /\ catalogLifeId = SuccessorLife
    /\ catalogState = "live"
    /\ successorStreamPresent
    /\ catalogState' = "removing"
    /\ catalogToken' = catalogToken + 1
    /\ UNCHANGED
         << catalogLifeId, successorStreamPresent, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, phase, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

CompleteHotList(a) ==
    /\ Current(a)
    /\ phase[a] = "resolved"
    /\ \E includeSuccessor \in BOOLEAN :
         /\ includeSuccessor => successorStreamPresent
         /\ intakeCut' = [intakeCut EXCEPT
                              ![a].list_complete = TRUE,
                              ![a].listed_predecessor = TRUE,
                              ![a].listed_successor = includeSuccessor]
    /\ phase' = [phase EXCEPT ![a] = "listed"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

TakeFreshCut(a) ==
    /\ Current(a)
    /\ phase[a] = "listed"
    /\ cutLifeId' = [cutLifeId EXCEPT ![a] = catalogLifeId]
    /\ cutState' = [cutState EXCEPT ![a] = catalogState]
    /\ cutToken' = [cutToken EXCEPT ![a] = catalogToken]
    /\ cutAfterList' = [cutAfterList EXCEPT ![a] = TRUE]
    /\ phase' = [phase EXCEPT ![a] = "cut"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

(* Consequential old-order mutation. It captures absence before rebirth, then cannot complete LIST
   until the successor has become `Live` and published its stream. The stale cut is consumed only
   after LIST, so the semantic missed-current-life invariant is the primary RED. *)
TakeFreshCutBeforeList(a) ==
    /\ SabotageCutBeforeList
    /\ Current(a)
    /\ phase[a] = "resolved"
    /\ catalogLifeId = "none"
    /\ catalogState = "absent"
    /\ cutLifeId' = [cutLifeId EXCEPT ![a] = catalogLifeId]
    /\ cutState' = [cutState EXCEPT ![a] = catalogState]
    /\ cutToken' = [cutToken EXCEPT ![a] = catalogToken]
    /\ cutAfterList' = [cutAfterList EXCEPT ![a] = FALSE]
    /\ phase' = [phase EXCEPT ![a] = "earlycut"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

CompleteHotListAfterEarlyCut(a) ==
    /\ SabotageCutBeforeList
    /\ Current(a)
    /\ phase[a] = "earlycut"
    /\ catalogLifeId = SuccessorLife
    /\ catalogState = "live"
    /\ successorStreamPresent
    /\ intakeCut' = [intakeCut EXCEPT
                         ![a].list_complete = TRUE,
                         ![a].listed_predecessor = TRUE,
                         ![a].listed_successor = TRUE]
    /\ phase' = [phase EXCEPT ![a] = "listedearly"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

BuildRefWalkPlan(a) ==
    /\ Current(a)
    /\ phase[a] = "cut"
    /\ intakeCut' = [intakeCut EXCEPT
                         ![a] = PlanRecord(a, cutToken[a], cutLifeId[a], cutState[a],
                                                  PlanLives(cutLifeId[a], cutState[a]), FALSE)]
    /\ phase' = [phase EXCEPT ![a] = "planned"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

BuildRefWalkPlanFromEarlyCut(a) ==
    /\ SabotageCutBeforeList
    /\ Current(a)
    /\ phase[a] = "listedearly"
    /\ catalogLifeId = SuccessorLife
    /\ Walkable(catalogState)
    /\ intakeCut' = [intakeCut EXCEPT
                         ![a] = PlanRecord(a, cutToken[a], cutLifeId[a], cutState[a],
                                                  PlanLives(cutLifeId[a], cutState[a]), FALSE)]
    /\ phase' = [phase EXCEPT ![a] = "planned"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

BuildRefWalkPlanDeferringDeadDebris(a) ==
    /\ SabotageAbsentListedDefers
    /\ Current(a)
    /\ phase[a] = "cut"
    /\ intakeCut[a].listed_predecessor
    /\ cutLifeId[a] # PredecessorLife
    /\ intakeCut' = [intakeCut EXCEPT
                         ![a] = PlanRecord(a, cutToken[a], cutLifeId[a], cutState[a],
                                                  PlanLives(cutLifeId[a], cutState[a]), TRUE)]
    /\ phase' = [phase EXCEPT ![a] = "planned"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

Defer(a) ==
    /\ Current(a)
    /\ phase[a] = "planned"
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

AdoptFromCut(a) ==
    /\ Current(a)
    /\ phase[a] = "planned"
    /\ ~intakeCut[a].defer_for_dead_debris
    /\ \E nextRow \in IF intakeCut[a].plan_lives # {} THEN {"unproved", "ready", "held"}
                     ELSE {"none"} :
         /\ adoptedRow' = nextRow
         /\ adoptedLifeId' = IF intakeCut[a].plan_lives # {} THEN cutLifeId[a] ELSE "none"
         /\ adoptedGeneration' = adoptedGeneration + 1
    /\ adoptedValid' = TRUE
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            authorityLossDone, leaseOwner, leaseSeq,
            parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

FoldBypassDrain(a) ==
    /\ SabotageFoldBypassesDrain
    /\ Current(a)
    /\ phase[a] \in {"issued", "uncertain", "conflict"}
    /\ parentRow[a] = "ready"
    /\ catalogLifeId = PredecessorLife
    /\ catalogState = "removing"
    /\ adoptedGeneration' = adoptedGeneration + 1
    /\ adoptedLifeId' = PredecessorLife
    /\ adoptedRow' = "held"
    /\ adoptedValid' = TRUE
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ advancedWithDebt' = TRUE
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            authorityLossDone, leaseOwner, leaseSeq,
            parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

RebuildBypassDrain(a) ==
    /\ SabotageRebuildBypassesDrain
    /\ Current(a)
    /\ phase[a] \in {"issued", "uncertain", "conflict"}
    /\ parentRow[a] = "ready"
    /\ catalogLifeId = PredecessorLife
    /\ catalogState = "removing"
    /\ adoptedGeneration' = adoptedGeneration + 1
    /\ adoptedLifeId' = PredecessorLife
    /\ adoptedRow' = "held"
    /\ adoptedValid' = TRUE
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ advancedWithDebt' = TRUE
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            authorityLossDone, leaseOwner, leaseSeq,
            parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

DeferBypassDrain(a) ==
    /\ SabotageDeferBypassesDrain
    /\ Current(a)
    /\ phase[a] \in {"issued", "uncertain", "conflict"}
    /\ parentRow[a] = "ready"
    /\ catalogLifeId = PredecessorLife
    /\ catalogState = "removing"
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ advancedWithDebt' = TRUE
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

ContinueAfterUnknown(a) ==
    /\ SabotageContinueAfterUnknown
    /\ Current(a)
    /\ phase[a] = "uncertain"
    /\ parentRow[a] = "ready"
    /\ catalogLifeId = PredecessorLife
    /\ catalogState = "removing"
    /\ cutLifeId' = [cutLifeId EXCEPT ![a] = catalogLifeId]
    /\ cutState' = [cutState EXCEPT ![a] = catalogState]
    /\ cutToken' = [cutToken EXCEPT ![a] = catalogToken]
    /\ cutAfterList' = [cutAfterList EXCEPT ![a] = FALSE]
    /\ phase' = [phase EXCEPT ![a] = "cut"]
    /\ advancedWithDebt' = TRUE
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState, intakeCut,
            deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

AdoptHeldOverDeposedRequest(a) ==
    /\ SabotageStaleDeleteAfterSuccessorHold
    /\ Current(a)
    /\ \E old \in Actors \ {a} : phase[old] = "issued"
    /\ catalogLifeId = PredecessorLife
    /\ catalogState = "removing"
    /\ adoptedGeneration' = adoptedGeneration + 1
    /\ adoptedLifeId' = PredecessorLife
    /\ adoptedRow' = "held"
    /\ adoptedValid' = TRUE
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            authorityLossDone, leaseOwner, leaseSeq,
            parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

LoseAuthority(a) ==
    /\ Current(a)
    /\ phase[a] = "parent"
    /\ ~authorityLossDone
    /\ \A other \in Actors \ {a} : phase[other] \notin {"issued", "uncertain", "conflict"}
    /\ adoptedValid' = FALSE
    /\ authorityLossDone' = TRUE
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedGeneration, adoptedLifeId, adoptedRow,
            leaseOwner, leaseSeq, phase, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

RebuildAuthorityOnly(a) ==
    /\ Current(a)
    /\ phase[a] = "parent"
    /\ ~adoptedValid
    /\ adoptedValid' = TRUE
    /\ adoptedGeneration' = adoptedGeneration + 1
    /\ adoptedLifeId' = IF catalogState = "removing" THEN catalogLifeId ELSE "none"
    /\ adoptedRow' = IF catalogState = "removing" THEN "ready" ELSE "none"
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            authorityLossDone, leaseOwner, leaseSeq,
            parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

RebuildFromUnadoptedSealDeletes(a) ==
    /\ SabotageRebuildFromUnadoptedSeal
    /\ Current(a)
    /\ phase[a] = "parent"
    /\ ~adoptedValid
    /\ catalogState = "removing"
    /\ catalogLifeId' = "none"
    /\ catalogState' = "absent"
    /\ catalogToken' = catalogToken + 1
    /\ phase' = [phase EXCEPT ![a] = "done"]
    /\ deletedWithoutCurrentProof' = TRUE
    /\ UNCHANGED
         << successorStreamPresent, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, nonExactDelete, predecessorProofDeletedSuccessor >>

IntakeUsesPreDrainCut(a) ==
    /\ SabotageIntakeUsesPreDrainCut
    /\ Current(a)
    /\ phase[a] = "cut"
    /\ observedToken[a] # 0
    /\ intakeCut' = [intakeCut EXCEPT
                         ![a] = PlanRecord(a, observedToken[a], observedLifeId[a], observedState[a],
                                                  PlanLives(observedLifeId[a], observedState[a]), FALSE)]
    /\ phase' = [phase EXCEPT ![a] = "planned"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

AdoptFromCutWithStaleToken(a) ==
    /\ SabotageIntakeUsesStaleToken
    /\ Current(a)
    /\ phase[a] = "cut"
    /\ observedToken[a] # 0
    /\ observedToken[a] # cutToken[a]
    /\ intakeCut' = [intakeCut EXCEPT
                         ![a] = PlanRecord(a, observedToken[a], cutLifeId[a], cutState[a],
                                                  PlanLives(cutLifeId[a], cutState[a]), FALSE)]
    /\ phase' = [phase EXCEPT ![a] = "planned"]
    /\ UNCHANGED
         << catalogLifeId, catalogState, successorStreamPresent, catalogToken, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList,
            advancedWithDebt, deletedWithoutCurrentProof, nonExactDelete,
            predecessorProofDeletedSuccessor >>

(* Identity-blind mutation: an old-life proof deletes same-name successor `Removing`. The honest
   exact row/token action cannot take this transition. *)
DeleteSuccessorWithPredecessorProof(a) ==
    /\ SabotagePredecessorDeletesSuccessor
    /\ phase[a] = "issued"
    /\ parentLifeId[a] = PredecessorLife
    /\ observedLifeId[a] = PredecessorLife
    /\ catalogLifeId = SuccessorLife
    /\ catalogState = "removing"
    /\ catalogLifeId' = "none"
    /\ catalogState' = "absent"
    /\ catalogToken' = catalogToken + 1
    /\ phase' = [phase EXCEPT ![a] = "resolved"]
    /\ predecessorProofDeletedSuccessor' = TRUE
    /\ nonExactDelete' = TRUE
    /\ UNCHANGED
         << successorStreamPresent, noiseDone,
            adoptedValid, adoptedGeneration, adoptedLifeId, adoptedRow, authorityLossDone,
            leaseOwner, leaseSeq, parentGeneration, parentLifeId, parentRow,
            observedToken, observedLifeId, observedState,
            cutLifeId, cutState, cutToken, cutAfterList, intakeCut,
            advancedWithDebt, deletedWithoutCurrentProof >>

NoOp == UNCHANGED vars

Next ==
    \/ \E a \in Actors :
         \/ Acquire(a) \/ ReadAdoptedParent(a) \/ ReadDrainCatalog(a)
         \/ DeleteSuccess(a) \/ DeleteUnknownLanded(a) \/ DeleteUnknownNotLanded(a)
         \/ DeleteConflict(a) \/ ResolveAbsent(a) \/ ResolveSameRemoving(a) \/ ResolveDifferentLife(a)
         \/ CompleteHotList(a) \/ TakeFreshCut(a)
         \/ TakeFreshCutBeforeList(a) \/ CompleteHotListAfterEarlyCut(a)
         \/ BuildRefWalkPlan(a) \/ BuildRefWalkPlanFromEarlyCut(a)
         \/ BuildRefWalkPlanDeferringDeadDebris(a)
         \/ Defer(a) \/ AdoptFromCut(a)
         \/ FoldBypassDrain(a) \/ RebuildBypassDrain(a) \/ DeferBypassDrain(a)
         \/ ContinueAfterUnknown(a) \/ AdoptHeldOverDeposedRequest(a)
         \/ LoseAuthority(a) \/ RebuildAuthorityOnly(a) \/ RebuildFromUnadoptedSealDeletes(a)
         \/ IntakeUsesPreDrainCut(a) \/ AdoptFromCutWithStaleToken(a)
         \/ DeleteSuccessorWithPredecessorProof(a)
    \/ CatalogNoise \/ BeginSameNameRebirth \/ PublishRebornLive \/ PutRebornStream
    \/ BeginRebornRemoval \/ NoOp

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ catalogLifeId \in CatalogLifeIds
    /\ catalogState \in CatalogStates
    /\ CatalogRowValid(catalogLifeId, catalogState)
    /\ successorStreamPresent \in BOOLEAN
    /\ catalogToken \in 1..7
    /\ noiseDone \in BOOLEAN
    /\ adoptedValid \in BOOLEAN
    /\ adoptedGeneration \in 1..3
    /\ adoptedLifeId \in CatalogLifeIds
    /\ adoptedRow \in RowKinds
    /\ authorityLossDone \in BOOLEAN
    /\ leaseOwner \in Actors \cup {"none"}
    /\ leaseSeq \in 0..2
    /\ phase \in [Actors -> Phases]
    /\ parentGeneration \in [Actors -> 0..3]
    /\ parentLifeId \in [Actors -> CatalogLifeIds]
    /\ parentRow \in [Actors -> RowKinds]
    /\ observedToken \in [Actors -> 0..7]
    /\ observedLifeId \in [Actors -> CatalogLifeIds]
    /\ observedState \in [Actors -> CatalogStates]
    /\ \A a \in Actors : CatalogRowValid(observedLifeId[a], observedState[a])
    /\ cutLifeId \in [Actors -> CatalogLifeIds]
    /\ cutState \in [Actors -> CatalogStates]
    /\ \A a \in Actors : CatalogRowValid(cutLifeId[a], cutState[a])
    /\ cutToken \in [Actors -> 0..7]
    /\ cutAfterList \in [Actors -> BOOLEAN]
    /\ intakeCut \in [Actors ->
                        [catalog_token : 0..7,
                         life_id : CatalogLifeIds,
                         state : CatalogStates,
                         list_complete : BOOLEAN,
                         listed_predecessor : BOOLEAN,
                         listed_successor : BOOLEAN,
                         plan_lives : SUBSET LifeIds,
                         dead_debris_count : 0..1,
                         defer_for_dead_debris : BOOLEAN]]
    /\ \A a \in Actors : CatalogRowValid(intakeCut[a].life_id, intakeCut[a].state)
    /\ advancedWithDebt \in BOOLEAN
    /\ deletedWithoutCurrentProof \in BOOLEAN
    /\ nonExactDelete \in BOOLEAN
    /\ predecessorProofDeletedSuccessor \in BOOLEAN

DrainBeforeDecision == ~advancedWithDebt
DeleteUsesCurrentAdoptedProof == ~deletedWithoutCurrentProof
ExactCatalogCAS == ~nonExactDelete
PredecessorProofCannotDeleteSuccessorRemoving == ~predecessorProofDeletedSuccessor

(* Checked when the plan actually consumes the cut. The semantic stale-cut invariant is primary;
   this provenance bit keeps explicit order as an additional assertion at that same transition. *)
FreshCutFollowsCompletedHotList ==
    \A a \in Actors :
        intakeCut[a].catalog_token # 0 =>
            intakeCut[a].list_complete /\ cutAfterList[a]

DeadListedPredecessorIsInert ==
    \A a \in Actors :
        phase[a] \in {"planned", "done"}
        /\ intakeCut[a].listed_predecessor
        /\ intakeCut[a].life_id # PredecessorLife
        => /\ intakeCut[a].dead_debris_count = 1
           /\ PredecessorLife \notin intakeCut[a].plan_lives
           /\ ~intakeCut[a].defer_for_dead_debris

(* Consequence of the post-LIST cut: a successor which the completed LIST returned and which is
   currently walkable cannot be classified as dead or omitted by a stale cut. *)
ListedCurrentLifeIsAdmitted ==
    \A a \in Actors :
        phase[a] \in {"planned", "done"}
        /\ intakeCut[a].listed_successor
        /\ catalogLifeId = SuccessorLife
        /\ Walkable(catalogState)
        => /\ SuccessorLife \in intakeCut[a].plan_lives
           /\ ~intakeCut[a].defer_for_dead_debris

SuccessorRemovingIsAdmitted ==
    \A a \in Actors :
        phase[a] \in {"planned", "done"}
        /\ intakeCut[a].life_id = SuccessorLife
        /\ intakeCut[a].state = "removing"
        => /\ intakeCut[a].plan_lives = {SuccessorLife}
           /\ PredecessorLife \notin intakeCut[a].plan_lives

IntakeConsumesFreshPostDrainCut ==
    \A a \in Actors :
        intakeCut[a].catalog_token # 0 =>
            /\ intakeCut[a].catalog_token = cutToken[a]
            /\ intakeCut[a].life_id = cutLifeId[a]
            /\ intakeCut[a].state = cutState[a]
            /\ intakeCut[a].plan_lives = PlanLives(intakeCut[a].life_id, intakeCut[a].state)

WITNESS_TAKEOVER_CONVERGES ==
    ~(catalogLifeId = "none"
      /\ catalogState = "absent"
      /\ leaseOwner = "B"
      /\ phase["B"] = "done"
      /\ adoptedLifeId = "none"
      /\ adoptedRow = "none"
      /\ phase["A"] = "resolved"
      /\ observedLifeId["A"] = "none"
      /\ observedState["A"] = "absent"
      /\ observedLifeId["B"] = PredecessorLife
      /\ observedState["B"] = "removing")

WITNESS_DRAINED_ROW_ABSENT_FROM_INTAKE ==
    ~(\E a \in Actors :
        /\ phase[a] = "done"
        /\ catalogLifeId = "none"
        /\ catalogState = "absent"
        /\ observedLifeId[a] = PredecessorLife
        /\ observedState[a] = "removing"
        /\ cutToken[a] > observedToken[a]
        /\ intakeCut[a].catalog_token = cutToken[a]
        /\ intakeCut[a].life_id = "none"
        /\ intakeCut[a].state = "absent"
        /\ intakeCut[a].plan_lives = {})

(* One trace covers all identity non-vacuity: A keeps an issued predecessor request; B deletes the
   predecessor; the same name is reborn, publishes a stream, then legally becomes `Removing`; B's
   later cut admits only the successor while old bytes remain listed; A's stale request returns a
   conflict without deleting the successor; and B adopts instead of perpetually deferring. *)
WITNESS_REBIRTH_WITH_RETAINED_DEBRIS_ADOPTS ==
    ~( /\ leaseOwner = "B"
       /\ phase["A"] = "conflict"
       /\ parentLifeId["A"] = PredecessorLife
       /\ observedLifeId["A"] = PredecessorLife
       /\ observedState["A"] = "removing"
       /\ observedToken["A"] < catalogToken
       /\ phase["B"] = "done"
       /\ catalogLifeId = SuccessorLife
       /\ catalogState = "removing"
       /\ successorStreamPresent
       /\ intakeCut["B"].list_complete
       /\ intakeCut["B"].listed_predecessor
       /\ intakeCut["B"].listed_successor
       /\ cutLifeId["B"] = SuccessorLife
       /\ cutState["B"] = "removing"
       /\ intakeCut["B"].dead_debris_count = 1
       /\ intakeCut["B"].plan_lives = {SuccessorLife}
       /\ ~intakeCut["B"].defer_for_dead_debris
       /\ adoptedGeneration > parentGeneration["B"]
       /\ adoptedLifeId = SuccessorLife
       /\ adoptedRow \in {"unproved", "ready", "held"}
       /\ ~predecessorProofDeletedSuccessor)

=============================================================================
