---------------------- MODULE CaManifestSweepWindow ----------------------
(* Wedge gate (2026-07-10): a COMMITTED manifest ref is dropped; its removal `-1` is appended to the
   shard journal but NOT yet sealed by the GC fold. A promoted build retired its build_seq, so the
   prefix is watermark-eligible for the orphan-manifest sweep. The sweep must NOT delete the committed
   body in the dropRef->fold-seal window: the removal-fold still needs the body present to emit the
   decrement. SabSweepCommitted drops the pending-committed-removal protection and MUST break the gate:
   the sweep deletes the body, the fold then can never decrement -> INV_FOLD_PROGRESS violated forever. *)
EXTENDS Naturals

CONSTANTS SabSweepCommitted   \* sweep ignores pending-committed-removals (the pre-fix bug)

VARIABLES
  body,            \* BOOLEAN: the committed manifest body object present?
  ownerState,      \* "committed" | "removing" (removal appended, unsealed) | "removed"
  sealedRemoval,   \* BOOLEAN: the -1 removal has been folded/sealed
  swept            \* BOOLEAN: the orphan sweep ran on this eligible prefix

vars == <<body, ownerState, sealedRemoval, swept>>

Init ==
  /\ body = TRUE
  /\ ownerState = "committed"
  /\ sealedRemoval = FALSE
  /\ swept = FALSE

\* Drop the committed ref: append the -1 removal (unsealed). Prefix becomes sweep-eligible.
DropRef ==
  /\ ownerState = "committed"
  /\ ownerState' = "removing"
  /\ UNCHANGED <<body, sealedRemoval, swept>>

\* The removal-fold: requires the body present to emit the decrement, then seals. If the body is gone
\* it CANNOT proceed (the real clamp: "edge-bearing committed body missing at removal-fold").
FoldSealRemoval ==
  /\ ownerState = "removing" /\ ~sealedRemoval /\ body
  /\ sealedRemoval' = TRUE /\ ownerState' = "removed"
  /\ UNCHANGED <<body, swept>>

\* The orphan-manifest sweep on the eligible prefix. CORRECT: skip a body with a pending (unsealed)
\* committed removal. SABOTAGE: delete it anyway.
Sweep ==
  /\ ~swept /\ body
  /\ swept' = TRUE
  /\ IF SabSweepCommitted
     THEN body' = FALSE                                     \* pre-fix: deletes the still-needed body
     ELSE IF ownerState = "removing" /\ ~sealedRemoval
          THEN body' = body                                 \* fix: protect the pending committed removal
          ELSE IF ownerState = "removed"
               THEN body' = FALSE                           \* fully sealed -> orphan, deletable
               ELSE body' = body                            \* committed & live -> owned, skip
  /\ UNCHANGED <<ownerState, sealedRemoval>>

Next == DropRef \/ FoldSealRemoval \/ Sweep

Spec == Init /\ [][Next]_vars /\ WF_vars(FoldSealRemoval)

TypeOK ==
  /\ body \in BOOLEAN /\ sealedRemoval \in BOOLEAN /\ swept \in BOOLEAN
  /\ ownerState \in {"committed","removing","removed"}

(* A removal that has begun must eventually seal — impossible if the body was swept away first. *)
INV_FOLD_PROGRESS == (ownerState = "removing") => (body \/ sealedRemoval)

=========================================================================
