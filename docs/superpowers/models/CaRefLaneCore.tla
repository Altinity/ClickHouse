----------------------------- MODULE CaRefLaneCore -----------------------------
(*
The content-addressed ref append lane, expressed as ownership states rather than
as the product of implementation outcomes.

The model deliberately does not enumerate `WedgeResolution`,
`SlotOccupyResult::Kind`, occupant classes, and diagnostic reasons. Those are
nested computations in the implementation, not independent state dimensions.
Every action below instead names one semantic state transition and its owner.

The exact append attempt is installed in `Writing` before any request can be
sent. An unresolved return changes only its owner: `Writing -> Wedged`. A
known-durable transaction that cannot be installed goes directly to
`NeedsRecovery`; no later id is allocated until recovery has replayed it.
*)
EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    MaxTxn,
    SabotageNoArm,
    SabotageDropUncertain,
    SabotageAppendBlocked,
    SabotageIncompleteRecovery,
    SabotageSkipIdentity,
    SabotageNoFence,
    SabotageCertifyBlocked

Bindings == {"token", "other", "none"}
Tokens == {"a", "b"}
LaneStates == {"Ready", "Writing", "Wedged", "NeedsRecovery", "Closed", "Faulted"}
Observations == {"none", "pending", "Durable", "Unknown", "SuccessorSeal", "Foreign"}
AttemptValues == [id : 1..MaxTxn, token : Tokens, binding : Bindings]
NoAttempt == [id |-> 0, token |-> "none", binding |-> "none"]

VARIABLES
    lane,
    cache_id,
    durable_id,
    cache_binding,
    durable_binding,
    attempt,
    runtime_generation,
    authority_generation,
    resolver_attempt,
    resolver_generation,
    observation,
    bad_append,
    bad_install,
    bad_certification,
    saw_commit,
    saw_unresolved,
    saw_retry_created,
    saw_recovery,
    saw_stale_result,
    saw_closed,
    saw_faulted

vars ==
    << lane, cache_id, durable_id, cache_binding, durable_binding, attempt,
       runtime_generation, authority_generation, resolver_attempt,
       resolver_generation, observation, bad_append, bad_install,
       bad_certification, saw_commit, saw_unresolved, saw_retry_created,
       saw_recovery, saw_stale_result, saw_closed, saw_faulted >>

CurrentRuntime == runtime_generation = authority_generation
Outstanding == lane \in {"Writing", "Wedged"}

Init ==
    /\ lane = "Ready"
    /\ cache_id = 0
    /\ durable_id = 0
    /\ cache_binding = "token"
    /\ durable_binding = "token"
    /\ attempt = NoAttempt
    /\ runtime_generation = 1
    /\ authority_generation = 1
    /\ resolver_attempt = NoAttempt
    /\ resolver_generation = 0
    /\ observation = "none"
    /\ bad_append = FALSE
    /\ bad_install = FALSE
    /\ bad_certification = FALSE
    /\ saw_commit = FALSE
    /\ saw_unresolved = FALSE
    /\ saw_retry_created = FALSE
    /\ saw_recovery = FALSE
    /\ saw_stale_result = FALSE
    /\ saw_closed = FALSE
    /\ saw_faulted = FALSE

StartWrite(new_binding, token) ==
    /\ ~SabotageNoArm
    /\ lane = "Ready"
    /\ CurrentRuntime
    /\ durable_id = cache_id
    /\ cache_id < MaxTxn
    /\ lane' = "Writing"
    /\ attempt' = [id |-> cache_id + 1, token |-> token, binding |-> new_binding]
    /\ UNCHANGED << cache_id, durable_id, cache_binding, durable_binding,
                    runtime_generation, authority_generation, resolver_attempt,
                    resolver_generation, observation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_recovery, saw_stale_result,
                    saw_closed, saw_faulted >>

