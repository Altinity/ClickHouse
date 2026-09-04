---
description: 'Implementation plan for phase A of the CAS hot-key write lane: one FIFO ticket per pool and key above the request engine, a last-known-object cache under one rule, the flat conflict pause with the growing schedule after a settled fault, and the catalog as the first caller. Eight tasks, each with its own test cycle.'
sidebar_label: 'CAS hot-key lane phase A plan'
sidebar_position: 9
slug: /superpowers/plans/cas-hot-key-lane-phase-a
title: 'CAS Hot-Key Lane Phase A Implementation Plan'
doc_type: 'plan'
---

# CAS hot-key write lane, phase A — Implementation Plan {#cas-hot-key-lane-phase-a-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every catalog mutation of one pool waits its turn in a per-key FIFO above the request engine, starts from the pool's last known object when the cache holds one, and is repaid after a flat jitter when it loses a race to another server, so the in-process 412 storms and the growing conflict backoff that made `DROP TABLE` p90 11.9 s disappear.

**Architecture:** A new `Backend/CasHotKeys.{h,cpp}` owns, per pool, one lane per key (a deque of tickets, a condition variable, a holder timestamp) and one byte-bounded `CacheBase` of last known objects. `CasHotKeys::submit` waits for the ticket's turn re-checking the caller's own fence, lease and deadline every 200 ms slice, then runs the caller's `decide` on the cached object or on the engine's own `observe`, writes through the engine's `replace`/`create`, and returns the engine's `WriteResult` unchanged. A verdict a `decide` renders on a cached base is never delivered: the entry is dropped, the key is read, the `decide` runs again. `Conflict` gains `any_ambiguous`, `Retry` gains `conflictBackoff`, and the two engine read-modify-write loops repay a clean lost race after that flat pause. `CasRefCatalog::casUpdateImpl` submits through the lane in a hand-written loop under the caller's frozen policy. `Pool` owns the `CasHotKeys` and hands its three planes a pointer to it.

**Tech Stack:** C++ (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`), gtest via `unit_tests_dbms`, `CacheBase` from `src/Common/CacheBase.h`, `ProfileEvents`/`CurrentMetrics`.

**Spec:** `docs/superpowers/specs/2026-09-04-cas-hot-key-write-lane-design.md` (revision 34, phase A; accepted by the codex review of revision 33 with no CRITICAL or MAJOR). The plan argues from the spec; read both. The spec's revision-history section ends with the checklist of rules the review rounds established; every task below implements against it, and the reviewer in Task 6 verifies against it.

## Global Constraints {#global-constraints}

- **Worktree and branch.** All code work happens in the `lane-g` worktree at `/home/mfilimonov/workspace/ClickHouse/lane-g`, on a NEW branch `cas-hot-key-lane` created from `cas-gc-rebuild` (Task 0). Its `build/` is configured and warm (`build/src/unit_tests_dbms` built today), so incremental builds are cheap. `lane-g` carries two modified files under `utils/ca-soak/scenarios/` and untracked files under `docs/superpowers/models/` and `__cache__/` that belong to another agent's work: never stage, stash, reset or clean them. The plan document itself lives in the `master` worktree on `cas-gc-rebuild`.
- **Shared checkouts.** Stage named paths only, never `git add -A`. Every commit is `git add <paths> && git commit -m "..." -- <paths>` followed by `git log -1 --stat`, whose output must list exactly those paths. Before the merge in Task 6, `git -C /home/mfilimonov/workspace/ClickHouse/master diff --cached --stat` must be empty. Never push.
- **Suite naming is a gate.** The CAS gate is exactly `--gtest_filter='CAS*'`. Every new suite MUST be named `CAS…` (`CASHotKeys`, `CASRequests`, `CASRefCatalog`, `CASPool`); a suite that does not match never runs in the gate.
- **No sleep to order threads.** A `std::latch`, a condition variable, or the lane's own bounded `wait_for` slice whose expiry is itself the assertion. The one place a test waits on real time is the lane's 200 ms slice, which is the design's own.
- **No durable-format, key-shape or protocol-step change.** Nothing persisted by the pool changes; the catalog's bytes, the write's precondition and the resolve read are the engine's today.
- **Nothing outside `ContentAddressed/` except:** `src/Common/ProfileEvents.cpp`, `src/Common/CurrentMetrics.cpp`, `src/Disks/tests/*.cpp`, and `docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md` (three sentences, Task 1). No edit to `src/Common/CacheBase.h`, `src/IO`, or the S3 transport.
- **The engine changes are exactly the four the spec names:** the `CasHotKeys` pointer carried by `CasRequests` and exposed as `CasOperation::hotKeys()`; `friend class CasHotKeys` on `CasOperation`; `Conflict::any_ambiguous`; `Retry::conflictBackoff` with `pauseForConflict`. No other change to `CasRequests.{h,cpp}`.
- **C++ style:** Allman braces. **Comments:** state the reason; never cite this plan, the spec, a review, a backlog or a task number. Function names in prose as `f`, not `f()`. Say "exception", not "crash", for a `LOGICAL_ERROR`.
- **Fail fast.** Never run a test binary after a failed build; never commit after a failed test. Each build, each analysis, each test run and each commit is its own step.
- **Build and test logs** go to the build directory and are analysed by a subagent, never read directly (see Conventions).
- New `gtest_*.cpp` files under `src/Disks/tests/` need no CMake edit (globbed with `CONFIGURE_DEPENDS`); a new `.cpp` under `Backend/` needs none either (`add_headers_and_sources(dbms .../Backend)`), but the first build after adding one re-runs cmake by itself.

## Conventions {#conventions}

Three step templates, referenced by name from every task. Each occurrence names its own log file so parallel runs never overwrite each other. `LG` is `/home/mfilimonov/workspace/ClickHouse/lane-g`.

**BUILD(`<log>`)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g
ninja -C build unit_tests_dbms > build/<log> 2>&1
```

No `; echo "$?"` after it. A nonzero status means the next step is the analysis, never the test run.

**ANALYZE(`<log>`)** — dispatch a subagent (`ca-review-lite`, sonnet, medium effort) with this prompt, and do not read the log yourself:

> Read `/home/mfilimonov/workspace/ClickHouse/lane-g/build/<log>`. Report, in at most ten lines: whether the build or test run succeeded; the first error or first failing test with its file and line; and the total counts (errors, or tests run/passed/failed). Quote no more than three lines of the log.

**TEST(`<filter>`, `<log>`)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g
./build/src/unit_tests_dbms --gtest_filter='<filter>' > build/<log> 2>&1
```

Same rule: a nonzero status means the run failed, and the next step is the analysis, never the commit. A test run is valid only if the BUILD that preceded it succeeded: a green suite after a failed build is the wrong binary.

**COMMIT(`<paths>`, `<message>`)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g
git add <paths> && git commit -m "<message>" -- <paths>
git log -1 --stat
```

The `--stat` must list exactly `<paths>`.

## File structure {#file-structure}

| file | responsibility |
|---|---|
| `Backend/CasHotKeys.h` (new) | the lane: `CasHotKeys` class, `Decide`, `submit`, the two test seams, the two `*_hook_for_test` members |
| `Backend/CasHotKeys.cpp` (new) | the ticket (enter, wait, leave), the hold (base, decide, write, settle), the cache helpers, the log line |
| `Backend/CasRequests.h` | `#include CasHotKeys.h`; ctor parameter `CasHotKeys * hot_keys_ = nullptr`; members `own_hot_keys`, `hot_keys`; `CasOperation::hotKeys()`; `friend class CasHotKeys`; `pauseForConflict` |
| `Backend/CasRequests.cpp` | ctor wiring; `detail::recordConflictPause`; `pauseForConflict`; `any_ambiguous` at the two `Conflict` returns of `writeLoop`; the pause choice in `readModifyWrite` and `readModifyWriteOnPresence`; the presence loop's rebuilt `Conflict` |
| `Backend/CasWriteResult.h` | `Conflict::any_ambiguous` |
| `Backend/CasRetry.h` | `Retry::conflictBackoff` |
| `Pool/CasRefCatalog.cpp` | `casUpdateImpl` submits in a loop; `createNamespace` catches the fence marker around step 1; `casAdmitEntry` translates it |
| `Pool/CasPool.h`, `Pool/CasPool.cpp` | `PoolConfig::hot_key_cache_bytes`; `Pool::hot_keys` before the three planes; the planes get `&hot_keys` |
| `Pool/CasMountRuntime.h` | the contract comment on the three hooks that run under `driver_mutex` |
| `src/Common/ProfileEvents.cpp`, `src/Common/CurrentMetrics.cpp` | five events, two metrics |
| `src/Disks/tests/gtest_cas_hot_keys.cpp` (new) | lane-level tests, suite `CASHotKeys`, on a synchronized clock |
| `src/Disks/tests/gtest_cas_requests.cpp` | Half 2 tests, suite `CASRequests` |
| `src/Disks/tests/gtest_cas_ref_catalog.cpp` | catalog-level tests, suite `CASRefCatalog` |
| `src/Disks/tests/gtest_cas_pool.cpp` | the pool-level test, suite `CASPool` |
| `docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md` | three sentences of the contract |

---

### Task 0: Branch, baseline, plan commit {#task-0}

**Files:**
- Commit in `master`: `docs/superpowers/plans/2026-09-04-cas-hot-key-lane-phase-a.md`

- [ ] **Step 1: Commit this plan in the master worktree**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git diff --cached --stat
git add docs/superpowers/plans/2026-09-04-cas-hot-key-lane-phase-a.md && git commit -m "ca-docs: implementation plan for the hot-key lane, phase A (spec rev.34)" -- docs/superpowers/plans/2026-09-04-cas-hot-key-lane-phase-a.md
git log -1 --stat
```

The first command must print nothing (an empty index); the `--stat` must list only the plan.

- [ ] **Step 2: Create the branch in lane-g**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g
git status --short
git checkout -b cas-hot-key-lane cas-gc-rebuild
git log --oneline -1
```

`git status --short` shows only the foreign files named in Global Constraints; if it shows anything under `src/` or `docs/superpowers/specs/`, stop and report.

- [ ] **Step 3: Confirm the build is warm**

BUILD(`build_task0.log`), then ANALYZE(`build_task0.log`). Expected: success with no compilation work, or a short incremental build after the checkout.

- [ ] **Step 4: Baseline gate**

TEST(`CAS*`, `test_task0_gate.log`), then ANALYZE(`test_task0_gate.log`). Record the counts (tests run, passed) in the task's report; Task 6 compares against them. Expected: all green. A red baseline is reported, not fixed here.

---

### Task 1: Half 2 in the engine — `Conflict::any_ambiguous`, `conflictBackoff`, `pauseForConflict` {#task-1}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasWriteResult.h:50`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h` (after `backoff`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h` (private section of `CasOperation`, beside `pauseAndReissue`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp` (`detail`, `pauseAndReissue`'s neighbourhood, `writeLoop` lines 908–920, `readModifyWrite` ~980, `readModifyWriteOnPresence` ~1034)
- Modify: `src/Common/ProfileEvents.cpp:939` (after `CASRequestReissue`)
- Modify: `docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md:364,648,684,691`
- Test: `src/Disks/tests/gtest_cas_requests.cpp`

**Interfaces:**
- Produces: `struct Conflict { Observation seen; uint32_t attempts_sent = 0; bool any_ambiguous = false; }`; `static uint64_t Retry::conflictBackoff()`; private `std::optional<WriteResult> CasOperation::pauseForConflict(WriteState & state, const Retry::Bound & bound)`; profile event `CASRequestConflictPause`. Task 4's catalog loop reads `Conflict::any_ambiguous` and calls `Retry::conflictBackoff`.

- [ ] **Step 1: Write the failing tests**

Append to `src/Disks/tests/gtest_cas_requests.cpp`, after `ReadModifyWriteLosesNoIncrementUnderContentionAndBoundsAHotKey`. The file already has `ProfileEvents` externs at its top for the events it reads; add `CASRequestConflictPause` and `CASRequestReissue` there if absent:

```cpp
namespace ProfileEvents
{
    extern const Event CASRequestReissue;
    extern const Event CASRequestConflictPause;
}
```

Then the tests. A competitor that moves the key before each of our first `K` attempts, through the write hook, is the same device `ReadModifyWriteDoesNotClaimACompetitorsIdenticalBytesAfterAnEarlierAmbiguity` uses; the `inside` flag keeps the competitor's own writes from re-entering the hook.

```cpp
namespace
{

/// Moves `key` under the caller before each of its first `moves` write attempts, and optionally makes
/// each of those attempts ambiguous (the store never answers it) so the resolve read is what settles
/// the race.
struct RaceMaker
{
    RaceMaker(std::shared_ptr<CountingBackend> backend_, FakeClock & clock, String key_, int moves_, bool ambiguous_)
        : backend(std::move(backend_)), key(std::move(key_)), moves(moves_), ambiguous(ambiguous_)
        , rival_requests(makeRequests(backend, clock)), rival(rival_requests.admit())
    {
        backend->onBeforeWrite(key, [this]
        {
            if (inside || made >= moves)
                return;
            inside = true;
            if (const auto current = rival.read(key, Retry::once()))
                (void)rival.replace(key, current->bytes + "r", current->etag, Retry::once());
            else
                (void)rival.create(key, "r", Retry::once());
            if (ambiguous)
                backend->injectAmbiguousWrite(key);
            ++made;
            inside = false;
        });
    }

    std::shared_ptr<CountingBackend> backend;
    String key;
    int moves;
    bool ambiguous;
    int made = 0;
    bool inside = false;
    CasRequests rival_requests;
    CasOperation rival;
};

DecideOnObject appendX()
{
    return [](const std::optional<Object> & current) -> std::optional<String>
    {
        return current ? current->bytes + "x" : String("x");
    };
}

}

TEST(CASRequests, CleanConflictsArePacedFlatAndDoNotAdvanceTheReissueCounter)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    (void)orThrow(op.create("k", "v", Retry::standard()), "seed");
    constexpr int K = 4;
    RaceMaker races(backend, clock, "k", K, /*ambiguous=*/false);
    const auto pauses_before = ProfileEvents::global_counters[ProfileEvents::CASRequestConflictPause].load();
    const auto reissues_before = ProfileEvents::global_counters[ProfileEvents::CASRequestReissue].load();

    WriteResult result = op.readModifyWrite("k", appendX(), Retry::standard());

    ASSERT_TRUE(std::holds_alternative<Committed>(result));
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRequestConflictPause].load() - pauses_before, K);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRequestReissue].load() - reissues_before, 0u);
    ASSERT_EQ(clock.sleeps.size(), static_cast<size_t>(K));
    for (uint64_t s : clock.sleeps)
        EXPECT_LE(s, 200u);   /// flat: every pause is one `backoff(1)` draw, whatever the loss count
    EXPECT_EQ(backend->writeCount("k"), 1u + K + 1u + K);   /// seed, K refused, K rival moves, the one that landed
}

