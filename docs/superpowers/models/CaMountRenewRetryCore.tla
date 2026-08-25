-------------------------- MODULE CaMountRenewRetryCore --------------------------
(*
Focused safety model for split-phase conditional mount renewal.

One old-epoch runtime starts from a confirmed predecessor. A logical renewal captures
its attempt ID, body, expected token, body deadline, absolute operation deadline, and
cadence anchor before I/O. Physical retries are collapsed to `outstanding`: every copy
carries that one tuple. Backend landing, response delivery/loss, exact reads, local
terminalization, and post-terminal delivery remain separate actions.

The durable observers deliberately run while local confirmation is pending. They can
install a same-pair twin, GC-fence or reclaim a body, install a successor epoch, or
install a foreign holder. Honest conditional landing consumes the predecessor token;
the multiplicity sabotage is the sole action that lets two copies replace it.
*)
EXTENDS Naturals, FiniteSets

CONSTANTS
    AttemptIds,
    CurrentAttempt,
    OtherAttempt,
    Holders,
    SelfHolder,
    ForeignHolder,
    PredecessorToken,
    RenewalToken,
    TwinToken,
    SuccessorToken,
    ForeignToken,
    InitialConfirmedDeadline,
    MaxTime,
    LeaseDuration,
    RequestTimeout,
    CadencePeriod,
    MaxRetries,
    MaxOutstanding,
    Defects

AllowedDefects ==
    {"ignore_attempt_id",
     "refresh_deadline_from_response",
     "retry_with_new_body",
     "accept_after_terminal",
     "accept_successor",
     "drop_pending_on_terminal",
     "late_rearm",
     "response_relative_cadence",
     "send_after_deadline",
     "double_conditional_landing"}

Defect(name) == name \in Defects

NoAttempt == "NoAttempt"
NoHolder == "NoHolder"
NoToken == "NoToken"

RenewalBody == "RenewalBody"
RetryBody == "RetryBody"
PredecessorBody == "PredecessorBody"
TwinBody == RenewalBody
SuccessorBody == "SuccessorBody"
ForeignBody == "ForeignBody"
NoBody == "NoBody"

BodyNames ==
    {RenewalBody, RetryBody, PredecessorBody, SuccessorBody, ForeignBody, NoBody}
DurableKinds == {"Predecessor", "Renewal", "Twin", "Successor", "Foreign", "Absent"}
LocalStates ==
    {"Idle", "Sent", "Landed", "ResponseLost", "Resolved", "RetryWait",
     "Committed", "Conflict", "Unresolved"}
ResolveOutcomes == {"None", "Exact", "Predecessor", "Conflict"}
AdoptedKinds == {"None", "Renewal", "Twin", "Successor", "Foreign", "GCFenced"}

TokenSet ==
    {PredecessorToken, RenewalToken, TwinToken, SuccessorToken, ForeignToken, NoToken}
AttemptSet == AttemptIds \cup {NoAttempt}
HolderSet == Holders \cup {NoHolder}
DeadlineRange == 0..(MaxTime + LeaseDuration)

DurableState ==
    [kind : DurableKinds,
     token : TokenSet,
     holder : HolderSet,
     epoch : 0..1,
     attempt : AttemptSet,
     body : BodyNames,
     deadline : DeadlineRange]

HistoryState ==
    [sentOutsideBudget : BOOLEAN,
     tupleChanged : BOOLEAN,
     deadlineFromResponse : BOOLEAN,
     rearmedAfterTerminal : BOOLEAN,
     badAcknowledgement : BOOLEAN,
     lateOverwroteSuccessor : BOOLEAN,
     terminalDroppedPending : BOOLEAN,
     sawNonLandedTransient : BOOLEAN,
     sawLandedResponseLoss : BOOLEAN,
     sawRetrySent : BOOLEAN,
     directRetryReached : BOOLEAN,
     readAdoptionReached : BOOLEAN,
     exhaustionFencesReached : BOOLEAN,
     lateOldLanded : BOOLEAN,
     lateBeforeReclaimReached : BOOLEAN,
     oldCopyRefused : BOOLEAN,
     lateAfterSuccessorReached : BOOLEAN,
     slowSuccess : BOOLEAN,
     catchupReached : BOOLEAN]

InitialDurable ==
    [kind |-> "Predecessor",
     token |-> PredecessorToken,
     holder |-> SelfHolder,
     epoch |-> 0,
     attempt |-> NoAttempt,
     body |-> PredecessorBody,
     deadline |-> InitialConfirmedDeadline]

