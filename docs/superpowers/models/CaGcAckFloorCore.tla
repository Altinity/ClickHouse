-------------------- MODULE CaGcAckFloorCore --------------------
(* Ack-floor GC fence core — spec 2026-07-02-cas-gc-ack-floor-fence-redesign.md.
   Focused model (companion regime to CaCasMountCore): writers advertise observed_gc_round through
   heartbeats; GC graduates a retired entry only when condemn_round < min_ack computed over
   heartbeats that are live OR expired-but-not-fenced; a fenced heartbeat can never renew or land a
   commit again. Commits are two-step (prepare = gate evaluation, land = CAS response) so the
   in-flight window is a real interleaving. The pass is three steps (begin/fold/complete) so a
   commit landing between the fold cut and the deletes is a real interleaving.
   Ref removal is modeled as immediate (no fold lag): removal lag only delays condemnation
   (safe side); the hazard under test is the racing ADD.
   wPending is SET-valued (at most one element; {} = no in-flight commit) so TLC never compares a
   record against a non-record sentinel.
   Each Sabotage* flag breaks exactly one load-bearing rule and MUST yield a counterexample. *)
EXTENDS Integers, FiniteSets

CONSTANTS
    Writers, Blobs, MaxRound, MaxTok,
    SabotageIgnoreAckFloor,      \* graduate ignoring the floor entirely
    SabotageAckWithoutRead,      \* ack advances without installing the view
    SabotageAckBeforeDrain,      \* ack advances while an old-view commit is in flight
    SabotageSleeperRearm,        \* floor excludes expired-UNFENCED heartbeats (assumes dead without fence-out)
    SabotageSkipChangedShard,    \* the fold cut leaves one landed ref unconsumed
    SabotageAdoptRetiredToken,   \* the gate references a visibly-retired token instead of recreating
    SabotageOpenWriteBeforeLoad  \* a fresh mount starts with an unloaded (round-0) view

Toks == 1..MaxTok
Rounds == 0..MaxRound

VARIABLES
    round,       \* published gc round (gc/state)
    present,     \* [Blobs -> BOOLEAN]
    tok,         \* [Blobs -> Toks] current incarnation token
    nextTok,     \* [Blobs -> Toks \cup {MaxTok + 1}] next fresh token to mint
    deadTok,     \* [Blobs -> SUBSET Toks] deleted incarnations (INV_NO_RETURN oracle)
    retired,     \* current retired list: SUBSET [b: Blobs, t: Toks, r: Rounds]
    landed,      \* landed-but-unfolded refs: SUBSET [b: Blobs, t: Toks, w: Writers]
    folded,      \* folded refs (the in-degree source): SUBSET [b: Blobs, t: Toks, w: Writers]
    wStatus,     \* [Writers -> {"unmounted","live","expired","fenced","terminated"}]
    wView,       \* [Writers -> Rounds] installed view (retired-list version loaded)
    wAck,        \* [Writers -> Rounds] advertised observed_gc_round
    wPending,    \* [Writers -> SUBSET [b: Blobs, t: Toks]] in-flight commit; {} or a singleton
    gcPhase,     \* {"idle","running","folded"}
    minAckL,     \* the floor latched by GBegin (0..MaxRound+1; MaxRound+1 = empty floor set)
    sparedEver, recreatedEver, deletedEver   \* witness history flags

vars == << round, present, tok, nextTok, deadTok, retired, landed, folded,
           wStatus, wView, wAck, wPending, gcPhase, minAckL,
           sparedEver, recreatedEver, deletedEver >>

Indeg(b) == Cardinality({ rf \in folded : rf.b = b })
FloorSet == { w \in Writers : wStatus[w] \in
                (IF SabotageSleeperRearm THEN {"live"} ELSE {"live", "expired"}) }
MinAck == IF FloorSet = {} THEN MaxRound + 1
          ELSE CHOOSE m \in Rounds : /\ \E w \in FloorSet : wAck[w] = m
                                     /\ \A w \in FloorSet : wAck[w] >= m

Init ==
    /\ round = 0
    /\ present = [b \in Blobs |-> TRUE] /\ tok = [b \in Blobs |-> 1]
    /\ nextTok = [b \in Blobs |-> 2] /\ deadTok = [b \in Blobs |-> {}]
    /\ retired = {} /\ landed = {} /\ folded = {}
    /\ wStatus = [w \in Writers |-> "unmounted"]
    /\ wView = [w \in Writers |-> 0] /\ wAck = [w \in Writers |-> 0]
    /\ wPending = [w \in Writers |-> {}]
    /\ gcPhase = "idle" /\ minAckL = 0
    /\ sparedEver = FALSE /\ recreatedEver = FALSE /\ deletedEver = FALSE