TEST(CASRequests, AConflictThatSettledAFaultKeepsTheGrowingSchedule)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    (void)orThrow(op.create("k", "v", Retry::standard()), "seed");
    constexpr int K = 3;
    RaceMaker races(backend, clock, "k", K, /*ambiguous=*/true);
    const auto pauses_before = ProfileEvents::global_counters[ProfileEvents::CASRequestConflictPause].load();
    const auto reissues_before = ProfileEvents::global_counters[ProfileEvents::CASRequestReissue].load();

    WriteResult result = op.readModifyWrite("k", appendX(), Retry::standard());

    ASSERT_TRUE(std::holds_alternative<Committed>(result));
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRequestConflictPause].load() - pauses_before, 0u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRequestReissue].load() - reissues_before, K);
    ASSERT_EQ(clock.sleeps.size(), static_cast<size_t>(K));
    for (size_t i = 0; i < clock.sleeps.size(); ++i)
        EXPECT_LE(clock.sleeps[i], std::min<uint64_t>(5000, 200ull << i)) << "reissue " << i;
}

TEST(CASRequests, ReplaceReportsWhetherAConflictSettledAFault)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    const Etag seed = *orThrow(op.create("k", "v", Retry::standard()), "seed");
    {
        RaceMaker clean(backend, clock, "k", 1, /*ambiguous=*/false);
        WriteResult result = op.replace("k", "w", seed, Retry::standard());
        const auto * conflict = std::get_if<Conflict>(&result);
        ASSERT_NE(conflict, nullptr);
        EXPECT_FALSE(conflict->any_ambiguous);
        EXPECT_EQ(conflict->attempts_sent, 1u);
    }
    {
        RaceMaker faulty(backend, clock, "k", 1, /*ambiguous=*/true);
        WriteResult result = op.replace("k", "w", seed, Retry::standard());
        const auto * conflict = std::get_if<Conflict>(&result);
        ASSERT_NE(conflict, nullptr);
        EXPECT_TRUE(conflict->any_ambiguous);
        EXPECT_EQ(conflict->attempts_sent, 1u);   /// one attempt, lost, settled as moved: `attempts_sent` cannot tell
    }
}

TEST(CASRequests, OnPresenceUnderOnceKeepsTheFaultFlagOnTheRebuiltConflict)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    (void)orThrow(op.create("k", "v", Retry::standard()), "seed");
    RaceMaker faulty(backend, clock, "k", 1, /*ambiguous=*/true);

    WriteResult result = op.readModifyWriteOnPresence("k",
        [](const std::optional<Meta> &) -> std::optional<String> { return String("w"); }, Retry::once());

    const auto * conflict = std::get_if<Conflict>(&result);
    ASSERT_NE(conflict, nullptr);
    EXPECT_TRUE(conflict->any_ambiguous);
    EXPECT_TRUE(std::holds_alternative<Meta>(conflict->seen));   /// presence-only, as before
}

TEST(CASRequests, CleanConflictsBeforeAFaultDoNotInflateTheFaultsFirstBackoff)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    (void)orThrow(op.create("k", "v", Retry::standard()), "seed");
    constexpr int K = 3;
    RaceMaker races(backend, clock, "k", K, /*ambiguous=*/false);
    /// After the K clean races the next attempt is ambiguous with the precondition unchanged, so the
    /// engine reissues it; that reissue's pause must be the schedule's first, not its (K+1)-th.
    bool armed = false;
    backend->onBeforeWrite("k", [&]
    {
        /// The RaceMaker's hook is replaced by this one; it moves the key itself for the first K writes.
        if (races.inside)
            return;
        if (races.made < K)
        {
            races.inside = true;
            if (const auto current = races.rival.read("k", Retry::once()))
                (void)races.rival.replace("k", current->bytes + "r", current->etag, Retry::once());
            ++races.made;
            races.inside = false;
            return;
        }
        if (!armed)
        {
            armed = true;
            backend->injectAmbiguousWrite("k");
        }
    });

    WriteResult result = op.readModifyWrite("k", appendX(), Retry::standard());

    ASSERT_TRUE(std::holds_alternative<Committed>(result));
    ASSERT_EQ(clock.sleeps.size(), static_cast<size_t>(K + 1));
    EXPECT_LE(clock.sleeps[K], 200u) << "the first transport reissue sleeps within backoff(1)";
}
```

- [ ] **Step 2: Run the tests to verify they fail**

BUILD(`build_task1_red.log`), then ANALYZE(`build_task1_red.log`). Expected: compilation errors naming `any_ambiguous` and `CASRequestConflictPause` (the tests reference symbols that do not exist yet). That is the failing state for this step; do not run the binary.

- [ ] **Step 3: Add the field, the pause and the event**

`CasWriteResult.h`, replace the `Conflict` declaration:

```cpp
/// A competing write won: the key's current state does not match what this call expected.
/// `attempts_sent` counts the HTTP attempts this call made, the same count `Committed` and `GaveUp`
/// carry: an operator's attempt counters sum over ALL the endings of a write, and losing the key is
/// one an operator wants counted rather than dropped. `any_ambiguous` is true when an attempt of the
/// inner write ended without proof of whether it applied before the resolve read settled the race: a
/// caller outside the engine paces such a conflict on the growing schedule, as the engine's own
/// loops do, and a clean lost race on the flat one. `attempts_sent` cannot stand in for it: a
/// throttled first attempt whose resolve read finds the key moved is one attempt, ambiguous.
struct Conflict  { Observation seen; uint32_t attempts_sent = 0; bool any_ambiguous = false; };
```

`CasRetry.h`, after `backoff`:

```cpp
    /// The flat pause after a clean lost race: `backoff(1)`, uniform over [0, 200] ms. It does not grow
    /// with the writer's loss count, because a settled conflict has nothing to wait for but
    /// desynchronisation from its competitors, and growing it with the loss count made the oldest
    /// loser the slowest and the likeliest to lose again. A conflict that settled a transport fault
    /// keeps `backoff(attempt)`: the fault is what must pace the loop.
    static uint64_t conflictBackoff() { return backoff(1); }
```

`CasRequests.h`, in `CasOperation`'s private section after `pauseAndReissue`:

```cpp
    /// The sibling for a clean lost race: the same admission and the same reservation, a flat
    /// `Retry::conflictBackoff` sleep, and `state.reissues` untouched, so a transport fault that follows
    /// starts its own schedule at the beginning.
    std::optional<WriteResult> pauseForConflict(WriteState & state, const Retry::Bound & bound);
```

`CasRequests.cpp`: add the extern beside the others at the top:

```cpp
    extern const Event CASRequestConflictPause;
```

in `namespace detail`, after `recordReissue`:

```cpp
void recordConflictPause()
{
    ProfileEvents::increment(ProfileEvents::CASRequestConflictPause);
}
```

(and declare it in `CasRequests.h`'s `namespace detail` beside `recordReissue`: `void recordConflictPause();`). After `pauseAndReissue`'s definition:

```cpp
std::optional<WriteResult> CasOperation::pauseForConflict(WriteState & state, const Retry::Bound & bound)
{
    const uint64_t pause_ms = Retry::conflictBackoff();
    const uint64_t needed = reservedFor(pause_ms, 2);
    switch (gate(needed))
    {
        case Gate::FenceLost: return gaveUp(GaveUp::Why::FenceLost, sourceFor(bound), state);
        case Gate::NoBudget:  return gaveUp(GaveUp::Why::Deadline, GaveUp::Source::Lease, state);
        case Gate::Ok: break;
    }
    if (!fits(needed, bound))
        return gaveUp(GaveUp::Why::Deadline, sourceFor(bound), state);
    detail::recordConflictPause();
    owner.sleep_ms(pause_ms);
    return std::nullopt;
}
```

In `writeLoop`, the two `Conflict` returns (the clean arm and the moved arm) become:

```cpp
            return Conflict{state.last_seen, state.attempts_sent, state.any_ambiguous};
```

In `readModifyWrite`, replace

```cpp
        if (auto given_up = pauseAndReissue(state, bound))
            return *given_up;
```

with

```cpp
        /// A clean lost race is settled: the resolve read holds the fresh object and the next
        /// iteration decides on it. Only a conflict that settled a transport fault is paced by the
        /// growing schedule.
        if (auto given_up = state.any_ambiguous ? pauseAndReissue(state, bound) : pauseForConflict(state, bound))
            return *given_up;
```

In `readModifyWriteOnPresence`, the same replacement, and the rebuilt result under `single_attempt` becomes:

```cpp
        if (policy.single_attempt)
            return Conflict{state.last_seen, state.attempts_sent, state.any_ambiguous};
```

`src/Common/ProfileEvents.cpp`, after the `CASRequestReissue` line:

```cpp
    M(CASRequestConflictPause, "Number of clean lost races the CAS request contract repaid after a flat jitter instead of a growing backoff: the resolve read had settled the conflict and no transport fault preceded it.", ValueType::Number) \
```

- [ ] **Step 4: Build and run the new tests**

BUILD(`build_task1.log`), ANALYZE(`build_task1.log`). Then TEST(`CASRequests.*`, `test_task1_requests.log`), ANALYZE(`test_task1_requests.log`). Expected: every `CASRequests` test passes, the five new ones included. The existing `ReadModifyWriteLosesNoIncrementUnderContentionAndBoundsAHotKey` asserts the old pacing on a hot key; if it asserts sleep sizes that only the growing schedule satisfies, update its expectation to the flat one and say why in the test's comment (the resolve read settles a clean race; growing was the starvation).

- [ ] **Step 5: Edit the backend request contract**

In `docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md`:

1. Line 364, the `Conflict` line of the result vocabulary, becomes:
   ```
   struct Conflict  { Observation seen; uint32_t attempts_sent; bool any_ambiguous; };   // a competing write won; `any_ambiguous`: an attempt of the inner write was ambiguous before the resolve read settled it
   ```
2. Around line 648 in the `readModifyWrite` section, replace the sentence "Conflicts spend the same budget as errors: a hot key is also a failure, and it must end — GCS bounds mutations of one object at about one per second, and today's `_ckpt` and catalog loops reissue without a sleep." with: "Conflicts spend the same deadline as errors but not the same pace: a clean lost race is settled by the resolve read and reissued after `Retry::conflictBackoff()`, a flat uniform over [0, 200] ms that does not grow with the writer's loss count. The growing schedule belongs to transport faults, and to a conflict that settled one. A key several writers of one pool share is written through the hot-key lane, above this engine, and never conflicts with itself: see the hot-key write lane."
3. Line 684, in the hand-written loop rule after "sleeps with the engine's jitter between iterations", add: "the flat `conflictBackoff` after a refused precondition when the loop can tell it from a settled fault, `backoff(attempt)` otherwise and after a fault".
4. Line 691, the `CasRefCatalog::casUpdateImpl` row of the site table becomes: "| `CasRefCatalog::casUpdateImpl` | the hot-key lane's `submit`, under the caller's policy frozen once, `conflictBackoff` between submissions and `backoff` after a conflict that settled a fault; every other result handled as today |".

- [ ] **Step 6: Commit**

COMMIT(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasWriteResult.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp src/Common/ProfileEvents.cpp src/Disks/tests/gtest_cas_requests.cpp docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md`, `ca-engine: a clean lost race is repaid after a flat jitter; Conflict says whether it settled a transport fault`)

---

### Task 2: The lane without its cache — `CasHotKeys`, the ticket, the hold, the wiring {#task-2}

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h:143-147` (ctor), `:182-187` (members), `CasOperation` public (`hotKeys`) and private (`friend`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp:232-240` (ctor)
- Modify: `src/Common/ProfileEvents.cpp` (four events), `src/Common/CurrentMetrics.cpp:237` (two metrics)
- Test: `src/Disks/tests/gtest_cas_hot_keys.cpp` (new)

**Interfaces:**
- Consumes: `CasOperation::gate`, `fits`, `reservedFor`, `observe`, `gaveUpAfterFailedObservation`, `gaveUp`, `WriteState`, `Resolved`, `Gate`, `owner` (all private, through `friend`); `CasOperation::freeze`, `replace`, `create` (public); `Conflict::any_ambiguous` (Task 1).
- Produces: `class CasHotKeys { explicit CasHotKeys(uint64_t cache_budget_bytes); using Decide = std::function<std::optional<String>(const std::optional<Object> &)>; WriteResult submit(const String & key, CasOperation & op, const Retry & policy, const Decide & decide); size_t queueDepthForTest(const String & key) const; size_t laneCountForTest() const; size_t cacheEntriesForTest() const; std::function<void()> enter_after_lane_hook_for_test; std::function<void()> cache_fill_hook_for_test; }`; `CasRequests(BackendPtr, Fence, now, sleep, CasHotKeys * hot_keys_ = nullptr)`; `CasHotKeys & CasOperation::hotKeys() const`. Task 3 fills in `cached`, `remember`, `forget`; Task 4 calls `submit` from the catalog; Task 5 constructs one per pool.

- [ ] **Step 1: Write the failing tests**

Create `src/Disks/tests/gtest_cas_hot_keys.cpp`:

