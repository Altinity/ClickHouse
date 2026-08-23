---
description: 'Design for deadline-bounded, ambiguity-aware CAS mount-lease renewal retries and bounded recovery observability'
sidebar_label: 'CAS mount renewal retries'
sidebar_position: 4
slug: /superpowers/specs/cas-mount-renewal-retry-design
title: 'CAS mount renewal retry design'
doc_type: 'design'
---

# CAS mount renewal retry design {#cas-mount-renewal-retry-design}

**Status:** DRAFT for review, rev.2 (2026-08-23). This specification defines the minimum pre-release
fix for Altinity/ClickHouse issue `#2244`. It deliberately does not require compatibility with CAS
mount objects written by older, unreleased writer binaries.

## Decision {#decision}

The mount keeper retries a transient conditional renewal within the time still justified by its last
confirmed lease. Every retry belongs to one immutable logical renewal identified by a fresh
`write_attempt_id`; it repeats the exact same `(key, bytes, expected token)` and resolves ambiguity
with an exact `GET` before it may report success or send another request.

The implementation reuses `CasRequestController::putOverwriteControlled` with a mount-specific
operation context: an absolute BOOTTIME deadline, a cancellation gate, an interruptible wait, and a
diagnostic observer. The absolute deadline is the minimum of the existing `CasRequestBudget` and the
remaining confirmed-lease budget. There are no new user-facing settings. A retry, a resolving read,
and a response timestamp never extend the lease. Only a proven committed body advances the keeper's
token and deadline, and that deadline remains anchored at the body's original pre-I/O timestamp.

The periodic scheduler derives the next renewal from that same timestamp, not from response time. A
slowly resolved renewal therefore triggers its next beat immediately when the nominal cadence has
already elapsed.

This change also closes the snapshot-publication warning loop found in the issue RCA, and makes
renewal/remount recovery observable through bounded logs, `system.cas_log`, and `ProfileEvents`.
The remount protocol itself is unchanged. Per-step remount retries, persistent remount progress, and
the own-ambiguous-claim observation window are deferred explicitly to the backlog.

## Goals {#goals}

1. Keep a writable CAS mount live across intermittent object-store failures that fit inside the
   already confirmed lease.
2. Resolve a landed renewal whose response was lost without unnecessarily fencing the writer.
3. Preserve the existing fail-closed lease and single-writer guarantees.
4. Use the existing CAS request budget and expose no new configuration.
5. Make every recovered blip, exhausted renewal, lease trip, and remount attempt diagnosable without
   producing per-attempt warning storms.
6. Bound the snapshot-publication `NotReady` redispatch loop with its existing per-table backoff.
7. Prove the split-phase retry protocol with a focused TLA+ model before changing production C++.

## Non-goals {#non-goals}

- Per-step retry or progress preservation in `Pool::tryRemountOnce`.
- Changing `claimMountAwaitingExpiry`, its observation threshold, or its restart limit.
- Retrying owner claim, writer-epoch allocation, initial mount claim, keeper adoption, or farewell.
- Changing the durable owner/epoch semantics, mount key, object-store conditional dialect, or
  AWS/GCS authentication behavior.
- Extending a lease based on a retry, `GET`, response time, or local optimism.
- Allowing a writer to continue after an unresolved logical renewal.
- Compatibility with old CAS writer binaries or mount-lease bodies. CAS has not shipped.
- New retry, timeout, logging, or snapshot-backoff settings.

## Context and problem statement {#context-and-problem-statement}

`SingleWriterSlot::backgroundLoop` currently wakes every `mount_renew_period` (10 seconds by
default), calls `renewOnce`, and handles one conditional `PUT` whose request timeout is five seconds.
For a transient exception, `MountLeaseKeeper::shouldFenceOnTransientRenewFailure` retains the mount
while `now + lease_safety_margin < confirmed_deadline`; however, the loop then sleeps another full
renew period before trying again. The code therefore has retries in the broad sense, but no fast
in-period retry. A short outage can consume the useful part of a 30-second lease before the next beat.

