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
    SabotageCertifyBlocked,
    SabotageOldHandleRetarget,
    SabotageLateInvalidation,
    SabotageFenceLossPublication,
    SabotageMissingConfirmationAllocation

Bindings == {"token", "other", "none"}
Tokens == {"a", "b"}
LaneStates == {"Ready", "Writing", "Wedged", "NeedsRecovery", "Closed", "Faulted"}
Observations == {"none", "pending", "Durable", "Unknown", "SuccessorSeal", "Foreign"}
AttemptValues == [id : 1..MaxTxn, token : Tokens, binding : Bindings]
NoAttempt == [id |-> 0, token |-> "none", binding |-> "none"]

(*
Two runtime ids and two durable life ids are sufficient for every identity
interleaving in scope: one predecessor, one successor, removal/rebirth, and a
self-remount that reuses the durable life with a later accepted fence. The
six-state lane remains single-copy and belongs to the captured predecessor;
the cache submodel below does not multiply that state-space product.
*)
RuntimeIds == {"r1", "r2"}
NoRuntime == "none"
Lives == {1, 2}
NoIdentity == [life |-> 0, admitted |-> 0]
RuntimeIdentityValues ==
    [life : 0..2, admitted : 0..3]
ArmPhases == {"Armed", "Fenced", "ArmPending"}
OldOperations == {"append", "read", "publish", "resolve"}

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
    saw_durable_adoption,
    saw_recovery,
    saw_stale_result,
    saw_closed,
    saw_faulted,
    slot_runtime,
    old_handle_runtime,
    live_runtimes,
    runtime_identity,
    identity_ledger,
    runtime_cache_marker,
    runtime_durable_marker,
    catalog_life,
    observed_lives,
    accepted_generations,
    arm_phase,
    pending_invalidation,
    old_action_target,
    old_action_kind,
    bad_late_detach,
    bad_missing_allocation,
    saw_rebirth_old_action,
    saw_self_remount,
    saw_late_invalidation_preserved,
    saw_missing_confirmation

laneVars ==
    << lane, cache_id, durable_id, cache_binding, durable_binding, attempt,
       runtime_generation, authority_generation, resolver_attempt,
       resolver_generation, observation, bad_append, bad_install,
       bad_certification, saw_commit, saw_unresolved, saw_retry_created,
       saw_durable_adoption, saw_recovery, saw_stale_result, saw_closed, saw_faulted >>

runtimeVars ==
    << slot_runtime, old_handle_runtime, live_runtimes, runtime_identity,
       identity_ledger, runtime_cache_marker, runtime_durable_marker,
       catalog_life, observed_lives, accepted_generations, arm_phase,
       pending_invalidation, old_action_target, old_action_kind,
       bad_late_detach, bad_missing_allocation, saw_rebirth_old_action,
       saw_self_remount, saw_late_invalidation_preserved,
       saw_missing_confirmation >>

vars == << laneVars, runtimeVars >>

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
    /\ saw_durable_adoption = FALSE
    /\ saw_recovery = FALSE
    /\ saw_stale_result = FALSE
    /\ saw_closed = FALSE
    /\ saw_faulted = FALSE
    /\ slot_runtime = "r1"
    /\ old_handle_runtime = "r1"
    /\ live_runtimes = {"r1"}
    /\ runtime_identity =
        [r \in RuntimeIds |-> IF r = "r1" THEN [life |-> 1, admitted |-> 1] ELSE NoIdentity]
    /\ identity_ledger =
        [r \in RuntimeIds |-> IF r = "r1" THEN [life |-> 1, admitted |-> 1] ELSE NoIdentity]
    /\ runtime_cache_marker = [r \in RuntimeIds |-> 0]
    /\ runtime_durable_marker = [r \in RuntimeIds |-> 0]
    /\ catalog_life = 1
    /\ observed_lives = {1}
    /\ accepted_generations = {1}
    /\ arm_phase = "Armed"
    /\ pending_invalidation = NoRuntime
    /\ old_action_target = NoRuntime
    /\ old_action_kind = "none"
    /\ bad_late_detach = FALSE
    /\ bad_missing_allocation = FALSE
    /\ saw_rebirth_old_action = FALSE
    /\ saw_self_remount = FALSE
    /\ saw_late_invalidation_preserved = FALSE
    /\ saw_missing_confirmation = FALSE

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
                    saw_retry_created, saw_durable_adoption, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

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
                    saw_retry_created, saw_durable_adoption, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

