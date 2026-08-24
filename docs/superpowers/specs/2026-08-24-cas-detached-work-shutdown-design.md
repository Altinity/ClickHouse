---
description: 'Design for draining detached CAS background work at shutdown, for event emission that survives a reset Context, and for a destructor boundary that cannot terminate the process'
sidebar_label: 'CAS detached work at shutdown'
sidebar_position: 6
slug: /superpowers/specs/cas-detached-work-shutdown-design
title: 'CAS detached work outliving Context'
doc_type: 'design'
---

# CAS detached work outliving `Context` {#cas-detached-work-shutdown-design}

**Status:** DRAFT for review, rev.6 (2026-08-24).

This specification covers item 10 of `docs/superpowers/cas/final-checks-todo.md`: findings B3 and B4
of the second 2026-08-05 umbrella review. It also covers the third recommendation triage made and the
item text omits — a `~Pool` that cannot terminate the process — brought into scope deliberately, for
the reason in [why it is in scope](#failsoft-scope).

**Statements about the current code were checked against `cas-gc-rebuild` at `57241b3f20b`.**
Functions are named rather than cited by line: line numbers in this subtree move weekly, and a stale
one reads as authoritative.

Nothing here changes a durable format, a key shape, or a protocol step. It changes when in-process
objects are torn down and what a teardown is allowed to write.

## Decision {#decision}

One coupled change with three parts, none of which is sufficient alone.

1. **Sequencing and draining.** `ContentAddressedMetadataStorage` unpublishes its pool before waiting
   on anything, then stops and bounded-waits the detached CAS dispatches, which now go through a
   single tracked entry point on `Pool`. The stop closes the whole re-admission chain — including the
   recovery loop that sits below every other checkpoint — so the wait normally completes immediately.
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

The strong member has a second consequence independent of the crash. `Context::shared` is a raw
`ContextSharedPart *`, so a strong `ContextPtr` does not own the shared part and the neighbouring
comment about who owns it is not what this violates. What it defeats is the surrounding **intent** at
that call site — "Explicitly destroy Context": while a CAS disk is alive, and one lives to the end of
the process, `global_context.reset()` leaves the `Context` object itself alive, past
`resetSharedContext` and past the point the shutdown sequence expects it to be gone.

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
   references and leave the members empty. After this point no new caller can obtain any of them.
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
reference for an unbounded time with no one waiting on them. An ordinary caller's `PoolPtr` is scoped
to one operation and is expected to be short-lived, but nothing here drains those and nothing orders
them against `shutdown` — they are explicitly outside the guarantee rather than covered by it.

Explicit teardown accordingly becomes the *usual* place where the last reference is released — not the
guaranteed one. Parts 2 and 3 of the decision cover the exceptions, which is the honest reason they
are not optional.

## Tracked dispatch {#tracking}

### Registry state that outlives the `Pool` {#registry-state}

The in-flight count, its condition variable and the stopping flag live in a `DetachedRegistryState`
held by `shared_ptr`, owned by the `Pool` **and by every task lease** — so it outlives the `Pool`
rather than dying with it.

Each dispatched task holds a lease over that state plus a strong `PoolPtr`. **The release order inside
the lease is load-bearing:**

1. the task body returns;
2. the lease drops its strong `PoolPtr`;
3. **only then** does it decrement the count and notify the waiter.

Without this order the guarantee is false in a way that is easy to miss. If the strong reference lived
in the callable's captures, it would be released when the `std::function` is destroyed — which happens
*after* the body ran and can happen after the count already reached zero. Shutdown would observe zero,
release its own local `PoolPtr`, and `~Pool` would still run on the worker thread. Zero must mean "no
tracked task holds the `Pool`", and that is only true if the reference is released before the count
can reach zero.

### One entry point {#dispatcher}

```cpp
/// Launch one tracked detached task. Refuses after teardown has begun.
bool Pool::tryDispatchDetached(DetachedTask task);
```

It refuses with `false` once stopping has begun; otherwise it builds the lease, increments the count,
launches a `ThreadFromGlobalPool`, and hands the task a **stop token** — a read-only view of the same
registry state. On a launch failure it releases the lease in the order above, logs best-effort under a
nested `try`/`catch(...)`, and returns `false`.

**The contract is that the dispatch step does not fail its caller — not that this method never
throws.** It cannot promise the latter: converting or copying the callable into the by-value
`DetachedTask` argument allocates *in the caller's expression*, before the body is entered, so
`std::bad_alloc` can be raised without this method ever running. The caller-side guard below is
therefore armed **before** the task is constructed, and task construction and the call sit inside one
`try`. An unqualified "never throws" here would be a promise the signature cannot keep.

`CasRefLedger`'s injected `pin_owner` callback is replaced by one wired to this method. There is
deliberately no second "is stopping" accessor: every checkpoint that needs one runs inside a
dispatched task and reads its stop token.

### The caller's rollback is local, and only for a dispatch that never happened {#rollback}

The publisher's single-flight reservation — `pending_snapshot_publishes`, incremented under
`state_mutex` by `admitSnapshotPublishUnderStateLock` — necessarily precedes a fallible launch, so
compensation is unavoidable. It is confined to one scope-local, **non-copyable** guard:

- the reservation is already taken;
- arm the rollback guard;
- construct the task and call `tryDispatchDetached`, both inside one `try`;
- **`true`** → disarm the guard; the launched task settles normally;
- **`false`, or a throw from task construction or the call** → the guard performs
  `decrement pending_snapshot_publishes; notify publish_settle_cv`, and the failure is logged
  best-effort and swallowed.

Because the guard never enters the `std::function`, it is not subject to the copy-constructibility
that `startThreadFromGlobalPool`'s `std::function<void()>` imposes on everything a task captures.

**Normal settlement is untouched, and that is the point.** `settleSnapshotPublish` decrements and
re-admits under a *single* `state_mutex` hold precisely so that no observer sees a transient zero
between the two; `waitForSnapshotPublishSettle` would otherwise read a false "settled". A guard that
also decremented on some later destruction would either double-decrement a successful publish or, worse,
erase the reservation a re-admission had just taken — false quiescence, then a negative count.

So exactly one path owns each decrement: the guard fires **only** when no task was launched; every
launched task's reservation is retired by settlement alone.

**A failed dispatch must never fail its caller.** `dispatchSnapshotPublisher` is reached from
read-side maintenance and from settlement's own re-dispatch, not only from mutations, and the existing
`catch` states the contract verbatim: a background publish is "a best-effort maintenance trigger and
must never fail an otherwise-successful read or mutation". On the re-dispatch path a propagating
exception would additionally escape inside a detached thread.

The diagnostic dispatch has no reservation of its own and needs no guard.

### Where the stop is observed {#stop-checkpoints}

A stop checked only at the entry point stops nothing: `settleSnapshotPublish` re-admits and dispatches
a successor, and that chain outruns a bounded wait.

| Point | Reads | Effect |
|---|---|---|
| `Pool::tryDispatchDetached` | the registry's stopping flag | no dispatch; the caller's guard rolls the reservation back |
| `runtime_still_admitted`, inside the publisher | the task's stop token | an in-flight publish stops at its next checkpoint |
| before the `GET` in `Pool::reportImpossibleInterference` | the task's stop token | the diagnostic is abandoned |
| `settleSnapshotPublish`, before re-dispatch | the task's stop token | the self-re-admission chain terminates |
| inside `ensureRefTableRecovered` — its I/O checkpoints **and its retry sleep** | the task's stop token | recovery unparks instead of running out its backoff |

### The stop token reaches into recovery — and shutdown never touches remount's latch {#recovery-cancellation}

`tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntime` calls `ensureRefTableRecovered` *before* it
constructs `runtime_still_admitted`, so a publisher whose runtime entered `NeedsRecovery` sits below
every other checkpoint. That loop retries transient failures until `recovery_retry_budget_ms` — 120
seconds by default — is spent.

The obvious move, reusing the existing per-runtime `recovery_cancel_requested`, is **wrong**, and both
reasons are structural:

- `cancelRecoveriesAndAwaitQuiescence` waits on `recovery_cv` with no deadline and then
  **unconditionally clears** that flag. The remount worker is still running during the detached drain,
  so a concurrent remount would clear a cancellation shutdown had just latched — a lost cancellation
  with no trace.
- Setting the flag would not unpark a publisher that is *sleeping* anyway: `recovery_retry_sleep_fn`
  slices its sleep into 200 ms chunks and exits early **only** when `fence_ok_fn` goes false, and a
  terminal shutdown does not drop the mount fence. With `recovery_retry_max_backoff_ms` at 30 seconds
  against a drain deadline of seconds, the drain would expire while the publisher slept — defeating
  precisely the test that asserts a zero timeout.

The stop token removes both problems by not sharing a mechanism at all. It is threaded from the
dispatcher into the publisher and onward into `ensureRefTableRecovered`, which checks it at its
existing I/O and retry checkpoints **and in the slice loop of its retry sleep** — the loop is already
built to be interruptible, as its own comment says; it simply polls the wrong predicate for this case.
Shutdown therefore never modifies `recovery_cancel_requested`, self-remount keeps it exactly as it is,
and there is no latch to lose.

### What the deadline does and does not bound {#deadline}

A request already in flight is not interrupted, and what that costs differs by path:

- the publisher's **writes** go through the CAS request budget, so an outstanding attempt is bounded by
  it, and the drain deadline — `attempt_timeout_ms + lease_safety_margin_ms`, the one
  `CasRefLedger::drainRefLanesForShutdown` already uses — covers one such attempt resolving;
- **recovery reads** are direct backend `LIST`/`GET` calls, not request-controller operations, so the
  budget does not bound them. The stop token is what ends the retry loop and its backoff; the deadline
  is what stops shutdown waiting;
- the diagnostic path issues a raw backend `GET` that is likewise not budget-controlled. For it the
  deadline is insurance, not a derived bound: it caps how long shutdown waits, but nothing caps the
  `GET`.

### On expiry {#expiry}

Log a warning, increment a counter, proceed. Nothing is forced or cancelled harder.

This is not a fallback hiding a defect: this bounded design deliberately does not make the guarantee
absolute (see [the guarantee](#guarantee)), so it states what expiry means and makes it observable
instead of pretending it cannot happen. The counter turns "this occasionally goes wrong" into a number an
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

**`startup` resolves the reference exactly once, and holds the result.** The member is consulted at
two separate points today, and resolving it twice would let the context expire between them, so that
`background_watermark` is configured one way and the GC scheduler decided another — a partially
disabled mount, which is worse than either outcome and would be invisible. The sequence is therefore:

1. at the top of `startup`, if the member is engaged, `lock` it once;
2. if that fails, throw immediately — before `Pool::open`, so nothing has been mounted;
3. hold the resulting strong `ContextPtr` as a local through the single publish step, and derive
   **both** decisions from that one local;
4. release it when `startup` returns.

The sinks capture the **weak** reference, not this local: the local exists only to make `startup`
internally consistent, and a sink holding a strong reference is the original defect.

This also lets the shutdown sequence do what it says it does: with no long-lived strong reference left
in `src/Disks`, `global_context.reset()` destroys the `Context` object rather than merely dropping one
of several references to it.

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

These three must not be collapsed:

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

The item text names two halves; triage recommended a third. It belongs here because this design
deliberately moves `~Pool` into managed shutdown: it runs once either way, but afterwards it runs
where the rest of the shutdown sequence can be reasoned about, and a drain timeout still leaves the
old late path available. Destructors are `noexcept` by default, so an exception from any phase is
`std::terminate` — on the very path this design is making routine and predictable. Relocating a
throwing teardown without guarding it would be moving a crash, not fixing one.

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

**The guards must guard their own logging.** `tryLogCurrentException` is not `noexcept` and allocates
— it builds a `String` and calls `getLogger`, as its own comment in `src/Common/Exception.cpp` notes.
Under memory pressure, which is one of the conditions likeliest to make a teardown phase throw in the
first place, the handler can throw on the way to reporting the throw. Every `catch` on this boundary
therefore wraps its logging in a nested `try`/`catch(...)` that does nothing, as other `noexcept` CAS
paths already do. A promise that nothing escapes is worth exactly as much as its least-guarded line.

The metadata-storage destructor is subject to the same rule: it calls the teardown helper, and no
exception may escape it. A destructor that throws while draining merely relocates `std::terminate` one
level up.

## Observability {#observability}

Two new counters, named in the CAS convention already used by their neighbours:

| Counter | Incremented when | Read as |
|---|---|---|
| `CASDetachedWorkDrainTimeouts` | the bounded stop-and-drain expires with work still in flight | the guarantee in [the guarantee](#guarantee) could not be **established** for this teardown — it is conditional on a successful drain, so a timeout means unknown, not violated; expected to stay at zero |
| `CASEventDroppedContextExpired` | a sink resolves an **expired** weak context | events lost because the context was already gone; see [the outcome table](#context-outcomes) |

The two counters are not symmetric in what else they produce, and it is worth being precise about
that rather than implying a tidy pairing:

- the drain timeout also emits a warning naming how many dispatches were still in flight, so a
  shutdown log carries the same fact as the counter;
- an expired context emits **no** log line. By then the logging machinery is exactly what has gone
  away, and a counter is the only thing that can still record the loss;
- the existing warning about skipping the clean-release marker belongs to a **different** condition —
  an undrained ref lane in `~Pool` — and is neither of these. It is named here only so that a reader
  correlating a shutdown log does not attribute it to the drain.

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

**A zero in-flight count means the `Pool` reference is gone — failing-first if the order is wrong.**
The property from [registry state](#registry-state), asserted directly: a task whose body has returned
but whose lease has not released must not let the count reach zero. Observe the `Pool`'s use count (or
a weak reference to it) at the moment the drain returns and assert no tracked task still holds it. A
lease that decremented before releasing its reference passes every other test in this list and fails
this one.

**The stop token unparks a publisher inside recovery — failing-first.** The critical new
branch, and the one nothing else covers: drive a publisher into `ensureRefTableRecovered` — that is,
past the point where its runtime entered `NeedsRecovery` and before `runtime_still_admitted` exists —
then begin shutdown. Drive the case where it is **sleeping in backoff**, not merely between attempts:
that is the one a fence-only predicate misses, and with `recovery_retry_max_backoff_ms` at 30 seconds
it is also the one that blows a deadline measured in seconds. With the token observed in the retry
sleep, recovery unparks and the drain completes inside its deadline; without it, the drain times out.
Assert `CASDetachedWorkDrainTimeouts` is zero and that the drain returned well inside its deadline.

**The emit under a reset `Context` — failing-first, and in a subprocess.** "The context was destroyed
before the emit" is **not** reachable today: the strong sink is exactly what prevents that
destruction. The red scenario is `resetSharedContext` on a `Context` that is still alive and still
referenced — what `Server.cpp` does — followed by an emit.

Pre-change that dereferences a null `shared` and takes the whole test binary down, so it cannot be an
ordinary in-process failing-first test: running the red would hide every test behind it. Use the same
subprocess form as the destructor test below — `EXPECT_EXIT` with a predicate for a clean exit —
asserting that after the change the emit is **skipped safely** and the subprocess exits normally.
Assert the skip, **not** a counter: per [the outcome table](#context-outcomes), this path is not
counted.

**An expired weak context is counted.** A separate test, for the separate claim: release the context
entirely, emit, assert `CASEventDroppedContextExpired` advanced.

**The member no longer extends the context's lifetime.** A separate test again: hold the metadata
storage, release every other reference to the `Context`, assert it was destroyed. This pins the
property `Server.cpp` relies on when it explicitly destroys the context — not an invariant about who
owns the shared part, which a strong `ContextPtr` never did.

**An expired context supplied at `startup` fails startup.** Asserts the distinction in
[the member](#context-member) — that an expired supplied reference is an error, not the disabled path.

**The destructor boundary — a subprocess exit test, not a death test.** A `~Pool` whose phase 2
throws terminates the process today, so this must run in a subprocess: an in-process assertion would
take the whole binary with it and hide every test behind it. But the assertion after the change is
that the subprocess exits **normally**, which is not what `EXPECT_DEATH` expresses — it expects
abnormal termination. Use `EXPECT_EXIT` with a predicate for a clean exit code, so the test states
the post-change expectation directly instead of asserting the absence of one particular death.

Inside that subprocess, assert the **absence of the clean-release marker** as well as the clean exit:
a guard that survived the throw but forged the marker would pass an exit-code-only assertion, and
forging it is the worse of the two failures.

**The implicit destruction path.** `ContentAddressedMetadataStorage` destroyed **without** `shutdown`
having been called, with tracked detached work in flight: the drain must still happen. This is a named
defect in [what is wrong today](#defect) and the reason the destructor is added at all, so it needs its
own test — otherwise an implementation that fixes only `shutdown` passes everything else in this list.

**Helper idempotence.** The same storage destroyed **after** an explicit `shutdown`: the second
teardown must find nothing to do rather than repeat the wait. Without this, an implementation that
drains twice — or that waits a second full deadline on a pool that is already gone — is not caught by
anything above.

## Verification gate {#gate}

`unit_tests_dbms --gtest_filter='CAS*'` in a debug build and in an ASan build, plus a server
start/stop cycle on a CAS-backed disk with the CAS logs enabled — the crash this specification removes
is a shutdown-path crash, and no unit test exercises the real `Server.cpp` teardown order.

The filter is exactly `CAS*` — the strict form the CAS naming policy requires. Every suite this
specification adds, death-test and exit-test variants included, must therefore be named `CAS…`.
Widening the filter to `Cas*` or `Ca*` drags in foreign suites such as `CascadeWriteBuffer` and, worse,
masks a misnamed CAS suite instead of catching it: a suite that escapes `CAS*` escapes the gate.

## Out of scope {#out-of-scope}

- Item 9's B3, the inline `disk(metadata_type='cas', …)` privilege gate.
- The CAS disk-lifecycle leak on `DROP TABLE` — related and separately tracked. This design adds a
  destructor to `ContentAddressedMetadataStorage` but does not change who destroys it, or whether
  anyone does.
- Every durable format, key shape and protocol step.

`~Pool` running under `pointer_mutex` is **not** in this list: see [sequencing](#sequencing), which
closes it as a consequence.
