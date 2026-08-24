---
description: 'Design for CAS GC meta-pool job ownership and for the mount-claim error classification, closing the two untracked P1 findings B1 and B2a'
sidebar_label: 'CAS GC meta-job ownership'
sidebar_position: 5
slug: /superpowers/specs/cas-gc-meta-job-ownership-design
title: 'CAS GC meta-job ownership and mount-claim error classification'
doc_type: 'design'
---

# CAS GC meta-job ownership and mount-claim error classification {#cas-gc-meta-job-ownership-design}

**Status:** DRAFT for review, rev.2 (2026-08-24).

This specification covers item 9 of `docs/superpowers/cas/final-checks-todo.md` minus its third
bullet: findings **B1** (`~Gc` versus the `meta_pool` drain) and **B2a**
(`MountLeaseKeeper::claim` throwing `LOGICAL_ERROR`) from the 2026-08-05 umbrella review, both
re-verified against `cas-gc-rebuild` on 2026-08-24. The third bullet, B3 (an inline
`disk(metadata_type='cas', …)` bypassing the `SYSTEM CAS` privilege model), is explicitly **out of
scope**: it is a privilege-model decision with cross-tenant consequences and needs its own design.

There is no persisted-data compatibility question anywhere in this document. Nothing here changes a
durable format, a key shape, or a protocol step.

**What rev.2 changed.** Review found that rev.1's ownership boundary was not enforceable and that its
B2a coverage was nominal. Both are corrected here: the generic job-accepting API is removed
altogether rather than left in place beside a shared state object (`{#b1-fix}`), and all six changed
`claim` branches plus both fenced-precedence branches get direct tests driven by an explicit backend
race seam (`{#b2a-testing}`). rev.1's claim that all six branches mean a change *between two
observations* was also wrong and is corrected in `{#b2a-defect}`.

## Decision {#decision}

Two independent changes, landing as two commits on the existing `cas-gc-rebuild` branch.

1. **B1.** The bounded meta pool and everything its jobs touch move into one dedicated owner that
   exposes **typed operations** and accepts no caller-supplied callbacks. `Gc` loses the ability to
   put an arbitrary closure on that pool, so a job capturing `this` stops being expressible rather
   than merely being absent today. Separately, the round drains the pool on **every** exit path, not
   only on the success path.
2. **B2a.** Every `LOGICAL_ERROR` in `MountLeaseKeeper::claim` becomes `ABORTED`, matching the
   sibling renewal path. Each of the six branches, and both `MountFencedException` branches that must
   keep taking precedence over them, gets a direct test.

## B1 — the meta-pool job boundary {#b1}

### What is wrong today {#b1-defect}

`Gc` owns a bounded `ThreadPool` (`meta_pool`, `Gc/CasGc.h:950`) for the round's per-hash
freshness-meta writes. Two sites produce jobs for it, and **both capture `this`**:

- `Gc/CasGc.cpp:433` — the condemn-marker write: `[this, ref, token, condemn_round, size]`, which
  calls `writeCondemnedMeta(*store, …)` and then `noteCondemnMarkerDurable(…)`.
- `Gc/CasGc.cpp:867` — the delete confirmation:
  `[this, ref]() { deleteConfirmedMeta(store->backend(), store->layout(), ref); }`.

Capturing `this` makes a job depend on the whole object. What the jobs actually reach is `store`
(`Gc/CasGc.h:896`), the two job counters (`meta_jobs_scheduled_`, `meta_jobs_completed_`, `:956-957`)
and the condemn-marker confirmation registry (`condemn_marker_mutex` + `condemn_markers_confirmed`,
`:967-968`). `store` is declared *before* `meta_pool` and is therefore destroyed after
`~ThreadPool` has joined the workers; the counters and the registry are declared *after* it and are
destroyed *before* the join. A job still executing at that moment locks a destroyed mutex and inserts
into a destroyed set.

The code already believes otherwise. `Gc/CasGc.cpp:389` states that the counter capture is safe
because "the pool is a member of the same `Gc` and is joined by `~Gc` before the atomic dies" — a
guarantee that no code implements, since no `~Gc` exists. The comment is not a stale detail; it is
the exact argument this design replaces.

