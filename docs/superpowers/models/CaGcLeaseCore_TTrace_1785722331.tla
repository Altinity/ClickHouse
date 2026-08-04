---- MODULE CaGcLeaseCore_TTrace_1785722331 ----
EXTENDS CaGcLeaseCore, Sequences, TLCExt, Toolbox, Naturals, TLC, CaGcLeaseCore_TEConstants

_expression ==
    LET CaGcLeaseCore_TEExpression == INSTANCE CaGcLeaseCore_TEExpression
    IN CaGcLeaseCore_TEExpression!expression
----

_trace ==
    LET CaGcLeaseCore_TETrace == INSTANCE CaGcLeaseCore_TETrace
    IN CaGcLeaseCore_TETrace!trace
----

_inv ==
    ~(
        TLCGet("level") = Len(_TETrace)
        /\
        obs = ((L1 :> [seen |-> FALSE] @@ L2 :> [seen |-> FALSE]))
        /\
        falseSteal = (TRUE)
        /\
        inRound = ((L1 :> TRUE @@ L2 :> FALSE))
        /\
        committed = ({})
        /\
        hbSeq = (0)
        /\
        stSeq = (2)
        /\
        alive = ((L1 :> TRUE @@ L2 :> TRUE))
        /\
        roundFence = ((L1 :> 0 @@ L2 :> 0))
        /\
        stFence = (1)
        /\
        clock = (1)
        /\
        hbOwner = (NoneOwner)
        /\
        stOwner = (L2)
    )
----

_init ==
    /\ falseSteal = _TETrace[1].falseSteal
    /\ stFence = _TETrace[1].stFence
    /\ inRound = _TETrace[1].inRound
    /\ stSeq = _TETrace[1].stSeq
    /\ alive = _TETrace[1].alive
    /\ hbOwner = _TETrace[1].hbOwner
    /\ committed = _TETrace[1].committed
    /\ hbSeq = _TETrace[1].hbSeq
    /\ roundFence = _TETrace[1].roundFence
    /\ stOwner = _TETrace[1].stOwner
    /\ clock = _TETrace[1].clock
    /\ obs = _TETrace[1].obs
----

_next ==
    /\ \E i,j \in DOMAIN _TETrace:
        /\ \/ /\ j = i + 1
              /\ i = TLCGet("level")
        /\ falseSteal  = _TETrace[i].falseSteal
        /\ falseSteal' = _TETrace[j].falseSteal
        /\ stFence  = _TETrace[i].stFence
        /\ stFence' = _TETrace[j].stFence
        /\ inRound  = _TETrace[i].inRound
        /\ inRound' = _TETrace[j].inRound
        /\ stSeq  = _TETrace[i].stSeq
        /\ stSeq' = _TETrace[j].stSeq
        /\ alive  = _TETrace[i].alive
        /\ alive' = _TETrace[j].alive
        /\ hbOwner  = _TETrace[i].hbOwner
        /\ hbOwner' = _TETrace[j].hbOwner
        /\ committed  = _TETrace[i].committed
        /\ committed' = _TETrace[j].committed
        /\ hbSeq  = _TETrace[i].hbSeq
        /\ hbSeq' = _TETrace[j].hbSeq
        /\ roundFence  = _TETrace[i].roundFence
        /\ roundFence' = _TETrace[j].roundFence
        /\ stOwner  = _TETrace[i].stOwner
        /\ stOwner' = _TETrace[j].stOwner
        /\ clock  = _TETrace[i].clock
        /\ clock' = _TETrace[j].clock
        /\ obs  = _TETrace[i].obs
        /\ obs' = _TETrace[j].obs

\* Uncomment the ASSUME below to write the states of the error trace
\* to the given file in Json format. Note that you can pass any tuple
\* to `JsonSerialize`. For example, a sub-sequence of _TETrace.
    \* ASSUME
    \*     LET J == INSTANCE Json
    \*         IN J!JsonSerialize("CaGcLeaseCore_TTrace_1785722331.json", _TETrace)

=============================================================================

 Note that you can extract this module `CaGcLeaseCore_TEExpression`
  to a dedicated file to reuse `expression` (the module in the 
  dedicated `CaGcLeaseCore_TEExpression.tla` file takes precedence 
  over the module `CaGcLeaseCore_TEExpression` below).

---- MODULE CaGcLeaseCore_TEExpression ----
EXTENDS CaGcLeaseCore, Sequences, TLCExt, Toolbox, Naturals, TLC, CaGcLeaseCore_TEConstants

