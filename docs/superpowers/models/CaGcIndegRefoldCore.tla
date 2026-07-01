-------------------------- MODULE CaGcIndegRefoldCore --------------------------
(*****************************************************************************)
(* Focused TLA+ recheck (Phase 2) for the PROVEN CA GC bug H1b:             *)
(* blob in-degree undercount via re-fold of a fence-window removal.         *)
(*                                                                          *)
(* This is a NEW, MINIMAL model. It deliberately does NOT recompute         *)
(* in-degree from a folded edge SET (the way the big proven spec            *)
(* CaGcRootLocalPartManifestCore.tla does). The whole point of the bug is   *)
(* that the C++ accumulates in-degree as a NON-idempotent INTEGER delta     *)
(* stream (prior_generation_count + Sum of +/-1 journal deltas), guarded    *)
(* fail-closed at merged < 0 (CasBlobInDegree.cpp:161-165). Set-difference  *)
(* recompute is idempotent -> re-fold is a no-op -> underflow is            *)
(* unreachable in the big model. Integer delta accumulation is NOT          *)
(* idempotent -> re-folding an already-absorbed removal drives indeg to -1. *)
(*                                                                          *)
(* GC round order modelled: fold -> retire -> fence -> recheck -> trim.     *)
(* The bug lives in the completion-seal cursor handoff (CasGc.cpp:963):     *)
(* the seal persists the PRE-window fold-time cursor, not the cursor        *)
(* advanced past the events recheck just folded. The NEXT round's fold      *)
(* reconstructs its parent cursor from that seal (readSealedCursors,        *)
(* CasGc.cpp:211) and RE-FOLDS the window removal.                          *)
(*****************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Blobs,                              \* set of blob ids (use 1 blob: {b1})
    MaxLog,                             \* max journal events on the shard
    MaxRound,                           \* max GC rounds
    SabotageCompletionCursorAtFold      \* TRUE = C++ bug; FALSE = fix

(* Journal event: an owner-edge activation (+1) or removal (-1) for a blob. *)
Delta == {1, -1}
Events == [blob : Blobs, delta : Delta]

VARIABLES
    journal,          \* Seq of Events: the single shard's append-only owner-edge journal
    indeg,            \* [Blobs -> Int]: in-degree accumulated as an INTEGER delta stream
    foldCursor,       \* Int: journal length sealed by the current round's fold
    fenceVersion,     \* Int: fence position for the current round
    persistedCursor,  \* Int: cursor written into the completion seal at round end
    parentCursor,     \* Int: cursor the CURRENT round's fold reconstructs from the prior seal
    round,            \* Int: current GC round number
    phase,            \* round phase: "idle" -> "folded" -> "fenced" -> "rechecked" -> "sealed"
    folded            \* [Blobs -> BOOLEAN]: whether an activation for the blob was folded (armed for a drop)

vars == << journal, indeg, foldCursor, fenceVersion, persistedCursor,
           parentCursor, round, phase, folded >>

-----------------------------------------------------------------------------
(* Sum of deltas for a given blob over journal positions in (lo, hi].       *)
RECURSIVE SumDeltas(_, _, _, _)
SumDeltas(b, seq, lo, hi) ==
    IF hi <= lo THEN 0
    ELSE (IF seq[hi].blob = b THEN seq[hi].delta ELSE 0)
         + SumDeltas(b, seq, lo, hi - 1)

-----------------------------------------------------------------------------
Init ==
    /\ journal = << >>
    /\ indeg = [b \in Blobs |-> 0]
    /\ foldCursor = 0
    /\ fenceVersion = 0
    /\ persistedCursor = 0
    /\ parentCursor = 0
    /\ round = 0
    /\ phase = "idle"
    /\ folded = [b \in Blobs |-> FALSE]

-----------------------------------------------------------------------------
(* WRITER: append an owner-edge event to the journal at any time. This is   *)
(* the concurrent producer. A DROP that lands in the fence window is just   *)
(* an AppendRemoval that happens after fold has sealed its cursor but at or *)
(* below the fence position.                                                *)
AppendActivation(b) ==
    /\ Len(journal) < MaxLog
    /\ journal' = Append(journal, [blob |-> b, delta |-> 1])
    /\ UNCHANGED << indeg, foldCursor, fenceVersion, persistedCursor,
                    parentCursor, round, phase, folded >>

AppendRemoval(b) ==
    /\ Len(journal) < MaxLog
    (* only remove an edge that plausibly exists: some prior +1 outstanding *)
    /\ SumDeltas(b, journal, 0, Len(journal)) > 0
    /\ journal' = Append(journal, [blob |-> b, delta |-> -1])
    /\ UNCHANGED << indeg, foldCursor, fenceVersion, persistedCursor,
                    parentCursor, round, phase, folded >>

-----------------------------------------------------------------------------
(* FOLD: start a GC round. Reconstruct the parent cursor from the prior     *)
(* completion seal (this is the readSealedCursors handoff), then fold the   *)
(* journal window (parentCursor, Len(journal)] into indeg and seal          *)
(* foldCursor at the fold-time journal length.                              *)
Fold ==
    /\ phase = "idle"
    /\ round < MaxRound
    /\ LET pc == persistedCursor            \* readSealedCursors: parent = prior seal
           newIndeg == [b \in Blobs |->
                          indeg[b] + SumDeltas(b, journal, pc, Len(journal))]
       IN /\ parentCursor' = pc
          /\ indeg' = newIndeg
          /\ foldCursor' = Len(journal)      \* seal cursor at fold-time length
          /\ folded' = [b \in Blobs |->
                          folded[b] \/ SumDeltas(b, journal, pc, Len(journal)) > 0]
    /\ round' = round + 1
    /\ phase' = "folded"
    /\ UNCHANGED << journal, fenceVersion, persistedCursor >>

-----------------------------------------------------------------------------
(* FENCE: choose the fence position for this round. The fence may extend    *)
(* PAST foldCursor to cover events that landed during/after fold -- this is *)
(* the fence window (foldCursor, fenceVersion].                             *)
Fence ==
    /\ phase = "folded"
    /\ \E fv \in (foldCursor .. Len(journal)) :
          fenceVersion' = fv
    /\ phase' = "fenced"
    /\ UNCHANGED << journal, indeg, foldCursor, persistedCursor,
                    parentCursor, round, folded >>

-----------------------------------------------------------------------------
(* RECHECK: re-stream the fence window (foldCursor, fenceVersion] and fold  *)
(* those deltas into the COMPLETION generation. This correctly drives the   *)
(* blob to its true in-degree for the sealed round.                         *)
Recheck ==
    /\ phase = "fenced"
    /\ indeg' = [b \in Blobs |->
                   indeg[b] + SumDeltas(b, journal, foldCursor, fenceVersion)]
    /\ phase' = "rechecked"
    /\ UNCHANGED << journal, foldCursor, fenceVersion, persistedCursor,
                    parentCursor, round, folded >>

