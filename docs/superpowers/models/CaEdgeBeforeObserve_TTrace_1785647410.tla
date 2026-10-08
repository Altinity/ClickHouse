---- MODULE CaEdgeBeforeObserve_TTrace_1785647410 ----
EXTENDS Sequences, TLCExt, Toolbox, CaEdgeBeforeObserve, Naturals, TLC

_expression ==
    LET CaEdgeBeforeObserve_TEExpression == INSTANCE CaEdgeBeforeObserve_TEExpression
    IN CaEdgeBeforeObserve_TEExpression!expression
----

_trace ==
    LET CaEdgeBeforeObserve_TETrace == INSTANCE CaEdgeBeforeObserve_TETrace
    IN CaEdgeBeforeObserve_TETrace!trace
----

_inv ==
    ~(
        TLCGet("level") = Len(_TETrace)
        /\
        precommitted = (TRUE)
        /\
        entry = ([ht |-> [st |-> "none", r |-> 0], he |-> [st |-> "none", r |-> 0]])
        /\
        view = (4)
        /\
        committed = (TRUE)
        /\
        round = (4)
        /\
        aborted = (FALSE)
        /\
        doomed = ({})
        /\
        present = ([ht |-> TRUE, he |-> FALSE])
        /\
        adopted = (TRUE)
    )
----

_init ==
    /\ view = _TETrace[1].view
    /\ adopted = _TETrace[1].adopted
    /\ precommitted = _TETrace[1].precommitted
    /\ aborted = _TETrace[1].aborted
    /\ committed = _TETrace[1].committed
    /\ round = _TETrace[1].round
    /\ doomed = _TETrace[1].doomed
    /\ present = _TETrace[1].present
    /\ entry = _TETrace[1].entry
----

_next ==
    /\ \E i,j \in DOMAIN _TETrace:
        /\ \/ /\ j = i + 1
              /\ i = TLCGet("level")
        /\ view  = _TETrace[i].view
        /\ view' = _TETrace[j].view
        /\ adopted  = _TETrace[i].adopted
        /\ adopted' = _TETrace[j].adopted
        /\ precommitted  = _TETrace[i].precommitted
        /\ precommitted' = _TETrace[j].precommitted
        /\ aborted  = _TETrace[i].aborted
        /\ aborted' = _TETrace[j].aborted
        /\ committed  = _TETrace[i].committed
        /\ committed' = _TETrace[j].committed
        /\ round  = _TETrace[i].round
        /\ round' = _TETrace[j].round
        /\ doomed  = _TETrace[i].doomed
        /\ doomed' = _TETrace[j].doomed
        /\ present  = _TETrace[i].present
        /\ present' = _TETrace[j].present
        /\ entry  = _TETrace[i].entry
        /\ entry' = _TETrace[j].entry

\* Uncomment the ASSUME below to write the states of the error trace
\* to the given file in Json format. Note that you can pass any tuple
\* to `JsonSerialize`. For example, a sub-sequence of _TETrace.
    \* ASSUME
    \*     LET J == INSTANCE Json
    \*         IN J!JsonSerialize("CaEdgeBeforeObserve_TTrace_1785647410.json", _TETrace)

=============================================================================

 Note that you can extract this module `CaEdgeBeforeObserve_TEExpression`
  to a dedicated file to reuse `expression` (the module in the 
  dedicated `CaEdgeBeforeObserve_TEExpression.tla` file takes precedence 
  over the module `CaEdgeBeforeObserve_TEExpression` below).

---- MODULE CaEdgeBeforeObserve_TEExpression ----
EXTENDS Sequences, TLCExt, Toolbox, CaEdgeBeforeObserve, Naturals, TLC

expression == 
    [
        \* To hide variables of the `CaEdgeBeforeObserve` spec from the error trace,
        \* remove the variables below.  The trace will be written in the order
        \* of the fields of this record.
        view |-> view
        ,adopted |-> adopted
        ,precommitted |-> precommitted
        ,aborted |-> aborted
        ,committed |-> committed
        ,round |-> round
        ,doomed |-> doomed
        ,present |-> present
        ,entry |-> entry
        
        \* Put additional constant-, state-, and action-level expressions here:
        \* ,_stateNumber |-> _TEPosition
        \* ,_viewUnchanged |-> view = view'
        
        \* Format the `view` variable as Json value.
        \* ,_viewJson |->
        \*     LET J == INSTANCE Json
        \*     IN J!ToJson(view)
        
        \* Lastly, you may build expressions over arbitrary sets of states by
        \* leveraging the _TETrace operator.  For example, this is how to
        \* count the number of times a spec variable changed up to the current
        \* state in the trace.
        \* ,_viewModCount |->
        \*     LET F[s \in DOMAIN _TETrace] ==
        \*         IF s = 1 THEN 0
        \*         ELSE IF _TETrace[s].view # _TETrace[s-1].view
        \*             THEN 1 + F[s-1] ELSE F[s-1]
        \*     IN F[_TEPosition - 1]
    ]

