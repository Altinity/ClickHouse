----------------------------- MODULE CaBlobPublishCore -----------------------------
(*
Focused safety model for content-addressed blob publication.

Two writers share one content-addressed key and one GC actor. A writer separates body
observation, metadata observation, publication selection, backend landing, response
delivery, and recovery. This split exposes racing misses and a backend write that lands
after recovery has begun.

ETag tokens are a deterministic function of the complete envelope and logical payload.
Generation tokens consume nextToken at every backend landing. Both token families use
the same queued exact-delete action, while post-condemnation freshness compares the
envelope identity directly.
*)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Writers,
    WriterOne,
    WriterTwo,
    TokenFamilies,
    MaxAttempts,
    MaxWrites,
    MaxEnvelope,
    MaxToken,
    MaxMetaVersion,
    MaxFence,
    Defects

KeyPayload == "Content"
WrongPayload == "WrongContent"
NoPayload == "NoPayload"
Payloads == {KeyPayload, WrongPayload}

NoToken == <<"NoToken", 0, NoPayload>>
ETagToken(envelope, payload) == <<"ETag", envelope, payload>>
GenerationToken(token) == <<"Generation", token, NoPayload>>

TokenSet ==
    {NoToken}
    \cup {ETagToken(envelope, payload)
            : envelope \in 1..MaxEnvelope, payload \in Payloads}
    \cup {GenerationToken(token) : token \in 1..MaxToken}

Phases ==
    {"Precommit", "Head", "MetaRead", "Publish", "Publishing", "ResponseLost",
     "MetaClean", "Ready", "Committed", "Aborted"}
Precommits == {"Absent", "Durable"}
Observations == {"None", "Absent", "Present", "Clean", "Condemned"}
MetaStates == {"Absent", "Clean", "Condemned"}
ObservedMetaStates == MetaStates \cup {"Unread"}
Transports == {"None", "VerbatimCopy", "RetaggedStream"}

AllowedDefects ==
    {"adopt_condemned",
     "reuse_condemned_envelope",
     "recopy_after_condemned",
     "recopy_after_absent",
     "first_condemned_then_copy",
     "unconditional_delete",
     "ready_without_reobserve",
     "publish_before_precommit",
     "skip_meta_clean",
     "commit_after_fence",
     "wrong_payload"}

Defect(name) == name \in Defects

StagedEnvelope(w) == IF w = WriterOne THEN 1 ELSE 2
SourcePayload(w) ==
    IF Defect("wrong_payload") /\ w = WriterOne THEN WrongPayload ELSE KeyPayload

BodyState ==
    [present : BOOLEAN,
     payload : Payloads \cup {NoPayload},
     envelope : 0..MaxEnvelope,
     token : TokenSet,
     serial : 0..MaxWrites]

DeleteRecordSet ==
    [token : TokenSet,
     serial : 1..MaxWrites]

WriterState ==
    [phase : Phases,
     precommit : Precommits,
     observation : Observations,
     observedPayload : Payloads \cup {NoPayload},
     observedEnvelope : 0..MaxEnvelope,
     observedToken : TokenSet,
     observedWriteSeq : 0..MaxWrites,
     observedMeta : ObservedMetaStates,
     decisionFence : 0..MaxFence,
     ready : BOOLEAN,
     committed : BOOLEAN,
     materialized : BOOLEAN,
     cleanProof : BOOLEAN,
     publicationAttempted : BOOLEAN,
     everPublicationAttempted : BOOLEAN,
     attemptCount : 0..MaxAttempts,
     plannedTransport : Transports,
     plannedEnvelope : 0..MaxEnvelope,
     plannedObservation : Observations,
     plannedOldToken : TokenSet,
     plannedOldEnvelope : 0..MaxEnvelope,
     plannedObservedSeq : 0..MaxWrites,
     plannedInvalidVerbatimCopy : BOOLEAN,
     attemptLanded : BOOLEAN,
     latePending : BOOLEAN,
     lateTransport : Transports,
     lateEnvelope : 0..MaxEnvelope,
     lateObservation : Observations,
     lateOldToken : TokenSet,
     lateOldEnvelope : 0..MaxEnvelope,
     lateObservedSeq : 0..MaxWrites,
     lateInvalidVerbatimCopy : BOOLEAN,
     needsFreshPublication : BOOLEAN,
     adoptedCondemned : BOOLEAN,
     invalidVerbatimCopy : BOOLEAN,
     publishedWithoutPrecommit : BOOLEAN,
     fenced : BOOLEAN,
     sawResponseLost : BOOLEAN,
     sawRecoveryHead : BOOLEAN,
     sawStagedCopy : BOOLEAN,
     publishedFromAbsent : BOOLEAN,
     publishedAbsentSeq : 0..MaxWrites,
     retryLandedBeforeLate : BOOLEAN,
     sawCondemnedAttempt : BOOLEAN]

