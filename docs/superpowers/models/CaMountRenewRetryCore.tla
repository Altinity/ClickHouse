-------------------------- MODULE CaMountRenewRetryCore --------------------------
(*
Focused safety model for split-phase conditional mount renewal without a lease deadline.

One old-epoch runtime starts from a confirmed predecessor and runs two logical renewals in
sequence. A logical renewal captures its attempt ID, body, expected token, body deadline, start,
and cadence anchor before I/O. Physical retries are collapsed to `outstanding`: every copy carries
that one tuple. A renewal is never bounded by the lease: it sends and retries past the confirmed
deadline and ends only on success, a definitive answer, or a cancel. Backend landing, response
delivery/loss, exact reads, local terminalization, and post-terminal delivery remain separate
actions. Copies still in flight when the next renewal starts become `staleOutstanding`.

Write authority is the holder's own timer, `LocalAuthority`: `clock < confirmedDeadline`, unless a
terminal outcome tripped the fence. Success sets the deadline to the renewal's start plus the lease.

The durable observers deliberately run at any time, also while local confirmation is pending. They
can install a same-pair twin, GC-fence or reclaim a body, install a successor epoch, or install a
foreign holder. They never touch local state. A takeover is entitled once the current token has
been stable for one lease (`tokenStableUntil`), or for the successor once the old runtime is locally
terminal. Honest conditional landing consumes the expected token; the multiplicity sabotage is the
sole action that lets two copies replace it.
*)
EXTENDS Naturals, FiniteSets

CONSTANTS
    AttemptIds,
    FirstAttempt,
    SecondAttempt,
    OtherAttempt,
    Holders,
    SelfHolder,
    ForeignHolder,
    PredecessorToken,
    FirstRenewalToken,
    SecondRenewalToken,
    TwinToken,
    SuccessorToken,
    ForeignToken,
    InitialConfirmedDeadline,
    MaxTime,
    LeaseDuration,
    CadencePeriod,
    MaxRetries,
    MaxOutstanding,
    Defects

