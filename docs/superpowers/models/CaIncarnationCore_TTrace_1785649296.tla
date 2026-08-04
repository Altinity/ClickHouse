---- MODULE CaIncarnationCore_TTrace_1785649296 ----
EXTENDS CaIncarnationCore_TEConstants, CaIncarnationCore, Sequences, TLCExt, Toolbox, Naturals, TLC

_expression ==
    LET CaIncarnationCore_TEExpression == INSTANCE CaIncarnationCore_TEExpression
    IN CaIncarnationCore_TEExpression!expression
----

_trace ==
    LET CaIncarnationCore_TETrace == INSTANCE CaIncarnationCore_TETrace
    IN CaIncarnationCore_TETrace!trace
----

_prop ==
    ~(([]<>(
            cursor = ((s1 :> 4))
            /\
            hbObs = ((w1 :> -1 @@ w2 :> -1))
            /\
            fgRefs = ((s1 :> {}))
            /\
            deadTok = ((h1 :> {}))
            /\
            fencePos = ((s1 :> 0))
            /\
            trimBase = ((s1 :> 4))
            /\
            wEv = ((w1 :> {} @@ w2 :> {}))
            /\
            rootEdges = ({})
            /\
            wView = ((w1 :> 0 @@ w2 :> 0))
            /\
            nextTok = ((h1 :> 2))
            /\
            fgCut = ((s1 :> 0))
            /\
            roundOf = ((L1 :> 1))
            /\
            tokOf = ((h1 :> 1))
            /\
            reg = ([man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)])
            /\
            gcPhase = ((L1 :> "retiring"))
            /\
            pendCasc = ({})
            /\
            retired = ({[t |-> 1, h |-> h1, r |-> 1]})
            /\
            gcRound = (1)
            /\
            man = ((s1 :> [fence |-> 0, refs |-> {}, log |-> <<[h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"], [h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"]>>]))
            /\
            creator = ((h1 :> "none"))
            /\
            hbSeq = ((w1 :> 0 @@ w2 :> 0))
            /\
            everEdged = ({h1})
            /\
            treeEdges = ({})
            /\
            fgPhase = ("idle")
            /\
            inflight = ({})
            /\
            marker = ({})
            /\
            wDeps = ((w1 :> {} @@ w2 :> {<<h1, 1>>}))
            /\
            fencedSet = ((L1 :> {}))
            /\
            hbAlive = ((w1 :> FALSE @@ w2 :> FALSE))
            /\
            present = ((h1 :> TRUE))
            /\
            fgSeen = ({})
            /\
            wedged = ((w1 :> FALSE @@ w2 :> FALSE))
    ))/\([]<>(
            cursor = ((s1 :> 4))
            /\
            hbObs = ((w1 :> -1 @@ w2 :> -1))
            /\
            fgRefs = ((s1 :> {}))
            /\
            deadTok = ((h1 :> {}))
            /\
            fencePos = ((s1 :> 0))
            /\
            trimBase = ((s1 :> 4))
            /\
            wEv = ((w1 :> {} @@ w2 :> {}))
            /\
            rootEdges = ({})
            /\
            wView = ((w1 :> 0 @@ w2 :> 0))
            /\
            nextTok = ((h1 :> 2))
            /\
            fgCut = ((s1 :> 0))
            /\
            roundOf = ((L1 :> 1))
            /\
            tokOf = ((h1 :> 1))
            /\
            reg = ([man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)])
            /\
            gcPhase = ((L1 :> "retiring"))
            /\
            pendCasc = ({})
            /\
            retired = ({[t |-> 1, h |-> h1, r |-> 1]})
            /\
            gcRound = (1)
            /\
            man = ((s1 :> [fence |-> 0, refs |-> {}, log |-> <<[h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"], [h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"]>>]))
            /\
            creator = ((h1 :> "none"))
            /\
            hbSeq = ((w1 :> 0 @@ w2 :> 0))
            /\
            everEdged = ({h1})
            /\
            treeEdges = ({})
            /\
            fgPhase = ("idle")
            /\
            inflight = ({})
            /\
            marker = ({})
            /\
            wDeps = ((w1 :> {} @@ w2 :> {<<h1, 1>>, <<h1, 4>>}))
            /\
            fencedSet = ((L1 :> {}))
            /\
            hbAlive = ((w1 :> FALSE @@ w2 :> FALSE))
            /\
            present = ((h1 :> TRUE))
            /\
            fgSeen = ({})
            /\
            wedged = ((w1 :> FALSE @@ w2 :> FALSE))
    )))