=============================================================================



Parsing and semantic processing can take forever if the trace below is long.
 In this case, it is advised to uncomment the module below to deserialize the
 trace from a generated binary file.

\*
\*---- MODULE CaEdgeBeforeObserve_TETrace ----
\*EXTENDS IOUtils, CaEdgeBeforeObserve, TLC
\*
\*trace == IODeserialize("CaEdgeBeforeObserve_TTrace_1785647410.bin", TRUE)
\*
\*=============================================================================
\*

---- MODULE CaEdgeBeforeObserve_TETrace ----
EXTENDS CaEdgeBeforeObserve, TLC

trace == 
    <<
    ([precommitted |-> FALSE,entry |-> [ht |-> [st |-> "none", r |-> 0], he |-> [st |-> "none", r |-> 0]],view |-> 0,committed |-> FALSE,round |-> 1,aborted |-> FALSE,doomed |-> {},present |-> [ht |-> TRUE, he |-> TRUE],adopted |-> FALSE]),
    ([precommitted |-> FALSE,entry |-> [ht |-> [st |-> "none", r |-> 0], he |-> [st |-> "none", r |-> 0]],view |-> 1,committed |-> FALSE,round |-> 1,aborted |-> FALSE,doomed |-> {},present |-> [ht |-> TRUE, he |-> TRUE],adopted |-> FALSE]),
    ([precommitted |-> FALSE,entry |-> [ht |-> [st |-> "cond", r |-> 1], he |-> [st |-> "cond", r |-> 1]],view |-> 1,committed |-> FALSE,round |-> 2,aborted |-> FALSE,doomed |-> {},present |-> [ht |-> TRUE, he |-> TRUE],adopted |-> FALSE]),
    ([precommitted |-> FALSE,entry |-> [ht |-> [st |-> "cond", r |-> 1], he |-> [st |-> "cond", r |-> 1]],view |-> 2,committed |-> FALSE,round |-> 2,aborted |-> FALSE,doomed |-> {},present |-> [ht |-> TRUE, he |-> TRUE],adopted |-> FALSE]),
    ([precommitted |-> FALSE,entry |-> [ht |-> [st |-> "pend", r |-> 1], he |-> [st |-> "pend", r |-> 1]],view |-> 2,committed |-> FALSE,round |-> 3,aborted |-> FALSE,doomed |-> {},present |-> [ht |-> TRUE, he |-> TRUE],adopted |-> FALSE]),
    ([precommitted |-> FALSE,entry |-> [ht |-> [st |-> "pend", r |-> 1], he |-> [st |-> "pend", r |-> 1]],view |-> 2,committed |-> FALSE,round |-> 4,aborted |-> FALSE,doomed |-> {"ht", "he"},present |-> [ht |-> TRUE, he |-> TRUE],adopted |-> FALSE]),
    ([precommitted |-> FALSE,entry |-> [ht |-> [st |-> "pend", r |-> 1], he |-> [st |-> "pend", r |-> 1]],view |-> 4,committed |-> FALSE,round |-> 4,aborted |-> FALSE,doomed |-> {"ht", "he"},present |-> [ht |-> TRUE, he |-> TRUE],adopted |-> FALSE]),
    ([precommitted |-> TRUE,entry |-> [ht |-> [st |-> "pend", r |-> 1], he |-> [st |-> "pend", r |-> 1]],view |-> 4,committed |-> FALSE,round |-> 4,aborted |-> FALSE,doomed |-> {"ht", "he"},present |-> [ht |-> TRUE, he |-> TRUE],adopted |-> FALSE]),
    ([precommitted |-> TRUE,entry |-> [ht |-> [st |-> "none", r |-> 0], he |-> [st |-> "pend", r |-> 1]],view |-> 4,committed |-> FALSE,round |-> 4,aborted |-> FALSE,doomed |-> {"he"},present |-> [ht |-> TRUE, he |-> TRUE],adopted |-> TRUE]),
    ([precommitted |-> TRUE,entry |-> [ht |-> [st |-> "none", r |-> 0], he |-> [st |-> "pend", r |-> 1]],view |-> 4,committed |-> TRUE,round |-> 4,aborted |-> FALSE,doomed |-> {"he"},present |-> [ht |-> TRUE, he |-> TRUE],adopted |-> TRUE]),
    ([precommitted |-> TRUE,entry |-> [ht |-> [st |-> "none", r |-> 0], he |-> [st |-> "none", r |-> 0]],view |-> 4,committed |-> TRUE,round |-> 4,aborted |-> FALSE,doomed |-> {},present |-> [ht |-> TRUE, he |-> FALSE],adopted |-> TRUE])
    >>
----


=============================================================================

---- CONFIG CaEdgeBeforeObserve_TTrace_1785647410 ----
CONSTANTS
    MaxRound = 6
    OrderSabotage = FALSE
    AdoptCheck = TRUE
    K3Head = TRUE
    K3AdoptCheck = FALSE

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
\* Generated on Sun Aug 02 07:10:10 CEST 2026