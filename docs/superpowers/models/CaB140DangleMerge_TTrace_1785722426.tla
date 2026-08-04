---- MODULE CaB140DangleMerge_TTrace_1785722426 ----
EXTENDS Sequences, TLCExt, CaB140DangleMerge, Toolbox, Naturals, TLC, CaB140DangleMerge_TEConstants

_expression ==
    LET CaB140DangleMerge_TEExpression == INSTANCE CaB140DangleMerge_TEExpression
    IN CaB140DangleMerge_TEExpression!expression
----

_trace ==
    LET CaB140DangleMerge_TETrace == INSTANCE CaB140DangleMerge_TETrace
    IN CaB140DangleMerge_TETrace!trace
----

_inv ==
    ~(
        TLCGet("level") = Len(_TETrace)
        /\
        phase = ((L1 :> "building" @@ L2 :> "retiring"))
        /\
        logBase = (1)
        /\
        log = (<<[op |-> "add", h |-> t2], [op |-> "rem", h |-> t2]>>)
        /\
        gcState = ([cursor |-> 0, snapGeneration |-> 1])
        /\
        durSnap = ((0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {t2, b1}, cursor |-> 3] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]))
        /\
        refs = ({t1})
        /\
        durWritten = ({0, 1})
        /\
        everLost = ({b1})
        /\
        retired = ({})
        /\
        buildGen = ((L1 :> 1 @@ L2 :> 1))
        /\
        lease = (L2)
        /\
        present = ((t1 :> TRUE @@ t2 :> TRUE @@ b1 :> FALSE))
        /\
        wip = ((L1 :> [marker |-> {t1}, treeEdges |-> {<<t1, b1>>}, rootEdges |-> {t1}, known |-> {t1, b1}, cursor |-> 1] @@ L2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {t2, b1}, cursor |-> 3]))
    )
----

_init ==
    /\ phase = _TETrace[1].phase
    /\ gcState = _TETrace[1].gcState
    /\ everLost = _TETrace[1].everLost
    /\ log = _TETrace[1].log
    /\ durWritten = _TETrace[1].durWritten
    /\ refs = _TETrace[1].refs
    /\ present = _TETrace[1].present
    /\ durSnap = _TETrace[1].durSnap
    /\ wip = _TETrace[1].wip
    /\ buildGen = _TETrace[1].buildGen
    /\ lease = _TETrace[1].lease
    /\ logBase = _TETrace[1].logBase
    /\ retired = _TETrace[1].retired
----

_next ==
    /\ \E i,j \in DOMAIN _TETrace:
        /\ \/ /\ j = i + 1
              /\ i = TLCGet("level")
        /\ phase  = _TETrace[i].phase
        /\ phase' = _TETrace[j].phase
        /\ gcState  = _TETrace[i].gcState
        /\ gcState' = _TETrace[j].gcState
        /\ everLost  = _TETrace[i].everLost
        /\ everLost' = _TETrace[j].everLost
        /\ log  = _TETrace[i].log
        /\ log' = _TETrace[j].log
        /\ durWritten  = _TETrace[i].durWritten
        /\ durWritten' = _TETrace[j].durWritten
        /\ refs  = _TETrace[i].refs
        /\ refs' = _TETrace[j].refs
        /\ present  = _TETrace[i].present
        /\ present' = _TETrace[j].present
        /\ durSnap  = _TETrace[i].durSnap
        /\ durSnap' = _TETrace[j].durSnap
        /\ wip  = _TETrace[i].wip
        /\ wip' = _TETrace[j].wip
        /\ buildGen  = _TETrace[i].buildGen
        /\ buildGen' = _TETrace[j].buildGen
        /\ lease  = _TETrace[i].lease
        /\ lease' = _TETrace[j].lease
        /\ logBase  = _TETrace[i].logBase
        /\ logBase' = _TETrace[j].logBase
        /\ retired  = _TETrace[i].retired
        /\ retired' = _TETrace[j].retired