The data plane has a controlled retry layer and was observed recovering with messages such as
`Attempt 2/501 succeeded`, while the mount renewal remained one physical attempt per cadence. Once
the keeper crosses its safety boundary it latches the local write fence, moves the pool from `Live`
to `TransientNotLive`, and starts the whole-chain remount loop. The field incident then spent roughly
15 minutes refusing all writes while repeated remount attempts traversed a long sequential chain.

A naive retry is incorrect. If the first conditional `PUT` lands but its response is lost, the
keeper retains the old token. Reissuing against that token produces `PreconditionFailed`. The current
`onRenewMismatch` deliberately treats an unfenced body under the same `(server_uuid, writer_epoch)`
as `same_epoch_state_uncertain`: it may be the keeper's own ambiguous renewal, but it may instead be
a pathological same-pair twin after epoch-state loss. Adopting by pair or sequence alone would weaken
that protection.

The same RCA found 125,952 snapshot-publication refusal warnings in about 16 minutes. Most snapshot
failure exits already call `advancePublishBackoff`, but the `lane_state != Ready` exit only logs and
returns `false`. `settleSnapshotPublish` immediately re-evaluates the still-over-threshold table,
admits another publish, and recreates the same refusal loop.

## Existing invariants retained {#existing-invariants-retained}

1. The last confirmed holder write is the only source of lease authority.
2. A local mutation is admitted only while the mount fence is held and its BOOTTIME deadline has not
   expired.
3. A confirmed token mismatch is never retried as though it were a transport failure.
4. A GC-fenced body, successor epoch, foreign UUID, vanished mount, or unexplained same-pair body
   fences the current runtime.
5. The durable expiry and local fence deadline are anchored before I/O; a slow acknowledgement cannot
   move either deadline forward.
6. One unresolved logical write never gives birth to a different body under the same expected token.
7. Teardown stops and joins renewal before running the terminal mount operation.
8. Remount allocates a fresh writer epoch and re-arms the fence only after keeper installation and ref
   runtime quiescence; this ordering is untouched.

## Durable attempt identity {#durable-attempt-identity}

`MountLease` gains a required `UInt128 write_attempt_id`. It identifies one holder-originated lease
body, not the writer incarnation:

- `(server_uuid, writer_epoch)` identifies the writer incarnation;
- `seq` orders its successful holder writes;
- `write_attempt_id` distinguishes one exact logical write from another writer that somehow acquired
  the same pair;
- `expires_at_ms` and `started_at_ms` retain their current timing meaning.

Every holder-originated body mints a fresh random ID: initial claim/refresh, keeper mint/adopt,
renewal, and clean farewell. All physical retries of one logical operation reuse its ID and exact
bytes. A GC fence copies the observed ID while setting `gc_fenced`; a reclaim or successor claim
mints a new ID with its new body.

The decoder requires the field and rejects an old body that lacks it. This is an intentional
pre-release format break. The implementation increments `G_BUILD` to generation 10, introduces
`kMountWriteAttemptIdGeneration = 10`, adds breaking generation-10 change points for both
`FormatId::MountLease` and `FormatId::PoolMeta`, and makes `decodePoolMeta` require that pool-level
reader floor. The `MountLease` change point records the object-format history; the pool-level floor is
what actually makes generation-9 pools recreate-only before any old mount body is interpreted. No
dual decoder or migration is added. Unknown fields remain tolerated as before. The canonical encoder
uses the full field name `write_attempt_id`; this avoids introducing another abbreviated key
immediately before the separate full-word wire-key cleanup already tracked in the backlog.

Random 128-bit IDs are treated with the same uniqueness assumption already used for server and
operation identities in the CAS protocol. The focused TLA+ model represents IDs as distinct symbolic
values and includes an explicit sabotage that ignores them.

## Renewal operation {#renewal-operation}

### Immutable logical request {#immutable-logical-request}

At the start of one `renewOnce`, while preserving the existing `state_mutex` serialization, the
keeper fixes:

- the currently held `expected_token`;
- `seq + 1`;
- the pre-I/O wall and BOOTTIME timestamps;
- the current `min_active` value;
- one fresh `write_attempt_id`;
- the resulting canonical lease bytes.

