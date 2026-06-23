----------------------- MODULE CaResurrectLiveness -----------------------
\* =====================================================================================
\* STALE vs SHIPPED CODE (2026-06-23 currency review). Models an abstract condemn-time
\* HeartbeatGuard that was NEVER implemented (heartbeat-gated condemn belongs to the deferred
\* M-F Full GC). Shipped in-flight protection is precommit reachability -> CaBuildRootPrecommit.tla.
\* Superseded as a model of current behavior; kept as historical record.
\* Details: MODEL_CURRENCY_REVIEW_2026-06-22.md.
\* =====================================================================================
(***************************************************************************)
(* TLA+ LIVENESS model of the CA resurrect convergence (B167).             *)
(*                                                                         *)
(* Spec: docs/superpowers/specs/2026-06-16-ca-resurrect-reupload-design.md *)
(*                                                                         *)
(* A blob was referenced -> dropped -> CONDEMNED by GC. A build dedup-hits  *)
(* the SAME content and must re-reference it, then PUBLISH its part. The    *)
(* build HAS the body in hand (its BlobSource is re-invokable), so it never *)
(* GETs the existing object — it re-streams a FRESH incarnation stamped     *)
(* with its OWN build_id. The open question is purely about the WINDOW      *)
(* between that fresh upload and the publish that references it.            *)
(*                                                                         *)
(* THE TRAP THIS MODEL EXISTS TO EXPOSE: a fresh incarnation is NOT GC-     *)
(* invisible. The hash is `everEdged` (it WAS published once -> that is why *)
(* it became condemned) and stays InDeg=0 until this build publishes. So    *)
(* GC's condemn guard  present /\ everEdged /\ InDeg=0  IS satisfied for     *)
(* the build's own fresh incarnation: GC can re-condemn and exact-token     *)
(* delete it in the upload->publish span. An earlier version of this model  *)
(* collapsed upload+publish into ONE atomic step and thereby "proved" the   *)
(* writer-side re-upload converges — false confidence. It does NOT, alone.  *)
(*                                                                         *)
(* THE FIX UNDER TEST (HeartbeatGuard): incremental GC honors the BUILD     *)
(* heartbeat — it refuses to CONDEMN a blob whose envelope build_id has a   *)
(* live heartbeat. Condemn guard becomes                                   *)
(*     present /\ everEdged /\ InDeg=0 /\ ~liveBuild(build_id).             *)
(* The build's fresh incarnation carries its OWN (live) build_id, so GC     *)
(* cannot condemn it -> it survives to the publish that references it.      *)
(*                                                                         *)
(* PROPERTY checked: Liveness == <>published, under weak fairness of the    *)
(* build's own actions (GC is the unconstrained adversary).                *)
(*   - HeartbeatGuard = TRUE  : HOLDS  (the fix converges).                 *)
(*   - HeartbeatGuard = FALSE : VIOLATED — and crucially this is the        *)
(*     body-in-hand writer, NOT the old bodyless GET path. It proves the    *)
(*     writer-side re-stream ALONE is starvable and the heartbeat guard is  *)
(*     load-bearing: upload(fresh) -> GC re-condemns -> GC deletes ->        *)
(*     upload(fresh) -> ...  publish never fires.                          *)
(***************************************************************************)
EXTENDS Integers

CONSTANT HeartbeatGuard   \* TRUE = incremental GC honors the build heartbeat (the fix); FALSE = today

VARIABLES
    present,      \* an incarnation of the (everEdged) blob is present in the pool
    condemned,    \* GC has condemned the present incarnation (exact-token, in the retire pipeline)
    freshOwned,   \* the present incarnation is THIS build's fresh re-stamp (carries its LIVE build_id)
    published     \* terminal success: the build referenced its live incarnation and published

vars == << present, condemned, freshOwned, published >>

\* A dedup hit lands on a STALE condemned incarnation left by a PAST build (dead heartbeat): present,
\* condemned, not owned by us.
Init ==
    /\ present = TRUE
    /\ condemned = TRUE
    /\ freshOwned = FALSE
    /\ published = FALSE

----------------------------------------------------------------------------
\* W-rule (writer-side): re-stream the body into a FRESH incarnation stamped with this build's own
\* build_id. The fresh tag clears the old exact-token condemnation (GC's pending delete can no longer
\* match), and the incarnation now carries a LIVE build_id. Enabled whenever the build does not already
\* hold its own present fresh incarnation — i.e. on the initial stale hit AND after any GcDelete.
BuildUpload ==
    /\ ~published
    /\ ~(present /\ freshOwned)
    /\ present' = TRUE
    /\ freshOwned' = TRUE
    /\ condemned' = FALSE
    /\ UNCHANGED published

\* The publish gate references the build's own present fresh incarnation -> InDeg becomes >=1 -> publish.
\* Enabled while that incarnation is present (condemned-or-not: a present incarnation can still be
\* referenced; the reference re-pins it above any fence).
BuildPublish ==
    /\ ~published
    /\ present
    /\ freshOwned
    /\ published' = TRUE
    /\ UNCHANGED << present, condemned, freshOwned >>

\* GC condemns the present incarnation (present /\ everEdged /\ InDeg=0). everEdged and InDeg=0 hold
\* throughout (the blob was published-then-dropped; this build has not published yet). THE GUARD: with
\* HeartbeatGuard, GC reads the envelope build_id and refuses to condemn a blob owned by a LIVE build
\* (freshOwned). Without the guard, GC condemns regardless of ownership. No fairness on GC.
GcCondemn ==
    /\ present
    /\ ~condemned
    /\ ~published
    /\ (HeartbeatGuard => ~freshOwned)
    /\ condemned' = TRUE
    /\ UNCHANGED << present, freshOwned, published >>

\* GC's exact-token delete of a CONDEMNED incarnation. Removes it (and its ownership). With the guard a
\* freshOwned incarnation never becomes condemned, so it is never deleted.
GcDelete ==
    /\ present
    /\ condemned
    /\ present' = FALSE
    /\ condemned' = FALSE
    /\ freshOwned' = FALSE
    /\ UNCHANGED published

Next ==
    \/ BuildUpload
    \/ BuildPublish
    \/ GcCondemn
    \/ GcDelete

\* Weak fairness on the BUILD's own actions only (the build is alive and keeps trying; GC is the
\* unconstrained adversary). The question is whether GC can starve a live, body-in-hand build.
Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(BuildUpload)
    /\ WF_vars(BuildPublish)

----------------------------------------------------------------------------
\* The build eventually publishes. HOLDS with the heartbeat guard; VIOLATED without it (the sharper
\* B167: even body-in-hand re-upload is starved by an adversarial GC that re-condemns + deletes the
\* fresh incarnation in every upload->publish window).
Liveness == <>published

TypeOK ==
    /\ present \in BOOLEAN
    /\ condemned \in BOOLEAN
    /\ freshOwned \in BOOLEAN
    /\ published \in BOOLEAN
=============================================================================