VARIABLES
    tokenFamily,
    fenceGeneration,
    body,
    nextToken,
    nextEnvelope,
    writeSeq,
    metaState,
    metaVersion,
    queuedDeletes,
    seeded,
    writer,
    badFreshAfterCondemned,
    freshIncarnationDeleted,
    racingPublishersReached,
    stagedRetagReached,
    lateLandingReached

vars ==
    <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
      metaState, metaVersion, queuedDeletes, seeded, writer,
      badFreshAfterCondemned, freshIncarnationDeleted,
      racingPublishersReached, stagedRetagReached, lateLandingReached>>

EmptyBody ==
    [present |-> FALSE,
     payload |-> NoPayload,
     envelope |-> 0,
     token |-> NoToken,
     serial |-> 0]

InitialWriter ==
    [phase |-> "Precommit",
     precommit |-> "Absent",
     observation |-> "None",
     observedPayload |-> NoPayload,
     observedEnvelope |-> 0,
     observedToken |-> NoToken,
     observedWriteSeq |-> 0,
     observedMeta |-> "Unread",
     decisionFence |-> 0,
     ready |-> FALSE,
     committed |-> FALSE,
     materialized |-> FALSE,
     cleanProof |-> FALSE,
     publicationAttempted |-> FALSE,
     everPublicationAttempted |-> FALSE,
     attemptCount |-> 0,
     plannedTransport |-> "None",
     plannedEnvelope |-> 0,
     plannedObservation |-> "None",
     plannedOldToken |-> NoToken,
     plannedOldEnvelope |-> 0,
     plannedObservedSeq |-> 0,
     plannedInvalidVerbatimCopy |-> FALSE,
     attemptLanded |-> FALSE,
     latePending |-> FALSE,
     lateTransport |-> "None",
     lateEnvelope |-> 0,
     lateObservation |-> "None",
     lateOldToken |-> NoToken,
     lateOldEnvelope |-> 0,
     lateObservedSeq |-> 0,
     lateInvalidVerbatimCopy |-> FALSE,
     needsFreshPublication |-> FALSE,
     adoptedCondemned |-> FALSE,
     invalidVerbatimCopy |-> FALSE,
     publishedWithoutPrecommit |-> FALSE,
     fenced |-> FALSE,
     sawResponseLost |-> FALSE,
     sawRecoveryHead |-> FALSE,
     sawStagedCopy |-> FALSE,
     publishedFromAbsent |-> FALSE,
     publishedAbsentSeq |-> 0,
     retryLandedBeforeLate |-> FALSE,
     sawCondemnedAttempt |-> FALSE]

Init ==
    /\ tokenFamily \in TokenFamilies
    /\ fenceGeneration = 1
    /\ body = EmptyBody
    /\ nextToken = 1
    /\ nextEnvelope = 3
    /\ writeSeq = 0
    /\ metaState = "Absent"
    /\ metaVersion = 0
    /\ queuedDeletes = {}
    /\ seeded = FALSE
    /\ writer = [w \in Writers |-> InitialWriter]
    /\ badFreshAfterCondemned = FALSE
    /\ freshIncarnationDeleted = FALSE
    /\ racingPublishersReached = FALSE
    /\ stagedRetagReached = FALSE
    /\ lateLandingReached = FALSE

MintToken(envelope, payload) ==
    IF tokenFamily = "ETag"
    THEN ETagToken(envelope, payload)
    ELSE GenerationToken(nextToken)

NextTokenAfterLanding ==
    IF tokenFamily = "Generation" THEN nextToken + 1 ELSE nextToken

CanLand ==
    /\ writeSeq < MaxWrites
    /\ (tokenFamily = "ETag" \/ nextToken <= MaxToken)

OtherAbsentPublisherReached(w, observedSeq) ==
    \E other \in Writers \ {w} :
        /\ writer[other].publishedFromAbsent
        /\ writer[other].publishedAbsentSeq = observedSeq
        /\ SourcePayload(other) = SourcePayload(w)

