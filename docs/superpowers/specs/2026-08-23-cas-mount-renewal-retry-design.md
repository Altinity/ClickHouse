---
description: 'Design for deadline-bounded, ambiguity-aware CAS mount-lease renewal retries and bounded recovery observability'
sidebar_label: 'CAS mount renewal retries'
sidebar_position: 4
slug: /superpowers/specs/cas-mount-renewal-retry-design
title: 'CAS mount renewal retry design'
doc_type: 'design'
---

# CAS mount renewal retry design {#cas-mount-renewal-retry-design}

**Status:** DRAFT for review, rev.3 (2026-08-23). This specification defines the minimum pre-release
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

Ownership is simplified before retries are added. `SingleWriterSlot` is removed: it has exactly one
subclass and currently mixes durable slot protocol, a worker thread, callback-based lifecycle
effects, and subclass destruction ordering. `MountLeaseKeeper` becomes a synchronous state machine
for the durable mount object. The stable `CasMountRuntime` owns the renewal worker, wakeup,
cancellation, fence refresh, terminal failure handling, and remount scheduling. Renewal and remount
remain separate workers; combining them would change the remount protocol and is outside this fix.

This change also closes the snapshot-publication warning loop found in the issue RCA, and makes
renewal/remount recovery observable through bounded logs, `system.cas_log`, and `ProfileEvents`.
The durable remount protocol itself is unchanged. Per-step remount retries, persistent remount
progress, and the own-ambiguous-claim observation window are deferred explicitly to the backlog.

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
8. Reduce the renewal ownership surface rather than adding callbacks and latches to the existing
   single-subclass hierarchy.

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
- Combining renewal and remount into one maintenance state machine.

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
7. Teardown and remount stop and join the runtime-owned renewal worker before releasing or replacing
   the keeper.
8. Remount allocates a fresh writer epoch and re-arms the fence only after keeper installation and ref
   runtime quiescence. A parked renewal worker is released to run only after the fresh epoch, keeper,
   fence, and `Live` lifecycle are all published.
9. No keeper method calls back into its owner or can schedule destruction of the keeper executing it.

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

At the start of one synchronous `MountLeaseKeeper::renew`, the keeper fixes:

- the currently held `expected_token`;
- `seq + 1`;
- the pre-I/O wall and BOOTTIME timestamps;
- the current `min_active` value;
- one fresh `write_attempt_id`;
- the resulting canonical lease bytes.

No field changes between physical attempts. In particular, a retry does not refresh
`expires_at_ms`. Refreshing it would manufacture lease time from an unconfirmed attempt and make a
late straggler carry different authority from the request being resolved.

The runtime enforces a single renewal driver. Startup may call the internal redo before a worker
exists. Afterwards either the runtime worker drives renewal, or a deterministic direct test seam does
so while background renewal is disabled. Keeper replacement and release occur only after the worker
has joined. The keeper consequently needs no thread, callback, or general-purpose state mutex.

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

The controller uses the runtime's BOOTTIME callback. Before every backend request -- both a conditional
`PUT` and its resolving `GET` -- it requires `now + attempt_timeout_ms <= absolute_deadline_boot_ms`
and a true cancellation gate. It applies the same checks before and after every backoff. A relative
`condition_variable` wait may finish later than its BOOTTIME target after host suspend, because the
two clocks intentionally differ. That can delay detection/recovery by the unused part of the wait,
but the mandatory post-wait BOOTTIME gate sends no request and grants no authority after the absolute
deadline. This change claims suspend safety, not an immediate wakeup from suspend. Wall-clock jumps
may change the timestamp encoded in a future body, as today, but cannot enlarge retry authority or
delay cadence.

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

`MountLeaseKeeper::renew` converts these outcomes into a non-throwing admitted-operation result.
`Committed` records the new `seq`, token, confirmed BOOTTIME deadline, and cadence anchor before
returning. `Conflict` runs the existing body-aware classification for `gc_fenced`, superseded,
foreign, vanished, or same-pair uncertainty, but captures the classified exception into the returned
terminal result. `Unresolved` and a caught deterministic/non-retryable backend exception are also
terminal results. The keeper enters `RenewalTerminal` before returning any of them and records
`deposition_observed`; it cannot prepare another body or publish a clean farewell afterwards.

