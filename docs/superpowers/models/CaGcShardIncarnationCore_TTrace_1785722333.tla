---- MODULE CaGcShardIncarnationCore_TTrace_1785722333 ----
EXTENDS CaGcShardIncarnationCore_TEConstants, Sequences, CaGcShardIncarnationCore, TLCExt, Toolbox, Naturals, TLC

_expression ==
    LET CaGcShardIncarnationCore_TEExpression == INSTANCE CaGcShardIncarnationCore_TEExpression
    IN CaGcShardIncarnationCore_TEExpression!expression
----

_trace ==
    LET CaGcShardIncarnationCore_TETrace == INSTANCE CaGcShardIncarnationCore_TETrace
    IN CaGcShardIncarnationCore_TETrace!trace
----

_inv ==
    ~(
        TLCGet("level") = Len(_TETrace)
        /\
        cursor = ((n1 :> [inc |-> 1, pos |-> 3] @@ n2 :> [inc |-> 0, pos |-> 0]))
        /\
        deadTok = ((b1 :> {1, 2}))
        /\
        sIncMax = ((n1 :> 1 @@ n2 :> 0))
        /\
        log = ((n1 :> <<[op |-> "add", b |-> b1], [op |-> "fence", b |-> "none"]>> @@ n2 :> <<>>))
        /\
        tomb = ((n1 :> FALSE @@ n2 :> FALSE))
        /\
        fencePos = ((n1 :> 2 @@ n2 :> 0))
        /\
        rootEdges = ({})
        /\
        wView = ((w1 :> 1 @@ w2 :> 0))
        /\
        nextTok = ((b1 :> 3))
        /\
        everEdged = ({b1})
        /\
        inflight = ({})
        /\
        sPresent = ((n1 :> TRUE @@ n2 :> FALSE))
        /\
        wHave = ((w1 :> {} @@ w2 :> {}))
        /\
        roundOf = ((L1 :> 2))
        /\
        tokOf = ((b1 :> 2))
        /\
        refs = ((n1 :> {<<b1, 2>>} @@ n2 :> {}))
        /\
        gcPhase = ((L1 :> "fenced"))
        /\
        fencedSet = ((L1 :> {n1}))
        /\
        retired = ({})
        /\
        gcRound = (2)
        /\
        present = ((b1 :> FALSE))
        /\
        fence = ((n1 :> 2 @@ n2 :> 0))
        /\
        sInc = ((n1 :> 1 @@ n2 :> 0))
    )
----

_init ==
    /\ nextTok = _TETrace[1].nextTok
    /\ wHave = _TETrace[1].wHave
    /\ fence = _TETrace[1].fence
    /\ everEdged = _TETrace[1].everEdged
    /\ log = _TETrace[1].log
    /\ refs = _TETrace[1].refs
    /\ present = _TETrace[1].present
    /\ fencePos = _TETrace[1].fencePos
    /\ sInc = _TETrace[1].sInc
    /\ tomb = _TETrace[1].tomb
    /\ inflight = _TETrace[1].inflight
    /\ gcRound = _TETrace[1].gcRound
    /\ cursor = _TETrace[1].cursor
    /\ roundOf = _TETrace[1].roundOf
    /\ sIncMax = _TETrace[1].sIncMax
    /\ gcPhase = _TETrace[1].gcPhase
    /\ rootEdges = _TETrace[1].rootEdges
    /\ tokOf = _TETrace[1].tokOf
    /\ fencedSet = _TETrace[1].fencedSet
    /\ wView = _TETrace[1].wView
    /\ deadTok = _TETrace[1].deadTok
    /\ retired = _TETrace[1].retired
    /\ sPresent = _TETrace[1].sPresent
----