TypeOK ==
    /\ Writers = {WriterOne, WriterTwo}
    /\ WriterOne /= WriterTwo
    /\ TokenFamilies \subseteq {"ETag", "Generation"}
    /\ TokenFamilies /= {}
    /\ Defects \subseteq AllowedDefects
    /\ Cardinality(Defects) <= 1
    /\ MaxAttempts = 2
    /\ MaxWrites >= 2
    /\ MaxEnvelope >= 6
    /\ MaxToken >= MaxWrites
    /\ MaxMetaVersion >= 4
    /\ MaxFence >= 2
    /\ tokenFamily \in TokenFamilies
    /\ fenceGeneration \in 1..MaxFence
    /\ body \in BodyState
    /\ (body.present
        => /\ body.payload \in Payloads
           /\ body.envelope \in 1..MaxEnvelope
           /\ body.token /= NoToken
           /\ body.serial \in 1..MaxWrites)
    /\ (~body.present => body = EmptyBody)
    /\ nextToken \in 1..(MaxToken + 1)
    /\ nextEnvelope \in 3..MaxEnvelope
    /\ writeSeq \in 0..MaxWrites
    /\ metaState \in MetaStates
    /\ metaVersion \in 0..MaxMetaVersion
    /\ queuedDeletes \subseteq DeleteRecordSet
    /\ seeded \in BOOLEAN
    /\ writer \in [Writers -> WriterState]
    /\ \A w \in Writers :
        /\ (writer[w].ready => writer[w].phase \in {"Ready", "Committed"})
        /\ (writer[w].committed => writer[w].ready /\ writer[w].phase = "Committed")
        /\ (writer[w].publicationAttempted <=> writer[w].attemptCount > 0)
        /\ (writer[w].latePending => writer[w].publicationAttempted)
    /\ badFreshAfterCondemned \in BOOLEAN
    /\ freshIncarnationDeleted \in BOOLEAN
    /\ racingPublishersReached \in BOOLEAN
    /\ stagedRetagReached \in BOOLEAN
    /\ lateLandingReached \in BOOLEAN

(*
The honest seed envelope is disjoint from both current staged sources. The first-condemned
sabotage aliases writer one's staged bytes intentionally so its queued old ETag is reproduced.
*)
SeedBody ==
    /\ ~seeded
    /\ ~body.present
    /\ writeSeq = 0
    /\ \A w \in Writers : writer[w].phase = "Precommit"
    /\ metaVersion < MaxMetaVersion
    /\ CanLand
    /\ LET envelope ==
               IF Defect("first_condemned_then_copy")
               THEN StagedEnvelope(WriterOne)
               ELSE MaxEnvelope
           payload == KeyPayload
           token == MintToken(envelope, payload)
       IN /\ body' =
              [present |-> TRUE,
               payload |-> payload,
               envelope |-> envelope,
               token |-> token,
               serial |-> writeSeq + 1]
          /\ nextToken' = NextTokenAfterLanding
    /\ writeSeq' = writeSeq + 1
    /\ metaState' = "Clean"
    /\ metaVersion' = metaVersion + 1
    /\ seeded' = TRUE
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, nextEnvelope, queuedDeletes, writer,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

DurablePrecommit(w) ==
    /\ writer[w].phase = "Precommit"
    /\ writer[w].precommit = "Absent"
    /\ writer' =
        [writer EXCEPT
            ![w].phase = "Head",
            ![w].precommit = "Durable"]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

SkipPrecommit(w) ==
    /\ Defect("publish_before_precommit")
    /\ writer[w].phase = "Precommit"
    /\ writer[w].precommit = "Absent"
    /\ writer' = [writer EXCEPT ![w].phase = "Head"]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

ObserveBody(w) ==
    /\ writer[w].phase = "Head"
    /\ writer' =
        [writer EXCEPT
            ![w].phase = IF body.present THEN "MetaRead" ELSE "Publish",
            ![w].observation = IF body.present THEN "Present" ELSE "Absent",
            ![w].observedPayload = body.payload,
            ![w].observedEnvelope = body.envelope,
            ![w].observedToken = body.token,
            ![w].observedWriteSeq = writeSeq,
            ![w].observedMeta = "Unread",
            ![w].decisionFence = fenceGeneration,
            ![w].materialized = FALSE,
            ![w].cleanProof = FALSE,
            ![w].needsFreshPublication = FALSE,
            ![w].sawRecoveryHead =
                @ \/ writer[w].sawResponseLost]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