EmptyDurable ==
    [kind |-> "Absent",
     token |-> NoToken,
     holder |-> NoHolder,
     epoch |-> 0,
     attempt |-> NoAttempt,
     body |-> NoBody,
     deadline |-> 0]

InitialHistory ==
    [sentOutsideBudget |-> FALSE,
     tupleChanged |-> FALSE,
     deadlineFromResponse |-> FALSE,
     rearmedAfterTerminal |-> FALSE,
     badAcknowledgement |-> FALSE,
     lateOverwroteSuccessor |-> FALSE,
     terminalDroppedPending |-> FALSE,
     sawNonLandedTransient |-> FALSE,
     sawLandedResponseLoss |-> FALSE,
     sawRetrySent |-> FALSE,
     directRetryReached |-> FALSE,
     readAdoptionReached |-> FALSE,
     exhaustionFencesReached |-> FALSE,
     lateOldLanded |-> FALSE,
     lateBeforeReclaimReached |-> FALSE,
     oldCopyRefused |-> FALSE,
     lateAfterSuccessorReached |-> FALSE,
     slowSuccess |-> FALSE,
     catchupReached |-> FALSE]

VARIABLES
    clock,
    durable,
    gcFenced,
    confirmedBody,
    confirmedToken,
    confirmedDeadline,
    localAuthority,
    localState,
    resolveOutcome,
    logicalCreated,
    requestAttempt,
    requestBody,
    requestExpectedToken,
    requestDeadline,
    attemptStartedAt,
    cadenceAnchor,
    retryCount,
    outstanding,
    predecessorLandings,
    adoptedAttempt,
    adoptedKind,
    terminalSeen,
    cancelled,
    successObservedAt,
    nextBeatScheduled,
    nextBeat,
    reclaimSeen,
    history

vars ==
    <<clock, durable, gcFenced,
      confirmedBody, confirmedToken, confirmedDeadline,
      localAuthority, localState, resolveOutcome,
      logicalCreated, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
      attemptStartedAt, cadenceAnchor, retryCount, outstanding, predecessorLandings,
      adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
      nextBeatScheduled, nextBeat, reclaimSeen, history>>

Init ==
    /\ clock = 0
    /\ durable = InitialDurable
    /\ gcFenced = FALSE
    /\ confirmedBody = PredecessorBody
    /\ confirmedToken = PredecessorToken
    /\ confirmedDeadline = InitialConfirmedDeadline
    /\ localAuthority = TRUE
    /\ localState = "Idle"
    /\ resolveOutcome = "None"
    /\ logicalCreated = FALSE
    /\ requestAttempt = CurrentAttempt
    /\ requestBody = RenewalBody
    /\ requestExpectedToken = PredecessorToken
    /\ requestDeadline = 0
    /\ attemptStartedAt = 0
    /\ cadenceAnchor = 0
    /\ retryCount = 0
    /\ outstanding = 0
    /\ predecessorLandings = 0
    /\ adoptedAttempt = NoAttempt
    /\ adoptedKind = "None"
    /\ terminalSeen = FALSE
    /\ cancelled = FALSE
    /\ successObservedAt = 0
    /\ nextBeatScheduled = FALSE
    /\ nextBeat = 0
    /\ reclaimSeen = FALSE
    /\ history = InitialHistory

SafeOperationDeadline == InitialConfirmedDeadline
CanStartRequest == clock + RequestTimeout <= SafeOperationDeadline

RequestDurable ==
    [kind |-> "Renewal",
     token |-> RenewalToken,
     holder |-> SelfHolder,
     epoch |-> 0,
     attempt |-> requestAttempt,
     body |-> requestBody,
     deadline |-> requestDeadline]

TwinDurable ==
    [kind |-> "Twin",
     token |-> TwinToken,
     holder |-> SelfHolder,
     epoch |-> 0,
     attempt |-> OtherAttempt,
     body |-> TwinBody,
     deadline |-> requestDeadline]

SuccessorDurable ==
    [kind |-> "Successor",
     token |-> SuccessorToken,
     holder |-> SelfHolder,
     epoch |-> 1,
     attempt |-> OtherAttempt,
     body |-> SuccessorBody,
     deadline |-> MaxTime + LeaseDuration]

ForeignDurable ==
    [kind |-> "Foreign",
     token |-> ForeignToken,
     holder |-> ForeignHolder,
     epoch |-> 0,
     attempt |-> OtherAttempt,
     body |-> ForeignBody,
     deadline |-> MaxTime + LeaseDuration]

