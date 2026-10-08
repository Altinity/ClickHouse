---- MODULE CaRelinkConfirmCore_TTrace_1785648729 ----
EXTENDS Sequences, TLCExt, CaRelinkConfirmCore, Toolbox, Naturals, TLC, CaRelinkConfirmCore_TEConstants

_expression ==
    LET CaRelinkConfirmCore_TEExpression == INSTANCE CaRelinkConfirmCore_TEExpression
    IN CaRelinkConfirmCore_TEExpression!expression
----

_trace ==
    LET CaRelinkConfirmCore_TETrace == INSTANCE CaRelinkConfirmCore_TETrace
    IN CaRelinkConfirmCore_TETrace!trace
----

_inv ==
    ~(
        TLCGet("level") = Len(_TETrace)
        /\
        cursor = ((r1 :> 0 @@ "ns_s" :> 1))
        /\
        sawConfirmNo = (FALSE)
        /\
        rAnswer = ((r1 :> "yes"))
        /\
        sCacheRef = ("m1")
        /\
        sLeader = (TRUE)
        /\
        sPoison = (FALSE)
        /\
        sDurableRef = ("m2")
        /\
        sTarget = ("m2")
        /\
        condemned = ({})
        /\
        rState = ((r1 :> "promoted"))
        /\
        nextId = (3)
        /\
        journal = ({[id |-> 1, ns |-> "ns_s", blob |-> "b1", src |-> "s_m1", op |-> "del"], [id |-> 2, ns |-> r1, blob |-> "b1", src |-> r1, op |-> "add"]})
        /\
        sPending = (TRUE)
        /\
        round = (3)
        /\
        gcPhase = ("idle")
        /\
        sFence = (TRUE)
        /\
        sawConfirmUnk = (FALSE)
        /\
        rDurableBefore = ((r1 :> TRUE))
        /\
        pendingDelete = ({})
        /\
        present = ([b1 |-> FALSE])
        /\
        folded = ({})
        /\
        holes = (0)
    )
----

_init ==
    /\ rAnswer = _TETrace[1].rAnswer
    /\ pendingDelete = _TETrace[1].pendingDelete
    /\ sawConfirmNo = _TETrace[1].sawConfirmNo
    /\ sPending = _TETrace[1].sPending
    /\ journal = _TETrace[1].journal
    /\ present = _TETrace[1].present
    /\ sPoison = _TETrace[1].sPoison
    /\ condemned = _TETrace[1].condemned
    /\ nextId = _TETrace[1].nextId
    /\ sTarget = _TETrace[1].sTarget
    /\ sFence = _TETrace[1].sFence
    /\ sawConfirmUnk = _TETrace[1].sawConfirmUnk
    /\ rDurableBefore = _TETrace[1].rDurableBefore
    /\ cursor = _TETrace[1].cursor
    /\ sDurableRef = _TETrace[1].sDurableRef
    /\ sCacheRef = _TETrace[1].sCacheRef
    /\ holes = _TETrace[1].holes
    /\ gcPhase = _TETrace[1].gcPhase
    /\ round = _TETrace[1].round
    /\ sLeader = _TETrace[1].sLeader
    /\ rState = _TETrace[1].rState
    /\ folded = _TETrace[1].folded
----