Programming preconditions detected before admitting a logical operation -- renew before start,
renew after terminal/release, and concurrent-driver misuse -- remain direct exceptions. They send no
request and do not create a new lease-loss transition.

The runtime consumes the result only after the keeper frame has returned. On success it refreshes the
local BOOTTIME fence from `attempt_start_boot_ms` for background and direct calls alike. On a terminal
result from the background worker or fully-open direct seam, it idempotently increments the keeper
generation's loss accounting, trips the fence, schedules the existing remount worker, and reports or
rethrows the captured exception. The startup-only TTL-consumed redo propagates a terminal result to
the existing open rollback without scheduling remount, because the pool has not published a live
fence/runtime yet. No keeper method calls into `CasMountRuntime`, and no error is converted to
success.

### Scheduling after success {#scheduling-after-success}

The background loop currently waits one full `period` after the response. That is safe for a fast
response but wrong after a long retry: it shifts the next request later by the entire recovery time.

The runtime renewal worker instead computes the next due time from the keeper's committed result:

```text
last_committed_attempt_start_boot_ms + mount_renew_period
```

If it is in the future, the loop waits only until that point. If it is now or in the past, the next
logical renewal begins immediately with a new `seq`, timestamp, and `write_attempt_id`. A successful
late resolution still anchors `confirmed_deadline_boot_ms` and the BOOTTIME fence at its original
attempt start, never its response time. Forward or backward wall-clock changes have no effect on this
calculation. A host suspend during the relative wait may delay the worker's wakeup, but the next
BOOTTIME admission check refuses stale work as described above.

### Exhaustion and late stragglers {#exhaustion-and-late-stragglers}

When the controller returns `Unresolved`, at least one exact request may still land later. The keeper
must not create a different renewal body against that request's expected token, nor continue waiting
for a later scheduled beat. It enters `RenewalTerminal`; after the synchronous call returns, the
runtime fences immediately and hands recovery to the existing fresh-epoch remount path.

A late straggler can therefore extend the durable old-epoch body after the local writer has fenced.
This may delay the existing observation-based reclaim, but cannot authorize another local mutation.
The deferred remount design may reduce that availability cost; this specification does not weaken
the safety direction to avoid it.

## Class and API changes {#class-and-api-changes}

### Remove `SingleWriterSlot` {#remove-singlewriterslot}

`SingleWriterSlot` is deleted rather than extended. `MountLeaseKeeper` is its only subclass, so the
base currently buys no polymorphic reuse while owning a worker thread, two mutexes, a condition
variable, virtual payload/commit/failure hooks, a mismatch side-channel flag, and a derived-destructor
join workaround. Those responsibilities are split by ownership instead:

- `MountLeaseKeeper` owns the durable mount-slot state machine;
- `CasMountRuntime` owns scheduling, cancellation, the local fence, lifecycle, and both worker
  handles;
- `Pool` retains only the existing high-level mount/remount orchestration.

This refactor is contained in fork-owned CAS files. `CasServerRoot.{h,cpp}` and
`CasMountRuntime.{h,cpp}` do not exist at the `altinity/antalya-26.6` merge base, so it adds no new
overlap with upstream ClickHouse code.

### `MountLeaseKeeper` {#mountleasekeeper}

The keeper is synchronous and has no thread, condition variable, owner callback, or runtime pointer.
Its explicit state is:

```cpp
enum class MountLeaseKeeperState : uint8_t
{
    New,
    Active,
    RenewalTerminal,
    Released,
};

enum class MountRenewOutcome : uint8_t
{
    Committed,
    Terminal,
};

struct MountRenewResult
{
    MountRenewOutcome outcome;
    uint64_t attempt_start_boot_ms;
    CasOverwriteDiagnostics diagnostics;
    std::exception_ptr failure; // set only for Terminal
};

struct MountRenewOperationEnvironment
{
    std::function<uint64_t()> boot_ms;
    std::function<CasOverwriteStopCause()> stop_cause;
    std::function<bool(uint64_t)> wait_before_retry;
    std::function<void(const CasOverwriteProgress &)> observe;
};
```

Its public protocol is:

```cpp
uint64_t start(); // committed attempt-start BOOTTIME anchor
MountRenewResult renew(
    const CasRequestBudget & budget,
    const MountRenewOperationEnvironment & environment);
void release();
```

