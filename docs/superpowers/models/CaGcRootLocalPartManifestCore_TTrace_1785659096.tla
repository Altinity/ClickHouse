---- MODULE CaGcRootLocalPartManifestCore_TTrace_1785659096 ----
EXTENDS Sequences, TLCExt, Toolbox, Naturals, TLC, CaGcRootLocalPartManifestCore, CaGcRootLocalPartManifestCore_TEConstants

_expression ==
    LET CaGcRootLocalPartManifestCore_TEExpression == INSTANCE CaGcRootLocalPartManifestCore_TEExpression
    IN CaGcRootLocalPartManifestCore_TEExpression!expression
----

_trace ==
    LET CaGcRootLocalPartManifestCore_TETrace == INSTANCE CaGcRootLocalPartManifestCore_TETrace
    IN CaGcRootLocalPartManifestCore_TETrace!trace
----

_inv ==
    ~(
        TLCGet("level") = Len(_TETrace)
        /\
        cursor = ((n1 :> 1 @@ n2 :> 0))
        /\
        deadTok = ((b1 :> {1}))
        /\
        blobEdges = ({})
        /\
        shardIndeg = ((s1 :> (b1 :> 0)))
        /\
        listedTok = ((n1 :> 0 @@ n2 :> 0))
        /\
        attemptSeq = (0)
        /\
        fencePos = ((n1 :> 1 @@ n2 :> 0))
        /\
        trimBase = ((n1 :> 0 @@ n2 :> 0))
        /\
        wView = ((w1 :> 0))
        /\
        foldTok = ((n1 :> 0 @@ n2 :> 0))
        /\
        nextTok = ((b1 :> 2))
        /\
        storedTok = ((b1 :> 0))
        /\
        roundOf = ((L1 :> 1))
        /\
        tokOf = ((b1 :> 1))
        /\
        sweepEligible = ((bp1 :> FALSE))
        /\
        mNs = ((<<n1, m1>> :> n2 @@ <<n1, m2>> :> n1 @@ <<n2, m1>> :> n2 @@ <<n2, m2>> :> n2))
        /\
        journal = ((n1 :> <<[old |-> {}, new |-> {<<n1, m1>>}, ver |-> 1, ref |-> r1]>> @@ n2 :> <<>>))
        /\
        mRef = ((<<n1, m1>> :> <<n1, m1>> @@ <<n1, m2>> :> <<n1, m2>> @@ <<n2, m1>> :> <<n2, m1>> @@ <<n2, m2>> :> <<n2, m2>>))
        /\
        gcPhase = ((L1 :> "fenced"))
        /\
        coordFence = ((n1 :> 0 @@ n2 :> 0))
        /\
        reducerOwner = ((s1 :> L1))
        /\
        mEntries = ((<<n1, m1>> :> (p1 :> b1) @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")))
        /\
        attViewable = ((0 :> {} @@ 1 :> {} @@ 2 :> {}))
        /\
        retired = ({})
        /\
        gcRound = (1)
        /\
        blobIndeg = ((b1 :> 0))
        /\
        mPrefix = ((<<n1, m1>> :> bp1 @@ <<n1, m2>> :> bp1 @@ <<n2, m1>> :> bp1 @@ <<n2, m2>> :> bp1))
        /\
        extraShared = ({})
        /\
        owner = ((<<n1, m1>> :> r1 @@ <<n1, m2>> :> "none" @@ <<n2, m1>> :> "none" @@ <<n2, m2>> :> "none"))
        /\
        foldSeal = ((0 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 1 :> [foldedCursor |-> (n1 :> 1 @@ n2 :> 0), classified |-> {}] @@ 2 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}]))
        /\
        prevFencePos = ((n1 :> 0 @@ n2 :> 0))
        /\
        sealAt = ((0 :> {} @@ 1 :> {} @@ 2 :> {}))
        /\
        everEdged = ({<<n1, m1>>})
        /\
        fenceVersion = ((0 :> (n1 :> 0 @@ n2 :> 0) @@ 1 :> (n1 :> 1 @@ n2 :> 1) @@ 2 :> (n1 :> 0 @@ n2 :> 0)))
        /\
        mActiveEdges = ((<<n1, m1>> :> (p1 :> "noblob") @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")))
        /\
        completionSeal = ((0 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 1 :> [fenced |-> {n1, n2}, rechecked |-> {}, deleted |-> {b1}, adoptable |-> FALSE] @@ 2 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE]))
        /\
        mfCleanup = ({})
        /\
        inflight = ({})
        /\
        mBody = ((<<n1, m1>> :> TRUE @@ <<n1, m2>> :> FALSE @@ <<n2, m1>> :> FALSE @@ <<n2, m2>> :> FALSE))
        /\
        retiredAt = ((0 :> {} @@ 1 :> {} @@ 2 :> {}))
        /\
        foldedTok = ((n1 :> 0 @@ n2 :> 0))
        /\
        present = ((b1 :> FALSE))
        /\
        mfDeleted = ({})
        /\
        adopted = ((0 :> 0 @@ 1 :> 0 @@ 2 :> 0))
    )
----

_init ==
    /\ fencePos = _TETrace[1].fencePos
    /\ gcRound = _TETrace[1].gcRound
    /\ reducerOwner = _TETrace[1].reducerOwner
    /\ inflight = _TETrace[1].inflight
    /\ mEntries = _TETrace[1].mEntries
    /\ mActiveEdges = _TETrace[1].mActiveEdges
    /\ attViewable = _TETrace[1].attViewable
    /\ sweepEligible = _TETrace[1].sweepEligible
    /\ gcPhase = _TETrace[1].gcPhase
    /\ coordFence = _TETrace[1].coordFence
    /\ completionSeal = _TETrace[1].completionSeal
    /\ present = _TETrace[1].present
    /\ foldSeal = _TETrace[1].foldSeal
    /\ mBody = _TETrace[1].mBody
    /\ wView = _TETrace[1].wView
    /\ foldedTok = _TETrace[1].foldedTok
    /\ retired = _TETrace[1].retired
    /\ shardIndeg = _TETrace[1].shardIndeg
    /\ cursor = _TETrace[1].cursor
    /\ adopted = _TETrace[1].adopted
    /\ foldTok = _TETrace[1].foldTok
    /\ trimBase = _TETrace[1].trimBase
    /\ listedTok = _TETrace[1].listedTok
    /\ extraShared = _TETrace[1].extraShared
    /\ mRef = _TETrace[1].mRef
    /\ mPrefix = _TETrace[1].mPrefix
    /\ storedTok = _TETrace[1].storedTok
    /\ sealAt = _TETrace[1].sealAt
    /\ roundOf = _TETrace[1].roundOf
    /\ fenceVersion = _TETrace[1].fenceVersion
    /\ retiredAt = _TETrace[1].retiredAt
    /\ mfCleanup = _TETrace[1].mfCleanup
    /\ tokOf = _TETrace[1].tokOf
    /\ nextTok = _TETrace[1].nextTok
    /\ everEdged = _TETrace[1].everEdged
    /\ owner = _TETrace[1].owner
    /\ mfDeleted = _TETrace[1].mfDeleted
    /\ deadTok = _TETrace[1].deadTok
    /\ mNs = _TETrace[1].mNs
    /\ blobIndeg = _TETrace[1].blobIndeg
    /\ blobEdges = _TETrace[1].blobEdges
    /\ attemptSeq = _TETrace[1].attemptSeq
    /\ journal = _TETrace[1].journal
    /\ prevFencePos = _TETrace[1].prevFencePos
----

_next ==
    /\ \E i,j \in DOMAIN _TETrace:
        /\ \/ /\ j = i + 1
              /\ i = TLCGet("level")
        /\ fencePos  = _TETrace[i].fencePos
        /\ fencePos' = _TETrace[j].fencePos
        /\ gcRound  = _TETrace[i].gcRound
        /\ gcRound' = _TETrace[j].gcRound
        /\ reducerOwner  = _TETrace[i].reducerOwner
        /\ reducerOwner' = _TETrace[j].reducerOwner
        /\ inflight  = _TETrace[i].inflight
        /\ inflight' = _TETrace[j].inflight
        /\ mEntries  = _TETrace[i].mEntries
        /\ mEntries' = _TETrace[j].mEntries
        /\ mActiveEdges  = _TETrace[i].mActiveEdges
        /\ mActiveEdges' = _TETrace[j].mActiveEdges
        /\ attViewable  = _TETrace[i].attViewable
        /\ attViewable' = _TETrace[j].attViewable
        /\ sweepEligible  = _TETrace[i].sweepEligible
        /\ sweepEligible' = _TETrace[j].sweepEligible
        /\ gcPhase  = _TETrace[i].gcPhase
        /\ gcPhase' = _TETrace[j].gcPhase
        /\ coordFence  = _TETrace[i].coordFence
        /\ coordFence' = _TETrace[j].coordFence
        /\ completionSeal  = _TETrace[i].completionSeal
        /\ completionSeal' = _TETrace[j].completionSeal
        /\ present  = _TETrace[i].present
        /\ present' = _TETrace[j].present
        /\ foldSeal  = _TETrace[i].foldSeal
        /\ foldSeal' = _TETrace[j].foldSeal
        /\ mBody  = _TETrace[i].mBody
        /\ mBody' = _TETrace[j].mBody
        /\ wView  = _TETrace[i].wView
        /\ wView' = _TETrace[j].wView
        /\ foldedTok  = _TETrace[i].foldedTok
        /\ foldedTok' = _TETrace[j].foldedTok
        /\ retired  = _TETrace[i].retired
        /\ retired' = _TETrace[j].retired
        /\ shardIndeg  = _TETrace[i].shardIndeg
        /\ shardIndeg' = _TETrace[j].shardIndeg
        /\ cursor  = _TETrace[i].cursor
        /\ cursor' = _TETrace[j].cursor
        /\ adopted  = _TETrace[i].adopted
        /\ adopted' = _TETrace[j].adopted
        /\ foldTok  = _TETrace[i].foldTok
        /\ foldTok' = _TETrace[j].foldTok
        /\ trimBase  = _TETrace[i].trimBase
        /\ trimBase' = _TETrace[j].trimBase
        /\ listedTok  = _TETrace[i].listedTok
        /\ listedTok' = _TETrace[j].listedTok
        /\ extraShared  = _TETrace[i].extraShared
        /\ extraShared' = _TETrace[j].extraShared
        /\ mRef  = _TETrace[i].mRef
        /\ mRef' = _TETrace[j].mRef
        /\ mPrefix  = _TETrace[i].mPrefix
        /\ mPrefix' = _TETrace[j].mPrefix
        /\ storedTok  = _TETrace[i].storedTok
        /\ storedTok' = _TETrace[j].storedTok
        /\ sealAt  = _TETrace[i].sealAt
        /\ sealAt' = _TETrace[j].sealAt
        /\ roundOf  = _TETrace[i].roundOf
        /\ roundOf' = _TETrace[j].roundOf
        /\ fenceVersion  = _TETrace[i].fenceVersion
        /\ fenceVersion' = _TETrace[j].fenceVersion
        /\ retiredAt  = _TETrace[i].retiredAt
        /\ retiredAt' = _TETrace[j].retiredAt
        /\ mfCleanup  = _TETrace[i].mfCleanup
        /\ mfCleanup' = _TETrace[j].mfCleanup
        /\ tokOf  = _TETrace[i].tokOf
        /\ tokOf' = _TETrace[j].tokOf
        /\ nextTok  = _TETrace[i].nextTok
        /\ nextTok' = _TETrace[j].nextTok
        /\ everEdged  = _TETrace[i].everEdged
        /\ everEdged' = _TETrace[j].everEdged
        /\ owner  = _TETrace[i].owner
        /\ owner' = _TETrace[j].owner
        /\ mfDeleted  = _TETrace[i].mfDeleted
        /\ mfDeleted' = _TETrace[j].mfDeleted
        /\ deadTok  = _TETrace[i].deadTok
        /\ deadTok' = _TETrace[j].deadTok
        /\ mNs  = _TETrace[i].mNs
        /\ mNs' = _TETrace[j].mNs
        /\ blobIndeg  = _TETrace[i].blobIndeg
        /\ blobIndeg' = _TETrace[j].blobIndeg
        /\ blobEdges  = _TETrace[i].blobEdges
        /\ blobEdges' = _TETrace[j].blobEdges
        /\ attemptSeq  = _TETrace[i].attemptSeq
        /\ attemptSeq' = _TETrace[j].attemptSeq
        /\ journal  = _TETrace[i].journal
        /\ journal' = _TETrace[j].journal
        /\ prevFencePos  = _TETrace[i].prevFencePos
        /\ prevFencePos' = _TETrace[j].prevFencePos

\* Uncomment the ASSUME below to write the states of the error trace
\* to the given file in Json format. Note that you can pass any tuple
\* to `JsonSerialize`. For example, a sub-sequence of _TETrace.
    \* ASSUME
    \*     LET J == INSTANCE Json
    \*         IN J!JsonSerialize("CaGcRootLocalPartManifestCore_TTrace_1785659096.json", _TETrace)

=============================================================================

 Note that you can extract this module `CaGcRootLocalPartManifestCore_TEExpression`
  to a dedicated file to reuse `expression` (the module in the 
  dedicated `CaGcRootLocalPartManifestCore_TEExpression.tla` file takes precedence 
  over the module `CaGcRootLocalPartManifestCore_TEExpression` below).

---- MODULE CaGcRootLocalPartManifestCore_TEExpression ----
EXTENDS Sequences, TLCExt, Toolbox, Naturals, TLC, CaGcRootLocalPartManifestCore, CaGcRootLocalPartManifestCore_TEConstants

expression == 
    [
        \* To hide variables of the `CaGcRootLocalPartManifestCore` spec from the error trace,
        \* remove the variables below.  The trace will be written in the order
        \* of the fields of this record.
        fencePos |-> fencePos
        ,gcRound |-> gcRound
        ,reducerOwner |-> reducerOwner
        ,inflight |-> inflight
        ,mEntries |-> mEntries
        ,mActiveEdges |-> mActiveEdges
        ,attViewable |-> attViewable
        ,sweepEligible |-> sweepEligible
        ,gcPhase |-> gcPhase
        ,coordFence |-> coordFence
        ,completionSeal |-> completionSeal
        ,present |-> present
        ,foldSeal |-> foldSeal
        ,mBody |-> mBody
        ,wView |-> wView
        ,foldedTok |-> foldedTok
        ,retired |-> retired
        ,shardIndeg |-> shardIndeg
        ,cursor |-> cursor
        ,adopted |-> adopted
        ,foldTok |-> foldTok
        ,trimBase |-> trimBase
        ,listedTok |-> listedTok
        ,extraShared |-> extraShared
        ,mRef |-> mRef
        ,mPrefix |-> mPrefix
        ,storedTok |-> storedTok
        ,sealAt |-> sealAt
        ,roundOf |-> roundOf
        ,fenceVersion |-> fenceVersion
        ,retiredAt |-> retiredAt
        ,mfCleanup |-> mfCleanup
        ,tokOf |-> tokOf
        ,nextTok |-> nextTok
        ,everEdged |-> everEdged
        ,owner |-> owner
        ,mfDeleted |-> mfDeleted
        ,deadTok |-> deadTok
        ,mNs |-> mNs
        ,blobIndeg |-> blobIndeg
        ,blobEdges |-> blobEdges
        ,attemptSeq |-> attemptSeq
        ,journal |-> journal
        ,prevFencePos |-> prevFencePos
        
        \* Put additional constant-, state-, and action-level expressions here:
        \* ,_stateNumber |-> _TEPosition
        \* ,_fencePosUnchanged |-> fencePos = fencePos'
        
        \* Format the `fencePos` variable as Json value.
        \* ,_fencePosJson |->
        \*     LET J == INSTANCE Json
        \*     IN J!ToJson(fencePos)
        
        \* Lastly, you may build expressions over arbitrary sets of states by
        \* leveraging the _TETrace operator.  For example, this is how to
        \* count the number of times a spec variable changed up to the current
        \* state in the trace.
        \* ,_fencePosModCount |->
        \*     LET F[s \in DOMAIN _TETrace] ==
        \*         IF s = 1 THEN 0
        \*         ELSE IF _TETrace[s].fencePos # _TETrace[s-1].fencePos
        \*             THEN 1 + F[s-1] ELSE F[s-1]
        \*     IN F[_TEPosition - 1]
    ]

=============================================================================



Parsing and semantic processing can take forever if the trace below is long.
 In this case, it is advised to uncomment the module below to deserialize the
 trace from a generated binary file.

\*
\*---- MODULE CaGcRootLocalPartManifestCore_TETrace ----
\*EXTENDS IOUtils, TLC, CaGcRootLocalPartManifestCore, CaGcRootLocalPartManifestCore_TEConstants
\*
\*trace == IODeserialize("CaGcRootLocalPartManifestCore_TTrace_1785659096.bin", TRUE)
\*
\*=============================================================================
\*

---- MODULE CaGcRootLocalPartManifestCore_TETrace ----
EXTENDS TLC, CaGcRootLocalPartManifestCore, CaGcRootLocalPartManifestCore_TEConstants

trace == 
    <<
    ([cursor |-> (n1 :> 0 @@ n2 :> 0),deadTok |-> (b1 :> {}),blobEdges |-> {},shardIndeg |-> (s1 :> (b1 :> 0)),listedTok |-> (n1 :> 0 @@ n2 :> 0),attemptSeq |-> 0,fencePos |-> (n1 :> 0 @@ n2 :> 0),trimBase |-> (n1 :> 0 @@ n2 :> 0),wView |-> (w1 :> 0),foldTok |-> (n1 :> 0 @@ n2 :> 0),nextTok |-> (b1 :> 1),storedTok |-> (b1 :> 0),roundOf |-> (L1 :> 0),tokOf |-> (b1 :> 0),sweepEligible |-> (bp1 :> FALSE),mNs |-> (<<n1, m1>> :> n1 @@ <<n1, m2>> :> n1 @@ <<n2, m1>> :> n2 @@ <<n2, m2>> :> n2),journal |-> (n1 :> <<>> @@ n2 :> <<>>),mRef |-> (<<n1, m1>> :> <<n1, m1>> @@ <<n1, m2>> :> <<n1, m2>> @@ <<n2, m1>> :> <<n2, m1>> @@ <<n2, m2>> :> <<n2, m2>>),gcPhase |-> (L1 :> "idle"),coordFence |-> (n1 :> 0 @@ n2 :> 0),reducerOwner |-> (s1 :> L1),mEntries |-> (<<n1, m1>> :> (p1 :> "noblob") @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),attViewable |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),retired |-> {},gcRound |-> 0,blobIndeg |-> (b1 :> 0),mPrefix |-> (<<n1, m1>> :> bp1 @@ <<n1, m2>> :> bp1 @@ <<n2, m1>> :> bp1 @@ <<n2, m2>> :> bp1),extraShared |-> {},owner |-> (<<n1, m1>> :> "none" @@ <<n1, m2>> :> "none" @@ <<n2, m1>> :> "none" @@ <<n2, m2>> :> "none"),foldSeal |-> (0 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 1 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 2 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}]),prevFencePos |-> (n1 :> 0 @@ n2 :> 0),sealAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),everEdged |-> {},fenceVersion |-> (0 :> (n1 :> 0 @@ n2 :> 0) @@ 1 :> (n1 :> 0 @@ n2 :> 0) @@ 2 :> (n1 :> 0 @@ n2 :> 0)),mActiveEdges |-> (<<n1, m1>> :> (p1 :> "noblob") @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),completionSeal |-> (0 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 1 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 2 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE]),mfCleanup |-> {},inflight |-> {},mBody |-> (<<n1, m1>> :> FALSE @@ <<n1, m2>> :> FALSE @@ <<n2, m1>> :> FALSE @@ <<n2, m2>> :> FALSE),retiredAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),foldedTok |-> (n1 :> 0 @@ n2 :> 0),present |-> (b1 :> FALSE),mfDeleted |-> {},adopted |-> (0 :> 0 @@ 1 :> 0 @@ 2 :> 0)]),
    ([cursor |-> (n1 :> 0 @@ n2 :> 0),deadTok |-> (b1 :> {}),blobEdges |-> {},shardIndeg |-> (s1 :> (b1 :> 0)),listedTok |-> (n1 :> 0 @@ n2 :> 0),attemptSeq |-> 0,fencePos |-> (n1 :> 0 @@ n2 :> 0),trimBase |-> (n1 :> 0 @@ n2 :> 0),wView |-> (w1 :> 0),foldTok |-> (n1 :> 0 @@ n2 :> 0),nextTok |-> (b1 :> 1),storedTok |-> (b1 :> 0),roundOf |-> (L1 :> 0),tokOf |-> (b1 :> 0),sweepEligible |-> (bp1 :> FALSE),mNs |-> (<<n1, m1>> :> n2 @@ <<n1, m2>> :> n1 @@ <<n2, m1>> :> n2 @@ <<n2, m2>> :> n2),journal |-> (n1 :> <<>> @@ n2 :> <<>>),mRef |-> (<<n1, m1>> :> <<n1, m1>> @@ <<n1, m2>> :> <<n1, m2>> @@ <<n2, m1>> :> <<n2, m1>> @@ <<n2, m2>> :> <<n2, m2>>),gcPhase |-> (L1 :> "idle"),coordFence |-> (n1 :> 0 @@ n2 :> 0),reducerOwner |-> (s1 :> L1),mEntries |-> (<<n1, m1>> :> (p1 :> b1) @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),attViewable |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),retired |-> {},gcRound |-> 0,blobIndeg |-> (b1 :> 0),mPrefix |-> (<<n1, m1>> :> bp1 @@ <<n1, m2>> :> bp1 @@ <<n2, m1>> :> bp1 @@ <<n2, m2>> :> bp1),extraShared |-> {},owner |-> (<<n1, m1>> :> "none" @@ <<n1, m2>> :> "none" @@ <<n2, m1>> :> "none" @@ <<n2, m2>> :> "none"),foldSeal |-> (0 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 1 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 2 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}]),prevFencePos |-> (n1 :> 0 @@ n2 :> 0),sealAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),everEdged |-> {<<n1, m1>>},fenceVersion |-> (0 :> (n1 :> 0 @@ n2 :> 0) @@ 1 :> (n1 :> 0 @@ n2 :> 0) @@ 2 :> (n1 :> 0 @@ n2 :> 0)),mActiveEdges |-> (<<n1, m1>> :> (p1 :> "noblob") @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),completionSeal |-> (0 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 1 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 2 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE]),mfCleanup |-> {},inflight |-> {},mBody |-> (<<n1, m1>> :> TRUE @@ <<n1, m2>> :> FALSE @@ <<n2, m1>> :> FALSE @@ <<n2, m2>> :> FALSE),retiredAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),foldedTok |-> (n1 :> 0 @@ n2 :> 0),present |-> (b1 :> FALSE),mfDeleted |-> {},adopted |-> (0 :> 0 @@ 1 :> 0 @@ 2 :> 0)]),
    ([cursor |-> (n1 :> 0 @@ n2 :> 0),deadTok |-> (b1 :> {}),blobEdges |-> {},shardIndeg |-> (s1 :> (b1 :> 0)),listedTok |-> (n1 :> 0 @@ n2 :> 0),attemptSeq |-> 0,fencePos |-> (n1 :> 0 @@ n2 :> 0),trimBase |-> (n1 :> 0 @@ n2 :> 0),wView |-> (w1 :> 0),foldTok |-> (n1 :> 0 @@ n2 :> 0),nextTok |-> (b1 :> 2),storedTok |-> (b1 :> 0),roundOf |-> (L1 :> 0),tokOf |-> (b1 :> 1),sweepEligible |-> (bp1 :> FALSE),mNs |-> (<<n1, m1>> :> n2 @@ <<n1, m2>> :> n1 @@ <<n2, m1>> :> n2 @@ <<n2, m2>> :> n2),journal |-> (n1 :> <<>> @@ n2 :> <<>>),mRef |-> (<<n1, m1>> :> <<n1, m1>> @@ <<n1, m2>> :> <<n1, m2>> @@ <<n2, m1>> :> <<n2, m1>> @@ <<n2, m2>> :> <<n2, m2>>),gcPhase |-> (L1 :> "idle"),coordFence |-> (n1 :> 0 @@ n2 :> 0),reducerOwner |-> (s1 :> L1),mEntries |-> (<<n1, m1>> :> (p1 :> b1) @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),attViewable |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),retired |-> {},gcRound |-> 0,blobIndeg |-> (b1 :> 0),mPrefix |-> (<<n1, m1>> :> bp1 @@ <<n1, m2>> :> bp1 @@ <<n2, m1>> :> bp1 @@ <<n2, m2>> :> bp1),extraShared |-> {},owner |-> (<<n1, m1>> :> "none" @@ <<n1, m2>> :> "none" @@ <<n2, m1>> :> "none" @@ <<n2, m2>> :> "none"),foldSeal |-> (0 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 1 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 2 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}]),prevFencePos |-> (n1 :> 0 @@ n2 :> 0),sealAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),everEdged |-> {<<n1, m1>>},fenceVersion |-> (0 :> (n1 :> 0 @@ n2 :> 0) @@ 1 :> (n1 :> 0 @@ n2 :> 0) @@ 2 :> (n1 :> 0 @@ n2 :> 0)),mActiveEdges |-> (<<n1, m1>> :> (p1 :> "noblob") @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),completionSeal |-> (0 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 1 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 2 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE]),mfCleanup |-> {},inflight |-> {},mBody |-> (<<n1, m1>> :> TRUE @@ <<n1, m2>> :> FALSE @@ <<n2, m1>> :> FALSE @@ <<n2, m2>> :> FALSE),retiredAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),foldedTok |-> (n1 :> 0 @@ n2 :> 0),present |-> (b1 :> TRUE),mfDeleted |-> {},adopted |-> (0 :> 0 @@ 1 :> 0 @@ 2 :> 0)]),
    ([cursor |-> (n1 :> 0 @@ n2 :> 0),deadTok |-> (b1 :> {}),blobEdges |-> {},shardIndeg |-> (s1 :> (b1 :> 0)),listedTok |-> (n1 :> 0 @@ n2 :> 0),attemptSeq |-> 0,fencePos |-> (n1 :> 0 @@ n2 :> 0),trimBase |-> (n1 :> 0 @@ n2 :> 0),wView |-> (w1 :> 0),foldTok |-> (n1 :> 0 @@ n2 :> 0),nextTok |-> (b1 :> 2),storedTok |-> (b1 :> 0),roundOf |-> (L1 :> 0),tokOf |-> (b1 :> 1),sweepEligible |-> (bp1 :> FALSE),mNs |-> (<<n1, m1>> :> n2 @@ <<n1, m2>> :> n1 @@ <<n2, m1>> :> n2 @@ <<n2, m2>> :> n2),journal |-> (n1 :> <<[old |-> {}, new |-> {<<n1, m1>>}, ver |-> 1, ref |-> r1]>> @@ n2 :> <<>>),mRef |-> (<<n1, m1>> :> <<n1, m1>> @@ <<n1, m2>> :> <<n1, m2>> @@ <<n2, m1>> :> <<n2, m1>> @@ <<n2, m2>> :> <<n2, m2>>),gcPhase |-> (L1 :> "idle"),coordFence |-> (n1 :> 0 @@ n2 :> 0),reducerOwner |-> (s1 :> L1),mEntries |-> (<<n1, m1>> :> (p1 :> b1) @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),attViewable |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),retired |-> {},gcRound |-> 0,blobIndeg |-> (b1 :> 0),mPrefix |-> (<<n1, m1>> :> bp1 @@ <<n1, m2>> :> bp1 @@ <<n2, m1>> :> bp1 @@ <<n2, m2>> :> bp1),extraShared |-> {},owner |-> (<<n1, m1>> :> r1 @@ <<n1, m2>> :> "none" @@ <<n2, m1>> :> "none" @@ <<n2, m2>> :> "none"),foldSeal |-> (0 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 1 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 2 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}]),prevFencePos |-> (n1 :> 0 @@ n2 :> 0),sealAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),everEdged |-> {<<n1, m1>>},fenceVersion |-> (0 :> (n1 :> 0 @@ n2 :> 0) @@ 1 :> (n1 :> 0 @@ n2 :> 0) @@ 2 :> (n1 :> 0 @@ n2 :> 0)),mActiveEdges |-> (<<n1, m1>> :> (p1 :> "noblob") @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),completionSeal |-> (0 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 1 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 2 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE]),mfCleanup |-> {},inflight |-> {},mBody |-> (<<n1, m1>> :> TRUE @@ <<n1, m2>> :> FALSE @@ <<n2, m1>> :> FALSE @@ <<n2, m2>> :> FALSE),retiredAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),foldedTok |-> (n1 :> 0 @@ n2 :> 0),present |-> (b1 :> TRUE),mfDeleted |-> {},adopted |-> (0 :> 0 @@ 1 :> 0 @@ 2 :> 0)]),
    ([cursor |-> (n1 :> 0 @@ n2 :> 0),deadTok |-> (b1 :> {}),blobEdges |-> {},shardIndeg |-> (s1 :> (b1 :> 0)),listedTok |-> (n1 :> 0 @@ n2 :> 0),attemptSeq |-> 0,fencePos |-> (n1 :> 0 @@ n2 :> 0),trimBase |-> (n1 :> 0 @@ n2 :> 0),wView |-> (w1 :> 0),foldTok |-> (n1 :> 0 @@ n2 :> 0),nextTok |-> (b1 :> 2),storedTok |-> (b1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),sweepEligible |-> (bp1 :> FALSE),mNs |-> (<<n1, m1>> :> n2 @@ <<n1, m2>> :> n1 @@ <<n2, m1>> :> n2 @@ <<n2, m2>> :> n2),journal |-> (n1 :> <<[old |-> {}, new |-> {<<n1, m1>>}, ver |-> 1, ref |-> r1]>> @@ n2 :> <<>>),mRef |-> (<<n1, m1>> :> <<n1, m1>> @@ <<n1, m2>> :> <<n1, m2>> @@ <<n2, m1>> :> <<n2, m1>> @@ <<n2, m2>> :> <<n2, m2>>),gcPhase |-> (L1 :> "retiring"),coordFence |-> (n1 :> 0 @@ n2 :> 0),reducerOwner |-> (s1 :> L1),mEntries |-> (<<n1, m1>> :> (p1 :> b1) @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),attViewable |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),retired |-> {},gcRound |-> 1,blobIndeg |-> (b1 :> 0),mPrefix |-> (<<n1, m1>> :> bp1 @@ <<n1, m2>> :> bp1 @@ <<n2, m1>> :> bp1 @@ <<n2, m2>> :> bp1),extraShared |-> {},owner |-> (<<n1, m1>> :> r1 @@ <<n1, m2>> :> "none" @@ <<n2, m1>> :> "none" @@ <<n2, m2>> :> "none"),foldSeal |-> (0 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 1 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 2 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}]),prevFencePos |-> (n1 :> 0 @@ n2 :> 0),sealAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),everEdged |-> {<<n1, m1>>},fenceVersion |-> (0 :> (n1 :> 0 @@ n2 :> 0) @@ 1 :> (n1 :> 0 @@ n2 :> 0) @@ 2 :> (n1 :> 0 @@ n2 :> 0)),mActiveEdges |-> (<<n1, m1>> :> (p1 :> "noblob") @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),completionSeal |-> (0 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 1 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 2 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE]),mfCleanup |-> {},inflight |-> {},mBody |-> (<<n1, m1>> :> TRUE @@ <<n1, m2>> :> FALSE @@ <<n2, m1>> :> FALSE @@ <<n2, m2>> :> FALSE),retiredAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),foldedTok |-> (n1 :> 0 @@ n2 :> 0),present |-> (b1 :> TRUE),mfDeleted |-> {},adopted |-> (0 :> 0 @@ 1 :> 0 @@ 2 :> 0)]),
    ([cursor |-> (n1 :> 1 @@ n2 :> 0),deadTok |-> (b1 :> {}),blobEdges |-> {},shardIndeg |-> (s1 :> (b1 :> 0)),listedTok |-> (n1 :> 0 @@ n2 :> 0),attemptSeq |-> 0,fencePos |-> (n1 :> 0 @@ n2 :> 0),trimBase |-> (n1 :> 0 @@ n2 :> 0),wView |-> (w1 :> 0),foldTok |-> (n1 :> 0 @@ n2 :> 0),nextTok |-> (b1 :> 2),storedTok |-> (b1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),sweepEligible |-> (bp1 :> FALSE),mNs |-> (<<n1, m1>> :> n2 @@ <<n1, m2>> :> n1 @@ <<n2, m1>> :> n2 @@ <<n2, m2>> :> n2),journal |-> (n1 :> <<[old |-> {}, new |-> {<<n1, m1>>}, ver |-> 1, ref |-> r1]>> @@ n2 :> <<>>),mRef |-> (<<n1, m1>> :> <<n1, m1>> @@ <<n1, m2>> :> <<n1, m2>> @@ <<n2, m1>> :> <<n2, m1>> @@ <<n2, m2>> :> <<n2, m2>>),gcPhase |-> (L1 :> "retiring"),coordFence |-> (n1 :> 0 @@ n2 :> 0),reducerOwner |-> (s1 :> L1),mEntries |-> (<<n1, m1>> :> (p1 :> b1) @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),attViewable |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),retired |-> {},gcRound |-> 1,blobIndeg |-> (b1 :> 0),mPrefix |-> (<<n1, m1>> :> bp1 @@ <<n1, m2>> :> bp1 @@ <<n2, m1>> :> bp1 @@ <<n2, m2>> :> bp1),extraShared |-> {},owner |-> (<<n1, m1>> :> r1 @@ <<n1, m2>> :> "none" @@ <<n2, m1>> :> "none" @@ <<n2, m2>> :> "none"),foldSeal |-> (0 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 1 :> [foldedCursor |-> (n1 :> 1 @@ n2 :> 0), classified |-> {}] @@ 2 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}]),prevFencePos |-> (n1 :> 0 @@ n2 :> 0),sealAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),everEdged |-> {<<n1, m1>>},fenceVersion |-> (0 :> (n1 :> 0 @@ n2 :> 0) @@ 1 :> (n1 :> 0 @@ n2 :> 0) @@ 2 :> (n1 :> 0 @@ n2 :> 0)),mActiveEdges |-> (<<n1, m1>> :> (p1 :> "noblob") @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),completionSeal |-> (0 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 1 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 2 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE]),mfCleanup |-> {},inflight |-> {},mBody |-> (<<n1, m1>> :> TRUE @@ <<n1, m2>> :> FALSE @@ <<n2, m1>> :> FALSE @@ <<n2, m2>> :> FALSE),retiredAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),foldedTok |-> (n1 :> 0 @@ n2 :> 0),present |-> (b1 :> TRUE),mfDeleted |-> {},adopted |-> (0 :> 0 @@ 1 :> 0 @@ 2 :> 0)]),
    ([cursor |-> (n1 :> 1 @@ n2 :> 0),deadTok |-> (b1 :> {}),blobEdges |-> {},shardIndeg |-> (s1 :> (b1 :> 0)),listedTok |-> (n1 :> 0 @@ n2 :> 0),attemptSeq |-> 0,fencePos |-> (n1 :> 0 @@ n2 :> 0),trimBase |-> (n1 :> 0 @@ n2 :> 0),wView |-> (w1 :> 0),foldTok |-> (n1 :> 0 @@ n2 :> 0),nextTok |-> (b1 :> 2),storedTok |-> (b1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),sweepEligible |-> (bp1 :> FALSE),mNs |-> (<<n1, m1>> :> n2 @@ <<n1, m2>> :> n1 @@ <<n2, m1>> :> n2 @@ <<n2, m2>> :> n2),journal |-> (n1 :> <<[old |-> {}, new |-> {<<n1, m1>>}, ver |-> 1, ref |-> r1]>> @@ n2 :> <<>>),mRef |-> (<<n1, m1>> :> <<n1, m1>> @@ <<n1, m2>> :> <<n1, m2>> @@ <<n2, m1>> :> <<n2, m1>> @@ <<n2, m2>> :> <<n2, m2>>),gcPhase |-> (L1 :> "retiring"),coordFence |-> (n1 :> 0 @@ n2 :> 0),reducerOwner |-> (s1 :> L1),mEntries |-> (<<n1, m1>> :> (p1 :> b1) @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),attViewable |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),retired |-> {[b |-> b1, t |-> 1, r |-> 1]},gcRound |-> 1,blobIndeg |-> (b1 :> 0),mPrefix |-> (<<n1, m1>> :> bp1 @@ <<n1, m2>> :> bp1 @@ <<n2, m1>> :> bp1 @@ <<n2, m2>> :> bp1),extraShared |-> {},owner |-> (<<n1, m1>> :> r1 @@ <<n1, m2>> :> "none" @@ <<n2, m1>> :> "none" @@ <<n2, m2>> :> "none"),foldSeal |-> (0 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 1 :> [foldedCursor |-> (n1 :> 1 @@ n2 :> 0), classified |-> {}] @@ 2 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}]),prevFencePos |-> (n1 :> 0 @@ n2 :> 0),sealAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),everEdged |-> {<<n1, m1>>},fenceVersion |-> (0 :> (n1 :> 0 @@ n2 :> 0) @@ 1 :> (n1 :> 0 @@ n2 :> 0) @@ 2 :> (n1 :> 0 @@ n2 :> 0)),mActiveEdges |-> (<<n1, m1>> :> (p1 :> "noblob") @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),completionSeal |-> (0 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 1 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 2 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE]),mfCleanup |-> {},inflight |-> {},mBody |-> (<<n1, m1>> :> TRUE @@ <<n1, m2>> :> FALSE @@ <<n2, m1>> :> FALSE @@ <<n2, m2>> :> FALSE),retiredAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),foldedTok |-> (n1 :> 0 @@ n2 :> 0),present |-> (b1 :> TRUE),mfDeleted |-> {},adopted |-> (0 :> 0 @@ 1 :> 0 @@ 2 :> 0)]),
    ([cursor |-> (n1 :> 1 @@ n2 :> 0),deadTok |-> (b1 :> {}),blobEdges |-> {},shardIndeg |-> (s1 :> (b1 :> 0)),listedTok |-> (n1 :> 0 @@ n2 :> 0),attemptSeq |-> 0,fencePos |-> (n1 :> 0 @@ n2 :> 0),trimBase |-> (n1 :> 0 @@ n2 :> 0),wView |-> (w1 :> 0),foldTok |-> (n1 :> 0 @@ n2 :> 0),nextTok |-> (b1 :> 2),storedTok |-> (b1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),sweepEligible |-> (bp1 :> FALSE),mNs |-> (<<n1, m1>> :> n2 @@ <<n1, m2>> :> n1 @@ <<n2, m1>> :> n2 @@ <<n2, m2>> :> n2),journal |-> (n1 :> <<[old |-> {}, new |-> {<<n1, m1>>}, ver |-> 1, ref |-> r1]>> @@ n2 :> <<>>),mRef |-> (<<n1, m1>> :> <<n1, m1>> @@ <<n1, m2>> :> <<n1, m2>> @@ <<n2, m1>> :> <<n2, m1>> @@ <<n2, m2>> :> <<n2, m2>>),gcPhase |-> (L1 :> "fencing"),coordFence |-> (n1 :> 0 @@ n2 :> 0),reducerOwner |-> (s1 :> L1),mEntries |-> (<<n1, m1>> :> (p1 :> b1) @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),attViewable |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),retired |-> {[b |-> b1, t |-> 1, r |-> 1]},gcRound |-> 1,blobIndeg |-> (b1 :> 0),mPrefix |-> (<<n1, m1>> :> bp1 @@ <<n1, m2>> :> bp1 @@ <<n2, m1>> :> bp1 @@ <<n2, m2>> :> bp1),extraShared |-> {},owner |-> (<<n1, m1>> :> r1 @@ <<n1, m2>> :> "none" @@ <<n2, m1>> :> "none" @@ <<n2, m2>> :> "none"),foldSeal |-> (0 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 1 :> [foldedCursor |-> (n1 :> 1 @@ n2 :> 0), classified |-> {}] @@ 2 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}]),prevFencePos |-> (n1 :> 0 @@ n2 :> 0),sealAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),everEdged |-> {<<n1, m1>>},fenceVersion |-> (0 :> (n1 :> 0 @@ n2 :> 0) @@ 1 :> (n1 :> 0 @@ n2 :> 0) @@ 2 :> (n1 :> 0 @@ n2 :> 0)),mActiveEdges |-> (<<n1, m1>> :> (p1 :> "noblob") @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),completionSeal |-> (0 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 1 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 2 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE]),mfCleanup |-> {},inflight |-> {},mBody |-> (<<n1, m1>> :> TRUE @@ <<n1, m2>> :> FALSE @@ <<n2, m1>> :> FALSE @@ <<n2, m2>> :> FALSE),retiredAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),foldedTok |-> (n1 :> 0 @@ n2 :> 0),present |-> (b1 :> TRUE),mfDeleted |-> {},adopted |-> (0 :> 0 @@ 1 :> 0 @@ 2 :> 0)]),
    ([cursor |-> (n1 :> 1 @@ n2 :> 0),deadTok |-> (b1 :> {}),blobEdges |-> {},shardIndeg |-> (s1 :> (b1 :> 0)),listedTok |-> (n1 :> 0 @@ n2 :> 0),attemptSeq |-> 0,fencePos |-> (n1 :> 1 @@ n2 :> 0),trimBase |-> (n1 :> 0 @@ n2 :> 0),wView |-> (w1 :> 0),foldTok |-> (n1 :> 0 @@ n2 :> 0),nextTok |-> (b1 :> 2),storedTok |-> (b1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),sweepEligible |-> (bp1 :> FALSE),mNs |-> (<<n1, m1>> :> n2 @@ <<n1, m2>> :> n1 @@ <<n2, m1>> :> n2 @@ <<n2, m2>> :> n2),journal |-> (n1 :> <<[old |-> {}, new |-> {<<n1, m1>>}, ver |-> 1, ref |-> r1]>> @@ n2 :> <<>>),mRef |-> (<<n1, m1>> :> <<n1, m1>> @@ <<n1, m2>> :> <<n1, m2>> @@ <<n2, m1>> :> <<n2, m1>> @@ <<n2, m2>> :> <<n2, m2>>),gcPhase |-> (L1 :> "fencing"),coordFence |-> (n1 :> 0 @@ n2 :> 0),reducerOwner |-> (s1 :> L1),mEntries |-> (<<n1, m1>> :> (p1 :> b1) @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),attViewable |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),retired |-> {[b |-> b1, t |-> 1, r |-> 1]},gcRound |-> 1,blobIndeg |-> (b1 :> 0),mPrefix |-> (<<n1, m1>> :> bp1 @@ <<n1, m2>> :> bp1 @@ <<n2, m1>> :> bp1 @@ <<n2, m2>> :> bp1),extraShared |-> {},owner |-> (<<n1, m1>> :> r1 @@ <<n1, m2>> :> "none" @@ <<n2, m1>> :> "none" @@ <<n2, m2>> :> "none"),foldSeal |-> (0 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 1 :> [foldedCursor |-> (n1 :> 1 @@ n2 :> 0), classified |-> {}] @@ 2 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}]),prevFencePos |-> (n1 :> 0 @@ n2 :> 0),sealAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),everEdged |-> {<<n1, m1>>},fenceVersion |-> (0 :> (n1 :> 0 @@ n2 :> 0) @@ 1 :> (n1 :> 1 @@ n2 :> 0) @@ 2 :> (n1 :> 0 @@ n2 :> 0)),mActiveEdges |-> (<<n1, m1>> :> (p1 :> "noblob") @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),completionSeal |-> (0 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 1 :> [fenced |-> {n1}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 2 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE]),mfCleanup |-> {},inflight |-> {},mBody |-> (<<n1, m1>> :> TRUE @@ <<n1, m2>> :> FALSE @@ <<n2, m1>> :> FALSE @@ <<n2, m2>> :> FALSE),retiredAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),foldedTok |-> (n1 :> 0 @@ n2 :> 0),present |-> (b1 :> TRUE),mfDeleted |-> {},adopted |-> (0 :> 0 @@ 1 :> 0 @@ 2 :> 0)]),
    ([cursor |-> (n1 :> 1 @@ n2 :> 0),deadTok |-> (b1 :> {}),blobEdges |-> {},shardIndeg |-> (s1 :> (b1 :> 0)),listedTok |-> (n1 :> 0 @@ n2 :> 0),attemptSeq |-> 0,fencePos |-> (n1 :> 1 @@ n2 :> 0),trimBase |-> (n1 :> 0 @@ n2 :> 0),wView |-> (w1 :> 0),foldTok |-> (n1 :> 0 @@ n2 :> 0),nextTok |-> (b1 :> 2),storedTok |-> (b1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),sweepEligible |-> (bp1 :> FALSE),mNs |-> (<<n1, m1>> :> n2 @@ <<n1, m2>> :> n1 @@ <<n2, m1>> :> n2 @@ <<n2, m2>> :> n2),journal |-> (n1 :> <<[old |-> {}, new |-> {<<n1, m1>>}, ver |-> 1, ref |-> r1]>> @@ n2 :> <<>>),mRef |-> (<<n1, m1>> :> <<n1, m1>> @@ <<n1, m2>> :> <<n1, m2>> @@ <<n2, m1>> :> <<n2, m1>> @@ <<n2, m2>> :> <<n2, m2>>),gcPhase |-> (L1 :> "fenced"),coordFence |-> (n1 :> 0 @@ n2 :> 0),reducerOwner |-> (s1 :> L1),mEntries |-> (<<n1, m1>> :> (p1 :> b1) @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),attViewable |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),retired |-> {[b |-> b1, t |-> 1, r |-> 1]},gcRound |-> 1,blobIndeg |-> (b1 :> 0),mPrefix |-> (<<n1, m1>> :> bp1 @@ <<n1, m2>> :> bp1 @@ <<n2, m1>> :> bp1 @@ <<n2, m2>> :> bp1),extraShared |-> {},owner |-> (<<n1, m1>> :> r1 @@ <<n1, m2>> :> "none" @@ <<n2, m1>> :> "none" @@ <<n2, m2>> :> "none"),foldSeal |-> (0 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 1 :> [foldedCursor |-> (n1 :> 1 @@ n2 :> 0), classified |-> {}] @@ 2 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}]),prevFencePos |-> (n1 :> 0 @@ n2 :> 0),sealAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),everEdged |-> {<<n1, m1>>},fenceVersion |-> (0 :> (n1 :> 0 @@ n2 :> 0) @@ 1 :> (n1 :> 1 @@ n2 :> 1) @@ 2 :> (n1 :> 0 @@ n2 :> 0)),mActiveEdges |-> (<<n1, m1>> :> (p1 :> "noblob") @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),completionSeal |-> (0 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 1 :> [fenced |-> {n1, n2}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 2 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE]),mfCleanup |-> {},inflight |-> {},mBody |-> (<<n1, m1>> :> TRUE @@ <<n1, m2>> :> FALSE @@ <<n2, m1>> :> FALSE @@ <<n2, m2>> :> FALSE),retiredAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),foldedTok |-> (n1 :> 0 @@ n2 :> 0),present |-> (b1 :> TRUE),mfDeleted |-> {},adopted |-> (0 :> 0 @@ 1 :> 0 @@ 2 :> 0)]),
    ([cursor |-> (n1 :> 1 @@ n2 :> 0),deadTok |-> (b1 :> {}),blobEdges |-> {},shardIndeg |-> (s1 :> (b1 :> 0)),listedTok |-> (n1 :> 0 @@ n2 :> 0),attemptSeq |-> 0,fencePos |-> (n1 :> 1 @@ n2 :> 0),trimBase |-> (n1 :> 0 @@ n2 :> 0),wView |-> (w1 :> 0),foldTok |-> (n1 :> 0 @@ n2 :> 0),nextTok |-> (b1 :> 2),storedTok |-> (b1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),sweepEligible |-> (bp1 :> FALSE),mNs |-> (<<n1, m1>> :> n2 @@ <<n1, m2>> :> n1 @@ <<n2, m1>> :> n2 @@ <<n2, m2>> :> n2),journal |-> (n1 :> <<[old |-> {}, new |-> {<<n1, m1>>}, ver |-> 1, ref |-> r1]>> @@ n2 :> <<>>),mRef |-> (<<n1, m1>> :> <<n1, m1>> @@ <<n1, m2>> :> <<n1, m2>> @@ <<n2, m1>> :> <<n2, m1>> @@ <<n2, m2>> :> <<n2, m2>>),gcPhase |-> (L1 :> "fenced"),coordFence |-> (n1 :> 0 @@ n2 :> 0),reducerOwner |-> (s1 :> L1),mEntries |-> (<<n1, m1>> :> (p1 :> b1) @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),attViewable |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),retired |-> {[b |-> b1, t |-> 1, r |-> 1]},gcRound |-> 1,blobIndeg |-> (b1 :> 0),mPrefix |-> (<<n1, m1>> :> bp1 @@ <<n1, m2>> :> bp1 @@ <<n2, m1>> :> bp1 @@ <<n2, m2>> :> bp1),extraShared |-> {},owner |-> (<<n1, m1>> :> r1 @@ <<n1, m2>> :> "none" @@ <<n2, m1>> :> "none" @@ <<n2, m2>> :> "none"),foldSeal |-> (0 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 1 :> [foldedCursor |-> (n1 :> 1 @@ n2 :> 0), classified |-> {}] @@ 2 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}]),prevFencePos |-> (n1 :> 0 @@ n2 :> 0),sealAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),everEdged |-> {<<n1, m1>>},fenceVersion |-> (0 :> (n1 :> 0 @@ n2 :> 0) @@ 1 :> (n1 :> 1 @@ n2 :> 1) @@ 2 :> (n1 :> 0 @@ n2 :> 0)),mActiveEdges |-> (<<n1, m1>> :> (p1 :> "noblob") @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),completionSeal |-> (0 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 1 :> [fenced |-> {n1, n2}, rechecked |-> {}, deleted |-> {b1}, adoptable |-> FALSE] @@ 2 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE]),mfCleanup |-> {},inflight |-> {[b |-> b1, t |-> 1]},mBody |-> (<<n1, m1>> :> TRUE @@ <<n1, m2>> :> FALSE @@ <<n2, m1>> :> FALSE @@ <<n2, m2>> :> FALSE),retiredAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),foldedTok |-> (n1 :> 0 @@ n2 :> 0),present |-> (b1 :> TRUE),mfDeleted |-> {},adopted |-> (0 :> 0 @@ 1 :> 0 @@ 2 :> 0)]),
    ([cursor |-> (n1 :> 1 @@ n2 :> 0),deadTok |-> (b1 :> {1}),blobEdges |-> {},shardIndeg |-> (s1 :> (b1 :> 0)),listedTok |-> (n1 :> 0 @@ n2 :> 0),attemptSeq |-> 0,fencePos |-> (n1 :> 1 @@ n2 :> 0),trimBase |-> (n1 :> 0 @@ n2 :> 0),wView |-> (w1 :> 0),foldTok |-> (n1 :> 0 @@ n2 :> 0),nextTok |-> (b1 :> 2),storedTok |-> (b1 :> 0),roundOf |-> (L1 :> 1),tokOf |-> (b1 :> 1),sweepEligible |-> (bp1 :> FALSE),mNs |-> (<<n1, m1>> :> n2 @@ <<n1, m2>> :> n1 @@ <<n2, m1>> :> n2 @@ <<n2, m2>> :> n2),journal |-> (n1 :> <<[old |-> {}, new |-> {<<n1, m1>>}, ver |-> 1, ref |-> r1]>> @@ n2 :> <<>>),mRef |-> (<<n1, m1>> :> <<n1, m1>> @@ <<n1, m2>> :> <<n1, m2>> @@ <<n2, m1>> :> <<n2, m1>> @@ <<n2, m2>> :> <<n2, m2>>),gcPhase |-> (L1 :> "fenced"),coordFence |-> (n1 :> 0 @@ n2 :> 0),reducerOwner |-> (s1 :> L1),mEntries |-> (<<n1, m1>> :> (p1 :> b1) @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),attViewable |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),retired |-> {},gcRound |-> 1,blobIndeg |-> (b1 :> 0),mPrefix |-> (<<n1, m1>> :> bp1 @@ <<n1, m2>> :> bp1 @@ <<n2, m1>> :> bp1 @@ <<n2, m2>> :> bp1),extraShared |-> {},owner |-> (<<n1, m1>> :> r1 @@ <<n1, m2>> :> "none" @@ <<n2, m1>> :> "none" @@ <<n2, m2>> :> "none"),foldSeal |-> (0 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}] @@ 1 :> [foldedCursor |-> (n1 :> 1 @@ n2 :> 0), classified |-> {}] @@ 2 :> [foldedCursor |-> (n1 :> 0 @@ n2 :> 0), classified |-> {}]),prevFencePos |-> (n1 :> 0 @@ n2 :> 0),sealAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),everEdged |-> {<<n1, m1>>},fenceVersion |-> (0 :> (n1 :> 0 @@ n2 :> 0) @@ 1 :> (n1 :> 1 @@ n2 :> 1) @@ 2 :> (n1 :> 0 @@ n2 :> 0)),mActiveEdges |-> (<<n1, m1>> :> (p1 :> "noblob") @@ <<n1, m2>> :> (p1 :> "noblob") @@ <<n2, m1>> :> (p1 :> "noblob") @@ <<n2, m2>> :> (p1 :> "noblob")),completionSeal |-> (0 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE] @@ 1 :> [fenced |-> {n1, n2}, rechecked |-> {}, deleted |-> {b1}, adoptable |-> FALSE] @@ 2 :> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE]),mfCleanup |-> {},inflight |-> {},mBody |-> (<<n1, m1>> :> TRUE @@ <<n1, m2>> :> FALSE @@ <<n2, m1>> :> FALSE @@ <<n2, m2>> :> FALSE),retiredAt |-> (0 :> {} @@ 1 :> {} @@ 2 :> {}),foldedTok |-> (n1 :> 0 @@ n2 :> 0),present |-> (b1 :> FALSE),mfDeleted |-> {},adopted |-> (0 :> 0 @@ 1 :> 0 @@ 2 :> 0)])
    >>