WriteLands ==
    /\ lane = "Writing"
    /\ durable_id = cache_id
    /\ durable_id' = attempt.id
    /\ durable_binding' = attempt.binding
    /\ UNCHANGED << lane, cache_id, cache_binding, attempt,
                    runtime_generation, authority_generation, resolver_attempt,
                    resolver_generation, observation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_durable_adoption, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

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
                    saw_unresolved, saw_retry_created, saw_durable_adoption,
                    saw_recovery, saw_stale_result, saw_closed, saw_faulted >>

WriteDefinitelyRejected ==
    /\ lane = "Writing"
    /\ durable_id = cache_id
    /\ lane' = "Ready"
    /\ attempt' = NoAttempt
    /\ UNCHANGED << cache_id, durable_id, cache_binding, durable_binding,
                    runtime_generation, authority_generation, resolver_attempt,
                    resolver_generation, observation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_durable_adoption, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

WriteUnresolved ==
    /\ lane = "Writing"
    /\ lane' = "Wedged"
    /\ saw_unresolved' = TRUE
    /\ UNCHANGED << cache_id, durable_id, cache_binding, durable_binding,
                    attempt, runtime_generation, authority_generation,
                    resolver_attempt, resolver_generation, observation,
                    bad_append, bad_install, bad_certification, saw_commit,
                    saw_retry_created, saw_durable_adoption, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

DropUncertain ==
    /\ SabotageDropUncertain
    /\ lane \in {"Writing", "Wedged"}
    /\ lane' = "Ready"
    /\ attempt' = NoAttempt
    /\ UNCHANGED << cache_id, durable_id, cache_binding, durable_binding,
                    runtime_generation, authority_generation, resolver_attempt,
                    resolver_generation, observation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_durable_adoption, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

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
                    saw_retry_created, saw_durable_adoption, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

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
                    bad_certification, saw_commit, saw_unresolved, saw_durable_adoption,
                    saw_recovery, saw_stale_result, saw_closed, saw_faulted >>

ObserveUnknown ==
    /\ observation = "pending"
    /\ observation' = "Unknown"
    /\ UNCHANGED << lane, cache_id, durable_id, cache_binding,
                    durable_binding, attempt, runtime_generation,
                    authority_generation, resolver_attempt,
                    resolver_generation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_durable_adoption, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

ObserveSuccessorSeal ==
    /\ observation = "pending"
    /\ observation' = "SuccessorSeal"
    /\ UNCHANGED << lane, cache_id, durable_id, cache_binding,
                    durable_binding, attempt, runtime_generation,
                    authority_generation, resolver_attempt,
                    resolver_generation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_durable_adoption, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

ObserveForeign ==
    /\ observation = "pending"
    /\ observation' = "Foreign"
    /\ UNCHANGED << lane, cache_id, durable_id, cache_binding,
                    durable_binding, attempt, runtime_generation,
                    authority_generation, resolver_attempt,
                    resolver_generation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_durable_adoption, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

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
                           /\ saw_durable_adoption' = TRUE
                           /\ UNCHANGED << saw_closed, saw_faulted >>
                      ELSE /\ lane' = "NeedsRecovery"
                           /\ attempt' = NoAttempt
                           /\ UNCHANGED << cache_id, cache_binding, bad_install,
                                           saw_durable_adoption, saw_closed, saw_faulted >>
               [] observation = "Unknown" ->
                    /\ lane' = lane
                    /\ UNCHANGED << cache_id, cache_binding, attempt, bad_install,
                                    saw_durable_adoption, saw_closed, saw_faulted >>
               [] observation = "SuccessorSeal" ->
                    IF CurrentRuntime \/ SabotageNoFence
                      THEN /\ lane' = "Closed"
                           /\ attempt' = NoAttempt
                           /\ saw_closed' = TRUE
                           /\ UNCHANGED << cache_id, cache_binding, bad_install,
                                           saw_durable_adoption, saw_faulted >>
                      ELSE /\ UNCHANGED << lane, cache_id, cache_binding, attempt,
                                           bad_install, saw_durable_adoption, saw_closed, saw_faulted >>
               [] observation = "Foreign" ->
                    IF CurrentRuntime \/ SabotageNoFence
                      THEN /\ lane' = "Faulted"
                           /\ attempt' = NoAttempt
                           /\ saw_faulted' = TRUE
                           /\ UNCHANGED << cache_id, cache_binding, bad_install,
                                           saw_durable_adoption, saw_closed >>
                      ELSE /\ UNCHANGED << lane, cache_id, cache_binding, attempt,
                                           bad_install, saw_durable_adoption, saw_closed, saw_faulted >>
          ELSE /\ UNCHANGED << lane, cache_id, cache_binding, attempt, bad_install,
                              saw_durable_adoption >>
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
                    saw_retry_created, saw_durable_adoption, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

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
                    saw_retry_created, saw_durable_adoption, saw_stale_result,
                    saw_closed, saw_faulted >>

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
                    saw_retry_created, saw_durable_adoption, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

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
                    saw_unresolved, saw_retry_created, saw_durable_adoption,
                    saw_recovery, saw_stale_result, saw_closed, saw_faulted >>

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
                    saw_commit, saw_unresolved, saw_retry_created, saw_durable_adoption,
                    saw_recovery, saw_stale_result, saw_closed, saw_faulted >>