-----------------------------------------------------------------------------
(* SEAL / TRIM: write the completion seal cursor and end the round.         *)
(*   BUG  (SabotageCompletionCursorAtFold = TRUE):                          *)
(*        persistedCursor = foldCursor  -- the pre-window fold-time cursor.  *)
(*        trim only drops events <= foldCursor, so the recheck-folded       *)
(*        window event SURVIVES and the NEXT fold re-folds it.              *)
(*   FIX  (SabotageCompletionCursorAtFold = FALSE):                         *)
(*        persistedCursor = max(foldCursor, fenceVersion) -- advanced past   *)
(*        what recheck already folded, so no re-fold next round.            *)
Seal ==
    /\ phase = "rechecked"
    /\ persistedCursor' =
         IF SabotageCompletionCursorAtFold
         THEN foldCursor
         ELSE IF foldCursor > fenceVersion THEN foldCursor ELSE fenceVersion
    /\ phase' = "idle"
    /\ UNCHANGED << journal, indeg, foldCursor, fenceVersion,
                    parentCursor, round, folded >>

-----------------------------------------------------------------------------
Next ==
    \/ \E b \in Blobs : AppendActivation(b)
    \/ \E b \in Blobs : AppendRemoval(b)
    \/ Fold
    \/ Fence
    \/ Recheck
    \/ Seal

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
StateConstraint ==
    /\ Len(journal) <= MaxLog
    /\ round <= MaxRound

TypeOK ==
    /\ journal \in Seq(Events)
    /\ indeg \in [Blobs -> Int]
    /\ foldCursor \in Int
    /\ fenceVersion \in Int
    /\ persistedCursor \in Int
    /\ parentCursor \in Int
    /\ round \in 0..MaxRound
    /\ phase \in {"idle", "folded", "fenced", "rechecked", "sealed"}
    /\ folded \in [Blobs -> BOOLEAN]

(* THE bug: fail-closed underflow in the C++ (merged < 0 -> CORRUPTED_DATA) *)
INV_INDEG_NONNEGATIVE == \A b \in Blobs : indeg[b] >= 0

=============================================================================