(* Sabotage: make the durable effect while the lane still advertises `Ready`. *)
UnarmedWrite(new_binding) ==
    /\ SabotageNoArm
    /\ lane = "Ready"
    /\ CurrentRuntime
    /\ durable_id = cache_id
    /\ durable_id < MaxTxn
    /\ durable_id' = durable_id + 1
    /\ durable_binding' = new_binding
    /\ UNCHANGED << lane, cache_id, cache_binding, attempt,
                    runtime_generation, authority_generation, resolver_attempt,
                    resolver_generation, observation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_recovery, saw_stale_result,
                    saw_closed, saw_faulted >>

WriteLands ==
    /\ lane = "Writing"
    /\ durable_id = cache_id
    /\ durable_id' = attempt.id
    /\ durable_binding' = attempt.binding
    /\ UNCHANGED << lane, cache_id, cache_binding, attempt,
                    runtime_generation, authority_generation, resolver_attempt,
                    resolver_generation, observation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_recovery, saw_stale_result,
                    saw_closed, saw_faulted >>

InstallCommitted ==
    /\ lane = "Writing"
    /\ CurrentRuntime
    /\ durable_id = attempt.id
    /\ cache_id = attempt.id - 1
    /\ lane' = "Ready"
    /\ cache_id' = attempt.id
    /\ cache_binding' = attempt.binding
    /\ attempt' = NoAttempt
    /\ saw_commit' = TRUE
    /\ UNCHANGED << durable_id, durable_binding, runtime_generation,
                    authority_generation, resolver_attempt, resolver_generation,
                    observation, bad_append, bad_install, bad_certification,
                    saw_unresolved, saw_retry_created, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

WriteDefinitelyRejected ==
    /\ lane = "Writing"
    /\ durable_id = cache_id
    /\ lane' = "Ready"
    /\ attempt' = NoAttempt
    /\ UNCHANGED << cache_id, durable_id, cache_binding, durable_binding,
                    runtime_generation, authority_generation, resolver_attempt,
                    resolver_generation, observation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_recovery, saw_stale_result,
                    saw_closed, saw_faulted >>

WriteUnresolved ==
    /\ lane = "Writing"
    /\ lane' = "Wedged"
    /\ saw_unresolved' = TRUE
    /\ UNCHANGED << cache_id, durable_id, cache_binding, durable_binding,
                    attempt, runtime_generation, authority_generation,
                    resolver_attempt, resolver_generation, observation,
                    bad_append, bad_install, bad_certification, saw_commit,
                    saw_retry_created, saw_recovery, saw_stale_result,
                    saw_closed, saw_faulted >>

DropUncertain ==
    /\ SabotageDropUncertain
    /\ lane \in {"Writing", "Wedged"}
    /\ lane' = "Ready"
    /\ attempt' = NoAttempt
    /\ UNCHANGED << cache_id, durable_id, cache_binding, durable_binding,
                    runtime_generation, authority_generation, resolver_attempt,
                    resolver_generation, observation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_recovery, saw_stale_result,
                    saw_closed, saw_faulted >>

BeginResolve ==
    /\ lane = "Wedged"
    /\ resolver_attempt = NoAttempt
    /\ resolver_attempt' = attempt
    /\ resolver_generation' = runtime_generation
    /\ observation' = "pending"
    /\ UNCHANGED << lane, cache_id, durable_id, cache_binding,
                    durable_binding, attempt, runtime_generation,
                    authority_generation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_recovery, saw_stale_result,
                    saw_closed, saw_faulted >>

ObserveDurable ==
    /\ observation = "pending"
    /\ resolver_attempt # NoAttempt
    /\ durable_id \in {resolver_attempt.id - 1, resolver_attempt.id}
    /\ observation' = "Durable"
    /\ durable_id' = resolver_attempt.id
    /\ durable_binding' = resolver_attempt.binding
    /\ saw_retry_created' = (saw_retry_created \/ (durable_id = resolver_attempt.id - 1))
    /\ UNCHANGED << lane, cache_id, cache_binding, attempt,
                    runtime_generation, authority_generation, resolver_attempt,
                    resolver_generation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

