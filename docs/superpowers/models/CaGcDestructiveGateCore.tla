-------------------- MODULE CaGcDestructiveGateCore --------------------
(* Destructive GC gate core.

   This deliberately narrow model separates two authorization levels. Physical
   condemnation and physical deletion require an authoritative, non-empty,
   completely proven catalog frontier with no anomaly and no carried hold.
   Exact erasure of one `Removing` lifecycle row instead requires that row's
   local cleanup evidence, absence of a hold on that life, and a current GC
   fence. An unrelated whole-round suppressor must not invalidate the latter
   proof.

   Catalog mutation ordering, pre-fold proof drainage, delta intake, object
   codecs, ref-log walking, sharding, and physical delete families have their
   own models. Here they are fixed inputs so this module owns only the split
   between the physical gate and the exact lifecycle gate. *)
EXTENDS Integers

CONSTANTS
    Target,
    Other,
    CatalogUniverse,
    FrontierProven,
    Anomalies,
    CarriedHolds,
    TargetRemoving,
    TargetHasEvidence,
    TargetHeld,
    GcFenceCurrent,
    PhysicalCandidatePresent,
    SabotageGateOmitsAnomalies,
    SabotageGateOmitsHolds,
    SabotageGateOmitsFrontier,
    SabotageGateAcceptsEmptyUniverse,
    SabotageLifecycleUsesGlobalSuppression

Identities == {Target, Other}

ASSUME
    /\ Target # Other
    /\ CatalogUniverse \subseteq Identities
    /\ FrontierProven \subseteq CatalogUniverse
    /\ Anomalies \subseteq Identities
    /\ CarriedHolds \subseteq Identities
    /\ TargetRemoving \in BOOLEAN
    /\ TargetHasEvidence \in BOOLEAN
    /\ TargetHeld \in BOOLEAN
    /\ GcFenceCurrent \in BOOLEAN
    /\ PhysicalCandidatePresent \in BOOLEAN
    /\ SabotageGateOmitsAnomalies \in BOOLEAN
    /\ SabotageGateOmitsHolds \in BOOLEAN
    /\ SabotageGateOmitsFrontier \in BOOLEAN
    /\ SabotageGateAcceptsEmptyUniverse \in BOOLEAN
    /\ SabotageLifecycleUsesGlobalSuppression \in BOOLEAN

UniverseAuthoritative == TRUE

FrontierComplete ==
    /\ UniverseAuthoritative
    /\ CatalogUniverse # {}
    /\ FrontierProven = CatalogUniverse

PhysicalGateOpen ==
    /\ Anomalies = {}
    /\ CarriedHolds = {}
    /\ FrontierComplete

RemovalEraseProved ==
    /\ TargetRemoving
    /\ TargetHasEvidence
    /\ ~TargetHeld
    /\ GcFenceCurrent

(* Each physical sabotage removes exactly one honest conjunct. The empty-set
   control is intentionally distinct from frontier equality: `{}` equals `{}`
   while a destructive round must still fail closed. *)
