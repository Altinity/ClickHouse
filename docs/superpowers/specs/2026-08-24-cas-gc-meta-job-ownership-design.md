---
description: 'Design for CAS GC meta-pool job ownership and for the mount-claim error classification, closing the two untracked P1 findings B1 and B2a'
sidebar_label: 'CAS GC meta-job ownership'
sidebar_position: 5
slug: /superpowers/specs/cas-gc-meta-job-ownership-design
title: 'CAS GC meta-job ownership and mount-claim error classification'
doc_type: 'design'
---

# CAS GC meta-job ownership and mount-claim error classification {#cas-gc-meta-job-ownership-design}

**Status:** DRAFT for review, rev.1 (2026-08-24).

This specification covers item 9 of `docs/superpowers/cas/final-checks-todo.md` minus its third
bullet: findings **B1** (`~Gc` versus the `meta_pool` drain) and **B2a**
(`MountLeaseKeeper::claim` throwing `LOGICAL_ERROR`) from the 2026-08-05 umbrella review, both
re-verified against `cas-gc-rebuild` on 2026-08-24. The third bullet, B3 (an inline
`disk(metadata_type='cas', …)` bypassing the `SYSTEM CAS` privilege model), is explicitly **out of
scope**: it is a privilege-model decision with cross-tenant consequences and needs its own design.

There is no persisted-data compatibility question anywhere in this document. Nothing here changes a
durable format, a key shape, or a protocol step.

## Decision {#decision}

Two independent changes, landing as two commits on the existing `cas-gc-rebuild` branch.

1. **B1.** Everything the bounded GC meta pool touches moves into one shared object captured **by
   value** into every job. A job then depends on nothing owned by `Gc`, so no member-declaration
   order can reopen the gap, and no explicit `~Gc` is needed for safety. Separately, the round drains
   the pool on **every** exit path, not only on the success path.
2. **B2a.** Every `LOGICAL_ERROR` in `MountLeaseKeeper::claim` becomes `ABORTED`, matching the
   sibling renewal path. The death test that pins the current abort becomes an ordinary throw
   assertion.

## B1 — the meta-pool job boundary {#b1}

### What is wrong today {#b1-defect}

`Gc` owns a bounded `ThreadPool` (`meta_pool`, `Gc/CasGc.h:950`) for the round's per-hash
freshness-meta writes. Two sites produce jobs for it, and **both capture `this`**:

- `Gc/CasGc.cpp:433` — the condemn-marker write: `[this, ref, token, condemn_round, size]`, which
  calls `writeCondemnedMeta(*store, …)` and then `noteCondemnMarkerDurable(…)`.
- `Gc/CasGc.cpp:867` — the delete confirmation:
  `[this, ref]() { deleteConfirmedMeta(store->backend(), store->layout(), ref); }`.

Capturing `this` makes a job depend on the whole object. What the jobs actually reach is `store`,
the two job counters (`meta_jobs_scheduled_`, `meta_jobs_completed_`, `Gc/CasGc.h:956-957`) and the
condemn-marker confirmation registry (`condemn_marker_mutex` + `condemn_markers_confirmed`,
`:967-968`). Of those, everything except `store` is declared **after** `meta_pool` and is therefore
destroyed **before** it — that is, before `~ThreadPool` joins the workers. A job still executing at
that moment locks a destroyed mutex and inserts into a destroyed set.

The code already believes otherwise. `Gc/CasGc.cpp:389` states that the counter capture is safe
because "the pool is a member of the same `Gc` and is joined by `~Gc` before the atomic dies" — a
guarantee that no code implements, since no `~Gc` exists. The comment is not a stale detail; it is
the exact argument this design replaces.

rev.8 made `Gc` destruction routine (`UNMOUNT`, `SYSTEM CAS GC STOP`) rather than a shutdown-only
event, so the window is wider than when the code was written, not narrower.

### Why this is not fixed by an explicit destructor alone {#b1-why-not-ordering}

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
bitten by here. The fix therefore removes the argument instead of restating it.

