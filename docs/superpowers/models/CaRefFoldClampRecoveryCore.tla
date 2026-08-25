-------------------- MODULE CaRefFoldClampRecoveryCore --------------------
(* GC fold-clamp recoverability core — spec 2026-07-11-cas-ref-table-snapshot-log-design.md
   §gc-step-produce-manifest-edge-delta (transaction atomicity / the fold barrier) and §gc-retire
   (delete-after-sealed-decrements: the post-CAS owner-removed body delete).

   WHY A DEDICATED MODULE. CaRefDeltaIntakeCore abstracts a ref log as an OPAQUE durable key with an
   id; it has no notion of a log carrying several manifest EDGES, of a manifest BODY, of the mid-log
   CLAMP, or of the post-CAS owner-removed body delete (the code's R6). Those are exactly the objects
   the clamp-recoverability bug lives among, so a faithful gate needs its own small state rather than
   a graft that would either be unfaithful or perturb that module's five green configs.

   MODELED SCENARIO (the smallest that reproduces the freeze). One table, one two-edge log L:
     e1 = a `-1` owner-removal naming body A (present at removal-fold: its token is captured for the
          deferred post-CAS delete),
     e2 = a `+1` whose body B is TRANSIENTLY absent (a spurious backend 404), which CLAMPS the log.
   The fold processes edges in order; a clamp leaves the durable cursor BELOW L (the whole log re-folds
   next round). The round then runs the post-CAS owner-removed body delete over the round's captured
   cleanup set (R6, unconditional). B's absence heals once.

   THE RULE UNDER TEST (per-log transaction atomicity). A `-1` body token joins the ROUND cleanup set
   only once the WHOLE log folds; a clamp discards the log's staged tokens. SabotageEdgeGranularity
   drops this: it commits e1's token to the round cleanup set the instant e1 folds (edge granularity),
   so the post-CAS delete reclaims A's body while L is still unfolded behind the clamp -- and every
   later re-fold then finds A missing and clamps forever (a permanent, pool-wide destructive freeze).

   The same round-CAS boundary also owns orphan-manifest nomination. Exact GET/decode produces a
   NEUTRAL candidate: it neither consumes a B2 ordinal nor enters unmatched-remove accounting. The
   candidate becomes durable only in CommitRound's gc/state CAS; exact-token deletion is allowed
   only after that adoption. Keeping nomination in this module is intentional: the fold phase is
   orthogonal, but the candidate -> adopted -> delete ordering is the same commit/delete protocol
   already modeled here, so a second module would duplicate its critical boundary. *)
EXTENDS Integers

CONSTANTS
    SabotageEdgeGranularity,          \* commit a `-1` body token at edge granularity
    SabotageDeleteBeforeAdoption,     \* delete an orphan candidate before the round CAS adopts it
    SabotageNominationContaminates    \* route neutral nomination through ref accounting

VARIABLES
    aPresent,       \* body A present (the `-1` edge's target; if deleted, L can never re-fold)
    bPresent,       \* body B present (the `+1` edge's target; starts absent, heals once)
    bHealed,        \* sticky: B's transient absence has healed (fires at most once)
    logCleanup,     \* this round's per-log staged owner-removed tokens ({} or {"A"})
    roundCleanup,   \* this round's COMMITTED cleanup set (what the post-CAS delete reclaims)
    edgePos,        \* within-round processing position for L: 0 none, 1 e1 folded, 2 e2 folded
    cursorPastL,    \* durable, monotone: the fold cursor has advanced past L (the whole log folded)
    clamped,        \* this round's fold clamped
    phase,          \* "idle" | "fold" | "committed"
    manifestPresent,\* orphan manifest body discovered by exact GET/decode
    nomination,     \* "None" | "Candidate" | "Adopted" in the round's gc/state
    b2OrdinalTouched,       \* ghost: neutral nomination entered B2 ordinal accounting
    unmatchedRemoveTouched  \* ghost: neutral nomination entered unmatched-remove accounting

vars == << aPresent, bPresent, bHealed, logCleanup, roundCleanup, edgePos, cursorPastL, clamped,
           phase, manifestPresent, nomination, b2OrdinalTouched, unmatchedRemoveTouched >>

Init ==
    /\ aPresent = TRUE
    /\ bPresent = FALSE
    /\ bHealed = FALSE
    /\ logCleanup = {}
    /\ roundCleanup = {}
    /\ edgePos = 0
    /\ cursorPastL = FALSE
    /\ clamped = FALSE
    /\ phase = "idle"
    /\ manifestPresent = TRUE
    /\ nomination = "None"
    /\ b2OrdinalTouched = FALSE
    /\ unmatchedRemoveTouched = FALSE

(* Exact GET/decode finds an orphan manifest and nominates it as neutral input to the next round.
   The sabotage deliberately sends that nomination through both ref-accounting paths. *)
DiscoverNeutralNomination ==
    /\ phase = "idle"
    /\ manifestPresent
    /\ nomination = "None"
    /\ nomination' = "Candidate"
    /\ b2OrdinalTouched' = SabotageNominationContaminates
    /\ unmatchedRemoveTouched' = SabotageNominationContaminates
    /\ UNCHANGED << aPresent, bPresent, bHealed, logCleanup, roundCleanup, edgePos,
                    cursorPastL, clamped, phase, manifestPresent >>

(* Start a round: reset this round's staging, edge position and clamp flag. Only interesting while the
   cursor has not already passed L (once past, there is nothing left of L to fold). *)
BeginRound ==
    /\ phase = "idle"
    /\ (~cursorPastL \/ nomination = "Candidate")
    /\ phase' = "fold"
    /\ logCleanup' = {}
    /\ roundCleanup' = {}
    /\ edgePos' = IF cursorPastL THEN 2 ELSE 0
    /\ clamped' = FALSE
    /\ UNCHANGED << aPresent, bPresent, bHealed, cursorPastL, manifestPresent, nomination,
                    b2OrdinalTouched, unmatchedRemoveTouched >>

(* Fold e1, the `-1` on A. Present body => stage A's token (per-log by default; committed to the ROUND
   set immediately under sabotage) and advance to e2. Missing body (A already reclaimed) => the removal
   edge's own body is missing at removal-fold, which CLAMPS -- the permanent clamp under sabotage. *)
FoldEdge1 ==
    /\ phase = "fold"
    /\ ~clamped
    /\ edgePos = 0
    /\ IF aPresent
       THEN /\ logCleanup' = {"A"}
            /\ roundCleanup' = IF SabotageEdgeGranularity THEN (roundCleanup \cup {"A"}) ELSE roundCleanup
            /\ edgePos' = 1
            /\ UNCHANGED clamped
       ELSE /\ clamped' = TRUE
            /\ UNCHANGED << logCleanup, roundCleanup, edgePos >>
    /\ UNCHANGED << aPresent, bPresent, bHealed, cursorPastL, phase, manifestPresent, nomination,
                    b2OrdinalTouched, unmatchedRemoveTouched >>

(* Fold e2, the `+1` on B. Present body => the WHOLE log folds: merge the per-log buffer into the round
   cleanup (fixed path) and advance the cursor past L. Absent body => CLAMP: the cursor stays below L
   and the per-log buffer is discarded (fixed path: roundCleanup unchanged). *)
FoldEdge2 ==
    /\ phase = "fold"
    /\ ~clamped
    /\ edgePos = 1
    /\ IF bPresent
       THEN /\ roundCleanup' = roundCleanup \cup logCleanup
            /\ cursorPastL' = TRUE
            /\ edgePos' = 2
            /\ UNCHANGED clamped
       ELSE /\ clamped' = TRUE
            /\ UNCHANGED << roundCleanup, cursorPastL, edgePos >>
    /\ UNCHANGED << aPresent, bPresent, bHealed, logCleanup, phase, manifestPresent, nomination,
                    b2OrdinalTouched, unmatchedRemoveTouched >>

(* Scan complete -> the round's single CAS commits; the post-CAS delete may now run. *)
CommitRound ==
    /\ phase = "fold"
    /\ (clamped \/ edgePos = 2)
    /\ phase' = "committed"
    /\ nomination' = IF nomination = "Candidate" THEN "Adopted" ELSE nomination
    /\ UNCHANGED << aPresent, bPresent, bHealed, logCleanup, roundCleanup, edgePos, cursorPastL,
                    clamped, manifestPresent, b2OrdinalTouched, unmatchedRemoveTouched >>

(* R6: the post-CAS owner-removed body delete, UNCONDITIONAL over the round cleanup set (the code
   deletes it even when the round clamped). If A is in the round cleanup set, A's body is reclaimed. *)
PostCasDelete ==
    /\ phase = "committed"
    /\ aPresent' = IF "A" \in roundCleanup THEN FALSE ELSE aPresent
    /\ phase' = "idle"
    /\ UNCHANGED << bPresent, bHealed, logCleanup, roundCleanup, edgePos, cursorPastL, clamped,
                    manifestPresent, nomination, b2OrdinalTouched, unmatchedRemoveTouched >>

(* Exact-token orphan deletion is post-adoption. Under sabotage, the candidate may be deleted before
   CommitRound, exposing the ownership gap directly. A death after honest adoption merely leaves a
   rediscoverable leak; safety does not promise that a particular process retries the deletion. *)
DeleteNominatedManifest ==
    /\ manifestPresent
    /\ nomination # "None"
    /\ (SabotageDeleteBeforeAdoption \/ nomination = "Adopted")
    /\ manifestPresent' = FALSE
    /\ UNCHANGED << aPresent, bPresent, bHealed, logCleanup, roundCleanup, edgePos, cursorPastL,
                    clamped, phase, nomination, b2OrdinalTouched, unmatchedRemoveTouched >>

(* The transient backend 404 on B heals (fires once); models "the blip is not permanent". *)
HealB ==
    /\ ~bHealed
    /\ bPresent' = TRUE
    /\ bHealed' = TRUE
    /\ UNCHANGED << aPresent, logCleanup, roundCleanup, edgePos, cursorPastL, clamped, phase,
                    manifestPresent, nomination, b2OrdinalTouched, unmatchedRemoveTouched >>

NoOp == UNCHANGED vars

Next ==
    \/ DiscoverNeutralNomination
    \/ BeginRound \/ FoldEdge1 \/ FoldEdge2 \/ CommitRound \/ PostCasDelete
    \/ DeleteNominatedManifest
    \/ HealB
    \/ NoOp

Spec == Init /\ [][Next]_vars
             /\ WF_vars(BeginRound) /\ WF_vars(FoldEdge1) /\ WF_vars(FoldEdge2)
             /\ WF_vars(CommitRound) /\ WF_vars(PostCasDelete) /\ WF_vars(HealB)

----------------------------------------------------------------------------
(* ---- invariants ---- *)

TypeOK ==
    /\ aPresent \in BOOLEAN
    /\ bPresent \in BOOLEAN
    /\ bHealed \in BOOLEAN
    /\ logCleanup \subseteq {"A"}
    /\ roundCleanup \subseteq {"A"}
    /\ edgePos \in 0..2
    /\ cursorPastL \in BOOLEAN
    /\ clamped \in BOOLEAN
    /\ phase \in {"idle", "fold", "committed"}
    /\ manifestPresent \in BOOLEAN
    /\ nomination \in {"None", "Candidate", "Adopted"}
    /\ b2OrdinalTouched \in BOOLEAN
    /\ unmatchedRemoveTouched \in BOOLEAN

(* (SAFETY) The post-CAS delete never reclaims a `-1` body whose log is still unfolded behind a clamp:
   a body enters the round cleanup set only for a FULLY FOLDED log, so it is deleted only after its
   removal edge is durably adopted (delete-after-sealed-decrements). Equivalent to: A can be gone only
   if the cursor is past L. SabotageEdgeGranularity commits A while L is clamped and violates this. *)
NoDeleteBehindClamp == (~aPresent) => cursorPastL

(* A neutral orphan nomination becomes durable in the round CAS before its exact-token delete. *)
NominationAdoptedBeforeManifestDelete == (~manifestPresent) => nomination = "Adopted"

(* Neutral nomination is outside both ref-delta accounting mechanisms. *)
NeutralNominationPreservesRefAccounting == ~b2OrdinalTouched /\ ~unmatchedRemoveTouched

(* (LIVENESS) A clamp from a TRANSIENT fault is never permanent: once B heals, a later round folds the
   whole log. On the fixed path A survives the clamp round, so e1 re-folds and the cursor advances; under
   sabotage A was deleted, e1 can never re-fold, and cursorPastL is pinned FALSE forever -- this property
   fails, exposing the permanent freeze. *)
EventuallyFolded == bHealed ~> cursorPastL

(* Negated reachability witnesses: TLC must find the intended honest nomination and delete paths. *)
WITNESS_NEUTRAL_NOMINATION_ADOPTED ==
    ~(nomination = "Adopted" /\ manifestPresent /\ ~b2OrdinalTouched /\ ~unmatchedRemoveTouched)
WITNESS_MANIFEST_DELETE_AFTER_ADOPTION == ~(~manifestPresent /\ nomination = "Adopted")

=============================================================================
