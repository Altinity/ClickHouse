---- MODULE CaRetiredInRun_TTrace_1785647757 ----
EXTENDS Sequences, TLCExt, Toolbox, CaRetiredInRun, CaRetiredInRun_TEConstants, Naturals, TLC

_expression ==
    LET CaRetiredInRun_TEExpression == INSTANCE CaRetiredInRun_TEExpression
    IN CaRetiredInRun_TEExpression!expression
----

_trace ==
    LET CaRetiredInRun_TETrace == INSTANCE CaRetiredInRun_TETrace
    IN CaRetiredInRun_TETrace!trace
----

_inv ==
    ~(
        TLCGet("level") = Len(_TETrace)
        /\
        journal = (<<>>)
        /\
        meta = ((b1 :> "clean" @@ b2 :> "clean"))
        /\
        phys = ((b1 :> 0 @@ b2 :> 0))
        /\
        badDeleteEver = (FALSE)
        /\
        nextTok = ((b1 :> 1 @@ b2 :> 1))
        /\
        liveRef = ((b1 :> FALSE @@ b2 :> FALSE))
        /\
        nextAttempt = (3)
        /\
        adopted = ([round |-> 2, cut |-> 0, gen |-> 2, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])])
        /\
        lenAtAdopt = (0)
        /\
        artifacts = (<<[round |-> 1, cut |-> 0, gen |-> 1, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]>>)
    )
----

_init ==
    /\ nextTok = _TETrace[1].nextTok
    /\ journal = _TETrace[1].journal
    /\ badDeleteEver = _TETrace[1].badDeleteEver
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
        /\ nextTok  = _TETrace[i].nextTok
        /\ nextTok' = _TETrace[j].nextTok
        /\ journal  = _TETrace[i].journal
        /\ journal' = _TETrace[j].journal
        /\ badDeleteEver  = _TETrace[i].badDeleteEver
        /\ badDeleteEver' = _TETrace[j].badDeleteEver
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
    \*         IN J!JsonSerialize("CaRetiredInRun_TTrace_1785647757.json", _TETrace)

=============================================================================

 Note that you can extract this module `CaRetiredInRun_TEExpression`
  to a dedicated file to reuse `expression` (the module in the 
  dedicated `CaRetiredInRun_TEExpression.tla` file takes precedence 
  over the module `CaRetiredInRun_TEExpression` below).

---- MODULE CaRetiredInRun_TEExpression ----
EXTENDS Sequences, TLCExt, Toolbox, CaRetiredInRun, CaRetiredInRun_TEConstants, Naturals, TLC

expression == 
    [
        \* To hide variables of the `CaRetiredInRun` spec from the error trace,
        \* remove the variables below.  The trace will be written in the order
        \* of the fields of this record.
        nextTok |-> nextTok
        ,journal |-> journal
        ,badDeleteEver |-> badDeleteEver
        ,lenAtAdopt |-> lenAtAdopt
        ,meta |-> meta
        ,artifacts |-> artifacts
        ,nextAttempt |-> nextAttempt
        ,phys |-> phys
        ,adopted |-> adopted
        ,liveRef |-> liveRef
        
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
\*---- MODULE CaRetiredInRun_TETrace ----
\*EXTENDS IOUtils, CaRetiredInRun, CaRetiredInRun_TEConstants, TLC
\*
\*trace == IODeserialize("CaRetiredInRun_TTrace_1785647757.bin", TRUE)
\*
\*=============================================================================
\*

---- MODULE CaRetiredInRun_TETrace ----
EXTENDS CaRetiredInRun, CaRetiredInRun_TEConstants, TLC

trace == 
    <<
    ([journal |-> <<>>,meta |-> (b1 :> "clean" @@ b2 :> "clean"),phys |-> (b1 :> 0 @@ b2 :> 0),badDeleteEver |-> FALSE,nextTok |-> (b1 :> 1 @@ b2 :> 1),liveRef |-> (b1 :> FALSE @@ b2 :> FALSE),nextAttempt |-> 1,adopted |-> [round |-> 0, cut |-> 0, gen |-> 0, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])],lenAtAdopt |-> 0,artifacts |-> <<>>]),
    ([journal |-> <<>>,meta |-> (b1 :> "clean" @@ b2 :> "clean"),phys |-> (b1 :> 0 @@ b2 :> 0),badDeleteEver |-> FALSE,nextTok |-> (b1 :> 1 @@ b2 :> 1),liveRef |-> (b1 :> FALSE @@ b2 :> FALSE),nextAttempt |-> 2,adopted |-> [round |-> 1, cut |-> 0, gen |-> 1, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])],lenAtAdopt |-> 0,artifacts |-> <<[round |-> 1, cut |-> 0, gen |-> 1, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]>>]),
    ([journal |-> <<>>,meta |-> (b1 :> "clean" @@ b2 :> "clean"),phys |-> (b1 :> 0 @@ b2 :> 0),badDeleteEver |-> FALSE,nextTok |-> (b1 :> 1 @@ b2 :> 1),liveRef |-> (b1 :> FALSE @@ b2 :> FALSE),nextAttempt |-> 3,adopted |-> [round |-> 2, cut |-> 0, gen |-> 2, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])],lenAtAdopt |-> 0,artifacts |-> <<[round |-> 1, cut |-> 0, gen |-> 1, edges |-> (b1 :> 0 @@ b2 :> 0), cond |-> (b1 :> [st |-> "none", tok |-> 0, round |-> 0] @@ b2 :> [st |-> "none", tok |-> 0, round |-> 0])]>>])
    >>
----


=============================================================================

---- MODULE CaRetiredInRun_TEConstants ----
EXTENDS CaRetiredInRun

CONSTANTS b1, b2

=============================================================================

---- CONFIG CaRetiredInRun_TTrace_1785647757 ----
CONSTANTS
    Blobs = { b1 , b2 }
    MaxRound = 4
    MaxToken = 3
    MaxJournal = 5
    Sabotage = "attempt_reuse"
    b2 = b2
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
\* Generated on Sun Aug 02 07:15:58 CEST 2026