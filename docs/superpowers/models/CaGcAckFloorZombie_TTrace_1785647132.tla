---- MODULE CaGcAckFloorZombie_TTrace_1785647132 ----
EXTENDS CaGcAckFloorZombie, Sequences, TLCExt, CaGcAckFloorZombie_TEConstants, Toolbox, Naturals, TLC

_expression ==
    LET CaGcAckFloorZombie_TEExpression == INSTANCE CaGcAckFloorZombie_TEExpression
    IN CaGcAckFloorZombie_TEExpression!expression
----

_trace ==
    LET CaGcAckFloorZombie_TETrace == INSTANCE CaGcAckFloorZombie_TETrace
    IN CaGcAckFloorZombie_TETrace!trace
----

_inv ==
    ~(
        TLCGet("level") = Len(_TETrace)
        /\
        deadTok = ((b1 :> {1}))
        /\
        deletedEver = (TRUE)
        /\
        snapFloor = ((l1 :> 2 @@ l2 :> 2))
        /\
        wAck = ((w1 :> 2 @@ w2 :> 2))
        /\
        snapIndeg = ((l1 :> (b1 :> 1) @@ l2 :> (b1 :> 0)))
        /\
        wView = ((w1 :> 2 @@ w2 :> 2))
        /\
        nextTok = ((b1 :> 2))
        /\
        gPhase = ((l1 :> "idle" @@ l2 :> "idle"))
        /\
        tok = ((b1 :> 1))
        /\
        round = (3)
        /\
        wPending = ((w1 :> {} @@ w2 :> {}))
        /\
        refs = ({[b |-> b1, t |-> 1, w |-> w1]})
        /\
        snapRetired = ((l1 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]} @@ l2 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]}))
        /\
        snapRound = ((l1 :> 2 @@ l2 :> 2))
        /\
        retired = ({})
        /\
        present = ((b1 :> FALSE))
    )
----

_init ==
    /\ deletedEver = _TETrace[1].deletedEver
    /\ snapFloor = _TETrace[1].snapFloor
    /\ retired = _TETrace[1].retired
    /\ snapRetired = _TETrace[1].snapRetired
    /\ tok = _TETrace[1].tok
    /\ snapRound = _TETrace[1].snapRound
    /\ refs = _TETrace[1].refs
    /\ wView = _TETrace[1].wView
    /\ gPhase = _TETrace[1].gPhase
    /\ wAck = _TETrace[1].wAck
    /\ snapIndeg = _TETrace[1].snapIndeg
    /\ nextTok = _TETrace[1].nextTok
    /\ round = _TETrace[1].round
    /\ present = _TETrace[1].present
    /\ wPending = _TETrace[1].wPending
    /\ deadTok = _TETrace[1].deadTok
----

_next ==
    /\ \E i,j \in DOMAIN _TETrace:
        /\ \/ /\ j = i + 1
              /\ i = TLCGet("level")
        /\ deletedEver  = _TETrace[i].deletedEver
        /\ deletedEver' = _TETrace[j].deletedEver
        /\ snapFloor  = _TETrace[i].snapFloor
        /\ snapFloor' = _TETrace[j].snapFloor
        /\ retired  = _TETrace[i].retired
        /\ retired' = _TETrace[j].retired
        /\ snapRetired  = _TETrace[i].snapRetired
        /\ snapRetired' = _TETrace[j].snapRetired
        /\ tok  = _TETrace[i].tok
        /\ tok' = _TETrace[j].tok
        /\ snapRound  = _TETrace[i].snapRound
        /\ snapRound' = _TETrace[j].snapRound
        /\ refs  = _TETrace[i].refs
        /\ refs' = _TETrace[j].refs
        /\ wView  = _TETrace[i].wView
        /\ wView' = _TETrace[j].wView
        /\ gPhase  = _TETrace[i].gPhase
        /\ gPhase' = _TETrace[j].gPhase
        /\ wAck  = _TETrace[i].wAck
        /\ wAck' = _TETrace[j].wAck
        /\ snapIndeg  = _TETrace[i].snapIndeg
        /\ snapIndeg' = _TETrace[j].snapIndeg
        /\ nextTok  = _TETrace[i].nextTok
        /\ nextTok' = _TETrace[j].nextTok
        /\ round  = _TETrace[i].round
        /\ round' = _TETrace[j].round
        /\ present  = _TETrace[i].present
        /\ present' = _TETrace[j].present
        /\ wPending  = _TETrace[i].wPending
        /\ wPending' = _TETrace[j].wPending
        /\ deadTok  = _TETrace[i].deadTok
        /\ deadTok' = _TETrace[j].deadTok

