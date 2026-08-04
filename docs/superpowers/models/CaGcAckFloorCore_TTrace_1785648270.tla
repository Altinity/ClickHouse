---- MODULE CaGcAckFloorCore_TTrace_1785648270 ----
EXTENDS Sequences, TLCExt, Toolbox, Naturals, TLC, CaGcAckFloorCore, CaGcAckFloorCore_TEConstants

_expression ==
    LET CaGcAckFloorCore_TEExpression == INSTANCE CaGcAckFloorCore_TEExpression
    IN CaGcAckFloorCore_TEExpression!expression
----

_trace ==
    LET CaGcAckFloorCore_TETrace == INSTANCE CaGcAckFloorCore_TETrace
    IN CaGcAckFloorCore_TETrace!trace
----

_inv ==
    ~(
        TLCGet("level") = Len(_TETrace)
        /\
        deadTok = ((b1 :> {1}))
        /\
        rebuiltEver = (FALSE)
        /\
        deletedEver = (TRUE)
        /\
        wAck = ((w1 :> 0 @@ w2 :> 0))
        /\
        recreatedEver = (FALSE)
        /\
        clampedEver = (FALSE)
        /\
        sparedEver = (FALSE)
        /\
        wView = ((w1 :> 0 @@ w2 :> 0))
        /\
        nextTok = ((b1 :> 2))
        /\
        tok = ((b1 :> 1))
        /\
        wStatus = ((w1 :> "live" @@ w2 :> "unmounted"))
        /\
        minAckL = (5)
        /\
        round = (2)
        /\
        wPending = ((w1 :> {} @@ w2 :> {}))
        /\
        copyForwardEver = (FALSE)
        /\
        gcPhase = ("idle")
        /\
        retired = ({})
        /\
        present = ((b1 :> FALSE))
        /\
        lostRefs = ({})
        /\
        clampedL = (FALSE)
        /\
        folded = ({})
        /\
        landed = ({[b |-> b1, w |-> w1, t |-> 1]})
    )
----

_init ==
    /\ landed = _TETrace[1].landed
    /\ copyForwardEver = _TETrace[1].copyForwardEver
    /\ deletedEver = _TETrace[1].deletedEver
    /\ nextTok = _TETrace[1].nextTok
    /\ sparedEver = _TETrace[1].sparedEver
    /\ present = _TETrace[1].present
    /\ tok = _TETrace[1].tok
    /\ lostRefs = _TETrace[1].lostRefs
    /\ clampedEver = _TETrace[1].clampedEver
    /\ recreatedEver = _TETrace[1].recreatedEver
    /\ minAckL = _TETrace[1].minAckL
    /\ wAck = _TETrace[1].wAck
    /\ gcPhase = _TETrace[1].gcPhase
    /\ wStatus = _TETrace[1].wStatus
    /\ round = _TETrace[1].round
    /\ clampedL = _TETrace[1].clampedL
    /\ rebuiltEver = _TETrace[1].rebuiltEver
    /\ wView = _TETrace[1].wView
    /\ deadTok = _TETrace[1].deadTok
    /\ folded = _TETrace[1].folded
    /\ retired = _TETrace[1].retired
    /\ wPending = _TETrace[1].wPending
----

