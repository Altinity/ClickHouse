-------------------- MODULE CaGcAckFloorZombie --------------------
(* Two-leader companion to CaGcAckFloorCore — formalizes the Task-9 amendment (two-phase
   graduation / delete_pending) of spec 2026-07-02-cas-gc-ack-floor-fence-redesign.md.

   The one-CAS round means a DEPOSED leader reaches the delete site with state latched at its
   lease acquire: its retired-list snapshot can be arbitrarily stale (an entry spared by the new
   leader — pipeline reset — then re-condemned at r' >= min_ack looks floor-passed under its old
   r). The amendment restricts pre-CAS deletes to entries ALREADY PUBLISHED as delete_pending by a
   previous round's CAS; fresh graduations are only (re)published pending, never deleted in the
   same pass. This module checks exactly that discipline with TWO leaders whose passes fully
   interleave: each leader latches (round, retired list, ref-cut in-degrees) at GBegin, deletes
   pre-publish, and its publish CAS succeeds only if (round, retired) are unchanged since the
   latch. SabotageEagerZombieDelete lets a pass ALSO delete its fresh graduations (the
   pre-amendment behavior) and MUST yield an INV_NO_DANGLE counterexample.

   Writer machinery is trimmed to what the hazard needs (always-live writers, beat with drain,
   prepare/land two-step commits with the recreate gate, ref drop). Sleeper/fence-out/terminate
   live in CaGcAckFloorCore and are orthogonal here. *)
EXTENDS Integers, FiniteSets

CONSTANTS
    Writers, Leaders, Blobs, MaxRound, MaxTok,
    SabotageEagerZombieDelete   \* a pass deletes its FRESH graduations too (not just published pendings)

Toks == 1..MaxTok
Rounds == 0..MaxRound
Entry == [b : Blobs, t : Toks, r : Rounds, pending : BOOLEAN]