\* Uncomment the ASSUME below to write the states of the error trace
\* to the given file in Json format. Note that you can pass any tuple
\* to `JsonSerialize`. For example, a sub-sequence of _TETrace.
    \* ASSUME
    \*     LET J == INSTANCE Json
    \*         IN J!JsonSerialize("CaGcAckFloorZombie_TTrace_1785647132.json", _TETrace)

=============================================================================

 Note that you can extract this module `CaGcAckFloorZombie_TEExpression`
  to a dedicated file to reuse `expression` (the module in the 
  dedicated `CaGcAckFloorZombie_TEExpression.tla` file takes precedence 
  over the module `CaGcAckFloorZombie_TEExpression` below).

---- MODULE CaGcAckFloorZombie_TEExpression ----
EXTENDS CaGcAckFloorZombie, Sequences, TLCExt, CaGcAckFloorZombie_TEConstants, Toolbox, Naturals, TLC

expression == 
    [
        \* To hide variables of the `CaGcAckFloorZombie` spec from the error trace,
        \* remove the variables below.  The trace will be written in the order
        \* of the fields of this record.
        deletedEver |-> deletedEver
        ,snapFloor |-> snapFloor
        ,retired |-> retired
        ,snapRetired |-> snapRetired
        ,tok |-> tok
        ,snapRound |-> snapRound
        ,refs |-> refs
        ,wView |-> wView
        ,gPhase |-> gPhase
        ,wAck |-> wAck
        ,snapIndeg |-> snapIndeg
        ,nextTok |-> nextTok
        ,round |-> round
        ,present |-> present
        ,wPending |-> wPending
        ,deadTok |-> deadTok
        
        \* Put additional constant-, state-, and action-level expressions here:
        \* ,_stateNumber |-> _TEPosition
        \* ,_deletedEverUnchanged |-> deletedEver = deletedEver'
        
        \* Format the `deletedEver` variable as Json value.
        \* ,_deletedEverJson |->
        \*     LET J == INSTANCE Json
        \*     IN J!ToJson(deletedEver)
        
        \* Lastly, you may build expressions over arbitrary sets of states by
        \* leveraging the _TETrace operator.  For example, this is how to
        \* count the number of times a spec variable changed up to the current
        \* state in the trace.
        \* ,_deletedEverModCount |->
        \*     LET F[s \in DOMAIN _TETrace] ==
        \*         IF s = 1 THEN 0
        \*         ELSE IF _TETrace[s].deletedEver # _TETrace[s-1].deletedEver
        \*             THEN 1 + F[s-1] ELSE F[s-1]
        \*     IN F[_TEPosition - 1]
    ]

=============================================================================



Parsing and semantic processing can take forever if the trace below is long.
 In this case, it is advised to uncomment the module below to deserialize the
 trace from a generated binary file.

\*
\*---- MODULE CaGcAckFloorZombie_TETrace ----
\*EXTENDS CaGcAckFloorZombie, IOUtils, CaGcAckFloorZombie_TEConstants, TLC
\*
\*trace == IODeserialize("CaGcAckFloorZombie_TTrace_1785647132.bin", TRUE)
\*
\*=============================================================================
\*

---- MODULE CaGcAckFloorZombie_TETrace ----
EXTENDS CaGcAckFloorZombie, CaGcAckFloorZombie_TEConstants, TLC