rev.8 made `Gc` destruction routine (`UNMOUNT`, `SYSTEM CAS GC STOP`) rather than a shutdown-only
event, so the window is wider than when the code was written, not narrower.

### Why an explicit destructor is not the fix {#b1-why-not-ordering}

An explicit `~Gc` that drains `meta_pool` first (or moving the `meta_pool` declaration last) does fix
today's instance. It does not fix the class, and it cannot be tested:

- **No test can fail today.** `~ThreadPool` (`src/Common/ThreadPool.cpp:562-587`) sets `shutdown`
  and **joins** the worker threads, so the enclosing object's storage is never freed while a job
  runs. The members are destructed-but-still-allocated. Address Sanitizer does not report reads of a
  destructed `std::mutex` or `std::set` in still-allocated storage, so a failing-first test for the
  ordering fix does not exist. A plan step claiming to watch such a test go red would be false.
- **The regression is equally invisible.** A later member reordering, or a new job that reaches one
  more member, reopens the gap with no compiler error, no test failure and no sanitizer report — the
  same silence that let the `:389` comment stand for weeks while describing a guarantee that was
  never implemented.

A correctness argument that no mechanism can check is exactly what this codebase has already been
bitten by here. The fix therefore removes the argument instead of restating it — and, per review, it
must remove the *ability to restate it*, which a shared state object beside a callback-accepting
`scheduleMetaJob` would not do: `scheduleMetaJob([this] { … })` would still compile and still be
wrong. The enforcement has to be the absence of that API.

### The fix: a typed owner with no callback surface {#b1-fix}

One class owns the pool and everything the pool's work touches, and exposes only the two operations
the round actually needs:

```cpp
/// Owns the bounded pool for the round's per-hash freshness-meta writes AND everything those writes
/// touch. There is deliberately NO way to hand this class a closure: the only paths onto the pool
/// are the two typed operations below, each of which captures nothing but `shared_ptr<State>`. A job
/// therefore cannot reach anything owned by `Gc`, and no member-declaration order can reopen the
/// lifetime gap that made this class necessary.
class GcMetaWriter
{
public:
    GcMetaWriter(PoolPtr store, LoggerPtr logger, size_t pool_size);

    /// The two operations. Each schedules one bounded-pool job; neither can throw out of the job
    /// (a per-hash meta op is advisory -- the ledger and the exact-token body delete are the
    /// safety core), and neither takes a caller-supplied callable.
    void scheduleCondemnMarkerWrite(const BlobRef & ref, const Token & token,
                                    uint64_t condemn_round, uint64_t size);
    void scheduleConfirmedMetaDelete(const BlobRef & ref);

    /// Wait for every scheduled job. Called twice per round: once as the `meta_pool_wait` protocol
    /// barrier, and once from the round's exit guard.
    void drain();

    uint64_t scheduled() const;
    uint64_t completed() const;

    /// The condemn-marker confirmation registry, read by the graduation gate on the round thread and
    /// written by both the round thread and pool completions.
    void noteCondemnMarkerDurable(const BlobRef & ref, const Token & token);
    bool condemnMarkerConfirmedInProcess(const BlobRef & ref, const Token & token);
    void forgetCondemnMarker(const BlobRef & ref, const Token & token);

private:
    /// Everything a job reaches, held by `shared_ptr` and captured by value into every job.
    struct State
    {
        PoolPtr store;
        LoggerPtr logger;
        std::atomic<uint64_t> scheduled{0};
        std::atomic<uint64_t> completed{0};
        std::mutex condemn_marker_mutex;
        std::set<std::pair<BlobRef, String>> condemn_markers_confirmed;
    };

    std::shared_ptr<State> state;
    ThreadPool pool;
};
```

`Gc` replaces `meta_pool`, the two counters, the mutex, the set and `scheduleMetaJob` with a single
`GcMetaWriter meta_writer` member. The call sites change mechanically:

