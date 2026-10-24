---- MODULE CaRetiredInRunFoldAbortWitness_TTrace_1785647872 ----
EXTENDS Sequences, TLCExt, Toolbox, CaRetiredInRunFoldAbortWitness_TEConstants, CaRetiredInRunFoldAbortWitness, Naturals, TLC

_expression ==
    LET CaRetiredInRunFoldAbortWitness_TEExpression == INSTANCE CaRetiredInRunFoldAbortWitness_TEExpression
    IN CaRetiredInRunFoldAbortWitness_TEExpression!expression
----

_trace ==
    LET CaRetiredInRunFoldAbortWitness_TETrace == INSTANCE CaRetiredInRunFoldAbortWitness_TETrace
    IN CaRetiredInRunFoldAbortWitness_TETrace!trace
----

_inv ==
    ~(
        TLCGet("level") = Len(_TETrace)
        /\
        unresolved = ((b1 :> FALSE @@ b2 :> FALSE))
        /\
        journal = (<<<<b1, "add">>, <<b1, "rm">>, <<b1, "add">>>>)
        /\
        meta = ((b1 :> "clean" @@ b2 :> "clean"))
        /\
        phys = ((b1 :> 0 @@ b2 :> 0))
        /\
        badDeleteEver = (FALSE)
        /\
        leaders = ([La |-> [cut |-> 2, phase |-> "executed", snap |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 2, art |-> [round |-> 2, gen |-> 2, cut |-> 2, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]], Lb |-> [cut |-> 3, phase |-> "idle", snap |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 3, art |-> [round |-> 2, gen |-> 2, cut |-> 3, edges |-> (b1 :> 1 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]]])
        /\
        nextTok = ((b1 :> 2 @@ b2 :> 1))
        /\
        liveRef = ((b1 :> TRUE @@ b2 :> FALSE))
        /\
        nextAttempt = (4)
        /\
        adopted = ([round |-> 2, gen |-> 2, cut |-> 3, edges |-> (b1 :> 1 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])])
        /\
        lenAtAdopt = (3)
        /\
        artifacts = ((1 :> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])] @@ 3 :> [round |-> 2, gen |-> 2, cut |-> 3, edges |-> (b1 :> 1 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]))
    )
----

_init ==
    /\ leaders = _TETrace[1].leaders
    /\ nextTok = _TETrace[1].nextTok
    /\ journal = _TETrace[1].journal
    /\ badDeleteEver = _TETrace[1].badDeleteEver
    /\ unresolved = _TETrace[1].unresolved
    /\ lenAtAdopt = _TETrace[1].lenAtAdopt
    /\ meta = _TETrace[1].meta
    /\ artifacts = _TETrace[1].artifacts
    /\ nextAttempt = _TETrace[1].nextAttempt
    /\ phys = _TETrace[1].phys
    /\ adopted = _TETrace[1].adopted
    /\ liveRef = _TETrace[1].liveRef
----

_next ==
    /\ \E i,j \in DOMAIN _TETrace:
        /\ \/ /\ j = i + 1
              /\ i = TLCGet("level")
        /\ leaders  = _TETrace[i].leaders
        /\ leaders' = _TETrace[j].leaders
        /\ nextTok  = _TETrace[i].nextTok
        /\ nextTok' = _TETrace[j].nextTok
        /\ journal  = _TETrace[i].journal
        /\ journal' = _TETrace[j].journal
        /\ badDeleteEver  = _TETrace[i].badDeleteEver
        /\ badDeleteEver' = _TETrace[j].badDeleteEver
        /\ unresolved  = _TETrace[i].unresolved
        /\ unresolved' = _TETrace[j].unresolved
        /\ lenAtAdopt  = _TETrace[i].lenAtAdopt
        /\ lenAtAdopt' = _TETrace[j].lenAtAdopt
        /\ meta  = _TETrace[i].meta
        /\ meta' = _TETrace[j].meta
        /\ artifacts  = _TETrace[i].artifacts
        /\ artifacts' = _TETrace[j].artifacts
        /\ nextAttempt  = _TETrace[i].nextAttempt
        /\ nextAttempt' = _TETrace[j].nextAttempt
        /\ phys  = _TETrace[i].phys
        /\ phys' = _TETrace[j].phys
        /\ adopted  = _TETrace[i].adopted
        /\ adopted' = _TETrace[j].adopted
        /\ liveRef  = _TETrace[i].liveRef
        /\ liveRef' = _TETrace[j].liveRef