----

_init ==
    /\ wedged = _TETrace[1].wedged
    /\ nextTok = _TETrace[1].nextTok
    /\ everEdged = _TETrace[1].everEdged
    /\ hbAlive = _TETrace[1].hbAlive
    /\ present = _TETrace[1].present
    /\ fgCut = _TETrace[1].fgCut
    /\ fencePos = _TETrace[1].fencePos
    /\ wEv = _TETrace[1].wEv
    /\ reg = _TETrace[1].reg
    /\ inflight = _TETrace[1].inflight
    /\ marker = _TETrace[1].marker
    /\ hbObs = _TETrace[1].hbObs
    /\ gcRound = _TETrace[1].gcRound
    /\ wDeps = _TETrace[1].wDeps
    /\ cursor = _TETrace[1].cursor
    /\ man = _TETrace[1].man
    /\ creator = _TETrace[1].creator
    /\ fgSeen = _TETrace[1].fgSeen
    /\ roundOf = _TETrace[1].roundOf
    /\ fgPhase = _TETrace[1].fgPhase
    /\ gcPhase = _TETrace[1].gcPhase
    /\ rootEdges = _TETrace[1].rootEdges
    /\ tokOf = _TETrace[1].tokOf
    /\ hbSeq = _TETrace[1].hbSeq
    /\ fencedSet = _TETrace[1].fencedSet
    /\ wView = _TETrace[1].wView
    /\ pendCasc = _TETrace[1].pendCasc
    /\ treeEdges = _TETrace[1].treeEdges
    /\ deadTok = _TETrace[1].deadTok
    /\ retired = _TETrace[1].retired
    /\ fgRefs = _TETrace[1].fgRefs
    /\ trimBase = _TETrace[1].trimBase
----

