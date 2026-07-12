-------------------- MODULE CaRefNsCleanupStaleLeaderCore --------------------
(* GC namespace-cleanup stale-leader core — spec 2026-07-11-cas-ref-table-snapshot-log-design.md
   §gc-step-clean-ref-objects (Step 6 straggler safety) and §namespace-birth (the recreation gate).

   WHY A DEDICATED MODULE. Neither existing GC-side model represents the namespace-cleanup ITEM
   lifecycle: CaRefDeltaIntakeCore abstracts logs as opaque durable keys with a fold cursor (its
   `Cleanup` action is covered-LOG cleanup, not the physical `@cas@` prefix pass), and
   CaRefWriterCleanupCore is the WRITER's ownership lifecycle and explicitly excludes recreation
   ("no recreation in this small model"). The stale-leader bug lives exactly in the removal ->
   Completed -> `_cleanup` marker -> writer recreation -> straggler physical-delete interleaving, so a
   faithful gate needs its own state.

   MODELED INTERLEAVING (spec §Step 6). A GC leader A wins a round with a Pending namespace-cleanup
   item and STALLS after its round CAS while still owing its physical-delete pass (a VM pause — the
   P3.1 fence-out class). Meanwhile a successor B wins a later round, observes the namespace physically
   empty, promotes the item to Completed, and publishes the `_cleanup` marker (durableRound advances).
   The writer, gated ONLY by observing that marker (spec §Namespace Birth: "observing an empty physical
   prefix is not sufficient"), recreates the namespace with a SUCCESSOR-epoch manifest and a verbatim
   file at the SAME key. Then A resumes its Pending pass over a FRESH live LIST.

   THE RULE UNDER TEST (Step 6 straggler safety). The Pending pass must delete nothing once a successor
   could have recreated the namespace: it re-reads gc/state (aborts unless the ROUND still matches —
   never the lease seq), aborts on the `_cleanup` marker's presence (the exact recreation precondition,
   load-bearing), and epoch-filters manifest deletes to the removed incarnation's epoch. SabotageNo
   StragglerGuard drops all three — the pre-fix behavior — and the fresh LIST + exact-token delete then
   reclaims the recreated manifest and verbatim file (verbatim files carry no epoch at all). *)
EXTENDS Integers

CONSTANTS
    MaxRound,
    SabotageNoStragglerGuard   \* drop the round/marker/epoch guards: a live LIST + exact-token delete

ASSUME MaxRound \in Nat /\ MaxRound >= 1

VARIABLES
    markerPublished,            \* the `_cleanup` marker is durable (a successor Completed the item)
    recreatedManifest,          \* a successor-epoch recreated manifest is present (epoch > removed)
    recreatedFile,              \* a recreated verbatim file is present at a fixed key (no epoch)
    durableRound,               \* the durable gc/state round (advances when a successor commits)
    staleRound,                 \* the round leader A's Pending pass belongs to (captured at its CAS)
    passDone,                   \* leader A's stale Pending pass has executed (once)
    deletedRecreatedManifest,   \* ghost: the stale pass deleted recreated manifest data
    deletedRecreatedFile        \* ghost: the stale pass deleted recreated verbatim-file data

vars == << markerPublished, recreatedManifest, recreatedFile, durableRound, staleRound, passDone,
           deletedRecreatedManifest, deletedRecreatedFile >>

Init ==
    /\ markerPublished = FALSE
    /\ recreatedManifest = FALSE
    /\ recreatedFile = FALSE
    /\ durableRound = 1
    /\ staleRound = 1            \* A captured its Pending item at round 1
    /\ passDone = FALSE
    /\ deletedRecreatedManifest = FALSE
    /\ deletedRecreatedFile = FALSE

(* A successor round Completes the item: it publishes the `_cleanup` marker and advances the durable
   round (every commit strictly increments it). Fires while the item is still merely Pending. *)
SuccessorCompletes ==
    /\ ~markerPublished
    /\ durableRound < MaxRound
    /\ markerPublished' = TRUE
    /\ durableRound' = durableRound + 1
    /\ UNCHANGED << recreatedManifest, recreatedFile, staleRound, passDone,
                    deletedRecreatedManifest, deletedRecreatedFile >>

(* The writer recreates the namespace, gated ONLY on observing the `_cleanup` marker (spec §Namespace
   Birth). Recreation writes a successor-epoch manifest and re-uploads a verbatim file at the same key. *)
WriterRecreatesManifest ==
    /\ markerPublished
    /\ ~recreatedManifest
    /\ recreatedManifest' = TRUE
    /\ UNCHANGED << markerPublished, recreatedFile, durableRound, staleRound, passDone,
                    deletedRecreatedManifest, deletedRecreatedFile >>

WriterRecreatesFile ==
    /\ markerPublished
    /\ ~recreatedFile
    /\ recreatedFile' = TRUE
    /\ UNCHANGED << markerPublished, recreatedManifest, durableRound, staleRound, passDone,
                    deletedRecreatedManifest, deletedRecreatedFile >>

(* Leader A resumes its stale Pending pass (once). SabotageNoStragglerGuard => a live LIST + exact-token
   delete reclaims whatever is present (recreated data included). Guarded => abort unless the round is
   unchanged AND the marker is absent; and even then the epoch filter spares greater-epoch manifests
   (and a verbatim file can only exist after the marker, which the guard already caught). *)
StaleLeaderPass ==
    /\ ~passDone
    /\ passDone' = TRUE
    /\ IF SabotageNoStragglerGuard
       THEN /\ deletedRecreatedManifest' = (deletedRecreatedManifest \/ recreatedManifest)
            /\ deletedRecreatedFile' = (deletedRecreatedFile \/ recreatedFile)
       ELSE IF (durableRound # staleRound) \/ markerPublished
            THEN UNCHANGED << deletedRecreatedManifest, deletedRecreatedFile >>   \* abort: delete nothing
            ELSE /\ deletedRecreatedManifest' = deletedRecreatedManifest   \* epoch filter spares recreated
                 /\ deletedRecreatedFile' = (deletedRecreatedFile \/ recreatedFile)
    /\ UNCHANGED << markerPublished, recreatedManifest, recreatedFile, durableRound, staleRound >>

NoOp == UNCHANGED vars

Next ==
    \/ SuccessorCompletes
    \/ WriterRecreatesManifest \/ WriterRecreatesFile
    \/ StaleLeaderPass
    \/ NoOp

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
(* ---- invariants ---- *)

TypeOK ==
    /\ markerPublished \in BOOLEAN
    /\ recreatedManifest \in BOOLEAN
    /\ recreatedFile \in BOOLEAN
    /\ durableRound \in 1..MaxRound
    /\ staleRound \in 1..MaxRound
    /\ passDone \in BOOLEAN
    /\ deletedRecreatedManifest \in BOOLEAN
    /\ deletedRecreatedFile \in BOOLEAN

(* (SAFETY, spec §Step 6: "Completion means no worker or retry can issue another delete for that
   removal.") The stale Pending pass never deletes recreated data — neither a successor-epoch manifest
   (epoch filter) nor a verbatim file (marker HEAD). SabotageNoStragglerGuard reaches the recreated
   data through a live LIST and violates this. *)
NoRecreatedDataDeleted == ~deletedRecreatedManifest /\ ~deletedRecreatedFile

=============================================================================
