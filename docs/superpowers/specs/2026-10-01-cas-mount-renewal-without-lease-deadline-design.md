---
description: 'Design for CAS-298, CAS-292 and CAS-342: the mount lease is renewed by one loop that keeps retrying until the store gives a definitive answer, the lease TTL decides when writes stop, GC counts a token sighting from after its read, the renewal thread is not failed by the memory tracker, and the code that existed only to keep renewals inside a lease budget is removed.'
sidebar_label: 'Mount renewal without a lease deadline'
sidebar_position: 103
slug: /superpowers/specs/cas-mount-renewal-without-lease-deadline
title: 'CAS mount renewal without a lease deadline'
doc_type: 'guide'
---

# CAS mount renewal without a lease deadline {#cas-mount-renewal-without-lease-deadline}

One loop renews the mount lease and keeps trying until the store answers. The lease TTL, not the
renewal's retry budget, decides when writes stop. The code that existed to keep a renewal inside a
lease budget goes away. The protocol between servers stays free of clocks: no timestamp crosses the
wire as authority, and no server compares its clock with another's.

Closes [issue #2431](https://github.com/Altinity/ClickHouse/issues/2431) (CAS-298), CAS-152,
[issue #2403](https://github.com/Altinity/ClickHouse/issues/2403) (CAS-292) and CAS-342.

## Step one: GC counts a sighting from after its read {#gc-sighting}

GC treats a server as dead when its mount token stays unchanged for `TTL + TTL/20 + period`
(41.5 s) on the GC leader's own clock, and then writes `gc_fenced` into the slot. The wait must
start when GC saw the token. Today it starts earlier.

`Gc::runRegularRound` and the rebuild pass call `computeHeartbeatFloor` with a clock sample taken
before the `LIST` and the per-slot reads. The function stores that sample as the first sighting.
A sighting made `d` after the sample is credited `d` too early:

| Time | Event |
|---|---|
| 0 s | The round starts and samples the clock. The store is slow. |
| 18 s | The holder commits a renewal: token `T`, authority until 48 s. |
| 20 s | GC reaches the slot, sees `T`, records "first seen at 0 s". |
| 42 s | A second round sees `T` unchanged: 42 − 0 ≥ 41.5, so it fences. It really watched for 22 s. |
| 42 to 48 s | The slot is fenced and the holder still considers itself entitled to write. |

Both conditions are needed: the walk spends more than `threshold − TTL` (11.5 s; 10 s with a
5 percent faster observer clock) before the read, and a second round follows soon. The regular
timer waits a full `gc_interval_sec` after a round ends, so only a manual round, an early requested
round or the rebuild pass can be that second round.

Today a holder whose renewals fail fences itself about 14 s after its last success, which hides
the defect in that case. A holder whose renewal thread is stalled keeps authority to `start + TTL`
already, so the defect is reachable in the current code too. The rest of this design keeps the
lease until the TTL in every case, so the fix comes first.

**Fix.** A sighting is recorded with a clock sample taken after the read that produced it, on the
first decision and on the re-decision that follows a refused fence-out. The stability test uses a
sample taken before the confirming read; the round-start sample qualifies. `claimMountAwaitingExpiry`
already samples this way. Both observers use one helper, [`TokenWatch`](#what-goes-away). No format
change.

## How it works {#how-it-works}

1. A renewal succeeded. The lease is valid for `cas_mount_lease_ttl_ms` (30 s), counted from the
   moment that renewal started.
2. Wait `cas_mount_renew_period_ms` (10 s), counted from the same moment. Start the next renewal.
3. A renewal is one conditional `PUT` of the mount object. Its timeouts are the disk's connect
   timeout (1 s) and `cas_attempt_timeout_ms` (5 s) for each send and receive.
4. It succeeded: go to 1.
5. It failed for a transient reason (timeout, `503`, connection error): try again.
   - The request used a second or more: retry at once.
   - The request failed faster: wait out the rest of that second first, so retries never spin.
   - The outcome is unclear: read the object first to see whether the `PUT` landed.
6. Keep trying for as long as it takes. Meanwhile the holder's own timer decides about writes. Once
   the lease has expired, writes are refused. When a renewal restores the lease, writes are accepted again, under
   the same `writer_epoch`, with no remount.
7. Stop trying only on a definitive answer: the slot is no longer ours (another writer's body,
   `gc_fenced`, the object is gone), the store refuses the request for good, or the server is
   stopping or was told to remount. Then remount, as today.

Three details:

- New conditional writes stop being admitted about 16 s before the expiry, because a write reserves
  time for its own request and for the read that checks it. This is today's admission rule.
- After a long outage the first successful renewal counts from when it started, which may already
  be more than a TTL ago. The next renewal then follows at once, and that one restores the lease.
- The same holds at startup and after a remount: the lease counts from the claim. If the claim is
  already too old to admit a write, the loop renews first; only then does the open return or the
  remount report `Live`. Nothing else renews.

## What changes {#what-changes}

| | Today | With this design |
|---|---|---|
| Retries of a failed renewal | about 4 s | until a definitive answer |
| When retries end | fence, remount, fresh epoch, an observation of at least 36.5 s | they do not end |
| `PUT` fails for under about 19 s | at least 36.5 s without writes | the lease never expires |
| `PUT` fails for 60 s, store-wide | fence, then remount after the store returns | writes refused until the store returns and a renewal restores the lease |
| This node cut off, GC elsewhere healthy | fence and remount | a later GC round that saw the token unchanged for 41.5 s fences the slot; the renewal reads `gc_fenced`; the remount reclaims at once |
| Memory limit exceeded on the renewal thread | terminal renewal, or the thread ends | the tracker does not throw on that thread |
| Ways to renew | four: worker, startup, remount, direct | one: the loop |
| Threads per mount for the lease | two, with a parking protocol | one |

Today's 4 s is `TTL − period − margin − 2 × envelope`: before each request the engine reserves two
7 s envelopes against the lease deadline. The 36.5 s is the token-stability threshold in
`claimMountAwaitingExpiry`.

## Settings {#settings}

No new setting.

| Setting | Default | Role |
|---|---|---|
| `cas_mount_lease_ttl_ms` | 30000 | Lease length from the start of the last successful renewal. Writes are never admitted past it |
| `cas_mount_renew_period_ms` | 10000 | How often a renewal starts, counted from the start of the last successful one |
| `cas_attempt_timeout_ms` | 5000 | Send and receive timeout of one request; also the size of the time a data write reserves |
| `connect_timeout_ms` of the disk | 1000 | Connect timeout, applied to TCP and again to TLS |
| `cas_lease_safety_margin_ms` | 2000 | Margin that write admission and the farewell keep before the expiry; not used by renewals |

The one-second spacing of retries is a constant in code. `validateCasRequestBudget` keeps both
inequalities. `period + 2 × envelope + margin < TTL` now means: between two renewals there is always
room to admit a write.

## What you see {#observability}

| Signal | Where | Meaning |
|---|---|---|
| `lifecycle = 'not_live'`, `lifecycle_reason = 'lease_expired'`, `lifecycle_since`, `lifecycle_detail` | `system.cas_mounts` | The confirmed deadline has passed and no renewal has restored the lease yet. `lifecycle_since` is that deadline; `lifecycle_detail` carries the text of the last failed request |
| `CASMountLeaseExpired` (new) | `system.events` | +1 when a renewal restores a lease that had expired |
| `expired_ms` in the `watermark_renew` event; a `WARNING` with the last failure | `system.cas_log`, server log | How long the lease was expired and why |
| `CASMountRenewalAttempts`, `CASMountRenewalRetries` | `system.events` | Advance as requests are sent, not at the end, so an outage shows while it lasts |
| `CASMountLeaseLost` | `system.events` | Unchanged: the mount was fenced locally and a remount follows |
| Text of a refused write | exception | Where the runtime itself refuses (`checkFenceOrThrow`), the text says the lease expired and that writes resume when a renewal succeeds. The request engine and the ref ledger cannot tell an expired lease from too little budget or a lost fence; their texts ("no lease budget", "gave up at the lease deadline") stay and still name the lease |

The expired row is shown only while the lifecycle enum is `Live`; the enum itself is not touched.
`lifecycle_reason = 'lease_expired'` is about this server's own confirmed deadline. The existing
`state = 'expired'` column is a display value from the body's wall-clock `expires_at_ms` plus a skew
allowance. The two are independent signals and need not agree.

## Simpler code {#simpler-code}

Most of the machinery around the lease exists because a renewal used to need lease budget. Without
that need the two-worker protocol has no job. What replaces it is smaller: one loop, one arming
rule, and a readiness wait.

### One loop {#one-loop}

One thread per writable mount owns the lease:

```text
until stopped or the lifecycle is terminal:
    if a remount is requested:         # requested generation > handled generation
        latch the fence                # a reclaim always starts fenced
        reclaim for that generation,   # today's remount chain: probe, fresh epoch, claim, quiesce
        or wait and retry
        if the claim admits a write: arm the fence and report Live     (arming rule)
        continue                       # a request raised meanwhile is served before any renewal
    wait until one period after the last successful start, a request, or a stop
    renew                              # retries inside; ends on success, a definitive answer,
                                       # a stop, a remount request or a terminal lifecycle
    success:  deadline = start + TTL
              if the fence is not armed and the deadline admits a write:
                  arm it and report Live                                 (arming rule)
    ended by a stop or a terminal lifecycle: trip the fence without counting a loss; exit
    terminal: trip the fence and request a remount, unless one is already pending
```

**Arming rule.** The fence is armed, and `Live` reported, only when no remount request is pending,
no stop is requested, the lifecycle is not terminal, and the deadline admits a ref append now. The
check and the arm are one step under `driver_mutex`, the mutex that the publication of a FORGET
intent also takes. `Live` is published before `lost` is cleared: a reader that sees the fence armed
reads `Live` afterwards, and the one window a reader without the mutex can see is `Live` with the
fence still lost, which only refuses.

"Terminal lifecycle" is today's `remountTerminal` predicate, including a published FORGET intent.
A stop trips the fence without counting a lease loss, as today. The requested and handled remount
generations, the terminal predicate and `driver_mutex` with its condition variable stay; the mutex
is never held across a reclaim, a request or a join.

Two things the lifecycle model found in today's code close on the way:

- **A remount request raised during a reclaim is overridden.** The running reclaim re-arms the fence
  over the trip that came with the request, and the next reclaim starts with `noteLeaseLost` only,
  so the pool runs not `Live` on an armed fence. Latching the fence at the start of every reclaim
  and the arming rule close it.
- **With the arming rule no reclaim can arm the fence while FORGET is joining the thread.** The
  second `tripMountLost` of FORGET stays as a backstop.

### What goes away {#what-goes-away}

| Removed | What it guaranteed | What guarantees it now |
|---|---|---|
| The startup and remount re-anchor renewals (`renewRenewerForStartupOnce`, `renewRenewerForRemountOnce`, `renewForRemount`) and the copied `period + 2 × envelope` window test in `mountWritable` and `tryRemountOnce` | After a slow claim or a long quiesce the first worker renewal still had lease budget, and an open returned or a remount reported `Live` only with a lease that admits writes | A renewal needs no budget. If the claim admits a ref append the fence is armed from it; otherwise the loop renews first and that renewal arms the fence ([readiness](#readiness)) |
| The direct renewal (`renewWatermarkOnce`, `DirectCall`), used by tests only | Tests could drive one renewal | Tests call the loop's renewal step |
| The remount worker thread, `ParkRequested` / `Parked`, `DriverLease`, `admitRenewerCall`, all nine `RenewalDriverState` values | The renewer is never replaced while a renewal is in flight; one driver at a time | One thread renews and reclaims in sequence. Remount requests stay generations, so a request raised during a reclaim is not lost |
| The thread-local observability stack, its deferred delivery and nesting (`MountRenewObservabilityStack`, `configureMountRenewObservability`, `deliverDeferredMountRenewObservability`) | The sink reached `renewOn` without a parameter; a sink that renews was tolerated; logging waited until `remount_mutex` was released | `renew` returns its result and the loop reports it. No renewal runs under `remount_mutex`. An event sink must not renew; production sinks only append to a system log |
| `MountLeaseRenewer::confirmed_deadline_boot_ms`; renewals on the fence-gated mount plane | A copy of the fence deadline inside the renewer; a renewal that the fence could refuse | The runtime's fence deadline is the only copy and the farewell receives it as an argument. Renewals run on a plane with no lease budget whose sleep a stop wakes; a renewal still ends on a remount request, a terminal lifecycle and a published FORGET intent. Claim and farewell keep their plane and its plain sleep, because the farewell runs after the stop |
| The token-stability code written twice | See [step one](#gc-sighting) | `TokenWatch`: `sighted(token, sample_after_read)` and `stableFor(sample_before_read)`, used by `claimMountAwaitingExpiry` and `computeHeartbeatFloor` |
| `2 × envelope` computed in `refAppendFenceOk`, `validateCasRequestBudget` and `terminate`; the literals `2000` (four places) and `90'000` (three); ignored parameters of `consumeRenewResult`; comments about a stack-local observer and an unclean-boundary marker that do not exist; "Startup-only" in the text of `cas_lease_safety_margin_ms` | Nothing | One definition each; the stale text is corrected. The farewell keeps its own slack and minimum window |

Estimated effect, from the line counts of the affected areas and not from a patch: in
`CasMountRuntime.cpp`, `CasServerRoot.cpp` and `CasPool.cpp` (5,167 lines today) about 1,000 lines
go and about 450 come, a net of about 500. Across all production files the net is about 480 lines
fewer. Test code grows by about 1,000 lines, so code plus tests end up larger. What shrinks is the
ownership protocol: nine driver states to none, four renewal entry points to one, two lease threads
to one. The retry and reporting code grows.

Tests go only where their subject becomes unreachable: the direct renewal and its races with
worker start, the order of the re-anchor inside open and remount, the deferred reporting of a
renewal made under `remount_mutex`, and nested renewals from a sink. Tests whose guarantee stays
reachable are adapted to the loop, not deleted ([test 17](#tests)).

### What stays {#what-stays}

- The claim protocol, the observation wait, the epoch and owner objects, the remount chain's steps
  and their order.
- The renewer's states (`New`, `Active`, `RenewalTerminal`, `Released`) and the tuple and start
  rules.
- The fence, `admit`, `mayMutate`, the fence generation, lifecycle terminal publication.
- The farewell and its lease-bound policy: `Retry::untilLeaseSafe`, `lease_deadline_ms`,
  `Bound::lease_bound`. The bound is absolute and its give-up reports the lease as the source; a
  window computed by the caller would move with the clock and report the policy.
- The request engine: reservations, gates and pauses of every data write.
- The GC leader lease. It looks similar and is a different thing: it counts loop ticks, has no TTL
  and no clock, keeps its heartbeat in a separate object, and safety does not depend on leadership
  being exclusive or timely: the durable authority checks against `gc/state` and the existing
  cleanup proofs carry it. The only place GC meets the
  mount lease is the fence-out of dead mounts, which `TokenWatch` now serves.

### Readiness is kept {#readiness}

Today an open or a remount whose claim is more than 4 s old renews once before arming the fence, so
that callers find a lease that admits writes. Callers depend on it: ref-table recovery checks
admission outside its retry loop, and table loading propagates the failure. The order stays, through
the one path:

- The claim admits a ref append: arm the fence and report `Live` at once.
- It does not: the fence stays latched and the lifecycle stays not `Live`. The loop's next renewal
  runs in that state. A success whose deadline admits a ref append arms the fence from that
  renewal's start and reports `Live` in the same step, under the arming rule. A success with a stale
  start arms nothing; the loop renews again at once.
- At an open the runtime starts with the fence unarmed, which admits writes. An open whose claim
  does not admit a ref append therefore latches the fence itself, without counting a lease loss,
  before it starts the thread. The open has not returned, so no caller sees that state.

So "not `Live` implies the fence is lost" holds as it does today, and a state "armed but not `Live`"
does not exist. An open waits one TTL for readiness; then it requests a stop, joins the thread and
fails, as a failed re-anchor fails it today. A request or a credential refresh already under way,
and the join, come on top of that TTL.

### Further cleanups {#further-cleanups}

Small, independent, found by a second pass over the same files. Each removes state that nothing
reads or says one thing in one place. None changes behaviour, except that item 7 drops a duplicate
log line.

1. Delete `process_epoch`, its random initial value, `setProcessEpoch` and the `epoch` /
   `writerEpoch` accessors. No production code reads them; identity uses the allocated
   `writer_epoch`, incarnation checks use `liveWriterEpoch`.
2. Drop `terminal_state_published`: the `Vanished` lifecycle values already say it. Keep
   `vanished_intent`, which FORGET publishes earlier, and the publication order under `driver_mutex`.
3. One source for the renewal period and for the boot clock: remove the copy `renewal_period` and
   `MountRenewOperationEnvironment::boot_ms`.
4. `CasGcScheduler::runRoundLogged` takes only the trigger; `allow_steal` follows from scheduled
   versus manual. `runOneRoundNow` loses its parameter.
5. One predicate for "background work must stop": the GC scheduler uses the runtime's
   `remountTerminal` instead of repeating its two conditions.
6. Remove write-only fields: `MountFence::server_uuid` and `writer_epoch`, the runtime's
   `backend_ptr`, `Pool::farewellRequests`, `MountRenewResult::sent_any`.
7. Remove the second observation log line in `claimMountAwaitingExpiry`; correct six comments and
   two setting descriptions that describe timing wrongly, and the comment at step (2) of
   `Pool::forgetDisk`, which says a live pool becomes `TransientNotLive` after the intent is
   published.
8. Replace `setRoundInFlightForTest` by assertions in the test that already parks a real round.
9. Rename the renewer's `open_requests` to `claim_farewell_requests`; the pool's "open plane" is
   the GC plane.

Looked at and left alone, because each loses something:

- **One request plane with one sleep for GC, renewal and farewell.** Their stop conditions differ:
  FORGET keeps the GC plane usable while it stops the lease thread, and the farewell runs after the
  stop.
- **One claim policy for open, remount and decommission.** A remount must not use
  `cas_unsafe_remount_no_delay`, and decommission must not wait.
- **GC leadership without the heartbeat object, the acquisition-time pulse or the `i_am_leader`
  hint.** A long fold leaves `gc/state` unchanged; without the pulse a follower steals from a live
  leader.
- **A simpler steal rule.** Heartbeat movement under a deposed owner's name must still block a
  steal, and a manual round must not arm one.

## Renewal under memory pressure {#memory-pressure}

A memory-limit exception inside a request is a transient failure and is retried like any other. Two
gaps remain, from issue #2403: an exception outside the request ends the renewal thread for good,
and the `PUT` runs on a pool thread with a 1 MiB buffer.

- The loop holds `LockMemoryExceptionInThread(VariableContext::Global)` across each iteration.
  Allocations stay accounted; the tracker does not throw on this thread. Keeper and the transaction
  log use the same guard.
- `ObjectStorageBackend::conditionalWriteSettings` sets `s3_allow_parallel_part_upload = false`, so
  a conditional `PUT` runs on the calling thread and the guard covers it. This applies to every
  conditional control-plane write, not only the renewal. The caller waits for the `PUT` either way;
  a large conditional write uploads its parts one after another, which can take longer and uses
  less memory.
- A conditional `PUT` starts with a buffer of the body size, at least one byte, instead of 1 MiB;
  `WriteBufferFromS3` keeps its own growth and caps. Reads keep their current sizing.
- An unwrapped `std::bad_alloc` that reaches the request engine is still terminal. Classification
  of failures is unchanged.

## Why it is safe {#safety}

**Nobody acts on a holder's death without first changing the mount object.** GC fences by a
conditional write (`computeHeartbeatFloor`). A claim, a reclaim, `cas_unsafe_remount_no_delay` and
the farewell are conditional on the token they read. Decommission reclaims only a fenced or
farewelled slot and deletes by exact token. Holder bodies carry a random `write_attempt_id`, so a
token never repeats. `expires_at_ms` is used for display and for one non-authoritative precheck in
`allocateWriterEpoch`. A renewal that succeeds against its expected token therefore proves the slot
was not taken.

**The holder never has authority longer than an observer waits.** Authority ends at `start + TTL` on
the holder's clock. An observer sees the new token no earlier than that start and acts only after
the token stayed unchanged for `TTL + TTL/20 + cadence` on its own clock. Retrying does not move the
start. The observer side needs [step one](#gc-sighting).

**No clock is shared.** The holder measures `start + TTL` on its own `BOOTTIME` clock. An observer
measures the threshold on its own monotonic clock, from its own sighting. Nothing compares the two,
and `expires_at_ms` on the wire authorizes nothing. The one timing assumption is today's: the two
clocks run at rates within 5 percent of each other, which is the `TTL/20` term. This design adds no
assumption. It does use the existing one fully: a holder with failing renewals no longer fences
itself early. The margin between the end of the holder's authority and the earliest fence is then
`Allowance + cadence` without drift (11.5 s for GC, 6.5 s for a re-mount) and 9.5 s and 4.8 s at
5 percent drift. Drift beyond the allowance breaks it ([models](#models)).

**A request that lands late is harmless.** A renewal request that lands after the holder stopped is
conditional on the old token: it lands on an unfenced, non-farewell body, or a successor moved the
token and it is refused (`WitnessLateBeforeReclaim`, `WitnessLateAfterSuccessor`). A claim that
skips the observation wait is safe only after the old runtime is locally terminal, which is the
remount case.

**A write that spans the expiry is the same write as inside one lease.** A thread can stall between
admission and send, or across a retry pause, and continue after the lease was restored; a reservation
is not an execution deadline. A request sent before the expiry can land after the restore. Nobody
else held authority in between, and every write on the mount plane is a conditional create, a
token-conditional update or delete, or content-addressed. Callers that see a refusal
(`resolveWedgeOnce`, `commitRefChunk`, `publishCkpt`, the catalog mutators, `ensureBlobPresent`)
refuse or mark the table for recovery; none treats it as the end of the incarnation.

**The models agree.** `CaCasMountCore.Renew` has no expiry guard and `Write` is guarded by the clock
only. The results are in [models](#models).

**Checked beyond the mount code.** In `Gc/`, creator retirement needs a fenced, farewell or
superseded-epoch certificate, orphan eligibility uses the durable epoch and build floor, recovery
seals only older epochs, and GC leadership rests on `gc/state`. Code above `ContentAddressed/` does
not parse the lease error and does not use `system.cas_mounts` as authority; the refusal class stays
`NETWORK_ERROR`, which the part-check paths retry. `Backend/CasProbe.cpp` fails a store that ignores
`If-Match` on `PUT` or `DELETE`.

## Rules {#rules}

The exact contract. A review checks the design against this list.

1. **One path.** Only the loop renews. An open returns, and a remount reports `Live`, only after a
   ref append can be admitted. If the claim does not admit one, the fence stays latched until a
   renewal of the loop succeeds with a deadline that does; that renewal arms it.
2. **One tuple.** A renewal fixes `(key, bytes, expected token, write_attempt_id, start)` before I/O.
   Every request repeats it byte for byte. Unchanged.
3. **Start.** The start is the `BOOTTIME` instant sampled before the first request. Success sets the
   deadline to `start + TTL`. A response never moves it. Unchanged.
4. **No deadline.** The renewal is not bounded by the lease or by a policy window and reserves
   nothing against either. It ends on success, a definitive answer, stop or a remount request. The
   policy window is `UINT64_MAX`; `Retry::bind` saturates, so the only horizon is the range of the
   clock.
5. **One engine call.** A renewal is one call of the request engine, so the read that adopts a
   landed request belongs to the call that sent it. No second renewal starts while one is unresolved.
   A timed-out copy of an already committed renewal can still be in flight when the next one starts;
   it is conditional on a token that no longer exists.
6. **Definitive answers are terminal.** Unchanged classification: foreign or successor occupant,
   own slot `gc_fenced`, own `(uuid, epoch)` body with other bytes, vanished slot, deterministic
   store refusal, deterministic local failure. A refused precondition whose resolving read keeps
   failing transiently is not an answer yet: the read is retried, spaced, until the slot is read or
   the renewal is ended. A store refusal that follows an unclear request of the same renewal is not
   an answer either, because the unclear request may still land: the engine settles it by a read and
   retries. Such a renewal ends on a stop or a remount request; meanwhile the lease expires and the
   refusal shows as the last failure.
7. **What ends a renewal early.** A stop, a pending remount request, a terminal lifecycle or a
   published FORGET intent, whatever the lease budget. A lost fence alone does not: the readiness
   renewal of rule 1 runs under one, and every trip that must stop a renewal comes with a request,
   an intent or a stop. `NotAttempted` only when a stop was already requested when the renewal began
   and nothing was sent; every other early end is terminal and forbids the farewell.
8. **Spacing.** Each retry draws an interval of 800 to 1200 ms and waits until that long has passed
   since the failed request started; a request that already took that long is retried at once. This
   covers the ordinary, the connect-failure and the credential-refresh retry of the `PUT` and the
   retry of the read that checks it. The read that follows an unclear `PUT` is not a retry and is
   sent at once. The first-attempt fuse keeps its immediate reissue. Worst case is about five
   requests per second. A stop wakes the wait. A remount request is seen at the next admission
   check, after a request or a credential refresh already under way returns.
9. **Authority.** Write admission is unchanged: budget, `lost` and generation as today. An expiry
   changes neither `mount_fence.lost`, nor `fence_generation`, nor the lifecycle enum, nor the
   epoch.
10. **Resume.** After a success whose `start + TTL` is already past, writes stay refused and the
    cadence starts the next renewal at once. Writes return when a success leaves enough budget for
    admission.
11. **Observer timestamps.** An observer records the first sighting of a token with a clock sample
    taken after the read, and tests stability with a sample taken before the confirming read. The
    test is false when that sample precedes the sighting.
12. **Memory guard.** The tracker does not throw on the lease thread, and a conditional `PUT` runs
    on the thread that issues it.
13. **One owner of the renewer.** Only the lease thread renews, replaces or reclaims while it runs.
    Open installs the renewer before the thread starts; teardown and FORGET join the thread before
    the farewell or the re-latch. A remount request is a generation: a reclaim acknowledges only the
    generation it served, and a terminal renewal raises one only if none is pending.
14. **Latch and arm.** A reclaim starts by latching the fence. The fence is armed, by a reclaim or by
    a renewal, only when no remount request is pending, no stop is requested, the lifecycle is not
    terminal and the deadline admits a ref append; the check and the arm are one step under
    `driver_mutex`, and `Live` is published before `lost` is cleared.

## Order of work {#order-of-work}

Each step leaves the tree working. The rules describe the state after step 6.

0. Models ([below](#models)). Done.
1. `TokenWatch` and the GC sighting fix (CAS-342).
2. Memory guard, inline `PUT`, buffer size (CAS-292).
3. The worker's renewal without a deadline, spacing, expiry reporting (CAS-298). The startup and
   remount re-anchors keep their bounded policy in this step.
4. One thread: move the reclaim into the loop, with the latch at its start and the arming rule;
   remove the remount worker and the driver states. The re-anchors stay as calls from the loop.
5. One renewal path: the readiness wait of the open, the closed initial state, the readiness
   renewal in the loop; then remove the re-anchors and the direct renewal.
6. The renewal returns its result; remove the thread-local stack. Needs step 5.
7. One definition for each duplicated quantity; stale text.
8. The [further cleanups](#further-cleanups), one commit each.

Steps 1 to 3 close the three issues and form the first implementation plan. Steps 4 to 8 restructure
and remove code; they form a second plan, written against the code as it is after step 3, and can be
deferred as a group.

## Wedged ref lanes after an outage {#wedged-lanes}

An append made ambiguous by the outage leaves its lane `Wedged` until the next mutation of that
namespace runs `resolveWedgeOnce`. Today a remount also clears it. Without the remount, a wedge in a
namespace that gets no further mutation stays. It has no correctness consequence: the attempted
tuple is retained and cannot be bypassed. Costs:

- a relink confirm for that table answers `Unknown`;
- the shutdown writes no farewell, so the next start pays the observation wait;
- the runtime and its attempt bytes stay in memory, cache eviction skips it, and the cache can
  exceed its budget;
- snapshot publication for that table is refused.

The state is not new: a blip shorter than today's 4 s window leaves the same wedge. The worst case
equals what every outage costs today, deferred to the next restart and limited to servers with a
quiet wedged table. Background retry of wedged lanes is a separate change.

## Models {#models}

Step 0 is done. Four TLA+ models cover the design. Every row of every runner meets its exact
expectation; the controller re-ran each battery after the change.

| Model | Subject | Result |
|---|---|---|
| `CaMountRenewRetryCore` | One renewal: tuple, start, retries past the deadline, adoption by read, late copies, two renewals in a row | 19 rows. Safe graph exhaustive: 2,775,783 distinct states |
| `CaMountObserveCore` (new) | A holder and an observer on separate clocks: when a fence can land | 20 rows and 12 sweep points. Greens exhaustive: 60 to 70 million distinct states each |
| `CaDiskLifecycle` | One lease thread, remount generations, arming, FORGET, teardown | 17 rows. `main` with liveness: 5,423 distinct states |
| `CaCasMountCore` | Mount, fence, remount and ref lanes across epochs | 23 rows; `stage1` and `v9_recoverygen` unchanged |

What they establish:

- **Authority is the holder's own timer.** `LocalAuthority` is `~fenceLost /\ clock <
  confirmedDeadline`; no observer action touches it. `AuthorityEndsAtConfirmedDeadline`: the deadline
  is the first send of the committed renewal plus the lease. The sabotage
  `sab_anchor_deadline_at_retry` is caught only by the independently recorded first send.
- **Retrying past the deadline is reachable and safe.** `WitnessRetryPastDeadline` and
  `WitnessAuthorityRestored` (a stale success, then a second renewal restores authority) fire with
  all twelve invariants on. With the old budget guards put back, both become unreachable.
- **No authority once someone else is entitled.** `NoAuthorityAfterEntitledTakeover` holds for a GC
  fence, a successor and a foreign writer.
- **Rule 11 is what keeps a fence out of the holder's authority.** `NoFenceInsideAuthority` holds
  with drift inside the allowance, for GC and for a re-mount, when the holder uses its full TTL.
  Three sabotages violate it: `sab_sighting_before_read` (today's code), `sab_redecision_stale_sample`
  (the re-decision after a refused fence-out) and `sab_wrapping_stability` (a stability test with a
  sample older than the sighting). The walk bound of step one is confirmed: green up to
  `threshold − TTL`, red one tick above.
- **A bounded clock-rate difference is load-bearing.** `env_drift_exceeds_allowance`, with drift far
  above the allowance, violates `NoFenceInsideAuthority`.
- **The margin survives the full TTL.** `FenceMarginAtLeast` is exact on twelve scaled points, which
  match the model's discrete formula (it includes one banked observer tick). The real-scale figures,
  9.5 s for GC and 4.8 s for a re-mount at 5 percent drift, are calculated from the continuous form
  `(TTL + TTL/20 + cadence)/1.05 − TTL`; TLC did not run at that scale.
- **One thread keeps every lifecycle property**, with the arming rule and the latch at the start of
  a reclaim ([one loop](#one-loop)). `NoArmWhileRequestPending`, `NoJoinWindowReclaim`,
  `RenewNotAfterIntent`, `LiveOnlyWhenAdmissible`, `I7RequestNotLost` each have a sabotage.
- **A late renewal under the same epoch, then a lane retry and a write, is reachable** with the seven
  invariants of `CaCasMountCore_v9_recoverygen.cfg` green (`witness_laterenew`, through the wrapper
  module `CaCasMountCoreLateRenew`).

Limits:

- `CaMountObserveCore` runs with an allowance of 25, 17 and 10 percent of the lease. The exact
  5 percent configuration (`TTL = 20`) did not complete and was stopped at 184 million states. The
  real-scale margins come from the formula that the scaled points confirm.
- `GcFence` in `CaCasMountCore` stays an abstraction (`mount.deadline + Drift <= clock`);
  `CaMountObserveCore` is its justification. `CaCasMountCore_rev6_observe.cfg` still never completes.
- `CaDiskLifecycle` has no time and does not model the open.
- A paused holder clock is outside the rate bound; the code uses `BOOTTIME` for that reason.
- Not modelled: the spacing of retries, the memory guard, reporting.

TLC runs through the `run_*.sh` scripts with `nice 10`, an explicit heap cap, and a state directory
on a disk with room; the docs record state counts, not wall-clock time.

## Tests, failing first {#tests}

Unit tests use `RenewalScriptBackend` and `Ops` from `src/Disks/tests/gtest_cas_heartbeat.cpp` with
an injected boot clock; `CASMountRuntime` tests live in `gtest_cas_mount_runtime.cpp`. The rule each
test guards is in brackets. Order follows the [order of work](#order-of-work). Tests 6, 7 and 9
guard behaviour that holds today too: on today's code they fail only because the renewal gives up at
the deadline before reaching the asserted state. Test 17 is regression coverage carried over to the
loop; it passes today in its present form.

1. `CASTokenWatch.CountsFromAfterTheRead` [11]: a sighting is dated by the sample passed after the
   read; `stableFor` compares against the sample passed before the confirming read and is false when
   that sample precedes the sighting; a changed token restarts the watch.
2. `CASHeartbeat.GcCountsASightingFromAfterItsRead` [11], three cases over an injected clock
   function. The clock advances by more than `threshold − TTL` between the call of
   `computeHeartbeatFloor` and its read of a slot: a second call one threshold after the first
   call's start does not fence; a call one threshold after the read does. A sighting recorded in the
   re-decision after a refused fence-out is counted from after that read. A call whose own clock
   sample precedes the threshold does not fence, however long its walk takes.
3. `CASMountRuntime.MemoryLimitDoesNotEndTheLeaseThread` [12]: with the global tracker over its
   limit, an allocation before the request and another while the result is consumed do not throw,
   the tracker still counts them, and the renewal over the in-memory backend succeeds. A memory-limit
   exception injected into a request is retried, does not trip the fence and does not end the thread.
4. `CASBackend.ConditionalPutRunsOnTheCallingThread` [12]: `conditionalWriteSettings` has parallel
   part upload off, and a conditional `PUT` over the S3 object storage runs on the thread that
   issued it. A small conditional `PUT` starts with a buffer of its body size, and an empty body with
   one byte.
5. `CASHeartbeat.RenewalOutlivesTheReservationCutoff` [4]: `PUT` fails transiently for 8 s, then
   succeeds. `Committed`, renewer `Active`, and the number of `PUT`s matches one per spacing
   interval. Replaces `RenewalStopsBeforeTheCutoffWhenEveryAttemptConsumesTheEnvelope`.
6. `CASHeartbeat.RenewalSendsOneTupleOnEveryAttempt` [2, 5]: across that outage every request
   carries the same bytes, expected token and `write_attempt_id`; no second body appears before the
   first is resolved; the renewal succeeds when the outage ends.
7. `CASHeartbeat.RenewalSucceedsPastTheDeadlineWithItsFirstStart` [3]: failures last longer than the
   TTL. The result reports the first request's start; `start + TTL` is past.
8. `CASHeartbeat.LandedAttemptIsAdoptedAfterALongOutage` [5]: the first request lands, its response
   is lost, later requests and reads fail past the deadline, then a read succeeds. `Committed` with
   `resolved_by_read`, not `same_epoch_state_uncertain`.
9. `CASHeartbeat.DefinitiveAnswersStayTerminalPastTheDeadline` [6]: past the deadline the slot turns
   `gc_fenced`, foreign, a newer own epoch, an own-epoch body with other bytes, or absent; or the
   store refuses deterministically; or the request raises a deterministic local failure. Each is
   terminal with today's classification, asserted by name. A clean refused precondition followed by
   resolving reads that keep failing transiently is not terminal: the read is retried until it
   answers, and then the classification follows.
10. `CASHeartbeat.RenewalSpacesRetries` [8], four cases. A credential-refresh retry keeps the spacing. Fast connect failures: `PUT`s start 800 to
    1200 ms apart. Unclear `PUT` with a failing read: the read follows its `PUT` at once; retries of
    the read and of the `PUT` keep the spacing. A request that takes 5 s on the injected clock is
    retried with no wait. In each case the first-attempt fuse reissues at once for its verb, and the
    renewal is still running after 30 s.
11. `CASHeartbeat.StopDuringARetryWaitEndsTheRenewal` [7, 8]: a stop during the wait returns without
    waiting it out; terminal; `release` forbidden. A stop after the renewal began but before its
    first send is terminal too. `CancellationBeforeSendIsNotAttemptedAndAllowsRelease` and
    `CancellationAfterSendIsTerminalAndForbidsRelease` stay green.
12. `CASMountRuntime.ExpiryRefusesWritesAndResumesUnderTheSameEpoch` [9, 10]: driven by the real
    loop over a scripted backend. Past the deadline `admit` returns `NoBudget`, `lost` is false, the
    lifecycle enum is `Live`. After the stale success `admit` still returns `NoBudget` and the next
    renewal starts with no cadence wait. After it `admit` returns `Ok`, same epoch and generation.
    An operation admitted before the expiry continues after the restore; the same operation is
    refused after a remount changed the generation.
13. `CASMountRuntime.ExpiryIsReported`: during the expiry the lifecycle snapshot reads `not_live` /
    `lease_expired`, `lifecycle_since` is the deadline and `lifecycle_detail` holds the last failure
    text, also when only the resolving reads fail; the two attempt counters advance during the
    outage; a stale success does not end the expired interval; the restoring renewal increments
    `CASMountLeaseExpired` once and the event carries `expired_ms` from the original deadline; the
    completion writes one summary line, whatever the number of requests.
14. `CASMountRuntime.OpenAndRemountReportReadyOnlyWhenWritable` [1, 14]: an open whose claim is 20 s
    old does not arm the fence; the loop renews with no cadence wait; that renewal arms the fence
    from its own start and the open returns with `admit` passing for a ref append. If the renewal
    keeps failing for one TTL the open stops the thread, joins it and fails. A readiness renewal that
    succeeds with a stale start arms nothing and reports nothing; the next one does. Neither does a
    success whose deadline has not passed but leaves less than a ref append needs. Before the
    thread starts, the open's latched fence refuses a write while the renewal itself is sent. A
    remount whose quiesce took 20 s stays not `Live` with the fence lost until the same renewal, and
    is never armed while not `Live`.
15. `CASMountRuntime.ARemountRequestEndsTheRenewalAndTheSameThreadReclaims` [7, 13]: an interference
    report while the renewal retries ends it within one wait plus one request; the lease thread runs
    the reclaim and then renews under the new epoch; a second request raised during the reclaim is
    handled before the next renewal: the first reclaim arms nothing, the second starts with the fence
    latched, and no write is admitted between them. The first report produces exactly one remount
    generation, one epoch change and one lease-loss count, also when the reclaim fails and is
    retried several times. On today's code the first reclaim re-arms
    the fence.
16. `CASMountRuntime.StopDuringAReclaimJoinsTheThread` [13]: a stop during a reclaim joins the thread
    when the reclaim attempt returns, the fence is tripped without a lease-loss count, and no
    farewell is written for the unclaimed slot. A FORGET intent published while the renewal retries
    ends the renewal and the loop. A reclaim that finishes after FORGET published its intent arms
    nothing.
17. Adapted to the loop, same assertions [13]: a terminal lifecycle wakes the thread during the
    cadence wait and during the reclaim backoff; a failed thread start fails the open closed; a
    renewal that fails right after a remount is not dropped; a deterministic failure fences without
    waiting for the cadence; an event sink that throws changes no outcome; a lease loss is counted
    once and a FORGET counts none; the farewell follows the join. From `gtest_cas_event_log.cpp`: a
    first-attempt success is silent, the events of a renewal are bounded and complete, terminal
    details carry the request count and classification. The three farewell tests in
    `gtest_cas_heartbeat.cpp` stay as they are.
18. Integration, `tests/integration/test_cas_mount_renewal_retry` with `s3_fault_proxy.py`:
    `test_short_put_outage_does_not_fence` holds `503` on `PUT` for 8 s across a renewal. No
    `fenced after` line on any node, `CASMountLeaseLost` unchanged, the next `INSERT` succeeds.
19. Same file, `test_long_put_outage_resumes_under_the_same_epoch`: the fault lasts 45 s. Afterwards
    `writer_epoch` in `system.cas_mounts` is unchanged, `CASMountLeaseExpired` is 1, an `INSERT`
    succeeds and the log has no remount line.
20. Soak card S39 queries every replica in its short-fault check.

## Documentation {#documentation}

- `docs/en/antalya/cas/architecture/mounts-and-leases.md`: the "Absolute deadline" and "Losing the
  lease" paragraphs, the lifecycle diagram, the 4 s scheduling-lateness sentence, the two-worker and
  parking description under "Mount, unmount, crash".
- `docs/en/antalya/cas/configuration.md`, `operations/troubleshooting.md`: the expired state, the
  new counter and event field, what a failing renewal now means, the text of
  `cas_lease_safety_margin_ms`.
- `docs/superpowers/models/CaMountRenewRetryCore_RESULTS.md` and `README.md`.

## Options considered {#options}

- **Tune the numbers** (period 5 s, pause clamped to what fits): 9 s of retries; the rule and the
  36.5 s cost stay.
- **Retry until the real deadline, then fence and remount**: about 18 s of tolerance, then today's
  cost.
- **Fresh epoch after every expiry**, written against the holder's own token with no observation
  wait: fails every build in flight and re-recovers every ref table each time; not needed for safety.
- **Fresh body and start per request**: breaks `OneLogicalBodyPerExpectedToken` and needs a set of
  pending attempt ids; the catch-up renewal gives the same result for one extra `PUT`.
- **Standard S3 retry delays** (`retry_initial_delay_ms`, `retry_max_delay_ms`) for the spacing: up
  to 5 s of extra wait after the store returns.
- **Immediate reclaim of a self-authored slot** (CAS-153): makes the remount cheaper instead of
  avoiding it. Still useful after an unclean restart.
- **A preallocated buffer per renewer** for memory pressure: the object-storage API has no
  read-into-caller-buffer entry point and the SDK allocates per request anyway.
- **One lease class for the mount and for GC leadership**: the two share an idea, not a mechanism
  ([what stays](#what-stays)); a common class would have to carry both a clock and a tick counter.
- **Arm from the claim and let callers retry**: ref-table recovery and table loading do not retry an
  admission refusal, so readiness is kept instead.
- **A farewell bounded by a window the caller computes**: the cutoff would move with the clock
  between computing and binding, and the give-up would report the policy instead of the lease.
- **`lost` as the only trigger of a reclaim**: a reclaim clears it, so a request raised during the
  reclaim would be dropped, and FORGET and stop set it without wanting a reclaim.

## Out of scope {#out-of-scope}

- Background retry of wedged ref lanes.
- Reservation, gates and pauses of data writes in `CasOperation`, including the sites that check
  both `gate` and `fits` and the loops that pause without a reservation.
- Renaming or splitting `cas_attempt_timeout_ms`.
- The policy that an exception escaping the loop's own state machine ends the thread.
- The GC leader lease.