_next ==
    /\ \E i,j \in DOMAIN _TETrace:
        /\ \/ /\ j = i + 1
              /\ i = TLCGet("level")
        /\ rAnswer  = _TETrace[i].rAnswer
        /\ rAnswer' = _TETrace[j].rAnswer
        /\ pendingDelete  = _TETrace[i].pendingDelete
        /\ pendingDelete' = _TETrace[j].pendingDelete
        /\ sawConfirmNo  = _TETrace[i].sawConfirmNo
        /\ sawConfirmNo' = _TETrace[j].sawConfirmNo
        /\ sPending  = _TETrace[i].sPending
        /\ sPending' = _TETrace[j].sPending
        /\ journal  = _TETrace[i].journal
        /\ journal' = _TETrace[j].journal
        /\ present  = _TETrace[i].present
        /\ present' = _TETrace[j].present
        /\ sPoison  = _TETrace[i].sPoison
        /\ sPoison' = _TETrace[j].sPoison
        /\ condemned  = _TETrace[i].condemned
        /\ condemned' = _TETrace[j].condemned
        /\ nextId  = _TETrace[i].nextId
        /\ nextId' = _TETrace[j].nextId
        /\ sTarget  = _TETrace[i].sTarget
        /\ sTarget' = _TETrace[j].sTarget
        /\ sFence  = _TETrace[i].sFence
        /\ sFence' = _TETrace[j].sFence
        /\ sawConfirmUnk  = _TETrace[i].sawConfirmUnk
        /\ sawConfirmUnk' = _TETrace[j].sawConfirmUnk
        /\ rDurableBefore  = _TETrace[i].rDurableBefore
        /\ rDurableBefore' = _TETrace[j].rDurableBefore
        /\ cursor  = _TETrace[i].cursor
        /\ cursor' = _TETrace[j].cursor
        /\ sDurableRef  = _TETrace[i].sDurableRef
        /\ sDurableRef' = _TETrace[j].sDurableRef
        /\ sCacheRef  = _TETrace[i].sCacheRef
        /\ sCacheRef' = _TETrace[j].sCacheRef
        /\ holes  = _TETrace[i].holes
        /\ holes' = _TETrace[j].holes
        /\ gcPhase  = _TETrace[i].gcPhase
        /\ gcPhase' = _TETrace[j].gcPhase
        /\ round  = _TETrace[i].round
        /\ round' = _TETrace[j].round
        /\ sLeader  = _TETrace[i].sLeader
        /\ sLeader' = _TETrace[j].sLeader
        /\ rState  = _TETrace[i].rState
        /\ rState' = _TETrace[j].rState
        /\ folded  = _TETrace[i].folded
        /\ folded' = _TETrace[j].folded

\* Uncomment the ASSUME below to write the states of the error trace
\* to the given file in Json format. Note that you can pass any tuple
\* to `JsonSerialize`. For example, a sub-sequence of _TETrace.
    \* ASSUME
    \*     LET J == INSTANCE Json
    \*         IN J!JsonSerialize("CaRelinkConfirmCore_TTrace_1785648729.json", _TETrace)

=============================================================================

 Note that you can extract this module `CaRelinkConfirmCore_TEExpression`
  to a dedicated file to reuse `expression` (the module in the 
  dedicated `CaRelinkConfirmCore_TEExpression.tla` file takes precedence 
  over the module `CaRelinkConfirmCore_TEExpression` below).

---- MODULE CaRelinkConfirmCore_TEExpression ----
EXTENDS Sequences, TLCExt, CaRelinkConfirmCore, Toolbox, Naturals, TLC, CaRelinkConfirmCore_TEConstants

expression == 
    [
        \* To hide variables of the `CaRelinkConfirmCore` spec from the error trace,
        \* remove the variables below.  The trace will be written in the order
        \* of the fields of this record.
        rAnswer |-> rAnswer
        ,pendingDelete |-> pendingDelete
        ,sawConfirmNo |-> sawConfirmNo
        ,sPending |-> sPending
        ,journal |-> journal
        ,present |-> present
        ,sPoison |-> sPoison
        ,condemned |-> condemned
        ,nextId |-> nextId
        ,sTarget |-> sTarget
        ,sFence |-> sFence
        ,sawConfirmUnk |-> sawConfirmUnk
        ,rDurableBefore |-> rDurableBefore
        ,cursor |-> cursor
        ,sDurableRef |-> sDurableRef
        ,sCacheRef |-> sCacheRef
        ,holes |-> holes
        ,gcPhase |-> gcPhase
        ,round |-> round
        ,sLeader |-> sLeader
        ,rState |-> rState
        ,folded |-> folded
        
        \* Put additional constant-, state-, and action-level expressions here:
        \* ,_stateNumber |-> _TEPosition
        \* ,_rAnswerUnchanged |-> rAnswer = rAnswer'
        
        \* Format the `rAnswer` variable as Json value.
        \* ,_rAnswerJson |->
        \*     LET J == INSTANCE Json
        \*     IN J!ToJson(rAnswer)
        
        \* Lastly, you may build expressions over arbitrary sets of states by
        \* leveraging the _TETrace operator.  For example, this is how to
        \* count the number of times a spec variable changed up to the current
        \* state in the trace.
        \* ,_rAnswerModCount |->
        \*     LET F[s \in DOMAIN _TETrace] ==
        \*         IF s = 1 THEN 0
        \*         ELSE IF _TETrace[s].rAnswer # _TETrace[s-1].rAnswer
        \*             THEN 1 + F[s-1] ELSE F[s-1]
        \*     IN F[_TEPosition - 1]
    ]