ExactRequestDurable ==
    /\ ~gcFenced
    /\ durable.kind = "Renewal"
    /\ durable.token = RenewalToken
    /\ durable.holder = SelfHolder
    /\ durable.epoch = 0
    /\ durable.attempt = CurrentAttempt
    /\ durable.body = RenewalBody
    /\ durable.deadline = requestDeadline

TwinMatchesIgnoringId ==
    /\ ~gcFenced
    /\ durable.kind = "Twin"
    /\ durable.holder = SelfHolder
    /\ durable.epoch = 0
    /\ durable.attempt = OtherAttempt
    /\ durable.body = RenewalBody

PredecessorStillCurrent ==
    /\ ~gcFenced
    /\ durable.kind = "Predecessor"
    /\ durable.token = requestExpectedToken

TypeOK ==
    /\ AttemptIds = {CurrentAttempt, OtherAttempt}
    /\ CurrentAttempt /= OtherAttempt
    /\ Holders = {SelfHolder, ForeignHolder}
    /\ SelfHolder /= ForeignHolder
    /\ Cardinality({PredecessorToken, RenewalToken, TwinToken, SuccessorToken, ForeignToken}) = 5
    /\ NoToken \notin {PredecessorToken, RenewalToken, TwinToken, SuccessorToken, ForeignToken}
    /\ InitialConfirmedDeadline = 3
    /\ MaxTime = 4
    /\ LeaseDuration = 4
    /\ RequestTimeout = 1
    /\ CadencePeriod = 2
    /\ MaxRetries = 1
    /\ MaxOutstanding = 2
    /\ Defects \subseteq AllowedDefects
    /\ Cardinality(Defects) <= 1
    /\ clock \in 0..MaxTime
    /\ durable \in DurableState
    /\ gcFenced \in BOOLEAN
    /\ confirmedBody \in BodyNames
    /\ confirmedToken \in TokenSet
    /\ confirmedDeadline \in DeadlineRange
    /\ localAuthority \in BOOLEAN
    /\ localState \in LocalStates
    /\ resolveOutcome \in ResolveOutcomes
    /\ logicalCreated \in BOOLEAN
    /\ requestAttempt \in AttemptIds
    /\ requestBody \in {RenewalBody, RetryBody}
    /\ requestExpectedToken \in TokenSet
    /\ requestDeadline \in DeadlineRange
    /\ attemptStartedAt \in 0..MaxTime
    /\ cadenceAnchor \in 0..MaxTime
    /\ retryCount \in 0..MaxRetries
    /\ outstanding \in 0..MaxOutstanding
    /\ predecessorLandings \in 0..MaxOutstanding
    /\ adoptedAttempt \in AttemptSet
    /\ adoptedKind \in AdoptedKinds
    /\ terminalSeen \in BOOLEAN
    /\ cancelled \in BOOLEAN
    /\ successObservedAt \in 0..MaxTime
    /\ nextBeatScheduled \in BOOLEAN
    /\ nextBeat \in 0..(MaxTime + CadencePeriod)
    /\ reclaimSeen \in BOOLEAN
    /\ history \in HistoryState
    /\ (logicalCreated => requestDeadline = attemptStartedAt + LeaseDuration)
    /\ (nextBeatScheduled => localState = "Committed")

AdvanceTime ==
    /\ clock < MaxTime
    /\ clock' = clock + 1
    /\ UNCHANGED
        <<durable, gcFenced,
          confirmedBody, confirmedToken, confirmedDeadline,
          localAuthority, localState, resolveOutcome,
          logicalCreated, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, retryCount, outstanding, predecessorLandings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

SendRenewal ==
    /\ ~logicalCreated
    /\ localAuthority
    /\ localState = "Idle"
    /\ outstanding = 0
    /\ (CanStartRequest \/ Defect("send_after_deadline"))
    /\ logicalCreated' = TRUE
    /\ requestDeadline' = clock + LeaseDuration
    /\ attemptStartedAt' = clock
    /\ cadenceAnchor' = clock
    /\ localState' = "Sent"
    /\ resolveOutcome' = "None"
    /\ retryCount' = 0
    /\ outstanding' = 1
    /\ history' =
        [history EXCEPT
            !.sentOutsideBudget = @ \/ ~CanStartRequest]
    /\ UNCHANGED
        <<clock, durable, gcFenced,
          confirmedBody, confirmedToken, confirmedDeadline, localAuthority,
          requestAttempt, requestBody, requestExpectedToken,
          predecessorLandings, adoptedAttempt, adoptedKind,
          terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen>>