trace == 
    <<
    ([deadTok |-> (b1 :> {}),deletedEver |-> FALSE,snapFloor |-> (l1 :> 0 @@ l2 :> 0),wAck |-> (w1 :> 0 @@ w2 :> 0),snapIndeg |-> (l1 :> (b1 :> 0) @@ l2 :> (b1 :> 0)),wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),gPhase |-> (l1 :> "idle" @@ l2 :> "idle"),tok |-> (b1 :> 1),round |-> 0,wPending |-> (w1 :> {} @@ w2 :> {}),refs |-> {},snapRetired |-> (l1 :> {} @@ l2 :> {}),snapRound |-> (l1 :> 0 @@ l2 :> 0),retired |-> {},present |-> (b1 :> TRUE)]),
    ([deadTok |-> (b1 :> {}),deletedEver |-> FALSE,snapFloor |-> (l1 :> 0 @@ l2 :> 0),wAck |-> (w1 :> 0 @@ w2 :> 0),snapIndeg |-> (l1 :> (b1 :> 0) @@ l2 :> (b1 :> 0)),wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),gPhase |-> (l1 :> "idle" @@ l2 :> "idle"),tok |-> (b1 :> 1),round |-> 0,wPending |-> (w1 :> {[b |-> b1, t |-> 1]} @@ w2 :> {}),refs |-> {},snapRetired |-> (l1 :> {} @@ l2 :> {}),snapRound |-> (l1 :> 0 @@ l2 :> 0),retired |-> {},present |-> (b1 :> TRUE)]),
    ([deadTok |-> (b1 :> {}),deletedEver |-> FALSE,snapFloor |-> (l1 :> 0 @@ l2 :> 0),wAck |-> (w1 :> 0 @@ w2 :> 0),snapIndeg |-> (l1 :> (b1 :> 0) @@ l2 :> (b1 :> 0)),wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),gPhase |-> (l1 :> "latched" @@ l2 :> "idle"),tok |-> (b1 :> 1),round |-> 0,wPending |-> (w1 :> {[b |-> b1, t |-> 1]} @@ w2 :> {}),refs |-> {},snapRetired |-> (l1 :> {} @@ l2 :> {}),snapRound |-> (l1 :> 0 @@ l2 :> 0),retired |-> {},present |-> (b1 :> TRUE)]),
    ([deadTok |-> (b1 :> {}),deletedEver |-> FALSE,snapFloor |-> (l1 :> 0 @@ l2 :> 0),wAck |-> (w1 :> 0 @@ w2 :> 0),snapIndeg |-> (l1 :> (b1 :> 0) @@ l2 :> (b1 :> 0)),wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),gPhase |-> (l1 :> "idle" @@ l2 :> "idle"),tok |-> (b1 :> 1),round |-> 1,wPending |-> (w1 :> {[b |-> b1, t |-> 1]} @@ w2 :> {}),refs |-> {},snapRetired |-> (l1 :> {} @@ l2 :> {}),snapRound |-> (l1 :> 0 @@ l2 :> 0),retired |-> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]},present |-> (b1 :> TRUE)]),
    ([deadTok |-> (b1 :> {}),deletedEver |-> FALSE,snapFloor |-> (l1 :> 0 @@ l2 :> 0),wAck |-> (w1 :> 0 @@ w2 :> 0),snapIndeg |-> (l1 :> (b1 :> 0) @@ l2 :> (b1 :> 0)),wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),gPhase |-> (l1 :> "latched" @@ l2 :> "idle"),tok |-> (b1 :> 1),round |-> 1,wPending |-> (w1 :> {[b |-> b1, t |-> 1]} @@ w2 :> {}),refs |-> {},snapRetired |-> (l1 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]} @@ l2 :> {}),snapRound |-> (l1 :> 1 @@ l2 :> 0),retired |-> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]},present |-> (b1 :> TRUE)]),
    ([deadTok |-> (b1 :> {}),deletedEver |-> FALSE,snapFloor |-> (l1 :> 0 @@ l2 :> 0),wAck |-> (w1 :> 0 @@ w2 :> 0),snapIndeg |-> (l1 :> (b1 :> 0) @@ l2 :> (b1 :> 0)),wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),gPhase |-> (l1 :> "latched" @@ l2 :> "idle"),tok |-> (b1 :> 1),round |-> 1,wPending |-> (w1 :> {} @@ w2 :> {}),refs |-> {[b |-> b1, t |-> 1, w |-> w1]},snapRetired |-> (l1 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]} @@ l2 :> {}),snapRound |-> (l1 :> 1 @@ l2 :> 0),retired |-> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]},present |-> (b1 :> TRUE)]),
    ([deadTok |-> (b1 :> {}),deletedEver |-> FALSE,snapFloor |-> (l1 :> 0 @@ l2 :> 0),wAck |-> (w1 :> 0 @@ w2 :> 0),snapIndeg |-> (l1 :> (b1 :> 0) @@ l2 :> (b1 :> 0)),wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),gPhase |-> (l1 :> "idle" @@ l2 :> "idle"),tok |-> (b1 :> 1),round |-> 2,wPending |-> (w1 :> {} @@ w2 :> {}),refs |-> {[b |-> b1, t |-> 1, w |-> w1]},snapRetired |-> (l1 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]} @@ l2 :> {}),snapRound |-> (l1 :> 1 @@ l2 :> 0),retired |-> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]},present |-> (b1 :> TRUE)]),
    ([deadTok |-> (b1 :> {}),deletedEver |-> FALSE,snapFloor |-> (l1 :> 0 @@ l2 :> 0),wAck |-> (w1 :> 2 @@ w2 :> 0),snapIndeg |-> (l1 :> (b1 :> 0) @@ l2 :> (b1 :> 0)),wView |-> (w1 :> 2 @@ w2 :> 0),nextTok |-> (b1 :> 2),gPhase |-> (l1 :> "idle" @@ l2 :> "idle"),tok |-> (b1 :> 1),round |-> 2,wPending |-> (w1 :> {} @@ w2 :> {}),refs |-> {[b |-> b1, t |-> 1, w |-> w1]},snapRetired |-> (l1 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]} @@ l2 :> {}),snapRound |-> (l1 :> 1 @@ l2 :> 0),retired |-> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]},present |-> (b1 :> TRUE)]),
    ([deadTok |-> (b1 :> {}),deletedEver |-> FALSE,snapFloor |-> (l1 :> 0 @@ l2 :> 0),wAck |-> (w1 :> 2 @@ w2 :> 2),snapIndeg |-> (l1 :> (b1 :> 0) @@ l2 :> (b1 :> 0)),wView |-> (w1 :> 2 @@ w2 :> 2),nextTok |-> (b1 :> 2),gPhase |-> (l1 :> "idle" @@ l2 :> "idle"),tok |-> (b1 :> 1),round |-> 2,wPending |-> (w1 :> {} @@ w2 :> {}),refs |-> {[b |-> b1, t |-> 1, w |-> w1]},snapRetired |-> (l1 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]} @@ l2 :> {}),snapRound |-> (l1 :> 1 @@ l2 :> 0),retired |-> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]},present |-> (b1 :> TRUE)]),
    ([deadTok |-> (b1 :> {}),deletedEver |-> FALSE,snapFloor |-> (l1 :> 2 @@ l2 :> 0),wAck |-> (w1 :> 2 @@ w2 :> 2),snapIndeg |-> (l1 :> (b1 :> 1) @@ l2 :> (b1 :> 0)),wView |-> (w1 :> 2 @@ w2 :> 2),nextTok |-> (b1 :> 2),gPhase |-> (l1 :> "latched" @@ l2 :> "idle"),tok |-> (b1 :> 1),round |-> 2,wPending |-> (w1 :> {} @@ w2 :> {}),refs |-> {[b |-> b1, t |-> 1, w |-> w1]},snapRetired |-> (l1 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]} @@ l2 :> {}),snapRound |-> (l1 :> 2 @@ l2 :> 0),retired |-> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]},present |-> (b1 :> TRUE)]),
    ([deadTok |-> (b1 :> {}),deletedEver |-> FALSE,snapFloor |-> (l1 :> 2 @@ l2 :> 0),wAck |-> (w1 :> 2 @@ w2 :> 2),snapIndeg |-> (l1 :> (b1 :> 1) @@ l2 :> (b1 :> 0)),wView |-> (w1 :> 2 @@ w2 :> 2),nextTok |-> (b1 :> 2),gPhase |-> (l1 :> "latched" @@ l2 :> "idle"),tok |-> (b1 :> 1),round |-> 2,wPending |-> (w1 :> {} @@ w2 :> {}),refs |-> {},snapRetired |-> (l1 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]} @@ l2 :> {}),snapRound |-> (l1 :> 2 @@ l2 :> 0),retired |-> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]},present |-> (b1 :> TRUE)]),
    ([deadTok |-> (b1 :> {}),deletedEver |-> FALSE,snapFloor |-> (l1 :> 2 @@ l2 :> 2),wAck |-> (w1 :> 2 @@ w2 :> 2),snapIndeg |-> (l1 :> (b1 :> 1) @@ l2 :> (b1 :> 0)),wView |-> (w1 :> 2 @@ w2 :> 2),nextTok |-> (b1 :> 2),gPhase |-> (l1 :> "latched" @@ l2 :> "latched"),tok |-> (b1 :> 1),round |-> 2,wPending |-> (w1 :> {} @@ w2 :> {}),refs |-> {},snapRetired |-> (l1 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]} @@ l2 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]}),snapRound |-> (l1 :> 2 @@ l2 :> 2),retired |-> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]},present |-> (b1 :> TRUE)]),
    ([deadTok |-> (b1 :> {}),deletedEver |-> FALSE,snapFloor |-> (l1 :> 2 @@ l2 :> 2),wAck |-> (w1 :> 2 @@ w2 :> 2),snapIndeg |-> (l1 :> (b1 :> 1) @@ l2 :> (b1 :> 0)),wView |-> (w1 :> 2 @@ w2 :> 2),nextTok |-> (b1 :> 2),gPhase |-> (l1 :> "idle" @@ l2 :> "latched"),tok |-> (b1 :> 1),round |-> 3,wPending |-> (w1 :> {} @@ w2 :> {}),refs |-> {},snapRetired |-> (l1 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]} @@ l2 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]}),snapRound |-> (l1 :> 2 @@ l2 :> 2),retired |-> {},present |-> (b1 :> TRUE)]),
    ([deadTok |-> (b1 :> {}),deletedEver |-> FALSE,snapFloor |-> (l1 :> 2 @@ l2 :> 2),wAck |-> (w1 :> 2 @@ w2 :> 2),snapIndeg |-> (l1 :> (b1 :> 1) @@ l2 :> (b1 :> 0)),wView |-> (w1 :> 2 @@ w2 :> 2),nextTok |-> (b1 :> 2),gPhase |-> (l1 :> "idle" @@ l2 :> "latched"),tok |-> (b1 :> 1),round |-> 3,wPending |-> (w1 :> {[b |-> b1, t |-> 1]} @@ w2 :> {}),refs |-> {},snapRetired |-> (l1 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]} @@ l2 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]}),snapRound |-> (l1 :> 2 @@ l2 :> 2),retired |-> {},present |-> (b1 :> TRUE)]),
    ([deadTok |-> (b1 :> {}),deletedEver |-> FALSE,snapFloor |-> (l1 :> 2 @@ l2 :> 2),wAck |-> (w1 :> 2 @@ w2 :> 2),snapIndeg |-> (l1 :> (b1 :> 1) @@ l2 :> (b1 :> 0)),wView |-> (w1 :> 2 @@ w2 :> 2),nextTok |-> (b1 :> 2),gPhase |-> (l1 :> "idle" @@ l2 :> "latched"),tok |-> (b1 :> 1),round |-> 3,wPending |-> (w1 :> {} @@ w2 :> {}),refs |-> {[b |-> b1, t |-> 1, w |-> w1]},snapRetired |-> (l1 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]} @@ l2 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]}),snapRound |-> (l1 :> 2 @@ l2 :> 2),retired |-> {},present |-> (b1 :> TRUE)]),
    ([deadTok |-> (b1 :> {1}),deletedEver |-> TRUE,snapFloor |-> (l1 :> 2 @@ l2 :> 2),wAck |-> (w1 :> 2 @@ w2 :> 2),snapIndeg |-> (l1 :> (b1 :> 1) @@ l2 :> (b1 :> 0)),wView |-> (w1 :> 2 @@ w2 :> 2),nextTok |-> (b1 :> 2),gPhase |-> (l1 :> "idle" @@ l2 :> "idle"),tok |-> (b1 :> 1),round |-> 3,wPending |-> (w1 :> {} @@ w2 :> {}),refs |-> {[b |-> b1, t |-> 1, w |-> w1]},snapRetired |-> (l1 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]} @@ l2 :> {[b |-> b1, t |-> 1, r |-> 1, pending |-> FALSE]}),snapRound |-> (l1 :> 2 @@ l2 :> 2),retired |-> {},present |-> (b1 :> FALSE)])
    >>
----


=============================================================================

---- MODULE CaGcAckFloorZombie_TEConstants ----
EXTENDS CaGcAckFloorZombie

CONSTANTS w1, w2, l1, l2, b1

=============================================================================

---- CONFIG CaGcAckFloorZombie_TTrace_1785647132 ----
CONSTANTS
    Writers = { w1 , w2 }
    Leaders = { l1 , l2 }
    Blobs = { b1 }
    MaxRound = 5
    MaxTok = 4
    SabotageEagerZombieDelete = TRUE
    l2 = l2
    l1 = l1
    w1 = w1
    b1 = b1
    w2 = w2

INVARIANT
    _inv

CHECK_DEADLOCK
    \* CHECK_DEADLOCK off because of PROPERTY or INVARIANT above.
    FALSE

INIT
    _init

NEXT
    _next

CONSTANT
    _TETrace <- _trace

ALIAS
    _expression
=============================================================================
\* Generated on Sun Aug 02 07:05:34 CEST 2026