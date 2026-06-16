----------------------- MODULE CaResurrectLiveness -----------------------
(***************************************************************************)
(* TLA+ LIVENESS model of the CA resurrect convergence (B167).             *)
(*                                                                         *)
(* Spec: docs/superpowers/specs/2026-06-16-ca-resurrect-reupload-design.md *)
(*                                                                         *)
(* A blob was referenced -> dropped -> CONDEMNED by GC (zero in-degree). A *)
(* build dedup-hits the same content and must re-reference (resurrect) it, *)
(* then PUBLISH its part. Two resurrect modes:                             *)
(*                                                                         *)
(*  - RECREATABLE (the B167 fix): the build re-uploads a FRESH incarnation *)
(*    from its OWN bytes and references it in ONE step. A fresh incarnation *)
(*    is invisible to GC's manifest-fold until it is referenced, so GC     *)
(*    cannot condemn/delete it in the upload->publish span. Always makes    *)
(*    progress.                                                            *)
(*                                                                         *)
(*  - BODYLESS (today's publish-gate path): the build has no bytes, so it  *)
(*    must re-derive them by GET-ing the EXISTING condemned object         *)
(*    (HEAD -> GET -> rewrite). GC's exact-token delete can land in the    *)
(*    HEAD->GET window -> ABORTED -> the merge re-creates the blob (which   *)
(*    GC re-condemns) and re-observes -> GC deletes again -> ... An         *)
(*    adversarial GC that deletes in EVERY window starves the build.       *)
(*                                                                         *)
(* PROPERTY checked: Liveness == <>published  (the build eventually         *)
(* publishes), under weak fairness of the build's own actions.             *)
(*   - Recreatable = TRUE  : HOLDS (the fix converges).                    *)
(*   - Recreatable = FALSE : VIOLATED (the B167 livelock; a fair behaviour *)
(*     never publishes because GcDelete preempts BodylessComplete forever).*)
(*                                                                         *)
(* Why the existing safety model (CaIncarnationCore) did NOT catch B167:   *)
(* it proves SAFETY (which holds — no data loss) and models the            *)
(* body-in-hand writer; it does not model a BODYLESS gate that gives up,   *)
(* nor check LIVENESS. This model adds exactly that dimension.             *)
(***************************************************************************)
EXTENDS Integers

CONSTANT Recreatable    \* TRUE = B167 fix (re-upload fresh); FALSE = today's bodyless gate

VARIABLES
    present,          \* is an incarnation of the (everEdged) blob present in the pool
    condemned,        \* is that present incarnation condemned by GC (exact-token)
    observedPresent,  \* bodyless build observed the incarnation present (the HEAD step)
    published         \* terminal success: the build referenced a live incarnation + published

vars == << present, condemned, observedPresent, published >>

Init ==
    /\ present = TRUE          \* the condemned-but-present incarnation a dedup hit lands on
    /\ condemned = TRUE
    /\ observedPresent = FALSE
    /\ published = FALSE

----------------------------------------------------------------------------
\* THE FIX. One atomic step: re-upload a FRESH incarnation from the build's own bytes and reference
\* it. A fresh incarnation is not in any manifest fold until referenced, so GC cannot touch it in the
\* span -> the build always makes progress, regardless of what GC does to the old condemned one.
BuildRecreatable ==
    /\ Recreatable
    /\ ~published
    /\ published' = TRUE
    /\ UNCHANGED << present, condemned, observedPresent >>

\* TODAY (bodyless gate): observe the existing condemned incarnation (HEAD).
BodylessObserve ==
    /\ ~Recreatable
    /\ ~published
    /\ present
    /\ condemned
    /\ ~observedPresent
    /\ observedPresent' = TRUE
    /\ UNCHANGED << present, condemned, published >>

\* Bodyless complete: the GET found the incarnation still present -> rewrite fresh + reference +
\* publish. ENABLED ONLY while the observed incarnation is still present; GcDelete disables it.
BodylessComplete ==
    /\ ~Recreatable
    /\ ~published
    /\ observedPresent
    /\ present
    /\ published' = TRUE
    /\ UNCHANGED << present, condemned, observedPresent >>

\* The merge retry re-creates the blob after a vanish (putBlob has a body), and GC re-condemns the
\* unreferenced re-creation before the gate revalidates -> back to the racy condemned-present state.
ReCreate ==
    /\ ~Recreatable
    /\ ~published
    /\ ~present
    /\ present' = TRUE
    /\ condemned' = TRUE
    /\ observedPresent' = FALSE
    /\ UNCHANGED published

\* The adversary: GC's exact-token delete of the condemned incarnation. It can land in the bodyless
\* HEAD->GET window (after Observe, before Complete), starving the build. No fairness on GC.
GcDelete ==
    /\ present
    /\ condemned
    /\ present' = FALSE
    /\ observedPresent' = FALSE   \* the observed incarnation is gone -> a stale observation
    /\ UNCHANGED << condemned, published >>

Next ==
    \/ BuildRecreatable
    \/ BodylessObserve
    \/ BodylessComplete
    \/ ReCreate
    \/ GcDelete

\* Weak fairness on the BUILD's own actions only (GC is the adversary, unconstrained). The build keeps
\* trying; the question is whether it can be starved.
Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(BuildRecreatable)
    /\ WF_vars(BodylessObserve)
    /\ WF_vars(BodylessComplete)
    /\ WF_vars(ReCreate)

----------------------------------------------------------------------------
\* The build eventually publishes. HOLDS for the fix (Recreatable=TRUE), VIOLATED for the bodyless
\* gate (Recreatable=FALSE) — GcDelete preempts BodylessComplete in every window, forever.
Liveness == <>published

TypeOK ==
    /\ present \in BOOLEAN
    /\ condemned \in BOOLEAN
    /\ observedPresent \in BOOLEAN
    /\ published \in BOOLEAN
=============================================================================