_next ==
    /\ \E i,j \in DOMAIN _TETrace:
        /\ \/ /\ j = i + 1
              /\ i = TLCGet("level")
        /\ nextTok  = _TETrace[i].nextTok
        /\ nextTok' = _TETrace[j].nextTok
        /\ wHave  = _TETrace[i].wHave
        /\ wHave' = _TETrace[j].wHave
        /\ fence  = _TETrace[i].fence
        /\ fence' = _TETrace[j].fence
        /\ everEdged  = _TETrace[i].everEdged
        /\ everEdged' = _TETrace[j].everEdged
        /\ log  = _TETrace[i].log
        /\ log' = _TETrace[j].log
        /\ refs  = _TETrace[i].refs
        /\ refs' = _TETrace[j].refs
        /\ present  = _TETrace[i].present
        /\ present' = _TETrace[j].present
        /\ fencePos  = _TETrace[i].fencePos
        /\ fencePos' = _TETrace[j].fencePos
        /\ sInc  = _TETrace[i].sInc
        /\ sInc' = _TETrace[j].sInc
        /\ tomb  = _TETrace[i].tomb
        /\ tomb' = _TETrace[j].tomb
        /\ inflight  = _TETrace[i].inflight
        /\ inflight' = _TETrace[j].inflight
        /\ gcRound  = _TETrace[i].gcRound
        /\ gcRound' = _TETrace[j].gcRound
        /\ cursor  = _TETrace[i].cursor
        /\ cursor' = _TETrace[j].cursor
        /\ roundOf  = _TETrace[i].roundOf
        /\ roundOf' = _TETrace[j].roundOf
        /\ sIncMax  = _TETrace[i].sIncMax
        /\ sIncMax' = _TETrace[j].sIncMax
        /\ gcPhase  = _TETrace[i].gcPhase
        /\ gcPhase' = _TETrace[j].gcPhase
        /\ rootEdges  = _TETrace[i].rootEdges
        /\ rootEdges' = _TETrace[j].rootEdges
        /\ tokOf  = _TETrace[i].tokOf
        /\ tokOf' = _TETrace[j].tokOf
        /\ fencedSet  = _TETrace[i].fencedSet
        /\ fencedSet' = _TETrace[j].fencedSet
        /\ wView  = _TETrace[i].wView
        /\ wView' = _TETrace[j].wView
        /\ deadTok  = _TETrace[i].deadTok
        /\ deadTok' = _TETrace[j].deadTok
        /\ retired  = _TETrace[i].retired
        /\ retired' = _TETrace[j].retired
        /\ sPresent  = _TETrace[i].sPresent
        /\ sPresent' = _TETrace[j].sPresent

\* Uncomment the ASSUME below to write the states of the error trace
\* to the given file in Json format. Note that you can pass any tuple
\* to `JsonSerialize`. For example, a sub-sequence of _TETrace.
    \* ASSUME
    \*     LET J == INSTANCE Json
    \*         IN J!JsonSerialize("CaGcShardIncarnationCore_TTrace_1785722333.json", _TETrace)

=============================================================================

 Note that you can extract this module `CaGcShardIncarnationCore_TEExpression`
  to a dedicated file to reuse `expression` (the module in the 
  dedicated `CaGcShardIncarnationCore_TEExpression.tla` file takes precedence 
  over the module `CaGcShardIncarnationCore_TEExpression` below).

---- MODULE CaGcShardIncarnationCore_TEExpression ----
EXTENDS CaGcShardIncarnationCore_TEConstants, Sequences, CaGcShardIncarnationCore, TLCExt, Toolbox, Naturals, TLC

expression == 
    [
        \* To hide variables of the `CaGcShardIncarnationCore` spec from the error trace,
        \* remove the variables below.  The trace will be written in the order
        \* of the fields of this record.
        nextTok |-> nextTok
        ,wHave |-> wHave
        ,fence |-> fence
        ,everEdged |-> everEdged
        ,log |-> log
        ,refs |-> refs
        ,present |-> present
        ,fencePos |-> fencePos
        ,sInc |-> sInc
        ,tomb |-> tomb
        ,inflight |-> inflight
        ,gcRound |-> gcRound
        ,cursor |-> cursor
        ,roundOf |-> roundOf
        ,sIncMax |-> sIncMax
        ,gcPhase |-> gcPhase
        ,rootEdges |-> rootEdges
        ,tokOf |-> tokOf
        ,fencedSet |-> fencedSet
        ,wView |-> wView
        ,deadTok |-> deadTok
        ,retired |-> retired
        ,sPresent |-> sPresent
        
        \* Put additional constant-, state-, and action-level expressions here:
        \* ,_stateNumber |-> _TEPosition
        \* ,_nextTokUnchanged |-> nextTok = nextTok'
        
        \* Format the `nextTok` variable as Json value.
        \* ,_nextTokJson |->
        \*     LET J == INSTANCE Json
        \*     IN J!ToJson(nextTok)
        
        \* Lastly, you may build expressions over arbitrary sets of states by
        \* leveraging the _TETrace operator.  For example, this is how to
        \* count the number of times a spec variable changed up to the current
        \* state in the trace.
        \* ,_nextTokModCount |->
        \*     LET F[s \in DOMAIN _TETrace] ==
        \*         IF s = 1 THEN 0
        \*         ELSE IF _TETrace[s].nextTok # _TETrace[s-1].nextTok
        \*             THEN 1 + F[s-1] ELSE F[s-1]
        \*     IN F[_TEPosition - 1]
    ]