No field changes between physical attempts. In particular, a retry does not refresh
`expires_at_ms`. Refreshing it would manufacture lease time from an unconfirmed attempt and make a
late straggler carry different authority from the request being resolved.

### Deadline derivation {#deadline-derivation}

The keeper maintains `confirmed_deadline_boot_ms`, anchored from the BOOTTIME timestamp captured
before the last proven holder write. Wall clock remains only in the durable lease body's
`started_at_ms`/`expires_at_ms`; it does not decide local retry admission or cadence.

At the start of the logical operation the keeper computes, with checked/saturating arithmetic:

```text
lease_retry_deadline_boot_ms = confirmed_deadline_boot_ms - lease_safety_margin_ms
request_budget_deadline_boot_ms = attempt_start_boot_ms + cas_request_budget.operation_deadline_ms
absolute_deadline_boot_ms = min(lease_retry_deadline_boot_ms, request_budget_deadline_boot_ms)
```

If the boundary is already reached, no request is sent. The controller retains the existing
`attempt_timeout_ms`, `max_attempts`, `retry_initial_backoff_ms`, and `retry_max_backoff_ms`, but
receives the absolute deadline rather than a duration that it would re-anchor from a later clock read.
Preemption between calculation and controller entry can therefore only consume budget, never move
the boundary forward.

The controller uses the keeper's BOOTTIME callback. Before every backend request -- both a conditional
`PUT` and its resolving `GET` -- it requires `now + attempt_timeout_ms <= absolute_deadline_boot_ms`
and a true cancellation gate. It applies the same checks before and after every backoff. The local
BOOTTIME mount fence and renewal controller consequently share one suspend-safe proof window. Wall
clock jumps may change the timestamp encoded in a future body, as today, but cannot enlarge retry
authority or delay the next beat.

### Result classification {#result-classification}

`CasRequestController::putOverwriteControlled` applies its existing resolve-before-reissue rules,
with the operation context adding a gate before every request:

1. `PUT` returns `Done`: the attempt committed and supplies the new token.
2. `PUT` throws transiently or returns `PreconditionFailed`: issue one exact `GET`.
3. The current token still equals `expected_token`: this attempt has not replaced the observed
   incarnation; after bounded backoff, reissue the same request.
4. Current bytes equal the attempted bytes, including `write_attempt_id`: this logical attempt
   committed; adopt the observed token.
5. A different token and different bytes: return `Conflict` and run the existing body-aware mount
   classification.
6. An absent/unreadable result remains ambiguous and may reissue only while all budget gates allow it.

The final controller fence check remains mandatory after both direct and read-resolved success. A
result learned after the last confirmed lease's safety boundary is `Unresolved`, even when the exact
body is visible. This is conservative: the durable body may keep the slot occupied, but the local
writer does not reclaim authority after its proof window closed.

`Committed` records the new `seq` and token and calls the existing commit hooks. `Conflict` invokes
`onRenewMismatch`, which re-reads and classifies `gc_fenced`, superseded, foreign, vanished, or
same-pair uncertainty exactly as today. `Unresolved` is an explicit terminal result, not an ordinary
exception for `backgroundLoop` to reconsider on the next cadence.

`SingleWriterSlot::renewOnce` owns terminalization for every admitted logical renewal, regardless of
its caller. On `Unresolved`, `Conflict` classification, or a thrown deterministic/non-retryable
backend error it records the terminal outcome, releases `state_mutex`, invokes an idempotent
`notifyRenewFailedOnce`, and then returns the exception to its caller. Calling the lifecycle callback
outside `state_mutex` preserves the existing lock order into pool/remount code. Programming
precondition guards that reject a call before a logical renewal is admitted remain plain exceptions:
they send no request and do not manufacture a lease-loss notification.

`notifyRenewFailedOnce` latches the keeper and schedules remount exactly once even for direct calls.
The background loop only logs the already terminal outcome and exits; it never converts it into a
later new-body attempt. No fallback converts an error into success.

### Scheduling after success {#scheduling-after-success}

The background loop currently waits one full `period` after the response. That is safe for a fast
response but wrong after a long retry: it shifts the next request later by the entire recovery time.