(* ---- writer actions ---- *)

WOpen(w) ==
    /\ wStatus[w] = "unmounted"
    /\ wStatus' = [wStatus EXCEPT ![w] = "live"]
    /\ wView' = [wView EXCEPT ![w] = IF SabotageOpenWriteBeforeLoad THEN 0 ELSE round]
    /\ wAck' = [wAck EXCEPT ![w] = IF SabotageOpenWriteBeforeLoad THEN 0 ELSE round]
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, landed, folded,
                    wPending, gcPhase, minAckL, sparedEver, recreatedEver, deletedEver >>

WBeat(w) ==
    /\ wStatus[w] = "live"
    /\ (SabotageAckBeforeDrain \/ wPending[w] = {})            \* drain-before-advertise
    /\ wView' = IF SabotageAckWithoutRead THEN wView ELSE [wView EXCEPT ![w] = round]
    /\ wAck' = [wAck EXCEPT ![w] = round]
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, landed, folded,
                    wStatus, wPending, gcPhase, minAckL, sparedEver, recreatedEver, deletedEver >>

(* Gate evaluation. `visible` = the entry for the blob's CURRENT token is in the writer's loaded
   list version. Visible + honest => recreate (fresh incarnation referenced, never the listed one) —
   the implementation's putBlob cold-reuse rule. Absent blob => re-upload from source (same recreate
   shape). The blob overwrite is immediate (a real PUT at prepare time); only the REF lands later. *)
WPrepare(w, b) ==
    /\ wStatus[w] = "live" /\ wPending[w] = {} /\ nextTok[b] <= MaxTok
    /\ LET visible == \E e \in retired : e.b = b /\ e.t = tok[b] /\ e.r <= wView[w]
           mustRecreate == (~present[b]) \/ (visible /\ ~SabotageAdoptRetiredToken)
       IN IF mustRecreate
          THEN /\ present' = [present EXCEPT ![b] = TRUE]
               /\ tok' = [tok EXCEPT ![b] = nextTok[b]]
               /\ nextTok' = [nextTok EXCEPT ![b] = @ + 1]
               /\ wPending' = [wPending EXCEPT ![w] = { [b |-> b, t |-> nextTok[b]] }]
               /\ recreatedEver' = TRUE
          ELSE /\ present[b]                                    \* adopt the current incarnation
               /\ wPending' = [wPending EXCEPT ![w] = { [b |-> b, t |-> tok[b]] }]
               /\ UNCHANGED << present, tok, nextTok, recreatedEver >>
    /\ UNCHANGED << round, deadTok, retired, landed, folded, wStatus, wView, wAck,
                    gcPhase, minAckL, sparedEver, deletedEver >>

WLand(w) ==
    /\ wStatus[w] = "live" /\ wPending[w] # {}
    /\ LET p == CHOOSE x \in wPending[w] : TRUE
       IN landed' = landed \cup { [b |-> p.b, t |-> p.t, w |-> w] }
    /\ wPending' = [wPending EXCEPT ![w] = {}]
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, folded, wStatus,
                    wView, wAck, gcPhase, minAckL, sparedEver, recreatedEver, deletedEver >>

WDropRef(rf) ==
    /\ rf \in folded
    /\ folded' = folded \ {rf}
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, landed, wStatus,
                    wView, wAck, wPending, gcPhase, minAckL, sparedEver, recreatedEver, deletedEver >>

WExpire(w) ==
    /\ wStatus[w] = "live"
    /\ wStatus' = [wStatus EXCEPT ![w] = "expired"]
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, landed, folded,
                    wView, wAck, wPending, gcPhase, minAckL, sparedEver, recreatedEver, deletedEver >>

(* A live-again sleeper: an EXPIRED (never fenced) heartbeat renews successfully. Honest and safe —
   the honest floor still counts expired heartbeats. Becomes lethal only when the floor excludes
   expired-unfenced writers (SabotageSleeperRearm). The pending commit survives the nap. *)
WSleeperRenew(w) ==
    /\ wStatus[w] = "expired"
    /\ wStatus' = [wStatus EXCEPT ![w] = "live"]
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, landed, folded,
                    wView, wAck, wPending, gcPhase, minAckL, sparedEver, recreatedEver, deletedEver >>

WTerminate(w) ==
    /\ wStatus[w] = "live" /\ wPending[w] = {}                  \* graceful stop drains first
    /\ wStatus' = [wStatus EXCEPT ![w] = "terminated"]
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, landed, folded,
                    wView, wAck, wPending, gcPhase, minAckL, sparedEver, recreatedEver, deletedEver >>

