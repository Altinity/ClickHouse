---
description: 'Design for draining detached CAS background work at shutdown, for event emission that survives a reset Context, and for a destructor boundary that cannot terminate the process'
sidebar_label: 'CAS detached work at shutdown'
sidebar_position: 6
slug: /superpowers/specs/cas-detached-work-shutdown-design
title: 'CAS detached work outliving Context'
doc_type: 'design'
---

# CAS detached work outliving `Context` {#cas-detached-work-shutdown-design}

**Status:** DRAFT for review, rev.1 (2026-08-24).

This specification covers item 10 of `docs/superpowers/cas/final-checks-todo.md`: findings B3 and B4
of the second 2026-08-05 umbrella review, re-verified against `cas-gc-rebuild` on 2026-08-24. It also
covers the third recommendation that triage made and the item text omits — a `~Pool` that cannot
terminate the process — which was brought into scope deliberately, for the reason in
`{#failsoft-scope}`.

Nothing here changes a durable format, a key shape, or a protocol step. It changes when in-process
objects are torn down and what a teardown is allowed to write.

## Decision {#decision}

One coupled change with three parts, none of which is sufficient alone.

1. **Sequencing and draining.** `ContentAddressedMetadataStorage` unpublishes its pool before waiting
   on anything, then stops and bounded-waits the detached CAS dispatches it now tracks. The stop
   closes the whole re-admission chain, not just the entry point, so the wait normally completes
   immediately.
2. **Emission that survives a reset `Context`.** The metadata storage stops holding a strong
   `ContextPtr`, its two event sinks resolve a weak reference per event and drop the event if it has
   expired, and the two CAS-owned accessors in `Interpreters/Context.cpp` stop dereferencing a null
   `shared`.
3. **A destructor boundary that cannot terminate.** Every phase of `~Pool` is individually guarded,
   `drained` defaults to false so a failed drain suppresses the clean-release marker rather than
   forging it, and the new metadata-storage destructor lets no exception escape.

The bounded wait can expire. Parts 2 and 3 are what make expiry survivable, which is why this is one
change rather than three.

## What is wrong today {#defect}

Verified link by link at `cas-gc-rebuild` HEAD on 2026-08-24.

1. **`shutdown` drains only the GC scheduler.**
   `ContentAddressedMetadataStorage::shutdown` (`:879-901`) latches `shutdown_called`, moves the
   scheduler out and resets `part_access` and `cas_store` under `pointer_mutex`, then calls
   `old_scheduler->stop()`. Nothing waits for detached work.
2. **Two detached dispatches hold a strong `Pool` reference and are untracked.**
   `Pool::reportImpossibleInterference` (`Pool/CasPool.cpp:1564-1596`) captures
   `shared_from_this()` and detaches; `CasRefLedger::dispatchSnapshotPublisher`
   (`Pool/CasRefLedger.cpp:3995-4010`) captures `pin_owner()` — the same `shared_from_this` — plus
   raw `this`, and detaches. These are the only two `.detach()` calls in the CAS subtree.
3. **The publisher is a routine path, not an anomaly path.** It is dispatched from ordinary ref
   mutation, so a publisher in flight at shutdown is an everyday state.
4. **`~Pool` is not quiet.** It calls `mount_runtime.stopBackgroundWorkers`, then
   `ref_ledger.drainRefLanesForShutdown`, then `mount_runtime.finishTeardown(drained)`, which on a
   clean drain performs the keeper's terminal release — object-storage I/O plus an unconditional
   `MountRelease` event emit (`Pool/CasServerRoot.cpp:1270-1271`, with the durable farewell write on
   the line after).
5. **The event sink dereferences a `Context` that shutdown has already emptied.**
   `ContentAddressedMetadataStorage` holds `const ContextPtr context` (`:590`) — the only long-lived
   strong `ContextPtr` in `src/Disks`. Both sinks copy it into a lambda (`:487`, `:565`) and call
   `Context::getContentAddressedLog` / `getContentAddressedGarbageCollectionLog`
   (`src/Interpreters/Context.cpp:6336-6350`), which open with `SharedLockGuard lock(shared->mutex)`
   and never test `shared`. `Server.cpp:1644-1649` calls `resetSharedContext`, which nulls `shared`,
   immediately before releasing the context.