The mount keeper instead computes the next due time as:

```text
last_committed_attempt_start_boot_ms + mount_renew_period
```

If it is in the future, the loop waits only until that point. If it is now or in the past, the next
logical renewal begins immediately with a new `seq`, timestamp, and `write_attempt_id`. A successful
late resolution still anchors `confirmed_deadline_boot_ms` and the BOOTTIME fence at its original
attempt start, never its response time. Forward or backward wall-clock changes have no effect on this
calculation.

### Exhaustion and late stragglers {#exhaustion-and-late-stragglers}

When the controller returns `Unresolved`, at least one exact request may still land later. The keeper
must not create a different renewal body against that request's expected token, nor continue waiting
for a later scheduled beat. It fences immediately and hands recovery to the existing fresh-epoch
remount path.

A late straggler can therefore extend the durable old-epoch body after the local writer has fenced.
This may delay the existing observation-based reclaim, but cannot authorize another local mutation.
The deferred remount design may reduce that availability cost; this specification does not weaken
the safety direction to avoid it.

## Class and API changes {#class-and-api-changes}

### `SingleWriterSlot` {#singlewriterslot}

The base keeps ownership of `seq`, `last_token`, `dead`, `state_mutex`, the renewal thread, and the
start/stop/terminal lifecycle. It gains an explicit write result and two protected policy hooks:

```cpp
struct RenewalWriteResult
{
    enum class Kind { Committed, Conflict, Unresolved };
    Kind kind;
    Token token;
    CasOverwriteDiagnostics diagnostics;
};

virtual RenewalWriteResult writeRenewal(
    const String & body,
    const Token & expected_token);

virtual std::chrono::milliseconds nextRenewalDelay(
    std::chrono::milliseconds period) const;
```

The default `writeRenewal` performs the current raw one-shot `backend->putOverwrite` and maps its
returned outcome. The default `nextRenewalDelay` returns `period`. `renewOnce` creates one
payload/body, calls the write hook, records a committed token, and invokes `onRenewMismatch` only for
`Conflict`.

After its existing precondition guards admit a logical renewal, `renewOnce` translates every
terminal write outcome into a captured exception, releases `state_mutex`, and calls
`notifyRenewFailedOnce` before rethrowing. `notifyRenewFailedOnce` uses a base-owned one-way latch, so
a direct `renewWatermarkOnce`/`keeperRenewOnce`, the background loop, and teardown cannot double-fire
`onRenewFailed`, `CASMountLeaseLost`, or remount scheduling. After the latch is set, another direct or
background `renewOnce` refuses before encoding or sending a body. It may still evaluate the existing
off-lock payload callbacks before acquiring `state_mutex`; preserving that ordering avoids inverting
the pool/state locks. A precondition exception raised before admission does not call the notification
hook.

The old `last_renew_failure_was_confirmed_mismatch` and
`shouldFenceOnTransientRenewFailure` cadence-level mechanism is removed. Its availability role is
superseded by the controlled retry loop; retaining it would allow an exhausted logical request to be
replaced by a new body.

The opaque `RenewPayload` gains the holder write ID so `prepareRenew` can mint it once and
`encodeBody` can encode it without mutable side channels.

The background wait becomes interruptible for both the nominal cadence and retry backoff. A protected
base helper exposes an interruptible wait and cancellation predicate backed by the existing
`wakeup`/`stop_requested` state; no new public cancellation API is exposed. A stop wakes the retry
sleep. The operation context rechecks cancellation before a resolving `GET`, after the wait, and
before another `PUT`, so shutdown joins at most the one backend request already in flight when stop
was published.

### `MountLeaseKeeper` {#mountleasekeeper}

The keeper receives the existing `CasRequestBudget` from `CasMountRuntime`/`Pool` in addition to the
existing TTL and safety margin. It overrides:

- `writeRenewal` to create the narrowed controller and translate `Committed`, `Conflict`, and
  `Unresolved` into the base contract;
- `nextRenewalDelay` to schedule from the last committed attempt's anchor;
- the existing mismatch/failure hooks, whose body classification remains unchanged.