FenceMove ==
    /\ authority_generation = 1
    /\ arm_phase = "Armed"
    /\ (~SabotageFenceLossPublication
        \/ (catalog_life \in Lives
            /\ "r2" \notin live_runtimes
            /\ identity_ledger["r2"] = NoIdentity))
    /\ authority_generation' = 2
    /\ arm_phase' = "Fenced"
    /\ IF SabotageFenceLossPublication
          THEN /\ slot_runtime' = "r2"
               /\ live_runtimes' = live_runtimes \cup {"r2"}
               /\ runtime_identity' =
                    [runtime_identity EXCEPT !["r2"] = [life |-> catalog_life, admitted |-> 2]]
               /\ identity_ledger' =
                    [identity_ledger EXCEPT !["r2"] = [life |-> catalog_life, admitted |-> 2]]
          ELSE /\ slot_runtime' = NoRuntime
               /\ UNCHANGED << live_runtimes, runtime_identity, identity_ledger >>
    /\ UNCHANGED << lane, cache_id, durable_id, cache_binding,
                    durable_binding, attempt, runtime_generation,
                    resolver_attempt, resolver_generation, observation,
                    bad_append, bad_install, bad_certification, saw_commit,
                    saw_unresolved, saw_retry_created, saw_durable_adoption,
                    saw_recovery, saw_stale_result, saw_closed, saw_faulted,
                    old_handle_runtime, runtime_cache_marker,
                    runtime_durable_marker, catalog_life, observed_lives,
                    accepted_generations, pending_invalidation,
                    old_action_target, old_action_kind, bad_late_detach,
                    bad_missing_allocation, saw_rebirth_old_action,
                    saw_self_remount, saw_late_invalidation_preserved,
                    saw_missing_confirmation >>

ForeignWrite ==
    /\ authority_generation # runtime_generation
    /\ durable_id < MaxTxn
    /\ durable_id' = durable_id + 1
    /\ durable_binding' = "other"
    /\ UNCHANGED << lane, cache_id, cache_binding, attempt,
                    runtime_generation, authority_generation, resolver_attempt,
                    resolver_generation, observation, bad_append, bad_install,
                    bad_certification, saw_commit, saw_unresolved,
                    saw_retry_created, saw_durable_adoption, saw_recovery,
                    saw_stale_result, saw_closed, saw_faulted >>

BeginRearm ==
    /\ arm_phase = "Fenced"
    /\ arm_phase' = "ArmPending"
    /\ UNCHANGED << laneVars, slot_runtime, old_handle_runtime,
                    live_runtimes, runtime_identity, identity_ledger,
                    runtime_cache_marker, runtime_durable_marker, catalog_life,
                    observed_lives, accepted_generations, pending_invalidation,
                    old_action_target, old_action_kind, bad_late_detach,
                    bad_missing_allocation, saw_rebirth_old_action,
                    saw_self_remount, saw_late_invalidation_preserved,
                    saw_missing_confirmation >>