_next ==
    /\ \E i,j \in DOMAIN _TETrace:
        /\ \/ /\ j = i + 1
              /\ i = TLCGet("level")
           \/ /\ i = _TTraceLassoEnd
              /\ j = _TTraceLassoStart
        /\ wedged  = _TETrace[i].wedged
        /\ wedged' = _TETrace[j].wedged
        /\ nextTok  = _TETrace[i].nextTok
        /\ nextTok' = _TETrace[j].nextTok
        /\ everEdged  = _TETrace[i].everEdged
        /\ everEdged' = _TETrace[j].everEdged
        /\ hbAlive  = _TETrace[i].hbAlive
        /\ hbAlive' = _TETrace[j].hbAlive
        /\ present  = _TETrace[i].present
        /\ present' = _TETrace[j].present
        /\ fgCut  = _TETrace[i].fgCut
        /\ fgCut' = _TETrace[j].fgCut
        /\ fencePos  = _TETrace[i].fencePos
        /\ fencePos' = _TETrace[j].fencePos
        /\ wEv  = _TETrace[i].wEv
        /\ wEv' = _TETrace[j].wEv
        /\ reg  = _TETrace[i].reg
        /\ reg' = _TETrace[j].reg
        /\ inflight  = _TETrace[i].inflight
        /\ inflight' = _TETrace[j].inflight
        /\ marker  = _TETrace[i].marker
        /\ marker' = _TETrace[j].marker
        /\ hbObs  = _TETrace[i].hbObs
        /\ hbObs' = _TETrace[j].hbObs
        /\ gcRound  = _TETrace[i].gcRound
        /\ gcRound' = _TETrace[j].gcRound
        /\ wDeps  = _TETrace[i].wDeps
        /\ wDeps' = _TETrace[j].wDeps
        /\ cursor  = _TETrace[i].cursor
        /\ cursor' = _TETrace[j].cursor
        /\ man  = _TETrace[i].man
        /\ man' = _TETrace[j].man
        /\ creator  = _TETrace[i].creator
        /\ creator' = _TETrace[j].creator
        /\ fgSeen  = _TETrace[i].fgSeen
        /\ fgSeen' = _TETrace[j].fgSeen
        /\ roundOf  = _TETrace[i].roundOf
        /\ roundOf' = _TETrace[j].roundOf
        /\ fgPhase  = _TETrace[i].fgPhase
        /\ fgPhase' = _TETrace[j].fgPhase
        /\ gcPhase  = _TETrace[i].gcPhase
        /\ gcPhase' = _TETrace[j].gcPhase
        /\ rootEdges  = _TETrace[i].rootEdges
        /\ rootEdges' = _TETrace[j].rootEdges
        /\ tokOf  = _TETrace[i].tokOf
        /\ tokOf' = _TETrace[j].tokOf
        /\ hbSeq  = _TETrace[i].hbSeq
        /\ hbSeq' = _TETrace[j].hbSeq
        /\ fencedSet  = _TETrace[i].fencedSet
        /\ fencedSet' = _TETrace[j].fencedSet
        /\ wView  = _TETrace[i].wView
        /\ wView' = _TETrace[j].wView
        /\ pendCasc  = _TETrace[i].pendCasc
        /\ pendCasc' = _TETrace[j].pendCasc
        /\ treeEdges  = _TETrace[i].treeEdges
        /\ treeEdges' = _TETrace[j].treeEdges
        /\ deadTok  = _TETrace[i].deadTok
        /\ deadTok' = _TETrace[j].deadTok
        /\ retired  = _TETrace[i].retired
        /\ retired' = _TETrace[j].retired
        /\ fgRefs  = _TETrace[i].fgRefs
        /\ fgRefs' = _TETrace[j].fgRefs
        /\ trimBase  = _TETrace[i].trimBase
        /\ trimBase' = _TETrace[j].trimBase

\* Uncomment the ASSUME below to write the states of the error trace
\* to the given file in Json format. Note that you can pass any tuple
\* to `JsonSerialize`. For example, a sub-sequence of _TETrace.
    \* ASSUME
    \*     LET J == INSTANCE Json
    \*         IN J!JsonSerialize("CaIncarnationCore_TTrace_1785649296.json", _TETrace)


_view ==
    <<wedged, nextTok, everEdged, hbAlive, present, fgCut, fencePos, wEv, reg, inflight, marker, hbObs, gcRound, wDeps, cursor, man, creator, fgSeen, roundOf, fgPhase, gcPhase, rootEdges, tokOf, hbSeq, fencedSet, wView, pendCasc, treeEdges, deadTok, retired, fgRefs, trimBase, IF TLCGet("level") = _TTraceLassoEnd + 1 THEN _TTraceLassoStart ELSE TLCGet("level")>>
=============================================================================

 Note that you can extract this module `CaIncarnationCore_TEExpression`
  to a dedicated file to reuse `expression` (the module in the 
  dedicated `CaIncarnationCore_TEExpression.tla` file takes precedence 
  over the module `CaIncarnationCore_TEExpression` below).

---- MODULE CaIncarnationCore_TEExpression ----
EXTENDS CaIncarnationCore_TEConstants, CaIncarnationCore, Sequences, TLCExt, Toolbox, Naturals, TLC

