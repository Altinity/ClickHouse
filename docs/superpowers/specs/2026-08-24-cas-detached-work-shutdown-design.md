---
description: 'Design for draining detached CAS background work at shutdown, for event emission that survives a reset Context, and for a destructor boundary that cannot terminate the process'
sidebar_label: 'CAS detached work at shutdown'
sidebar_position: 6
slug: /superpowers/specs/cas-detached-work-shutdown-design
title: 'CAS detached work outliving Context'
doc_type: 'design'
---

# CAS detached work outliving `Context` {#cas-detached-work-shutdown-design}

**Status:** DRAFT for review, rev.2 (2026-08-24).

This specification covers item 10 of `docs/superpowers/cas/final-checks-todo.md`: findings B3 and B4
of the second 2026-08-05 umbrella review. It also covers the third recommendation triage made and the
item text omits — a `~Pool` that cannot terminate the process — brought into scope deliberately, for
the reason in [why it is in scope](#failsoft-scope).

**Every claim below was verified against `cas-gc-rebuild` at `57241b3f20b`.** Functions are named
rather than cited by line, because line numbers in this subtree move weekly and a stale one reads as
authoritative.

Nothing here changes a durable format, a key shape, or a protocol step. It changes when in-process
objects are torn down and what a teardown is allowed to write.

**What rev.2 changed.** Review found six defects in rev.1. The farewell ordering was described
backwards; the RAII pin as specified could not compile; the counter and its test disagreed; two
sections contradicted each other about `~Pool`; a fixed defect was listed as out of scope; and the
weak `Context` could not distinguish "never supplied" from "already released". Each is corrected in
place below.

## Decision {#decision}

One coupled change with three parts, none of which is sufficient alone.

1. **Sequencing and draining.** `ContentAddressedMetadataStorage` unpublishes its pool before waiting
   on anything, then stops and bounded-waits the detached CAS dispatches it now tracks. The stop
   closes the whole re-admission chain, not just its entry point, so the wait normally completes
   immediately.
2. **Emission that survives a reset `Context`.** The metadata storage stops holding a strong
   `ContextPtr`, its two event sinks resolve a weak reference per event, and the two CAS-owned
   accessors in `Interpreters/Context.cpp` stop dereferencing a null `shared`.
3. **A destructor boundary that cannot terminate.** Every phase of `~Pool` is individually guarded,
   `drained` defaults to false so a failed drain suppresses the clean-release marker rather than
   forging it, and the new metadata-storage destructor lets no exception escape.

The bounded wait can expire. Parts 2 and 3 are what make expiry survivable, which is why this is one
change rather than three.

## What is wrong today {#defect}

1. **`shutdown` drains only the GC scheduler.** `ContentAddressedMetadataStorage::shutdown` latches
   `shutdown_called`, moves the scheduler out and resets `part_access` and `cas_store` under
   `pointer_mutex`, then stops the scheduler. Nothing waits for detached work.
2. **Two detached dispatches hold a strong `Pool` reference and are untracked.**
   `Pool::reportImpossibleInterference` captures `shared_from_this` and detaches;
   `CasRefLedger::dispatchSnapshotPublisher` captures `pin_owner` — the same `shared_from_this` —
   plus raw `this`, and detaches. These are the only two `detach` calls in the CAS subtree.
3. **The publisher is a routine path, not an anomaly path.** It is dispatched from ordinary ref
   mutation, so a publisher in flight at shutdown is an everyday state.
4. **`~Pool` is not quiet.** It stops the mount-runtime workers, drains the ref lanes, and calls
   `CasMountRuntime::finishTeardown`, which on a clean drain performs the keeper's terminal release —
   object-storage I/O plus a `MountRelease` event emit.
5. **The event sink dereferences a `Context` that shutdown has already emptied.**
   `ContentAddressedMetadataStorage` holds a `const ContextPtr` member — the only long-lived strong
   `ContextPtr` in `src/Disks`. Both sinks copy it into a lambda and call
   `Context::getContentAddressedLog` / `Context::getContentAddressedGarbageCollectionLog`, which open
   with `SharedLockGuard lock(shared->mutex)` and never test `shared`. `Server.cpp` calls
   `resetSharedContext`, which nulls `shared`, immediately before releasing the context.

So a `~Pool` that runs late enough emits an event through a null `shared`, and the process dies during
shutdown.

**What this does *not* cost, contrary to the original review.** `MountLeaseKeeper::terminate` performs
its `putOverwrite` **first** and emits `MountRelease` **last**. The farewell marker is therefore
already durable when the emit crashes. The review's claim that the crash also eats the farewell write
described an older tree and does not hold at `57241b3f20b`. The defect is the process dying at
shutdown — which is enough — and overstating it would misdirect whoever reads this next.

The strong member has a second consequence independent of the crash: while a CAS disk is alive, and
one lives to the end of the process, `global_context.reset()` does not destroy the `Context`, so the
comment beside it in `Server.cpp` — "no one could own shared part of Context" — describes a state that
is not true.

Finally, `ContentAddressedMetadataStorage` has **no destructor**. An instance destroyed without
`shutdown` drops its pool from the implicit member destructor with no drain at all. The item text does
not mention this path.

## Sequencing: unpublish, then wait {#sequencing}

The drain must not run while the pool is still published. `ContentAddressedMetadataStorage::poolAccess`
copies `cas_store` into a strong `PoolPtr` under `pointer_mutex`; if the member still holds the pool
during the wait, a caller can take a fresh strong reference *after* the wait has succeeded, and the
wait proves nothing.

Teardown therefore becomes, in order:

1. Under `pointer_mutex`, move `gc_scheduler`, `part_access` and `cas_store` into local strong
   references and leave the members empty. After this point no new caller can obtain either.
2. Outside `pointer_mutex`: stop the scheduler, which joins its threads; release the local
   `part_access`; stop and drain detached work through the local `PoolPtr`; release the local
   `PoolPtr`.

This sequencing also **fixes**, rather than merely avoiding, the known defect that `~Pool` can run
under `pointer_mutex` and block snapshot readers for the length of a teardown: once the last reference
is a local, the destructor it may trigger runs outside the mutex by construction. That is a
consequence worth stating, because a later reader looking for that defect will otherwise not know it
was closed here.

The teardown helper is **idempotent**: `shutdown` and the destructor may both run, in that order, and
the second call must find nothing to do rather than repeat the wait.

### The guarantee, stated exactly {#guarantee}

This design does **not** promise that `~Pool` runs on the shutdown thread. A `PoolPtr` handed out by
`poolAccess` before step 1 can outlive `shutdown` in a caller's frame, and nothing here tracks those.

What a successful drain establishes is narrower and checkable: **no tracked detached work holds a
reference to the `Pool`.** The two dispatches this specification tracks are the ones that hold the
reference for an unbounded time with no one waiting on them; an ordinary caller's `PoolPtr` is scoped
to a call that the rest of shutdown already orders against.

Explicit teardown accordingly becomes the *usual* place where the last reference is released — not the
guaranteed one. Parts 2 and 3 of the decision cover the exceptions, which is the honest reason they
are not optional.

## Tracked dispatch: the pin, the counters, the stop {#tracking}

### The pin must be copyable {#pin}

`ThreadFromGlobalPool` passes its callable through `std::function<void()>`
(`startThreadFromGlobalPool` in `src/Common/ThreadPool.h`), so **the captured pin must be
copy-constructible**. A move-only RAII guard does not compile here, and specifying one would have sent
the implementer into a wall.

The pin is therefore a copyable, single-release guard: `Pool::pin_owner` is renamed
`Pool::try_pin_detached_work` and returns either an empty `shared_ptr` — meaning stopping has begun
and the dispatch must not be made — or a `PoolPtr`-shaped guard whose control block owns a strong
`Pool` reference and, when its **last** copy is destroyed, decrements the in-flight count and notifies
the waiter exactly once. An aliasing `shared_ptr` gives this directly: the stored pointer is the
`Pool`, the owned block is the token that releases both.

One object then does both jobs — it accounts for the work and keeps the `Pool` alive — so the
publisher's raw `this` capture, which is a `Pool` subobject, stays valid without a second,
separately-reasoned reference.

The pin is taken **before** the thread is created, because the `ThreadFromGlobalPool` constructor can
throw on pool exhaustion; the guard then releases the count on unwind with no hand-written path.

### Two counters, not one {#counters}

The pin releases the **new** registry count and nothing else. The publisher's existing per-runtime
accounting stays exactly as it is: on the dispatch-failure path, `pending_snapshot_publishes` must
still be decremented and `publish_settle_cv` notified, as `dispatchSnapshotPublisher`'s current
`catch` does.

The two counters answer different questions — "may this runtime be dropped or its cache trimmed"
versus "does any detached work still hold the pool" — and different callers wait on them. The
diagnostic dispatch has no such existing accounting at all and gains only the pin.

### Where the stop is observed {#stop-checkpoints}

A stop checked only at the entry point stops nothing, because the publisher re-admits itself:
`CasRefLedger::settleSnapshotPublish` decrements the in-flight count and, under the same lock, calls
`admitSnapshotPublishUnderStateLock` to re-fire the accumulated tail, dispatching a fresh publisher
when it succeeds. Left alone, that chain outruns a bounded wait and makes expiry the normal outcome
rather than the anomaly.

The stopping flag is therefore observed at four points:

| Point | Effect |
|---|---|
| `Pool::try_pin_detached_work` | the dispatch is not made |
| `runtime_still_admitted`, inside the publisher | an in-flight publish stops at its next checkpoint |
| before the `GET` in `Pool::reportImpossibleInterference` | the diagnostic is abandoned |
| `CasRefLedger::settleSnapshotPublish`, before re-dispatch | the self-re-admission chain terminates |

A backend request already in flight is **not** interrupted. That is deliberate, and it is what the
bounded deadline is for: the wait covers one outstanding attempt resolving, not a retry cycle. The
deadline is the one its neighbour `CasRefLedger::drainRefLanesForShutdown` already uses,
`attempt_timeout_ms + lease_safety_margin_ms`.

### On expiry {#expiry}

Log a warning, increment a counter, proceed. Nothing is forced or cancelled harder.

This is not a fallback hiding a defect: no mechanism can make the guarantee absolute (see
[the guarantee](#guarantee)), so the design states what expiry means and makes it observable instead
of pretending it cannot happen. The counter turns "this occasionally goes wrong" into a number an
operator can watch, and the stop checkpoints are what keep it at zero in normal operation.

## Emission that survives a reset `Context` {#context}

### The member becomes an optional weak reference {#context-member}

A bare `std::weak_ptr<const Context>` cannot distinguish two states that must not be conflated:
**no context was ever supplied** — a legitimate configuration in which the log integration is simply
off, which is what today's nullness checks in `startup` already encode — and **a supplied context has
since been released**, which during `startup` would be a real defect.

The member is therefore `std::optional<std::weak_ptr<const Context>>`:

- `nullopt` means the integration is deliberately off. `startup` reads it exactly where it reads the
  null check today, and takes the same disabled path.
- An engaged-but-expired reference during `startup` fails startup with an exception. It must not
  silently degrade into the disabled path: that would be a fallback converting a lifetime bug into a
  missing feature.
- Each sink resolves the reference per event; an expired one drops the event and counts it.

This also restores the invariant asserted in `Server.cpp`: with no long-lived strong reference left in
`src/Disks`, `global_context.reset()` destroys the `Context` again.

### The accessors stop dereferencing a null `shared` {#context-accessors}

`Context::getContentAddressedLog` and `Context::getContentAddressedGarbageCollectionLog` were added by
CAS work in `c48a45a4719` and do not exist on `master`, so this is a fix to CAS-owned code that
happens to live in a shared file — not a change to an upstream contract.

The template is `Context::getZooKeeperLog`, **not** a bare ternary:

```cpp
std::lock_guard lock(mutex_shared_context);
if (!shared)
    return {};
SharedLockGuard lock2(shared->mutex);
if (!shared->system_logs)
    return {};
return shared->system_logs-><the log>;
```

`mutex_shared_context` is held from the `shared` test through the nested `shared->mutex` and the
`shared_ptr` copy. A `shared ? … : nullptr` that released the mutex before dereferencing would leave
exactly the race it appears to remove.

With the sinks weak, no CAS path reaches these accessors on a released context. Fixing them anyway
means no future caller can crash on one, which is a different and durable property.

### Three distinct outcomes, only two of them counted {#context-outcomes}

These must not be collapsed, and rev.1 collapsed them:

| State | What the sink sees | Counted? |
|---|---|---|
| the weak reference has expired — the `Context` is gone | `lock` returns null | yes, as a lost event |
| `resetSharedContext` has run but the `Context` is still referenced | `lock` succeeds; the accessor returns an empty pointer | no — see below |
| no system log is configured at all | the accessor returns an empty pointer | no; this is ordinary steady-state |

The second and third states are indistinguishable at the sink — both are an accessor returning
nothing — and the third is the normal case for any server that has not enabled the CAS logs. Counting
them would make the counter fire constantly and mean nothing. Only a genuinely expired weak reference
is counted, and the test for the reset-context path asserts a safe skip rather than a counter.

## A destructor boundary that cannot terminate {#failsoft}

### Why it is in scope {#failsoft-scope}

The item text names two halves; triage recommended a third. It belongs here because the drain
**increases** its exposure: today `~Pool` runs late and only sometimes on the shutdown thread, and
afterwards explicit teardown becomes the usual place it runs. Destructors are `noexcept` by default,
so an exception from any phase is `std::terminate`. Fixing the timing without fixing the throw would
raise the frequency of the crash it is meant to remove.

### All three phases, and `drained` defaulting to false {#failsoft-phases}

`~Pool` has three phases, and only the keeper's `release` inside `CasMountRuntime::finishTeardown` is
guarded today:

1. `mount_runtime.stopBackgroundWorkers()`
2. the ref-lane drain, from which `drained` is computed together with `writerCleanupDutiesPending`
3. `mount_runtime.finishTeardown(drained)` — which calls `stopBackgroundWorkers` **again**, the
   belt-and-suspenders re-join, so guarding only phase 1 leaves that same call unguarded on its second
   invocation

Each phase gets its own guard, and `drained` is initialized to `false` before phase 2 rather than
assigned from it.

That default is load-bearing, not a detail. `finishTeardown(true)` writes the clean-release marker,
which a successor reads as proof that no in-flight conditional `PUT` from this incarnation can still
land; the source calls writing it without an actual drain a protocol-safety bug. A guard that
swallowed a throw from phase 2 while leaving `drained` at whatever a partial computation produced
could forge that proof. With the default, a failed drain takes the fail-closed arm: no marker, and a
logged warning that the next mount must treat this end as unclean. A throw from phase 2 must therefore
still reach phase 3 — a failed drain is precisely when the terminal bookkeeping matters most.

The metadata-storage destructor is subject to the same rule: it calls the teardown helper, and no
exception may escape it. A destructor that throws while draining merely relocates `std::terminate` one
level up.

## Observability {#observability}

Two new counters, named in the CAS convention already used by their neighbours:

| Counter | Incremented when | Read as |
|---|---|---|
| `CASDetachedWorkDrainTimeouts` | the bounded stop-and-drain expires with work still in flight | the guarantee in [the guarantee](#guarantee) did not hold for this teardown; expected to stay at zero |
| `CASEventDroppedContextReleased` | a sink resolves an **expired** weak context | events lost because the context outlived nothing; see [the outcome table](#context-outcomes) |

Two log lines, both at warning level and both on the teardown path, so an operator reading a shutdown
log sees the same facts the counters carry: one when the drain expires, naming the number of
dispatches still in flight, and one — already present — when `finishTeardown` skips the clean-release
marker.

## Testing {#testing}

No test in this set may use a sleep to order threads: each uses a condition variable or a barrier, or
a bounded wait whose expiry is itself the assertion.

**The drain — failing-first.** Tracked detached work is held in flight; `shutdown` must not return
while it is. This is the only formulation that does not race the work's completion, and today
`shutdown` returns immediately, which is the defect.

**The stop closes the re-admission chain — failing-first.** A runtime whose settlement keeps
re-admitting: with the stop observed only at the entry point the bounded wait expires; with it
observed at settlement the chain terminates. Assert `CASDetachedWorkDrainTimeouts` is zero — the
counter is the assertion, not the wall clock.

**The emit under a reset `Context` — failing-first.** "The context was destroyed before the emit" is
**not** reachable today: the strong sink is exactly what prevents that destruction. The red scenario
is `resetSharedContext` on a `Context` that is still alive and still referenced — what `Server.cpp`
does — followed by an emit. Today that dereferences a null `shared`; afterwards the event is skipped
safely. Assert the skip, **not** a counter: per
[the outcome table](#context-outcomes), this path is not counted.

**An expired weak context is counted.** A separate test, for the separate claim: release the context
entirely, emit, assert `CASEventDroppedContextReleased` advanced.

**The member no longer extends the context's lifetime.** A separate test again: hold the metadata
storage, release every other reference to the `Context`, assert it was destroyed. This is what pins
the `Server.cpp` invariant.

**An expired context supplied at `startup` fails startup.** Asserts the distinction in
[the member](#context-member) — that an expired supplied reference is an error, not the disabled path.

**The destructor boundary — as a death test.** A `~Pool` whose phase 2 throws terminates the process
today, so the pre-change behaviour can only be observed in a subprocess; the check belongs in a
death test rather than an in-process assertion, which would take the whole binary with it and hide
every test behind it. After the change: logged, survived, and `finishTeardown` still ran with
`drained == false`. Assert the **absence of the clean-release marker**, not merely the absence of a
crash — a guard that survived the throw but forged the marker would pass a crash-only assertion.

## Verification gate {#gate}

`unit_tests_dbms --gtest_filter=Cas*:CA*` in a debug build and in an ASan build, plus a server
start/stop cycle on a CAS-backed disk with the CAS logs enabled — the crash this specification removes
is a shutdown-path crash, and no unit test exercises the real `Server.cpp` teardown order.

## Out of scope {#out-of-scope}

- Item 9's B3, the inline `disk(metadata_type='cas', …)` privilege gate.
- The CAS disk-lifecycle leak on `DROP TABLE` — related and separately tracked. This design adds a
  destructor to `ContentAddressedMetadataStorage` but does not change who destroys it, or whether
  anyone does.
- Every durable format, key shape and protocol step.

`~Pool` running under `pointer_mutex` is **not** in this list: see [sequencing](#sequencing), which
closes it as a consequence.