| Today | After |
|---|---|
| `Gc::scheduleCondemnMarkerWrite`, `CasGc.cpp:430-440` | forwards to `meta_writer.scheduleCondemnMarkerWrite` |
| `Gc::scheduleMetaJob(…deleteConfirmedMeta…)`, `:867` | `meta_writer.scheduleConfirmedMetaDelete(ref)` |
| `forgetCondemnMarker`, `:870`, `:916`, `:965` | `meta_writer.forgetCondemnMarker(…)` |
| `condemnMarkerConfirmedInProcess`, `:1890` | `meta_writer.condemnMarkerConfirmedInProcess(…)` |
| `noteCondemnMarkerDurable`, `:1896` | `meta_writer.noteCondemnMarkerDurable(…)` |
| `meta_pool->wait()`, `:1030` | `meta_writer.drain()` |
| counter reads at `:511`, `:1026-1034` | `meta_writer.scheduled()` / `meta_writer.completed()` |

`Gc::scheduleMetaJob` is **deleted**, not made private: a private one is still reachable from every
existing member and from the `tests::GcRoundPlanSignatureAccess` friend, which is most of the surface
that matters. After the change, the grep that proves the property is one line — no closure-accepting
entry point onto the pool exists.

The strong `PoolPtr` copy inside `State` is deliberate and is **not** the detached-work problem
tracked as item 10 of `final-checks-todo.md`. These jobs run on a pool whose destructor joins them,
so the reference is bounded by that join; item 10 concerns work handed to `detach` with no join at
all. The distinction is worth stating in the source comment so a reviewer does not read one as the
other.

`~Gc` is **not** added, and `GcMetaWriter` gets no hand-written destructor either. Their absence is
the property being asserted: once the jobs own what they touch, destruction order carries no
correctness weight. The `:389` comment is deleted rather than corrected.

### Round-exit drain {#b1-round-drain}

Independently of the ownership change, `runRegularRound` must drain the pool on every exit path.

`Cas::Gc gc` is a **member of the scheduler** (`Gc/CasGcScheduler.h:190`), not a per-round object, so
rounds share one pool and one registry. Today the only drain is the `meta_pool_wait` phase at
`CasGc.cpp:1030`. A round that throws between scheduling its condemn markers (during the fold) and
that wait leaves its jobs in flight while round N+1 starts. Those jobs then insert into the
confirmation registry that round N+1's graduation gate reads (`CasGc.cpp:1890`), and they land inside
round N+1's counter deltas, which are computed against a baseline sampled at its own start
(`CasGc.cpp:511`).

The round therefore drains under a scope guard covering both the normal and the throwing exit. The
existing `meta_pool_wait` phase stays exactly where it is: it is a protocol barrier — this round's
condemns must be durable no later than the ledger they are paired with — not cleanup, and moving it
would change when the round publishes.

### Recorded, not changed: dropped queued jobs {#b1-dropped-jobs}

`~ThreadPool` joins the workers but does **not** run jobs still queued: `finalize` sets `shutdown`
under the lock and wakes idle threads, which then exit (`src/Common/ThreadPool.cpp:562-587`).
Destroying a `Gc` with a backlog therefore silently discards those meta writes.

This is safe and stays as it is. A discarded condemn-marker write leaves its `(ref, token)`
unconfirmed, and the graduation gate then carries the entry and retries the write on a later round —
the same fail-safe delay a lost CAS or a thrown job already takes (`CasGc.cpp:435-437`). It is
specified here only because it was nowhere recorded, and because "the pool is drained on
destruction" is the natural and wrong thing for a reader to assume.

### Testing {#b1-testing}

- **Round-exit drain — failing-first, deterministic.** A round is made to throw after its condemn
  markers are scheduled; after the throw propagates, the round's scheduled and completed job counts
  are equal. Red today, green after the scope guard.
- **Both real jobs, in flight across `Gc` destruction.** Per review, the synthetic job of rev.1 would
  have proven nothing about the production closures, so the test drives the real ones. A backend seam
  blocks *inside* the meta write (a latch in the `.meta`-key branch, extending the existing
  `MetaWriteFaultBackend` pattern in `cas_test_helpers.h:1991`), which makes "the job is in flight"
  an observed fact rather than a timing hope: the test waits until the backend reports it entered,
  destroys the `Gc`, then releases the latch. Once for `scheduleCondemnMarkerWrite` and once for
  `scheduleConfirmedMetaDelete`.
  This test does **not** go red before the change — for the reason given in `{#b1-why-not-ordering}`,
  no sanitizer reports the pre-change behaviour — and the plan must say so rather than stage a false
  red step. Its value is as a pin under ASan and TSan for every later change, and the enforcement of
  the boundary itself is the compile-level one: the API that would allow a bad job no longer exists.