So a `~Pool` that runs late enough emits an event through a null `shared` — and does it on the line
*before* the durable farewell write, so the crash also costs that write.

The strong member has a second consequence independent of the crash: while a CAS disk is alive, and
one lives to the end of the process, `global_context.reset()` does not destroy the `Context`, so the
comment at `Server.cpp:1644-1646` — "no one could own shared part of Context" — describes a state
that is not true.

Finally, `ContentAddressedMetadataStorage` has **no destructor**. An instance destroyed without
`shutdown` drops its pool from the implicit member destructor with no drain at all. The item text
does not mention this path.

## Sequencing: unpublish, then wait {#sequencing}

The drain must not run while the pool is still published. `poolAccess` hands out a strong `PoolPtr`
under `pointer_mutex`; if `cas_store` still holds the pool during the wait, a caller can take a fresh
strong reference *after* the wait has succeeded, and the wait proves nothing.

Teardown therefore becomes, in order:

1. Under `pointer_mutex`, move `gc_scheduler`, `part_access` and `cas_store` into local strong
   references and leave the members empty. After this point no new caller can obtain either.
2. Outside `pointer_mutex`: stop the scheduler (it joins its threads); release the local
   `part_access`; call the pool's stop-and-drain through the local `PoolPtr`; release the local
   `PoolPtr`.

Waiting outside `pointer_mutex` is not incidental. `~Pool` under that mutex already blocks snapshot
readers for the length of its teardown; holding it across a bounded network wait as well would make
that materially worse.

### The guarantee, stated exactly {#guarantee}

This design does **not** promise that `~Pool` runs on the shutdown thread. A `PoolPtr` handed out by
`poolAccess` before step 1 can outlive `shutdown` in a caller's frame, and nothing here tracks those.

What a successful drain establishes is narrower and checkable: **no tracked detached work holds a
reference to the `Pool`.** The two dispatches this specification tracks are the ones that hold the
reference for an unbounded time with no one waiting on them; an ordinary caller's `PoolPtr` is scoped
to a call that the rest of shutdown already orders against. Parts 2 and 3 of the decision cover the
residue either way, which is the honest reason they are not optional.

## Tracked dispatch: the pin, the counters, the stop {#tracking}

### The pin {#pin}

`Pool` gains a small registry — an in-flight count, a condition variable, and a stopping flag — and a
RAII pin that **owns a `PoolPtr`**. Owning it is the point: one object then both accounts for the work
and keeps the `Pool` alive for the duration, so the publisher's raw `this` capture (a ledger member,
i.e. a `Pool` subobject) stays valid without a second, separately-reasoned reference.

The pin is taken **before** the thread is created, because the `ThreadFromGlobalPool` constructor can
throw on pool exhaustion. Both sites already unwind that case by hand; with the pin, the unwinding of
the new count is automatic.

Acquiring a pin fails when stopping has begun. A refused pin means the dispatch is simply not made:
after shutdown starts, no new detached work is created.

### Two counters, not one {#counters}

The pin releases the **new** registry count. It does not replace the publisher's existing
per-runtime accounting: `pending_snapshot_publishes` must still be decremented and
`publish_settle_cv` notified on the dispatch-failure path, exactly as
`dispatchSnapshotPublisher`'s current `catch` does (`Pool/CasRefLedger.cpp:4013-4023`). The two
counters answer different questions — "may this runtime be dropped or its cache trimmed" versus "does
any detached work still hold the pool" — and are waited on by different callers.

The diagnostic dispatch has no such existing accounting at all, and gains only the pin.

### Where the stop is observed {#stop-checkpoints}