The controller is a lightweight per-logical-operation value. Constructing it does not construct an
S3 client, authentication provider, connection pool, or retry strategy; it shares the existing
`BackendPtr`.

### `CasRequestController` operation context and diagnostics {#casrequestcontroller-diagnostics}

The controlled-overwrite API gains an optional `CasOverwriteOperationContext` used by mount renewal:

```cpp
struct CasOverwriteOperationContext
{
    uint64_t absolute_deadline_ms;
    std::function<bool()> may_continue;
    std::function<bool(uint64_t)> wait_before_retry;
    std::function<void(const CasOverwriteProgress &)> observe;
};
```

The deadline uses the same clock as the controller's injected `now_ms`. `may_continue` is checked
before every `PUT`, resolving `GET`, wait, and committed return. `wait_before_retry` returns `false`
when shutdown interrupts it. `observe` reports request start, first transition to ambiguity, retry,
read-resolved success, and terminal result; it does not make protocol decisions.

Existing callers keep the current overload and behavior. The mount overload/context adds the
absolute deadline and additional cancellation gates without changing their relative-deadline
operations.

`CasOverwriteResult` is extended with diagnostic-only fields needed by final mount observability:

- physical attempts sent;
- whether success was resolved by `GET`;
- the final unresolved reason.

No caller branches on diagnostic fields for correctness. Mount counters and the one-time `retrying`
event are emitted by the progress observer at the physical transition, so an unfinished call is not
silently missing its attempts and the event is not reconstructed after the fact.

### `CasMountRuntime` and `Pool` {#casmountruntime-and-pool}

`CasMountRuntime::installKeeper` passes the already validated `cas_request_budget` to the keeper. No
new setting is loaded or merged.

The remount callback remains whole-chain and returns `bool`. `Pool::tryRemountOnce` adds a local step
label and monotonically increasing attempt number solely for logs/events/counters; the sequence of
backend operations, retries, locks, epoch allocation, keeper replacement, quiescence, and fence
re-arm is byte-for-byte behaviorally unchanged.

## Observability {#observability}

### ProfileEvents {#profileevents}

Add:

- `CASMountRenewalAttempts`: every physical conditional renewal `PUT` sent;
- `CASMountRenewalRetries`: every physical `PUT` after the first within one logical renewal;
- `CASMountRenewalResolved`: committed outcome proved by exact `GET`;
- `CASMountRenewalRecovered`: logical renewal that committed after a retry or resolving read;
- `CASMountRenewalDeadlineExceeded`: logical renewal whose confirmed-lease budget ended unresolved;
- `CASRemountAttempts`: every invocation of the current whole-chain remount attempt;
- `CASRemountSucceeded`: attempts that restored `Live` under a fresh epoch;
- `CASRemountFailed`: attempts that returned without restoring `Live`, including a caught step error.

`CASMountLeaseLost` is normalized to increment exactly once when `onRenewFailed` actually deposits the
keeper. Classification branches must not also increment it. Its description is updated to include
deadline exhaustion. Existing specialized counters such as `CASRemountHeldTransient` remain subsets
with their current meaning.

### `system.cas_log` {#system-cas-log}

The existing `watermark_renew` event type records only nontrivial logical renewals:

- `retrying`: the first physical attempt became ambiguous;
- `recovered`: a retry/read resolution committed before the deadline;
- `failed`: the logical renewal ended without retained authority.

Structured detail includes `server_root_id`, `writer_epoch`, `seq`, a shortened attempt ID, attempts
sent, elapsed milliseconds, remaining confirmed budget, and final classification. An ordinary
first-attempt success emits no row.

`mount_remount` events add the whole-chain attempt number and current step. Success retains
`outcome=ok`; failures retain `outcome=failed` but name the step that returned or threw. This is
diagnostic only and must not add an extra backend read.

### Text logging {#text-logging}

One logical renewal may emit at default levels at most:

- one `WARNING` when it enters retry;
- one `INFO` when it recovers; or
- one final `WARNING` when it fences.