ComputedPhysicalGateOpen ==
    /\ UniverseAuthoritative
    /\ (CatalogUniverse # {} \/ SabotageGateAcceptsEmptyUniverse)
    /\ (FrontierProven = CatalogUniverse \/ SabotageGateOmitsFrontier)
    /\ (Anomalies = {} \/ SabotageGateOmitsAnomalies)
    /\ (CarriedHolds = {} \/ SabotageGateOmitsHolds)

VARIABLES
    phase,
    gateComputed,
    gateOpen,
    condemned,
    physicallyDeleted,
    removalErased,
    lifecycleBlockedBySuppression,
    emptyEqualityObserved

vars ==
    <<phase,
      gateComputed,
      gateOpen,
      condemned,
      physicallyDeleted,
      removalErased,
      lifecycleBlockedBySuppression,
      emptyEqualityObserved>>

Init ==
    /\ phase = "selected"
    /\ gateComputed = FALSE
    /\ gateOpen = FALSE
    /\ condemned = FALSE
    /\ physicallyDeleted = FALSE
    /\ removalErased = FALSE
    /\ lifecycleBlockedBySuppression = FALSE
    /\ emptyEqualityObserved = FALSE

ComputeGate ==
    /\ phase = "selected"
    /\ phase' = "ready"
    /\ gateComputed' = TRUE
    /\ gateOpen' = ComputedPhysicalGateOpen
    /\ emptyEqualityObserved' =
          (CatalogUniverse = {} /\ FrontierProven = {} /\ FrontierProven = CatalogUniverse)
    /\ UNCHANGED
          <<condemned,
            physicallyDeleted,
            removalErased,
            lifecycleBlockedBySuppression>>

(* Condemnation is an irreversible physical decision in its own right, so it
   checks the gate independently of the later delete action. *)
CondemnPhysicalCandidate ==
    /\ phase = "ready"
    /\ gateComputed
    /\ gateOpen
    /\ PhysicalCandidatePresent
    /\ ~condemned
    /\ condemned' = TRUE
    /\ UNCHANGED
          <<phase,
            gateComputed,
            gateOpen,
            physicallyDeleted,
            removalErased,
            lifecycleBlockedBySuppression,
            emptyEqualityObserved>>

(* Physical deletion repeats the same gate check rather than inheriting
   authorization merely from the earlier condemnation. *)
PhysicallyDeleteCandidate ==
    /\ phase = "ready"
    /\ gateComputed
    /\ gateOpen
    /\ PhysicalCandidatePresent
    /\ condemned
    /\ ~physicallyDeleted
    /\ physicallyDeleted' = TRUE
    /\ UNCHANGED
          <<phase,
            gateComputed,
            gateOpen,
            condemned,
            removalErased,
            lifecycleBlockedBySuppression,
            emptyEqualityObserved>>

EraseProvedRemoval ==
    /\ phase = "ready"
    /\ RemovalEraseProved
    /\ ~removalErased
    /\ (~SabotageLifecycleUsesGlobalSuppression \/ gateOpen)
    /\ removalErased' = TRUE
    /\ UNCHANGED
          <<phase,
            gateComputed,
            gateOpen,
            condemned,
            physicallyDeleted,
            lifecycleBlockedBySuppression,
            emptyEqualityObserved>>

(* Negative control: incorrectly reuse the unrelated whole-round physical
   gate as a prerequisite for an otherwise proved exact lifecycle erasure. *)
BlockProvedRemovalByGlobalSuppression ==
    /\ phase = "ready"
    /\ RemovalEraseProved
    /\ SabotageLifecycleUsesGlobalSuppression
    /\ ~gateOpen
    /\ ~removalErased
    /\ ~lifecycleBlockedBySuppression
    /\ lifecycleBlockedBySuppression' = TRUE
    /\ UNCHANGED
          <<phase,
            gateComputed,
            gateOpen,
            condemned,
            physicallyDeleted,
            removalErased,
            emptyEqualityObserved>>

Finish ==
    /\ phase = "ready"
    /\ phase' = "done"
    /\ UNCHANGED
          <<gateComputed,
            gateOpen,
            condemned,
            physicallyDeleted,
            removalErased,
            lifecycleBlockedBySuppression,
            emptyEqualityObserved>>

Next ==
    \/ ComputeGate
    \/ CondemnPhysicalCandidate
    \/ PhysicallyDeleteCandidate
    \/ EraseProvedRemoval
    \/ BlockProvedRemovalByGlobalSuppression
    \/ Finish

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Safety invariants. *)

TypeOK ==
    /\ phase \in {"selected", "ready", "done"}
    /\ gateComputed \in BOOLEAN
    /\ gateOpen \in BOOLEAN
    /\ condemned \in BOOLEAN
    /\ physicallyDeleted \in BOOLEAN
    /\ removalErased \in BOOLEAN
    /\ lifecycleBlockedBySuppression \in BOOLEAN
    /\ emptyEqualityObserved \in BOOLEAN
    /\ physicallyDeleted => condemned
    /\ gateComputed <=> phase # "selected"

PhysicalDeleteOnlyWhenGateOpen ==
    (condemned \/ physicallyDeleted) => PhysicalGateOpen

ProvedRemovalEraseIsNotPhysicalSuppression ==
    ~lifecycleBlockedBySuppression

-----------------------------------------------------------------------------
(* Negated reachability witnesses: a named violation is the evidence. *)

WITNESS_HEALTHY_PHYSICAL_DELETE ==
    ~(gateComputed /\ condemned /\ physicallyDeleted)

WITNESS_SUPPRESSED_REMOVAL_ERASE ==
    ~(gateComputed /\ ~gateOpen /\ RemovalEraseProved /\ removalErased)

WITNESS_EMPTY_UNIVERSE_SUPPRESSED ==
    ~(gateComputed
      /\ CatalogUniverse = {}
      /\ FrontierProven = {}
      /\ emptyEqualityObserved
      /\ ~gateOpen
      /\ ~condemned
      /\ ~physicallyDeleted)

=============================================================================