=============================================================================



Parsing and semantic processing can take forever if the trace below is long.
 In this case, it is advised to uncomment the module below to deserialize the
 trace from a generated binary file.

\*
\*---- MODULE CaRelinkConfirmCore_TETrace ----
\*EXTENDS IOUtils, CaRelinkConfirmCore, TLC, CaRelinkConfirmCore_TEConstants
\*
\*trace == IODeserialize("CaRelinkConfirmCore_TTrace_1785648729.bin", TRUE)
\*
\*=============================================================================
\*

---- MODULE CaRelinkConfirmCore_TETrace ----
EXTENDS CaRelinkConfirmCore, TLC, CaRelinkConfirmCore_TEConstants

trace == 
    <<
    ([cursor |-> (r1 :> 0 @@ "ns_s" :> 0),sawConfirmNo |-> FALSE,rAnswer |-> (r1 :> "none"),sCacheRef |-> "m1",sLeader |-> FALSE,sPoison |-> FALSE,sDurableRef |-> "m1",sTarget |-> "m1",condemned |-> {},rState |-> (r1 :> "init"),nextId |-> 1,journal |-> {},sPending |-> FALSE,round |-> 0,gcPhase |-> "idle",sFence |-> TRUE,sawConfirmUnk |-> FALSE,rDurableBefore |-> (r1 :> FALSE),pendingDelete |-> {},present |-> [b1 |-> TRUE],folded |-> {[src |-> "s_m1", b |-> "b1"]},holes |-> 0]),
    ([cursor |-> (r1 :> 0 @@ "ns_s" :> 0),sawConfirmNo |-> FALSE,rAnswer |-> (r1 :> "none"),sCacheRef |-> "m1",sLeader |-> TRUE,sPoison |-> FALSE,sDurableRef |-> "m1",sTarget |-> "m2",condemned |-> {},rState |-> (r1 :> "init"),nextId |-> 1,journal |-> {},sPending |-> TRUE,round |-> 0,gcPhase |-> "idle",sFence |-> TRUE,sawConfirmUnk |-> FALSE,rDurableBefore |-> (r1 :> FALSE),pendingDelete |-> {},present |-> [b1 |-> TRUE],folded |-> {[src |-> "s_m1", b |-> "b1"]},holes |-> 0]),
    ([cursor |-> (r1 :> 0 @@ "ns_s" :> 0),sawConfirmNo |-> FALSE,rAnswer |-> (r1 :> "none"),sCacheRef |-> "m1",sLeader |-> TRUE,sPoison |-> FALSE,sDurableRef |-> "m2",sTarget |-> "m2",condemned |-> {},rState |-> (r1 :> "init"),nextId |-> 2,journal |-> {[id |-> 1, ns |-> "ns_s", blob |-> "b1", src |-> "s_m1", op |-> "del"]},sPending |-> TRUE,round |-> 0,gcPhase |-> "idle",sFence |-> TRUE,sawConfirmUnk |-> FALSE,rDurableBefore |-> (r1 :> FALSE),pendingDelete |-> {},present |-> [b1 |-> TRUE],folded |-> {[src |-> "s_m1", b |-> "b1"]},holes |-> 0]),
    ([cursor |-> (r1 :> 0 @@ "ns_s" :> 1),sawConfirmNo |-> FALSE,rAnswer |-> (r1 :> "none"),sCacheRef |-> "m1",sLeader |-> TRUE,sPoison |-> FALSE,sDurableRef |-> "m2",sTarget |-> "m2",condemned |-> {},rState |-> (r1 :> "init"),nextId |-> 2,journal |-> {[id |-> 1, ns |-> "ns_s", blob |-> "b1", src |-> "s_m1", op |-> "del"]},sPending |-> TRUE,round |-> 0,gcPhase |-> "folded",sFence |-> TRUE,sawConfirmUnk |-> FALSE,rDurableBefore |-> (r1 :> FALSE),pendingDelete |-> {},present |-> [b1 |-> TRUE],folded |-> {},holes |-> 0]),
    ([cursor |-> (r1 :> 0 @@ "ns_s" :> 1),sawConfirmNo |-> FALSE,rAnswer |-> (r1 :> "none"),sCacheRef |-> "m1",sLeader |-> TRUE,sPoison |-> FALSE,sDurableRef |-> "m2",sTarget |-> "m2",condemned |-> {"b1"},rState |-> (r1 :> "init"),nextId |-> 2,journal |-> {[id |-> 1, ns |-> "ns_s", blob |-> "b1", src |-> "s_m1", op |-> "del"]},sPending |-> TRUE,round |-> 1,gcPhase |-> "idle",sFence |-> TRUE,sawConfirmUnk |-> FALSE,rDurableBefore |-> (r1 :> FALSE),pendingDelete |-> {},present |-> [b1 |-> TRUE],folded |-> {},holes |-> 0]),
    ([cursor |-> (r1 :> 0 @@ "ns_s" :> 1),sawConfirmNo |-> FALSE,rAnswer |-> (r1 :> "none"),sCacheRef |-> "m1",sLeader |-> TRUE,sPoison |-> FALSE,sDurableRef |-> "m2",sTarget |-> "m2",condemned |-> {"b1"},rState |-> (r1 :> "init"),nextId |-> 2,journal |-> {[id |-> 1, ns |-> "ns_s", blob |-> "b1", src |-> "s_m1", op |-> "del"]},sPending |-> TRUE,round |-> 1,gcPhase |-> "folded",sFence |-> TRUE,sawConfirmUnk |-> FALSE,rDurableBefore |-> (r1 :> FALSE),pendingDelete |-> {},present |-> [b1 |-> TRUE],folded |-> {},holes |-> 0]),
    ([cursor |-> (r1 :> 0 @@ "ns_s" :> 1),sawConfirmNo |-> FALSE,rAnswer |-> (r1 :> "none"),sCacheRef |-> "m1",sLeader |-> TRUE,sPoison |-> FALSE,sDurableRef |-> "m2",sTarget |-> "m2",condemned |-> {"b1"},rState |-> (r1 :> "init"),nextId |-> 2,journal |-> {[id |-> 1, ns |-> "ns_s", blob |-> "b1", src |-> "s_m1", op |-> "del"]},sPending |-> TRUE,round |-> 2,gcPhase |-> "idle",sFence |-> TRUE,sawConfirmUnk |-> FALSE,rDurableBefore |-> (r1 :> FALSE),pendingDelete |-> {"b1"},present |-> [b1 |-> TRUE],folded |-> {},holes |-> 0]),
    ([cursor |-> (r1 :> 0 @@ "ns_s" :> 1),sawConfirmNo |-> FALSE,rAnswer |-> (r1 :> "none"),sCacheRef |-> "m1",sLeader |-> TRUE,sPoison |-> FALSE,sDurableRef |-> "m2",sTarget |-> "m2",condemned |-> {"b1"},rState |-> (r1 :> "init"),nextId |-> 2,journal |-> {[id |-> 1, ns |-> "ns_s", blob |-> "b1", src |-> "s_m1", op |-> "del"]},sPending |-> TRUE,round |-> 2,gcPhase |-> "folded",sFence |-> TRUE,sawConfirmUnk |-> FALSE,rDurableBefore |-> (r1 :> FALSE),pendingDelete |-> {"b1"},present |-> [b1 |-> TRUE],folded |-> {},holes |-> 0]),
    ([cursor |-> (r1 :> 0 @@ "ns_s" :> 1),sawConfirmNo |-> FALSE,rAnswer |-> (r1 :> "none"),sCacheRef |-> "m1",sLeader |-> TRUE,sPoison |-> FALSE,sDurableRef |-> "m2",sTarget |-> "m2",condemned |-> {"b1"},rState |-> (r1 :> "published"),nextId |-> 3,journal |-> {[id |-> 1, ns |-> "ns_s", blob |-> "b1", src |-> "s_m1", op |-> "del"], [id |-> 2, ns |-> r1, blob |-> "b1", src |-> r1, op |-> "add"]},sPending |-> TRUE,round |-> 2,gcPhase |-> "folded",sFence |-> TRUE,sawConfirmUnk |-> FALSE,rDurableBefore |-> (r1 :> FALSE),pendingDelete |-> {"b1"},present |-> [b1 |-> TRUE],folded |-> {},holes |-> 0]),
    ([cursor |-> (r1 :> 0 @@ "ns_s" :> 1),sawConfirmNo |-> FALSE,rAnswer |-> (r1 :> "yes"),sCacheRef |-> "m1",sLeader |-> TRUE,sPoison |-> FALSE,sDurableRef |-> "m2",sTarget |-> "m2",condemned |-> {"b1"},rState |-> (r1 :> "confirmed"),nextId |-> 3,journal |-> {[id |-> 1, ns |-> "ns_s", blob |-> "b1", src |-> "s_m1", op |-> "del"], [id |-> 2, ns |-> r1, blob |-> "b1", src |-> r1, op |-> "add"]},sPending |-> TRUE,round |-> 2,gcPhase |-> "folded",sFence |-> TRUE,sawConfirmUnk |-> FALSE,rDurableBefore |-> (r1 :> TRUE),pendingDelete |-> {"b1"},present |-> [b1 |-> TRUE],folded |-> {},holes |-> 0]),
    ([cursor |-> (r1 :> 0 @@ "ns_s" :> 1),sawConfirmNo |-> FALSE,rAnswer |-> (r1 :> "yes"),sCacheRef |-> "m1",sLeader |-> TRUE,sPoison |-> FALSE,sDurableRef |-> "m2",sTarget |-> "m2",condemned |-> {},rState |-> (r1 :> "confirmed"),nextId |-> 3,journal |-> {[id |-> 1, ns |-> "ns_s", blob |-> "b1", src |-> "s_m1", op |-> "del"], [id |-> 2, ns |-> r1, blob |-> "b1", src |-> r1, op |-> "add"]},sPending |-> TRUE,round |-> 3,gcPhase |-> "idle",sFence |-> TRUE,sawConfirmUnk |-> FALSE,rDurableBefore |-> (r1 :> TRUE),pendingDelete |-> {},present |-> [b1 |-> FALSE],folded |-> {},holes |-> 0]),
    ([cursor |-> (r1 :> 0 @@ "ns_s" :> 1),sawConfirmNo |-> FALSE,rAnswer |-> (r1 :> "yes"),sCacheRef |-> "m1",sLeader |-> TRUE,sPoison |-> FALSE,sDurableRef |-> "m2",sTarget |-> "m2",condemned |-> {},rState |-> (r1 :> "promoted"),nextId |-> 3,journal |-> {[id |-> 1, ns |-> "ns_s", blob |-> "b1", src |-> "s_m1", op |-> "del"], [id |-> 2, ns |-> r1, blob |-> "b1", src |-> r1, op |-> "add"]},sPending |-> TRUE,round |-> 3,gcPhase |-> "idle",sFence |-> TRUE,sawConfirmUnk |-> FALSE,rDurableBefore |-> (r1 :> TRUE),pendingDelete |-> {},present |-> [b1 |-> FALSE],folded |-> {},holes |-> 0])
    >>
----


=============================================================================

---- MODULE CaRelinkConfirmCore_TEConstants ----
EXTENDS CaRelinkConfirmCore

CONSTANTS r1

=============================================================================

---- CONFIG CaRelinkConfirmCore_TTrace_1785648729 ----
CONSTANTS
    Receivers = { r1 }
    MaxId = 5
    MaxRound = 5
    MaxHoles = 0
    SabotageNoGate1 = FALSE
    SabotageStaleCache = TRUE
    SabotageNoPoison = FALSE
    SabotageNoFence = FALSE
    SabotagePublishAfterConfirm = FALSE
    r1 = r1

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
\* Generated on Sun Aug 02 07:32:10 CEST 2026