```cpp
#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasWriteResult.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasThrottlingBackend.h>
#include "cas_test_helpers.h"

#include <Common/ProfileEvents.h>
#include <IO/S3/Client.h>
#include <Poco/Exception.h>

#include <atomic>
#include <deque>
#include <latch>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ProfileEvents
{
    extern const Event CASHotKeyQueueWaitMicroseconds;
    extern const Event CASHotKeyCacheStarts;
    extern const Event CASHotKeyReadStarts;
    extern const Event CASHotKeyCacheVerdictsReread;
    extern const Event CASRequestGaveUp;
}

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;

namespace
{

/// The harness's `FakeClock` is single-threaded. The lane is not: its holders sleep on the engine's
/// clock from their own threads while the test thread advances it, so every access goes through one
/// mutex. A sleep still advances the clock by what it slept, so a holder's transport backoff is real
/// time to every waiter's deadline.
struct SyncClock
{
    std::mutex mutex;
    uint64_t now = 1'000'000;
    std::vector<uint64_t> sleeps;

    std::function<uint64_t()> nowFn()
    {
        return [this] { std::lock_guard lock(mutex); return now; };
    }
    std::function<void(uint64_t)> sleepFn()
    {
        return [this](uint64_t ms) { std::lock_guard lock(mutex); sleeps.push_back(ms); now += ms; };
    }
    void advance(uint64_t ms) { std::lock_guard lock(mutex); now += ms; }
    size_t sleepCount() { std::lock_guard lock(mutex); return sleeps.size(); }
};

/// The object under test lists the tickets that wrote it, comma-separated, so order is visible.
CasHotKeys::Decide appendTicket(int ticket)
{
    return [ticket](const std::optional<Object> & current) -> std::optional<String>
    {
        if (!current)
            return std::to_string(ticket);
        return current->bytes + "," + std::to_string(ticket);
    };
}

uint64_t counter(ProfileEvents::Event event)
{
    return ProfileEvents::global_counters[event].load();
}

/// A one-shot gate a write hook parks on: the first write of the key waits here until the test
/// releases it; every later write passes.
struct ParkFirstWrite
{
    std::latch parked{1};
    std::latch release{1};
    std::atomic<int> seen{0};

    void install(CountingBackend & backend, const String & key)
    {
        backend.onBeforeWrite(key, [this]
        {
            if (seen.fetch_add(1) != 0)
                return;
            parked.count_down();
            release.wait();
        });
    }
};

std::exception_ptr s3Error(Aws::S3::S3Errors code, const String & name)
{
    return std::make_exception_ptr(DB::S3Exception("the store answered " + name, code, name));
}

}

TEST(CASHotKeys, SubmissionsOfOneKeyAreSerializedInArrivalOrder)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    CasHotKeys hot_keys(0);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    constexpr int N = 4;
    ParkFirstWrite park;
    park.install(*backend, "k");

    std::vector<std::thread> threads;
    std::vector<std::optional<WriteResult>> results(N);
    std::deque<std::latch> go;   /// a deque: `std::latch` is neither copyable nor movable
    for (int i = 0; i < N; ++i)
        go.emplace_back(1);
    for (int i = 0; i < N; ++i)
    {
        threads.emplace_back([&, i]
        {
            go[i].wait();
            auto op = requests.admit();
            results[i] = hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(i + 1));
        });
    }
    /// The first holder is released into its write and parked there; every later thread is released
    /// only after its item is seen queued, so arrival order is the release order.
    go[0].count_down();
    park.parked.wait();
    for (int i = 1; i < N; ++i)
    {
        go[i].count_down();
        while (hot_keys.queueDepthForTest("k") < static_cast<size_t>(i + 1))
            std::this_thread::yield();
    }
    park.release.count_down();
    for (auto & t : threads)
        t.join();

    EXPECT_EQ(backend->writeCount("k"), static_cast<uint64_t>(N));
    EXPECT_EQ(backend->getCount("k"), static_cast<uint64_t>(N));   /// no cache in this task: a read per hold
    std::vector<Etag> etags;
    for (const auto & result : results)
    {
        ASSERT_TRUE(result.has_value());
        const auto * committed = std::get_if<Committed>(&*result);
        ASSERT_NE(committed, nullptr);
        etags.push_back(committed->etag);
    }
    for (size_t i = 1; i < etags.size(); ++i)
        EXPECT_FALSE(etags[i] == etags[i - 1]);
    DB::Cas::tests::expectBytes(*backend, "k", "1,2,3,4");
    auto reader = requests.admit();
    EXPECT_EQ(reader.read("k", Retry::standard())->etag, etags.back());
    EXPECT_EQ(hot_keys.laneCountForTest(), 0u);
    EXPECT_EQ(hot_keys.queueDepthForTest("k"), 0u);
}

TEST(CASHotKeys, ADecideRunsWithTheLaneMutexReleased)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    CasHotKeys hot_keys(0);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto op = requests.admit();
    /// `queueDepthForTest` takes the lane's mutex; a `decide` run under it would deadlock this test,
    /// which the runner's per-test timeout reports. The call is the assertion.
    WriteResult result = hot_keys.submit("k", op, op.freeze(Retry::standard()),
        [&](const std::optional<Object> &) -> std::optional<String>
        {
            EXPECT_EQ(hot_keys.queueDepthForTest("k"), 1u);
            return String("1");
        });
    EXPECT_TRUE(std::holds_alternative<Committed>(result));
}

TEST(CASHotKeys, AFailedEnqueueLeavesNoEmptyLaneBehind)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    CasHotKeys hot_keys(0);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto op = requests.admit();
    hot_keys.enter_after_lane_hook_for_test = [] { throw std::bad_alloc(); };
    EXPECT_THROW(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(1)), std::bad_alloc);
    EXPECT_EQ(hot_keys.laneCountForTest(), 0u);
    EXPECT_EQ(backend->writeCount("k"), 0u);
    hot_keys.enter_after_lane_hook_for_test = {};
    EXPECT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(1))));
}

TEST(CASHotKeys, ResultsAreTheEnginesOwn)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->setRefreshCredentialsResult(false);
    CasHotKeys hot_keys(0);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto op = requests.admit();
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(1))));

    /// The store refuses the bytes: the caller gets that `Refused`, at once.
    backend->failNextWriteWith("k", s3Error(Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));
    {
        WriteResult result = hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(2));
        ASSERT_TRUE(std::holds_alternative<Refused>(result));
    }
    /// A clean refused precondition with the store unchanged: `Conflict` carrying the occupant.
    backend->refuseNextWrite("k");
    {
        WriteResult result = hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(3));
        const auto * conflict = std::get_if<Conflict>(&result);
        ASSERT_NE(conflict, nullptr);
        EXPECT_TRUE(std::holds_alternative<Object>(conflict->seen));
        EXPECT_FALSE(conflict->any_ambiguous);
    }
    /// The resolve read fails at the transport under `once`: nothing observed.
    backend->refuseNextWrite("k");
    backend->failNextReadWith("k", std::make_exception_ptr(Poco::TimeoutException("resolve")));
    {
        WriteResult result = hot_keys.submit("k", op, op.freeze(Retry::once()), appendTicket(4));
        const auto * conflict = std::get_if<Conflict>(&result);
        ASSERT_NE(conflict, nullptr);
        EXPECT_TRUE(std::holds_alternative<NotObserved>(conflict->seen));
    }
    /// An ambiguous attempt whose resolve read fails at the transport under `once`: unresolved.
    backend->injectAmbiguousWrite("k");
    backend->failNextReadWith("k", std::make_exception_ptr(Poco::TimeoutException("resolve")));
    {
        WriteResult result = hot_keys.submit("k", op, op.freeze(Retry::once()), appendTicket(5));
        const auto * gave_up = std::get_if<GaveUp>(&result);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::Unresolved);
        EXPECT_TRUE(gave_up->sent_any);
    }
    /// The fence trips inside the hold, before the write: nothing sent.
    bool alive = true;
    auto fenced = requests.admit([&] { return alive; });
    {
        WriteResult result = hot_keys.submit("k", fenced, fenced.freeze(Retry::standard()),
            [&](const std::optional<Object> & current) -> std::optional<String>
            {
                alive = false;
                return current->bytes + ",6";
            });
        const auto * gave_up = std::get_if<GaveUp>(&result);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
        EXPECT_FALSE(gave_up->sent_any);
    }
    /// The fence trips after the landed write: the object carries the ticket, the caller is told so.
    alive = true;
    backend->onWriteCommitted("k", [&] { alive = false; });
    {
        WriteResult result = hot_keys.submit("k", fenced, fenced.freeze(Retry::standard()), appendTicket(7));
        const auto * gave_up = std::get_if<GaveUp>(&result);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
        EXPECT_TRUE(gave_up->sent_any);
        DB::Cas::tests::expectBytes(*backend, "k", "1,7");
    }
    backend->onWriteCommitted("k", {});
    /// The engine call throws a local fault: it reaches the caller and the key is handed over.
    backend->failNextWriteWith("k", std::make_exception_ptr(std::logic_error("local")));
    EXPECT_THROW(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(8)), std::logic_error);
    EXPECT_EQ(hot_keys.laneCountForTest(), 0u);
    EXPECT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(9))));
}

TEST(CASHotKeys, ABaseReadThatFailsGivesUpAsReadModifyWriteDoes)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->setAttemptTimeoutMs(1000);
    CasHotKeys hot_keys(0);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto op = requests.admit();
    (void)orThrow(op.create("k", "1", Retry::standard()), "seed");

    /// Enough armed failures to outlast a standard window: the read loop gives up at its deadline.
    for (int i = 0; i < 64; ++i)
        backend->failNextReadWith("k", std::make_exception_ptr(Poco::TimeoutException("read")));
    WriteResult lane = hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(2));
    for (int i = 0; i < 64; ++i)
        backend->failNextReadWith("k", std::make_exception_ptr(Poco::TimeoutException("read")));
    WriteResult verb = op.readModifyWrite("k", appendTicket(2), Retry::standard());

    const auto * a = std::get_if<GaveUp>(&lane);
    const auto * b = std::get_if<GaveUp>(&verb);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->why, b->why);
    EXPECT_EQ(a->deadline_source, b->deadline_source);
    EXPECT_EQ(a->sent_any, b->sent_any);
    EXPECT_FALSE(a->sent_any);
    EXPECT_EQ(a->last_seen.index(), b->last_seen.index());

    /// A fence that refuses the read's own reservation, and nothing smaller: the wait step passes
    /// (it asks for zero), the base read is refused before its first attempt.
    bool refuse_reservations = false;
    Fence fence{[] { return uint64_t{0}; },
                [&](uint64_t, uint64_t needed) { return refuse_reservations && needed > 0 ? Fence::Admit::LostOrRearmed : Fence::Admit::Ok; },
                [](uint64_t) {}};
    CasRequests fenced_requests(backend, fence, clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto fenced = fenced_requests.admit();
    refuse_reservations = true;
    WriteResult result = hot_keys.submit("k", fenced, fenced.freeze(Retry::standard()), appendTicket(3));
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
    EXPECT_FALSE(gave_up->sent_any);
}

TEST(CASHotKeys, WaitersLeaveOnTheirOwnFenceLeaseAndDeadline)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    CasHotKeys hot_keys(0);
    bool lease_spent = false;
    Fence fence{[] { return uint64_t{0}; },
                [&](uint64_t, uint64_t) { return lease_spent ? Fence::Admit::NoBudget : Fence::Admit::Ok; },
                [](uint64_t) {}};
    CasRequests requests(backend, fence, clock.nowFn(), clock.sleepFn(), &hot_keys);
    ParkFirstWrite park;
    park.install(*backend, "k");

    std::thread holder([&]
    {
        auto op = requests.admit();
        (void)hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(1));
    });
    park.parked.wait();

    const auto gave_up_before = counter(ProfileEvents::CASRequestGaveUp);
    /// A waiter whose own window ends while the holder is parked.
    std::optional<WriteResult> by_deadline;
    std::thread deadline_waiter([&]
    {
        auto op = requests.admit();
        by_deadline = hot_keys.submit("k", op, op.freeze(Retry::within(500)), appendTicket(2));
    });
    /// A waiter whose task stops.
    std::atomic<bool> alive{true};
    std::optional<WriteResult> by_liveness;
    std::thread liveness_waiter([&]
    {
        auto op = requests.admit([&] { return alive.load(); });
        by_liveness = hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(3));
    });
    while (hot_keys.queueDepthForTest("k") < 3)
        std::this_thread::yield();

    clock.advance(600);
    deadline_waiter.join();
    alive = false;
    liveness_waiter.join();
    {
        const auto * gave_up = std::get_if<GaveUp>(&*by_deadline);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::Deadline);
        EXPECT_EQ(gave_up->deadline_source, GaveUp::Source::Policy);
        EXPECT_FALSE(gave_up->sent_any);
        EXPECT_EQ(gave_up->attempts_sent, 0u);
        EXPECT_TRUE(std::holds_alternative<NotObserved>(gave_up->last_seen));
    }
    {
        const auto * gave_up = std::get_if<GaveUp>(&*by_liveness);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
        EXPECT_FALSE(gave_up->sent_any);
    }
    /// A waiter whose lease budget is gone, and whose task has stopped at the same slice: the lease
    /// speaks first, as the engine's own gate orders it.
    std::optional<WriteResult> by_lease;
    std::thread lease_waiter([&]
    {
        auto op = requests.admit([&] { return alive.load(); });
        by_lease = hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(4));
    });
    while (hot_keys.queueDepthForTest("k") < 2)
        std::this_thread::yield();
    lease_spent = true;
    lease_waiter.join();
    {
        const auto * gave_up = std::get_if<GaveUp>(&*by_lease);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::Deadline);
        EXPECT_EQ(gave_up->deadline_source, GaveUp::Source::Lease);
    }
    EXPECT_EQ(counter(ProfileEvents::CASRequestGaveUp) - gave_up_before, 3u);
    EXPECT_EQ(hot_keys.queueDepthForTest("k"), 1u) << "only the parked holder remains";
    EXPECT_EQ(backend->writeCount("k"), 1u) << "no second write started";

    lease_spent = false;
    park.release.count_down();
    holder.join();
    DB::Cas::tests::expectBytes(*backend, "k", "1");
    EXPECT_EQ(hot_keys.laneCountForTest(), 0u);
}

TEST(CASHotKeys, AThrottledHolderKeepsTheWaitersQueuedThroughItsBackoff)
{
    SyncClock clock;
    auto inner = std::make_shared<CountingBackend>();
    /// The first request naming each key is refused with a 429, so the holder's first `PUT` is
    /// ambiguous and reissued after the growing schedule; the waiters' windows outlast it.
    auto backend = std::make_shared<ThrottlingBackend>(inner, ThrottlingBackend::Mode::FirstPerKey, 1, 429);
    CasHotKeys hot_keys(0);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);

    std::vector<std::thread> threads;
    std::vector<std::optional<WriteResult>> results(3);
    for (int i = 0; i < 3; ++i)
        threads.emplace_back([&, i]
        {
            auto op = requests.admit();
            results[i] = hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(i + 1));
        });
    for (auto & t : threads)
        t.join();

    for (const auto & result : results)
        EXPECT_TRUE(std::holds_alternative<Committed>(*result));
    EXPECT_GE(clock.sleepCount(), 1u) << "the throttled write slept its backoff inside the hold";
    EXPECT_EQ(inner->writeCount("k"), 3u);
    EXPECT_EQ(hot_keys.laneCountForTest(), 0u);
}
```

- [ ] **Step 2: Build to verify the tests fail**

BUILD(`build_task2_red.log`), ANALYZE(`build_task2_red.log`). Expected: errors naming `CasHotKeys.h` (missing) and the five-argument `CasRequests` constructor.

