------------------------ MODULE CaGcResurrectReuploadOrphan ------------------------
(*****************************************************************************)
(* Focused TLA+ reproduction of RESURRECT-REUPLOAD-ORPHAN (found 2026-07-07  *)
(* in the ca-soak S30 churn scenario via system.content_addressed_log).      *)
(*                                                                          *)
(* WHY THE CANONICAL MODEL MISSED IT: CaIncarnationCore's GRetire condemns   *)
(* by (hash, CURRENT token) — guard `~E e \in retired: e.h=h /\ e.t=tokOf[h]`*)
(* — so after a resurrect changes the token it re-condemns the new one, and  *)
(* NoLeakForever holds. The SHIPPED C++ closeBlob (CasBlobInDegree.cpp        *)
(* ~L225-251) keys the "already retired?" decision on the HASH only: if a     *)
(* prior retired entry exists for the hash it SETTLES that (stale-token)      *)
(* entry and NEVER reaches the fresh-condemn path for the current token; and  *)
(* the fold only visits blobs TOUCHED this window. So the model already       *)
(* encoded the CORRECT algorithm; the code diverged from its own proof.       *)
(*                                                                          *)
(* This model makes the retire faithful to the SHIPPED code and reproduces   *)
(* the leak. FixReCondemnCurrentToken toggles the two behaviors:             *)
(*   - FALSE (shipped bug): retire keyed on hash; settle the stale entry;     *)
(*       the fold, being touch-gated, never re-condemns the replaced token.   *)
(*       => NoLeakForever VIOLATED (the re-uploaded incarnation orphans).     *)
(*   - TRUE  (fix): when the fold settles a prior entry whose token differs   *)
(*       from the current object (a resurrect replaced it) with in-degree 0,  *)
(*       re-condemn the CURRENT token in the SAME (touched) fold.             *)
(*       => NoLeakForever HOLDS.                                             *)
(*                                                                          *)
(* PROPERTY: NoLeakForever == a present, ever-edged, unreferenced incarnation *)
(* is eventually deleted or referenced again — under weak fairness of GC.     *)
(*****************************************************************************)
EXTENDS Integers

CONSTANTS
    MaxTok,                      \* incarnation tokens 1..MaxTok (bound the state space)
    MaxEdge,                     \* max concurrent owner edges on the hash
    FixReCondemnCurrentToken     \* TRUE = fix; FALSE = shipped bug

VARIABLES
    objTok,        \* 0 = the pool has no object at the hash key; else the present incarnation token
    indeg,         \* owner-edge count on the hash (in-degree); reference iff > 0
    everEdged,     \* the hash was referenced at least once (only ever-edged blobs are GC candidates)
    retTok,        \* retired-pipeline entry's token; 0 = no entry for this hash
    retPending,    \* the retired entry has graduated to delete_pending (next round deletes it)
    touched,       \* the hash had owner-edge activity since the last fold (models closeBlob cur_touched)
    nextTok,       \* next fresh incarnation token to allocate
    writerDone     \* the writer has stopped (no more uploads/edges) — lets the leak stutter

vars == << objTok, indeg, everEdged, retTok, retPending, touched, nextTok, writerDone >>

Toks == 0..MaxTok

Init ==
    /\ objTok = 0
    /\ indeg = 0
    /\ everEdged = FALSE
    /\ retTok = 0
    /\ retPending = FALSE
    /\ touched = FALSE
    /\ nextTok = 1
    /\ writerDone = FALSE

------------------------------------------------------------------------------
(* WRITER actions.                                                           *)

\* Fresh upload into an EMPTY key (fresh pool or after a delete).
Upload ==
    /\ ~writerDone
    /\ objTok = 0
    /\ nextTok <= MaxTok
    /\ objTok' = nextTok
    /\ nextTok' = nextTok + 1
    /\ touched' = TRUE
    /\ UNCHANGED << indeg, everEdged, retTok, retPending, writerDone >>

\* Add an owner edge (a part/ref that references the present object).
AddEdge ==
    /\ ~writerDone
    /\ objTok /= 0
    /\ indeg < MaxEdge
    /\ indeg' = indeg + 1
    /\ everEdged' = TRUE
    /\ touched' = TRUE
    /\ UNCHANGED << objTok, retTok, retPending, nextTok, writerDone >>

\* Drop an owner edge (a table/part dropped).
RemoveEdge ==
    /\ ~writerDone
    /\ indeg > 0
    /\ indeg' = indeg - 1
    /\ touched' = TRUE
    /\ UNCHANGED << objTok, everEdged, retTok, retPending, nextTok, writerDone >>

\* RESURRECT: a build dedup-hits the SAME content whose present incarnation is CONDEMNED (its token is
\* in the retire pipeline). Per INV-1 it must NOT revive the condemned bytes; it re-uploads a FRESH
\* incarnation (new token) at the same key and references it. This is the token replacement A -> B.
Resurrect ==
    /\ ~writerDone
    /\ objTok /= 0
    /\ retTok = objTok           \* the present incarnation is the one in the retire pipeline (condemned)
    /\ indeg < MaxEdge           \* the resurrecting build adds one owner edge
    /\ nextTok <= MaxTok
    /\ objTok' = nextTok         \* new incarnation B replaces A at the key
    /\ nextTok' = nextTok + 1
    /\ indeg' = indeg + 1        \* the resurrecting build references B
    /\ everEdged' = TRUE
    /\ touched' = TRUE
    /\ UNCHANGED << retTok, retPending, writerDone >>

WriterDone ==
    /\ ~writerDone
    /\ writerDone' = TRUE
    /\ UNCHANGED << objTok, indeg, everEdged, retTok, retPending, touched, nextTok >>

------------------------------------------------------------------------------
(* GC actions. The fold (GcFold) only VISITS a hash that was touched this     *)
(* window (models closeBlob cur_touched); it consumes the touch.              *)

\* A GC fold round. The retired list is walked EVERY round (settling an existing entry is NOT
\* touch-gated — closeBlob's settleRetiredBelow processes retired hashes with no current edges too).
\* Only FRESH condemn (discovering a NEW zero-in-degree blob) is touch-gated: the fold only visits a
\* blob it has edge deltas for this window (closeBlob cur_touched). A round is enabled whenever it has
\* work: an existing retired entry to settle, or a touched blob to (maybe) fresh-condemn.
GcFold ==
    /\ retTok /= 0 \/ touched
    /\ touched' = FALSE
    /\ IF retTok /= 0
       THEN \* SETTLE the existing retired entry (unconditional every round)
            IF indeg > 0
            THEN /\ retTok' = 0 /\ retPending' = FALSE           \* SPARE: recovery wins (in-degree > 0)
            ELSE IF FixReCondemnCurrentToken /\ retTok /= objTok /\ objTok /= 0
                 THEN /\ retTok' = objTok /\ retPending' = FALSE  \* FIX: object REPLACED -> re-condemn current token
                 ELSE /\ retTok' = retTok /\ retPending' = TRUE   \* SHIPPED: graduate the STALE token; never re-observe current
       ELSE \* retTok = 0: FRESH-CONDEMN a newly discovered zero-in-degree blob (TOUCH-GATED)
            IF touched /\ indeg = 0 /\ objTok /= 0 /\ everEdged
            THEN /\ retTok' = objTok /\ retPending' = FALSE
            ELSE /\ retTok' = 0 /\ retPending' = retPending      \* untouched or still referenced: nothing to condemn
    /\ UNCHANGED << objTok, indeg, everEdged, nextTok, writerDone >>

\* Exact-token delete (R3). Deletes only if the present object still bears the retired token.
GcDelete ==
    /\ retTok /= 0
    /\ retPending
    /\ IF objTok = retTok
       THEN objTok' = 0           \* exact-token match -> physically delete
       ELSE objTok' = objTok      \* REPLACED (object is a newer token) -> skip the delete
    /\ retTok' = 0                 \* the pipeline entry is consumed either way
    /\ retPending' = FALSE
    /\ UNCHANGED << indeg, everEdged, touched, nextTok, writerDone >>

------------------------------------------------------------------------------
Next ==
    \/ Upload \/ AddEdge \/ RemoveEdge \/ Resurrect \/ WriterDone
    \/ GcFold \/ GcDelete

Spec == Init /\ [][Next]_vars

\* Weak fairness on GC so a reachable delete/condemn is not starved; the bug is a state the fold
\* CANNOT re-visit (touched=FALSE, no pipeline entry), so fairness cannot rescue it.
FairSpec == Spec /\ WF_vars(GcFold) /\ WF_vars(GcDelete)

------------------------------------------------------------------------------
TypeOK ==
    /\ objTok \in Toks
    /\ indeg \in 0..MaxEdge
    /\ everEdged \in BOOLEAN
    /\ retTok \in Toks
    /\ retPending \in BOOLEAN
    /\ touched \in BOOLEAN
    /\ nextTok \in 1..(MaxTok + 1)
    /\ writerDone \in BOOLEAN

\* Safety companion: the pipeline entry, when present, names a real (non-zero) token.
INV_RET_SANE == (retTok /= 0) => (retTok \in 1..MaxTok)

\* THE liveness property: a present, ever-edged, unreferenced incarnation is eventually deleted or
\* referenced again. The orphan (a resurrect-replaced token that GC never re-condemns) violates it.
NoLeakForever ==
    [] ( (objTok /= 0 /\ everEdged /\ indeg = 0) => <>(objTok = 0 \/ indeg > 0) )
=============================================================================