=============================================================================



Parsing and semantic processing can take forever if the trace below is long.
 In this case, it is advised to uncomment the module below to deserialize the
 trace from a generated binary file.

\*
\*---- MODULE CaGcShardIncarnationCore_TETrace ----
\*EXTENDS CaGcShardIncarnationCore_TEConstants, IOUtils, CaGcShardIncarnationCore, TLC
\*
\*trace == IODeserialize("CaGcShardIncarnationCore_TTrace_1785722333.bin", TRUE)
\*
\*=============================================================================
\*

---- MODULE CaGcShardIncarnationCore_TETrace ----
EXTENDS CaGcShardIncarnationCore_TEConstants, CaGcShardIncarnationCore, TLC

trace == 
    <<
    ([cursor |-> (n1 :> [inc |-> 0, pos |-> 0] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {}),sIncMax |-> (n1 :> 0 @@ n2 :> 0),log |-> (n1 :> <<>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 1),everEdged |-> {},inflight |-> {},sPresent |-> (n1 :> FALSE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 0),tokOf |-> (b1 :> 0),refs |-> (n1 :> {} @@ n2 :> {}),gcPhase |-> (L1 :> "idle"),fencedSet |-> (L1 :> {}),retired |-> {},gcRound |-> 0,present |-> (b1 :> FALSE),fence |-> (n1 :> 0 @@ n2 :> 0),sInc |-> (n1 :> 0 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 0, pos |-> 0] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {}),sIncMax |-> (n1 :> 0 @@ n2 :> 0),log |-> (n1 :> <<>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),everEdged |-> {},inflight |-> {},sPresent |-> (n1 :> FALSE @@ n2 :> FALSE),wHave |-> (w1 :> {<<b1, 1>>} @@ w2 :> {}),roundOf |-> (L1 :> 0),tokOf |-> (b1 :> 1),refs |-> (n1 :> {} @@ n2 :> {}),gcPhase |-> (L1 :> "idle"),fencedSet |-> (L1 :> {}),retired |-> {},gcRound |-> 0,present |-> (b1 :> TRUE),fence |-> (n1 :> 0 @@ n2 :> 0),sInc |-> (n1 :> 0 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 0, pos |-> 0] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1]>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),everEdged |-> {},inflight |-> {},sPresent |-> (n1 :> TRUE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 0),tokOf |-> (b1 :> 1),refs |-> (n1 :> {<<b1, 1>>} @@ n2 :> {}),gcPhase |-> (L1 :> "idle"),fencedSet |-> (L1 :> {}),retired |-> {},gcRound |-> 0,present |-> (b1 :> TRUE),fence |-> (n1 :> 0 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 0, pos |-> 0] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1], [op |-> "rem", b |-> b1]>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),everEdged |-> {},inflight |-> {},sPresent |-> (n1 :> TRUE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 0),tokOf |-> (b1 :> 1),refs |-> (n1 :> {} @@ n2 :> {}),gcPhase |-> (L1 :> "idle"),fencedSet |-> (L1 :> {}),retired |-> {},gcRound |-> 0,present |-> (b1 :> TRUE),fence |-> (n1 :> 0 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 0, pos |-> 0] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1], [op |-> "rem", b |-> b1], [op |-> "tomb", b |-> "none"]>> @@ n2 :> <<>>),tomb |-> (n1 :> TRUE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),everEdged |-> {},inflight |-> {},sPresent |-> (n1 :> TRUE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 0),tokOf |-> (b1 :> 1),refs |-> (n1 :> {} @@ n2 :> {}),gcPhase |-> (L1 :> "idle"),fencedSet |-> (L1 :> {}),retired |-> {},gcRound |-> 0,present |-> (b1 :> TRUE),fence |-> (n1 :> 0 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 0, pos |-> 0] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1], [op |-> "rem", b |-> b1], [op |-> "tomb", b |-> "none"]>> @@ n2 :> <<>>),tomb |-> (n1 :> TRUE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),everEdged |-> {},inflight |-> {},sPresent |-> (n1 :> TRUE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),refs |-> (n1 :> {} @@ n2 :> {}),gcPhase |-> (L1 :> "retiring"),fencedSet |-> (L1 :> {}),retired |-> {},gcRound |-> 1,present |-> (b1 :> TRUE),fence |-> (n1 :> 0 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 1] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1], [op |-> "rem", b |-> b1], [op |-> "tomb", b |-> "none"]>> @@ n2 :> <<>>),tomb |-> (n1 :> TRUE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {<<n1, b1>>},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),everEdged |-> {b1},inflight |-> {},sPresent |-> (n1 :> TRUE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),refs |-> (n1 :> {} @@ n2 :> {}),gcPhase |-> (L1 :> "retiring"),fencedSet |-> (L1 :> {}),retired |-> {},gcRound |-> 1,present |-> (b1 :> TRUE),fence |-> (n1 :> 0 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 2] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1], [op |-> "rem", b |-> b1], [op |-> "tomb", b |-> "none"]>> @@ n2 :> <<>>),tomb |-> (n1 :> TRUE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),everEdged |-> {b1},inflight |-> {},sPresent |-> (n1 :> TRUE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),refs |-> (n1 :> {} @@ n2 :> {}),gcPhase |-> (L1 :> "retiring"),fencedSet |-> (L1 :> {}),retired |-> {},gcRound |-> 1,present |-> (b1 :> TRUE),fence |-> (n1 :> 0 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 3] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1], [op |-> "rem", b |-> b1], [op |-> "tomb", b |-> "none"]>> @@ n2 :> <<>>),tomb |-> (n1 :> TRUE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),everEdged |-> {b1},inflight |-> {},sPresent |-> (n1 :> TRUE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),refs |-> (n1 :> {} @@ n2 :> {}),gcPhase |-> (L1 :> "retiring"),fencedSet |-> (L1 :> {}),retired |-> {},gcRound |-> 1,present |-> (b1 :> TRUE),fence |-> (n1 :> 0 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 3] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1], [op |-> "rem", b |-> b1], [op |-> "tomb", b |-> "none"]>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),everEdged |-> {b1},inflight |-> {},sPresent |-> (n1 :> FALSE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),refs |-> (n1 :> {} @@ n2 :> {}),gcPhase |-> (L1 :> "retiring"),fencedSet |-> (L1 :> {}),retired |-> {},gcRound |-> 1,present |-> (b1 :> TRUE),fence |-> (n1 :> 0 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 3] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1], [op |-> "rem", b |-> b1], [op |-> "tomb", b |-> "none"]>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),everEdged |-> {b1},inflight |-> {},sPresent |-> (n1 :> FALSE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),refs |-> (n1 :> {} @@ n2 :> {}),gcPhase |-> (L1 :> "retiring"),fencedSet |-> (L1 :> {}),retired |-> {[b |-> b1, t |-> 1, r |-> 1]},gcRound |-> 1,present |-> (b1 :> TRUE),fence |-> (n1 :> 0 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 3] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1], [op |-> "rem", b |-> b1], [op |-> "tomb", b |-> "none"]>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (b1 :> 2),everEdged |-> {b1},inflight |-> {},sPresent |-> (n1 :> FALSE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),refs |-> (n1 :> {} @@ n2 :> {}),gcPhase |-> (L1 :> "fenced"),fencedSet |-> (L1 :> {}),retired |-> {[b |-> b1, t |-> 1, r |-> 1]},gcRound |-> 1,present |-> (b1 :> TRUE),fence |-> (n1 :> 0 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 3] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1], [op |-> "rem", b |-> b1], [op |-> "tomb", b |-> "none"]>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 1 @@ w2 :> 0),nextTok |-> (b1 :> 2),everEdged |-> {b1},inflight |-> {},sPresent |-> (n1 :> FALSE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),refs |-> (n1 :> {} @@ n2 :> {}),gcPhase |-> (L1 :> "fenced"),fencedSet |-> (L1 :> {}),retired |-> {[b |-> b1, t |-> 1, r |-> 1]},gcRound |-> 1,present |-> (b1 :> TRUE),fence |-> (n1 :> 0 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 3] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1], [op |-> "rem", b |-> b1], [op |-> "tomb", b |-> "none"]>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 1 @@ w2 :> 0),nextTok |-> (b1 :> 2),everEdged |-> {b1},inflight |-> {[b |-> b1, t |-> 1]},sPresent |-> (n1 :> FALSE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),refs |-> (n1 :> {} @@ n2 :> {}),gcPhase |-> (L1 :> "fenced"),fencedSet |-> (L1 :> {}),retired |-> {[b |-> b1, t |-> 1, r |-> 1]},gcRound |-> 1,present |-> (b1 :> TRUE),fence |-> (n1 :> 0 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 3] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {1}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1], [op |-> "rem", b |-> b1], [op |-> "tomb", b |-> "none"]>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 1 @@ w2 :> 0),nextTok |-> (b1 :> 2),everEdged |-> {b1},inflight |-> {},sPresent |-> (n1 :> FALSE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),refs |-> (n1 :> {} @@ n2 :> {}),gcPhase |-> (L1 :> "fenced"),fencedSet |-> (L1 :> {}),retired |-> {},gcRound |-> 1,present |-> (b1 :> FALSE),fence |-> (n1 :> 0 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 3] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {1}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1], [op |-> "rem", b |-> b1], [op |-> "tomb", b |-> "none"]>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 1 @@ w2 :> 0),nextTok |-> (b1 :> 3),everEdged |-> {b1},inflight |-> {},sPresent |-> (n1 :> FALSE @@ n2 :> FALSE),wHave |-> (w1 :> {<<b1, 2>>} @@ w2 :> {}),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 2),refs |-> (n1 :> {} @@ n2 :> {}),gcPhase |-> (L1 :> "fenced"),fencedSet |-> (L1 :> {}),retired |-> {},gcRound |-> 1,present |-> (b1 :> TRUE),fence |-> (n1 :> 0 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 3] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {1}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1]>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 1 @@ w2 :> 0),nextTok |-> (b1 :> 3),everEdged |-> {b1},inflight |-> {},sPresent |-> (n1 :> TRUE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 2),refs |-> (n1 :> {<<b1, 2>>} @@ n2 :> {}),gcPhase |-> (L1 :> "fenced"),fencedSet |-> (L1 :> {}),retired |-> {},gcRound |-> 1,present |-> (b1 :> TRUE),fence |-> (n1 :> 1 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 3] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {1}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1]>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 1 @@ w2 :> 0),nextTok |-> (b1 :> 3),everEdged |-> {b1},inflight |-> {},sPresent |-> (n1 :> TRUE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 2),refs |-> (n1 :> {<<b1, 2>>} @@ n2 :> {}),gcPhase |-> (L1 :> "idle"),fencedSet |-> (L1 :> {}),retired |-> {},gcRound |-> 1,present |-> (b1 :> TRUE),fence |-> (n1 :> 1 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 3] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {1}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1]>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 1 @@ w2 :> 0),nextTok |-> (b1 :> 3),everEdged |-> {b1},inflight |-> {},sPresent |-> (n1 :> TRUE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 2),tokOf |-> (b1 :> 2),refs |-> (n1 :> {<<b1, 2>>} @@ n2 :> {}),gcPhase |-> (L1 :> "retiring"),fencedSet |-> (L1 :> {}),retired |-> {},gcRound |-> 2,present |-> (b1 :> TRUE),fence |-> (n1 :> 1 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 3] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {1}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1]>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 0 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 1 @@ w2 :> 0),nextTok |-> (b1 :> 3),everEdged |-> {b1},inflight |-> {},sPresent |-> (n1 :> TRUE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 2),tokOf |-> (b1 :> 2),refs |-> (n1 :> {<<b1, 2>>} @@ n2 :> {}),gcPhase |-> (L1 :> "retiring"),fencedSet |-> (L1 :> {}),retired |-> {[b |-> b1, t |-> 2, r |-> 2]},gcRound |-> 2,present |-> (b1 :> TRUE),fence |-> (n1 :> 1 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 3] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {1}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1], [op |-> "fence", b |-> "none"]>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 2 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 1 @@ w2 :> 0),nextTok |-> (b1 :> 3),everEdged |-> {b1},inflight |-> {},sPresent |-> (n1 :> TRUE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 2),tokOf |-> (b1 :> 2),refs |-> (n1 :> {<<b1, 2>>} @@ n2 :> {}),gcPhase |-> (L1 :> "fenced"),fencedSet |-> (L1 :> {n1}),retired |-> {[b |-> b1, t |-> 2, r |-> 2]},gcRound |-> 2,present |-> (b1 :> TRUE),fence |-> (n1 :> 2 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 3] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {1}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1], [op |-> "fence", b |-> "none"]>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 2 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 1 @@ w2 :> 0),nextTok |-> (b1 :> 3),everEdged |-> {b1},inflight |-> {[b |-> b1, t |-> 2]},sPresent |-> (n1 :> TRUE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 2),tokOf |-> (b1 :> 2),refs |-> (n1 :> {<<b1, 2>>} @@ n2 :> {}),gcPhase |-> (L1 :> "fenced"),fencedSet |-> (L1 :> {n1}),retired |-> {[b |-> b1, t |-> 2, r |-> 2]},gcRound |-> 2,present |-> (b1 :> TRUE),fence |-> (n1 :> 2 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)]),
    ([cursor |-> (n1 :> [inc |-> 1, pos |-> 3] @@ n2 :> [inc |-> 0, pos |-> 0]),deadTok |-> (b1 :> {1, 2}),sIncMax |-> (n1 :> 1 @@ n2 :> 0),log |-> (n1 :> <<[op |-> "add", b |-> b1], [op |-> "fence", b |-> "none"]>> @@ n2 :> <<>>),tomb |-> (n1 :> FALSE @@ n2 :> FALSE),fencePos |-> (n1 :> 2 @@ n2 :> 0),rootEdges |-> {},wView |-> (w1 :> 1 @@ w2 :> 0),nextTok |-> (b1 :> 3),everEdged |-> {b1},inflight |-> {},sPresent |-> (n1 :> TRUE @@ n2 :> FALSE),wHave |-> (w1 :> {} @@ w2 :> {}),roundOf |-> (L1 :> 2),tokOf |-> (b1 :> 2),refs |-> (n1 :> {<<b1, 2>>} @@ n2 :> {}),gcPhase |-> (L1 :> "fenced"),fencedSet |-> (L1 :> {n1}),retired |-> {},gcRound |-> 2,present |-> (b1 :> FALSE),fence |-> (n1 :> 2 @@ n2 :> 0),sInc |-> (n1 :> 1 @@ n2 :> 0)])
    >>
----


=============================================================================

---- MODULE CaGcShardIncarnationCore_TEConstants ----
EXTENDS CaGcShardIncarnationCore

CONSTANTS b1, n1, n2, w1, w2, L1

=============================================================================

---- CONFIG CaGcShardIncarnationCore_TTrace_1785722333 ----
CONSTANTS
    Blobs = { b1 }
    Shards = { n1 , n2 }
    Writers = { w1 , w2 }
    Leaders = { L1 }
    MaxTok = 2
    MaxRound = 3
    MaxLog = 6
    MaxInc = 3
    SabotageNewbornNoFloor = FALSE
    SabotagePathKeyedCursor = FALSE
    SabotageDeleteBeforeFold = FALSE
    SabotageIncarnationReuse = TRUE
    b1 = b1
    n1 = n1
    w1 = w1
    w2 = w2
    L1 = L1
    n2 = n2

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
\* Generated on Mon Aug 03 03:58:55 CEST 2026