`start` changes `New -> Active`; `renew` is admitted only in `Active`; a terminal admitted result
changes `Active -> RenewalTerminal`; and a clean `release` changes only `Active -> Released`.
Destruction performs no write. In particular, `RenewalTerminal` can never emit a clean farewell for
an unresolved request. Returning the start anchor removes the parallel timestamp capture currently
maintained by `Pool`; the fence is armed from the exact attempt the keeper proved.

`renew` receives the existing `CasRequestBudget` plus a small operation environment supplied by the
runtime: BOOTTIME, cancellation, interruptible backoff, and progress observation. It samples
`min_active` before constructing the body, preserving the existing lock order in which the pool's
build registry is never entered under keeper state. The single-driver contract makes a keeper mutex
unnecessary; misuse is rejected before I/O.

For an admitted operation, `renew` returns `MountRenewResult` rather than letting a backend or
body-classification exception escape across the runtime ownership boundary. A terminal result keeps
the original exception in `failure`; the synchronous/direct caller may rethrow it after applying its
lifecycle policy. Programming misuse before admission still throws directly. The controlled request
uses the existing `BackendPtr`; it does not construct an S3 client, authentication provider,
connection pool, or SDK retry strategy.

### `CasRequestController` operation context and diagnostics {#casrequestcontroller-diagnostics}

The controlled-overwrite API gains an optional `CasOverwriteOperationContext` used by mount renewal:

```cpp
enum class CasOverwriteDeadlineSource : uint8_t
{
    RequestBudget,
    ExternalLeaseSafety,
};

enum class CasOverwriteStopCause : uint8_t
{
    Continue,
    Cancelled,
    FenceOrLifecycleLost,
};

enum class CasOverwriteProgressKind : uint8_t
{
    PutStarted,
    BecameAmbiguous,
    ResolveStarted,
    RetryStarted,
    ResolvedByGet,
};

struct CasOverwriteProgress
{
    CasOverwriteProgressKind kind;
    uint32_t attempt_no;
};

struct CasOverwriteOperationContext
{
    uint64_t absolute_deadline_ms;
    CasOverwriteDeadlineSource deadline_source;
    std::function<CasOverwriteStopCause()> stop_cause;
    std::function<bool(uint64_t)> wait_before_retry;
    std::function<void(const CasOverwriteProgress &)> observe;
};

struct CasOverwriteDiagnostics
{
    uint32_t attempts_sent = 0;
    bool resolved_by_get = false;
    CasUnresolvedReason unresolved_reason = CasUnresolvedReason::NotUnresolved;
    CasOverwriteDeadlineSource deadline_source = CasOverwriteDeadlineSource::RequestBudget;
    CasOverwriteStopCause stop_cause = CasOverwriteStopCause::Continue;
};
```

The deadline uses the same clock as the controller's injected `now_ms`. `stop_cause` is checked before
every `PUT`, resolving `GET`, wait, and committed return; only `Continue` admits work.
`wait_before_retry` returns `false` when shutdown interrupts it. `observe` reports request start,
first transition to ambiguity, retry, and read-resolved success; the returned diagnostics report the
terminal result. The observer does not make protocol decisions. Every observer call is contained by
a non-throwing adapter: an allocation or event-sink exception is suppressed and reported at most once
at `DEBUG`, never converted into a lease failure.

Existing callers keep the current overload and behavior. The mount overload/context adds the
absolute deadline and additional cancellation gates without changing their relative-deadline
operations.

`CasOverwriteResult` gains `CasOverwriteDiagnostics`. Existing `CasUnresolvedReason` remains the
protocol-level reason; no duplicate reason enum is introduced. The deadline source says which input
won the `min` when the context was built. A tie is classified as `ExternalLeaseSafety`, because the
lease boundary is the correctness reason the mount must stop. `AttemptsExhausted`, cancellation/fence
loss, pre-attempt refusal, post-write refusal, request-budget deadline, and external lease-safety
deadline therefore have deterministic final log/metric mappings. Only the existing
`unresolvedProvesNothingWasSent` predicate may branch on `unresolved_reason`; all newly added fields
are diagnostic.

Mount counters and the one-time `retrying` event are emitted by the progress observer at the physical
transition, so an unfinished call is not silently missing its attempts and the event is not
reconstructed after the fact.

