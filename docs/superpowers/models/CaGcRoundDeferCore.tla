-------------------- MODULE CaGcRoundDeferCore --------------------
(* GC round skip-unchanged (DEFER) core — spec 2026-07-06-cas-gc-round-skip-unchanged-design.md.
   Phase 4 Lever A: a round that would make NO destructive decision DEFERs (re-adopts the sealed
   in-degree snapshot) instead of rebuilding it. Mirrors CaGcAckFloorCore's round shape
   (gcPhase idle -> running -> folded; GBegin / GFold / GComplete; a monotone heartbeat floor
   min_ack; a `retired` delete_pending pipeline graduated when condemn_round < min_ack).

   THE HAZARD UNDER TEST (mirror of the 2026-06-27 concurrent-leader leak, which was the
   unfolded -1 / owner-removal side): an unfolded owner-ADDITION (+1) that lands while a
   delete_pending blob graduates on the STALE (not-fully-folded) snapshot -> the stale in-degree
   still reads 0 while reality is 1 -> physically deleting a live-referenced blob = dangling.

   THE SAFETY RULE the model validates (spec S4.1): NO destructive decision is ever made on a
   not-fully-folded snapshot. Operationally, a due graduation FORCE-FOLDS first: GComplete may
   physically delete blob b only when `unfolded` holds no delta touching b (the fold that would
   cover b's protective +1 must have run this round). This is the exact mirror of the ack-floor
   model's "a ref landing after the fold cut is invisible to GComplete" interleaving: here a writer
   may add a +1 in the folded phase (after GFold), so `unfolded` is the after-cut carrier.

   THE LIVENESS RULE (spec S4.3): deferral is bounded (deferCount < MaxDefer) so every unfolded
   delta is eventually folded -- no permanent skip that would re-introduce the -1 leak.

   FOLD vs DEFER are mutually exclusive per idle round and their disjunction is total: DEFER when
   ~GraduationDue and the bound is not hit; otherwise a FOLD round (GBegin) is the enabled move.
   Weak fairness is placed ONLY on the round machinery (DeferRound / GBegin / GFold / GComplete) so
   the bound forces a fold in the honest model; writers and ack advance are the unconstrained
   environment (no fairness). Ref removal is carried through `unfolded` (op = "del") like additions;
   the delete guard force-folds on ANY pending delta for b, so the safety rule covers both signs.

   Each Sabotage* flag breaks exactly one load-bearing rule and MUST yield a counterexample:
     SabotageGraduateOnStale  -> drop the unfolded-covers-b delete guard -> NoOverDelete violated.
     SabotageUnboundedDefer   -> drop the deferCount < MaxDefer bound     -> EventuallyFolded violated. *)
EXTENDS Integers, FiniteSets

CONSTANTS
    Writers, Blobs, MaxRound, MaxDefer,
    SabotageGraduateOnStale,   \* graduate/delete on a stale snapshot (drop the unfolded-covers-b guard)
    SabotageUnboundedDefer     \* remove the deferCount < MaxDefer bound (permanent skip)

Rounds == 0..MaxRound

VARIABLES
    round,        \* published gc round (gc/state)
    present,      \* [Blobs -> BOOLEAN] blob body physically present
    folded,       \* sealed in-degree snapshot: SUBSET [b: Blobs, w: Writers]
    unfolded,     \* deferred edge deltas not yet folded: SUBSET [b: Blobs, w: Writers, op: {"add","del"}]
    retired,      \* delete_pending entries: SUBSET [b: Blobs, condemn_round: Rounds]
    minAck,       \* heartbeat floor (monotone, <= round)
    gcPhase,      \* {"idle","running","folded"}
    minAckL,      \* floor latched by GBegin (graduation reads the latched floor)
    deferCount,   \* consecutive deferred rounds since the last fold
    deletedThisStep,          \* blobs physically deleted by the LAST transition (for NoOverDelete)
    deferredWithUnfoldedEver  \* witness history: a DEFER round happened while unfolded # {}

vars == << round, present, folded, unfolded, retired, minAck, gcPhase, minAckL,
           deferCount, deletedThisStep, deferredWithUnfoldedEver >>

Indeg(b) == Cardinality({ rf \in folded : rf.b = b })       \* in-degree from the SEALED snapshot only
UnfoldedTouches(b) == \E u \in unfolded : u.b = b            \* any deferred delta (add OR del) for b
GraduationDue == \E e \in retired : e.condemn_round < minAck \* a delete_pending entry is due to graduate
FoldEnabled == GraduationDue \/ (~SabotageUnboundedDefer /\ deferCount >= MaxDefer)

Init ==
    /\ round = 0
    /\ present = [b \in Blobs |-> TRUE]
    /\ folded = {}
    /\ unfolded = {}
    /\ retired = {}
    /\ minAck = 0
    /\ gcPhase = "idle"
    /\ minAckL = 0
    /\ deferCount = 0
    /\ deletedThisStep = {}
    /\ deferredWithUnfoldedEver = FALSE

(* ---- environment (unconstrained: no fairness) ---- *)

(* A writer accepts a NEW owner edge (+1). It becomes a deferred delta -- invisible to the sealed
   snapshot until a fold drains it. Enabled on a present blob (a reference is always to live content;
   in reality a re-reference of a condemned blob is a fresh incarnation -- token detail lives in the
   ack-floor model). A writer may add in ANY gcPhase, so a +1 can land AFTER the fold cut. *)
WriterAddEdge(w, b) ==
    /\ present[b]
    /\ [b |-> b, w |-> w] \notin folded
    /\ ~(\E u \in unfolded : u.b = b /\ u.w = w /\ u.op = "add")
    /\ unfolded' = unfolded \cup { [b |-> b, w |-> w, op |-> "add"] }
    /\ deletedThisStep' = {}
    /\ UNCHANGED << round, present, folded, retired, minAck, gcPhase, minAckL, deferCount, deferredWithUnfoldedEver >>

(* A writer drops an owner edge (-1), carried as a deferred delta (the conservative-safe direction:
   an unfolded removal only delays condemnation). *)
WriterRemoveEdge(w, b) ==
    /\ [b |-> b, w |-> w] \in folded
    /\ ~(\E u \in unfolded : u.b = b /\ u.w = w /\ u.op = "del")
    /\ unfolded' = unfolded \cup { [b |-> b, w |-> w, op |-> "del"] }
    /\ deletedThisStep' = {}
    /\ UNCHANGED << round, present, folded, retired, minAck, gcPhase, minAckL, deferCount, deferredWithUnfoldedEver >>

(* The heartbeat floor rises as writers ack newer rounds; it can never run ahead of the published round. *)
AckAdvance ==
    /\ minAck < round
    /\ minAck' = minAck + 1
    /\ deletedThisStep' = {}
    /\ UNCHANGED << round, present, folded, unfolded, retired, gcPhase, minAckL, deferCount, deferredWithUnfoldedEver >>

(* ---- round machinery (weak-fair) ---- *)

(* DEFER: re-adopt the sealed snapshot. Advances `round` (so min_ack may rise via acks) but drains
   NOTHING and deletes nothing. Guarded by ~GraduationDue (no destructive decision due) and the
   defer bound. round/deferCount cap so the honest state space stays finite (the bound disables it
   at MaxDefer); under SabotageUnboundedDefer the guard's OR keeps it enabled forever. *)
DeferRound ==
    /\ gcPhase = "idle"
    /\ ~GraduationDue
    /\ (SabotageUnboundedDefer \/ deferCount < MaxDefer)
    /\ round' = IF round < MaxRound THEN round + 1 ELSE round
    /\ deferCount' = IF deferCount < MaxDefer THEN deferCount + 1 ELSE deferCount
    /\ deferredWithUnfoldedEver' = (deferredWithUnfoldedEver \/ unfolded # {})
    /\ deletedThisStep' = {}
    /\ UNCHANGED << present, folded, unfolded, retired, minAck, gcPhase, minAckL >>

(* FOLD round, step 1: latch the floor. Enabled only when a fold is warranted (a graduation is due,
   or the defer bound is hit) -- mutually exclusive with DeferRound so the environment can never be
   forced to fold while it is still permitted to defer (this is what lets SabotageUnboundedDefer
   starve the fold). *)
GBegin ==
    /\ gcPhase = "idle"
    /\ FoldEnabled
    /\ minAckL' = minAck
    /\ gcPhase' = "running"
    /\ deletedThisStep' = {}
    /\ UNCHANGED << round, present, folded, unfolded, retired, minAck, deferCount, deferredWithUnfoldedEver >>

(* FOLD round, step 2: drain ALL accumulated deltas into the sealed snapshot (adds referenced, dels
   removed) and reset the defer counter. This is the force-fold that makes the graduation decision
   run on an up-to-date in-degree. *)
GFold ==
    /\ gcPhase = "running"
    /\ LET adds == { [b |-> u.b, w |-> u.w] : u \in { x \in unfolded : x.op = "add" } }
           dels == { [b |-> u.b, w |-> u.w] : u \in { x \in unfolded : x.op = "del" } }
       IN folded' = (folded \cup adds) \ dels
    /\ unfolded' = {}
    /\ deferCount' = 0
    /\ gcPhase' = "folded"
    /\ deletedThisStep' = {}
    /\ UNCHANGED << round, present, retired, minAck, minAckL, deferredWithUnfoldedEver >>

(* FOLD round, step 3: over the folded cut, graduate/condemn/delete.
   A due entry whose in-degree RECOVERED (a folded +1) is cancelled (dropped, blob survives).
   A due entry at in-degree 0 physically deletes its blob ONLY when `unfolded` holds no delta
   touching it (force-fold-before-graduation) -- the load-bearing guard SabotageGraduateOnStale
   drops. A writer that added a +1 in the "folded" phase (after GFold) sits in `unfolded` and is
   invisible to Indeg here, so the guard is the only thing protecting it. New zero-in-degree blobs
   are condemned into the delete_pending list. *)
GComplete ==
    /\ gcPhase = "folded"
    /\ LET due        == { e \in retired : e.condemn_round < minAckL }
           recovered  == { e \in due : Indeg(e.b) > 0 }
           deletable  == { e \in due : Indeg(e.b) = 0
                                       /\ (SabotageGraduateOnStale \/ ~UnfoldedTouches(e.b)) }
           kills      == { e.b : e \in deletable }
           survivors  == retired \ (recovered \cup deletable)
           newly      == { [b |-> bb, condemn_round |-> round] :
                             bb \in { x \in Blobs : present[x] /\ Indeg(x) = 0
                                                    /\ ~(\E e \in retired : e.b = x) } }
       IN /\ present' = [b \in Blobs |-> IF b \in kills THEN FALSE ELSE present[b]]
          /\ retired' = survivors \cup newly
          /\ deletedThisStep' = kills
    /\ round' = IF round < MaxRound THEN round + 1 ELSE round
    /\ gcPhase' = "idle"
    /\ UNCHANGED << folded, unfolded, minAck, minAckL, deferCount, deferredWithUnfoldedEver >>

(* Self-loop so bounded counters exhausting is not a TLC deadlock (house pattern). *)
NoOp == UNCHANGED vars

Next ==
    \/ \E w \in Writers, b \in Blobs : WriterAddEdge(w, b) \/ WriterRemoveEdge(w, b)
    \/ AckAdvance
    \/ DeferRound
    \/ GBegin \/ GFold \/ GComplete
    \/ NoOp

(* Weak fairness on the round machinery only. The bound forces DeferRound to yield to GBegin at
   MaxDefer (honest liveness); dropping the bound lets DeferRound run forever (the sabotage). *)
Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(DeferRound)
    /\ WF_vars(GBegin)
    /\ WF_vars(GFold)
    /\ WF_vars(GComplete)

(* ---- invariants ---- *)

TypeOK ==
    /\ round \in Rounds
    /\ minAck \in Rounds
    /\ minAckL \in Rounds
    /\ deferCount \in 0..MaxDefer
    /\ gcPhase \in {"idle", "running", "folded"}
    /\ present \in [Blobs -> BOOLEAN]
    /\ \A rf \in folded : rf.b \in Blobs /\ rf.w \in Writers
    /\ \A u \in unfolded : u.b \in Blobs /\ u.w \in Writers /\ u.op \in {"add", "del"}
    /\ \A e \in retired : e.b \in Blobs /\ e.condemn_round \in Rounds
    /\ deletedThisStep \subseteq Blobs
    /\ deferredWithUnfoldedEver \in BOOLEAN

(* No reference -- folded snapshot edge, or an unfolded ADDITION -- may point at an absent blob. *)
NoDangle ==
    /\ \A rf \in folded : present[rf.b]
    /\ \A u \in unfolded : (u.op = "add") => present[u.b]

(* No blob is physically deleted while ANY unfolded delta could still protect it (the +1 hazard). *)
NoOverDelete == \A b \in Blobs :
    (b \in deletedThisStep) => (\A e \in unfolded : e.b # b)

(* Bounded deferral: an unfolded delta is always eventually folded (no permanent skip). *)
EventuallyFolded == (unfolded # {}) ~> (unfolded = {})

(* ---- witness (negated reachability; a TLC "violation" = the DEFER-then-FOLD state IS reachable) ----
   deferredWithUnfoldedEver records that a DEFER round ran while unfolded # {} (so deferCount > 0 at
   that step); unfolded only ever returns to {} via GFold. Hence a state with both flags means a real
   DEFER-then-FOLD happened -- the safety is not vacuous. *)
W_DeferThenFold == ~(deferredWithUnfoldedEver /\ unfolded = {})

=============================================================================