ObserveMeta(w) ==
    /\ writer[w].phase = "MetaRead"
    /\ writer[w].precommit = "Durable"
    /\ body.present
    /\ IF metaState = "Condemned"
       THEN
           writer' =
               [writer EXCEPT
                   ![w].phase = "Publish",
                   ![w].observation = "Condemned",
                   ![w].observedPayload = body.payload,
                   ![w].observedEnvelope = body.envelope,
                   ![w].observedToken = body.token,
                   ![w].observedWriteSeq = writeSeq,
                   ![w].observedMeta = "Condemned",
                   ![w].needsFreshPublication = TRUE]
       ELSE IF metaState = "Clean"
       THEN
           writer' =
               [writer EXCEPT
                   ![w].phase = "Ready",
                   ![w].observation = "Clean",
                   ![w].observedMeta = "Clean",
                   ![w].ready = TRUE,
                   ![w].materialized = TRUE,
                   ![w].cleanProof = TRUE]
       ELSE
           writer' =
               [writer EXCEPT
                   ![w].phase = "MetaClean",
                   ![w].observation = "Clean",
                   ![w].observedMeta = "Absent"]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

ObservationInvalidated(w) ==
    /\ writer[w].phase = "MetaRead"
    /\ ~body.present
    /\ writer' = [writer EXCEPT ![w].phase = "Head"]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

AdoptCondemned(w) ==
    /\ Defect("adopt_condemned")
    /\ writer[w].phase = "MetaRead"
    /\ writer[w].precommit = "Durable"
    /\ body.present
    /\ metaState = "Condemned"
    /\ writer' =
        [writer EXCEPT
            ![w].phase = "Ready",
            ![w].observation = "Condemned",
            ![w].observedMeta = "Condemned",
            ![w].ready = TRUE,
            ![w].materialized = TRUE,
            ![w].cleanProof = TRUE,
            ![w].needsFreshPublication = TRUE,
            ![w].adoptedCondemned = TRUE]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

BeginPublication(w) ==
    /\ writer[w].phase = "Publish"
    /\ writer[w].attemptCount < MaxAttempts
    /\ ~writer[w].fenced
    /\ writer[w].decisionFence = fenceGeneration
    /\ (writer[w].precommit = "Durable" \/ Defect("publish_before_precommit"))
    /\ LET priorAttempted == writer[w].publicationAttempted
           observation == writer[w].observation
           safeCopy == ~priorAttempted /\ observation = "Absent"
           defectiveCopy ==
               \/ Defect("recopy_after_condemned")
                  /\ priorAttempted
                  /\ observation = "Condemned"
               \/ Defect("recopy_after_absent")
                  /\ priorAttempted
                  /\ observation = "Absent"
               \/ Defect("first_condemned_then_copy")
                  /\ priorAttempted
                  /\ observation = "Absent"
                  /\ writer[w].sawCondemnedAttempt
           useCopy == safeCopy \/ defectiveCopy
           reuseCondemnedEnvelope ==
               /\ Defect("reuse_condemned_envelope")
               /\ observation = "Condemned"
           consumeFreshEnvelope == ~useCopy /\ ~reuseCondemnedEnvelope
           chosenEnvelope ==
               IF useCopy
               THEN StagedEnvelope(w)
               ELSE IF reuseCondemnedEnvelope
                    THEN writer[w].observedEnvelope
                    ELSE nextEnvelope
       IN /\ (~consumeFreshEnvelope \/ nextEnvelope < MaxEnvelope)
          /\ writer' =
              [writer EXCEPT
                  ![w].phase = "Publishing",
                  ![w].publicationAttempted = TRUE,
                  ![w].everPublicationAttempted = TRUE,
                  ![w].attemptCount = @ + 1,
                  ![w].plannedTransport =
                      IF useCopy THEN "VerbatimCopy" ELSE "RetaggedStream",
                  ![w].plannedEnvelope = chosenEnvelope,
                  ![w].plannedObservation = observation,
                  ![w].plannedOldToken = writer[w].observedToken,
                  ![w].plannedOldEnvelope = writer[w].observedEnvelope,
                  ![w].plannedObservedSeq = writer[w].observedWriteSeq,
                  ![w].plannedInvalidVerbatimCopy =
                      useCopy /\ ~(~priorAttempted /\ observation = "Absent"),
                  ![w].attemptLanded = FALSE,
                  ![w].publishedWithoutPrecommit =
                      @ \/ writer[w].precommit /= "Durable",
                  ![w].sawCondemnedAttempt =
                      @ \/ observation = "Condemned"]
          /\ nextEnvelope' =
              IF consumeFreshEnvelope THEN nextEnvelope + 1 ELSE nextEnvelope
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