(* ---- GC actions ---- *)

GFenceOut(w) ==
    /\ wStatus[w] = "expired"
    /\ wStatus' = [wStatus EXCEPT ![w] = "fenced"]              \* token-guarded fence-out: renew dead forever
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, landed, folded,
                    wView, wAck, wPending, gcPhase, minAckL, sparedEver, recreatedEver, deletedEver >>

GBegin ==
    /\ gcPhase = "idle" /\ round < MaxRound
    /\ minAckL' = MinAck
    /\ gcPhase' = "running"
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, landed, folded,
                    wStatus, wView, wAck, wPending, sparedEver, recreatedEver, deletedEver >>

GFold ==
    /\ gcPhase = "running"
    /\ IF SabotageSkipChangedShard /\ landed # {}
       THEN \E skip \in landed : /\ folded' = folded \cup (landed \ {skip})
                                 /\ landed' = {skip}
       ELSE /\ folded' = folded \cup landed
            /\ landed' = {}
    /\ gcPhase' = "folded"
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, wStatus, wView,
                    wAck, wPending, minAckL, sparedEver, recreatedEver, deletedEver >>

(* Merge + graduate + condemn + publish, over the folded cut. Refs landing after GFold sit in
   `landed` and are invisible here — exactly the implementation's cut. Exact-token delete: a
   graduated entry whose token no longer matches (recreated meanwhile) is dropped without a delete. *)
GComplete ==
    /\ gcPhase = "folded"
    /\ LET grads == { e \in retired : Indeg(e.b) = 0
                                      /\ (SabotageIgnoreAckFloor \/ e.r < minAckL) }
           spares == { e \in retired : Indeg(e.b) > 0 }
           kills == { e \in grads : tok[e.b] = e.t }            \* exact-token delete lands
           newly == { [b |-> b, t |-> tok[b], r |-> round + 1] :
                        b \in { bb \in Blobs : present[bb] /\ Indeg(bb) = 0
                                /\ ~(\E e \in retired : e.b = bb) } }
       IN /\ present' = [b \in Blobs |-> IF \E e \in kills : e.b = b THEN FALSE ELSE present[b]]
          /\ deadTok' = [b \in Blobs |-> IF \E e \in kills : e.b = b
                                         THEN deadTok[b] \cup {tok[b]} ELSE deadTok[b]]
          /\ retired' = (retired \ (grads \cup spares)) \cup newly
          /\ sparedEver' = (sparedEver \/ spares # {})
          /\ deletedEver' = (deletedEver \/ kills # {})
    /\ round' = round + 1
    /\ gcPhase' = "idle"
    /\ UNCHANGED << tok, nextTok, landed, folded, wStatus, wView, wAck, wPending, minAckL,
                    recreatedEver >>

(* Self-loop so bounded counters (round/tok) exhausting is not a TLC deadlock (house pattern). *)
NoOp == UNCHANGED vars

Next ==
    \/ \E w \in Writers : WOpen(w) \/ WBeat(w) \/ WLand(w) \/ WExpire(w)
                          \/ WSleeperRenew(w) \/ WTerminate(w) \/ GFenceOut(w)
    \/ \E w \in Writers, b \in Blobs : WPrepare(w, b)
    \/ \E rf \in folded : WDropRef(rf)
    \/ GBegin \/ GFold \/ GComplete
    \/ NoOp

Spec == Init /\ [][Next]_vars

(* ---- invariants ---- *)

TypeOK ==
    /\ round \in Rounds /\ gcPhase \in {"idle", "running", "folded"}
    /\ minAckL \in 0..(MaxRound + 1)
    /\ \A w \in Writers : /\ wAck[w] \in Rounds /\ wView[w] \in Rounds
                          /\ Cardinality(wPending[w]) <= 1
    /\ \A e \in retired : e.b \in Blobs /\ e.t \in Toks /\ e.r \in Rounds

(* No reference — landed or folded — may point at an absent blob... *)
INV_NO_DANGLE == \A rf \in (landed \cup folded) : present[rf.b]
(* ...or bind a deleted incarnation. *)
INV_NO_RETURN == \A rf \in (landed \cup folded) : rf.t \notin deadTok[rf.b]
(* The honest ack never runs ahead of the installed view. *)
INV_ACK_LE_VIEW == \A w \in Writers : wAck[w] <= wView[w]

(* ---- witnesses (negated reachability; a TLC "violation" = the state IS reachable) ---- *)
W_DeleteHappens == ~deletedEver
W_SpareHappens == ~sparedEver
W_RecreateHappens == ~recreatedEver

=============================================================================