A stop that is only checked at the entry point does not stop anything, because the publisher
re-admits itself. `settleSnapshotPublish` (`Pool/CasRefLedger.cpp:4021-4041`) decrements the in-flight
count and, under the same lock, calls `admitSnapshotPublishUnderStateLock` to re-fire the accumulated
tail, dispatching a fresh publisher when it succeeds. Left alone, that chain outruns a bounded wait
and makes expiry the normal outcome rather than the anomaly.

The stopping flag is therefore observed at four points:

| Point | Effect |
|---|---|
| pin acquisition | the dispatch is not made |
| `runtime_still_admitted` (`CasRefLedger.cpp:4229-4235`) | an in-flight publish stops at its next checkpoint |
| before the diagnostic `GET` (`CasPool.cpp:1571`) | the diagnostic is abandoned |
| settlement, before re-dispatch (`CasRefLedger.cpp:4039`) | the self-re-admission chain terminates |

A backend request already in flight is **not** interrupted. That is deliberate and it is what the
bounded deadline is for: the wait covers one outstanding attempt resolving, not a retry cycle. The
deadline is the one its neighbour already uses,
`attempt_timeout_ms + lease_safety_margin_ms` (`Pool/CasPool.cpp`, the `drainRefLanesForShutdown`
call).

### On expiry {#expiry}

Log a warning and increment a `ProfileEvent`, then proceed. Nothing is forced or cancelled harder.

This is not a fallback that hides a defect: no mechanism can make the guarantee absolute (see
`{#guarantee}`), so the design states what expiry means and makes it observable rather than pretending
it cannot happen. The counter is what turns "this occasionally goes wrong" into a number an operator
can watch, and the stop checkpoints are what keep that number at zero in normal operation.

### Both teardown paths {#both-paths}

The stop-and-drain lives in a private helper of `ContentAddressedMetadataStorage`, called from
`shutdown` and from a **new destructor**. Without the destructor, an instance destroyed without
`shutdown` — which nothing today prevents — reaches the same hazard by the shorter route of the
implicit member destructor.

The destructor must not let an exception escape (see `{#failsoft}`); a destructor that throws while
draining merely relocates `std::terminate` one level up.

## Emission that survives a reset `Context` {#context}

### The member becomes weak {#context-member}

`const ContextPtr context` becomes a weak reference. Its four uses are compatible: two are nullness
checks during `startup` (`:750`, `:842`), which runs while the context is unambiguously alive and
becomes a `lock` that always succeeds there; two build the sinks (`:485-487`, `:563-565`).

Each sink captures the weak reference and resolves it per event. An expired reference means the event
is dropped and a `ProfileEvent` counting events lost to an already-released context is incremented —
a silent drop here would be indistinguishable from a working log.

This also restores the invariant asserted at `Server.cpp:1644-1646`: with no long-lived strong
reference left in `src/Disks`, `global_context.reset()` destroys the `Context` again.

### The accessors stop dereferencing a null `shared` {#context-accessors}

`Context::getContentAddressedLog` and `Context::getContentAddressedGarbageCollectionLog` were added by
CAS work (`c48a45a4719`) and do not exist on `master`, so this is a fix to CAS-owned code that happens
to live in a shared file — not a change to an upstream contract.

The template is `Context::getZooKeeperLog` (`src/Interpreters/Context.cpp:6435-6447`), **not** a bare
ternary:

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
`shared_ptr` copy. A `shared ? … : nullptr` that drops the mutex before dereferencing would leave
exactly the race it appears to remove.

With the sinks weak, no CAS path reaches these accessors on a released context. Fixing them anyway
means no future caller can crash on one, which is a different and durable property.

## A destructor boundary that cannot terminate {#failsoft}

### Why it is in scope {#failsoft-scope}

The item text names two halves. Triage recommended a third — a fail-soft `~Pool` — and it belongs
here because the drain **increases** its exposure: today `~Pool` runs late and rarely on the shutdown
thread, and after this change it runs there on every server shutdown. Destructors are `noexcept` by
default, so an exception from any phase is `std::terminate`. Fixing the timing without fixing the
throw would raise the frequency of the crash it is meant to remove.