BackendLand(w) ==
    /\ writer[w].phase = "Publishing"
    /\ ~writer[w].attemptLanded
    /\ CanLand
    /\ LET envelope == writer[w].plannedEnvelope
           payload == SourcePayload(w)
           token == MintToken(envelope, payload)
           afterCondemned == writer[w].plannedObservation = "Condemned"
           fromAbsent == writer[w].plannedObservation = "Absent"
           stagedCopy ==
               /\ writer[w].plannedTransport = "VerbatimCopy"
               /\ fromAbsent
           retaggedAfterCopy ==
               /\ writer[w].plannedTransport = "RetaggedStream"
               /\ afterCondemned
               /\ envelope /= StagedEnvelope(w)
               /\ writer[w].sawStagedCopy
               /\ writer[w].sawResponseLost
               /\ writer[w].sawRecoveryHead
           reproducesQueuedToken ==
               \E record \in queuedDeletes : record.token = token
       IN /\ (~(Defect("first_condemned_then_copy")
                 /\ writer[w].plannedInvalidVerbatimCopy)
               \/ reproducesQueuedToken)
          /\ body' =
              [present |-> TRUE,
               payload |-> payload,
               envelope |-> envelope,
               token |-> token,
               serial |-> writeSeq + 1]
          /\ nextToken' = NextTokenAfterLanding
          /\ writer' =
              [writer EXCEPT
                  ![w].attemptLanded = TRUE,
                  ![w].invalidVerbatimCopy =
                      @ \/ writer[w].plannedInvalidVerbatimCopy,
                  ![w].needsFreshPublication =
                      IF afterCondemned
                      THEN envelope = writer[w].plannedOldEnvelope
                      ELSE @,
                  ![w].sawStagedCopy = @ \/ stagedCopy,
                  ![w].publishedFromAbsent = @ \/ fromAbsent,
                  ![w].publishedAbsentSeq =
                      IF fromAbsent THEN writer[w].plannedObservedSeq ELSE @,
                  ![w].retryLandedBeforeLate =
                      @ \/ writer[w].latePending]
          /\ badFreshAfterCondemned' =
              (badFreshAfterCondemned
               \/ (afterCondemned
                   /\ envelope = writer[w].plannedOldEnvelope))
          /\ racingPublishersReached' =
              (racingPublishersReached
               \/ (fromAbsent
                   /\ OtherAbsentPublisherReached(w, writer[w].plannedObservedSeq)))
          /\ stagedRetagReached' = (stagedRetagReached \/ retaggedAfterCopy)
    /\ writeSeq' = writeSeq + 1
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, nextEnvelope,
          metaState, metaVersion, queuedDeletes, seeded,
          freshIncarnationDeleted, lateLandingReached>>

PublicationSucceeded(w) ==
    /\ writer[w].phase = "Publishing"
    /\ writer[w].attemptLanded
    /\ ~( (Defect("recopy_after_condemned")
           \/ Defect("recopy_after_absent"))
         /\ writer[w].attemptCount = 1
         /\ writer[w].plannedTransport = "VerbatimCopy" )
    /\ writer' =
        [writer EXCEPT
            ![w].phase = "MetaClean",
            ![w].attemptLanded = FALSE]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

PublicationResponseLost(w) ==
    /\ writer[w].phase = "Publishing"
    /\ writer' =
        [writer EXCEPT
            ![w].phase = "ResponseLost",
            ![w].sawResponseLost = TRUE,
            ![w].latePending = ~writer[w].attemptLanded,
            ![w].lateTransport =
                IF writer[w].attemptLanded THEN "None" ELSE writer[w].plannedTransport,
            ![w].lateEnvelope =
                IF writer[w].attemptLanded THEN 0 ELSE writer[w].plannedEnvelope,
            ![w].lateObservation =
                IF writer[w].attemptLanded THEN "None" ELSE writer[w].plannedObservation,
            ![w].lateOldToken =
                IF writer[w].attemptLanded THEN NoToken ELSE writer[w].plannedOldToken,
            ![w].lateOldEnvelope =
                IF writer[w].attemptLanded THEN 0 ELSE writer[w].plannedOldEnvelope,
            ![w].lateObservedSeq =
                IF writer[w].attemptLanded THEN 0 ELSE writer[w].plannedObservedSeq,
            ![w].lateInvalidVerbatimCopy =
                IF writer[w].attemptLanded
                THEN FALSE
                ELSE writer[w].plannedInvalidVerbatimCopy,
            ![w].attemptLanded = FALSE]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