_next ==
    /\ \E i,j \in DOMAIN _TETrace:
        /\ \/ /\ j = i + 1
              /\ i = TLCGet("level")
        /\ landed  = _TETrace[i].landed
        /\ landed' = _TETrace[j].landed
        /\ copyForwardEver  = _TETrace[i].copyForwardEver
        /\ copyForwardEver' = _TETrace[j].copyForwardEver
        /\ deletedEver  = _TETrace[i].deletedEver
        /\ deletedEver' = _TETrace[j].deletedEver
        /\ nextTok  = _TETrace[i].nextTok
        /\ nextTok' = _TETrace[j].nextTok
        /\ sparedEver  = _TETrace[i].sparedEver
        /\ sparedEver' = _TETrace[j].sparedEver
        /\ present  = _TETrace[i].present
        /\ present' = _TETrace[j].present
        /\ tok  = _TETrace[i].tok
        /\ tok' = _TETrace[j].tok
        /\ lostRefs  = _TETrace[i].lostRefs
        /\ lostRefs' = _TETrace[j].lostRefs
        /\ clampedEver  = _TETrace[i].clampedEver
        /\ clampedEver' = _TETrace[j].clampedEver
        /\ recreatedEver  = _TETrace[i].recreatedEver
        /\ recreatedEver' = _TETrace[j].recreatedEver
        /\ minAckL  = _TETrace[i].minAckL
        /\ minAckL' = _TETrace[j].minAckL
        /\ wAck  = _TETrace[i].wAck
        /\ wAck' = _TETrace[j].wAck
        /\ gcPhase  = _TETrace[i].gcPhase
        /\ gcPhase' = _TETrace[j].gcPhase
        /\ wStatus  = _TETrace[i].wStatus
        /\ wStatus' = _TETrace[j].wStatus
        /\ round  = _TETrace[i].round
        /\ round' = _TETrace[j].round
        /\ clampedL  = _TETrace[i].clampedL
        /\ clampedL' = _TETrace[j].clampedL
        /\ rebuiltEver  = _TETrace[i].rebuiltEver
        /\ rebuiltEver' = _TETrace[j].rebuiltEver
        /\ wView  = _TETrace[i].wView
        /\ wView' = _TETrace[j].wView
        /\ deadTok  = _TETrace[i].deadTok
        /\ deadTok' = _TETrace[j].deadTok
        /\ folded  = _TETrace[i].folded
        /\ folded' = _TETrace[j].folded
        /\ retired  = _TETrace[i].retired
        /\ retired' = _TETrace[j].retired
        /\ wPending  = _TETrace[i].wPending
        /\ wPending' = _TETrace[j].wPending

\* Uncomment the ASSUME below to write the states of the error trace
\* to the given file in Json format. Note that you can pass any tuple
\* to `JsonSerialize`. For example, a sub-sequence of _TETrace.
    \* ASSUME
    \*     LET J == INSTANCE Json
    \*         IN J!JsonSerialize("CaGcAckFloorCore_TTrace_1785648270.json", _TETrace)

=============================================================================

 Note that you can extract this module `CaGcAckFloorCore_TEExpression`
  to a dedicated file to reuse `expression` (the module in the 
  dedicated `CaGcAckFloorCore_TEExpression.tla` file takes precedence 
  over the module `CaGcAckFloorCore_TEExpression` below).

---- MODULE CaGcAckFloorCore_TEExpression ----
EXTENDS Sequences, TLCExt, Toolbox, Naturals, TLC, CaGcAckFloorCore, CaGcAckFloorCore_TEConstants

expression == 
    [
        \* To hide variables of the `CaGcAckFloorCore` spec from the error trace,
        \* remove the variables below.  The trace will be written in the order
        \* of the fields of this record.
        landed |-> landed
        ,copyForwardEver |-> copyForwardEver
        ,deletedEver |-> deletedEver
        ,nextTok |-> nextTok
        ,sparedEver |-> sparedEver
        ,present |-> present
        ,tok |-> tok
        ,lostRefs |-> lostRefs
        ,clampedEver |-> clampedEver
        ,recreatedEver |-> recreatedEver
        ,minAckL |-> minAckL
        ,wAck |-> wAck
        ,gcPhase |-> gcPhase
        ,wStatus |-> wStatus
        ,round |-> round
        ,clampedL |-> clampedL
        ,rebuiltEver |-> rebuiltEver
        ,wView |-> wView
        ,deadTok |-> deadTok
        ,folded |-> folded
        ,retired |-> retired
        ,wPending |-> wPending
        
        \* Put additional constant-, state-, and action-level expressions here:
        \* ,_stateNumber |-> _TEPosition
        \* ,_landedUnchanged |-> landed = landed'
        
        \* Format the `landed` variable as Json value.
        \* ,_landedJson |->
        \*     LET J == INSTANCE Json
        \*     IN J!ToJson(landed)
        
        \* Lastly, you may build expressions over arbitrary sets of states by
        \* leveraging the _TETrace operator.  For example, this is how to
        \* count the number of times a spec variable changed up to the current
        \* state in the trace.
        \* ,_landedModCount |->
        \*     LET F[s \in DOMAIN _TETrace] ==
        \*         IF s = 1 THEN 0
        \*         ELSE IF _TETrace[s].landed # _TETrace[s-1].landed
        \*             THEN 1 + F[s-1] ELSE F[s-1]
        \*     IN F[_TEPosition - 1]
    ]