FailRearm ==
    /\ arm_phase = "ArmPending"
    /\ arm_phase' = "Fenced"
    /\ UNCHANGED << laneVars, slot_runtime, old_handle_runtime,
                    live_runtimes, runtime_identity, identity_ledger,
                    runtime_cache_marker, runtime_durable_marker, catalog_life,
                    observed_lives, accepted_generations, pending_invalidation,
                    old_action_target, old_action_kind, bad_late_detach,
                    bad_missing_allocation, saw_rebirth_old_action,
                    saw_self_remount, saw_late_invalidation_preserved,
                    saw_missing_confirmation >>

AcceptRearm ==
    /\ arm_phase = "ArmPending"
    /\ authority_generation = 2
    /\ "r2" \notin live_runtimes
    /\ identity_ledger["r2"] = NoIdentity
    /\ catalog_life = 1
    /\ authority_generation' = 3
    /\ accepted_generations' = accepted_generations \cup {3}
    /\ arm_phase' = "Armed"
    /\ slot_runtime' = "r2"
    /\ live_runtimes' = live_runtimes \cup {"r2"}
    /\ runtime_identity' =
        [runtime_identity EXCEPT !["r2"] = [life |-> catalog_life, admitted |-> 3]]
    /\ identity_ledger' =
        [identity_ledger EXCEPT !["r2"] = [life |-> catalog_life, admitted |-> 3]]
    /\ saw_self_remount' = TRUE
    /\ UNCHANGED << lane, cache_id, durable_id, cache_binding,
                    durable_binding, attempt, runtime_generation,
                    resolver_attempt, resolver_generation, observation,
                    bad_append, bad_install, bad_certification, saw_commit,
                    saw_unresolved, saw_retry_created, saw_durable_adoption,
                    saw_recovery, saw_stale_result, saw_closed, saw_faulted,
                    old_handle_runtime, runtime_cache_marker,
                    runtime_durable_marker, catalog_life, observed_lives,
                    pending_invalidation, old_action_target, old_action_kind,
                    bad_late_detach, bad_missing_allocation,
                    saw_rebirth_old_action, saw_late_invalidation_preserved,
                    saw_missing_confirmation >>

RemoveCatalogLife ==
    /\ catalog_life = 1
    /\ pending_invalidation = NoRuntime
    /\ catalog_life' = 0
    /\ pending_invalidation' = slot_runtime
    /\ UNCHANGED << laneVars, slot_runtime, old_handle_runtime,
                    live_runtimes, runtime_identity, identity_ledger,
                    runtime_cache_marker, runtime_durable_marker,
                    observed_lives, accepted_generations, arm_phase,
                    old_action_target, old_action_kind, bad_late_detach,
                    bad_missing_allocation, saw_rebirth_old_action,
                    saw_self_remount, saw_late_invalidation_preserved,
                    saw_missing_confirmation >>

RebirthCatalogLife ==
    /\ catalog_life = 0
    /\ catalog_life' = 2
    /\ observed_lives' = observed_lives \cup {2}
    /\ UNCHANGED << laneVars, slot_runtime, old_handle_runtime,
                    live_runtimes, runtime_identity, identity_ledger,
                    runtime_cache_marker, runtime_durable_marker,
                    accepted_generations, arm_phase, pending_invalidation,
                    old_action_target, old_action_kind, bad_late_detach,
                    bad_missing_allocation, saw_rebirth_old_action,
                    saw_self_remount, saw_late_invalidation_preserved,
                    saw_missing_confirmation >>

FreshCatalogLookup ==
    /\ catalog_life = 2
    /\ arm_phase = "Armed"
    /\ "r2" \notin live_runtimes
    /\ identity_ledger["r2"] = NoIdentity
    /\ slot_runtime' = "r2"
    /\ live_runtimes' = live_runtimes \cup {"r2"}
    /\ runtime_identity' =
        [runtime_identity EXCEPT
            !["r2"] = [life |-> catalog_life, admitted |-> authority_generation]]
    /\ identity_ledger' =
        [identity_ledger EXCEPT
            !["r2"] = [life |-> catalog_life, admitted |-> authority_generation]]
    /\ UNCHANGED << laneVars, old_handle_runtime, runtime_cache_marker,
                    runtime_durable_marker, catalog_life, observed_lives,
                    accepted_generations, arm_phase, pending_invalidation,
                    old_action_target, old_action_kind, bad_late_detach,
                    bad_missing_allocation, saw_rebirth_old_action,
                    saw_self_remount, saw_late_invalidation_preserved,
                    saw_missing_confirmation >>