- [ ] **Step 3: Write `CasHotKeys.h`**

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasWriteResult.h>
#include <Common/CacheBase.h>
#include <base/types.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace DB::Cas
{

class CasOperation;

/// One conditional write in flight per key from the operations that write through here, in arrival
/// order, plus the last object this pool knows per key so the next write needs no read.
///
/// Compare-and-swap is needed only against other servers; inside one server every writer of a shared
/// key used to race every other, and each lost race cost a read, a refused write, a resolve read and a
/// growing sleep. `submit` takes a FIFO ticket for the key, waits its turn re-checking the caller's
/// own fence, lease and deadline, obtains a base (the cache's object, else the engine's own read),
/// runs the caller's `decide` on it, lands the candidate through the engine's `replace` or `create`
/// on the caller's own operation and thread, and returns the engine's result unchanged. The caller
/// keeps the retry loop: a `Conflict` is a lost race against another server, and the caller submits
/// again after `Retry::conflictBackoff`.
///
/// The cache is a hint and never a source of truth: every write against it is conditional on its
/// etag, so a stale entry costs one 412 and one resolve read; and a verdict a `decide` renders on it
/// (a refusal by exception, or "nothing to write") is never delivered, because a refusal without a
/// write is the one thing a 412 cannot correct -- the entry is dropped, the key is read, and the
/// `decide` runs again on the read. The store's answer to a write decided on a hint is delivered
/// whatever it is: it is a fact about the store, and the caller's ordinary retry learns the rest.
///
/// One instance per pool, shared by its three request planes; a `CasRequests` built without one owns
/// a private instance with no cache. Every callback (`decide`, a `Liveness` closure, a backend hook)
/// runs with `mutex` released: the mutex is a leaf that calls nothing.
class CasHotKeys
{
public:
    /// `cache_budget_bytes` bounds the remembered objects; 0 disables the cache, and every hold reads.
    explicit CasHotKeys(uint64_t cache_budget_bytes);
    ~CasHotKeys();
    CasHotKeys(const CasHotKeys &) = delete;
    CasHotKeys & operator=(const CasHotKeys &) = delete;

    /// The caller's mutation of `key`, the engine's own decide shape: the candidate bytes to write over
    /// `base`, nothing (`Declined`), or a refusal by exception. `base` is absent when the key does not
    /// exist; a caller that refuses to bootstrap throws there. A `decide` may be run twice, and a
    /// later run on a fresh read is the decision that counts; it issues no write through this lane;
    /// its reads go through `op`; it reads `base->bytes` and never `base->etag`.
    using Decide = std::function<std::optional<String>(const std::optional<Object> &)>;

    /// One hold on `key`: wait for the turn, obtain a base, run `decide`, one engine write, remember.
    /// `policy` is frozen at entry, so time in the queue spends the caller's window. Returns the
    /// engine's own result for that write, in class and content, and propagates a `decide`'s
    /// exception as `readModifyWrite` does, never from a cached base.
    WriteResult submit(const String & key, CasOperation & op, const Retry & policy, const Decide & decide);

    /// Items in the key's queue, the holder included; 0 for a key with no lane.
    size_t queueDepthForTest(const String & key) const;
    /// Lanes in existence: a lane lives while its queue holds an item.
    size_t laneCountForTest() const;
    /// Entries the cache holds.
    size_t cacheEntriesForTest() const;

    /// TEST SEAM: runs under `mutex` after the key's lane was found or created and before the item
    /// is queued; a throw here is the enqueue's allocation failing.
    std::function<void()> enter_after_lane_hook_for_test;
    /// TEST SEAM: runs inside `remember` before the cache is filled; a throw here is the fill failing.
    std::function<void()> cache_fill_hook_for_test;

private:
    /// One queued submission, on its caller's stack: the caller's own guard is the only thing that
    /// removes it, so nothing here outlives the stack it lives on.
    struct Item
    {
        uint64_t ticket;
    };
    /// One key. Created on first use, erased when its queue empties; referenced only by threads whose
    /// item is in its queue.
    struct Lane
    {
        std::deque<Item *> queue;                    /// guarded by `mutex`; the front is the holder
        std::optional<uint64_t> holder_since_ms;     /// guarded by `mutex`; for the log line
        std::condition_variable cv;
    };
    /// A remembered object together with its key's size, so the weight below is never zero.
    struct Remembered
    {
        Object object;
        size_t key_bytes;
    };
    struct RememberedWeight
    {
        size_t operator()(const Remembered & remembered) const;
    };
    using Cache = CacheBase<String, Remembered, std::hash<String>, RememberedWeight>;

    WriteResult hold(const String & key, CasOperation & op, const Retry & policy, const Retry::Bound & bound,
                     const Decide & decide);
    void leave(const String & key, Item & item, bool entered_hold, uint64_t entered_ms, CasOperation & op) noexcept;

    std::optional<Object> cached(const String & key) const;
    void remember(const String & key, Object object) noexcept;
    void forget(const String & key) noexcept;

    mutable std::mutex mutex;                        /// `mutable` for the const test seams
    uint64_t next_ticket = 0;                        /// guarded by `mutex`; the holder's identity in the log line
    std::unordered_map<String, Lane> lanes;          /// guarded by `mutex`
    const uint64_t cache_budget_bytes;
    /// The last known object per key. Its synchronization is its own; it is read and written with
    /// `mutex` released. Null when the budget is 0.
    std::unique_ptr<Cache> cache;
};

}
```

- [ ] **Step 4: Write `CasHotKeys.cpp`**

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>

#include <Common/CurrentMetrics.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/logger_useful.h>

#include <algorithm>
#include <chrono>

namespace ProfileEvents
{
    extern const Event CASHotKeyQueueWaitMicroseconds;
    extern const Event CASHotKeyCacheStarts;
    extern const Event CASHotKeyReadStarts;
    extern const Event CASHotKeyCacheVerdictsReread;
}

namespace CurrentMetrics
{
    extern const Metric CASHotKeyCacheBytes;
    extern const Metric CASHotKeyCacheEntries;
}

namespace DB::Cas
{

namespace
{

GaveUp::Source sourceFor(const Retry::Bound & bound)
{
    return bound.lease_bound ? GaveUp::Source::Lease : GaveUp::Source::Policy;
}

/// The wait slice: how late, at most, a waiter notices its own fence or deadline. A handover wakes it
/// at once through the lane's condition variable.
constexpr auto kWaitSlice = std::chrono::milliseconds(200);

}

size_t CasHotKeys::RememberedWeight::operator()(const Remembered & remembered) const
{
    /// Never zero: the key, the incarnation and the containers weigh something even when the object
    /// is empty, so the byte budget bounds the entry count as well as the bytes.
    return remembered.key_bytes + remembered.object.bytes.size() + remembered.object.etag.render().size() + 64;
}

CasHotKeys::CasHotKeys(uint64_t cache_budget_bytes_)
    : cache_budget_bytes(cache_budget_bytes_)
    , cache(cache_budget_bytes_ == 0
            ? nullptr
            : std::make_unique<Cache>("LRU", CurrentMetrics::CASHotKeyCacheBytes, CurrentMetrics::CASHotKeyCacheEntries,
                                      cache_budget_bytes_))
{
}

CasHotKeys::~CasHotKeys() = default;

WriteResult CasHotKeys::submit(const String & key, CasOperation & op, const Retry & policy, const Decide & decide)
{
    /// Frozen here so that the queue wait and the write share one deadline; a policy already frozen
    /// by the caller's loop is returned unchanged.
    const Retry frozen = op.freeze(policy);
    const Retry::Bound bound = frozen.bind(op.owner.now_ms());
    const uint64_t entered_ms = op.owner.now_ms();

    Item item{};
    {
        std::lock_guard lock(mutex);
        auto [it, inserted] = lanes.try_emplace(key);
        try
        {
            if (enter_after_lane_hook_for_test)
                enter_after_lane_hook_for_test();
            item.ticket = ++next_ticket;
            it->second.queue.push_back(&item);
        }
        catch (...)
        {
            /// A lane that holds nothing is nobody's; erasing it puts the map back as it was.
            if (inserted)
                lanes.erase(it);
            throw;
        }
    }

    /// From here the item is in the queue, and this guard is the only thing that removes it: on every
    /// exit, normal or by unwinding, it runs the leave step, which allocates nothing and cannot throw.
    bool entered_hold = false;
    struct Leave
    {
        CasHotKeys & owner;
        const String & key;
        Item & item;
        const bool & entered_hold;
        uint64_t entered_ms;
        CasOperation & op;
        ~Leave() noexcept { owner.leave(key, item, entered_hold, entered_ms, op); }
    } guard{*this, key, item, entered_hold, entered_ms, op};

    for (;;)
    {
        /// Outside the mutex: the engine's own admission in the engine's order (the fence generation,
        /// the lease budget, then the caller's liveness, which the engine reports as a lost fence),
        /// then the caller's bound. A lost fence is therefore never reported as a policy deadline, and
        /// an exhausted lease is reported as the lease.
        std::optional<WriteResult> leaving;
        CasOperation::WriteState nothing_sent;
        switch (op.gate(0))
        {
            case CasOperation::Gate::FenceLost:
                leaving = op.gaveUp(GaveUp::Why::FenceLost, sourceFor(bound), nothing_sent);
                break;
            case CasOperation::Gate::NoBudget:
                leaving = op.gaveUp(GaveUp::Why::Deadline, GaveUp::Source::Lease, nothing_sent);
                break;
            case CasOperation::Gate::Ok:
                break;
        }
        if (!leaving && !op.fits(0, bound))
            leaving = op.gaveUp(GaveUp::Why::Deadline, sourceFor(bound), nothing_sent);

        std::unique_lock lock(mutex);
        if (leaving)
            return std::move(*leaving);   /// the lock is released before the guard erases the item
        Lane & lane = lanes.at(key);
        if (lane.queue.front() == &item)
        {
            lane.holder_since_ms = op.owner.now_ms();
            entered_hold = true;
            break;
        }
        lane.cv.wait_for(lock, kWaitSlice);
    }
    ProfileEvents::increment(ProfileEvents::CASHotKeyQueueWaitMicroseconds, (op.owner.now_ms() - entered_ms) * 1000);
    return hold(key, op, frozen, bound, decide);
}

void CasHotKeys::leave(const String & key, Item & item, bool entered_hold, uint64_t entered_ms, CasOperation & op) noexcept
{
    const uint64_t now = op.owner.now_ms();
    /// The front item's ticket and how long it has held, when this caller left at its own bound behind
    /// it: what makes a stuck holder visible while it is stuck.
    std::optional<std::pair<uint64_t, uint64_t>> stuck_behind;
    {
        std::lock_guard lock(mutex);
        auto it = lanes.find(key);
        Lane & lane = it->second;
        lane.queue.erase(std::find(lane.queue.begin(), lane.queue.end(), &item));
        if (entered_hold)
            lane.holder_since_ms.reset();
        else if (!lane.queue.empty() && lane.holder_since_ms)
            stuck_behind = std::pair{lane.queue.front()->ticket, now - *lane.holder_since_ms};
        if (lane.queue.empty())
            lanes.erase(it);
        else
            lane.cv.notify_all();
    }
    try
    {
        if (!entered_hold)
            ProfileEvents::increment(ProfileEvents::CASHotKeyQueueWaitMicroseconds, (now - entered_ms) * 1000);
        if (stuck_behind)
            LOG_WARNING(getLogger("CasHotKeys"),
                "hot key '{}': a writer left at its own bound while ticket {} has held the key for {} ms",
                key, stuck_behind->first, stuck_behind->second);
    }
    catch (...)
    {
        tryLogCurrentException("CasHotKeys");
    }
}

WriteResult CasHotKeys::hold(const String & key, CasOperation & op, const Retry & policy, const Retry::Bound & bound,
                             const Decide & decide)
{
    CasOperation::WriteState state;
    std::optional<Object> base;
    bool from_cache = false;
    /// A single-attempt submission never starts from a hint: its one attempt is on fresh state, as
    /// the engine's own verb reads before it.
    if (!policy.single_attempt)
    {
        if (auto remembered = cached(key))
        {
            base = std::move(remembered);
            from_cache = true;
            ProfileEvents::increment(ProfileEvents::CASHotKeyCacheStarts);
        }
    }
    const auto read_base = [&]() -> std::optional<WriteResult>
    {
        ProfileEvents::increment(ProfileEvents::CASHotKeyReadStarts);
        CasOperation::Resolved resolved = op.observe(key, policy, bound);
        state.last_seen = resolved.seen;
        base.reset();
        if (const auto * object = std::get_if<Object>(&state.last_seen))
            base = *object;
        else if (!std::holds_alternative<ProvenAbsent>(state.last_seen))
            return op.gaveUpAfterFailedObservation(resolved.stop, state, bound);
        return std::nullopt;
    };
    if (!from_cache)
        if (auto refused = read_base())
            return *refused;

    std::optional<String> candidate;
    bool verdict_on_hint = false;
    try
    {
        candidate = decide(base);
        verdict_on_hint = from_cache && !candidate;
    }
    catch (...)
    {
        if (!from_cache)
            throw;
        verdict_on_hint = true;
    }
    if (verdict_on_hint)
    {
        /// A verdict rendered on a hint proves only that a hint is not a proof: the entry is dropped,
        /// the key is read, and what the `decide` does on the read is the result. A read the caller's
        /// own gate refuses is that give-up, as for any caller that cannot read.
        forget(key);
        ProfileEvents::increment(ProfileEvents::CASHotKeyCacheVerdictsReread);
        if (auto refused = read_base())
            return *refused;
        from_cache = false;
        candidate = decide(base);
    }
    if (!candidate)
    {
        if (base)
            remember(key, *base);
        return Declined{state.last_seen};
    }

    WriteResult result = [&]
    {
        try
        {
            return base ? op.replace(key, *candidate, base->etag, policy) : op.create(key, *candidate, policy);
        }
        catch (...)
        {
            forget(key);
            throw;
        }
    }();
    std::visit(detail::Overload{
        [&](const Committed & committed) { remember(key, Object{*candidate, committed.etag}); },
        [&](const Conflict & conflict)
        {
            if (const auto * object = std::get_if<Object>(&conflict.seen))
                remember(key, *object);
            else
                forget(key);
        },
        [&](const Refused &) { forget(key); },
        [&](const GaveUp & gave_up) { if (gave_up.sent_any) forget(key); },
        [&](const Declined &) {}}, result);
    return result;
}

std::optional<Object> CasHotKeys::cached(const String & key) const
{
    if (!cache)
        return std::nullopt;
    if (auto hit = cache->get(key))
        return hit->object;
    return std::nullopt;
}

void CasHotKeys::remember(const String & key, Object object) noexcept
{
    if (!cache)
        return;
    try
    {
        if (cache_fill_hook_for_test)
            cache_fill_hook_for_test();
        auto remembered = std::make_shared<Remembered>(Remembered{std::move(object), key.size()});
        /// An object above the budget is not a hint worth evicting everything else for.
        if (RememberedWeight{}(*remembered) > cache_budget_bytes)
        {
            forget(key);
            return;
        }
        cache->set(key, remembered);
    }
    catch (...)
    {
        /// A hint that could not be stored is no hint: the next hold reads. The result that led here
        /// stands; a failed fill must never replace it.
        tryLogCurrentException("CasHotKeys");
        forget(key);
    }
}

void CasHotKeys::forget(const String & key) noexcept
{
    if (!cache)
        return;
    try
    {
        cache->remove(key);
    }
    catch (...)
    {
        tryLogCurrentException("CasHotKeys");
    }
}

size_t CasHotKeys::queueDepthForTest(const String & key) const
{
    std::lock_guard lock(mutex);
    const auto it = lanes.find(key);
    return it == lanes.end() ? 0 : it->second.queue.size();
}

size_t CasHotKeys::laneCountForTest() const
{
    std::lock_guard lock(mutex);
    return lanes.size();
}

size_t CasHotKeys::cacheEntriesForTest() const
{
    return cache ? cache->count() : 0;
}

}
```