ObserveUnknown ==
    /\ observation = "pending"
    /\ observation' = "Unknown"
    /\ UNCHANGED << lane, cache_id, durable_id, cache_binding,
                    durable_binding, attempt, runtime_generation,
                    authority_generation, resolver_attempt,
                    resolver_generation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_recovery, saw_stale_result,
                    saw_closed, saw_faulted >>

ObserveSuccessorSeal ==
    /\ observation = "pending"
    /\ observation' = "SuccessorSeal"
    /\ UNCHANGED << lane, cache_id, durable_id, cache_binding,
                    durable_binding, attempt, runtime_generation,
                    authority_generation, resolver_attempt,
                    resolver_generation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_recovery, saw_stale_result,
                    saw_closed, saw_faulted >>

ObserveForeign ==
    /\ observation = "pending"
    /\ observation' = "Foreign"
    /\ UNCHANGED << lane, cache_id, durable_id, cache_binding,
                    durable_binding, attempt, runtime_generation,
                    authority_generation, resolver_attempt,
                    resolver_generation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_recovery, saw_stale_result,
                    saw_closed, saw_faulted >>

SameResolution ==
    /\ lane = "Wedged"
    /\ resolver_attempt # NoAttempt
    /\ resolver_attempt = attempt
    /\ resolver_generation = runtime_generation