### The fix: shared state, captured by value {#b1-fix}

One object names the boundary between the round thread and the pool threads:

```cpp
/// Everything the bounded meta pool touches, and the ONLY thing its jobs may reach. Held by
/// `shared_ptr` and captured BY VALUE into every job, so a job outliving the `Gc` is safe by
/// construction rather than by member-declaration order.
struct MetaJobState
{
    /// Cumulative counts of jobs handed to the pool and of jobs that finished. The round reports
    /// its own deltas; see the `meta_pool_wait` phase.
    std::atomic<uint64_t> scheduled{0};
    std::atomic<uint64_t> completed{0};

    /// In-process confirmations of durable condemn-marker writes, keyed (blob, exact
    /// incarnation-token value). Pool completions insert concurrently with the round thread's reads.
    std::mutex condemn_marker_mutex;
    std::set<std::pair<BlobRef, String>> condemn_markers_confirmed;

    void noteCondemnMarkerDurable(const BlobRef & ref, const Token & token);
    bool condemnMarkerConfirmedInProcess(const BlobRef & ref, const Token & token);
    void forgetCondemnMarker(const BlobRef & ref, const Token & token);
};
```

`Gc` holds `std::shared_ptr<MetaJobState> meta_state`, constructed in the constructor body next to
`meta_pool`. The three accessors move from `Gc` to `MetaJobState` verbatim; their eight call sites
inside `Gc` (`CasGc.cpp:436`, `:870`, `:916`, `:965`, `:1890`, `:1896`, and the two definitions'
former callers) go through `meta_state->…`.

Neither job site captures `this` any more:

- the condemn-marker job captures `store` (a `PoolPtr` copy), `meta_state`, and its value arguments;
- the delete-confirmation job captures `store` and its `ref`.

The strong `PoolPtr` copy is deliberate and is **not** the detached-work problem tracked as item 10
of `final-checks-todo.md`. These jobs run on a pool that `~ThreadPool` joins, so the reference is
bounded by that join; item 10 concerns `.detach()`-ed work with no join at all. The distinction is
worth stating in the source comment so a reviewer does not read one as the other.

`~Gc` is **not** added. Its absence is the property being asserted: once the jobs own what they
touch, destruction order carries no correctness weight. The `:389` comment is deleted rather than
corrected.

### Round-exit drain {#b1-round-drain}

Independently of the ownership change, `runRegularRound` must drain the pool on every exit path.

`Cas::Gc gc` is a **member of the scheduler** (`Gc/CasGcScheduler.h:190`), not a per-round object, so
rounds share one pool and one `MetaJobState`. Today the only drain is the `meta_pool_wait` phase at
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
- **Ownership — a job outliving its `Gc`.** With the jobs holding `shared_ptr<MetaJobState>` and
  `PoolPtr`, a job that completes after its `Gc` has been destroyed is well-defined. This is asserted
  through a test seam that schedules a job directly, mirroring the existing `TEST SEAM` accessors on
  `Gc` (`Gc/CasGc.h:985-1007`). This test does not go red before the change — it cannot compile
  before it — and the plan must say so rather than stage a false red step.
- **Regression guard on the existing premise.** `gtest_cas_gc_ack_floor.cpp:1242-1277` reasons
  explicitly that the confirmation registry "dies with" its `Gc`, so a new `Gc` under the same
  identity starts empty and falls back to a synchronous `loadMeta` re-check. That premise survives —
  `meta_state` is a member, and a fresh `Gc` gets a fresh one — and the test must be re-run to prove
  it, not assumed.

## B2a — `claim()` is not a `LOGICAL_ERROR` site {#b2a}

### What is wrong today {#b2a-defect}

`MountLeaseKeeper::claim` (`Pool/CasServerRoot.cpp:1460-1537`) throws `LOGICAL_ERROR` on six
branches:

| Line | Condition |
|------|-----------|
| `:1468` | the key appeared between our `head` and our `putIfAbsent` |
| `:1479` | the key vanished between our `head` and our `get` |
| `:1489` | the slot is held by a foreign server |
| `:1499` | the slot is held by a different `writer_epoch` |
| `:1525` | the slot changed while we were adopting our own |
| `:1530` | the slot vanished while we were adopting our own |