- **Regression guard on the existing premise.** `gtest_cas_gc_ack_floor.cpp:1242-1277` reasons
  explicitly that the confirmation registry "dies with" its `Gc`, so a new `Gc` under the same
  identity starts empty and falls back to a synchronous `loadMeta` re-check. That premise survives —
  `meta_writer` is a member, and a fresh `Gc` gets a fresh one — and the test must be re-run to prove
  it, not assumed.

## B2a — `claim` is not a `LOGICAL_ERROR` site {#b2a}

### What is wrong today {#b2a-defect}

`MountLeaseKeeper::claim` (`Pool/CasServerRoot.cpp:1460-1537`) throws `LOGICAL_ERROR` on six
branches:

| Line | Condition | How it becomes true |
|------|-----------|---------------------|
| `:1468` | the key appeared between our `head` and our `putIfAbsent` | another process minted the slot inside our window |
| `:1479` | the key vanished between our `head` and our `get` | another process removed the slot inside our window |
| `:1489` | the slot is held by a foreign server | may already hold at our first `head` |
| `:1499` | the slot is held by a different `writer_epoch` | may already hold at our first `head` |
| `:1525` | the slot changed while we were adopting our own | another process wrote inside our adoption window |
| `:1530` | the slot vanished while we were adopting our own | another process removed it inside our adoption window |

Constructing a `DB::Exception` with `LOGICAL_ERROR` calls `handle_error_code`
(`src/Common/Exception.cpp`), which aborts the process on any `DEBUG_OR_SANITIZER_BUILD`. `claim` is
reached from `MountLeaseKeeper::start`, which the background self-remount path calls
(`Pool/CasPool.cpp:1233`) — so these are aborts on a background thread, on exactly the debug and
sanitizer lanes that certify the mount feature.

The classification is also simply wrong. Every one of the six describes an **externally reachable
conflict**: the mount object holds a state produced by another process. Four of them arise from a
change inside one of our own windows; the foreign-server and different-epoch cases may equally well
already be true at our first observation. Either way, none reports a violated invariant of ours. The
sibling renewal path already says so: the same conditions there raise `ABORTED` (`:1594`, `:1604`,
`:1618`).

### The fix {#b2a-fix}

All six become `ABORTED`. Message texts are preserved verbatim — the existing tests match on
substrings of them.

The two `gc_fenced` branches in the same function (`:1508`, `:1521`) already raise
`MountFencedException` and are **not** touched: they are caught by type at
`Pool/CasPool.cpp:709`, and that catch is load-bearing — it is what turns a GC fence during mount
into a bounded epoch-bump retry instead of a failed mount. Their **precedence** over the reclassified
branches is part of the contract, not an accident of ordering: `:1508` is tested before the
adoption `putOverwrite`, and `:1521` is tested before `:1525` inside the conflict resolution.

### Call-site audit {#b2a-audit}

Changing a thrown code requires checking where each throw lands, since nothing in the compiler will.
`claim` has exactly one caller, `start` (`:1548`), which in turn has exactly one caller,
`CasMountRuntime::startKeeper` (`Pool/CasMountRuntime.cpp:394`).

| Reaches the throw at | What catches it | Effect of the change |
|---|---|---|
| `Pool::open` fence-recovery loop, `Pool/CasPool.cpp:707-718` | `catch (const MountFencedException &)` — **by type** | None. The fenced branches keep raising that type; everything else propagated out of the mount open before and still does, failing the mount loudly. |
| Background self-remount, `Pool/CasPool.cpp:1233`, inside an unqualified `catch (...)` | everything | The handler does not discriminate on code, so its behaviour is unchanged — except that the throw stops aborting the process on debug and sanitizer builds. That is the fix. |
| `MountLeaseKeeper::terminalResult`, `:1636`, which rethrows when `e.code() == LOGICAL_ERROR` | — | Unreachable from here: `terminalResult` serves `renew`, and `claim` is called only from `start`. |

There is no path here in which the only actor able to clear the offending state is the one being
killed, so none of these throws can wedge anything: both call sites either fail a mount loudly at
startup or retry on the self-remount loop.