ResponseLoss ==
    /\ logicalCreated
    /\ ~terminalSeen
    /\ localState \in {"Sent", "Landed"}
    /\ localState' = "ResponseLost"
    /\ resolveOutcome' = "None"
    /\ history' =
        [history EXCEPT
            !.sawNonLandedTransient = @ \/ (localState = "Sent"),
            !.sawLandedResponseLoss = @ \/ (localState = "Landed")]
    /\ UNCHANGED
        <<clock, durable, gcFenced,
          confirmedBody, confirmedToken, confirmedDeadline, localAuthority,
          logicalCreated, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, retryCount, outstanding, predecessorLandings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen>>

LandCopy ==
    /\ logicalCreated
    /\ ~terminalSeen
    /\ localState \notin {"Committed", "Unresolved"}
    /\ outstanding > 0
    /\ PredecessorStillCurrent
    /\ predecessorLandings < MaxOutstanding
    /\ durable' = RequestDurable
    /\ gcFenced' = FALSE
    /\ outstanding' = outstanding - 1
    /\ predecessorLandings' = predecessorLandings + 1
    /\ localState' = IF localState = "ResponseLost" THEN localState ELSE "Landed"
    /\ UNCHANGED
        <<clock, confirmedBody, confirmedToken, confirmedDeadline, localAuthority,
          resolveOutcome, logicalCreated, requestAttempt, requestBody,
          requestExpectedToken, requestDeadline, attemptStartedAt, cadenceAnchor,
          retryCount, adoptedAttempt, adoptedKind, terminalSeen, cancelled,
          successObservedAt, nextBeatScheduled, nextBeat, reclaimSeen, history>>

RefuseCopy ==
    /\ logicalCreated
    /\ ~terminalSeen
    /\ localState \notin {"Committed", "Unresolved"}
    /\ outstanding > 0
    /\ ~PredecessorStillCurrent
    /\ outstanding' = outstanding - 1
    /\ localState' = IF localState = "ResponseLost" THEN localState ELSE "Conflict"
    /\ resolveOutcome' = IF localState = "ResponseLost" THEN resolveOutcome ELSE "Conflict"
    /\ UNCHANGED
        <<clock, durable, gcFenced,
          confirmedBody, confirmedToken, confirmedDeadline, localAuthority,
          logicalCreated, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, retryCount, predecessorLandings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

DoubleConditionalLanding ==
    /\ Defect("double_conditional_landing")
    /\ logicalCreated
    /\ ~terminalSeen
    /\ outstanding >= 2
    /\ predecessorLandings = 0
    /\ PredecessorStillCurrent
    /\ durable' = RequestDurable
    /\ gcFenced' = FALSE
    /\ outstanding' = outstanding - 2
    /\ predecessorLandings' = predecessorLandings + 2
    /\ localState' = IF localState = "ResponseLost" THEN localState ELSE "Landed"
    /\ UNCHANGED
        <<clock, confirmedBody, confirmedToken, confirmedDeadline, localAuthority,
          resolveOutcome, logicalCreated, requestAttempt, requestBody,
          requestExpectedToken, requestDeadline, attemptStartedAt, cadenceAnchor,
          retryCount, adoptedAttempt, adoptedKind, terminalSeen, cancelled,
          successObservedAt, nextBeatScheduled, nextBeat, reclaimSeen, history>>

AcknowledgeRenewal ==
    /\ logicalCreated
    /\ ~terminalSeen
    /\ localState = "Landed"
    /\ ExactRequestDurable
    /\ clock <= SafeOperationDeadline
    /\ ( ~Defect("refresh_deadline_from_response")
         \/ clock > attemptStartedAt )
    /\ localState' = "Committed"
    /\ confirmedBody' = durable.body
    /\ confirmedToken' = durable.token
    /\ confirmedDeadline' =
        IF Defect("refresh_deadline_from_response")
        THEN clock + LeaseDuration
        ELSE durable.deadline
    /\ adoptedAttempt' = durable.attempt
    /\ adoptedKind' = "Renewal"
    /\ successObservedAt' = clock
    /\ history' =
        [history EXCEPT
            !.deadlineFromResponse = @ \/ Defect("refresh_deadline_from_response"),
            !.badAcknowledgement = @ \/ ~ExactRequestDurable,
            !.directRetryReached =
                @ \/ (history.sawNonLandedTransient /\ history.sawRetrySent),
            !.slowSuccess = @ \/ (clock >= cadenceAnchor + CadencePeriod)]
    /\ UNCHANGED
        <<clock, durable, gcFenced, localAuthority, resolveOutcome,
          logicalCreated, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, retryCount, outstanding, predecessorLandings,
          terminalSeen, cancelled, nextBeatScheduled, nextBeat, reclaimSeen>>