VARIABLES
    round,       \* published gc round (gc/state)
    retired,     \* published current retired list: SUBSET Entry
    present, tok, nextTok, deadTok,
    refs,        \* committed references: SUBSET [b: Blobs, t: Toks, w: Writers]
    wView, wAck, \* [Writers -> Rounds]
    wPending,    \* [Writers -> SUBSET [b: Blobs, t: Toks]] ({} or singleton)
    gPhase,      \* [Leaders -> {"idle","latched"}]
    snapFloor,   \* [Leaders -> Rounds]            the ack floor latched at GBegin — LOAD-BEARING
                 \* ORDER: the floor MUST be latched no later than the fold cut. A floor read after
                 \* the cut sees acks advertised by writers whose in-flight commits landed AFTER the
                 \* cut (invisible to this pass's in-degrees) -> a fresh graduation over a live ref.
    snapRound,   \* [Leaders -> Rounds]            latched at GBegin
    snapRetired, \* [Leaders -> SUBSET Entry]      latched at GBegin
    snapIndeg,   \* [Leaders -> [Blobs -> 0..10]]  ref-cut in-degree latched at GBegin
    deletedEver

vars == << round, retired, present, tok, nextTok, deadTok, refs, wView, wAck, wPending,
           gPhase, snapFloor, snapRound, snapRetired, snapIndeg, deletedEver >>

MinAck == CHOOSE m \in Rounds : /\ \E w \in Writers : wAck[w] = m
                                /\ \A w \in Writers : wAck[w] >= m

Init ==
    /\ round = 0 /\ retired = {}
    /\ present = [b \in Blobs |-> TRUE] /\ tok = [b \in Blobs |-> 1]
    /\ nextTok = [b \in Blobs |-> 2] /\ deadTok = [b \in Blobs |-> {}]
    /\ refs = {}
    /\ wView = [w \in Writers |-> 0] /\ wAck = [w \in Writers |-> 0]
    /\ wPending = [w \in Writers |-> {}]
    /\ gPhase = [l \in Leaders |-> "idle"]
    /\ snapFloor = [l \in Leaders |-> 0]
    /\ snapRound = [l \in Leaders |-> 0]
    /\ snapRetired = [l \in Leaders |-> {}]
    /\ snapIndeg = [l \in Leaders |-> [b \in Blobs |-> 0]]
    /\ deletedEver = FALSE

(* ---- writers (always live; drain + gate as in the core model) ---- *)

WBeat(w) ==
    /\ wPending[w] = {}                                        \* drain-before-advertise
    /\ wView' = [wView EXCEPT ![w] = round]
    /\ wAck' = [wAck EXCEPT ![w] = round]
    /\ UNCHANGED << round, retired, present, tok, nextTok, deadTok, refs, wPending,
                    gPhase, snapFloor, snapRound, snapRetired, snapIndeg, deletedEver >>

WPrepare(w, b) ==
    /\ wPending[w] = {} /\ nextTok[b] <= MaxTok
    /\ LET visible == \E e \in retired : e.b = b /\ e.t = tok[b] /\ e.r <= wView[w]
           mustRecreate == (~present[b]) \/ visible
       IN IF mustRecreate
          THEN /\ present' = [present EXCEPT ![b] = TRUE]
               /\ tok' = [tok EXCEPT ![b] = nextTok[b]]
               /\ nextTok' = [nextTok EXCEPT ![b] = @ + 1]
               /\ wPending' = [wPending EXCEPT ![w] = { [b |-> b, t |-> nextTok[b]] }]
          ELSE /\ present[b]
               /\ wPending' = [wPending EXCEPT ![w] = { [b |-> b, t |-> tok[b]] }]
               /\ UNCHANGED << present, tok, nextTok >>
    /\ UNCHANGED << round, retired, deadTok, refs, wView, wAck,
                    gPhase, snapFloor, snapRound, snapRetired, snapIndeg, deletedEver >>

WLand(w) ==
    /\ wPending[w] # {}
    /\ LET p == CHOOSE x \in wPending[w] : TRUE
       IN refs' = refs \cup { [b |-> p.b, t |-> p.t, w |-> w] }
    /\ wPending' = [wPending EXCEPT ![w] = {}]
    /\ UNCHANGED << round, retired, present, tok, nextTok, deadTok, wView, wAck,
                    gPhase, snapFloor, snapRound, snapRetired, snapIndeg, deletedEver >>

WDropRef(rf) ==
    /\ rf \in refs
    /\ refs' = refs \ {rf}
    /\ UNCHANGED << round, retired, present, tok, nextTok, deadTok, wView, wAck, wPending,
                    gPhase, snapFloor, snapRound, snapRetired, snapIndeg, deletedEver >>

(* ---- leaders: latch -> delete(pre-publish) -> try-publish ---- *)

(* GBegin(l): one atomic latch of (round, retired, floor-independent ref-cut in-degrees). The
   in-degree latch models the pass's fold cut: refs landing after it are invisible to THIS pass. *)
GBegin(l) ==
    /\ gPhase[l] = "idle" /\ round < MaxRound
    /\ snapFloor' = [snapFloor EXCEPT ![l] = MinAck]   \* floor latched BEFORE the cut (order invariant)
    /\ snapRound' = [snapRound EXCEPT ![l] = round]
    /\ snapRetired' = [snapRetired EXCEPT ![l] = retired]
    /\ snapIndeg' = [snapIndeg EXCEPT ![l] =
                        [b \in Blobs |-> Cardinality({ rf \in refs : rf.b = b })]]
    /\ gPhase' = [gPhase EXCEPT ![l] = "latched"]
    /\ UNCHANGED << round, retired, present, tok, nextTok, deadTok, refs, wView, wAck,
                    wPending, deletedEver >>

(* GFinish(l): the pass tail — pre-publish deletes, then the single CAS.
   DELETES (destructive, execute regardless of the publish outcome):
     honest    = latched entries with pending = TRUE and latched in-degree 0;
     sabotage += latched FRESH graduations (r < the pass floor, in-degree 0) — the pre-amendment
                 single-phase behavior the zombie hazard breaks.
   Exact-token: a delete lands only where tok[b] still equals the entry token.
   PUBLISH: succeeds only if (round, retired) are UNCHANGED since GBegin (the gc/state token
   guard); a deposed leader's merge output evaporates, its deletes do not — which is exactly why
   the honest delete set may contain nothing that was not already published pending. *)
GFinish(l) ==
    /\ gPhase[l] = "latched"
    /\ LET floorL == snapFloor[l]
           snap == snapRetired[l]
           indeg(b) == snapIndeg[l][b]
           redel == { e \in snap : e.pending /\ indeg(e.b) = 0 }
           fresh == { e \in snap : ~e.pending /\ e.r < floorL /\ indeg(e.b) = 0 }
           dels  == IF SabotageEagerZombieDelete THEN redel \cup fresh ELSE redel
           kills == { e \in dels : tok[e.b] = e.t }
           canPublish == (round = snapRound[l]) /\ (retired = snapRetired[l])
           newly == { [b |-> b, t |-> tok[b], r |-> snapRound[l] + 1, pending |-> FALSE] :
                        b \in { bb \in Blobs : present[bb] /\ indeg(bb) = 0
                                /\ ~(\E e \in snap : e.b = bb) } }
           kept  == { e \in snap : indeg(e.b) = 0 /\ e \notin dels /\ ~(e.r < floorL /\ ~e.pending) }
           pend  == { [e EXCEPT !.pending = TRUE] :
                        e \in { x \in snap : ~x.pending /\ x.r < floorL /\ indeg(x.b) = 0 } }
       IN /\ present' = [b \in Blobs |-> IF \E e \in kills : e.b = b THEN FALSE ELSE present[b]]
          /\ deadTok' = [b \in Blobs |-> IF \E e \in kills : e.b = b
                                         THEN deadTok[b] \cup {tok[b]} ELSE deadTok[b]]
          /\ deletedEver' = (deletedEver \/ kills # {})
          /\ IF canPublish
             THEN /\ retired' = kept \cup pend \cup newly
                  /\ round' = round + 1
             ELSE UNCHANGED << retired, round >>          \* deposed: publish CAS fails, pass evaporates
    /\ gPhase' = [gPhase EXCEPT ![l] = "idle"]
    /\ UNCHANGED << tok, nextTok, refs, wView, wAck, wPending, snapFloor, snapRound, snapRetired, snapIndeg >>

NoOp == UNCHANGED vars

Next ==
    \/ \E w \in Writers : WBeat(w) \/ WLand(w)
    \/ \E w \in Writers, b \in Blobs : WPrepare(w, b)
    \/ \E rf \in refs : WDropRef(rf)
    \/ \E l \in Leaders : GBegin(l) \/ GFinish(l)
    \/ NoOp

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ round \in Rounds
    /\ \A e \in retired : e \in Entry
    /\ \A w \in Writers : wAck[w] <= wView[w]

INV_NO_DANGLE == \A rf \in refs : present[rf.b]
INV_NO_RETURN == \A rf \in refs : rf.t \notin deadTok[rf.b]

W_DeleteHappens == ~deletedEver

=============================================================================
