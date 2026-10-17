---- MODULE CaGcRoundDeferCore_TTrace_1785722424 ----
EXTENDS Sequences, TLCExt, CaGcRoundDeferCore, CaGcRoundDeferCore_TEConstants, Toolbox, Naturals, TLC

_expression ==
    LET CaGcRoundDeferCore_TEExpression == INSTANCE CaGcRoundDeferCore_TEExpression
    IN CaGcRoundDeferCore_TEExpression!expression
----

_trace ==
    LET CaGcRoundDeferCore_TETrace == INSTANCE CaGcRoundDeferCore_TETrace
    IN CaGcRoundDeferCore_TETrace!trace
----

_inv ==
    ~(
        TLCGet("level") = Len(_TETrace)
        /\
        deferredWithUnfoldedEver = (FALSE)
        /\
        round = (4)
        /\
        minAckL = (4)
        /\
        gcPhase = ("idle")
        /\
        deferCount = (0)
        /\
        retired = ({})
        /\
        present = ((b1 :> FALSE))
        /\
        unfolded = ({[b |-> b1, w |-> w1, op |-> "add"]})
        /\
        folded = ({})
        /\
        minAck = (4)
        /\
        deletedThisStep = ({b1})
    )
----

_init ==
    /\ retired = _TETrace[1].retired
    /\ unfolded = _TETrace[1].unfolded
    /\ deferCount = _TETrace[1].deferCount
    /\ folded = _TETrace[1].folded
    /\ gcPhase = _TETrace[1].gcPhase
    /\ minAck = _TETrace[1].minAck
    /\ round = _TETrace[1].round
    /\ present = _TETrace[1].present
    /\ deferredWithUnfoldedEver = _TETrace[1].deferredWithUnfoldedEver
    /\ deletedThisStep = _TETrace[1].deletedThisStep
    /\ minAckL = _TETrace[1].minAckL
----

_next ==
    /\ \E i,j \in DOMAIN _TETrace:
        /\ \/ /\ j = i + 1
              /\ i = TLCGet("level")
        /\ retired  = _TETrace[i].retired
        /\ retired' = _TETrace[j].retired
        /\ unfolded  = _TETrace[i].unfolded
        /\ unfolded' = _TETrace[j].unfolded
        /\ deferCount  = _TETrace[i].deferCount
        /\ deferCount' = _TETrace[j].deferCount
        /\ folded  = _TETrace[i].folded
        /\ folded' = _TETrace[j].folded
        /\ gcPhase  = _TETrace[i].gcPhase
        /\ gcPhase' = _TETrace[j].gcPhase
        /\ minAck  = _TETrace[i].minAck
        /\ minAck' = _TETrace[j].minAck
        /\ round  = _TETrace[i].round
        /\ round' = _TETrace[j].round
        /\ present  = _TETrace[i].present
        /\ present' = _TETrace[j].present
        /\ deferredWithUnfoldedEver  = _TETrace[i].deferredWithUnfoldedEver
        /\ deferredWithUnfoldedEver' = _TETrace[j].deferredWithUnfoldedEver
        /\ deletedThisStep  = _TETrace[i].deletedThisStep
        /\ deletedThisStep' = _TETrace[j].deletedThisStep
        /\ minAckL  = _TETrace[i].minAckL
        /\ minAckL' = _TETrace[j].minAckL

\* Uncomment the ASSUME below to write the states of the error trace
\* to the given file in Json format. Note that you can pass any tuple
\* to `JsonSerialize`. For example, a sub-sequence of _TETrace.
    \* ASSUME
    \*     LET J == INSTANCE Json
    \*         IN J!JsonSerialize("CaGcRoundDeferCore_TTrace_1785722424.json", _TETrace)

=============================================================================

 Note that you can extract this module `CaGcRoundDeferCore_TEExpression`
  to a dedicated file to reuse `expression` (the module in the 
  dedicated `CaGcRoundDeferCore_TEExpression.tla` file takes precedence 
  over the module `CaGcRoundDeferCore_TEExpression` below).