ExactResolve ==
    /\ logicalCreated
    /\ ~terminalSeen
    /\ localState = "ResponseLost"
    /\ IF ExactRequestDurable
       THEN
           /\ localState' = "Resolved"
           /\ resolveOutcome' = "Exact"
       ELSE IF PredecessorStillCurrent
       THEN
           /\ localState' = "Resolved"
           /\ resolveOutcome' = "Predecessor"
       ELSE IF Defect("ignore_attempt_id") /\ TwinMatchesIgnoringId
       THEN
           /\ localState' = "Resolved"
           /\ resolveOutcome' = "Exact"
       ELSE
           /\ localState' = "Conflict"
           /\ resolveOutcome' = "Conflict"
    /\ UNCHANGED
        <<clock, durable, gcFenced,
          confirmedBody, confirmedToken, confirmedDeadline, localAuthority,
          logicalCreated, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, retryCount, outstanding, predecessorLandings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

AcceptResolved ==
    /\ logicalCreated
    /\ ~terminalSeen
    /\ localState = "Resolved"
    /\ resolveOutcome = "Exact"
    /\ (ExactRequestDurable
        \/ (Defect("ignore_attempt_id") /\ TwinMatchesIgnoringId))
    /\ clock <= SafeOperationDeadline
    /\ localState' = "Committed"
    /\ confirmedBody' = durable.body
    /\ confirmedToken' = durable.token
    /\ confirmedDeadline' = durable.deadline
    /\ adoptedAttempt' = durable.attempt
    /\ adoptedKind' = durable.kind
    /\ successObservedAt' = clock
    /\ history' =
        [history EXCEPT
            !.readAdoptionReached =
                @ \/ (history.sawLandedResponseLoss /\ ExactRequestDurable),
            !.slowSuccess = @ \/ (clock >= cadenceAnchor + CadencePeriod)]
    /\ UNCHANGED
        <<clock, durable, gcFenced, localAuthority, resolveOutcome,
          logicalCreated, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, retryCount, outstanding, predecessorLandings,
          terminalSeen, cancelled, nextBeatScheduled, nextBeat, reclaimSeen>>

EnterRetryWait ==
    /\ logicalCreated
    /\ ~terminalSeen
    /\ ~cancelled
    /\ localState = "Resolved"
    /\ resolveOutcome = "Predecessor"
    /\ localState' = "RetryWait"
    /\ UNCHANGED
        <<clock, durable, gcFenced,
          confirmedBody, confirmedToken, confirmedDeadline, localAuthority,
          resolveOutcome, logicalCreated, requestAttempt, requestBody,
          requestExpectedToken, requestDeadline, attemptStartedAt, cadenceAnchor,
          retryCount, outstanding, predecessorLandings, adoptedAttempt, adoptedKind,
          terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

RetrySend ==
    /\ logicalCreated
    /\ ~terminalSeen
    /\ ~cancelled
    /\ localState = "RetryWait"
    /\ retryCount < MaxRetries
    /\ outstanding < MaxOutstanding
    /\ (CanStartRequest \/ Defect("send_after_deadline"))
    /\ localState' = "Sent"
    /\ resolveOutcome' = "None"
    /\ retryCount' = retryCount + 1
    /\ outstanding' = outstanding + 1
    /\ requestBody' =
        IF Defect("retry_with_new_body") THEN RetryBody ELSE requestBody
    /\ history' =
        [history EXCEPT
            !.sentOutsideBudget = @ \/ ~CanStartRequest,
            !.tupleChanged = @ \/ Defect("retry_with_new_body"),
            !.sawRetrySent = TRUE]
    /\ UNCHANGED
        <<clock, durable, gcFenced,
          confirmedBody, confirmedToken, confirmedDeadline, localAuthority,
          logicalCreated, requestAttempt, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, predecessorLandings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen>>