Constructing a `DB::Exception` with `LOGICAL_ERROR` calls `handle_error_code`
(`src/Common/Exception.cpp`), which aborts the process on any `DEBUG_OR_SANITIZER_BUILD`. `claim` is
reached from `MountLeaseKeeper::start`, which the background self-remount path calls
(`Pool/CasPool.cpp:1233`) — so these are aborts on a background thread, on exactly the debug and
sanitizer lanes that certify the mount feature.

The classification is also simply wrong. Every one of the six fires because **another process
changed the mount object between two of our observations**. None of them reports a violated
invariant of ours. The sibling renewal path already says so: the same conditions there raise
`ABORTED` (`:1594`, `:1604`, `:1618`).

### The fix {#b2a-fix}

All six become `ABORTED`. Message texts are preserved verbatim — three tests match on substrings of
them.

The two `gc_fenced` branches in the same function (`:1508`, `:1521`) already raise
`MountFencedException` and are **not** touched: they are caught by type, and that catch is
load-bearing.

### Call-site audit {#b2a-audit}

Changing a thrown code requires checking where each throw lands, since nothing in the compiler will.
`claim` has exactly one caller, `start` (`:1548`), which in turn has exactly one caller,
`CasMountRuntime::startKeeper` (`Pool/CasMountRuntime.cpp:394`).

| Reaches the throw at | What catches it | Effect of the change |
|---|---|---|
| `Pool::open` fence-recovery loop, `Pool/CasPool.cpp:707-718` | `catch (const MountFencedException &)` — **by type** | None. The fenced branches keep raising that type; everything else propagated out of the mount open before and still does, failing the mount loudly. |
| Background self-remount, `Pool/CasPool.cpp:1233`, inside `catch (...)` | everything, by an unqualified catch | The handler does not discriminate on code, so its behaviour is unchanged — except that the throw is no longer an `abort()` on debug and sanitizer builds. That is the fix. |
| `MountLeaseKeeper::terminalResult`, `:1636`, which rethrows when `e.code() == LOGICAL_ERROR` | — | Unreachable from here: `terminalResult` serves `renew`, and `claim` is called only from `start`. |

There is no path here in which the only actor able to clear the offending state is the one being
killed, so none of these throws can wedge anything: both call sites either fail a mount loudly at
startup or retry on the self-remount loop.

### Testing {#b2a-testing}

`gtest_cas_mount.cpp:1057`, `CASMountLease.KeeperStartAdoptsOurOwnClaimNotDoubleStart`, currently
pins the abort with `EXPECT_DEATH` and sets `abort_on_logical_error` inside the death-test block. It
becomes an ordinary `EXPECT_THROW` asserting code `ABORTED` and the same message substring — the
same assertion in release and sanitizer builds, with no death-test split needed. This is the
correct outcome per the standing rule: a condition reachable from outside is reclassified rather
than wrapped in a death test.

The whole file is swept for sibling `EXPECT_DEATH` / `EXPECT_THROW` assertions around `claim`,
because a process abort hides every test behind it in the same binary.

Two further tests match on these message substrings and must stay green unchanged:
`gtest_cas_pool.cpp:1824` and `gtest_cas_heartbeat.cpp:373`.

## Verification gate {#gate}

`unit_tests_dbms --gtest_filter=Cas*:CA*` in a **debug** build and in an **ASan** build. Both are
required: B2a is specifically about behaviour that differs on sanitizer lanes, so a release-only run
says nothing about it.

## Out of scope {#out-of-scope}

- **B3**, the inline `disk(metadata_type='cas', …)` privilege gate — its own design.
- **Item 10**, detached CAS work outliving `Context` — a separate architectural design, currently
  the next one queued. Only the boundary noted in `{#b1-fix}` connects them.
- The `meta_pool_wait` phase's position, the round's publication order, and every durable format.