ApplyResolution ==
    /\ observation \in {"Durable", "Unknown", "SuccessorSeal", "Foreign"}
    /\ IF SameResolution
          THEN CASE observation = "Durable" ->
                    IF CurrentRuntime \/ SabotageNoFence
                      THEN /\ lane' = "Ready"
                           /\ cache_id' = resolver_attempt.id
                           /\ cache_binding' = resolver_attempt.binding
                           /\ attempt' = NoAttempt
                           /\ bad_install' =
                               (bad_install \/ (~SabotageSkipIdentity /\ resolver_attempt # attempt))
                           /\ UNCHANGED << saw_closed, saw_faulted >>
                      ELSE /\ lane' = "NeedsRecovery"
                           /\ attempt' = NoAttempt
                           /\ UNCHANGED << cache_id, cache_binding, bad_install,
                                           saw_closed, saw_faulted >>
               [] observation = "Unknown" ->
                    /\ lane' = lane
                    /\ UNCHANGED << cache_id, cache_binding, attempt, bad_install,
                                    saw_closed, saw_faulted >>
               [] observation = "SuccessorSeal" ->
                    IF CurrentRuntime \/ SabotageNoFence
                      THEN /\ lane' = "Closed"
                           /\ attempt' = NoAttempt
                           /\ saw_closed' = TRUE
                           /\ UNCHANGED << cache_id, cache_binding, bad_install, saw_faulted >>
                      ELSE /\ UNCHANGED << lane, cache_id, cache_binding, attempt,
                                           bad_install, saw_closed, saw_faulted >>
               [] observation = "Foreign" ->
                    IF CurrentRuntime \/ SabotageNoFence
                      THEN /\ lane' = "Faulted"
                           /\ attempt' = NoAttempt
                           /\ saw_faulted' = TRUE
                           /\ UNCHANGED << cache_id, cache_binding, bad_install, saw_closed >>
                      ELSE /\ UNCHANGED << lane, cache_id, cache_binding, attempt,
                                           bad_install, saw_closed, saw_faulted >>
          ELSE /\ UNCHANGED << lane, cache_id, cache_binding, attempt, bad_install >>
               /\ UNCHANGED << saw_closed, saw_faulted >>
    /\ resolver_attempt' = NoAttempt
    /\ resolver_generation' = 0
    /\ observation' = "none"
    /\ UNCHANGED << durable_id, durable_binding, runtime_generation,
                    authority_generation, bad_append, bad_certification,
                    saw_commit, saw_unresolved, saw_retry_created, saw_recovery >>
    /\ IF ~SameResolution
          \/ (~CurrentRuntime /\ ~SabotageNoFence /\ observation # "Durable")
          THEN saw_stale_result' = TRUE
          ELSE UNCHANGED saw_stale_result

KnownDurableInstallFailure ==
    /\ observation = "Durable"
    /\ SameResolution
    /\ CurrentRuntime
    /\ lane' = "NeedsRecovery"
    /\ attempt' = NoAttempt
    /\ resolver_attempt' = NoAttempt
    /\ resolver_generation' = 0
    /\ observation' = "none"
    /\ UNCHANGED << cache_id, durable_id, cache_binding, durable_binding,
                    runtime_generation, authority_generation, bad_append,
                    bad_install, bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_recovery, saw_stale_result,
                    saw_closed, saw_faulted >>

Recover ==
    /\ lane = "NeedsRecovery"
    /\ CurrentRuntime
    /\ lane' = "Ready"
    /\ cache_id' = IF SabotageIncompleteRecovery THEN cache_id ELSE durable_id
    /\ cache_binding' = IF SabotageIncompleteRecovery THEN cache_binding ELSE durable_binding
    /\ saw_recovery' = TRUE
    /\ UNCHANGED << durable_id, durable_binding, attempt, runtime_generation,
                    authority_generation, resolver_attempt,
                    resolver_generation, observation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_stale_result, saw_closed,
                    saw_faulted >>

AppendWhileBlocked ==
    /\ SabotageAppendBlocked
    /\ lane \in {"Wedged", "NeedsRecovery", "Closed", "Faulted"}
    /\ durable_id < MaxTxn
    /\ durable_id' = durable_id + 1
    /\ durable_binding' = "other"
    /\ bad_append' = TRUE
    /\ UNCHANGED << lane, cache_id, cache_binding, attempt,
                    runtime_generation, authority_generation, resolver_attempt,
                    resolver_generation, observation, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_recovery, saw_stale_result,
                    saw_closed, saw_faulted >>

ReplaceAttempt ==
    /\ SabotageSkipIdentity
    /\ lane = "Wedged"
    /\ resolver_attempt # NoAttempt
    /\ attempt.token = "a"
    /\ attempt' = [attempt EXCEPT !.token = "b", !.binding = "other"]
    /\ UNCHANGED << lane, cache_id, durable_id, cache_binding,
                    durable_binding, runtime_generation, authority_generation,
                    resolver_attempt, resolver_generation, observation,
                    bad_append, bad_install, bad_certification, saw_commit,
                    saw_unresolved, saw_retry_created, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

ApplyWithoutIdentity ==
    /\ SabotageSkipIdentity
    /\ lane = "Wedged"
    /\ observation = "Durable"
    /\ resolver_attempt # attempt
    /\ resolver_generation = runtime_generation
    /\ CurrentRuntime
    /\ lane' = "Ready"
    /\ cache_id' = resolver_attempt.id
    /\ cache_binding' = resolver_attempt.binding
    /\ attempt' = NoAttempt
    /\ resolver_attempt' = NoAttempt
    /\ resolver_generation' = 0
    /\ observation' = "none"
    /\ bad_install' = TRUE
    /\ UNCHANGED << durable_id, durable_binding, runtime_generation,
                    authority_generation, bad_append, bad_certification,
                    saw_commit, saw_unresolved, saw_retry_created, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

FenceMove ==
    /\ authority_generation = 1
    /\ authority_generation' = 2
    /\ UNCHANGED << lane, cache_id, durable_id, cache_binding,
                    durable_binding, attempt, runtime_generation,
                    resolver_attempt, resolver_generation, observation,
                    bad_append, bad_install, bad_certification, saw_commit,
                    saw_unresolved, saw_retry_created, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

ForeignWrite ==
    /\ authority_generation # runtime_generation
    /\ durable_id < MaxTxn
    /\ durable_id' = durable_id + 1
    /\ durable_binding' = "other"
    /\ UNCHANGED << lane, cache_id, cache_binding, attempt,
                    runtime_generation, authority_generation, resolver_attempt,
                    resolver_generation, observation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_recovery, saw_stale_result,
                    saw_closed, saw_faulted >>

Remount ==
    /\ authority_generation # runtime_generation
    /\ lane' = "Ready"
    /\ cache_id' = durable_id
    /\ cache_binding' = durable_binding
    /\ attempt' = NoAttempt
    /\ runtime_generation' = authority_generation
    /\ resolver_attempt' = NoAttempt
    /\ resolver_generation' = 0
    /\ observation' = "none"
    /\ saw_recovery' = TRUE
    /\ UNCHANGED << durable_id, durable_binding, authority_generation,
                    bad_append, bad_install, bad_certification, saw_commit,
                    saw_unresolved, saw_retry_created, saw_stale_result,
                    saw_closed, saw_faulted >>

Certify ==
    /\ \/ (lane = "Ready" /\ (CurrentRuntime \/ SabotageNoFence))
       \/ (SabotageCertifyBlocked /\ lane # "Ready")
    /\ bad_certification' =
        (bad_certification
         \/ ~(lane = "Ready"
              /\ CurrentRuntime
              /\ cache_id = durable_id
              /\ cache_binding = durable_binding))
    /\ UNCHANGED << lane, cache_id, durable_id, cache_binding,
                    durable_binding, attempt, runtime_generation,
                    authority_generation, resolver_attempt,
                    resolver_generation, observation, bad_append, bad_install,
                    saw_commit, saw_unresolved, saw_retry_created, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

Next ==
    \/ \E b \in Bindings, t \in Tokens : StartWrite(b, t)
    \/ \E b \in Bindings : UnarmedWrite(b)
    \/ WriteLands
    \/ InstallCommitted
    \/ WriteDefinitelyRejected
    \/ WriteUnresolved
    \/ DropUncertain
    \/ BeginResolve
    \/ ObserveDurable
    \/ ObserveUnknown
    \/ ObserveSuccessorSeal
    \/ ObserveForeign
    \/ ApplyResolution
    \/ KnownDurableInstallFailure
    \/ Recover
    \/ AppendWhileBlocked
    \/ ReplaceAttempt
    \/ ApplyWithoutIdentity
    \/ FenceMove
    \/ Remount
    \/ Certify

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ lane \in LaneStates
    /\ cache_id \in 0..MaxTxn
    /\ durable_id \in 0..MaxTxn
    /\ cache_binding \in Bindings
    /\ durable_binding \in Bindings
    /\ attempt \in AttemptValues \cup {NoAttempt}
    /\ runtime_generation \in {1, 2}
    /\ authority_generation \in {1, 2}
    /\ resolver_attempt \in AttemptValues \cup {NoAttempt}
    /\ resolver_generation \in {0, 1, 2}
    /\ observation \in Observations
    /\ bad_append \in BOOLEAN
    /\ bad_install \in BOOLEAN
    /\ bad_certification \in BOOLEAN
    /\ saw_commit \in BOOLEAN
    /\ saw_unresolved \in BOOLEAN
    /\ saw_retry_created \in BOOLEAN
    /\ saw_recovery \in BOOLEAN
    /\ saw_stale_result \in BOOLEAN
    /\ saw_closed \in BOOLEAN
    /\ saw_faulted \in BOOLEAN
    /\ cache_id <= durable_id

ReadyCaughtUp ==
    (lane = "Ready" /\ CurrentRuntime)
        => (cache_id = durable_id /\ cache_binding = durable_binding)

OutstandingRetainsExactAttempt ==
    Outstanding
        => (attempt # NoAttempt
            /\ attempt.id = cache_id + 1
            /\ durable_id \in {cache_id, attempt.id})

NoAttemptOutsideOutstanding ==
    lane \notin {"Writing", "Wedged"} => attempt = NoAttempt

NoAppendWhileBlocked == ~bad_append
InstallMatchesAttempt == ~bad_install
CertifiedViewIsCurrent == ~bad_certification

W_Commit == ~saw_commit
W_Unresolved == ~saw_unresolved
W_RetryCreated == ~saw_retry_created
W_Recovery == ~saw_recovery
W_StaleResult == ~saw_stale_result
W_Closed == ~saw_closed
W_Faulted == ~saw_faulted

=============================================================================