`detail::Overload` is the visitor helper in `CasWriteResult.h`. The `cached`, `remember`, `forget` bodies are complete here so that Task 3 needs no engine edits, only tests; with a budget of 0 they are no-ops and every hold reads.

- [ ] **Step 5: Wire `CasRequests` and `CasOperation`**

`CasRequests.h`: add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.h>` to the includes. The constructor becomes:

```cpp
    /// `now_ms` defaults to `CLOCK_BOOTTIME` milliseconds -- the same clock a mount lease deadline is
    /// expressed on, so `Retry::untilLeaseSafe` and this engine compare like with like. `sleep_ms`
    /// defaults to a real sleep. `attempt_reservation_ms` is taken from the backend's own attempt
    /// timeout: it is what the engine reserves before it starts anything. `hot_keys` is the pool's
    /// write lane, shared by its planes; without one this object owns a private lane with no cache,
    /// so a write through it costs today's read and write.
    CasRequests(BackendPtr backend_, Fence fence_,
                std::function<uint64_t()> now_ms_ = {}, std::function<void(uint64_t)> sleep_ms_ = {},
                CasHotKeys * hot_keys_ = nullptr);
```

Private members, declared in this order after `attempt_reservation_ms`:

```cpp
    /// The private lane of a `CasRequests` built without a pool; null when `hot_keys` is the pool's.
    std::unique_ptr<CasHotKeys> own_hot_keys;
    CasHotKeys * hot_keys;
```

In `CasOperation`'s public section, after `admitted`:

```cpp
    /// The write lane for keys several writers of this pool share.
    CasHotKeys & hotKeys() const { return *owner.hot_keys; }
```

In `CasOperation`'s private section, beside `friend class CasRequests;`:

```cpp
    /// The lane waits on this operation's own admission and reads and writes through its verbs; it
    /// needs the gate, the reservation, the resolve read and the give-up helpers, never the transport.
    friend class CasHotKeys;
```

`CasRequests.cpp`, the constructor:

```cpp
CasRequests::CasRequests(BackendPtr backend_, Fence fence_,
                         std::function<uint64_t()> now_ms_, std::function<void(uint64_t)> sleep_ms_,
                         CasHotKeys * hot_keys_)
    : backend(std::move(backend_))
    , fence(std::move(fence_))
    , now_ms(now_ms_ ? std::move(now_ms_) : std::function<uint64_t()>(bootClockMs))
    , sleep_ms(sleep_ms_ ? std::move(sleep_ms_) : std::function<void(uint64_t)>(sleepForMilliseconds))
    , attempt_reservation_ms(backend->attemptTimeoutMs())
    , own_hot_keys(hot_keys_ ? nullptr : std::make_unique<CasHotKeys>(0))
    , hot_keys(hot_keys_ ? hot_keys_ : own_hot_keys.get())
{
}
```

`src/Common/ProfileEvents.cpp`, after `CASRefQueueWaitMicroseconds` (line 802):

```cpp
    M(CASHotKeyQueueWaitMicroseconds, "Total time CAS writers of a shared key spent queued in the hot-key lane before holding it or leaving, in microseconds. A rising value with a flat write rate means the holder is slow, not the store.", ValueType::Microseconds) \
    M(CASHotKeyCacheStarts, "Number of hot-key lane holds that started from the pool's last known object instead of a read.", ValueType::Number) \
    M(CASHotKeyReadStarts, "Number of hot-key lane holds that started from a read of the key.", ValueType::Number) \
    M(CASHotKeyCacheVerdictsReread, "Number of verdicts (a refusal or a decline) a hot-key lane decide rendered on a cached object and that were re-rendered on a fresh read instead of delivered.", ValueType::Number) \
```

`src/Common/CurrentMetrics.cpp`, after `CASManifestDecodeCacheEntries` (line 237):

```cpp
    M(CASHotKeyCacheBytes, "Bytes retained by the CA hot-key lane's cache of last known objects") \
    M(CASHotKeyCacheEntries, "Entries retained by the CA hot-key lane's cache of last known objects") \
```

- [ ] **Step 6: Build and run**

BUILD(`build_task2.log`), ANALYZE(`build_task2.log`). TEST(`CASHotKeys.*:CASRequests.*`, `test_task2_lane.log`), ANALYZE(`test_task2_lane.log`). Expected: all pass. `ADecideRunsWithTheLaneMutexReleased` passing within the runner's timeout is the mutex assertion.

- [ ] **Step 7: Commit**

COMMIT(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp src/Common/ProfileEvents.cpp src/Common/CurrentMetrics.cpp src/Disks/tests/gtest_cas_hot_keys.cpp`, `ca-engine: the hot-key lane, one write in flight per pool and key, above the request engine`)

---

### Task 3: The cache under one rule {#task-3}

**Files:**
- Modify (tests only; the code landed in Task 2): `src/Disks/tests/gtest_cas_hot_keys.cpp`

**Interfaces:**
- Consumes: `CasHotKeys(cache_budget_bytes > 0)`, `cacheEntriesForTest`, `cache_fill_hook_for_test`.
- Produces: the behaviour Task 4's catalog tests rely on: a hold after a landed write reads nothing; a verdict on a cached base is re-rendered on a read; a `single_attempt` submission reads.

- [ ] **Step 1: Write the failing tests**

Append to `src/Disks/tests/gtest_cas_hot_keys.cpp`:

```cpp
TEST(CASHotKeys, TheNextHoldStartsFromTheLandedObjectWithoutARead)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    CasHotKeys hot_keys(16ULL << 20);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto op = requests.admit();
    const auto cache_starts_before = counter(ProfileEvents::CASHotKeyCacheStarts);

    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(1))));
    EXPECT_EQ(backend->getCount("k"), 1u);
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(2))));
    EXPECT_EQ(backend->getCount("k"), 1u) << "the second hold started from the cache";
    EXPECT_EQ(counter(ProfileEvents::CASHotKeyCacheStarts) - cache_starts_before, 1u);
    DB::Cas::tests::expectBytes(*backend, "k", "1,2");

    /// Under `once` the one attempt is on fresh state: a read, no cached start.
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::once()), appendTicket(3))));
    EXPECT_EQ(backend->getCount("k"), 2u);
}

TEST(CASHotKeys, AnExternalWriterCostsOneResolveReadAndOneRetry)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    CasHotKeys hot_keys(16ULL << 20);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto op = requests.admit();
    CasRequests external_requests = DB::Cas::tests::openRequestsForTest(backend);
    auto external = external_requests.admit();
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(1))));

    const auto current = external.read("k", Retry::standard());
    (void)orThrow(external.replace("k", "E", current->etag, Retry::standard()), "external");
    const uint64_t gets_before = backend->getCount("k");
    const uint64_t writes_before = backend->writeCount("k");

    /// The caller's loop: submit, and on a conflict submit again after the flat pause.
    std::optional<WriteResult> result;
    for (int i = 0; i < 3 && !result; ++i)
    {
        WriteResult attempt = hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(2));
        if (std::holds_alternative<Conflict>(attempt))
            op.pause(Retry::conflictBackoff());
        else
            result = std::move(attempt);
    }
    ASSERT_TRUE(result && std::holds_alternative<Committed>(*result));
    EXPECT_EQ(backend->getCount("k") - gets_before, 1u) << "one resolve read";
    EXPECT_EQ(backend->writeCount("k") - writes_before, 2u) << "one refused write, one that landed";
    DB::Cas::tests::expectBytes(*backend, "k", "E,2");
    /// The submission after that starts from the cache.
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(3))));
    EXPECT_EQ(backend->getCount("k") - gets_before, 1u);
}

TEST(CASHotKeys, MalformedBytesRepairedExternallyRaiseNoCorruptionVerdict)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    CasHotKeys hot_keys(16ULL << 20);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto op = requests.admit();
    CasRequests external_requests = DB::Cas::tests::openRequestsForTest(backend);
    auto external = external_requests.admit();
    /// A decide that refuses bytes it cannot decode, as the catalog's does.
    const CasHotKeys::Decide strict = [](const std::optional<Object> & current) -> std::optional<String>
    {
        if (current && current->bytes.find("garbage") != String::npos)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "not a ticket list");
        return current ? current->bytes + ",9" : String("9");
    };
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), strict)));

    auto current = external.read("k", Retry::standard());
    const Etag garbage = *orThrow(external.replace("k", "garbage", current->etag, Retry::standard()), "break");
    /// The lane's next hold starts from its cache, loses to the garbage, and remembers the garbage
    /// its resolve read saw; the caller pauses and submits again.
    WriteResult first = hot_keys.submit("k", op, op.freeze(Retry::standard()), strict);
    ASSERT_TRUE(std::holds_alternative<Conflict>(first));
    (void)orThrow(external.replace("k", "9", garbage, Retry::standard()), "repair");
    const auto reread_before = counter(ProfileEvents::CASHotKeyCacheVerdictsReread);
    /// The verdict on the cached garbage is not delivered: one read, and the decide lands on the repair.
    WriteResult second = hot_keys.submit("k", op, op.freeze(Retry::standard()), strict);
    ASSERT_TRUE(std::holds_alternative<Committed>(second));
    EXPECT_EQ(counter(ProfileEvents::CASHotKeyCacheVerdictsReread) - reread_before, 1u);
    DB::Cas::tests::expectBytes(*backend, "k", "9,9");
    /// And when the read is garbage too, that is the real corruption.
    current = external.read("k", Retry::standard());
    (void)orThrow(external.replace("k", "garbage", current->etag, Retry::standard()), "break again");
    (void)hot_keys.submit("k", op, op.freeze(Retry::standard()), strict);   /// conflict: the cache now holds garbage
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { (void)hot_keys.submit("k", op, op.freeze(Retry::standard()), strict); });
}

TEST(CASHotKeys, ADeclineOnAHintIsRerenderedOnARead)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    CasHotKeys hot_keys(16ULL << 20);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto op = requests.admit();
    CasRequests external_requests = DB::Cas::tests::openRequestsForTest(backend);
    auto external = external_requests.admit();
    /// Writes "1" once and declines while the object already says "1".
    const CasHotKeys::Decide idempotent = [](const std::optional<Object> & current) -> std::optional<String>
    {
        if (current && current->bytes == "1")
            return std::nullopt;
        return String("1");
    };
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), idempotent)));
    /// On a fresh read the decline is the caller's answer.
    {
        WriteResult result = hot_keys.submit("k", op, op.freeze(Retry::once()), idempotent);
        const auto * declined = std::get_if<Declined>(&result);
        ASSERT_NE(declined, nullptr);
        EXPECT_TRUE(std::holds_alternative<Object>(declined->seen));
    }
    /// An external writer replaces the object; the cached hint still says "1", so the decide would
    /// decline on it. The decline is not delivered: the lane reads and the decide writes.
    const auto current = external.read("k", Retry::standard());
    (void)orThrow(external.replace("k", "0", current->etag, Retry::standard()), "external");
    const uint64_t gets_before = backend->getCount("k");
    WriteResult result = hot_keys.submit("k", op, op.freeze(Retry::standard()), idempotent);
    ASSERT_TRUE(std::holds_alternative<Committed>(result));
    EXPECT_EQ(backend->getCount("k") - gets_before, 1u);
    DB::Cas::tests::expectBytes(*backend, "k", "1");
    /// On an absent key the decline names absence.
    WriteResult absent = hot_keys.submit("missing", op, op.freeze(Retry::standard()),
        [](const std::optional<Object> &) -> std::optional<String> { return std::nullopt; });
    const auto * declined = std::get_if<Declined>(&absent);
    ASSERT_NE(declined, nullptr);
    EXPECT_TRUE(std::holds_alternative<ProvenAbsent>(declined->seen));
}

TEST(CASHotKeys, TheCacheForgetsWhatItCannotVouchFor)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->setRefreshCredentialsResult(false);
    CasHotKeys hot_keys(16ULL << 20);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto op = requests.admit();
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(1))));
    const auto reads = [&] { return backend->getCount("k"); };

    /// Refused: dropped, the next hold reads.
    backend->failNextWriteWith("k", s3Error(Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));
    uint64_t before = reads();
    ASSERT_TRUE(std::holds_alternative<Refused>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(2))));
    EXPECT_EQ(reads(), before);
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(2))));
    EXPECT_EQ(reads(), before + 1);

    /// Unresolved after a send: dropped.
    backend->injectAmbiguousWrite("k");
    backend->failNextReadWith("k", std::make_exception_ptr(Poco::TimeoutException("resolve")));
    before = reads();
    ASSERT_TRUE(std::holds_alternative<GaveUp>(hot_keys.submit("k", op, op.freeze(Retry::once()), appendTicket(3))));
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(3))));
    EXPECT_EQ(reads(), before + 2);   /// the failed resolve read counted once, the next hold's read once

    /// An exception out of the write: dropped.
    backend->failNextWriteWith("k", std::make_exception_ptr(std::logic_error("local")));
    before = reads();
    EXPECT_THROW(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(4)), std::logic_error);
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(4))));
    EXPECT_EQ(reads(), before + 1);

    /// A give-up that sent nothing leaves the entry as it was: the next hold starts from it.
    bool alive = true;
    auto fenced = requests.admit([&] { return alive; });
    before = reads();
    WriteResult nothing_sent = hot_keys.submit("k", fenced, fenced.freeze(Retry::standard()),
        [&](const std::optional<Object> & current) -> std::optional<String> { alive = false; return current->bytes + ",5"; });
    ASSERT_TRUE(std::holds_alternative<GaveUp>(nothing_sent));
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(5))));
    EXPECT_EQ(reads(), before);

    /// A fill that throws after a landed write: the result stands, the next hold reads.
    hot_keys.cache_fill_hook_for_test = [] { throw std::bad_alloc(); };
    before = reads();
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(6))));
    hot_keys.cache_fill_hook_for_test = {};
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(7))));
    EXPECT_EQ(reads(), before + 1);
    DB::Cas::tests::expectBytes(*backend, "k", "1,2,3,4,5,6,7");
}

TEST(CASHotKeys, TheBudgetBoundsBytesAndEntries)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    /// Two entries of one-byte objects weigh 2 x (1 + 1 + etag + 64); a budget of one entry and a half
    /// holds one at a time.
    auto probe_op_requests = DB::Cas::tests::openRequestsForTest(backend);
    auto probe = probe_op_requests.admit();
    const size_t etag_bytes = orThrow(probe.create("probe", "x", Retry::standard()), "probe")->render().size();
    const uint64_t one_entry = 1 + 1 + etag_bytes + 64;
    CasHotKeys hot_keys(one_entry + one_entry / 2);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto op = requests.admit();
    const CasHotKeys::Decide one_byte = [](const std::optional<Object> &) -> std::optional<String> { return String("x"); };

    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("a", op, op.freeze(Retry::standard()), one_byte)));
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("b", op, op.freeze(Retry::standard()), one_byte)));
    EXPECT_EQ(hot_keys.cacheEntriesForTest(), 1u) << "the older entry was evicted";
    const uint64_t gets_a = backend->getCount("a");
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("a", op, op.freeze(Retry::standard()), one_byte)));
    EXPECT_EQ(backend->getCount("a"), gets_a + 1) << "the evicted key reads";

    /// An object above the budget is not stored.
    const CasHotKeys::Decide big = [&](const std::optional<Object> &) -> std::optional<String> { return String(one_entry * 2, 'y'); };
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("c", op, op.freeze(Retry::standard()), big)));
    const uint64_t gets_c = backend->getCount("c");
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("c", op, op.freeze(Retry::standard()), big)));
    EXPECT_EQ(backend->getCount("c"), gets_c + 1);

    /// Empty objects weigh their key and their allowance: N of them stay bounded by the budget.
    CasHotKeys small(4 * one_entry);
    CasRequests small_requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &small);
    auto small_op = small_requests.admit();
    const CasHotKeys::Decide empty = [](const std::optional<Object> &) -> std::optional<String> { return String(); };
    for (int i = 0; i < 40; ++i)
        ASSERT_TRUE(std::holds_alternative<Committed>(small.submit("e" + std::to_string(i), small_op, small_op.freeze(Retry::standard()), empty)));
    EXPECT_LE(small.cacheEntriesForTest(), 4u);
}

TEST(CASHotKeys, ACachedStartPastTheDeadlineSendsNothing)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->setAttemptTimeoutMs(1000);
    CasHotKeys hot_keys(16ULL << 20);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto op = requests.admit();
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(1))));

    /// The window fits the wait's zero reservation but not the write's two attempt envelopes.
    int decided = 0;
    const uint64_t writes_before = backend->writeCount("k");
    WriteResult result = hot_keys.submit("k", op, op.freeze(Retry::within(500)),
        [&](const std::optional<Object> & current) -> std::optional<String> { ++decided; return current->bytes + ",2"; });
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    EXPECT_EQ(gave_up->why, GaveUp::Why::Deadline);
    EXPECT_FALSE(gave_up->sent_any);
    EXPECT_EQ(decided, 1);
    EXPECT_EQ(backend->writeCount("k"), writes_before);
}

TEST(CASHotKeys, AnIdenticalCandidateLandedByAnotherServerIsTheEnginesCommit)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    CasHotKeys hot_keys(16ULL << 20);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto op = requests.admit();
    CasRequests external_requests = DB::Cas::tests::openRequestsForTest(backend);
    auto external = external_requests.admit();
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(1))));

    /// Another server lands exactly the candidate this hold will compute from its stale hint, and this
    /// hold's own refused write loses its answer. The resolve read finds the candidate's bytes under
    /// the moved incarnation: the engine's own rule calls that landed.
    const auto current = external.read("k", Retry::standard());
    const Etag theirs = *orThrow(external.replace("k", "1,2", current->etag, Retry::standard()), "identical");
    backend->injectAmbiguousWrite("k");
    WriteResult result = hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(2));
    const auto * committed = std::get_if<Committed>(&result);
    ASSERT_NE(committed, nullptr);
    EXPECT_TRUE(committed->resolved_by_read);
    EXPECT_TRUE(committed->etag == theirs);
    DB::Cas::tests::expectBytes(*backend, "k", "1,2");
}
```