\* Uncomment the ASSUME below to write the states of the error trace
\* to the given file in Json format. Note that you can pass any tuple
\* to `JsonSerialize`. For example, a sub-sequence of _TETrace.
    \* ASSUME
    \*     LET J == INSTANCE Json
    \*         IN J!JsonSerialize("CaRetiredInRunFoldAbortWitness_TTrace_1785647872.json", _TETrace)

=============================================================================

 Note that you can extract this module `CaRetiredInRunFoldAbortWitness_TEExpression`
  to a dedicated file to reuse `expression` (the module in the 
  dedicated `CaRetiredInRunFoldAbortWitness_TEExpression.tla` file takes precedence 
  over the module `CaRetiredInRunFoldAbortWitness_TEExpression` below).

---- MODULE CaRetiredInRunFoldAbortWitness_TEExpression ----
EXTENDS Sequences, TLCExt, Toolbox, CaRetiredInRunFoldAbortWitness_TEConstants, CaRetiredInRunFoldAbortWitness, Naturals, TLC

expression == 
    [
        \* To hide variables of the `CaRetiredInRunFoldAbortWitness` spec from the error trace,
        \* remove the variables below.  The trace will be written in the order
        \* of the fields of this record.
        leaders |-> leaders
        ,nextTok |-> nextTok
        ,journal |-> journal
        ,badDeleteEver |-> badDeleteEver
        ,unresolved |-> unresolved
        ,lenAtAdopt |-> lenAtAdopt
        ,meta |-> meta
        ,artifacts |-> artifacts
        ,nextAttempt |-> nextAttempt
        ,phys |-> phys
        ,adopted |-> adopted
        ,liveRef |-> liveRef
        
        \* Put additional constant-, state-, and action-level expressions here:
        \* ,_stateNumber |-> _TEPosition
        \* ,_leadersUnchanged |-> leaders = leaders'
        
        \* Format the `leaders` variable as Json value.
        \* ,_leadersJson |->
        \*     LET J == INSTANCE Json
        \*     IN J!ToJson(leaders)
        
        \* Lastly, you may build expressions over arbitrary sets of states by
        \* leveraging the _TETrace operator.  For example, this is how to
        \* count the number of times a spec variable changed up to the current
        \* state in the trace.
        \* ,_leadersModCount |->
        \*     LET F[s \in DOMAIN _TETrace] ==
        \*         IF s = 1 THEN 0
        \*         ELSE IF _TETrace[s].leaders # _TETrace[s-1].leaders
        \*             THEN 1 + F[s-1] ELSE F[s-1]
        \*     IN F[_TEPosition - 1]
    ]

=============================================================================



Parsing and semantic processing can take forever if the trace below is long.
 In this case, it is advised to uncomment the module below to deserialize the
 trace from a generated binary file.

\*
\*---- MODULE CaRetiredInRunFoldAbortWitness_TETrace ----
\*EXTENDS IOUtils, CaRetiredInRunFoldAbortWitness_TEConstants, CaRetiredInRunFoldAbortWitness, TLC
\*
\*trace == IODeserialize("CaRetiredInRunFoldAbortWitness_TTrace_1785647872.bin", TRUE)
\*
\*=============================================================================
\*

---- MODULE CaRetiredInRunFoldAbortWitness_TETrace ----
EXTENDS CaRetiredInRunFoldAbortWitness_TEConstants, CaRetiredInRunFoldAbortWitness, TLC