RecoverKeepingLateLanding(w) ==
    /\ writer[w].phase = "ResponseLost"
    /\ writer[w].latePending
    /\ writer' = [writer EXCEPT ![w].phase = "Head"]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

RecoverWithoutLateLanding(w) ==
    /\ writer[w].phase = "ResponseLost"
    /\ writer' =
        [writer EXCEPT
            ![w].phase = "Head",
            ![w].latePending = FALSE,
            ![w].lateTransport = "None",
            ![w].lateEnvelope = 0,
            ![w].lateObservation = "None",
            ![w].lateOldToken = NoToken,
            ![w].lateOldEnvelope = 0,
            ![w].lateObservedSeq = 0,
            ![w].lateInvalidVerbatimCopy = FALSE]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

ResolveLateAsNotLanded(w) ==
    /\ writer[w].latePending
    /\ writer[w].sawRecoveryHead
    /\ writer[w].phase \in {"Head", "MetaRead", "Publish"}
    /\ writer' =
        [writer EXCEPT
            ![w].latePending = FALSE,
            ![w].lateTransport = "None",
            ![w].lateEnvelope = 0,
            ![w].lateObservation = "None",
            ![w].lateOldToken = NoToken,
            ![w].lateOldEnvelope = 0,
            ![w].lateObservedSeq = 0,
            ![w].lateInvalidVerbatimCopy = FALSE]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

(* An old backend request remains independently landable after retry publication and readiness. *)
LateBackendLand(w) ==
    /\ writer[w].latePending
    /\ writer[w].sawRecoveryHead
    /\ CanLand
    /\ LET envelope == writer[w].lateEnvelope
           payload == SourcePayload(w)
           token == MintToken(envelope, payload)
           afterCondemned == writer[w].lateObservation = "Condemned"
           fromAbsent == writer[w].lateObservation = "Absent"
           stagedCopy ==
               /\ writer[w].lateTransport = "VerbatimCopy"
               /\ fromAbsent
           retaggedAfterCopy ==
               /\ writer[w].lateTransport = "RetaggedStream"
               /\ afterCondemned
               /\ envelope /= StagedEnvelope(w)
               /\ writer[w].sawStagedCopy
               /\ writer[w].sawResponseLost
               /\ writer[w].sawRecoveryHead
           reproducesQueuedToken ==
               \E record \in queuedDeletes : record.token = token
       IN /\ (~(Defect("first_condemned_then_copy")
                 /\ writer[w].lateInvalidVerbatimCopy)
               \/ reproducesQueuedToken)
          /\ body' =
              [present |-> TRUE,
               payload |-> payload,
               envelope |-> envelope,
               token |-> token,
               serial |-> writeSeq + 1]
          /\ nextToken' = NextTokenAfterLanding
          /\ writer' =
              [writer EXCEPT
                  ![w].latePending = FALSE,
                  ![w].lateTransport = "None",
                  ![w].lateEnvelope = 0,
                  ![w].lateObservation = "None",
                  ![w].lateOldToken = NoToken,
                  ![w].lateOldEnvelope = 0,
                  ![w].lateObservedSeq = 0,
                  ![w].lateInvalidVerbatimCopy = FALSE,
                  ![w].invalidVerbatimCopy =
                      @ \/ writer[w].lateInvalidVerbatimCopy,
                  ![w].needsFreshPublication =
                      IF afterCondemned
                      THEN envelope = writer[w].lateOldEnvelope
                      ELSE @,
                  ![w].sawStagedCopy = @ \/ stagedCopy,
                  ![w].publishedFromAbsent = @ \/ fromAbsent,
                  ![w].publishedAbsentSeq =
                      IF fromAbsent THEN writer[w].lateObservedSeq ELSE @]
          /\ badFreshAfterCondemned' =
              (badFreshAfterCondemned
               \/ (afterCondemned
                   /\ envelope = writer[w].lateOldEnvelope))
          /\ racingPublishersReached' =
              (racingPublishersReached
               \/ (fromAbsent
                   /\ OtherAbsentPublisherReached(w, writer[w].lateObservedSeq)))
          /\ stagedRetagReached' = (stagedRetagReached \/ retaggedAfterCopy)
    /\ writeSeq' = writeSeq + 1
    /\ lateLandingReached' = TRUE
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, nextEnvelope,
          metaState, metaVersion, queuedDeletes, seeded,
          freshIncarnationDeleted>>