- [ ] **Step 2: Build and run**

BUILD(`build_task3.log`), ANALYZE(`build_task3.log`). TEST(`CASHotKeys.*`, `test_task3_lane.log`), ANALYZE(`test_task3_lane.log`). Expected: all pass on the Task 2 code. A failure here is a defect in Task 2's `hold`, `remember`, `forget` or `cached`; fix it in `CasHotKeys.cpp`, rebuild, rerun.

- [ ] **Step 3: Commit**

COMMIT(`src/Disks/tests/gtest_cas_hot_keys.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.cpp`, `ca-engine: the hot-key lane's cache pinned by tests — a verdict on a hint is never delivered, the store's answer always is`) — list `CasHotKeys.cpp` only if Step 2 changed it.

---

### Task 4: The catalog writes through the lane {#task-4}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.cpp` (`casUpdateImpl` ~152-190, `casAdmitEntry` ~302-330, `createNamespace`'s step-1 `try`)
- Test: `src/Disks/tests/gtest_cas_ref_catalog.cpp`

**Interfaces:**
- Consumes: `CasOperation::hotKeys().submit`, `Conflict::any_ambiguous`, `Retry::conflictBackoff`.
- Produces: no API change. `casUpdate`, `casAdmitEntry`, the five lifecycle functions keep their signatures, markers and outcomes; `createNamespace` returns `FencedOut` for a fence lost during step 1 instead of leaking the private marker.

- [ ] **Step 1: Write the failing tests**

Append to `src/Disks/tests/gtest_cas_ref_catalog.cpp`. The file's anonymous namespace already has `liveEntry`, `entryInState`, `WriteCountingBackend` (with `writes(key)` and `refuseNextWrite`), `noAuthorityRefresh`, `seedObject`; add these helpers to it:

```cpp
namespace ProfileEvents
{
    extern const Event CASHotKeyReadStarts;
    extern const Event CASHotKeyCacheVerdictsReread;
    extern const Event CASRequestConflictPause;
    extern const Event CASRequestReissue;
}

namespace
{

/// A pool's own engine: a `CasRequests` over the pool's shared lane with a cache, on a fake clock,
/// beside a plain `CasRequests` that stands for another server.
struct PoolAndExternal
{
    explicit PoolAndExternal(std::shared_ptr<CountingBackend> backend_)
        : backend(std::move(backend_))
        , hot_keys(16ULL << 20)
        , pool(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys)
        , external(DB::Cas::tests::openRequestsForTest(backend))
    {
    }
    std::shared_ptr<CountingBackend> backend;
    FakeClock clock;
    CasHotKeys hot_keys;
    CasRequests pool;
    CasRequests external;
};

uint64_t eventCount(ProfileEvents::Event event)
{
    return ProfileEvents::global_counters[event].load();
}

}
```

(`CountingBackend` and `FakeClock` come from `cas_test_helpers.h`; add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.h>` and `using DB::Cas::tests::CountingBackend; using DB::Cas::tests::FakeClock;` beside the file's other usings.)

The tests:

```cpp
TEST(CASRefCatalog, AStaleHintNeverProducesAFalseEntryChanged)
{
    /// The pool's last catalog write left "a" Creating; another server completed it to Live. The
    /// caller reads fresh, sees Live, and calls beginRemoving: the cached Creating row differs from
    /// what it observed and would say EntryChanged. That verdict is not delivered.
    PoolAndExternal f(std::make_shared<CountingBackend>());
    const Layout layout("p");
    CasOperation pool_op = f.pool.admit();
    CasRefCatalog::initializeEmptyForNewPool(pool_op, layout);
    const CatalogEntry creating = entryInState("a", NsState::Creating, 1);
    CasRefCatalog::casAdmitEntry(pool_op, layout, 1, creating);   /// through the lane: the cache holds Creating

    CasOperation other = f.external.admit();
    ASSERT_EQ(CasRefCatalog::completeCreation(other, layout, creating), CasRefCatalog::NamespaceCreationOutcome::Live);

    const CatalogEntry observed = CasRefCatalog::read(other, layout).catalog.entries.at(0);
    ASSERT_EQ(observed.state, NsState::Live);
    const uint64_t reads_before = f.backend->getCount(layout.refCatalogKey());
    const uint64_t rereads_before = eventCount(ProfileEvents::CASHotKeyCacheVerdictsReread);
    EXPECT_EQ(CasRefCatalog::beginRemoving(pool_op, layout, observed, 7), CasRefCatalog::BeginRemovingOutcome::Transitioned);
    EXPECT_EQ(f.backend->getCount(layout.refCatalogKey()) - reads_before, 1u) << "one lane read";
    EXPECT_EQ(eventCount(ProfileEvents::CASHotKeyCacheVerdictsReread) - rereads_before, 1u);
    EXPECT_EQ(CasRefCatalog::read(other, layout).catalog.entries.at(0).state, NsState::Removing);
}

TEST(CASRefCatalog, ATrueRefusalOnTheReadIsTheSameAsWithoutACache)
{
    /// The row really changed: the re-rendered verdict is the one a cache-less call renders.
    PoolAndExternal f(std::make_shared<CountingBackend>());
    const Layout layout("p");
    CasOperation pool_op = f.pool.admit();
    CasRefCatalog::initializeEmptyForNewPool(pool_op, layout);
    const CatalogEntry creating = entryInState("a", NsState::Creating, 1);
    CasRefCatalog::casAdmitEntry(pool_op, layout, 1, creating);

    CasOperation other = f.external.admit();
    ASSERT_EQ(CasRefCatalog::completeCreation(other, layout, creating), CasRefCatalog::NamespaceCreationOutcome::Live);
    const uint64_t writes_before = f.backend->writeCount(layout.refCatalogKey());
    const uint64_t reads_before = f.backend->getCount(layout.refCatalogKey());
    /// The caller believes it observed a Creating row under another incarnation. The pool's hint
    /// (Creating, incarnation 1) differs from it, so the verdict is rendered on the hint and not
    /// delivered; the read (Live, incarnation 1) differs from it too, and that verdict is the answer,
    /// the one a cache-less call renders: one read, no write. (Had the hint matched `observed`, the
    /// write path would run instead: a 412 and a resolve read, which the lane tests cover.)
    const CatalogEntry observed_elsewhere = entryInState("a", NsState::Creating, 2);
    EXPECT_EQ(CasRefCatalog::cancelStalledCreating(pool_op, layout, observed_elsewhere, [](const CreatorFence &) { return true; }),
              CasRefCatalog::StalledCreatingCancelOutcome::EntryChanged);
    EXPECT_EQ(f.backend->writeCount(layout.refCatalogKey()), writes_before) << "no write";
    EXPECT_EQ(f.backend->getCount(layout.refCatalogKey()) - reads_before, 1u) << "one lane read";
}
```

The remaining rows of the matrix follow the two shapes above exactly; write one test per row, named `AStaleHint...` / `ATrueRefusal...` with the function's name, using this table. In every row the cached row must DIFFER from what the function was given as `observed` (or, for admission, the cached catalog must refuse) so that the verdict is rendered on the hint; when the hint agrees with `observed` the write path runs instead (a 412 and a resolve read), which `AnExternalWriterCostsOneResolveReadAndOneRetry` already covers:

| function | cache holds | store holds (external) | stale-and-false expects | true-on-read expects |
|---|---|---|---|---|
| `createNamespaceStep1` via `createNamespace` | row "a" present (Live) | row "a" erased by `deleteCompletedRemoving` after `beginRemoving` on the external | `createNamespace` returns `Live` (a new incarnation), one lane read | store still has "a" Creating under another creator: `Superseded` |
| `completeCreation` | "a" Creating, incarnation 1 | "a" Creating with the same row (unchanged) but the catalog gained row "b" | `Live` (the row matched on the read; the cache was stale only elsewhere) | "a" completed to Live by the external: `Superseded` |
| `reconcileStaleCreator` | "a" Creating under creator X | unchanged row, catalog gained "b" | `Reconciled` | "a" completed to Live: `EntryChanged` |
| admission refusal (`checkCatalogAdmission`, `LIMIT_EXCEEDED`) | a catalog at the namespace limit | the external erased one row, so the fresh catalog has room | `createNamespace` admits: `Live` | the fresh catalog is also full: `LIMIT_EXCEEDED` thrown, no write |
| `casAdmitEntry` | row "a" present | the external erased "a" | admits "a" again: `Committed` path, one lane read | "a" still present: `LOGICAL_ERROR` from the grammar check on the read |

For the `LIMIT_EXCEEDED` rows use `gc_shards = 1` and the limit `checkCatalogAdmission` enforces; read that function in `Formats/CasRefCatalogFormat.cpp` for the exact count and seed one fewer than it.

```cpp
TEST(CASRefCatalog, AFenceLostDuringStepOneIsFencedOutNotABareMarker)
{
    PoolAndExternal f(std::make_shared<CountingBackend>());
    const Layout layout("p");
    CasOperation seed = f.pool.admit();
    CasRefCatalog::initializeEmptyForNewPool(seed, layout);
    const CreatorFence creator{.server_root_id = "srv", .writer_epoch = 1, .fence_generation = 1};

    /// Tripped before its turn: the lane's wait refuses it, nothing is written.
    {
        bool alive = true;
        CasOperation op = f.pool.admit([&] { return alive; });
        f.hot_keys.enter_after_lane_hook_for_test = [&] { alive = false; };
        const uint64_t writes_before = f.backend->writeCount(layout.refCatalogKey());
        EXPECT_EQ(CasRefCatalog::createNamespace(op, layout, 1, RootNamespace{"a"}, creator),
                  CasRefCatalog::NamespaceCreationOutcome::FencedOut);
        f.hot_keys.enter_after_lane_hook_for_test = {};
        EXPECT_EQ(f.backend->writeCount(layout.refCatalogKey()), writes_before);
    }
    /// Tripped between the landed step-1 write and the post-commit check: FencedOut, and the Creating
    /// row is durable for a later reconciler.
    {
        bool alive = true;
        CasOperation op = f.pool.admit([&] { return alive; });
        f.backend->onWriteCommitted(layout.refCatalogKey(), [&] { alive = false; });
        EXPECT_EQ(CasRefCatalog::createNamespace(op, layout, 1, RootNamespace{"b"}, creator),
                  CasRefCatalog::NamespaceCreationOutcome::FencedOut);
        f.backend->onWriteCommitted(layout.refCatalogKey(), {});
        const auto snap = CasRefCatalog::read(seed, layout);
        ASSERT_EQ(snap.catalog.entries.size(), 1u);
        EXPECT_EQ(snap.catalog.entries[0].state, NsState::Creating);
    }
    /// casAdmitEntry under a tripped fence throws the transient-unavailable class, never a bare marker.
    {
        CasOperation op = f.pool.admit([] { return false; });
        DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR,
            [&] { CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry("c", 3)); });
    }
}

