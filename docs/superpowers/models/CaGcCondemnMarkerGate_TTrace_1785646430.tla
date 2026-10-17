---- MODULE CaGcCondemnMarkerGate_TTrace_1785646430 ----
EXTENDS Sequences, TLCExt, Toolbox, CaGcCondemnMarkerGate, Naturals, TLC

_expression ==
    LET CaGcCondemnMarkerGate_TEExpression == INSTANCE CaGcCondemnMarkerGate_TEExpression
    IN CaGcCondemnMarkerGate_TEExpression!expression
----

_trace ==
    LET CaGcCondemnMarkerGate_TETrace == INSTANCE CaGcCondemnMarkerGate_TETrace
    IN CaGcCondemnMarkerGate_TETrace!trace
----

_inv ==
    ~(
        TLCGet("level") = Len(_TETrace)
        /\
        ret = ([tok |-> 0, st |-> "none", cr |-> 0, conf |-> FALSE])
        /\
        committed = (TRUE)
        /\
        edge = ("landed")
        /\
        round = (4)
        /\
        meta = ("clean")
        /\
        cut_taken = (FALSE)
        /\
        seeded = (TRUE)
        /\
        body = ([present |-> FALSE, tok |-> 0])
        /\
        folded = (FALSE)
        /\
        adopted = (1)
        /\
        next_gen = (2)
    )
----

_init ==
    /\ folded = _TETrace[1].folded
    /\ adopted = _TETrace[1].adopted
    /\ cut_taken = _TETrace[1].cut_taken
    /\ seeded = _TETrace[1].seeded
    /\ next_gen = _TETrace[1].next_gen
    /\ committed = _TETrace[1].committed
    /\ ret = _TETrace[1].ret
    /\ round = _TETrace[1].round
    /\ meta = _TETrace[1].meta
    /\ edge = _TETrace[1].edge
    /\ body = _TETrace[1].body
----

_next ==
    /\ \E i,j \in DOMAIN _TETrace:
        /\ \/ /\ j = i + 1
              /\ i = TLCGet("level")
        /\ folded  = _TETrace[i].folded
        /\ folded' = _TETrace[j].folded
        /\ adopted  = _TETrace[i].adopted
        /\ adopted' = _TETrace[j].adopted
        /\ cut_taken  = _TETrace[i].cut_taken
        /\ cut_taken' = _TETrace[j].cut_taken
        /\ seeded  = _TETrace[i].seeded
        /\ seeded' = _TETrace[j].seeded
        /\ next_gen  = _TETrace[i].next_gen
        /\ next_gen' = _TETrace[j].next_gen
        /\ committed  = _TETrace[i].committed
        /\ committed' = _TETrace[j].committed
        /\ ret  = _TETrace[i].ret
        /\ ret' = _TETrace[j].ret
        /\ round  = _TETrace[i].round
        /\ round' = _TETrace[j].round
        /\ meta  = _TETrace[i].meta
        /\ meta' = _TETrace[j].meta
        /\ edge  = _TETrace[i].edge
        /\ edge' = _TETrace[j].edge
        /\ body  = _TETrace[i].body
        /\ body' = _TETrace[j].body

\* Uncomment the ASSUME below to write the states of the error trace
\* to the given file in Json format. Note that you can pass any tuple
\* to `JsonSerialize`. For example, a sub-sequence of _TETrace.
    \* ASSUME
    \*     LET J == INSTANCE Json
    \*         IN J!JsonSerialize("CaGcCondemnMarkerGate_TTrace_1785646430.json", _TETrace)

=============================================================================

 Note that you can extract this module `CaGcCondemnMarkerGate_TEExpression`
  to a dedicated file to reuse `expression` (the module in the 
  dedicated `CaGcCondemnMarkerGate_TEExpression.tla` file takes precedence 
  over the module `CaGcCondemnMarkerGate_TEExpression` below).

---- MODULE CaGcCondemnMarkerGate_TEExpression ----
EXTENDS Sequences, TLCExt, Toolbox, CaGcCondemnMarkerGate, Naturals, TLC

expression == 
    [
        \* To hide variables of the `CaGcCondemnMarkerGate` spec from the error trace,
        \* remove the variables below.  The trace will be written in the order
        \* of the fields of this record.
        folded |-> folded
        ,adopted |-> adopted
        ,cut_taken |-> cut_taken
        ,seeded |-> seeded
        ,next_gen |-> next_gen
        ,committed |-> committed
        ,ret |-> ret
        ,round |-> round
        ,meta |-> meta
        ,edge |-> edge
        ,body |-> body
        
        \* Put additional constant-, state-, and action-level expressions here:
        \* ,_stateNumber |-> _TEPosition
        \* ,_foldedUnchanged |-> folded = folded'
        
        \* Format the `folded` variable as Json value.
        \* ,_foldedJson |->
        \*     LET J == INSTANCE Json
        \*     IN J!ToJson(folded)
        
        \* Lastly, you may build expressions over arbitrary sets of states by
        \* leveraging the _TETrace operator.  For example, this is how to
        \* count the number of times a spec variable changed up to the current
        \* state in the trace.
        \* ,_foldedModCount |->
        \*     LET F[s \in DOMAIN _TETrace] ==
        \*         IF s = 1 THEN 0
        \*         ELSE IF _TETrace[s].folded # _TETrace[s-1].folded
        \*             THEN 1 + F[s-1] ELSE F[s-1]
        \*     IN F[_TEPosition - 1]
    ]