Individual physical retries are `DEBUG`. The final message carries attempts, elapsed time, last
classification, and the confirmed deadline. This preserves evidence without replacing the original
failure with a retry log storm.

Every whole-chain remount attempt emits one final default-level result naming its attempt number and
step. Existing step-local messages remain, but instrumentation must not log every polling iteration
of `claimMountAwaitingExpiry` at warning level.

## Snapshot refusal backoff {#snapshot-refusal-backoff}

When `tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntime` observes
`lane_state != RefLaneState::Ready`, it arms `advancePublishBackoff` under `state_mutex` before
returning `false`. The existing `settleSnapshotPublish` re-evaluation then sees
`publish_backoff_until_ms` and declines immediate redispatch.

This uses the existing per-table sequence of 200 ms, 400 ms, and so on up to 30 seconds, increments
`CASRefSnapshotPublishBackoff`, and resets only after a durable snapshot publication. Direct test
calls remain one attempt per call; the production background path becomes rate limited. No snapshot
correctness state, threshold, or publication ordering changes.

The refusal warning is therefore emitted only when an actual backoff-spaced attempt runs. It is not
suppressed forever: a later trigger after the deadline retries, and success clears the cooldown.

## TLA+ gate {#tla-gate}

Production C++ work may not begin until a focused `CaMountRenewRetryCore` model passes. The existing
`CaCasMountCore` intentionally treats a resolved renewal as one atomic action and would require a
large unrelated state-space expansion to model transport attempts directly.

The focused model represents:

- confirmed token/body/deadline and local fencing authority;
- one logical request with a symbolic unique attempt ID;
- sent, landed, response-lost, resolved, committed, conflict, and unresolved states;
- a same-pair twin, GC fence, successor epoch, and foreign holder;
- backoff/time advance, cancellation, budget exhaustion, and next-beat scheduling.

Required invariants include:

1. only the exact current logical attempt may be adopted after ambiguity;
2. no twin, fenced body, successor, or foreign holder is adopted;
3. retry/read/response events never extend the confirmed deadline;
4. no request starts after the safe operation budget;
5. no mutation authority is re-armed from an unresolved or post-deadline result;
6. one unresolved logical request is never followed by a different body under the same expected
   token;
7. every acknowledged renewal names a durable body and its current token.

Required honest witnesses reach:

- a non-landed transient followed by direct retry success;
- a landed/lost response followed by exact-ID read adoption;
- unresolved exhaustion followed by fencing;
- slow success followed by an immediate catch-up renewal.

Each load-bearing rule receives a sabotage configuration, including adoption by pair while ignoring
the ID, response-time deadline refresh, new-body retry after ambiguity, acceptance after fence or
supersession, and response-relative cadence. Witnesses must be non-vacuous and sabotage runs must fail
the intended invariant rather than a shallower unrelated one.

The focused model owns the split state that the existing model deliberately abstracts away: a body
may already be durable while the local keeper has not yet proved or committed it. Observer actions
for GC fencing, twin replacement, reclaim, successor epochs, and foreign holders may run in that
window. The model must therefore explore those interleavings directly; a durable landed state is not
declared to be a stuttering step merely because local confirmation is pending.

There is no claimed formal refinement from the focused split-phase model to the atomic
`CaCasMountCore.Renew` action. The documented abstraction boundary is narrower: when the focused
operation reaches local `Committed`, its durable body/token and local authority satisfy the atomic
model's postconditions; terminal focused outcomes leave authority fenced and enter the already
modelled remount boundary. This correspondence is an audit aid, not a machine-checked refinement.

After the focused gate, rerun every committed `CaCasMountCore` green, red, and witness configuration
as a regression gate. Update both result documents with commands, worker counts, state/depth totals,
invariant verdicts, and the explicit abstraction boundary above. Do not claim that landed but
locally unconfirmed focused states refine to atomic-model stuttering.

## Runtime verification {#runtime-verification}

### Deterministic unit tests {#deterministic-unit-tests}

Fault-injecting backends and clocks must prove:

1. transient failure before landing, then a direct retry `Done`;
2. body lands but response is lost, exact `GET` adopts its token;
3. first resolve sees the expected predecessor token, then a late first attempt lands before the
   retry, whose conflict resolves to the exact body;