expression == 
    [
        \* To hide variables of the `CaIncarnationCore` spec from the error trace,
        \* remove the variables below.  The trace will be written in the order
        \* of the fields of this record.
        wedged |-> wedged
        ,nextTok |-> nextTok
        ,everEdged |-> everEdged
        ,hbAlive |-> hbAlive
        ,present |-> present
        ,fgCut |-> fgCut
        ,fencePos |-> fencePos
        ,wEv |-> wEv
        ,reg |-> reg
        ,inflight |-> inflight
        ,marker |-> marker
        ,hbObs |-> hbObs
        ,gcRound |-> gcRound
        ,wDeps |-> wDeps
        ,cursor |-> cursor
        ,man |-> man
        ,creator |-> creator
        ,fgSeen |-> fgSeen
        ,roundOf |-> roundOf
        ,fgPhase |-> fgPhase
        ,gcPhase |-> gcPhase
        ,rootEdges |-> rootEdges
        ,tokOf |-> tokOf
        ,hbSeq |-> hbSeq
        ,fencedSet |-> fencedSet
        ,wView |-> wView
        ,pendCasc |-> pendCasc
        ,treeEdges |-> treeEdges
        ,deadTok |-> deadTok
        ,retired |-> retired
        ,fgRefs |-> fgRefs
        ,trimBase |-> trimBase
        
        \* Put additional constant-, state-, and action-level expressions here:
        \* ,_stateNumber |-> _TEPosition
        \* ,_wedgedUnchanged |-> wedged = wedged'
        
        \* Format the `wedged` variable as Json value.
        \* ,_wedgedJson |->
        \*     LET J == INSTANCE Json
        \*     IN J!ToJson(wedged)
        
        \* Lastly, you may build expressions over arbitrary sets of states by
        \* leveraging the _TETrace operator.  For example, this is how to
        \* count the number of times a spec variable changed up to the current
        \* state in the trace.
        \* ,_wedgedModCount |->
        \*     LET F[s \in DOMAIN _TETrace] ==
        \*         IF s = 1 THEN 0
        \*         ELSE IF _TETrace[s].wedged # _TETrace[s-1].wedged
        \*             THEN 1 + F[s-1] ELSE F[s-1]
        \*     IN F[_TEPosition - 1]
    ]

=============================================================================



Parsing and semantic processing can take forever if the trace below is long.
 In this case, it is advised to uncomment the module below to deserialize the
 trace from a generated binary file.

\*
\*---- MODULE CaIncarnationCore_TETrace ----
\*EXTENDS CaIncarnationCore_TEConstants, CaIncarnationCore, IOUtils, TLC
\*
\*trace == IODeserialize("CaIncarnationCore_TTrace_1785649296.bin", TRUE)
\*
\*=============================================================================
\*

---- MODULE CaIncarnationCore_TETrace ----
EXTENDS CaIncarnationCore_TEConstants, CaIncarnationCore, TLC