### `CasMountRuntime` and `Pool` {#casmountruntime-and-pool}

`CasMountRuntime` gains the renewal worker state currently owned by `SingleWriterSlot`: thread handle,
wakeup condition, stop flag, and committed cadence anchor. The condition-variable mutex guards only
the wait state and thread handle; it is never held across backend I/O, a keeper call, failure handling,
remount scheduling, or thread join. Controller lambdas may safely reference the runtime because the
runtime stops and joins its worker before destruction.

The ownership API is deliberately small:

```cpp
uint64_t renewKeeperForStartupOnce();
void renewWatermarkOnce();
void startRenewalWorker(std::chrono::milliseconds period);
void stopRenewalWorker();

void renewalLoop(std::chrono::milliseconds period);        // private
void consumeLiveRenewResult(MountRenewResult result);      // private
```

`consumeLiveRenewResult` is called only after `MountLeaseKeeper::renew` has returned and the caller
will not touch that keeper again. The startup method deliberately does not call it: on success it
returns the attempt's BOOTTIME anchor for the initial fence arm; on terminal result it rethrows into
open rollback without publishing loss/remount lifecycle.

The runtime enforces these entry points:

- the startup-only `renewKeeperForStartupOnce` runs before a worker exists and propagates failure to
  open rollback without scheduling remount;
- the renewal worker is the sole caller after background operation starts;
- `Pool::renewWatermarkOnce`/`CasMountRuntime::renewWatermarkOnce` is a deterministic test and
  maintenance seam that refuses when background renewal is configured or a worker exists.

One runtime helper consumes `MountRenewResult`. It refreshes the fence on every committed result,
including direct success. For a terminal result on a fully open runtime it uses an installed-keeper
generation to deposit `CASMountLeaseLost`, `tripMountLost`, and `scheduleRemount` exactly once, after
all keeper access has ended. Startup does not call this helper.

Keeper reset, replacement, and clean release require a stopped and joined renewal worker. Remount
keeps its existing separate worker and whole-chain callback. It joins renewal before replacing the
old keeper, installs and starts the new synchronous keeper, publishes the fresh epoch, quiesces ref
runtimes, and arms the fence in the existing load-bearing order. A newly constructed renewal worker
must additionally observe `Live` before sending; during remount it may be constructed while lifecycle
is still `TransientNotLive`, but it remains parked until `noteRemounted` publishes the completed
runtime and wakes it. Thread-construction failure therefore leaves the pool fenced and transient, and
the current remount attempt fails closed.

Initial open has no externally visible pool yet. It installs/starts the keeper, performs the optional
TTL-consumed redo, publishes epoch and fence, then starts the worker. A worker-construction failure
trips the fence and fails open; it never leaves a writable pool without renewal.

The remount callback still returns `bool`. `Pool::tryRemountOnce` adds a local step label and
monotonically increasing attempt number solely for logs/events/counters; its durable claim, epoch,
quiescence, and fence ordering are otherwise unchanged. Renewal and remount remain separate workers.

## Change footprint and risk {#change-footprint-and-risk}

The ownership refactor principally changes `CasServerRoot.{h,cpp}`, `CasMountRuntime.{h,cpp}`, a
small amount of `CasPool.cpp` ordering/wiring, and their CAS tests. Expect several hundred mechanically
changed lines but a smaller production result: deleting the one-subclass base, its virtual hooks,
thread state, derived-destructor workaround, and callbacks should remove roughly 50--150 net lines.

Initial implementation risk is medium because start/adopt/release and remount ordering are
load-bearing. It does not change durable owner/epoch allocation, reclaim qualification, ref-runtime
quiescence, or fence-generation admission. The risk is concentrated in worker lifecycle and is
covered by deterministic join/replacement/startup-failure tests. Long-term concurrency risk is lower:
one stable object owns both worker handles and every local lifecycle effect, while the replaceable
keeper cannot call back into its owner.

The upstream merge surface does not grow. The principal files are absent at merge base
`4b7cecaa3cf5fe67bea984e43c5a6b875da3e821`; generic `CasRequestController` additions are optional
fields/overloads that preserve existing callers. No non-CAS object-storage path changes.

## Observability {#observability}

### ProfileEvents {#profileevents}