Cancel ==
    /\ logicalCreated
    /\ ~terminalSeen
    /\ localState /= "Committed"
    /\ ~cancelled
    /\ cancelled' = TRUE
    /\ UNCHANGED
        <<clock, durable, gcFenced,
          confirmedBody, confirmedToken, confirmedDeadline, localAuthority,
          localState, resolveOutcome, logicalCreated, requestAttempt, requestBody,
          requestExpectedToken, requestDeadline, attemptStartedAt, cadenceAnchor,
          retryCount, outstanding, predecessorLandings, adoptedAttempt, adoptedKind,
          terminalSeen, successObservedAt, nextBeatScheduled, nextBeat,
          reclaimSeen, history>>

Exhausted ==
    /\ ~cancelled
    /\ localState \in {"ResponseLost", "RetryWait"}
    /\ (retryCount = MaxRetries \/ ~CanStartRequest)

LocalTerminalize ==
    /\ logicalCreated
    /\ ~terminalSeen
    /\ localState /= "Committed"
    /\ (cancelled \/ localState = "Conflict" \/ Exhausted)
    /\ terminalSeen' = TRUE
    /\ localAuthority' = FALSE
    /\ localState' = "Unresolved"
    /\ resolveOutcome' = "Conflict"
    /\ outstanding' =
        IF Defect("drop_pending_on_terminal") /\ outstanding > 0
        THEN 0
        ELSE outstanding
    /\ history' =
        [history EXCEPT
            !.terminalDroppedPending =
                @ \/ (Defect("drop_pending_on_terminal") /\ outstanding > 0),
            !.exhaustionFencesReached = @ \/ Exhausted]
    /\ UNCHANGED
        <<clock, durable, gcFenced,
          confirmedBody, confirmedToken, confirmedDeadline,
          logicalCreated, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, retryCount, predecessorLandings,
          adoptedAttempt, adoptedKind, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen>>

SamePairTwin ==
    /\ logicalCreated
    /\ durable.kind = "Predecessor"
    /\ ~gcFenced
    /\ durable' = TwinDurable
    /\ localAuthority' = FALSE
    /\ UNCHANGED
        <<clock, gcFenced, confirmedBody, confirmedToken, confirmedDeadline,
          localState, resolveOutcome, logicalCreated, requestAttempt, requestBody,
          requestExpectedToken, requestDeadline, attemptStartedAt, cadenceAnchor,
          retryCount, outstanding, predecessorLandings, adoptedAttempt, adoptedKind,
          terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

GCFence ==
    /\ durable.kind /= "Absent"
    /\ ~gcFenced
    /\ gcFenced' = TRUE
    /\ localAuthority' = FALSE
    /\ UNCHANGED
        <<clock, durable, confirmedBody, confirmedToken, confirmedDeadline,
          localState, resolveOutcome, logicalCreated, requestAttempt, requestBody,
          requestExpectedToken, requestDeadline, attemptStartedAt, cadenceAnchor,
          retryCount, outstanding, predecessorLandings, adoptedAttempt, adoptedKind,
          terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

SuccessorClaim ==
    /\ logicalCreated
    /\ durable.kind /= "Successor"
    /\ durable' = SuccessorDurable
    /\ gcFenced' = FALSE
    /\ localAuthority' = FALSE
    /\ UNCHANGED
        <<clock, confirmedBody, confirmedToken, confirmedDeadline,
          localState, resolveOutcome, logicalCreated, requestAttempt, requestBody,
          requestExpectedToken, requestDeadline, attemptStartedAt, cadenceAnchor,
          retryCount, outstanding, predecessorLandings, adoptedAttempt, adoptedKind,
          terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

ForeignHolderWrite ==
    /\ logicalCreated
    /\ durable.kind /= "Foreign"
    /\ durable' = ForeignDurable
    /\ gcFenced' = FALSE
    /\ localAuthority' = FALSE
    /\ UNCHANGED
        <<clock, confirmedBody, confirmedToken, confirmedDeadline,
          localState, resolveOutcome, logicalCreated, requestAttempt, requestBody,
          requestExpectedToken, requestDeadline, attemptStartedAt, cadenceAnchor,
          retryCount, outstanding, predecessorLandings, adoptedAttempt, adoptedKind,
          terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

