-------------------- MODULE CaRefNsCleanupStaleLeaderCore --------------------
(* Perpetual namespace-janitor stale-leader core — spec
   2026-07-27-cas-ref-chain-complete-cut-design.md §2 INV-3.

   Task 5 has no synchronous namespace-removal cleanup pass. The next GC invocation performs only a
   catalog pre-fold drain; physical `cas/ns/<life_id>/...` residue is later listed by the perpetual
   janitor. This module keeps the stale-actor obligation that survives that simplification:

   1. the catalog-only drain deletes life 1's `Removing` row;
   2. a janitor LIST page captures an exact physical `life_id` from a returned key;
   3. the post-page catalog cut proves that id is no longer named and nominates it;
   4. the janitor stalls, the logical name is reborn with a fresh id, and the new life writes;
   5. the stale janitor resumes its exact physical delete.

   Honest cleanup always targets the captured physical id. It never re-resolves the logical name at
   resume time. `SabotageRederive` models that forbidden re-resolution; `SabotageNoIncarnation`
   independently models same-id rebirth. Either makes the stale delete reach live data. The focused
   cross-object order between the pre-fold catalog CAS and successor-seal adoption is proved by
   `CaRefPreFoldDrainCore`; duplicating it here would create a second protocol owner. *)
EXTENDS Integers

CONSTANTS
    MaxInc,
    SabotageNoIncarnation,
    SabotageRederive

ASSUME MaxInc \in Nat /\ MaxInc >= 2

Incs == 1..MaxInc

VARIABLES
    entry,             \* [state: {"removing","absent","live"}, inc: 0..MaxInc]
    listedInc,         \* physical life id captured from one janitor LIST page
    nominated,         \* a later catalog cut proved `listedInc` foreign
    nextInc,           \* fresh-life allocator; ordering is irrelevant, freshness is not
    objects,           \* physical `cas/ns/<life_id>/...` objects
    passDone,          \* the deposited janitor delete executes at most once
    deletedLiveData    \* sticky ghost: stale cleanup deleted the current live life's bytes

vars == << entry, listedInc, nominated, nextInc, objects, passDone, deletedLiveData >>

Init ==
    /\ entry = [state |-> "removing", inc |-> 1]
    /\ listedInc = 0
    /\ nominated = FALSE
    /\ nextInc = 2
    /\ objects = {1}
    /\ passDone = FALSE
    /\ deletedLiveData = FALSE

(* Abstracts the proved catalog-only pre-fold deletion. Evidence, no-hold and exact-row checks are
   local properties of `CaRefCatalogCore`; A/B helping and successor ordering are
   `CaRefPreFoldDrainCore`'s property. No physical object changes here. *)
PreFoldDelete ==
    /\ entry.state = "removing"
    /\ entry' = [state |-> "absent", inc |-> 0]
    /\ UNCHANGED << listedInc, nominated, nextInc, objects, passDone, deletedLiveData >>

(* One zero-trust LIST page returns a physical key and deposits the id encoded in that key. *)
ListCandidate ==
    /\ listedInc = 0
    /\ \E i \in objects : listedInc' = i
    /\ UNCHANGED << entry, nominated, nextInc, objects, passDone, deletedLiveData >>

(* Required order: LIST page, then a fresh complete catalog cut. Only an id that cut does not name is
   nominated. The cut may be absent or may already name a different reborn life. *)
PostPageCutAndNominate ==
    /\ listedInc # 0
    /\ ~nominated
    /\ (entry.state = "absent" \/ entry.inc # listedInc)
    /\ nominated' = TRUE
    /\ UNCHANGED << entry, listedInc, nextInc, objects, passDone, deletedLiveData >>

NewInc == IF SabotageNoIncarnation THEN listedInc ELSE nextInc

Recreate ==
    /\ entry.state = "absent"
    /\ listedInc # 0
    /\ nextInc <= MaxInc
    /\ entry' = [state |-> "live", inc |-> NewInc]
    /\ nextInc' = nextInc + 1
    /\ UNCHANGED << listedInc, nominated, objects, passDone, deletedLiveData >>

WriteObject ==
    /\ entry.state = "live"
    /\ objects' = objects \cup {entry.inc}
    /\ UNCHANGED << entry, listedInc, nominated, nextInc, passDone, deletedLiveData >>

DeleteTarget == IF SabotageRederive THEN entry.inc ELSE listedInc

(* A stale janitor may resume after arbitrary delay. Exact physical identity, not continued lease
   ownership, makes the old-life target structurally unable to name the fresh life. *)
StaleJanitorDelete ==
    /\ nominated
    /\ ~passDone
    /\ passDone' = TRUE
    /\ deletedLiveData' =
        (deletedLiveData \/
            (entry.state = "live" /\ entry.inc = DeleteTarget /\ DeleteTarget \in objects))
    /\ objects' = objects \ {DeleteTarget}
    /\ UNCHANGED << entry, listedInc, nominated, nextInc >>

NoOp == UNCHANGED vars

Next ==
    \/ PreFoldDelete
    \/ ListCandidate
    \/ PostPageCutAndNominate
    \/ Recreate
    \/ WriteObject
    \/ StaleJanitorDelete
    \/ NoOp

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ entry \in [state : {"removing", "absent", "live"}, inc : 0..MaxInc]
    /\ ((entry.state = "absent") <=> (entry.inc = 0))
    /\ listedInc \in 0..MaxInc
    /\ nominated \in BOOLEAN
    /\ nextInc \in 1..(MaxInc + 1)
    /\ objects \subseteq Incs
    /\ passDone \in BOOLEAN
    /\ deletedLiveData \in BOOLEAN

NoLiveDataDeleted == ~deletedLiveData

NominationCapturedPhysicalId == nominated => listedInc # 0

(* Negated witness: a physical id was captured and nominated before/through same-name rebirth, and
   remains distinct from the live successor id. A violation is the reachability evidence. *)
WITNESS_CAPTURED_BEFORE_REBIRTH ==
    ~(entry.state = "live" /\ nominated /\ listedInc # 0 /\ listedInc # entry.inc)

=============================================================================