\* Uncomment the ASSUME below to write the states of the error trace
\* to the given file in Json format. Note that you can pass any tuple
\* to `JsonSerialize`. For example, a sub-sequence of _TETrace.
    \* ASSUME
    \*     LET J == INSTANCE Json
    \*         IN J!JsonSerialize("CaB140DangleMerge_TTrace_1785722426.json", _TETrace)

=============================================================================

 Note that you can extract this module `CaB140DangleMerge_TEExpression`
  to a dedicated file to reuse `expression` (the module in the 
  dedicated `CaB140DangleMerge_TEExpression.tla` file takes precedence 
  over the module `CaB140DangleMerge_TEExpression` below).

---- MODULE CaB140DangleMerge_TEExpression ----
EXTENDS Sequences, TLCExt, CaB140DangleMerge, Toolbox, Naturals, TLC, CaB140DangleMerge_TEConstants

expression == 
    [
        \* To hide variables of the `CaB140DangleMerge` spec from the error trace,
        \* remove the variables below.  The trace will be written in the order
        \* of the fields of this record.
        phase |-> phase
        ,gcState |-> gcState
        ,everLost |-> everLost
        ,log |-> log
        ,durWritten |-> durWritten
        ,refs |-> refs
        ,present |-> present
        ,durSnap |-> durSnap
        ,wip |-> wip
        ,buildGen |-> buildGen
        ,lease |-> lease
        ,logBase |-> logBase
        ,retired |-> retired
        
        \* Put additional constant-, state-, and action-level expressions here:
        \* ,_stateNumber |-> _TEPosition
        \* ,_phaseUnchanged |-> phase = phase'
        
        \* Format the `phase` variable as Json value.
        \* ,_phaseJson |->
        \*     LET J == INSTANCE Json
        \*     IN J!ToJson(phase)
        
        \* Lastly, you may build expressions over arbitrary sets of states by
        \* leveraging the _TETrace operator.  For example, this is how to
        \* count the number of times a spec variable changed up to the current
        \* state in the trace.
        \* ,_phaseModCount |->
        \*     LET F[s \in DOMAIN _TETrace] ==
        \*         IF s = 1 THEN 0
        \*         ELSE IF _TETrace[s].phase # _TETrace[s-1].phase
        \*             THEN 1 + F[s-1] ELSE F[s-1]
        \*     IN F[_TEPosition - 1]
    ]

=============================================================================



Parsing and semantic processing can take forever if the trace below is long.
 In this case, it is advised to uncomment the module below to deserialize the
 trace from a generated binary file.

\*
\*---- MODULE CaB140DangleMerge_TETrace ----
\*EXTENDS IOUtils, CaB140DangleMerge, TLC, CaB140DangleMerge_TEConstants
\*
\*trace == IODeserialize("CaB140DangleMerge_TTrace_1785722426.bin", TRUE)
\*
\*=============================================================================
\*

---- MODULE CaB140DangleMerge_TETrace ----
EXTENDS CaB140DangleMerge, TLC, CaB140DangleMerge_TEConstants