OldHandleOperation(kind) ==
    /\ live_runtimes = RuntimeIds
    /\ runtime_identity["r2"].life = 2
    /\ old_action_target = NoRuntime
    /\ LET target == IF SabotageOldHandleRetarget THEN slot_runtime ELSE old_handle_runtime
       IN /\ target \in RuntimeIds
          /\ runtime_cache_marker[target] = 0
          /\ runtime_durable_marker[target] = 0
          /\ old_action_target' = target
          /\ old_action_kind' = kind
          /\ runtime_cache_marker' =
                IF kind = "append"
                  THEN runtime_cache_marker
                  ELSE [runtime_cache_marker EXCEPT ![target] = 1]
          /\ runtime_durable_marker' =
                IF kind = "append"
                  THEN [runtime_durable_marker EXCEPT ![target] = 1]
                  ELSE runtime_durable_marker
          /\ saw_rebirth_old_action' = (target = old_handle_runtime)
    /\ UNCHANGED << laneVars, slot_runtime, old_handle_runtime,
                    live_runtimes, runtime_identity, identity_ledger,
                    catalog_life, observed_lives, accepted_generations,
                    arm_phase, pending_invalidation, bad_late_detach,
                    bad_missing_allocation, saw_self_remount,
                    saw_late_invalidation_preserved,
                    saw_missing_confirmation >>