4. same `(server_uuid, writer_epoch, seq)` with another `write_attempt_id` fences;
5. an ambiguous body fenced by GC before resolution fences;
6. successor epoch, foreign UUID, and vanished body retain their existing fail-closed outcomes;
7. failing resolve reads exhaust the narrowed budget without an attempt after the boundary;
8. a valid budget with `max_attempts = 1` that ends unresolved terminalizes the keeper immediately;
   the background loop never wakes later to mint a new body for the old expected token;
9. a deterministic/non-retryable failure through the real background path terminalizes immediately
   rather than waiting one normal cadence;
10. a slowly resolved success refreshes from attempt start and schedules an immediate catch-up beat;
11. `stopBackground` published while a `PUT` is in flight permits that bounded call to finish but
    prevents the resolving `GET`; interruption during backoff sends nothing afterward;
12. preemption between deadline calculation and controller entry consumes the absolute budget and
    cannot re-anchor it later;
13. forward and backward wall-clock steps do not change BOOTTIME request admission, local fencing,
    or cadence; BOOTTIME advancement does;
14. direct `CasMountRuntime::renewWatermarkOnce` and `Pool::keeperRenewOnce` use the same controlled
    operation and deposit terminal failure/remount exactly once;
15. all holder-originated encoders mint a fresh `write_attempt_id`, retries preserve it, GC fencing
    preserves the observed ID, and reclaim/successor bodies replace it;
16. a missing `write_attempt_id` is rejected and a generation-9 pool is rejected at the
    generation-10 `PoolMeta` reader floor before any old mount body is interpreted;
17. retry metrics/events count once at their documented granularity and `CASMountLeaseLost` is not
    doubled;
18. `NotReady` snapshot publication arms exponential backoff, suppresses immediate settlement
    redispatch, and resets after durable success;
19. remount attempts report attempt number and exact failed step without changing their operation
    sequence.

Tests use injected clocks and interruptible wait seams. They must not use real sleeps to prove a
race or deadline.

### Integration and chaos tests {#integration-and-chaos-tests}

The object-storage integration lane injects a short conditional mount-`PUT` disruption and asserts:

- more than one physical renewal attempt occurs;
- the renewal recovers inside the confirmed deadline;
- the pool remains `Live`;
- mutations continue;
- no remount is scheduled;
- retry/recovery counters and the aggregate log sequence are present.

A separate deterministic backend test, rather than a fake HTTP response alone, proves the
landed-but-response-lost adoption because it must control both durable state and returned outcome.

The CA chaos soak treats any unexpected `mount lease not held`, `TransientNotLive`, or remount after
an injected in-budget blip as a scenario failure. The harness must not classify that state as an
ordinary retryable `NETWORK_ERROR` and still report the scenario recovered. A release validation run
must include repeated sub-TTL disruptions and verify zero unintended lease trips.

## Documentation and bookkeeping {#documentation-and-bookkeeping}

Implementation updates:

- `docs/en/antalya/cas/architecture/mounts-and-leases.md`: retry/resolve protocol, attempt identity,
  deadline anchoring, and scheduling;
- CAS operations/debugging documentation: new counters, aggregate logs, and interpretation;
- format documentation/golden tests for the new required lease field;
- the pool format generation and both `FormatId::PoolMeta` and `FormatId::MountLease` change-point
  tables;
- `docs/superpowers/cas/final-checks-todo.md`: mark only the minimum `#2244` cut complete after all
  gates pass;
- `docs/superpowers/cas/BACKLOG.md`: retain the remount follow-up under its own anchor;
- `CaMountRenewRetryCore_RESULTS.md` and `CaCasMountCore_RESULTS.md`;
- stale comments in `SingleWriterSlot`, `MountLeaseKeeper`, `CasRequestBudget`, snapshot publisher,
  remount loop, tests, and soak classification.

Searches for claims such as “no retry”, “one attempt per cadence”, “never re-armed”, and immediate
snapshot redispatch are part of the final documentation audit.

## Rejected alternatives {#rejected-alternatives}