Reclaim ==
    /\ gcFenced
    /\ durable.kind /= "Absent"
    /\ durable' = EmptyDurable
    /\ gcFenced' = FALSE
    /\ reclaimSeen' = TRUE
    /\ history' =
        [history EXCEPT
            !.lateBeforeReclaimReached =
                @ \/ (history.lateOldLanded /\ terminalSeen /\ ~localAuthority)]
    /\ UNCHANGED
        <<clock, confirmedBody, confirmedToken, confirmedDeadline, localAuthority,
          localState, resolveOutcome, logicalCreated, requestAttempt, requestBody,
          requestExpectedToken, requestDeadline, attemptStartedAt, cadenceAnchor,
          retryCount, outstanding, predecessorLandings, adoptedAttempt, adoptedKind,
          terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat>>

LateDeliveryLands ==
    /\ terminalSeen
    /\ localState = "Unresolved"
    /\ outstanding > 0
    /\ PredecessorStillCurrent
    /\ predecessorLandings < MaxOutstanding
    /\ durable' = RequestDurable
    /\ gcFenced' = FALSE
    /\ outstanding' = outstanding - 1
    /\ predecessorLandings' = predecessorLandings + 1
    /\ localAuthority' = IF Defect("late_rearm") THEN TRUE ELSE FALSE
    /\ history' =
        [history EXCEPT
            !.rearmedAfterTerminal = @ \/ Defect("late_rearm"),
            !.lateOverwroteSuccessor = @ \/ (durable.kind = "Successor"),
            !.lateOldLanded = TRUE]
    /\ UNCHANGED
        <<clock, confirmedBody, confirmedToken, confirmedDeadline,
          localState, resolveOutcome, logicalCreated, requestAttempt, requestBody,
          requestExpectedToken, requestDeadline, attemptStartedAt, cadenceAnchor,
          retryCount, adoptedAttempt, adoptedKind, terminalSeen, cancelled,
          successObservedAt, nextBeatScheduled, nextBeat, reclaimSeen>>

LateDeliveryRefused ==
    /\ terminalSeen
    /\ localState = "Unresolved"
    /\ outstanding > 0
    /\ ~PredecessorStillCurrent
    /\ outstanding' = outstanding - 1
    /\ localAuthority' = FALSE
    /\ history' =
        [history EXCEPT
            !.oldCopyRefused = @ \/ (durable.kind = "Successor"),
            !.lateAfterSuccessorReached =
                @ \/ (durable.kind = "Successor" /\ ~gcFenced)]
    /\ UNCHANGED
        <<clock, durable, gcFenced,
          confirmedBody, confirmedToken, confirmedDeadline,
          localState, resolveOutcome, logicalCreated, requestAttempt, requestBody,
          requestExpectedToken, requestDeadline, attemptStartedAt, cadenceAnchor,
          retryCount, predecessorLandings, adoptedAttempt, adoptedKind,
          terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen>>

AcceptAfterTerminal ==
    /\ Defect("accept_after_terminal")
    /\ terminalSeen
    /\ localState = "Unresolved"
    /\ ExactRequestDurable
    /\ localState' = "Committed"
    /\ localAuthority' = TRUE
    /\ confirmedBody' = durable.body
    /\ confirmedToken' = durable.token
    /\ confirmedDeadline' = durable.deadline
    /\ adoptedAttempt' = durable.attempt
    /\ adoptedKind' = "Renewal"
    /\ successObservedAt' = clock
    /\ history' =
        [history EXCEPT
            !.rearmedAfterTerminal = TRUE]
    /\ UNCHANGED
        <<clock, durable, gcFenced, resolveOutcome,
          logicalCreated, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, retryCount, outstanding, predecessorLandings,
          terminalSeen, cancelled, nextBeatScheduled, nextBeat, reclaimSeen>>

AcceptSuccessor ==
    /\ Defect("accept_successor")
    /\ ~terminalSeen
    /\ localState = "Conflict"
    /\ durable.kind = "Successor"
    /\ ~gcFenced
    /\ localState' = "Committed"
    /\ localAuthority' = TRUE
    /\ confirmedBody' = durable.body
    /\ confirmedToken' = durable.token
    /\ confirmedDeadline' = durable.deadline
    /\ adoptedAttempt' = durable.attempt
    /\ adoptedKind' = "Successor"
    /\ successObservedAt' = clock
    /\ history' =
        [history EXCEPT
            !.badAcknowledgement = TRUE]
    /\ UNCHANGED
        <<clock, durable, gcFenced, resolveOutcome,
          logicalCreated, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, retryCount, outstanding, predecessorLandings,
          terminalSeen, cancelled, nextBeatScheduled, nextBeat, reclaimSeen>>