### All three phases, and `drained` defaulting to false {#failsoft-phases}

`~Pool` (`Pool/CasPool.cpp`) has three phases, and only the keeper's `release` inside
`CasMountRuntime::finishTeardown` is guarded today:

1. `mount_runtime.stopBackgroundWorkers()`
2. `const bool ref_lanes_drained = ref_ledger.drainRefLanesForShutdown(…)`, then
   `drained = ref_lanes_drained && !writerCleanupDutiesPending()`
3. `mount_runtime.finishTeardown(drained)` — which calls `stopBackgroundWorkers` **again**, the
   belt-and-suspenders re-join, so guarding only phase 1 leaves the same call unguarded on its second
   invocation.

Each phase gets its own guard, and `drained` is initialized to `false` before phase 2 rather than
assigned from it.

That default is the load-bearing part, not a detail. `finishTeardown(true)` writes the clean-release
marker, which a successor reads as proof that no in-flight conditional `PUT` from this incarnation can
still land — the source calls writing it without an actual drain a protocol-safety bug. A guard that
swallowed a throw from phase 2 while leaving `drained` at whatever a partial computation produced
could forge that proof. With `drained` defaulting to false, a failed drain takes the fail-closed arm:
`finishTeardown(false)` skips the marker and logs that the next mount must treat this end as unclean.
A throw from phase 2 must therefore still reach phase 3 — the drain failing is precisely when the
terminal bookkeeping matters most.

## Testing {#testing}

**The drain — failing-first.** A pool with tracked detached work held in flight: `shutdown` must not
return while that work is in flight. Same formulation as elsewhere in this codebase, because it is the
only one that does not race the work's completion: today `shutdown` returns immediately, which is the
defect.

**The stop closes the re-admission chain — failing-first.** A runtime whose settlement keeps
re-admitting: with the stop observed only at the entry point, the bounded wait expires; with it
observed at settlement, the chain terminates and the wait completes. Assert the expiry `ProfileEvent`
is zero.

**The emit — failing-first, and the scenario matters.** "The context was destroyed before the emit"
is **not** reachable today: the strong sink is exactly what prevents that destruction. The red
scenario is `resetSharedContext` on a `Context` that is still alive and still referenced — which is
what `Server.cpp` does — followed by an emit. Today that dereferences a null `shared`; after the
change the event is dropped and counted.

**The member no longer extends the context's lifetime.** A separate test, because it is a separate
claim: hold the metadata storage, release every other reference to the `Context`, and assert it was
destroyed. This is the assertion that pins the `Server.cpp:1644-1646` invariant.

**The destructor boundary.** `~Pool` whose phase 2 throws: today `std::terminate`; after the change,
logged and survived — **and** `finishTeardown` still runs, with `drained == false`, so no clean-release
marker is written. Assert the absence of the marker, not merely the absence of a crash: a guard that
survived the throw but forged the marker would pass a crash-only assertion.

## Verification gate {#gate}

`unit_tests_dbms --gtest_filter=Cas*:CA*` in a debug build and in an ASan build, plus a server
start/stop cycle on a CAS-backed disk with the CAS logs enabled — the crash this specification removes
is a shutdown-path crash, and no unit test exercises the real `Server.cpp` teardown order.

## Out of scope {#out-of-scope}

- Item 9's B3, the inline `disk(metadata_type='cas', …)` privilege gate.
- The CAS disk-lifecycle leak on `DROP TABLE` — related, separately tracked, and not fixed here; this
  design adds a destructor to `ContentAddressedMetadataStorage` but does not change who destroys it.
- `~Pool` running under `pointer_mutex` and blocking snapshot readers. The sequencing in
  `{#sequencing}` deliberately does not make it worse, but shortening that hold is its own change.
- Every durable format, key shape and protocol step.