=============================================================================



Parsing and semantic processing can take forever if the trace below is long.
 In this case, it is advised to uncomment the module below to deserialize the
 trace from a generated binary file.

\*
\*---- MODULE CaGcCondemnMarkerGate_TETrace ----
\*EXTENDS IOUtils, CaGcCondemnMarkerGate, TLC
\*
\*trace == IODeserialize("CaGcCondemnMarkerGate_TTrace_1785646430.bin", TRUE)
\*
\*=============================================================================
\*

---- MODULE CaGcCondemnMarkerGate_TETrace ----
EXTENDS CaGcCondemnMarkerGate, TLC

trace == 
    <<
    ([ret |-> [tok |-> 0, st |-> "none", cr |-> 0, conf |-> FALSE],committed |-> FALSE,edge |-> "none",round |-> 1,meta |-> "clean",cut_taken |-> FALSE,seeded |-> FALSE,body |-> [present |-> FALSE, tok |-> 0],folded |-> FALSE,adopted |-> 0,next_gen |-> 1]),
    ([ret |-> [tok |-> 0, st |-> "none", cr |-> 0, conf |-> FALSE],committed |-> FALSE,edge |-> "none",round |-> 1,meta |-> "clean",cut_taken |-> FALSE,seeded |-> TRUE,body |-> [present |-> TRUE, tok |-> 1],folded |-> FALSE,adopted |-> 0,next_gen |-> 2]),
    ([ret |-> [tok |-> 0, st |-> "none", cr |-> 0, conf |-> FALSE],committed |-> FALSE,edge |-> "none",round |-> 1,meta |-> "clean",cut_taken |-> TRUE,seeded |-> TRUE,body |-> [present |-> TRUE, tok |-> 1],folded |-> FALSE,adopted |-> 0,next_gen |-> 2]),
    ([ret |-> [tok |-> 1, st |-> "condemned", cr |-> 1, conf |-> FALSE],committed |-> FALSE,edge |-> "none",round |-> 2,meta |-> "clean",cut_taken |-> FALSE,seeded |-> TRUE,body |-> [present |-> TRUE, tok |-> 1],folded |-> FALSE,adopted |-> 0,next_gen |-> 2]),
    ([ret |-> [tok |-> 1, st |-> "condemned", cr |-> 1, conf |-> FALSE],committed |-> FALSE,edge |-> "none",round |-> 2,meta |-> "clean",cut_taken |-> TRUE,seeded |-> TRUE,body |-> [present |-> TRUE, tok |-> 1],folded |-> FALSE,adopted |-> 0,next_gen |-> 2]),
    ([ret |-> [tok |-> 1, st |-> "pending", cr |-> 1, conf |-> TRUE],committed |-> FALSE,edge |-> "none",round |-> 3,meta |-> "clean",cut_taken |-> FALSE,seeded |-> TRUE,body |-> [present |-> TRUE, tok |-> 1],folded |-> FALSE,adopted |-> 0,next_gen |-> 2]),
    ([ret |-> [tok |-> 1, st |-> "pending", cr |-> 1, conf |-> TRUE],committed |-> FALSE,edge |-> "none",round |-> 3,meta |-> "clean",cut_taken |-> TRUE,seeded |-> TRUE,body |-> [present |-> TRUE, tok |-> 1],folded |-> FALSE,adopted |-> 0,next_gen |-> 2]),
    ([ret |-> [tok |-> 1, st |-> "pending", cr |-> 1, conf |-> TRUE],committed |-> FALSE,edge |-> "landed",round |-> 3,meta |-> "clean",cut_taken |-> TRUE,seeded |-> TRUE,body |-> [present |-> TRUE, tok |-> 1],folded |-> FALSE,adopted |-> 0,next_gen |-> 2]),
    ([ret |-> [tok |-> 1, st |-> "pending", cr |-> 1, conf |-> TRUE],committed |-> FALSE,edge |-> "landed",round |-> 3,meta |-> "clean",cut_taken |-> TRUE,seeded |-> TRUE,body |-> [present |-> TRUE, tok |-> 1],folded |-> FALSE,adopted |-> 1,next_gen |-> 2]),
    ([ret |-> [tok |-> 1, st |-> "pending", cr |-> 1, conf |-> TRUE],committed |-> TRUE,edge |-> "landed",round |-> 3,meta |-> "clean",cut_taken |-> TRUE,seeded |-> TRUE,body |-> [present |-> TRUE, tok |-> 1],folded |-> FALSE,adopted |-> 1,next_gen |-> 2]),
    ([ret |-> [tok |-> 0, st |-> "none", cr |-> 0, conf |-> FALSE],committed |-> TRUE,edge |-> "landed",round |-> 4,meta |-> "clean",cut_taken |-> FALSE,seeded |-> TRUE,body |-> [present |-> FALSE, tok |-> 0],folded |-> FALSE,adopted |-> 1,next_gen |-> 2])
    >>
----


=============================================================================

---- CONFIG CaGcCondemnMarkerGate_TTrace_1785646430 ----
CONSTANTS
    GateOnMarker = FALSE
    MaxGen = 3
    MaxRounds = 6

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
\* Generated on Sun Aug 02 06:53:50 CEST 2026