ReadyWithoutReobserve(w) ==
    /\ Defect("ready_without_reobserve")
    /\ writer[w].phase = "ResponseLost"
    /\ writer' =
        [writer EXCEPT
            ![w].phase = "Ready",
            ![w].ready = TRUE,
            ![w].cleanProof = TRUE]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

ReconcileMetaClean(w) ==
    /\ writer[w].phase = "MetaClean"
    /\ body.present
    /\ metaState /= "Condemned"
    /\ (metaState = "Clean" \/ metaVersion < MaxMetaVersion)
    /\ metaState' = "Clean"
    /\ metaVersion' =
        IF metaState = "Clean" THEN metaVersion ELSE metaVersion + 1
    /\ writer' =
        [writer EXCEPT
            ![w].phase = "Ready",
            ![w].ready = TRUE,
            ![w].materialized = TRUE,
            ![w].cleanProof = TRUE,
            ![w].needsFreshPublication = FALSE]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

MetaCleanNeedsRecovery(w) ==
    /\ writer[w].phase = "MetaClean"
    /\ (~body.present \/ metaState = "Condemned")
    /\ writer' =
        [writer EXCEPT
            ![w].phase = "Head",
            ![w].materialized = FALSE,
            ![w].cleanProof = FALSE]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

SkipMetaClean(w) ==
    /\ Defect("skip_meta_clean")
    /\ writer[w].phase = "MetaClean"
    /\ writer' =
        [writer EXCEPT
            ![w].phase = "Ready",
            ![w].ready = TRUE,
            ![w].materialized = TRUE,
            ![w].cleanProof = FALSE]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

GCCondemn ==
    /\ body.present
    /\ metaState /= "Condemned"
    /\ queuedDeletes = {}
    /\ metaVersion < MaxMetaVersion
    /\ ~(\E w \in Writers : writer[w].ready \/ writer[w].committed)
    /\ metaState' = "Condemned"
    /\ metaVersion' = metaVersion + 1
    /\ queuedDeletes' =
        queuedDeletes \cup {[token |-> body.token, serial |-> body.serial]}
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          seeded, writer, badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

DeleteLands(record) ==
    /\ body.present
    /\ (Defect("unconditional_delete") \/ body.token = record.token)

DeleteRemovesFresh(record) ==
    /\ DeleteLands(record)
    /\ (body.token /= record.token \/ body.serial > record.serial)

GCDelete(record) ==
    /\ record \in queuedDeletes
    /\ (~DeleteLands(record) \/ metaVersion < MaxMetaVersion)
    /\ ( ~(Defect("recopy_after_condemned")
           \/ Defect("recopy_after_absent"))
         \/ ~DeleteRemovesFresh(record)
         \/ (\E w \in Writers : writer[w].ready) )
    /\ IF DeleteLands(record)
       THEN
           /\ body' = EmptyBody
           /\ metaState' = "Absent"
           /\ metaVersion' = metaVersion + 1
       ELSE
           /\ body' = body
           /\ metaState' = metaState
           /\ metaVersion' = metaVersion
    /\ queuedDeletes' = queuedDeletes
    /\ freshIncarnationDeleted' =
        (freshIncarnationDeleted \/ DeleteRemovesFresh(record))
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, nextToken, nextEnvelope, writeSeq,
          seeded, writer, badFreshAfterCondemned,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

LoseFence(w) ==
    /\ ~writer[w].committed
    /\ ~writer[w].fenced
    /\ writer[w].phase \in {"Publish", "Ready"}
    /\ fenceGeneration < MaxFence
    /\ fenceGeneration' = fenceGeneration + 1
    /\ writer' = [writer EXCEPT ![w].fenced = TRUE]
    /\ UNCHANGED
        <<tokenFamily, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