expression == 
    [
        \* To hide variables of the `CaGcLeaseCore` spec from the error trace,
        \* remove the variables below.  The trace will be written in the order
        \* of the fields of this record.
        falseSteal |-> falseSteal
        ,stFence |-> stFence
        ,inRound |-> inRound
        ,stSeq |-> stSeq
        ,alive |-> alive
        ,hbOwner |-> hbOwner
        ,committed |-> committed
        ,hbSeq |-> hbSeq
        ,roundFence |-> roundFence
        ,stOwner |-> stOwner
        ,clock |-> clock
        ,obs |-> obs
        
        \* Put additional constant-, state-, and action-level expressions here:
        \* ,_stateNumber |-> _TEPosition
        \* ,_falseStealUnchanged |-> falseSteal = falseSteal'
        
        \* Format the `falseSteal` variable as Json value.
        \* ,_falseStealJson |->
        \*     LET J == INSTANCE Json
        \*     IN J!ToJson(falseSteal)
        
        \* Lastly, you may build expressions over arbitrary sets of states by
        \* leveraging the _TETrace operator.  For example, this is how to
        \* count the number of times a spec variable changed up to the current
        \* state in the trace.
        \* ,_falseStealModCount |->
        \*     LET F[s \in DOMAIN _TETrace] ==
        \*         IF s = 1 THEN 0
        \*         ELSE IF _TETrace[s].falseSteal # _TETrace[s-1].falseSteal
        \*             THEN 1 + F[s-1] ELSE F[s-1]
        \*     IN F[_TEPosition - 1]
    ]

=============================================================================



Parsing and semantic processing can take forever if the trace below is long.
 In this case, it is advised to uncomment the module below to deserialize the
 trace from a generated binary file.

\*
\*---- MODULE CaGcLeaseCore_TETrace ----
\*EXTENDS CaGcLeaseCore, IOUtils, TLC, CaGcLeaseCore_TEConstants
\*
\*trace == IODeserialize("CaGcLeaseCore_TTrace_1785722331.bin", TRUE)
\*
\*=============================================================================
\*

---- MODULE CaGcLeaseCore_TETrace ----
EXTENDS CaGcLeaseCore, TLC, CaGcLeaseCore_TEConstants

trace == 
    <<
    ([obs |-> (L1 :> [seen |-> FALSE] @@ L2 :> [seen |-> FALSE]),falseSteal |-> FALSE,inRound |-> (L1 :> FALSE @@ L2 :> FALSE),committed |-> {},hbSeq |-> 0,stSeq |-> 0,alive |-> (L1 :> TRUE @@ L2 :> TRUE),roundFence |-> (L1 :> 0 @@ L2 :> 0),stFence |-> 0,clock |-> 0,hbOwner |-> NoneOwner,stOwner |-> NoneOwner]),
    ([obs |-> (L1 :> [seen |-> FALSE] @@ L2 :> [seen |-> FALSE]),falseSteal |-> FALSE,inRound |-> (L1 :> TRUE @@ L2 :> FALSE),committed |-> {},hbSeq |-> 0,stSeq |-> 1,alive |-> (L1 :> TRUE @@ L2 :> TRUE),roundFence |-> (L1 :> 0 @@ L2 :> 0),stFence |-> 0,clock |-> 0,hbOwner |-> NoneOwner,stOwner |-> L1]),
    ([obs |-> (L1 :> [seen |-> FALSE] @@ L2 :> [seen |-> TRUE, hbOwner |-> NoneOwner, hbSeq |-> 0, owner |-> L1, seq |-> 1, clk |-> 0]),falseSteal |-> FALSE,inRound |-> (L1 :> TRUE @@ L2 :> FALSE),committed |-> {},hbSeq |-> 0,stSeq |-> 1,alive |-> (L1 :> TRUE @@ L2 :> TRUE),roundFence |-> (L1 :> 0 @@ L2 :> 0),stFence |-> 0,clock |-> 0,hbOwner |-> NoneOwner,stOwner |-> L1]),
    ([obs |-> (L1 :> [seen |-> FALSE] @@ L2 :> [seen |-> TRUE, hbOwner |-> NoneOwner, hbSeq |-> 0, owner |-> L1, seq |-> 1, clk |-> 0]),falseSteal |-> FALSE,inRound |-> (L1 :> TRUE @@ L2 :> FALSE),committed |-> {},hbSeq |-> 0,stSeq |-> 1,alive |-> (L1 :> TRUE @@ L2 :> TRUE),roundFence |-> (L1 :> 0 @@ L2 :> 0),stFence |-> 0,clock |-> 1,hbOwner |-> NoneOwner,stOwner |-> L1]),
    ([obs |-> (L1 :> [seen |-> FALSE] @@ L2 :> [seen |-> FALSE]),falseSteal |-> TRUE,inRound |-> (L1 :> TRUE @@ L2 :> FALSE),committed |-> {},hbSeq |-> 0,stSeq |-> 2,alive |-> (L1 :> TRUE @@ L2 :> TRUE),roundFence |-> (L1 :> 0 @@ L2 :> 0),stFence |-> 1,clock |-> 1,hbOwner |-> NoneOwner,stOwner |-> L2])
    >>
----


=============================================================================

---- MODULE CaGcLeaseCore_TEConstants ----
EXTENDS CaGcLeaseCore

CONSTANTS L1, L2, NoneOwner

=============================================================================

---- CONFIG CaGcLeaseCore_TTrace_1785722331 ----
CONSTANTS
    Actors = { L1 , L2 }
    None = NoneOwner
    EnableHeartbeat = FALSE
    MaxClock = 4
    MaxSeq = 5
    MaxFence = 3
    L1 = L1
    L2 = L2
    NoneOwner = NoneOwner

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
\* Generated on Mon Aug 03 03:58:52 CEST 2026