ScheduleNextBeat ==
    /\ localState = "Committed"
    /\ ~nextBeatScheduled
    /\ nextBeatScheduled' = TRUE
    /\ nextBeat' =
        IF Defect("response_relative_cadence")
        THEN successObservedAt + CadencePeriod
        ELSE cadenceAnchor + CadencePeriod
    /\ history' =
        [history EXCEPT
            !.catchupReached =
                @ \/ (history.slowSuccess
                      /\ cadenceAnchor + CadencePeriod <= clock)]
    /\ UNCHANGED
        <<clock, durable, gcFenced,
          confirmedBody, confirmedToken, confirmedDeadline, localAuthority,
          localState, resolveOutcome, logicalCreated, requestAttempt, requestBody,
          requestExpectedToken, requestDeadline, attemptStartedAt, cadenceAnchor,
          retryCount, outstanding, predecessorLandings, adoptedAttempt, adoptedKind,
          terminalSeen, cancelled, successObservedAt, reclaimSeen>>

Next ==
    \/ AdvanceTime
    \/ SendRenewal
    \/ ResponseLoss
    \/ LandCopy
    \/ RefuseCopy
    \/ DoubleConditionalLanding
    \/ AcknowledgeRenewal
    \/ ExactResolve
    \/ AcceptResolved
    \/ EnterRetryWait
    \/ RetrySend
    \/ Cancel
    \/ LocalTerminalize
    \/ SamePairTwin
    \/ GCFence
    \/ SuccessorClaim
    \/ ForeignHolderWrite
    \/ Reclaim
    \/ LateDeliveryLands
    \/ LateDeliveryRefused
    \/ AcceptAfterTerminal
    \/ AcceptSuccessor
    \/ ScheduleNextBeat

Spec == Init /\ [][Next]_vars

(* Only the exact symbolic `write_attempt_id` may be adopted. *)
ExactAttemptOnly ==
    adoptedAttempt \in {NoAttempt, CurrentAttempt}

(* Twins, GC-fenced bodies, successor epochs, and foreign holders remain fail closed. *)
ForeignOrSuccessorNeverAdopted ==
    adoptedKind \in {"None", "Renewal"}

(* A confirmed renewal uses the pre-I/O body deadline, never a response timestamp. *)
ConfirmedDeadlineNeverExtendedByResponse ==
    /\ ~history.deadlineFromResponse
    /\ (confirmedBody = RenewalBody => confirmedDeadline = requestDeadline)

(* The timeout of every physical send must fit inside the captured absolute budget. *)
NoRequestAfterSafeDeadline ==
    ~history.sentOutsideBudget

(* Once locally terminal, old-epoch authority remains fenced through all late outcomes. *)
TerminalNeverRearmsAuthority ==
    /\ ~history.rearmedAfterTerminal
    /\ (terminalSeen => ~localAuthority)

(* All physical copies retain the original body, attempt, and expected predecessor token. *)
OneLogicalBodyPerExpectedToken ==
    /\ ~history.tupleChanged
    /\ (logicalCreated
        => /\ requestAttempt = CurrentAttempt
           /\ requestBody = RenewalBody
           /\ requestExpectedToken = PredecessorToken)

(* Direct acknowledgement is possible only after that exact body and token are durable. *)
AcknowledgedRenewalIsDurable ==
    ~history.badAcknowledgement

(* A post-terminal old conditional copy cannot replace an installed successor. *)
LateDeliveryCannotOverwriteSuccessor ==
    ~history.lateOverwroteSuccessor

(* This is a transition-history guard, not a claim that all unresolved results have pending I/O. *)
PendingSurvivesLocalTerminal ==
    ~history.terminalDroppedPending

(* Conditional atomicity lets at most one physical copy consume the predecessor token. *)
OneIncarnationPerPredecessor ==
    predecessorLandings <= 1

(* The next beat derives from the pre-I/O cadence anchor, not result delivery time. *)
CadenceAnchoredAtAttemptStart ==
    ~nextBeatScheduled \/ nextBeat = cadenceAnchor + CadencePeriod

(* Negated reachability checks: TLC must violate each exact witness name. *)
WitnessDirectRetry == ~history.directRetryReached
WitnessReadAdoption == ~history.readAdoptionReached
WitnessExhaustionFences == ~history.exhaustionFencesReached
WitnessLateBeforeReclaim == ~history.lateBeforeReclaimReached
WitnessLateAfterSuccessor == ~history.lateAfterSuccessorReached
WitnessImmediateCatchup == ~history.catchupReached

================================================================================