TEST(CASRefCatalog, TheStoresAnswerToAHintsWriteIsDeliveredAndTheRetryLearnsTheRest)
{
    PoolAndExternal f(std::make_shared<CountingBackend>());
    f.backend->setRefreshCredentialsResult(false);
    const Layout layout("p");
    CasOperation pool_op = f.pool.admit();
    CasRefCatalog::initializeEmptyForNewPool(pool_op, layout);
    const CatalogEntry observed = liveEntry("a", 1);
    CasRefCatalog::casAdmitEntry(pool_op, layout, 1, observed);   /// the cache holds "a" Live

    /// Another server moved the row on; the hint still matches what this caller observed, so a write
    /// goes out, and the store refuses it definitively.
    CasOperation other = f.external.admit();
    ASSERT_EQ(CasRefCatalog::beginRemoving(other, layout, observed, 5), CasRefCatalog::BeginRemovingOutcome::Transitioned);
    f.backend->failNextWriteWith(layout.refCatalogKey(), s3Error(Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));
    const uint64_t writes_before = f.backend->writeCount(layout.refCatalogKey());
    const uint64_t reads_before = f.backend->getCount(layout.refCatalogKey());
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::S3_ERROR,
        [&] { (void)CasRefCatalog::beginRemoving(pool_op, layout, observed, 5); });
    EXPECT_EQ(f.backend->writeCount(layout.refCatalogKey()) - writes_before, 1u);
    EXPECT_EQ(f.backend->getCount(layout.refCatalogKey()) - reads_before, 0u);
    /// The caller's ordinary retry after a fresh observation learns the row moved.
    EXPECT_EQ(CasRefCatalog::beginRemoving(pool_op, layout, observed, 5), CasRefCatalog::BeginRemovingOutcome::AlreadyRemoving);

    /// The same stale hint, and the fence trips between the refused write and its resolve read.
    CasRefCatalog::casAdmitEntry(pool_op, layout, 1, liveEntry("b", 2));
    const CatalogEntry b = liveEntry("b", 2);
    ASSERT_EQ(CasRefCatalog::beginRemoving(other, layout, b, 6), CasRefCatalog::BeginRemovingOutcome::Transitioned);
    bool alive = true;
    CasOperation fenced = f.pool.admit([&] { return alive; });
    f.backend->onBeforeWrite(layout.refCatalogKey(), [&] { alive = false; });
    EXPECT_EQ(CasRefCatalog::beginRemoving(fenced, layout, b, 6), CasRefCatalog::BeginRemovingOutcome::FencedOut);
    f.backend->onBeforeWrite(layout.refCatalogKey(), {});
    EXPECT_EQ(CasRefCatalog::read(other, layout).catalog.entries.at(1).state, NsState::Removing) << "nothing of ours landed";
    CasOperation readmitted = f.pool.admit();
    EXPECT_EQ(CasRefCatalog::beginRemoving(readmitted, layout, b, 6), CasRefCatalog::BeginRemovingOutcome::AlreadyRemoving);
}

TEST(CASRefCatalog, TheCatalogLoopPacesAConflictByWhetherItSettledAFault)
{
    PoolAndExternal f(std::make_shared<CountingBackend>());
    const Layout layout("p");
    CasOperation pool_op = f.pool.admit();
    CasRefCatalog::initializeEmptyForNewPool(pool_op, layout);
    CasRefCatalog::casAdmitEntry(pool_op, layout, 1, liveEntry("a", 1));
    CasOperation other = f.external.admit();
    const String key = layout.refCatalogKey();

    /// K clean races: the external moves the catalog before each of the pool's first K writes.
    constexpr int K = 3;
    int moved = 0;
    bool inside = false;
    bool ambiguous = false;
    f.backend->onBeforeWrite(key, [&]
    {
        if (inside || moved >= K)
            return;
        inside = true;
        CasRefCatalog::casAdmitEntry(other, layout, 1, liveEntry("x" + std::to_string(moved), 10 + moved));
        if (ambiguous)
            f.backend->injectAmbiguousWrite(key);
        ++moved;
        inside = false;
    });
    const auto pauses_before = eventCount(ProfileEvents::CASRequestConflictPause);
    CasRefCatalog::casUpdate(pool_op, layout, [](const RefCatalog & cur)
    {
        RefCatalog next = cur;
        for (auto & e : next.entries)
            if (e.ns.string() == "a") { e.state = NsState::Removing; e.removal_started_round = 1; }
        return next;
    });
    ASSERT_EQ(f.clock.sleeps.size(), static_cast<size_t>(K));
    for (uint64_t s : f.clock.sleeps)
        EXPECT_LE(s, 200u);
    EXPECT_EQ(eventCount(ProfileEvents::CASRequestConflictPause) - pauses_before, 0u) << "the lane's caller pauses itself; the engine's counter is for its own loops";

    /// K conflicts that each settled a fault: the growing schedule, on the loop's own count.
    f.clock.sleeps.clear();
    moved = 0;
    ambiguous = true;
    CasRefCatalog::casUpdate(pool_op, layout, [](const RefCatalog & cur) { return cur; });
    ASSERT_EQ(f.clock.sleeps.size(), static_cast<size_t>(K));
    for (size_t i = 0; i < f.clock.sleeps.size(); ++i)
        EXPECT_LE(f.clock.sleeps[i], std::min<uint64_t>(5000, 200ull << i));
}

TEST(CASRefCatalog, TheGCEraseRacesTheLaneAsItRacesEverything)
{
    PoolAndExternal f(std::make_shared<CountingBackend>());
    const Layout layout("p");
    CasOperation pool_op = f.pool.admit();
    CasRefCatalog::initializeEmptyForNewPool(pool_op, layout);
    const CatalogEntry removing{.ns = RootNamespace{"a"}, .state = NsState::Removing,
                                .incarnation = UInt128{7}, .removal_started_round = 13};
    CasRefCatalog::casAdmitEntry(pool_op, layout, 1, liveEntry("keep", 1));
    /// Seed the Removing row through the external, as raw bytes: `casUpdate` refuses to add rows, and
    /// the point is that the pool's cache is stale about this one.
    CasOperation other = f.external.admit();
    {
        const CasRefCatalog::Snapshot snap = CasRefCatalog::read(other, layout);
        RefCatalog with_removing = snap.catalog;
        with_removing.entries.insert(with_removing.entries.begin(), removing);   /// "a" sorts before "keep"
        (void)orThrow(other.replace(layout.refCatalogKey(), encodeRefCatalog(with_removing), *snap.etag, Retry::standard()), "seed");
    }
    CasFoldSeal ready_parent;
    ready_parent.ref_lives.emplace(UInt128{7}, RefLifeFoldState{
        .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{1, 2}},
        .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 2}}});

    /// The erase runs on an open plane of the same pool while a lane holder is parked in its write:
    /// it does not wait.
    CasRequests open_plane(f.backend, Fence::open(), f.clock.nowFn(), f.clock.sleepFn(), &f.hot_keys);
    CasOperation erase_op = open_plane.admit();
    std::latch parked(1);
    std::latch release(1);
    int seen = 0;
    f.backend->onBeforeWrite(layout.refCatalogKey(), [&]
    {
        if (seen++ != 0)
            return;
        parked.count_down();
        release.wait();
    });
    std::thread holder([&]
    {
        CasRefCatalog::casUpdate(pool_op, layout, [](const RefCatalog & cur) { return cur; });
    });
    parked.wait();
    const auto result = CasRefCatalog::deleteCompletedRemovingAtSnapshot(
        erase_op, layout, CasRefCatalog::read(other, layout), removing, ready_parent, noAuthorityRefresh);
    EXPECT_EQ(result.outcome, CasRefCatalog::CompletedRemovingDeleteOutcome::Deleted);
    EXPECT_EQ(f.hot_keys.queueDepthForTest(layout.refCatalogKey()), 1u) << "the holder is still parked";
    release.count_down();
    holder.join();
    f.backend->onBeforeWrite(layout.refCatalogKey(), {});

    /// The pool's next submission pays exactly one resolve read and one retry write for the erase.
    const uint64_t reads_before = f.backend->getCount(layout.refCatalogKey());
    const uint64_t writes_before = f.backend->writeCount(layout.refCatalogKey());
    CasRefCatalog::casAdmitEntry(pool_op, layout, 1, liveEntry("c", 3));
    EXPECT_LE(f.backend->getCount(layout.refCatalogKey()) - reads_before, 1u);
    EXPECT_LE(f.backend->writeCount(layout.refCatalogKey()) - writes_before, 2u);
    const uint64_t reads_after = f.backend->getCount(layout.refCatalogKey());
    CasRefCatalog::casAdmitEntry(pool_op, layout, 1, liveEntry("d", 4));
    EXPECT_EQ(f.backend->getCount(layout.refCatalogKey()), reads_after) << "the one after starts from the cache";
}
```

Note: `FakeClock` is single-threaded and the GC-erase test has two threads; the holder thread only writes (no sleeps) while parked and the erase runs on the test thread, so the clock is touched by one thread at a time. Add `#include <latch>` and `#include <thread>`.

- [ ] **Step 2: Build to verify they fail**