### Testing {#b2a-testing}

rev.1 specified one test, which review correctly rejected as nominal: it covers only the
different-epoch branch, and the two other tests rev.1 cited
(`gtest_cas_pool.cpp:1824`, `gtest_cas_heartbeat.cpp:373`) exercise the **renewal** path, which this
change does not touch. Five branches and both precedence rules could regress with that gate green.

Coverage is therefore direct and per branch. Four of the eight branches need a state change inside a
window `claim` holds open, which today's helpers cannot produce, so the tests get one seam:

```cpp
/// Runs a caller-supplied action ONCE, immediately before the named backend call, so a test can make
/// the mount slot change inside a window `MountLeaseKeeper::claim` holds open. Same shape as the
/// existing one-shot fault backends in this header.
class MountSlotRaceBackend : public DB::Cas::InMemoryBackend
{
public:
    std::function<void()> before_put_if_absent;   /// fires once, then clears itself
    std::function<void()> before_get;
    std::function<void()> before_put_overwrite;
};
```

`get`, `head`, `putIfAbsent` and `putOverwrite` are all virtual on `Backend`
(`Backend/CasBackend.h:248-283`), so each hook is an override that fires and clears itself before
delegating to `InMemoryBackend`. A hook that must *remove* the slot reads its current token with
`get` and then calls `deleteExact` — the backend has no unconditional delete.

Each test constructs the slot state, builds a `MountLeaseKeeper` directly (as
`gtest_cas_mount.cpp:1057` already does), calls `start`, and asserts the exact code and a message
substring:

| Branch | Setup | Expected |
|---|---|---|
| appeared between `head` and `putIfAbsent` | empty slot; `before_put_if_absent` writes a lease | `ABORTED`, "appeared between head and putIfAbsent" |
| vanished between `head` and `get` | live own slot; `before_get` removes it | `ABORTED`, "vanished between head and get" |
| foreign server | slot held by a different uuid | `ABORTED`, "held by a foreign server" |
| different `writer_epoch` | own uuid, different epoch | `ABORTED`, "held by a different writer_epoch" |
| changed while adopting | own live slot; `before_put_overwrite` rewrites it under a new token | `ABORTED`, "changed while adopting our own mount slot" |
| vanished while adopting | own live slot; `before_put_overwrite` removes it | `ABORTED`, "vanished while adopting our own mount slot" |
| fenced before adoption (`:1508`) | own slot with `gc_fenced` set | `MountFencedException` |
| fenced inside the adoption window (`:1521`) | own live slot; `before_put_overwrite` rewrites it with `gc_fenced` set | `MountFencedException`, **not** the "changed while adopting" `ABORTED` — this is the precedence assertion |

`gtest_cas_mount.cpp:1057`, `CASMountLease.KeeperStartAdoptsOurOwnClaimNotDoubleStart`, currently
pins the abort with `EXPECT_DEATH` and sets `abort_on_logical_error` inside the death-test block. It
folds into the different-epoch row above as an ordinary `EXPECT_THROW` — the same assertion in
release and sanitizer builds, with no death-test split needed. This is the correct outcome per the
standing rule: a condition reachable from outside is reclassified rather than wrapped in a death
test.

The whole file is swept for sibling `EXPECT_DEATH` and `EXPECT_THROW` assertions around `claim`,
because a process abort hides every test behind it in the same binary.

`gtest_cas_pool.cpp:1824` and `gtest_cas_heartbeat.cpp:373` are renewal-path tests. They are expected
to stay green **unchanged** and are listed here only so that a later reader does not mistake them for
coverage of this change.

## Verification gate {#gate}

`unit_tests_dbms --gtest_filter=Cas*:CA*` in a **debug** build and in an **ASan** build. Both are
required: B2a is specifically about behaviour that differs on sanitizer lanes, so a release-only run
says nothing about it.

## Out of scope {#out-of-scope}

- **B3**, the inline `disk(metadata_type='cas', …)` privilege gate — its own design.
- **Item 10**, detached CAS work outliving `Context` — a separate architectural design, currently
  the next one queued. Only the boundary noted in `{#b1-fix}` connects them.
- The `meta_pool_wait` phase's position, the round's publication order, and every durable format.