LateExactInvalidation ==
    /\ pending_invalidation \in RuntimeIds
    /\ IF SabotageLateInvalidation
          THEN /\ slot_runtime' = NoRuntime
               /\ bad_late_detach' =
                    (bad_late_detach
                     \/ (slot_runtime \in RuntimeIds /\ slot_runtime # pending_invalidation))
          ELSE /\ slot_runtime' =
                    IF slot_runtime = pending_invalidation THEN NoRuntime ELSE slot_runtime
               /\ UNCHANGED bad_late_detach
    /\ saw_late_invalidation_preserved' =
        (slot_runtime = "r2" /\ pending_invalidation = "r1" /\ slot_runtime' = "r2")
    /\ pending_invalidation' = NoRuntime
    /\ UNCHANGED << laneVars, old_handle_runtime, live_runtimes,
                    runtime_identity, identity_ledger, runtime_cache_marker,
                    runtime_durable_marker, catalog_life, observed_lives,
                    accepted_generations, arm_phase, old_action_target,
                    old_action_kind, bad_missing_allocation,
                    saw_rebirth_old_action, saw_self_remount,
                    saw_missing_confirmation >>

ConfirmMissingName ==
    /\ catalog_life = 0
    /\ "r2" \notin live_runtimes
    /\ identity_ledger["r2"] = NoIdentity
    /\ IF SabotageMissingConfirmationAllocation
          THEN /\ slot_runtime' = "r2"
               /\ live_runtimes' = live_runtimes \cup {"r2"}
               /\ runtime_identity' =
                    [runtime_identity EXCEPT !["r2"] = [life |-> 0, admitted |-> 1]]
               /\ identity_ledger' =
                    [identity_ledger EXCEPT !["r2"] = [life |-> 0, admitted |-> 1]]
               /\ bad_missing_allocation' = TRUE
          ELSE /\ UNCHANGED << slot_runtime, live_runtimes,
                                runtime_identity, identity_ledger,
                                bad_missing_allocation >>
    /\ saw_missing_confirmation' = TRUE
    /\ UNCHANGED << laneVars, old_handle_runtime, runtime_cache_marker,
                    runtime_durable_marker, catalog_life, observed_lives,
                    accepted_generations, arm_phase, pending_invalidation,
                    old_action_target, old_action_kind, bad_late_detach,
                    saw_rebirth_old_action, saw_self_remount,
                    saw_late_invalidation_preserved >>

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
                    saw_commit, saw_unresolved, saw_retry_created, saw_durable_adoption,
                    saw_recovery, saw_stale_result, saw_closed, saw_faulted >>

LaneNext ==
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
    \/ Certify

Next ==
    \/ (LaneNext /\ UNCHANGED runtimeVars)
    \/ FenceMove
    \/ BeginRearm
    \/ FailRearm
    \/ AcceptRearm
    \/ RemoveCatalogLife
    \/ RebirthCatalogLife
    \/ FreshCatalogLookup
    \/ \E kind \in OldOperations : OldHandleOperation(kind)
    \/ LateExactInvalidation
    \/ ConfirmMissingName

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ lane \in LaneStates
    /\ cache_id \in 0..MaxTxn
    /\ durable_id \in 0..MaxTxn
    /\ cache_binding \in Bindings
    /\ durable_binding \in Bindings
    /\ attempt \in AttemptValues \cup {NoAttempt}
    /\ runtime_generation \in {1, 2, 3}
    /\ authority_generation \in {1, 2, 3}
    /\ resolver_attempt \in AttemptValues \cup {NoAttempt}
    /\ resolver_generation \in {0, 1, 2}
    /\ observation \in Observations
    /\ bad_append \in BOOLEAN
    /\ bad_install \in BOOLEAN
    /\ bad_certification \in BOOLEAN
    /\ saw_commit \in BOOLEAN
    /\ saw_unresolved \in BOOLEAN
    /\ saw_retry_created \in BOOLEAN
    /\ saw_durable_adoption \in BOOLEAN
    /\ saw_recovery \in BOOLEAN
    /\ saw_stale_result \in BOOLEAN
    /\ saw_closed \in BOOLEAN
    /\ saw_faulted \in BOOLEAN
    /\ cache_id <= durable_id
    /\ slot_runtime \in RuntimeIds \cup {NoRuntime}
    /\ old_handle_runtime \in RuntimeIds
    /\ live_runtimes \subseteq RuntimeIds
    /\ slot_runtime # NoRuntime => slot_runtime \in live_runtimes
    /\ old_handle_runtime \in live_runtimes
    /\ runtime_identity \in [RuntimeIds -> RuntimeIdentityValues]
    /\ identity_ledger \in [RuntimeIds -> RuntimeIdentityValues]
    /\ runtime_cache_marker \in [RuntimeIds -> 0..1]
    /\ runtime_durable_marker \in [RuntimeIds -> 0..1]
    /\ catalog_life \in 0..2
    /\ observed_lives \subseteq Lives
    /\ accepted_generations \subseteq {1, 3}
    /\ arm_phase \in ArmPhases
    /\ pending_invalidation \in RuntimeIds \cup {NoRuntime}
    /\ old_action_target \in RuntimeIds \cup {NoRuntime}
    /\ old_action_kind \in OldOperations \cup {"none"}
    /\ bad_late_detach \in BOOLEAN
    /\ bad_missing_allocation \in BOOLEAN
    /\ saw_rebirth_old_action \in BOOLEAN
    /\ saw_self_remount \in BOOLEAN
    /\ saw_late_invalidation_preserved \in BOOLEAN
    /\ saw_missing_confirmation \in BOOLEAN

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

NoOldHandleRetarget ==
    old_action_target \in {NoRuntime, old_handle_runtime}

ExactPredecessorInvalidationPreservesSuccessor == ~bad_late_detach

PublishedRuntimeHasAcceptedIdentity ==
    /\ (slot_runtime # NoRuntime => arm_phase = "Armed")
    /\ \A r \in live_runtimes :
        /\ runtime_identity[r].life \in observed_lives
        /\ runtime_identity[r].admitted \in accepted_generations

RuntimeIdentityImmutable ==
    /\ old_handle_runtime = "r1"
    /\ runtime_identity["r1"] = [life |-> 1, admitted |-> 1]
    /\ \A r \in live_runtimes :
        /\ runtime_identity[r] = identity_ledger[r]
        /\ runtime_identity[r] # NoIdentity

MissingNameConfirmationAllocatesNothing == ~bad_missing_allocation

W_Commit == ~saw_commit
W_Unresolved == ~saw_unresolved
W_RetryCreated == ~saw_retry_created
W_DurableAdoption == ~saw_durable_adoption
W_Recovery == ~saw_recovery
W_StaleResult == ~saw_stale_result
W_Closed == ~saw_closed
W_Faulted == ~saw_faulted
W_RebirthOldAction == ~saw_rebirth_old_action
W_SelfRemount == ~saw_self_remount
W_LateInvalidationPreserved == ~saw_late_invalidation_preserved
W_MissingConfirmation == ~saw_missing_confirmation

=============================================================================