Add:

- `CASMountRenewalAttempts`: every physical conditional renewal `PUT` sent;
- `CASMountRenewalRetries`: every physical `PUT` after the first within one logical renewal;
- `CASMountRenewalResolved`: committed outcome proved by exact `GET`;
- `CASMountRenewalRecovered`: logical renewal that committed after a retry or resolving read;
- `CASMountRenewalDeadlineExceeded`: logical renewal stopped by a deadline gate whose winning source
  was `ExternalLeaseSafety`;
- `CASRemountAttempts`: every invocation of the current whole-chain remount attempt;
- `CASRemountSucceeded`: attempts that restored `Live` under a fresh epoch;
- `CASRemountFailed`: attempts that returned without restoring `Live`, including a caught step error.

`CASMountLeaseLost` is normalized to increment exactly once when the runtime consumes the first
terminal result for an installed keeper generation. Classification branches must not also increment
it. Its description is updated to include deadline exhaustion. Existing specialized counters such as
`CASRemountHeldTransient` remain subsets with their current meaning.

### `system.cas_log` {#system-cas-log}

The existing `watermark_renew` event type records only nontrivial logical renewals:

- `retrying`: the first physical attempt became ambiguous;
- `recovered`: a retry/read resolution committed before the deadline;
- `failed`: the logical renewal ended without retained authority.

Structured detail includes `server_root_id`, `writer_epoch`, `seq`, a shortened attempt ID, attempts
sent, elapsed milliseconds, remaining confirmed budget, `unresolved_reason`, `deadline_source`,
`stop_cause`, and final classification. An ordinary first-attempt success emits no row.

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
- an outstanding network request that remains independently deliverable after local `Unresolved`;
- sent, landed, response-lost, resolved, committed, conflict, and unresolved local states;
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
7. every acknowledged renewal names a durable body and its current token;
8. delivery after local terminalization cannot restore old local authority or overwrite a fresh
   successor incarnation.

Required honest witnesses reach:

- a non-landed transient followed by direct retry success;
- a landed/lost response followed by exact-ID read adoption;
- unresolved exhaustion followed by fencing;
- local `Unresolved`, then late delivery before fresh-epoch reclaim, with the old runtime remaining
  fenced and recovery treating the extended body conservatively;
- local `Unresolved`, then fresh successor claim, then refusal of the old conditional straggler;
- slow success followed by an immediate catch-up renewal.

Each load-bearing rule receives a sabotage configuration, including adoption by pair while ignoring
the ID, response-time deadline refresh, new-body retry after ambiguity, acceptance after fence or
supersession, dropping the outstanding message at local terminalization, allowing late delivery to
re-arm authority, and response-relative cadence. The late-delivery witness makes message retention
non-vacuous; the corresponding sabotage must fail the intended invariant or witness rather than a
shallower unrelated one.

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
   the runtime worker never wakes later to mint a new body for the old expected token;
9. a deterministic/non-retryable failure through the real background path terminalizes immediately
   rather than waiting one normal cadence;
10. a slowly resolved success refreshes from attempt start and schedules an immediate catch-up beat;
11. `stopRenewalWorker` published while a `PUT` is in flight permits that bounded call to finish but
    prevents the resolving `GET`; interruption during backoff sends nothing afterward;
12. preemption between deadline calculation and controller entry consumes the absolute budget and
    cannot re-anchor it later;
13. forward and backward wall-clock steps do not change BOOTTIME request admission, local fencing,
    or cadence; BOOTTIME advancement does, including an overshoot representing host suspend;
14. `Pool::renewWatermarkOnce` through `CasMountRuntime::renewWatermarkOnce` refreshes the local fence
    on success and deposits terminal failure/remount exactly once; it refuses while a worker exists;
15. the startup-only `renewKeeperForStartupOnce` uses the same controlled request but never schedules
    remount; failure propagates into open rollback before a live fence/runtime is published;
16. a request remains deliverable after local `Unresolved`: landing before fresh-epoch reclaim leaves
    the old runtime fenced, while landing after successor claim is conditionally refused;
17. all holder-originated encoders mint a fresh `write_attempt_id`, retries preserve it, GC fencing
    preserves the observed ID, and reclaim/successor bodies replace it;