trace == 
    <<
    ([phase |-> (L1 :> "idle" @@ L2 :> "idle"),logBase |-> 0,log |-> <<>>,gcState |-> [cursor |-> 0, snapGeneration |-> 0],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {},durWritten |-> {0},everLost |-> {},retired |-> {},buildGen |-> (L1 :> 0 @@ L2 :> 0),lease |-> "none",present |-> (t1 :> FALSE @@ t2 :> FALSE @@ b1 :> FALSE),wip |-> (L1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ L2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0])]),
    ([phase |-> (L1 :> "idle" @@ L2 :> "idle"),logBase |-> 0,log |-> <<>>,gcState |-> [cursor |-> 0, snapGeneration |-> 0],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {},durWritten |-> {0},everLost |-> {},retired |-> {},buildGen |-> (L1 :> 0 @@ L2 :> 0),lease |-> "none",present |-> (t1 :> FALSE @@ t2 :> FALSE @@ b1 :> TRUE),wip |-> (L1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ L2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0])]),
    ([phase |-> (L1 :> "idle" @@ L2 :> "idle"),logBase |-> 0,log |-> <<[op |-> "add", h |-> t1]>>,gcState |-> [cursor |-> 0, snapGeneration |-> 0],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {t1},durWritten |-> {0},everLost |-> {},retired |-> {},buildGen |-> (L1 :> 0 @@ L2 :> 0),lease |-> "none",present |-> (t1 :> TRUE @@ t2 :> FALSE @@ b1 :> TRUE),wip |-> (L1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ L2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0])]),
    ([phase |-> (L1 :> "idle" @@ L2 :> "idle"),logBase |-> 0,log |-> <<[op |-> "add", h |-> t1], [op |-> "add", h |-> t2]>>,gcState |-> [cursor |-> 0, snapGeneration |-> 0],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {t1, t2},durWritten |-> {0},everLost |-> {},retired |-> {},buildGen |-> (L1 :> 0 @@ L2 :> 0),lease |-> "none",present |-> (t1 :> TRUE @@ t2 :> TRUE @@ b1 :> TRUE),wip |-> (L1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ L2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0])]),
    ([phase |-> (L1 :> "idle" @@ L2 :> "idle"),logBase |-> 0,log |-> <<[op |-> "add", h |-> t1], [op |-> "add", h |-> t2], [op |-> "rem", h |-> t2]>>,gcState |-> [cursor |-> 0, snapGeneration |-> 0],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {t1},durWritten |-> {0},everLost |-> {},retired |-> {},buildGen |-> (L1 :> 0 @@ L2 :> 0),lease |-> "none",present |-> (t1 :> TRUE @@ t2 :> TRUE @@ b1 :> TRUE),wip |-> (L1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ L2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0])]),
    ([phase |-> (L1 :> "building" @@ L2 :> "idle"),logBase |-> 0,log |-> <<[op |-> "add", h |-> t1], [op |-> "add", h |-> t2], [op |-> "rem", h |-> t2]>>,gcState |-> [cursor |-> 0, snapGeneration |-> 0],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {t1},durWritten |-> {0},everLost |-> {},retired |-> {},buildGen |-> (L1 :> 1 @@ L2 :> 0),lease |-> L1,present |-> (t1 :> TRUE @@ t2 :> TRUE @@ b1 :> TRUE),wip |-> (L1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ L2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0])]),
    ([phase |-> (L1 :> "building" @@ L2 :> "idle"),logBase |-> 0,log |-> <<[op |-> "add", h |-> t1], [op |-> "add", h |-> t2], [op |-> "rem", h |-> t2]>>,gcState |-> [cursor |-> 0, snapGeneration |-> 0],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {t1},durWritten |-> {0},everLost |-> {},retired |-> {},buildGen |-> (L1 :> 1 @@ L2 :> 0),lease |-> L1,present |-> (t1 :> TRUE @@ t2 :> TRUE @@ b1 :> TRUE),wip |-> (L1 :> [marker |-> {t1}, treeEdges |-> {<<t1, b1>>}, rootEdges |-> {t1}, known |-> {t1, b1}, cursor |-> 1] @@ L2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0])]),
    ([phase |-> (L1 :> "building" @@ L2 :> "idle"),logBase |-> 1,log |-> <<[op |-> "add", h |-> t2], [op |-> "rem", h |-> t2]>>,gcState |-> [cursor |-> 0, snapGeneration |-> 0],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {t1},durWritten |-> {0},everLost |-> {},retired |-> {},buildGen |-> (L1 :> 1 @@ L2 :> 0),lease |-> L1,present |-> (t1 :> TRUE @@ t2 :> TRUE @@ b1 :> TRUE),wip |-> (L1 :> [marker |-> {t1}, treeEdges |-> {<<t1, b1>>}, rootEdges |-> {t1}, known |-> {t1, b1}, cursor |-> 1] @@ L2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0])]),
    ([phase |-> (L1 :> "building" @@ L2 :> "building"),logBase |-> 1,log |-> <<[op |-> "add", h |-> t2], [op |-> "rem", h |-> t2]>>,gcState |-> [cursor |-> 0, snapGeneration |-> 0],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {t1},durWritten |-> {0},everLost |-> {},retired |-> {},buildGen |-> (L1 :> 1 @@ L2 :> 1),lease |-> L2,present |-> (t1 :> TRUE @@ t2 :> TRUE @@ b1 :> TRUE),wip |-> (L1 :> [marker |-> {t1}, treeEdges |-> {<<t1, b1>>}, rootEdges |-> {t1}, known |-> {t1, b1}, cursor |-> 1] @@ L2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0])]),
    ([phase |-> (L1 :> "building" @@ L2 :> "building"),logBase |-> 1,log |-> <<[op |-> "add", h |-> t2], [op |-> "rem", h |-> t2]>>,gcState |-> [cursor |-> 0, snapGeneration |-> 0],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {t1},durWritten |-> {0},everLost |-> {},retired |-> {},buildGen |-> (L1 :> 1 @@ L2 :> 1),lease |-> L2,present |-> (t1 :> TRUE @@ t2 :> TRUE @@ b1 :> TRUE),wip |-> (L1 :> [marker |-> {t1}, treeEdges |-> {<<t1, b1>>}, rootEdges |-> {t1}, known |-> {t1, b1}, cursor |-> 1] @@ L2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 1])]),
    ([phase |-> (L1 :> "building" @@ L2 :> "building"),logBase |-> 1,log |-> <<[op |-> "add", h |-> t2], [op |-> "rem", h |-> t2]>>,gcState |-> [cursor |-> 0, snapGeneration |-> 0],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {t1},durWritten |-> {0},everLost |-> {},retired |-> {},buildGen |-> (L1 :> 1 @@ L2 :> 1),lease |-> L2,present |-> (t1 :> TRUE @@ t2 :> TRUE @@ b1 :> TRUE),wip |-> (L1 :> [marker |-> {t1}, treeEdges |-> {<<t1, b1>>}, rootEdges |-> {t1}, known |-> {t1, b1}, cursor |-> 1] @@ L2 :> [marker |-> {t2}, treeEdges |-> {<<t2, b1>>}, rootEdges |-> {t2}, known |-> {t2, b1}, cursor |-> 2])]),
    ([phase |-> (L1 :> "building" @@ L2 :> "building"),logBase |-> 1,log |-> <<[op |-> "add", h |-> t2], [op |-> "rem", h |-> t2]>>,gcState |-> [cursor |-> 0, snapGeneration |-> 0],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {t1},durWritten |-> {0},everLost |-> {},retired |-> {},buildGen |-> (L1 :> 1 @@ L2 :> 1),lease |-> L2,present |-> (t1 :> TRUE @@ t2 :> TRUE @@ b1 :> TRUE),wip |-> (L1 :> [marker |-> {t1}, treeEdges |-> {<<t1, b1>>}, rootEdges |-> {t1}, known |-> {t1, b1}, cursor |-> 1] @@ L2 :> [marker |-> {t2}, treeEdges |-> {<<t2, b1>>}, rootEdges |-> {}, known |-> {t2, b1}, cursor |-> 3])]),
    ([phase |-> (L1 :> "building" @@ L2 :> "building"),logBase |-> 1,log |-> <<[op |-> "add", h |-> t2], [op |-> "rem", h |-> t2]>>,gcState |-> [cursor |-> 0, snapGeneration |-> 0],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {t1},durWritten |-> {0},everLost |-> {},retired |-> {},buildGen |-> (L1 :> 1 @@ L2 :> 1),lease |-> L2,present |-> (t1 :> TRUE @@ t2 :> TRUE @@ b1 :> TRUE),wip |-> (L1 :> [marker |-> {t1}, treeEdges |-> {<<t1, b1>>}, rootEdges |-> {t1}, known |-> {t1, b1}, cursor |-> 1] @@ L2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {t2, b1}, cursor |-> 3])]),
    ([phase |-> (L1 :> "building" @@ L2 :> "building"),logBase |-> 1,log |-> <<[op |-> "add", h |-> t2], [op |-> "rem", h |-> t2]>>,gcState |-> [cursor |-> 0, snapGeneration |-> 0],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {t2, b1}, cursor |-> 3] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {t1},durWritten |-> {0, 1},everLost |-> {},retired |-> {},buildGen |-> (L1 :> 1 @@ L2 :> 1),lease |-> L2,present |-> (t1 :> TRUE @@ t2 :> TRUE @@ b1 :> TRUE),wip |-> (L1 :> [marker |-> {t1}, treeEdges |-> {<<t1, b1>>}, rootEdges |-> {t1}, known |-> {t1, b1}, cursor |-> 1] @@ L2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {t2, b1}, cursor |-> 3])]),
    ([phase |-> (L1 :> "building" @@ L2 :> "retiring"),logBase |-> 1,log |-> <<[op |-> "add", h |-> t2], [op |-> "rem", h |-> t2]>>,gcState |-> [cursor |-> 0, snapGeneration |-> 1],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {t2, b1}, cursor |-> 3] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {t1},durWritten |-> {0, 1},everLost |-> {},retired |-> {},buildGen |-> (L1 :> 1 @@ L2 :> 1),lease |-> L2,present |-> (t1 :> TRUE @@ t2 :> TRUE @@ b1 :> TRUE),wip |-> (L1 :> [marker |-> {t1}, treeEdges |-> {<<t1, b1>>}, rootEdges |-> {t1}, known |-> {t1, b1}, cursor |-> 1] @@ L2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {t2, b1}, cursor |-> 3])]),
    ([phase |-> (L1 :> "building" @@ L2 :> "retiring"),logBase |-> 1,log |-> <<[op |-> "add", h |-> t2], [op |-> "rem", h |-> t2]>>,gcState |-> [cursor |-> 0, snapGeneration |-> 1],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {t2, b1}, cursor |-> 3] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {t1},durWritten |-> {0, 1},everLost |-> {},retired |-> {b1},buildGen |-> (L1 :> 1 @@ L2 :> 1),lease |-> L2,present |-> (t1 :> TRUE @@ t2 :> TRUE @@ b1 :> TRUE),wip |-> (L1 :> [marker |-> {t1}, treeEdges |-> {<<t1, b1>>}, rootEdges |-> {t1}, known |-> {t1, b1}, cursor |-> 1] @@ L2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {t2, b1}, cursor |-> 3])]),
    ([phase |-> (L1 :> "building" @@ L2 :> "retiring"),logBase |-> 1,log |-> <<[op |-> "add", h |-> t2], [op |-> "rem", h |-> t2]>>,gcState |-> [cursor |-> 0, snapGeneration |-> 1],durSnap |-> (0 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 1 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {t2, b1}, cursor |-> 3] @@ 2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0] @@ 3 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]),refs |-> {t1},durWritten |-> {0, 1},everLost |-> {b1},retired |-> {},buildGen |-> (L1 :> 1 @@ L2 :> 1),lease |-> L2,present |-> (t1 :> TRUE @@ t2 :> TRUE @@ b1 :> FALSE),wip |-> (L1 :> [marker |-> {t1}, treeEdges |-> {<<t1, b1>>}, rootEdges |-> {t1}, known |-> {t1, b1}, cursor |-> 1] @@ L2 :> [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {t2, b1}, cursor |-> 3])])
    >>
----


=============================================================================

---- MODULE CaB140DangleMerge_TEConstants ----
EXTENDS CaB140DangleMerge

CONSTANTS L1, L2, t1, t2, b1

=============================================================================

---- CONFIG CaB140DangleMerge_TTrace_1785722426 ----
CONSTANTS
    Leaders = { L1 , L2 }
    Trees = { t1 , t2 }
    Blobs = { b1 }
    MaxGen = 3
    MaxLog = 3
    TrimGated = FALSE
    CursorInSnap = FALSE
    t1 = t1
    L2 = L2
    L1 = L1
    t2 = t2
    b1 = b1

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
\* Generated on Mon Aug 03 04:00:36 CEST 2026