AllowedDefects ==
    {"ignore_attempt_id",
     "refresh_deadline_from_response",
     "anchor_deadline_at_retry",
     "retry_with_new_body",
     "accept_after_terminal",
     "accept_successor",
     "drop_pending_on_terminal",
     "late_rearm",
     "response_relative_cadence",
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

Rounds == 1..2

AttemptOf(r) == IF r = 1 THEN FirstAttempt ELSE SecondAttempt
RenewalTokenOf(r) == IF r = 1 THEN FirstRenewalToken ELSE SecondRenewalToken

BodyNames ==
    {RenewalBody, RetryBody, PredecessorBody, SuccessorBody, ForeignBody, NoBody}
DurableKinds == {"Predecessor", "Renewal", "Twin", "Successor", "Foreign", "Absent"}
LocalStates ==
    {"Idle", "Sent", "Landed", "ResponseLost", "Resolved", "RetryWait",
     "Committed", "Conflict", "Unresolved"}
ResolveOutcomes == {"None", "Exact", "Predecessor", "Conflict"}
AdoptedKinds == {"None", "Renewal", "Twin", "Successor", "Foreign", "GCFenced"}

TokenSet ==
    {PredecessorToken, FirstRenewalToken, SecondRenewalToken, TwinToken, SuccessorToken,
     ForeignToken, NoToken}
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
    [tupleChanged : BOOLEAN,
     deadlineFromResponse : BOOLEAN,
     rearmedAfterTerminal : BOOLEAN,
     badAcknowledgement : BOOLEAN,
     lateOverwroteSuccessor : BOOLEAN,
     terminalDroppedPending : BOOLEAN,
     entitledTakeover : BOOLEAN,
     sawNonLandedTransient : BOOLEAN,
     sawLandedResponseLoss : BOOLEAN,
     sawRetrySent : BOOLEAN,
     directRetryReached : BOOLEAN,
     readAdoptionReached : BOOLEAN,
     lateOldLanded : BOOLEAN,
     lateBeforeReclaimReached : BOOLEAN,
     oldCopyRefused : BOOLEAN,
     lateAfterSuccessorReached : BOOLEAN,
     slowSuccess : BOOLEAN,
     catchupReached : BOOLEAN,
     retryPastDeadlineReached : BOOLEAN,
     staleSuccessSeen : BOOLEAN,
     authorityRestoredReached : BOOLEAN]

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
    [tupleChanged |-> FALSE,
     deadlineFromResponse |-> FALSE,
     rearmedAfterTerminal |-> FALSE,
     badAcknowledgement |-> FALSE,
     lateOverwroteSuccessor |-> FALSE,
     terminalDroppedPending |-> FALSE,
     entitledTakeover |-> FALSE,
     sawNonLandedTransient |-> FALSE,
     sawLandedResponseLoss |-> FALSE,
     sawRetrySent |-> FALSE,
     directRetryReached |-> FALSE,
     readAdoptionReached |-> FALSE,
     lateOldLanded |-> FALSE,
     lateBeforeReclaimReached |-> FALSE,
     oldCopyRefused |-> FALSE,
     lateAfterSuccessorReached |-> FALSE,
     slowSuccess |-> FALSE,
     catchupReached |-> FALSE,
     retryPastDeadlineReached |-> FALSE,
     staleSuccessSeen |-> FALSE,
     authorityRestoredReached |-> FALSE]

VARIABLES
    clock,
    durable,
    gcFenced,
    tokenStableUntil,
    confirmedBody,
    confirmedToken,
    confirmedDeadline,
    confirmedAnchor,
    committedRound,
    fenceLost,
    localState,
    resolveOutcome,
    round,
    requestAttempt,
    requestBody,
    requestExpectedToken,
    requestDeadline,
    attemptStartedAt,
    cadenceAnchor,
    firstSendAt,
    retryCount,
    outstanding,
    staleOutstanding,
    landings,
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
    <<clock, durable, gcFenced, tokenStableUntil,
      confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
      fenceLost, localState, resolveOutcome,
      round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
      attemptStartedAt, cadenceAnchor, firstSendAt,
      retryCount, outstanding, staleOutstanding, landings,
      adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
      nextBeatScheduled, nextBeat, reclaimSeen, history>>

Init ==
    /\ clock = 0
    /\ durable = InitialDurable
    /\ gcFenced = FALSE
    /\ tokenStableUntil = InitialConfirmedDeadline
    /\ confirmedBody = PredecessorBody
    /\ confirmedToken = PredecessorToken
    /\ confirmedDeadline = InitialConfirmedDeadline
    /\ confirmedAnchor = 0
    /\ committedRound = 0
    /\ fenceLost = FALSE
    /\ localState = "Idle"
    /\ resolveOutcome = "None"
    /\ round = 0
    /\ requestAttempt = FirstAttempt
    /\ requestBody = RenewalBody
    /\ requestExpectedToken = PredecessorToken
    /\ requestDeadline = 0
    /\ attemptStartedAt = 0
    /\ cadenceAnchor = 0
    /\ firstSendAt = [r \in Rounds |-> 0]
    /\ retryCount = 0
    /\ outstanding = 0
    /\ staleOutstanding = 0
    /\ landings = [r \in Rounds |-> 0]
    /\ adoptedAttempt = NoAttempt
    /\ adoptedKind = "None"
    /\ terminalSeen = FALSE
    /\ cancelled = FALSE
    /\ successObservedAt = 0
    /\ nextBeatScheduled = FALSE
    /\ nextBeat = 0
    /\ reclaimSeen = FALSE
    /\ history = InitialHistory

LogicalCreated == round > 0

(* The holder's write authority: its own timer and its own fence, nothing another process wrote. *)
LocalAuthority == ~fenceLost /\ clock < confirmedDeadline

(* An observer that waited one lease after the current token appeared may act on the slot. *)
ObserverWaitedOut == clock >= tokenStableUntil

RequestDurable ==
    [kind |-> "Renewal",
     token |-> RenewalTokenOf(round),
     holder |-> SelfHolder,
     epoch |-> 0,
     attempt |-> requestAttempt,
     body |-> requestBody,
     deadline |-> requestDeadline]

StaleDurable ==
    [kind |-> "Renewal",
     token |-> FirstRenewalToken,
     holder |-> SelfHolder,
     epoch |-> 0,
     attempt |-> FirstAttempt,
     body |-> RenewalBody,
     deadline |-> firstSendAt[1] + LeaseDuration]

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
    /\ durable.token = RenewalTokenOf(round)
    /\ durable.holder = SelfHolder
    /\ durable.epoch = 0
    /\ durable.attempt = AttemptOf(round)
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
    /\ durable.token = requestExpectedToken

TypeOK ==
    /\ AttemptIds = {FirstAttempt, SecondAttempt, OtherAttempt}
    /\ Cardinality(AttemptIds) = 3
    /\ Holders = {SelfHolder, ForeignHolder}
    /\ SelfHolder /= ForeignHolder
    /\ Cardinality({PredecessorToken, FirstRenewalToken, SecondRenewalToken, TwinToken,
                    SuccessorToken, ForeignToken}) = 6
    /\ NoToken \notin {PredecessorToken, FirstRenewalToken, SecondRenewalToken, TwinToken,
                       SuccessorToken, ForeignToken}
    /\ InitialConfirmedDeadline = 3
    /\ MaxTime = 5
    /\ LeaseDuration = 4
    /\ CadencePeriod = 2
    /\ MaxRetries = 1
    /\ MaxOutstanding = 2
    /\ Defects \subseteq AllowedDefects
    /\ Cardinality(Defects) <= 1
    /\ clock \in 0..MaxTime
    /\ durable \in DurableState
    /\ gcFenced \in BOOLEAN
    /\ tokenStableUntil \in DeadlineRange
    /\ confirmedBody \in BodyNames
    /\ confirmedToken \in TokenSet
    /\ confirmedDeadline \in DeadlineRange
    /\ confirmedAnchor \in 0..MaxTime
    /\ committedRound \in 0..2
    /\ fenceLost \in BOOLEAN
    /\ localState \in LocalStates
    /\ resolveOutcome \in ResolveOutcomes
    /\ round \in 0..2
    /\ requestAttempt \in AttemptIds
    /\ requestBody \in {RenewalBody, RetryBody}
    /\ requestExpectedToken \in TokenSet
    /\ requestDeadline \in DeadlineRange
    /\ attemptStartedAt \in 0..MaxTime
    /\ cadenceAnchor \in 0..MaxTime
    /\ firstSendAt \in [Rounds -> 0..MaxTime]
    /\ retryCount \in 0..MaxRetries
    /\ outstanding \in 0..MaxOutstanding
    /\ staleOutstanding \in 0..MaxOutstanding
    /\ landings \in [Rounds -> 0..MaxOutstanding]
    /\ adoptedAttempt \in AttemptSet
    /\ adoptedKind \in AdoptedKinds
    /\ terminalSeen \in BOOLEAN
    /\ cancelled \in BOOLEAN
    /\ successObservedAt \in 0..MaxTime
    /\ nextBeatScheduled \in BOOLEAN
    /\ nextBeat \in 0..(MaxTime + CadencePeriod)
    /\ reclaimSeen \in BOOLEAN
    /\ history \in HistoryState
    /\ (LogicalCreated => requestDeadline = firstSendAt[round] + LeaseDuration)
    /\ (nextBeatScheduled => localState = "Committed")

AdvanceTime ==
    /\ clock < MaxTime
    /\ clock' = clock + 1
    /\ UNCHANGED
        <<durable, gcFenced, tokenStableUntil,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          fenceLost, localState, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, outstanding, staleOutstanding, landings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

(* The first renewal starts at any time; the second one when the cadence after a commit is due.
   Neither needs authority: the start may already be past the confirmed deadline. *)
SendRenewal ==
    /\ ~terminalSeen
    /\ \/ round = 0
       \/ /\ round = 1
          /\ localState = "Committed"
          /\ nextBeatScheduled
          /\ clock >= nextBeat
          /\ ~cancelled
    /\ round' = round + 1
    /\ requestAttempt' = AttemptOf(round + 1)
    /\ requestBody' = RenewalBody
    /\ requestExpectedToken' = confirmedToken
    /\ requestDeadline' = clock + LeaseDuration
    /\ attemptStartedAt' = clock
    /\ cadenceAnchor' = clock
    /\ firstSendAt' = [firstSendAt EXCEPT ![round + 1] = clock]
    /\ localState' = "Sent"
    /\ resolveOutcome' = "None"
    /\ retryCount' = 0
    /\ outstanding' = 1
    /\ staleOutstanding' = staleOutstanding + outstanding
    /\ nextBeatScheduled' = FALSE
    /\ UNCHANGED
        <<clock, durable, gcFenced, tokenStableUntil,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          fenceLost, landings, adoptedAttempt, adoptedKind,
          terminalSeen, cancelled, successObservedAt, nextBeat, reclaimSeen, history>>

ResponseLoss ==
    /\ LogicalCreated
    /\ ~terminalSeen
    /\ localState \in {"Sent", "Landed"}
    /\ localState' = "ResponseLost"
    /\ resolveOutcome' = "None"
    /\ history' =
        [history EXCEPT
            !.sawNonLandedTransient = @ \/ (localState = "Sent"),
            !.sawLandedResponseLoss = @ \/ (localState = "Landed")]
    /\ UNCHANGED
        <<clock, durable, gcFenced, tokenStableUntil,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          fenceLost, round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, outstanding, staleOutstanding, landings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen>>

LandCopy ==
    /\ LogicalCreated
    /\ ~terminalSeen
    /\ localState \notin {"Committed", "Unresolved"}
    /\ outstanding > 0
    /\ PredecessorStillCurrent
    /\ landings[round] < MaxOutstanding
    /\ durable' = RequestDurable
    /\ gcFenced' = FALSE
    /\ tokenStableUntil' = clock + LeaseDuration
    /\ outstanding' = outstanding - 1
    /\ landings' = [landings EXCEPT ![round] = @ + 1]
    /\ localState' = IF localState = "ResponseLost" THEN localState ELSE "Landed"
    /\ UNCHANGED
        <<clock, confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor,
          committedRound, fenceLost, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt, retryCount, staleOutstanding,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

RefuseCopy ==
    /\ LogicalCreated
    /\ ~terminalSeen
    /\ localState \notin {"Committed", "Unresolved"}
    /\ outstanding > 0
    /\ ~PredecessorStillCurrent
    /\ outstanding' = outstanding - 1
    /\ localState' = IF localState = "ResponseLost" THEN localState ELSE "Conflict"
    /\ resolveOutcome' = IF localState = "ResponseLost" THEN resolveOutcome ELSE "Conflict"
    /\ UNCHANGED
        <<clock, durable, gcFenced, tokenStableUntil,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          fenceLost, round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, staleOutstanding, landings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

(* A copy of the previous renewal is conditional on a token that renewal already replaced. *)
StaleCopyDelivery ==
    /\ staleOutstanding > 0
    /\ staleOutstanding' = staleOutstanding - 1
    /\ IF ~gcFenced /\ durable.token = PredecessorToken
       THEN
           /\ durable' = StaleDurable
           /\ tokenStableUntil' = clock + LeaseDuration
           /\ landings' = [landings EXCEPT ![1] = @ + 1]
       ELSE
           UNCHANGED <<durable, tokenStableUntil, landings>>
    /\ UNCHANGED
        <<clock, gcFenced,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          fenceLost, localState, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt, retryCount, outstanding,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

DoubleConditionalLanding ==
    /\ Defect("double_conditional_landing")
    /\ LogicalCreated
    /\ ~terminalSeen
    /\ outstanding >= 2
    /\ landings[round] = 0
    /\ PredecessorStillCurrent
    /\ durable' = RequestDurable
    /\ gcFenced' = FALSE
    /\ tokenStableUntil' = clock + LeaseDuration
    /\ outstanding' = outstanding - 2
    /\ landings' = [landings EXCEPT ![round] = @ + 2]
    /\ localState' = IF localState = "ResponseLost" THEN localState ELSE "Landed"
    /\ UNCHANGED
        <<clock, confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor,
          committedRound, fenceLost, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt, retryCount, staleOutstanding,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

(* The history a commit of the current renewal at `clock` leaves; `newDeadline` is what it sets. *)
CommitHistory(newDeadline) ==
    [history EXCEPT
        !.slowSuccess = @ \/ (clock >= cadenceAnchor + CadencePeriod),
        !.staleSuccessSeen =
            @ \/ (round = 1 /\ firstSendAt[1] + LeaseDuration < clock),
        !.authorityRestoredReached =
            @ \/ (round = 2 /\ history.staleSuccessSeen /\ ~fenceLost /\ clock < newDeadline)]

CommittedDeadline ==
    IF Defect("refresh_deadline_from_response")
    THEN clock + LeaseDuration
    ELSE attemptStartedAt + LeaseDuration

AcknowledgeRenewal ==
    /\ LogicalCreated
    /\ ~terminalSeen
    /\ localState = "Landed"
    /\ ExactRequestDurable
    /\ ( ~Defect("refresh_deadline_from_response")
         \/ clock > attemptStartedAt )
    /\ localState' = "Committed"
    /\ confirmedBody' = durable.body
    /\ confirmedToken' = durable.token
    /\ confirmedDeadline' = CommittedDeadline
    /\ confirmedAnchor' = attemptStartedAt
    /\ committedRound' = round
    /\ adoptedAttempt' = durable.attempt
    /\ adoptedKind' = "Renewal"
    /\ successObservedAt' = clock
    /\ history' =
        [CommitHistory(CommittedDeadline) EXCEPT
            !.deadlineFromResponse = @ \/ Defect("refresh_deadline_from_response"),
            !.badAcknowledgement = @ \/ ~ExactRequestDurable,
            !.directRetryReached =
                @ \/ (history.sawNonLandedTransient /\ history.sawRetrySent)]
    /\ UNCHANGED
        <<clock, durable, gcFenced, tokenStableUntil, fenceLost, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, outstanding, staleOutstanding, landings,
          terminalSeen, cancelled, nextBeatScheduled, nextBeat, reclaimSeen>>

ExactResolve ==
    /\ LogicalCreated
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
        <<clock, durable, gcFenced, tokenStableUntil,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          fenceLost, round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, outstanding, staleOutstanding, landings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

AcceptResolved ==
    /\ LogicalCreated
    /\ ~terminalSeen
    /\ localState = "Resolved"
    /\ resolveOutcome = "Exact"
    /\ (ExactRequestDurable
        \/ (Defect("ignore_attempt_id") /\ TwinMatchesIgnoringId))
    /\ localState' = "Committed"
    /\ confirmedBody' = durable.body
    /\ confirmedToken' = durable.token
    /\ confirmedDeadline' = attemptStartedAt + LeaseDuration
    /\ confirmedAnchor' = attemptStartedAt
    /\ committedRound' = round
    /\ adoptedAttempt' = durable.attempt
    /\ adoptedKind' = durable.kind
    /\ successObservedAt' = clock
    /\ history' =
        [CommitHistory(attemptStartedAt + LeaseDuration) EXCEPT
            !.readAdoptionReached =
                @ \/ (history.sawLandedResponseLoss /\ ExactRequestDurable)]
    /\ UNCHANGED
        <<clock, durable, gcFenced, tokenStableUntil, fenceLost, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, outstanding, staleOutstanding, landings,
          terminalSeen, cancelled, nextBeatScheduled, nextBeat, reclaimSeen>>

EnterRetryWait ==
    /\ LogicalCreated
    /\ ~terminalSeen
    /\ ~cancelled
    /\ localState = "Resolved"
    /\ resolveOutcome = "Predecessor"
    /\ localState' = "RetryWait"
    /\ UNCHANGED
        <<clock, durable, gcFenced, tokenStableUntil,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          fenceLost, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, outstanding, staleOutstanding, landings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

(* `MaxRetries` bounds the state space only: running out of it is not a transition. *)
RetrySend ==
    /\ LogicalCreated
    /\ ~terminalSeen
    /\ ~cancelled
    /\ localState = "RetryWait"
    /\ retryCount < MaxRetries
    /\ outstanding < MaxOutstanding
    /\ localState' = "Sent"
    /\ resolveOutcome' = "None"
    /\ retryCount' = retryCount + 1
    /\ outstanding' = outstanding + 1
    /\ requestBody' =
        IF Defect("retry_with_new_body") THEN RetryBody ELSE requestBody
    /\ attemptStartedAt' =
        IF Defect("anchor_deadline_at_retry") THEN clock ELSE attemptStartedAt
    /\ history' =
        [history EXCEPT
            !.tupleChanged = @ \/ Defect("retry_with_new_body"),
            !.sawRetrySent = TRUE,
            !.retryPastDeadlineReached = @ \/ (clock >= confirmedDeadline)]
    /\ UNCHANGED
        <<clock, durable, gcFenced, tokenStableUntil,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          fenceLost, round, requestAttempt, requestExpectedToken, requestDeadline,
          cadenceAnchor, firstSendAt, staleOutstanding, landings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen>>

(* A stop or a remount request after the renewal sent something. *)
Cancel ==
    /\ LogicalCreated
    /\ ~terminalSeen
    /\ localState /= "Committed"
    /\ ~cancelled
    /\ cancelled' = TRUE
    /\ UNCHANGED
        <<clock, durable, gcFenced, tokenStableUntil,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          fenceLost, localState, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, outstanding, staleOutstanding, landings,
          adoptedAttempt, adoptedKind, terminalSeen, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

LocalTerminalize ==
    /\ LogicalCreated
    /\ ~terminalSeen
    /\ localState /= "Committed"
    /\ (cancelled \/ localState = "Conflict")
    /\ terminalSeen' = TRUE
    /\ fenceLost' = TRUE
    /\ localState' = "Unresolved"
    /\ resolveOutcome' = "Conflict"
    /\ outstanding' =
        IF Defect("drop_pending_on_terminal") /\ outstanding > 0
        THEN 0
        ELSE outstanding
    /\ history' =
        [history EXCEPT
            !.terminalDroppedPending =
                @ \/ (Defect("drop_pending_on_terminal") /\ outstanding > 0)]
    /\ UNCHANGED
        <<clock, durable, gcFenced, tokenStableUntil,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, staleOutstanding, landings,
          adoptedAttempt, adoptedKind, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen>>

SamePairTwin ==
    /\ LogicalCreated
    /\ durable.token = requestExpectedToken
    /\ durable.token /= TwinToken
    /\ ~gcFenced
    /\ durable' = TwinDurable
    /\ tokenStableUntil' = clock + LeaseDuration
    /\ UNCHANGED
        <<clock, gcFenced,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          fenceLost, localState, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, outstanding, staleOutstanding, landings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen, history>>

GCFence ==
    /\ durable.kind /= "Absent"
    /\ ~gcFenced
    /\ gcFenced' = TRUE
    /\ tokenStableUntil' = clock + LeaseDuration
    /\ history' =
        [history EXCEPT !.entitledTakeover = @ \/ ObserverWaitedOut]
    /\ UNCHANGED
        <<clock, durable,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          fenceLost, localState, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, outstanding, staleOutstanding, landings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen>>

(* Same process, new epoch: entitled by waiting, or by the old runtime being locally terminal. *)
SuccessorClaim ==
    /\ LogicalCreated
    /\ durable.kind /= "Successor"
    /\ durable' = SuccessorDurable
    /\ gcFenced' = FALSE
    /\ tokenStableUntil' = clock + LeaseDuration
    /\ history' =
        [history EXCEPT !.entitledTakeover = @ \/ ObserverWaitedOut \/ terminalSeen]
    /\ UNCHANGED
        <<clock,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          fenceLost, localState, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, outstanding, staleOutstanding, landings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen>>

ForeignHolderWrite ==
    /\ LogicalCreated
    /\ durable.kind /= "Foreign"
    /\ durable' = ForeignDurable
    /\ gcFenced' = FALSE
    /\ tokenStableUntil' = clock + LeaseDuration
    /\ history' =
        [history EXCEPT !.entitledTakeover = @ \/ ObserverWaitedOut]
    /\ UNCHANGED
        <<clock,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          fenceLost, localState, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, outstanding, staleOutstanding, landings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen>>

Reclaim ==
    /\ gcFenced
    /\ durable.kind /= "Absent"
    /\ durable' = EmptyDurable
    /\ gcFenced' = FALSE
    /\ tokenStableUntil' = clock + LeaseDuration
    /\ reclaimSeen' = TRUE
    /\ history' =
        [history EXCEPT
            !.lateBeforeReclaimReached =
                @ \/ (history.lateOldLanded /\ terminalSeen /\ ~LocalAuthority)]
    /\ UNCHANGED
        <<clock,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          fenceLost, localState, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, outstanding, staleOutstanding, landings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat>>

LateDeliveryLands ==
    /\ terminalSeen
    /\ localState = "Unresolved"
    /\ outstanding > 0
    /\ PredecessorStillCurrent
    /\ landings[round] < MaxOutstanding
    /\ durable' = RequestDurable
    /\ gcFenced' = FALSE
    /\ tokenStableUntil' = clock + LeaseDuration
    /\ outstanding' = outstanding - 1
    /\ landings' = [landings EXCEPT ![round] = @ + 1]
    /\ fenceLost' = ~Defect("late_rearm")
    /\ history' =
        [history EXCEPT
            !.rearmedAfterTerminal = @ \/ Defect("late_rearm"),
            !.lateOverwroteSuccessor = @ \/ (durable.kind = "Successor"),
            !.lateOldLanded = TRUE]
    /\ UNCHANGED
        <<clock,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          localState, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt, retryCount, staleOutstanding,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen>>

LateDeliveryRefused ==
    /\ terminalSeen
    /\ localState = "Unresolved"
    /\ outstanding > 0
    /\ ~PredecessorStillCurrent
    /\ outstanding' = outstanding - 1
    /\ history' =
        [history EXCEPT
            !.oldCopyRefused = @ \/ (durable.kind = "Successor"),
            !.lateAfterSuccessorReached =
                @ \/ (durable.kind = "Successor" /\ ~gcFenced)]
    /\ UNCHANGED
        <<clock, durable, gcFenced, tokenStableUntil,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          fenceLost, localState, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, staleOutstanding, landings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          nextBeatScheduled, nextBeat, reclaimSeen>>

AcceptAfterTerminal ==
    /\ Defect("accept_after_terminal")
    /\ terminalSeen
    /\ localState = "Unresolved"
    /\ ExactRequestDurable
    /\ localState' = "Committed"
    /\ fenceLost' = FALSE
    /\ confirmedBody' = durable.body
    /\ confirmedToken' = durable.token
    /\ confirmedDeadline' = attemptStartedAt + LeaseDuration
    /\ confirmedAnchor' = attemptStartedAt
    /\ committedRound' = round
    /\ adoptedAttempt' = durable.attempt
    /\ adoptedKind' = "Renewal"
    /\ successObservedAt' = clock
    /\ history' =
        [history EXCEPT
            !.rearmedAfterTerminal = TRUE]
    /\ UNCHANGED
        <<clock, durable, gcFenced, tokenStableUntil, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, outstanding, staleOutstanding, landings,
          terminalSeen, cancelled, nextBeatScheduled, nextBeat, reclaimSeen>>

AcceptSuccessor ==
    /\ Defect("accept_successor")
    /\ ~terminalSeen
    /\ localState = "Conflict"
    /\ durable.kind = "Successor"
    /\ ~gcFenced
    /\ localState' = "Committed"
    /\ confirmedBody' = durable.body
    /\ confirmedToken' = durable.token
    /\ confirmedDeadline' = durable.deadline
    /\ confirmedAnchor' = attemptStartedAt
    /\ committedRound' = round
    /\ adoptedAttempt' = durable.attempt
    /\ adoptedKind' = "Successor"
    /\ successObservedAt' = clock
    /\ history' =
        [history EXCEPT
            !.badAcknowledgement = TRUE]
    /\ UNCHANGED
        <<clock, durable, gcFenced, tokenStableUntil, fenceLost, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, outstanding, staleOutstanding, landings,
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
        <<clock, durable, gcFenced, tokenStableUntil,
          confirmedBody, confirmedToken, confirmedDeadline, confirmedAnchor, committedRound,
          fenceLost, localState, resolveOutcome,
          round, requestAttempt, requestBody, requestExpectedToken, requestDeadline,
          attemptStartedAt, cadenceAnchor, firstSendAt,
          retryCount, outstanding, staleOutstanding, landings,
          adoptedAttempt, adoptedKind, terminalSeen, cancelled, successObservedAt,
          reclaimSeen>>

Next ==
    \/ AdvanceTime
    \/ SendRenewal
    \/ ResponseLoss
    \/ LandCopy
    \/ RefuseCopy
    \/ StaleCopyDelivery
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

(* Only the exact symbolic `write_attempt_id` of the committed renewal may be adopted. *)
ExactAttemptOnly ==
    adoptedAttempt = NoAttempt \/ adoptedAttempt = AttemptOf(committedRound)

(* Twins, GC-fenced bodies, successor epochs, and foreign holders remain fail closed. *)
ForeignOrSuccessorNeverAdopted ==
    adoptedKind \in {"None", "Renewal"}

(* A commit sets the deadline from the start it used, never from a response timestamp. *)
ConfirmedDeadlineNeverExtendedByResponse ==
    /\ ~history.deadlineFromResponse
    /\ (committedRound > 0 => confirmedDeadline = confirmedAnchor + LeaseDuration)

(* Once locally terminal, old-epoch authority remains fenced through all late outcomes. *)
TerminalNeverRearmsAuthority ==
    /\ ~history.rearmedAfterTerminal
    /\ (terminalSeen => ~LocalAuthority)

(* All physical copies of a renewal retain its body, attempt, and expected token. *)
OneLogicalBodyPerExpectedToken ==
    /\ ~history.tupleChanged
    /\ (LogicalCreated
        => /\ requestAttempt = AttemptOf(round)
           /\ requestBody = RenewalBody
           /\ requestExpectedToken =
                IF round = 1 THEN PredecessorToken ELSE RenewalTokenOf(1))

(* Direct acknowledgement is possible only after that exact body and token are durable. *)
AcknowledgedRenewalIsDurable ==
    ~history.badAcknowledgement

(* A post-terminal old conditional copy cannot replace an installed successor. *)
LateDeliveryCannotOverwriteSuccessor ==
    ~history.lateOverwroteSuccessor

(* This is a transition-history guard, not a claim that all unresolved results have pending I/O. *)
PendingSurvivesLocalTerminal ==
    ~history.terminalDroppedPending

(* Conditional atomicity lets at most one physical copy consume each renewal's expected token. *)
OneIncarnationPerPredecessor ==
    \A r \in Rounds : landings[r] <= 1

(* The next beat derives from the renewal's first send, not result delivery time. *)
CadenceAnchoredAtAttemptStart ==
    nextBeatScheduled
        => /\ nextBeat = cadenceAnchor + CadencePeriod
           /\ cadenceAnchor = firstSendAt[round]

(* The first conjunct holds by the definition of `LocalAuthority`; the second compares the
   deadline with `firstSendAt`, which only `SendRenewal` writes. *)
AuthorityEndsAtConfirmedDeadline ==
    /\ (LocalAuthority => clock < confirmedDeadline)
    /\ confirmedDeadline =
        IF committedRound = 0
        THEN InitialConfirmedDeadline
        ELSE firstSendAt[committedRound] + LeaseDuration

(* No authority once an observer that waited, or the successor after a local terminal, took over. *)
NoAuthorityAfterEntitledTakeover ==
    history.entitledTakeover => ~LocalAuthority

(* Negated reachability checks: TLC must violate each exact witness name. *)
WitnessDirectRetry == ~history.directRetryReached
WitnessReadAdoption == ~history.readAdoptionReached
WitnessLateBeforeReclaim == ~history.lateBeforeReclaimReached
WitnessLateAfterSuccessor == ~history.lateAfterSuccessorReached
WitnessImmediateCatchup == ~history.catchupReached
WitnessRetryPastDeadline == ~history.retryPastDeadlineReached
WitnessAuthorityRestored == ~history.authorityRestoredReached

================================================================================