18. a missing `write_attempt_id` is rejected and a generation-9 pool is rejected at the
    generation-10 `PoolMeta` reader floor before any old mount body is interpreted;
19. `MountLeaseKeeper` admits only `New -> Active -> Released` or
    `New -> Active -> RenewalTerminal`; terminal state forbids another body and clean farewell;
20. keeper reset, replacement, and release occur only after the runtime renewal worker joins, and
    direct renewal cannot race worker ownership;
21. worker-construction failure during initial open or remount leaves the pool fail closed; a remount
    worker sends nothing until the fresh runtime is `Live`;
22. retry metrics/events count once at their documented granularity and `CASMountLeaseLost` is not
    doubled;
23. every deadline/attempt/cancellation/fence terminal shape maps to the documented diagnostics, and
    an observer exception cannot change the renewal result;
24. `NotReady` snapshot publication arms exponential backoff, suppresses immediate settlement
    redispatch, and resets after durable success;
25. remount attempts report attempt number and exact failed step without changing their operation
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
- removal of stale `SingleWriterSlot` prose and updates to comments in `MountLeaseKeeper`,
  `CasMountRuntime`, `CasRequestBudget`, snapshot publisher, remount loop, tests, and soak
  classification.

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

### Extend `SingleWriterSlot` with more hooks {#extend-singlewriterslot-with-more-hooks}

Adding `writeRenewal`, cadence, cancellation, terminalization, and notification hooks preserves a
nominal abstraction that has one subclass. It also leaves a derived object owning a thread that can
schedule replacement of its own `unique_ptr`, forcing separate rules for background, fully-open
direct, and startup-direct calls. The stable runtime already owns every lifecycle effect, so retaining
the hierarchy makes ownership harder to prove without providing reuse.

### Keep callback ownership with `shared_ptr` {#keep-callback-ownership-with-sharedptr}

Extending keeper lifetime through callbacks can prevent immediate destruction, but it does not make
startup remount valid, define which caller publishes fence success/failure, or remove callback/thread
cycles. It turns an explicit join-before-replace invariant into distributed lifetime retention.

### One combined renewal/remount worker {#one-combined-renewal-remount-worker}

A single maintenance state machine could eventually remove another thread, but it changes the
remount protocol. External interference may request remount while renewal I/O is in flight; long claim
observation and whole-chain backoff would share scheduling with lease cadence; and old-renewal
completion would need generation-qualified arbitration against a fresh claim. That belongs with the
deferred remount redesign and its own focused model. This fix retains two workers under one stable
owner.

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
5. Retry work uses one absolute BOOTTIME deadline. No backend request starts unless its configured
   timeout fits before the last confirmed lease's safety boundary, no result is accepted after that
   boundary, and the boundary is never extended without a proven commit.
6. Any remaining ambiguity or admitted deterministic terminal failure in a fully open runtime fences
   exactly once and schedules the existing remount path exactly once. The startup-only redo instead
   fails open before publication and never schedules remount.
7. Slow successful resolution cannot shift the next cadence by response latency.
8. `MountLeaseKeeper` is synchronous, has explicit `New`/`Active`/`RenewalTerminal`/`Released` state,
   and has no worker or owner callback; `SingleWriterSlot` is removed.
9. `CasMountRuntime` is the sole renewal-worker owner. Direct renewal cannot coexist with that worker,
   and reset/replacement/release require it to be joined.
10. Shutdown interrupts backoff and waits only for an already in-flight bounded request. A host
    suspend may delay a relative wait, but its post-wait BOOTTIME check sends no stale request.
11. Snapshot `NotReady` refusal is rate limited by the existing per-table backoff.
12. Renewal and remount outcomes are reconstructible from counters plus aggregate logs without
    per-attempt warning spam.
13. The focused split-phase TLA+ honest/sabotage/witness gate, including post-terminal late delivery,
    and the complete existing
    `CaCasMountCore` regression battery have recorded expected verdicts without a false atomic
    refinement claim.
14. Deterministic C++ tests, the object-storage integration fault, and the lease-sensitive chaos soak
    pass.
15. Mount format, architecture, debugging, model results, TODO, backlog, soak policy, and stale code
    comments are updated consistently.
16. Deferred per-step remount work is present in the backlog and is not accidentally implemented as
    an unreviewed side effect of this change.