Commit(w) ==
    /\ writer[w].phase = "Ready"
    /\ writer[w].ready
    /\ ~writer[w].committed
    /\ \/ /\ ~writer[w].fenced
          /\ writer[w].decisionFence = fenceGeneration
       \/ /\ Defect("commit_after_fence")
          /\ writer[w].fenced
    /\ writer' =
        [writer EXCEPT
            ![w].phase = "Committed",
            ![w].committed = TRUE]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

Abort(w) ==
    /\ writer[w].phase \notin {"Ready", "Committed", "Aborted"}
    /\ writer[w].fenced
    /\ writer' = [writer EXCEPT ![w].phase = "Aborted"]
    /\ UNCHANGED
        <<tokenFamily, fenceGeneration, body, nextToken, nextEnvelope, writeSeq,
          metaState, metaVersion, queuedDeletes, seeded,
          badFreshAfterCondemned, freshIncarnationDeleted,
          racingPublishersReached, stagedRetagReached, lateLandingReached>>

Next ==
    \/ SeedBody
    \/ \E w \in Writers :
        \/ DurablePrecommit(w)
        \/ SkipPrecommit(w)
        \/ ObserveBody(w)
        \/ ObserveMeta(w)
        \/ ObservationInvalidated(w)
        \/ AdoptCondemned(w)
        \/ BeginPublication(w)
        \/ BackendLand(w)
        \/ PublicationSucceeded(w)
        \/ PublicationResponseLost(w)
        \/ RecoverKeepingLateLanding(w)
        \/ RecoverWithoutLateLanding(w)
        \/ ResolveLateAsNotLanded(w)
        \/ LateBackendLand(w)
        \/ ReadyWithoutReobserve(w)
        \/ ReconcileMetaClean(w)
        \/ MetaCleanNeedsRecovery(w)
        \/ SkipMetaClean(w)
        \/ LoseFence(w)
        \/ Commit(w)
        \/ Abort(w)
    \/ GCCondemn
    \/ \E record \in queuedDeletes : GCDelete(record)

(* Named witness actions set persistent reachability bits checked by the witness configs. *)
RacingPublishersWitnessAction ==
    \E w \in Writers :
        /\ BackendLand(w)
        /\ ~racingPublishersReached
        /\ racingPublishersReached'

StagedRetagWitnessAction ==
    \E w \in Writers :
        /\ BackendLand(w)
        /\ ~stagedRetagReached
        /\ stagedRetagReached'

LateLandingWitnessAction ==
    \E w \in Writers :
        /\ LateBackendLand(w)
        /\ writer[w].phase = "Ready"
        /\ writer[w].retryLandedBeforeLate

Spec == Init /\ [][Next]_vars

CommittedRefHasContent ==
    \A w \in Writers :
        writer[w].committed => (body.present /\ body.payload = KeyPayload)

ReadyRequiresObservedMaterialization ==
    \A w \in Writers :
        (writer[w].ready \/ writer[w].committed) => writer[w].materialized

CondemnedNeedsFreshPublication ==
    \A w \in Writers :
        /\ ~writer[w].adoptedCondemned
        /\ ((writer[w].ready \/ writer[w].committed)
            => ~writer[w].needsFreshPublication)

FreshAfterCondemned == ~badFreshAfterCondemned

PublicationAttemptIsMonotonic ==
    \A w \in Writers :
        writer[w].publicationAttempted = writer[w].everPublicationAttempted

VerbatimCopyOnlyFirstAbsent ==
    \A w \in Writers : ~writer[w].invalidVerbatimCopy

ExactDeleteCannotRemoveFreshIncarnation == ~freshIncarnationDeleted

PublicationRequiresDurablePrecommit ==
    \A w \in Writers :
        /\ ~writer[w].publishedWithoutPrecommit
        /\ (writer[w].publicationAttempted => writer[w].precommit = "Durable")

ReadyRequiresCleanMeta ==
    \A w \in Writers :
        (writer[w].ready \/ writer[w].committed) => writer[w].cleanProof

FencedWriterCannotCommit ==
    \A w \in Writers : writer[w].fenced => ~writer[w].committed

KeyNamesPayload == ~body.present \/ body.payload = KeyPayload

(* Negated reachability checks: TLC must violate each named witness. *)
WitnessRacingPublishers == ~racingPublishersReached
WitnessStagedRetag == ~stagedRetagReached
WitnessLateLanding == [][~LateLandingWitnessAction]_vars

====================================================================================