BUILD(`build_task4_red.log`), ANALYZE(`build_task4_red.log`). The tests compile (they use only existing APIs plus Task 2's), so run TEST(`CASRefCatalog.AStaleHint*:CASRefCatalog.ATrue*:CASRefCatalog.AFenceLostDuringStepOne*:CASRefCatalog.TheStoresAnswer*:CASRefCatalog.TheCatalogLoop*:CASRefCatalog.TheGCErase*`, `test_task4_red.log`), ANALYZE(`test_task4_red.log`). Expected: failures (the catalog still calls `readModifyWrite`, so the cache is never used and `createNamespace` still leaks the marker).

- [ ] **Step 3: Make the catalog submit through the lane**

In `CasRefCatalog.cpp`, `casUpdateImpl`: replace the block from `WriteResult result = op.readModifyWrite(key, decide, policy);` to `return std::move(*written);` with:

```cpp
    /// One hold at a time per pool on this key, from the pool's last known catalog when the lane holds
    /// one. The loop is this function's: a `Conflict` is a lost race against another server (the lane
    /// never conflicts with itself), repaid after the flat jitter, or after the growing schedule when
    /// the conflict settled a transport fault. Under a single-attempt policy the first `Conflict` is
    /// the answer, as the engine's own verb answers it.
    const Retry frozen = op.freeze(policy);
    uint32_t settled_faults = 0;
    for (;;)
    {
        WriteResult result = op.hotKeys().submit(key, op, frozen, decide);
        if (const auto * conflict = std::get_if<Conflict>(&result); conflict && !frozen.single_attempt)
        {
            op.pause(conflict->any_ambiguous ? Retry::backoff(++settled_faults) : Retry::conflictBackoff());
            continue;
        }
        /// The fence can be lost in two places and both mean the same to a lifecycle caller: inside
        /// `decide`, which throws the marker itself, and between two attempts, where the engine
        /// notices it first and no further `decide` runs. Normalising the second onto the first is
        /// what keeps "the fence moved" a returned outcome rather than an exception.
        if (const auto * gave_up = std::get_if<GaveUp>(&result); gave_up && gave_up->why == GaveUp::Why::FenceLost)
            throw CatalogFenceMovedMarker{};
        if (!std::holds_alternative<Committed>(result))
            throwCatalogWriteFailure(std::move(result), fmt::format("CAS ref catalog '{}' update", key));
        return std::move(*written);
    }
```

Update the function's header comment ("A refused precondition re-runs `mutate` against the FRESH body") to say the fresh body is the resolve read's, which the lane remembers and the next hold starts from. Leave `kMaxCatalogCasAttempts` where it is: the erase loop still uses it.

`casAdmitEntry`: wrap its `return casUpdateImpl(...)` in the same `try`/`catch (const CatalogFenceMovedMarker &)` that `casUpdate` has, throwing `throwCasTransientUnavailable` with the same subject and condition text.

`createNamespace`: the step-1 `try` gains a second handler:

```cpp
    catch (const CatalogFenceMovedMarker &)
    {
        /// The creator's own admission moved while step 1 waited its turn or wrote: an answer, not a
        /// failure, and the same one the two later steps already give.
        return NamespaceCreationOutcome::FencedOut;
    }
```

- [ ] **Step 4: Build and run the catalog suite**

BUILD(`build_task4.log`), ANALYZE(`build_task4.log`). TEST(`CASRefCatalog*:CASHotKeys.*`, `test_task4_catalog.log`), ANALYZE(`test_task4_catalog.log`). Expected: every test passes, the four pre-existing conflict tests (`CasUpdateRetriesOnConflictAgainstFreshState`, `BeginRemovingRechecksAdmissionAfterACatalogConflict`, `CasUpdateThrowsOnVanishMidRetryInsteadOfReplacingTheCatalog` and its death-test twin) unchanged. If `CasUpdateRetriesOnConflictAgainstFreshState` asserts `mutate_calls == 2` and now sees 3, the cause is a cache-less `CasRequests` re-reading on resubmission where `readModifyWrite` reused its resolve read; the spec accepts one extra read per conflict, not an extra `mutate` call: a resubmission on a cache-less lane reads once and decides once, so `mutate_calls` stays 2. Investigate before touching the assertion.

- [ ] **Step 5: Commit**

COMMIT(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.cpp src/Disks/tests/gtest_cas_ref_catalog.cpp`, `ca-catalog: catalog mutations take the pool's hot-key lane; a fence lost during creation step 1 is FencedOut`)

---

### Task 5: The pool owns the lane; the planes share it {#task-5}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h:77` (`PoolConfig`), `:1167-1172` (members)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:185-213` (ctor)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h:79,91,99` (comments)
- Test: `src/Disks/tests/gtest_cas_pool.cpp`

**Interfaces:**
- Consumes: `CasRequests(..., CasHotKeys *)`, `CasHotKeys(uint64_t)`.
- Produces: `PoolConfig::hot_key_cache_bytes`; `Pool::hot_keys`.

- [ ] **Step 1: Write the failing test**

Append to `src/Disks/tests/gtest_cas_pool.cpp` (the file's existing includes and usings cover `Pool`, `openPoolForTest`, `CountingBackend`; add the `ProfileEvents` externs it lacks):

```cpp
namespace ProfileEvents
{
    extern const Event CASHotKeyReadStarts;
    extern const Event CASRequestResolveRead;
    extern const Event CASRequestConflictPause;
}

TEST(CASPool, ConcurrentNamespaceCreationsNeverRaceEachOtherOnTheCatalog)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto pool = DB::Cas::tests::openPoolForTest(backend);
    const DB::Cas::Layout layout("p");
    const String key = layout.refCatalogKey();
    constexpr int N = 6;

    const uint64_t writes_before = backend->writeCount(key);
    const auto reads_before = ProfileEvents::global_counters[ProfileEvents::CASHotKeyReadStarts].load();
    const auto resolves_before = ProfileEvents::global_counters[ProfileEvents::CASRequestResolveRead].load();
    std::vector<std::thread> threads;
    for (int i = 0; i < N; ++i)
        threads.emplace_back([&, i] { (void)pool->namespaceLife(DB::Cas::RootNamespace{"ns" + std::to_string(i)}); });
    for (auto & t : threads)
        t.join();

    EXPECT_EQ(backend->writeCount(key) - writes_before, 2u * N) << "two catalog steps per creation, each one write";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRequestResolveRead].load() - resolves_before, 0u)
        << "no refused precondition, so no resolve read";
    EXPECT_LE(ProfileEvents::global_counters[ProfileEvents::CASHotKeyReadStarts].load() - reads_before, 1u)
        << "at most one lane read; every later hold started from the cache";

    /// Another server writes the catalog between two of this pool's mutations: one extra read and one
    /// retry write, then the cache is current again.
    {
        auto external_requests = DB::Cas::tests::openRequestsForTest(backend);
        auto external = external_requests.admit();
        DB::Cas::CasRefCatalog::casAdmitEntry(external, layout, 1,
            DB::Cas::CatalogEntry{.ns = DB::Cas::RootNamespace{"zz"}, .state = DB::Cas::NsState::Live, .incarnation = UInt128{99}});
    }
    const uint64_t writes_mid = backend->writeCount(key);
    const uint64_t gets_mid = backend->getCount(key);
    (void)pool->namespaceLife(DB::Cas::RootNamespace{"after"});
    EXPECT_EQ(backend->getCount(key) - gets_mid, 1u) << "one resolve read for the external write";
    EXPECT_EQ(backend->writeCount(key) - writes_mid, 3u) << "one refused, two landed";
}
```

`Pool::namespaceLife` resolves a namespace's life, creating it through `resolveNamespaceLife` when absent; the `_ckpt` publish of step 2 writes another key and is not counted here. If the pool's bootstrap or its GC leader touches the catalog key during the test, the counts above drift: open the pool, wait for `pool->namespaceLife` of a warm-up namespace, then take the `before` values.

- [ ] **Step 2: Build and run to verify it fails**

BUILD(`build_task5_red.log`), ANALYZE(`build_task5_red.log`). TEST(`CASPool.ConcurrentNamespaceCreations*`, `test_task5_red.log`), ANALYZE(`test_task5_red.log`). Expected: fails on the read-starts or resolve-read expectation, because each plane still owns a private lane with no cache.

- [ ] **Step 3: Wire the pool**

`CasPool.h`, after `manifest_decode_cache_bytes` in `PoolConfig`:

```cpp
    /// Byte bound for the hot-key lane's cache of last known objects (the catalog today). 0 disables
    /// the cache and every catalog write reads first. 16 MiB: the catalog is under 1 MiB, and the
    /// bound exists so a later opt-in of per-namespace keys has one.
    uint64_t hot_key_cache_bytes = 16ULL << 20;
```

`CasPool.h`, immediately before `mutable CasRequests mount_requests;`:

```cpp
    /// The pool's write lane for keys several of its writers share, declared before the three planes
    /// that carry a pointer to it, so it outlives every operation they admit. `mutable` for the same
    /// reason the planes are.
    mutable CasHotKeys hot_keys;
```

`CasPool.cpp`, the constructor's initializer list: add `, hot_keys(config.hot_key_cache_bytes)` right after `, meta(std::move(meta_))` and before `mount_requests`; then give each plane the pointer:

```cpp
    , mount_requests(pool_backend, Fence{
          [this] { return mount_runtime.fenceGeneration(); },
          [this](uint64_t g, uint64_t needed) { return mount_runtime.admit(g, needed); },
          [this](uint64_t g) { mount_runtime.checkFenceOrThrow(g); }},
          config.boot_ms_fn,
          mountPlaneSleepFn(),
          &hot_keys)
    , farewell_requests(pool_backend, Fence::open(), config.boot_ms_fn, {}, &hot_keys)
    , gc_requests(pool_backend, Fence{
          [] { return uint64_t{0}; },
          [this](uint64_t, uint64_t) { return teardownBegun() ? Fence::Admit::LostOrRearmed : Fence::Admit::Ok; },
          [](uint64_t) {}},
          config.boot_ms_fn,
          openPlaneSleepFn(),
          &hot_keys)
```

The two bootstrap `CasRequests` in `CasPool.cpp` (lines ~409 and ~909) stay as they are: the pool does not exist yet, and their private lane with no cache is today's behaviour.

`CasMountRuntime.h`: on the three hooks that run under `driver_mutex` (`remount_parked_hook_for_test`, `renewal_parked_predicate_false_hook_for_test`, `terminal_publication_driver_lock_acquired_hook_for_test`) append to each doc comment:

```cpp
    /// Runs with `driver_mutex` held: it must issue no backend request and never wait on the pool's
    /// hot-key lane, whose holders sleep under that mutex, or the test deadlocks itself.
```

- [ ] **Step 4: Build and run**

BUILD(`build_task5.log`), ANALYZE(`build_task5.log`). TEST(`CASPool.*`, `test_task5_pool.log`), ANALYZE(`test_task5_pool.log`). Expected: all pass.

- [ ] **Step 5: Commit**

COMMIT(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h src/Disks/tests/gtest_cas_pool.cpp`, `ca-pool: the pool owns one hot-key lane and its three planes share it`)

---

### Task 6: The gate, the review, the merge {#task-6}

**Files:** none new.

- [ ] **Step 1: The full CAS gate**

BUILD(`build_task6.log`), ANALYZE(`build_task6.log`). TEST(`CAS*`, `test_task6_gate.log`), ANALYZE(`test_task6_gate.log`). Expected: green, with the test count equal to Task 0's baseline plus the new tests (five in `CASRequests`, fourteen in `CASHotKeys`, eleven or more in `CASRefCatalog`, one in `CASPool`). Any red is a return item for the task that owns the file; never a skip.

- [ ] **Step 2: Sanitizer run of the lane tests**

If `lane-g` has no ASan build, skip this step and say so in the report. Otherwise:

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g
ninja -C build_asan unit_tests_dbms > build_asan/build_task6.log 2>&1
./build_asan/src/unit_tests_dbms --gtest_filter='CASHotKeys.*:CASRefCatalog.TheGCErase*' > build_asan/test_task6_lane.log 2>&1
```

ANALYZE both. A use-after-free or a data race here is a defect in the ticket guard, not a flake.

- [ ] **Step 3: Review**

Dispatch `ca-review` (opus, high) on the diff `git diff cas-gc-rebuild..cas-hot-key-lane` with this brief, and do not skip it because the gate is green:

> Review the diff against `docs/superpowers/specs/2026-09-04-cas-hot-key-write-lane-design.md` revision 34 and the checklist at the end of its revision history. Verify against the code, not the commit messages. For each rule of the checklist say where the code keeps it or where it does not. Report CODE/TEST findings first, then PROSE. Severity: CRITICAL only for a write that lands that the store's precondition should have refused, `Committed` for a write that did not land, or a verdict rendered on stale state reaching a caller; MAJOR for deadlock, livelock, leak, dangling reference, an unbounded wait not named as accepted, or a result the engine did not produce for that submission. Return the verdict in your final message and write it to `/home/mfilimonov/workspace/ClickHouse/lane-g/build/review_task6.md`.

Fix every CRITICAL and MAJOR in the task that owns the file (rebuild, rerun that task's tests and the gate, commit with the task's paths). Fold MINORs the same way when they are code; prose MINORs go into one commit at the end.

- [ ] **Step 4: Merge into `cas-gc-rebuild`**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git diff --cached --stat
git status --short | grep -v '^??' | grep -E 'src/|docs/superpowers/specs/2026-09-02' ; true
git merge --no-ff cas-hot-key-lane -m "ca: merge the hot-key write lane, phase A (spec rev.34)"
git log --oneline -3
```

The first command must print nothing. The second must print nothing under `src/` (foreign uncommitted edits under `src/` would be swept into a merge conflict resolution); if it does, stop and report. After the merge, run `cmake -S . -B build` in every `build_*` directory of the master tree that will be used next (a merge that adds files leaves a stale build graph otherwise), redirecting output to a log. Do not push.

---

### Task 7: The acceptance measurement {#task-7}

**Files:** none. This task produces numbers, recorded in `docs/superpowers/cas/BACKLOG.md` under `{#ref-catalog-cas-starvation}` (a paragraph "Measured after phase A") and, for the phase B gate, under `{#hot-key-lane-phase-b}`.

- [ ] **Step 1: Run the same workload the RCA measured**

Ten minutes of the parallel stateless suite on the CA-s3 lane, exactly as `docs/superpowers/cas/2026-09-04-ref-catalog-starvation-rca.md` describes its run (the same lane, the same `~10` parallel jobs, the local S3), on the merged `cas-gc-rebuild`. Record the run's start and end timestamps.

- [ ] **Step 2: The before-and-after numbers**

On the server, over the run's window (`$from`, `$to`):

```sql
SELECT quantile(0.5)(query_duration_ms) AS p50, quantile(0.9)(query_duration_ms) AS p90, max(query_duration_ms) AS max, count() AS n
FROM system.query_log
WHERE type = 'QueryFinish' AND query_kind = 'Drop' AND event_time BETWEEN $from AND $to;

SELECT quantile(0.5)(query_duration_ms) AS p50, quantile(0.9)(query_duration_ms) AS p90, max(query_duration_ms) AS max, count() AS n
FROM system.query_log
WHERE type = 'QueryFinish' AND query_kind = 'Create' AND event_time BETWEEN $from AND $to;

SELECT count()
FROM system.text_log
WHERE event_time BETWEEN $from AND $to AND message LIKE '%PreconditionFailed%' AND message LIKE '%ref_catalog%';

SELECT event, value FROM system.events
WHERE event IN ('CASHotKeyQueueWaitMicroseconds', 'CASHotKeyCacheStarts', 'CASHotKeyReadStarts',
                'CASHotKeyCacheVerdictsReread', 'CASRequestConflictPause', 'CASRequestReissue', 'CASRequestResolveRead');
```

Take the `system.events` values at the start and the end of the window and record the deltas. The `DROP TABLE` p90 was 11.9 s and the `PreconditionFailed` count was 113 in 80 s before; the phase A target is a p90 near one second and a count near zero on the catalog key.

- [ ] **Step 3: The phase B gate**

Divide the `CASHotKeyQueueWaitMicroseconds` delta by the number of holds (`CASHotKeyCacheStarts + CASHotKeyReadStarts`) for the mean queue wait per submission, and set it beside the mean `DROP TABLE` duration. Combining (phase B) pays when the queue wait, not the write, dominates a submission. Record both numbers and the verdict under `{#hot-key-lane-phase-b}` in the BACKLOG, commit the BACKLOG in the master worktree with `git commit -- docs/superpowers/cas/BACKLOG.md`.

---

## Self-review against the spec {#self-review}

- Goals: one write in flight per pool and key (Task 2, INV-1 by test `SubmissionsOfOneKeyAreSerializedInArrivalOrder` and `AThrottledHolder…`); N mutations cost N `PUT`s and at most one `GET` when the cache admits (Task 3 `TheNextHoldStarts…`, Task 5 pool test); flat jitter with the growing schedule after a settled fault (Task 1, Task 4 `TheCatalogLoopPaces…`); any hot key, `Decide` is the engine's shape (Task 2 header); the four engine changes and two catalog changes (Tasks 1, 2, 4); phase B seams (Task 2's class shape: `submit`, `Item`, the hold's order, `Conflict::any_ambiguous`, erase-when-empty).
- The hold: base through `observe` with `readModifyWrite`'s conversion (Task 2 `hold`, test `ABaseReadThatFails…`); the cached verdict rule (Task 3 `MalformedBytes…`, `ADeclineOnAHint…`, Task 4 `AStaleHint…`); `single_attempt` reads fresh (Task 3); the store's answer delivered (Task 4 `TheStoresAnswer…`); the settle rules and `remember` never a result (Task 3 `TheCacheForgets…`); the identical-candidate rule inherited (Task 3 `AnIdenticalCandidate…`).
- The ticket: enter with rollback (Task 2 `AFailedEnqueue…`); the wait's order and give-ups on a fresh `WriteState` (Task 2 `WaitersLeave…`); leave as the single remover, lane erased when empty, the log line (Task 2 `leave`); the mutex a leaf (Task 2 `ADecideRuns…`).
- Deadlines and fences, the stalled holder (Task 2 `WaitersLeave…`, `AThrottledHolder…`); the catalog's nested reads keep their policies (no change; Task 4 leaves `isCreatorFenceTerminal` calls as they are).
- The catalog: the loop, `single_attempt`, no attempt cap, the step-1 catch, `casAdmitEntry`'s translation (Task 4); the GC erase outside the lane (Task 4 `TheGCErase…`).
- Half 2 and the contract edits (Task 1); observability events and metrics (Task 2); the pool and `PoolConfig` (Task 5); the `driver_mutex` hook contract (Task 5).
- Tests 1 to 12 of the spec map to: 1 → Task 2/3 serialization and cache; 2 → Task 2 `ResultsAreTheEnginesOwn` and Task 4 `TheStoresAnswer…`; 3 → Task 4 matrix and Task 3 declines/corruption; 4 → Task 2 `ABaseReadThatFails…` and Task 3 `ACachedStartPastTheDeadline…`; 5 → Task 4 `AFenceLostDuringStepOne…`; 6 → Task 3 `AnExternalWriter…`; 7 → Task 3 `TheCacheForgets…`, `TheBudgetBounds…`; 8 → Task 2 `WaitersLeave…`; 9 → Task 2 `WaitersLeave…` (parked `PUT`) and `AThrottledHolder…`; 10 → Task 4 `TheGCErase…`; 11 → Task 1 and Task 4 `TheCatalogLoopPaces…`; 12 → Task 5.
- Not covered by a test in this plan, by choice: the log line's text (asserted only by its path running under a catch-all), and the `Generation` dialect (no spacing in phase A).