### Transparent SDK retries {#transparent-sdk-retries}

The SDK cannot decide whether an ambiguous conditional `PUT` landed, cannot compare the durable body
to this logical attempt, and cannot enforce the keeper's confirmed-lease deadline. Enabling it would
hide the very outcome the lease protocol must classify.

### Retry `renewOnce` on a shorter timer {#retry-renewonce-on-a-shorter-timer}

Each call would generate a new timestamp/body and retain a stale expected token. If the previous
request landed ambiguously, the next call would either conflict or create multiple distinguishable
logical writes while an older one could still arrive. Retries must stay inside one immutable logical
operation.

### Adopt by same pair or sequence {#adopt-by-same-pair-or-sequence}

This collapses an ordinary lost response and a pathological same-pair twin. The current code
deliberately fences that uncertainty. `write_attempt_id` makes own-write adoption explicit without
weakening the epoch-loss defense.

### A renewal-specific retry implementation {#renewal-specific-retry-implementation}

Duplicating resolve-before-reissue, attempt classification, deadline gates, and backoff in
`MountLeaseKeeper` would create two subtly different conditional-write controllers. The existing
controlled mutable overwrite already has the required semantics; this design narrows its budget and
adds lease policy around it.

### New `mount_renew_retry_*` settings {#new-mount-renew-retry-settings}

They would duplicate `CasRequestBudget` and admit invalid combinations with TTL and safety margin.
The confirmed lease already supplies the operation-specific upper bound.

### Full remount state machine in this fix {#full-remount-state-machine-in-this-fix}

Per-step progress changes the identity/epoch/claim/install state machine and needs its own TLA+ model
and fault matrix. It is valuable but not required to prevent the observed initial lease trip. Mixing
it into the minimum fix enlarges the review surface around the most critical single-writer ordering.

## Deferred remount follow-up {#deferred-remount-follow-up}

The follow-up must design and verify:

1. explicit remount steps with per-step retry classification;
2. preservation or safe recomputation of successful owner/catalog/epoch work;
3. whether an own ambiguous claim resets, preserves, or terminates the token-stability observation
   window;
4. prevention of repeated epoch burning on whole-chain transport failures;
5. cancellation/teardown at every step boundary;
6. a focused TLA+ model and an explicitly documented abstraction boundary against
   `CaCasMountCore`;
7. deterministic faults before, during, and after every state-changing remount step.

Until that work lands, the current whole-chain retry remains fail closed. This specification improves
its diagnosis but makes no claim that its recovery latency is optimal.

## Acceptance criteria {#acceptance-criteria}

1. No new user-facing setting or non-CAS behavior change is introduced.
2. One logical renewal uses one immutable body and one `write_attempt_id` across every physical
   attempt.
3. A landed/lost-response renewal can be adopted only by exact attempt identity and bytes.
4. Same-pair twins, GC fences, successor epochs, foreign UUIDs, and vanished mounts remain fail closed.
5. Retry work uses one absolute BOOTTIME deadline, never crosses the last confirmed lease's safety
   boundary, and never extends that boundary without a proven commit.
6. Any remaining ambiguity or admitted deterministic terminal failure fences exactly once and
   schedules the existing remount path exactly once, including through either direct renewal API.
7. Slow successful resolution cannot shift the next cadence by response latency.
8. Shutdown interrupts backoff and waits only for an already in-flight bounded request.
9. Snapshot `NotReady` refusal is rate limited by the existing per-table backoff.
10. Renewal and remount outcomes are reconstructible from counters plus aggregate logs without
    per-attempt warning spam.
11. The focused split-phase TLA+ honest/sabotage/witness gate and the complete existing
    `CaCasMountCore` regression battery have recorded expected verdicts without a false atomic
    refinement claim.
12. Deterministic C++ tests, the object-storage integration fault, and the lease-sensitive chaos soak
    pass.
13. Mount format, architecture, debugging, model results, TODO, backlog, soak policy, and stale code
    comments are updated consistently.
14. Deferred per-step remount work is present in the backlog and is not accidentally implemented as
    an unreviewed side effect of this change.