=============================================================================



Parsing and semantic processing can take forever if the trace below is long.
 In this case, it is advised to uncomment the module below to deserialize the
 trace from a generated binary file.

\*
\*---- MODULE CaGcAckFloorCore_TETrace ----
\*EXTENDS IOUtils, TLC, CaGcAckFloorCore, CaGcAckFloorCore_TEConstants
\*
\*trace == IODeserialize("CaGcAckFloorCore_TTrace_1785648270.bin", TRUE)
\*
\*=============================================================================
\*

---- MODULE CaGcAckFloorCore_TETrace ----
EXTENDS TLC, CaGcAckFloorCore, CaGcAckFloorCore_TEConstants

trace == 
    <<
    ([deadTok |-> (b1 :> {}),rebuiltEver |-> FALSE,deletedEver |-> FALSE,wAck |-> (w1 :> 0 @@ w2 :> 0),recreatedEver |-> FALSE,clampedEver |-> FALSE,sparedEver |-> FALSE,wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),tok |-> (b1 :> 1),wStatus |-> (w1 :> "unmounted" @@ w2 :> "unmounted"),minAckL |-> 0,round |-> 0,wPending |-> (w1 :> {} @@ w2 :> {}),copyForwardEver |-> FALSE,gcPhase |-> "idle",retired |-> {},present |-> (b1 :> TRUE),lostRefs |-> {},clampedL |-> FALSE,folded |-> {},landed |-> {}]),
    ([deadTok |-> (b1 :> {}),rebuiltEver |-> FALSE,deletedEver |-> FALSE,wAck |-> (w1 :> 0 @@ w2 :> 0),recreatedEver |-> FALSE,clampedEver |-> FALSE,sparedEver |-> FALSE,wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),tok |-> (b1 :> 1),wStatus |-> (w1 :> "live" @@ w2 :> "unmounted"),minAckL |-> 0,round |-> 0,wPending |-> (w1 :> {} @@ w2 :> {}),copyForwardEver |-> FALSE,gcPhase |-> "idle",retired |-> {},present |-> (b1 :> TRUE),lostRefs |-> {},clampedL |-> FALSE,folded |-> {},landed |-> {}]),
    ([deadTok |-> (b1 :> {}),rebuiltEver |-> FALSE,deletedEver |-> FALSE,wAck |-> (w1 :> 0 @@ w2 :> 0),recreatedEver |-> FALSE,clampedEver |-> FALSE,sparedEver |-> FALSE,wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),tok |-> (b1 :> 1),wStatus |-> (w1 :> "expired" @@ w2 :> "unmounted"),minAckL |-> 0,round |-> 0,wPending |-> (w1 :> {} @@ w2 :> {}),copyForwardEver |-> FALSE,gcPhase |-> "idle",retired |-> {},present |-> (b1 :> TRUE),lostRefs |-> {},clampedL |-> FALSE,folded |-> {},landed |-> {}]),
    ([deadTok |-> (b1 :> {}),rebuiltEver |-> FALSE,deletedEver |-> FALSE,wAck |-> (w1 :> 0 @@ w2 :> 0),recreatedEver |-> FALSE,clampedEver |-> FALSE,sparedEver |-> FALSE,wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),tok |-> (b1 :> 1),wStatus |-> (w1 :> "expired" @@ w2 :> "unmounted"),minAckL |-> 5,round |-> 0,wPending |-> (w1 :> {} @@ w2 :> {}),copyForwardEver |-> FALSE,gcPhase |-> "running",retired |-> {},present |-> (b1 :> TRUE),lostRefs |-> {},clampedL |-> FALSE,folded |-> {},landed |-> {}]),
    ([deadTok |-> (b1 :> {}),rebuiltEver |-> FALSE,deletedEver |-> FALSE,wAck |-> (w1 :> 0 @@ w2 :> 0),recreatedEver |-> FALSE,clampedEver |-> FALSE,sparedEver |-> FALSE,wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),tok |-> (b1 :> 1),wStatus |-> (w1 :> "expired" @@ w2 :> "unmounted"),minAckL |-> 5,round |-> 0,wPending |-> (w1 :> {} @@ w2 :> {}),copyForwardEver |-> FALSE,gcPhase |-> "folded",retired |-> {},present |-> (b1 :> TRUE),lostRefs |-> {},clampedL |-> FALSE,folded |-> {},landed |-> {}]),
    ([deadTok |-> (b1 :> {}),rebuiltEver |-> FALSE,deletedEver |-> FALSE,wAck |-> (w1 :> 0 @@ w2 :> 0),recreatedEver |-> FALSE,clampedEver |-> FALSE,sparedEver |-> FALSE,wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),tok |-> (b1 :> 1),wStatus |-> (w1 :> "expired" @@ w2 :> "unmounted"),minAckL |-> 5,round |-> 1,wPending |-> (w1 :> {} @@ w2 :> {}),copyForwardEver |-> FALSE,gcPhase |-> "idle",retired |-> {[b |-> b1, t |-> 1, r |-> 1]},present |-> (b1 :> TRUE),lostRefs |-> {},clampedL |-> FALSE,folded |-> {},landed |-> {}]),
    ([deadTok |-> (b1 :> {}),rebuiltEver |-> FALSE,deletedEver |-> FALSE,wAck |-> (w1 :> 0 @@ w2 :> 0),recreatedEver |-> FALSE,clampedEver |-> FALSE,sparedEver |-> FALSE,wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),tok |-> (b1 :> 1),wStatus |-> (w1 :> "expired" @@ w2 :> "unmounted"),minAckL |-> 5,round |-> 1,wPending |-> (w1 :> {} @@ w2 :> {}),copyForwardEver |-> FALSE,gcPhase |-> "running",retired |-> {[b |-> b1, t |-> 1, r |-> 1]},present |-> (b1 :> TRUE),lostRefs |-> {},clampedL |-> FALSE,folded |-> {},landed |-> {}]),
    ([deadTok |-> (b1 :> {}),rebuiltEver |-> FALSE,deletedEver |-> FALSE,wAck |-> (w1 :> 0 @@ w2 :> 0),recreatedEver |-> FALSE,clampedEver |-> FALSE,sparedEver |-> FALSE,wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),tok |-> (b1 :> 1),wStatus |-> (w1 :> "live" @@ w2 :> "unmounted"),minAckL |-> 5,round |-> 1,wPending |-> (w1 :> {} @@ w2 :> {}),copyForwardEver |-> FALSE,gcPhase |-> "running",retired |-> {[b |-> b1, t |-> 1, r |-> 1]},present |-> (b1 :> TRUE),lostRefs |-> {},clampedL |-> FALSE,folded |-> {},landed |-> {}]),
    ([deadTok |-> (b1 :> {}),rebuiltEver |-> FALSE,deletedEver |-> FALSE,wAck |-> (w1 :> 0 @@ w2 :> 0),recreatedEver |-> FALSE,clampedEver |-> FALSE,sparedEver |-> FALSE,wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),tok |-> (b1 :> 1),wStatus |-> (w1 :> "live" @@ w2 :> "unmounted"),minAckL |-> 5,round |-> 1,wPending |-> (w1 :> {[b |-> b1, t |-> 1]} @@ w2 :> {}),copyForwardEver |-> FALSE,gcPhase |-> "running",retired |-> {[b |-> b1, t |-> 1, r |-> 1]},present |-> (b1 :> TRUE),lostRefs |-> {},clampedL |-> FALSE,folded |-> {},landed |-> {}]),
    ([deadTok |-> (b1 :> {}),rebuiltEver |-> FALSE,deletedEver |-> FALSE,wAck |-> (w1 :> 0 @@ w2 :> 0),recreatedEver |-> FALSE,clampedEver |-> FALSE,sparedEver |-> FALSE,wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),tok |-> (b1 :> 1),wStatus |-> (w1 :> "live" @@ w2 :> "unmounted"),minAckL |-> 5,round |-> 1,wPending |-> (w1 :> {[b |-> b1, t |-> 1]} @@ w2 :> {}),copyForwardEver |-> FALSE,gcPhase |-> "folded",retired |-> {[b |-> b1, t |-> 1, r |-> 1]},present |-> (b1 :> TRUE),lostRefs |-> {},clampedL |-> FALSE,folded |-> {},landed |-> {}]),
    ([deadTok |-> (b1 :> {}),rebuiltEver |-> FALSE,deletedEver |-> FALSE,wAck |-> (w1 :> 0 @@ w2 :> 0),recreatedEver |-> FALSE,clampedEver |-> FALSE,sparedEver |-> FALSE,wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),tok |-> (b1 :> 1),wStatus |-> (w1 :> "live" @@ w2 :> "unmounted"),minAckL |-> 5,round |-> 1,wPending |-> (w1 :> {} @@ w2 :> {}),copyForwardEver |-> FALSE,gcPhase |-> "folded",retired |-> {[b |-> b1, t |-> 1, r |-> 1]},present |-> (b1 :> TRUE),lostRefs |-> {},clampedL |-> FALSE,folded |-> {},landed |-> {[b |-> b1, w |-> w1, t |-> 1]}]),
    ([deadTok |-> (b1 :> {1}),rebuiltEver |-> FALSE,deletedEver |-> TRUE,wAck |-> (w1 :> 0 @@ w2 :> 0),recreatedEver |-> FALSE,clampedEver |-> FALSE,sparedEver |-> FALSE,wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),tok |-> (b1 :> 1),wStatus |-> (w1 :> "live" @@ w2 :> "unmounted"),minAckL |-> 5,round |-> 2,wPending |-> (w1 :> {} @@ w2 :> {}),copyForwardEver |-> FALSE,gcPhase |-> "idle",retired |-> {},present |-> (b1 :> FALSE),lostRefs |-> {},clampedL |-> FALSE,folded |-> {},landed |-> {[b |-> b1, w |-> w1, t |-> 1]}])
    >>
----


=============================================================================

---- MODULE CaGcAckFloorCore_TEConstants ----
EXTENDS CaGcAckFloorCore

CONSTANTS w1, w2, b1

=============================================================================

---- CONFIG CaGcAckFloorCore_TTrace_1785648270 ----
CONSTANTS
    Writers = { w1 , w2 }
    Blobs = { b1 }
    MaxRound = 4
    MaxTok = 4
    SabotageIgnoreAckFloor = FALSE
    SabotageAckWithoutRead = FALSE
    SabotageAckBeforeDrain = FALSE
    SabotageSleeperRearm = TRUE
    SabotageSkipChangedShard = FALSE
    SabotageAdoptRetiredToken = FALSE
    SabotageOpenWriteBeforeLoad = FALSE
    SabotageRebuildDropEdge = FALSE
    SabotageRebuildKeepRetired = FALSE
    SabotageRebuildLowRound = FALSE
    SabotageClampNoSuppress = FALSE
    w2 = w2
    b1 = b1
    w1 = w1

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
\* Generated on Sun Aug 02 07:24:30 CEST 2026