trace == 
    <<
    ([unresolved |-> (b1 :> FALSE @@ b2 :> FALSE),journal |-> <<>>,meta |-> (b1 :> "clean" @@ b2 :> "clean"),phys |-> (b1 :> 0 @@ b2 :> 0),badDeleteEver |-> FALSE,leaders |-> [La |-> [cut |-> 0, phase |-> "idle", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 0, art |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]], Lb |-> [cut |-> 0, phase |-> "idle", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 0, art |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]]],nextTok |-> (b1 :> 1 @@ b2 :> 1),liveRef |-> (b1 :> FALSE @@ b2 :> FALSE),nextAttempt |-> 1,adopted |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])],lenAtAdopt |-> 0,artifacts |-> <<>>]),
    ([unresolved |-> (b1 :> FALSE @@ b2 :> FALSE),journal |-> <<<<b1, "add">>>>,meta |-> (b1 :> "clean" @@ b2 :> "clean"),phys |-> (b1 :> 1 @@ b2 :> 0),badDeleteEver |-> FALSE,leaders |-> [La |-> [cut |-> 0, phase |-> "idle", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 0, art |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]], Lb |-> [cut |-> 0, phase |-> "idle", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 0, art |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]]],nextTok |-> (b1 :> 2 @@ b2 :> 1),liveRef |-> (b1 :> TRUE @@ b2 :> FALSE),nextAttempt |-> 1,adopted |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])],lenAtAdopt |-> 0,artifacts |-> <<>>]),
    ([unresolved |-> (b1 :> FALSE @@ b2 :> FALSE),journal |-> <<<<b1, "add">>, <<b1, "rm">>>>,meta |-> (b1 :> "clean" @@ b2 :> "clean"),phys |-> (b1 :> 1 @@ b2 :> 0),badDeleteEver |-> FALSE,leaders |-> [La |-> [cut |-> 0, phase |-> "idle", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 0, art |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]], Lb |-> [cut |-> 0, phase |-> "idle", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 0, art |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]]],nextTok |-> (b1 :> 2 @@ b2 :> 1),liveRef |-> (b1 :> FALSE @@ b2 :> FALSE),nextAttempt |-> 1,adopted |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])],lenAtAdopt |-> 0,artifacts |-> <<>>]),
    ([unresolved |-> (b1 :> FALSE @@ b2 :> FALSE),journal |-> <<<<b1, "add">>, <<b1, "rm">>>>,meta |-> (b1 :> "clean" @@ b2 :> "clean"),phys |-> (b1 :> 1 @@ b2 :> 0),badDeleteEver |-> FALSE,leaders |-> [La |-> [cut |-> 0, phase |-> "captured", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 1, art |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]], Lb |-> [cut |-> 0, phase |-> "idle", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 0, art |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]]],nextTok |-> (b1 :> 2 @@ b2 :> 1),liveRef |-> (b1 :> FALSE @@ b2 :> FALSE),nextAttempt |-> 2,adopted |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])],lenAtAdopt |-> 0,artifacts |-> <<>>]),
    ([unresolved |-> (b1 :> FALSE @@ b2 :> FALSE),journal |-> <<<<b1, "add">>, <<b1, "rm">>>>,meta |-> (b1 :> "cond" @@ b2 :> "clean"),phys |-> (b1 :> 1 @@ b2 :> 0),badDeleteEver |-> FALSE,leaders |-> [La |-> [cut |-> 0, phase |-> "executed", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 1, art |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]], Lb |-> [cut |-> 0, phase |-> "idle", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 0, art |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]]],nextTok |-> (b1 :> 2 @@ b2 :> 1),liveRef |-> (b1 :> FALSE @@ b2 :> FALSE),nextAttempt |-> 2,adopted |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])],lenAtAdopt |-> 0,artifacts |-> <<>>]),
    ([unresolved |-> (b1 :> FALSE @@ b2 :> FALSE),journal |-> <<<<b1, "add">>, <<b1, "rm">>>>,meta |-> (b1 :> "cond" @@ b2 :> "clean"),phys |-> (b1 :> 1 @@ b2 :> 0),badDeleteEver |-> FALSE,leaders |-> [La |-> [cut |-> 0, phase |-> "idle", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 1, art |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]], Lb |-> [cut |-> 0, phase |-> "idle", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 0, art |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]]],nextTok |-> (b1 :> 2 @@ b2 :> 1),liveRef |-> (b1 :> FALSE @@ b2 :> FALSE),nextAttempt |-> 2,adopted |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])],lenAtAdopt |-> 2,artifacts |-> <<[round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]>>]),
    ([unresolved |-> (b1 :> TRUE @@ b2 :> FALSE),journal |-> <<<<b1, "add">>, <<b1, "rm">>, <<b1, "add">>>>,meta |-> (b1 :> "cond" @@ b2 :> "clean"),phys |-> (b1 :> 1 @@ b2 :> 0),badDeleteEver |-> FALSE,leaders |-> [La |-> [cut |-> 0, phase |-> "idle", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 1, art |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]], Lb |-> [cut |-> 0, phase |-> "idle", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 0, art |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]]],nextTok |-> (b1 :> 2 @@ b2 :> 1),liveRef |-> (b1 :> TRUE @@ b2 :> FALSE),nextAttempt |-> 2,adopted |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])],lenAtAdopt |-> 2,artifacts |-> <<[round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]>>]),
    ([unresolved |-> (b1 :> FALSE @@ b2 :> FALSE),journal |-> <<<<b1, "add">>, <<b1, "rm">>, <<b1, "add">>>>,meta |-> (b1 :> "cond" @@ b2 :> "clean"),phys |-> (b1 :> 1 @@ b2 :> 0),badDeleteEver |-> FALSE,leaders |-> [La |-> [cut |-> 0, phase |-> "idle", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 1, art |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]], Lb |-> [cut |-> 0, phase |-> "idle", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 0, art |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]]],nextTok |-> (b1 :> 2 @@ b2 :> 1),liveRef |-> (b1 :> TRUE @@ b2 :> FALSE),nextAttempt |-> 2,adopted |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])],lenAtAdopt |-> 2,artifacts |-> <<[round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]>>]),
    ([unresolved |-> (b1 :> FALSE @@ b2 :> FALSE),journal |-> <<<<b1, "add">>, <<b1, "rm">>, <<b1, "add">>>>,meta |-> (b1 :> "cond" @@ b2 :> "clean"),phys |-> (b1 :> 1 @@ b2 :> 0),badDeleteEver |-> FALSE,leaders |-> [La |-> [cut |-> 2, phase |-> "captured", snap |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 2, art |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]], Lb |-> [cut |-> 0, phase |-> "idle", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 0, art |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]]],nextTok |-> (b1 :> 2 @@ b2 :> 1),liveRef |-> (b1 :> TRUE @@ b2 :> FALSE),nextAttempt |-> 3,adopted |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])],lenAtAdopt |-> 2,artifacts |-> <<[round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]>>]),
    ([unresolved |-> (b1 :> FALSE @@ b2 :> FALSE),journal |-> <<<<b1, "add">>, <<b1, "rm">>, <<b1, "add">>>>,meta |-> (b1 :> "clean" @@ b2 :> "clean"),phys |-> (b1 :> 0 @@ b2 :> 0),badDeleteEver |-> FALSE,leaders |-> [La |-> [cut |-> 2, phase |-> "executed", snap |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 2, art |-> [round |-> 2, gen |-> 2, cut |-> 2, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]], Lb |-> [cut |-> 0, phase |-> "idle", snap |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 0, art |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]]],nextTok |-> (b1 :> 2 @@ b2 :> 1),liveRef |-> (b1 :> TRUE @@ b2 :> FALSE),nextAttempt |-> 3,adopted |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])],lenAtAdopt |-> 2,artifacts |-> <<[round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]>>]),
    ([unresolved |-> (b1 :> FALSE @@ b2 :> FALSE),journal |-> <<<<b1, "add">>, <<b1, "rm">>, <<b1, "add">>>>,meta |-> (b1 :> "clean" @@ b2 :> "clean"),phys |-> (b1 :> 0 @@ b2 :> 0),badDeleteEver |-> FALSE,leaders |-> [La |-> [cut |-> 2, phase |-> "executed", snap |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 2, art |-> [round |-> 2, gen |-> 2, cut |-> 2, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]], Lb |-> [cut |-> 3, phase |-> "captured", snap |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 3, art |-> [round |-> 0, gen |-> 0, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]]],nextTok |-> (b1 :> 2 @@ b2 :> 1),liveRef |-> (b1 :> TRUE @@ b2 :> FALSE),nextAttempt |-> 4,adopted |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])],lenAtAdopt |-> 2,artifacts |-> <<[round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]>>]),
    ([unresolved |-> (b1 :> FALSE @@ b2 :> FALSE),journal |-> <<<<b1, "add">>, <<b1, "rm">>, <<b1, "add">>>>,meta |-> (b1 :> "clean" @@ b2 :> "clean"),phys |-> (b1 :> 0 @@ b2 :> 0),badDeleteEver |-> FALSE,leaders |-> [La |-> [cut |-> 2, phase |-> "executed", snap |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 2, art |-> [round |-> 2, gen |-> 2, cut |-> 2, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]], Lb |-> [cut |-> 3, phase |-> "executed", snap |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 3, art |-> [round |-> 2, gen |-> 2, cut |-> 3, edges |-> (b1 :> 1 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]]],nextTok |-> (b1 :> 2 @@ b2 :> 1),liveRef |-> (b1 :> TRUE @@ b2 :> FALSE),nextAttempt |-> 4,adopted |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])],lenAtAdopt |-> 2,artifacts |-> <<[round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]>>]),
    ([unresolved |-> (b1 :> FALSE @@ b2 :> FALSE),journal |-> <<<<b1, "add">>, <<b1, "rm">>, <<b1, "add">>>>,meta |-> (b1 :> "clean" @@ b2 :> "clean"),phys |-> (b1 :> 0 @@ b2 :> 0),badDeleteEver |-> FALSE,leaders |-> [La |-> [cut |-> 2, phase |-> "executed", snap |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 2, art |-> [round |-> 2, gen |-> 2, cut |-> 2, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]], Lb |-> [cut |-> 3, phase |-> "idle", snap |-> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])], attempt |-> 3, art |-> [round |-> 2, gen |-> 2, cut |-> 3, edges |-> (b1 :> 1 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]]],nextTok |-> (b1 :> 2 @@ b2 :> 1),liveRef |-> (b1 :> TRUE @@ b2 :> FALSE),nextAttempt |-> 4,adopted |-> [round |-> 2, gen |-> 2, cut |-> 3, edges |-> (b1 :> 1 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])],lenAtAdopt |-> 3,artifacts |-> (1 :> [round |-> 1, gen |-> 1, cut |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "pend", tok |-> 1, round |-> 1] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])] @@ 3 :> [round |-> 2, gen |-> 2, cut |-> 3, edges |-> (b1 :> 1 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])])])
    >>
----


=============================================================================

---- MODULE CaRetiredInRunFoldAbortWitness_TEConstants ----
EXTENDS CaRetiredInRunFoldAbortWitness

CONSTANTS b1, b2

=============================================================================

---- CONFIG CaRetiredInRunFoldAbortWitness_TTrace_1785647872 ----
CONSTANTS
    Blobs = { b1 , b2 }
    MaxRound = 4
    MaxToken = 3
    MaxJournal = 5
    Sabotage = "no_pacing"
    b1 = b1
    b2 = b2

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
\* Generated on Sun Aug 02 07:17:55 CEST 2026