---- MODULE CaGcRoundDeferCore_TEExpression ----
EXTENDS Sequences, TLCExt, CaGcRoundDeferCore, CaGcRoundDeferCore_TEConstants, Toolbox, Naturals, TLC

expression == 
    [
        \* To hide variables of the `CaGcRoundDeferCore` spec from the error trace,
        \* remove the variables below.  The trace will be written in the order
        \* of the fields of this record.
        retired |-> retired
        ,unfolded |-> unfolded
        ,deferCount |-> deferCount
        ,folded |-> folded
        ,gcPhase |-> gcPhase
        ,minAck |-> minAck
        ,round |-> round
        ,present |-> present
        ,deferredWithUnfoldedEver |-> deferredWithUnfoldedEver
        ,deletedThisStep |-> deletedThisStep
        ,minAckL |-> minAckL
        
        \* Put additional constant-, state-, and action-level expressions here:
        \* ,_stateNumber |-> _TEPosition
        \* ,_retiredUnchanged |-> retired = retired'
        
        \* Format the `retired` variable as Json value.
        \* ,_retiredJson |->
        \*     LET J == INSTANCE Json
        \*     IN J!ToJson(retired)
        
        \* Lastly, you may build expressions over arbitrary sets of states by
        \* leveraging the _TETrace operator.  For example, this is how to
        \* count the number of times a spec variable changed up to the current
        \* state in the trace.
        \* ,_retiredModCount |->
        \*     LET F[s \in DOMAIN _TETrace] ==
        \*         IF s = 1 THEN 0
        \*         ELSE IF _TETrace[s].retired # _TETrace[s-1].retired
        \*             THEN 1 + F[s-1] ELSE F[s-1]
        \*     IN F[_TEPosition - 1]
    ]

=============================================================================



Parsing and semantic processing can take forever if the trace below is long.
 In this case, it is advised to uncomment the module below to deserialize the
 trace from a generated binary file.

\*
\*---- MODULE CaGcRoundDeferCore_TETrace ----
\*EXTENDS IOUtils, CaGcRoundDeferCore, CaGcRoundDeferCore_TEConstants, TLC
\*
\*trace == IODeserialize("CaGcRoundDeferCore_TTrace_1785722424.bin", TRUE)
\*
\*=============================================================================
\*

---- MODULE CaGcRoundDeferCore_TETrace ----
EXTENDS CaGcRoundDeferCore, CaGcRoundDeferCore_TEConstants, TLC