trace == 
    <<
    ([cursor |-> (s1 :> 0),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 0),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 1),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 0),tokOf |-> (h1 :> 0),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "idle"),pendCasc |-> {},retired |-> {},gcRound |-> 0,man |-> (s1 :> [fence |-> 0, refs |-> {}, log |-> <<>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {} @@ w2 :> {}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> FALSE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 0),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 0),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 0),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "idle"),pendCasc |-> {},retired |-> {},gcRound |-> 0,man |-> (s1 :> [fence |-> 0, refs |-> {}, log |-> <<>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {<<h1, 1>>} @@ w2 :> {}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 0),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 0),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 0),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "idle"),pendCasc |-> {},retired |-> {},gcRound |-> 0,man |-> (s1 :> [fence |-> 0, refs |-> {}, log |-> <<>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {<<h1, 1>>} @@ w2 :> {<<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 0),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 0),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 0),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "idle"),pendCasc |-> {},retired |-> {},gcRound |-> 0,man |-> (s1 :> [fence |-> 0, refs |-> {h1}, log |-> <<[h |-> h1, op |-> "add"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {} @@ w2 :> {<<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 1),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 0),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {<<s1, h1>>},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 0),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "idle"),pendCasc |-> {},retired |-> {},gcRound |-> 0,man |-> (s1 :> [fence |-> 0, refs |-> {h1}, log |-> <<[h |-> h1, op |-> "add"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {h1},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {} @@ w2 :> {<<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 1),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 0),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {<<s1, h1>>},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "retiring"),pendCasc |-> {},retired |-> {},gcRound |-> 1,man |-> (s1 :> [fence |-> 0, refs |-> {h1}, log |-> <<[h |-> h1, op |-> "add"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {h1},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {} @@ w2 :> {<<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 1),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 0),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {<<s1, h1>>},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "retiring"),pendCasc |-> {},retired |-> {},gcRound |-> 1,man |-> (s1 :> [fence |-> 0, refs |-> {h1}, log |-> <<[h |-> h1, op |-> "add"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {h1},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {<<h1, 1>>} @@ w2 :> {<<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 1),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 0),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {<<s1, h1>>},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "retiring"),pendCasc |-> {},retired |-> {},gcRound |-> 1,man |-> (s1 :> [fence |-> 0, refs |-> {}, log |-> <<[h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {h1},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {<<h1, 1>>} @@ w2 :> {<<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 1),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 1),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {<<s1, h1>>},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "retiring"),pendCasc |-> {},retired |-> {},gcRound |-> 1,man |-> (s1 :> [fence |-> 0, refs |-> {}, log |-> <<[h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {h1},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {<<h1, 1>>} @@ w2 :> {<<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 2),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 1),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "retiring"),pendCasc |-> {},retired |-> {},gcRound |-> 1,man |-> (s1 :> [fence |-> 0, refs |-> {}, log |-> <<[h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {h1},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {<<h1, 1>>} @@ w2 :> {<<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 2),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 1),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "retiring"),pendCasc |-> {},retired |-> {},gcRound |-> 1,man |-> (s1 :> [fence |-> 0, refs |-> {}, log |-> <<[h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {h1},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {<<h1, 1>>} @@ w2 :> {<<h1, 1>>, <<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 2),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 1),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "retiring"),pendCasc |-> {},retired |-> {},gcRound |-> 1,man |-> (s1 :> [fence |-> 0, refs |-> {h1}, log |-> <<[h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"], [h |-> h1, op |-> "add"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {h1},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {} @@ w2 :> {<<h1, 1>>, <<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 2),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 1),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "retiring"),pendCasc |-> {},retired |-> {[t |-> 1, h |-> h1, r |-> 1]},gcRound |-> 1,man |-> (s1 :> [fence |-> 0, refs |-> {h1}, log |-> <<[h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"], [h |-> h1, op |-> "add"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {h1},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {} @@ w2 :> {<<h1, 1>>, <<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 3),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 1),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {<<s1, h1>>},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "retiring"),pendCasc |-> {},retired |-> {[t |-> 1, h |-> h1, r |-> 1]},gcRound |-> 1,man |-> (s1 :> [fence |-> 0, refs |-> {h1}, log |-> <<[h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"], [h |-> h1, op |-> "add"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {h1},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {} @@ w2 :> {<<h1, 1>>, <<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 3),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 2),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {<<s1, h1>>},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "retiring"),pendCasc |-> {},retired |-> {[t |-> 1, h |-> h1, r |-> 1]},gcRound |-> 1,man |-> (s1 :> [fence |-> 0, refs |-> {h1}, log |-> <<[h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"], [h |-> h1, op |-> "add"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {h1},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {} @@ w2 :> {<<h1, 1>>, <<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 3),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 2),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {<<s1, h1>>},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "retiring"),pendCasc |-> {},retired |-> {[t |-> 1, h |-> h1, r |-> 1]},gcRound |-> 1,man |-> (s1 :> [fence |-> 0, refs |-> {}, log |-> <<[h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"], [h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {h1},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {} @@ w2 :> {<<h1, 1>>, <<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 3),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 3),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {<<s1, h1>>},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "retiring"),pendCasc |-> {},retired |-> {[t |-> 1, h |-> h1, r |-> 1]},gcRound |-> 1,man |-> (s1 :> [fence |-> 0, refs |-> {}, log |-> <<[h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"], [h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {h1},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {} @@ w2 :> {<<h1, 1>>, <<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 4),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 3),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "retiring"),pendCasc |-> {},retired |-> {[t |-> 1, h |-> h1, r |-> 1]},gcRound |-> 1,man |-> (s1 :> [fence |-> 0, refs |-> {}, log |-> <<[h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"], [h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {h1},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {} @@ w2 :> {<<h1, 1>>, <<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 4),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 4),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "retiring"),pendCasc |-> {},retired |-> {[t |-> 1, h |-> h1, r |-> 1]},gcRound |-> 1,man |-> (s1 :> [fence |-> 0, refs |-> {}, log |-> <<[h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"], [h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {h1},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {} @@ w2 :> {<<h1, 1>>, <<h1, 4>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)]),
    ([cursor |-> (s1 :> 4),hbObs |-> (w1 :> -1 @@ w2 :> -1),fgRefs |-> (s1 :> {}),deadTok |-> (h1 :> {}),fencePos |-> (s1 :> 0),trimBase |-> (s1 :> 4),wEv |-> (w1 :> {} @@ w2 :> {}),rootEdges |-> {},wView |-> (w1 :> 0 @@ w2 :> 0),nextTok |-> (h1 :> 2),fgCut |-> (s1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (h1 :> 1),reg |-> [man |-> (s1 :> TRUE), fence |-> 0, floor |-> (s1 :> 0), ns |-> {s1}, univ |-> (L1 :> {s1}), done |-> (L1 :> TRUE)],gcPhase |-> (L1 :> "retiring"),pendCasc |-> {},retired |-> {[t |-> 1, h |-> h1, r |-> 1]},gcRound |-> 1,man |-> (s1 :> [fence |-> 0, refs |-> {}, log |-> <<[h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"], [h |-> h1, op |-> "add"], [h |-> h1, op |-> "rem"]>>]),creator |-> (h1 :> "none"),hbSeq |-> (w1 :> 0 @@ w2 :> 0),everEdged |-> {h1},treeEdges |-> {},fgPhase |-> "idle",inflight |-> {},marker |-> {},wDeps |-> (w1 :> {} @@ w2 :> {<<h1, 1>>}),fencedSet |-> (L1 :> {}),hbAlive |-> (w1 :> FALSE @@ w2 :> FALSE),present |-> (h1 :> TRUE),fgSeen |-> {},wedged |-> (w1 :> FALSE @@ w2 :> FALSE)])
    >>
----


=============================================================================

---- MODULE CaIncarnationCore_TEConstants ----
EXTENDS CaIncarnationCore

CONSTANTS w1, w2, L1, s1, h1, _TTraceLassoStart, _TTraceLassoEnd

=============================================================================

---- CONFIG CaIncarnationCore_TTrace_1785649296 ----
CONSTANTS
    Writers = { w1 , w2 }
    Leaders = { L1 }
    Shards = { s1 }
    Hashes = { h1 }
    TreeHashes = { }
    MaxToken = 3
    MaxRound = 2
    MaxLog = 4
    EnableResurrect = TRUE
    EnableTrees = FALSE
    EnableDebris = FALSE
    EnableSplit = FALSE
    EnableOverwrite = FALSE
    SabotageNoFence = FALSE
    SabotageNoRecheckFold = FALSE
    SabotageNoRetireView = FALSE
    SabotageUncondDelete = FALSE
    SabotageReusedTag = FALSE
    SabotageCascadeRace = FALSE
    SabotageCutOverclaim = FALSE
    EnableReval = FALSE
    SabotageNoReobserve = FALSE
    EnableRegistry = FALSE
    EnableEvStale = FALSE
    SabotageNoRegistry = FALSE
    SabotageFoldTimeUniverse = FALSE
    SabotageNoEvReobserve = FALSE
    w1 = w1
    w2 = w2
    h1 = h1
    L1 = L1
    s1 = s1
_TTraceLassoStart = 19
_TTraceLassoEnd = 20

PROPERTY
    _prop

CHECK_DEADLOCK
    \* CHECK_DEADLOCK off because of PROPERTY or INVARIANT above.
    FALSE

INIT
    _init

NEXT
    _next

VIEW
    _view

CONSTANT
    _TETrace <- _trace

ALIAS
    _expression
=============================================================================
\* Generated on Sun Aug 02 07:41:40 CEST 2026