----


=============================================================================

---- MODULE CaGcRootLocalPartManifestCore_TEConstants ----
EXTENDS CaGcRootLocalPartManifestCore

CONSTANTS n1, n2, w1, L1, b1, m1, m2, r1, bd1, p1, bp1, s1

=============================================================================

---- CONFIG CaGcRootLocalPartManifestCore_TTrace_1785659096 ----
CONSTANTS
    Namespaces = { n1 , n2 }
    Writers = { w1 }
    Leaders = { L1 }
    Blobs = { b1 }
    ManifestInstances = { m1 , m2 }
    Refs = { r1 }
    Builds = { bd1 }
    Paths = { p1 }
    BuildPrefixes = { bp1 }
    MaxToken = 2
    MaxRound = 2
    MaxLog = 3
    EnablePrecommit = TRUE
    EnableMissingBody = FALSE
    EnableOrphanSweep = FALSE
    EnableMutablePayload = FALSE
    SabotageReuseManifestId = FALSE
    SabotageTwoOwners = FALSE
    SabotageSplitPromote = FALSE
    SabotageMissingBodyActivated = FALSE
    SabotageCommitSkipBlobReval = FALSE
    SabotagePrecommitlessProtect = FALSE
    SabotageNoOrphanSweep = FALSE
    SabotageWholesalePrefixDelete = FALSE
    SabotageFrozenSeqAuthority = FALSE
    SabotageMissingCommittedEmpty = FALSE
    SabotageDeleteBodyBeforeDecrements = FALSE
    SabotageCutOverclaim = FALSE
    SabotageRoundVisibilityEarly = FALSE
    SabotageNoFence = FALSE
    SabotageTrimUnincorporated = FALSE
    SabotageUncondDelete = FALSE
    SabotageReusedTag = FALSE
    SabotageBareNonce = FALSE
    SabotageKeyByRefNotId = FALSE
    SabotageAcceptNamespaceMismatch = TRUE
    SabotageAcceptRefMismatch = FALSE
    SabotageMutableAsReachability = FALSE
    SabotagePromoteAfterMissingBody = FALSE
    SabotageAdvancePastMissingBodyPrecommit = FALSE
    EnableTokenDiff = FALSE
    TokenObservable = FALSE
    SabotageSkipChangedShard = FALSE
    SabotageSkipParksDeadPrecommit = FALSE
    EnableLazyTrim = FALSE
    SabotageLazyFenceUnsafe = FALSE
    Shards = { s1 }
    EnableSharding = FALSE
    SabotageReducerOwnsFence = FALSE
    SabotageCrossShardDisplacement = FALSE
    EnableRetireTokenSource = FALSE
    SabotageStaleTokenOverDelete = FALSE
    EnableAttemptScoping = FALSE
    SabotageDeposedLeaderWritesFinalGen = FALSE
    MaxAttempt = 2
    bd1 = bd1
    bp1 = bp1
    r1 = r1
    p1 = p1
    s1 = s1
    n2 = n2
    L1 = L1
    m1 = m1
    n1 = n1
    w1 = w1
    b1 = b1
    m2 = m2

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
\* Generated on Sun Aug 02 10:25:42 CEST 2026