trace == 
    <<
    ([deferredWithUnfoldedEver |-> FALSE,round |-> 0,minAckL |-> 0,gcPhase |-> "idle",deferCount |-> 0,retired |-> {},present |-> (b1 :> TRUE),unfolded |-> {},folded |-> {},minAck |-> 0,deletedThisStep |-> {}]),
    ([deferredWithUnfoldedEver |-> FALSE,round |-> 1,minAckL |-> 0,gcPhase |-> "idle",deferCount |-> 1,retired |-> {},present |-> (b1 :> TRUE),unfolded |-> {},folded |-> {},minAck |-> 0,deletedThisStep |-> {}]),
    ([deferredWithUnfoldedEver |-> FALSE,round |-> 1,minAckL |-> 0,gcPhase |-> "idle",deferCount |-> 1,retired |-> {},present |-> (b1 :> TRUE),unfolded |-> {},folded |-> {},minAck |-> 1,deletedThisStep |-> {}]),
    ([deferredWithUnfoldedEver |-> FALSE,round |-> 2,minAckL |-> 0,gcPhase |-> "idle",deferCount |-> 2,retired |-> {},present |-> (b1 :> TRUE),unfolded |-> {},folded |-> {},minAck |-> 1,deletedThisStep |-> {}]),
    ([deferredWithUnfoldedEver |-> FALSE,round |-> 2,minAckL |-> 0,gcPhase |-> "idle",deferCount |-> 2,retired |-> {},present |-> (b1 :> TRUE),unfolded |-> {},folded |-> {},minAck |-> 2,deletedThisStep |-> {}]),
    ([deferredWithUnfoldedEver |-> FALSE,round |-> 3,minAckL |-> 0,gcPhase |-> "idle",deferCount |-> 3,retired |-> {},present |-> (b1 :> TRUE),unfolded |-> {},folded |-> {},minAck |-> 2,deletedThisStep |-> {}]),
    ([deferredWithUnfoldedEver |-> FALSE,round |-> 3,minAckL |-> 0,gcPhase |-> "idle",deferCount |-> 3,retired |-> {},present |-> (b1 :> TRUE),unfolded |-> {},folded |-> {},minAck |-> 3,deletedThisStep |-> {}]),
    ([deferredWithUnfoldedEver |-> FALSE,round |-> 3,minAckL |-> 3,gcPhase |-> "running",deferCount |-> 3,retired |-> {},present |-> (b1 :> TRUE),unfolded |-> {},folded |-> {},minAck |-> 3,deletedThisStep |-> {}]),
    ([deferredWithUnfoldedEver |-> FALSE,round |-> 3,minAckL |-> 3,gcPhase |-> "folded",deferCount |-> 0,retired |-> {},present |-> (b1 :> TRUE),unfolded |-> {},folded |-> {},minAck |-> 3,deletedThisStep |-> {}]),
    ([deferredWithUnfoldedEver |-> FALSE,round |-> 4,minAckL |-> 3,gcPhase |-> "idle",deferCount |-> 0,retired |-> {[b |-> b1, condemn_round |-> 3]},present |-> (b1 :> TRUE),unfolded |-> {},folded |-> {},minAck |-> 3,deletedThisStep |-> {}]),
    ([deferredWithUnfoldedEver |-> FALSE,round |-> 4,minAckL |-> 3,gcPhase |-> "idle",deferCount |-> 0,retired |-> {[b |-> b1, condemn_round |-> 3]},present |-> (b1 :> TRUE),unfolded |-> {},folded |-> {},minAck |-> 4,deletedThisStep |-> {}]),
    ([deferredWithUnfoldedEver |-> FALSE,round |-> 4,minAckL |-> 4,gcPhase |-> "running",deferCount |-> 0,retired |-> {[b |-> b1, condemn_round |-> 3]},present |-> (b1 :> TRUE),unfolded |-> {},folded |-> {},minAck |-> 4,deletedThisStep |-> {}]),
    ([deferredWithUnfoldedEver |-> FALSE,round |-> 4,minAckL |-> 4,gcPhase |-> "folded",deferCount |-> 0,retired |-> {[b |-> b1, condemn_round |-> 3]},present |-> (b1 :> TRUE),unfolded |-> {},folded |-> {},minAck |-> 4,deletedThisStep |-> {}]),
    ([deferredWithUnfoldedEver |-> FALSE,round |-> 4,minAckL |-> 4,gcPhase |-> "folded",deferCount |-> 0,retired |-> {[b |-> b1, condemn_round |-> 3]},present |-> (b1 :> TRUE),unfolded |-> {[b |-> b1, w |-> w1, op |-> "add"]},folded |-> {},minAck |-> 4,deletedThisStep |-> {}]),
    ([deferredWithUnfoldedEver |-> FALSE,round |-> 4,minAckL |-> 4,gcPhase |-> "idle",deferCount |-> 0,retired |-> {},present |-> (b1 :> FALSE),unfolded |-> {[b |-> b1, w |-> w1, op |-> "add"]},folded |-> {},minAck |-> 4,deletedThisStep |-> {b1}])
    >>
----


=============================================================================

---- MODULE CaGcRoundDeferCore_TEConstants ----
EXTENDS CaGcRoundDeferCore

CONSTANTS w1, w2, b1

=============================================================================

---- CONFIG CaGcRoundDeferCore_TTrace_1785722424 ----
CONSTANTS
    Writers = { w1 , w2 }
    Blobs = { b1 }
    MaxRound = 4
    MaxDefer = 3
    SabotageGraduateOnStale = TRUE
    SabotageUnboundedDefer = FALSE
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
\* Generated on Mon Aug 03 04:00:24 CEST 2026