# CAS Mount Renewal Without a Lease Deadline, Part 1: the Three Fixes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A transient object-store outage or a memory-limit event no longer fences the CAS mount: the background renewal retries until the store gives a definitive answer, the lease TTL alone decides when writes stop, GC counts a token sighting from after its read, and an expired lease is visible to the operator.

**Architecture:** Three independent fixes on one branch. (1) `TokenWatch` carries the observer timing rule and replaces the hand-written copies in `computeHeartbeatFloor` and `claimMountAwaitingExpiry`. (2) The lease thread holds `LockMemoryExceptionInThread` and conditional `PUT`s run on the calling thread. (3) The request engine gets a retry spacing; the worker's renewal uses `Retry::untilDefinitive` on its own request plane, and the runtime reports an expired lease through `system.cas_mounts`, a counter, an event field and the per-request attempt counters. The startup, remount and direct renewals keep their bounded policy; the two worker threads and their parking protocol stay. Part 2 of the spec (one thread, one renewal path, removal of the thread-local observability stack, cleanups) is a separate plan.

**Tech Stack:** C++23 (ClickHouse, Allman braces), gtest (`unit_tests_dbms`), pytest integration tests through `ci.praktika`, TLA+ models already updated (`docs/superpowers/models` on `cas-gc-rebuild`).

**Spec:** `/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-10-01-cas-mount-renewal-without-lease-deadline-design.md` (r12, `eae93802842` on `cas-gc-rebuild`). This plan implements its "Order of work" steps 1 to 3 and rules 2 to 12. Rules 1, 13, 14, "Simpler code" and "Further cleanups" are part 2.

**Worktree:** `/home/mfilimonov/workspace/ClickHouse/cas-95-1`, branch `fix/antalya-26.6/cas-mount-renewal-no-deadline`, based on `altinity/antalya-26.6` at `41824f955e7`. Release build dir `build/`. The base passes the gate: 2556 tests in 323 `CAS*` suites.

`CA` below means `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`.

## Global Constraints

- Allman braces in all C++ (CI style check).
- Build: `cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/<log> 2>&1; echo NINJA_EXIT=$? >> build/<log>`. No `-j`, no `nproc`. Read the last line for `NINJA_EXIT=0`; on failure `grep -a -n "error:" build/<log> | head -40`. If every error is in a file this task does not own, another task's edit is in flight: wait two minutes and build again.
- New `gtest_*.cpp` files are globbed at configure time: run `cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock cmake -S . -B build > build/cmake_<task>.log 2>&1` after adding one, before building.
- Every gtest run: `cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='<filter>' > build/<log> 2>&1; echo GATE_EXIT=$? >> build/<log> )`. Read `GATE_EXIT=` and `grep -a "FAILED\|PASSED" build/<log> | tail -20`.
- Long commands run in the foreground of the agent that needs them, with the maximum tool timeout. Never start one in the background and wait for a notification.
- The gate filter is EXACTLY `CAS*`. Every new suite name starts with `CAS`. A task is done only when the whole gate passes on its head commit.
- Before claiming a test result, confirm the binary is newer than the last source edit: `stat -c %Y build/src/unit_tests_dbms` against the edited files' mtimes.
- `LOGICAL_ERROR` expectations are death tests whose child exits through `std::_Exit`; never `EXPECT_THROW` on a `LOGICAL_ERROR` site.
- No `sleep` for races, in code or tests: order with the injected clock, the scripted backends, barriers and existing test hooks.
- No fallback paths: a failure propagates or is retried by the rule in the spec; nothing substitutes a default.
- No new setting. The retry spacing is the constant `kMountRenewRetrySpacingMs = 1000`.
- The on-store format does not change. The mount body, its fields and the token discipline stay as they are.
- Write admission for data writes does not change: `CasMountRuntime::admit`, `mayMutate`, `refAppendFenceOk` and the envelope reservation of every write other than the worker's renewal.
- The startup renewal, the remount renewal and the direct renewal keep `Retry::untilLeaseSafe` and their terminal behaviour.
- Changes outside `CA/` and `src/Disks/tests/` are limited to: one `ProfileEvents.cpp` entry, `StorageSystemContentAddressedMounts.cpp` if the snapshot needs it, the integration test directory, `docs/en/antalya/cas/` and `docs/en/operations/system-tables/cas_mounts.md`.
- Commits: `git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- <exact files>` then `git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "<subject>" -m "<body>" -m "Co-Authored-By: <model name> <noreply@anthropic.com>"`. Exact file paths only, never a directory, never `-A`. No rebase, no amend, NO push. No session links in commit messages.
- Tests fail first: every task writes its tests, builds, and records the red run before implementing. A test that guards behaviour which already holds today is marked as a regression test and its green-on-base result is recorded instead.
- Prose (comments, docs, commit messages) follows `~/.claude/skills/tech-prose/STYLE.md`. Comments give the reason, never history, plan, spec, backlog, review or task references. Names of SQL identifiers, classes and functions go in backticks; functions written as `f`, not `f()`. Say "exception", not "crash", for logical errors. "ASan", not "ASAN".
- Hooks capturing test locals: declare the captured locals BEFORE the `Pool`, runtime or backend that stores the hook (teardown can call hooks after the test body).
- Implementers do not dispatch subagents.

## Review Focus

1. **A stop or a remount request while only the resolving read keeps failing.** The renewal must end within one wait plus one request, terminal, with the farewell forbidden; it must not hang inside the read. Test: `CASHeartbeat.StopDuringARetryWaitEndsTheRenewal`, case with a failing resolve read (Task 4).
2. **A store that fails `GET` of the mount object while `PUT` works.** No fence; the attempt counters and the last-failure text advance during the outage. Test: `CASMountRuntime.ExpiryIsReported`, case where only the resolving reads fail (Task 5).
3. **A success with a stale start.** It is not a restore: no `CASMountLeaseExpired`, writes stay refused, and the next renewal starts with no cadence wait. Tests: `CASMountRuntime.ExpiryRefusesWritesAndResumesUnderTheSameEpoch` (Task 5) and the cadence test of Task 4.
4. **An observer clock sample older than the sighting** (a clock that steps back, a sample reused across rounds). `TokenWatch::stableFor` is false and nothing is fenced. Test: `CASTokenWatch.CountsFromAfterTheRead` (Task 1).
5. **An empty conditional `PUT` body.** The buffer is one byte and the write succeeds without tripping the buffer assertion. Test: `CASBackend.ConditionalPutBufferStartsAtTheBodySize` (Task 2).

## File Structure

| File | Responsibility in this plan | Tasks |
|---|---|---|
| `CA/Pool/CasServerRoot.h`, `CasServerRoot.cpp` | `TokenWatch`; `computeHeartbeatFloor` takes the clock; `MountRenewPolicy`, `MountRenewRequestEvent`, the worker plane of `MountLeaseRenewer`; reporting of a renewal | 1, 4, 5 |
| `CA/Gc/CasGc.cpp` | the two callers of `computeHeartbeatFloor` pass the clock function | 1 |
| `CA/Pool/CasMountRuntime.h`, `CasMountRuntime.cpp` | memory guard in `renewalLoop`; `renewalEnvironment` policy; `noteRenewRequest`, `lastRenewFailure`, `leaseExpiredSinceBootMs`; the restore edge in `consumeRenewResult` | 2, 4, 5 |
| `CA/Pool/CasPool.h`, `CasPool.cpp` | the `lease_requests` plane; `lifecycleSnapshot` shows an expired lease | 4, 5 |
| `CA/Backend/CasRetry.h`, `CasRetry.cpp` | `attempt_spacing_ms`, `untilDefinitive`, `spacedPause`, `drawSpacing` | 3 |
| `CA/Backend/CasRequests.h`, `CasRequests.cpp` | spacing in `writeLoop` and `readLoop`; the per-request hook | 3, 4 |
| `CA/Backend/CasObjectStorageBackend.h`, `CasObjectStorageBackend.cpp` | inline conditional `PUT`, body-sized buffer | 2 |
| `src/Common/ProfileEvents.cpp` | `CASMountLeaseExpired` | 5 |
| `src/Disks/tests/gtest_cas_mount.cpp` | `CASTokenWatch`, `CASHeartbeat.GcCountsASightingFromAfterItsRead`, mechanical updates | 1 |
| `src/Disks/tests/gtest_cas_pool.cpp` | memory-limit test; runtime-level renewal and expiry tests | 2, 4, 5 |
| `src/Disks/tests/gtest_cas_s3_single_attempt_client.cpp` | inline `PUT` and buffer tests | 2 |
| `src/Disks/tests/gtest_cas_requests.cpp` | `CASRetrySpacing`, `CASRequestsSpacing` | 3 |
| `src/Disks/tests/gtest_cas_heartbeat.cpp` | renewer-level tests of the worker policy | 4 |
| `tests/integration/test_cas_mount_renewal_retry/test.py` | short and long `PUT` outage | 6 |
| `docs/en/antalya/cas/architecture/mounts-and-leases.md`, `configuration.md`, `operations/troubleshooting.md` | what part 1 changes for an operator | 6 |

## Spec-to-task map

| Spec | Task |
|---|---|
| "Step one", rule 11, spec tests 1 and 2 | 1 |
| "Renewal under memory pressure", rule 12, spec tests 3 and 4 | 2 |
| Rule 8 (spacing), spec test 10 at engine level | 3 |
| Rules 2 to 7 for the worker, rule 10 (catch-up), spec tests 5 to 11 | 4 |
| "What you see", rule 9, spec tests 12 and 13 | 5 |
| Spec tests 18 and 19, "Documentation" | 6 |
| Rules 1, 13, 14, "Simpler code", "Further cleanups", spec tests 14 to 17 | part 2, not in this plan |

Tasks 1, 2 and 3 touch disjoint files and do not depend on each other. Task 4 needs Task 3. Task 5 needs Task 4. Task 6 needs Tasks 4 and 5.

---

### Task 1: `TokenWatch` and the GC sighting fix

GC dates a token sighting with the clock sample that `Gc` takes before the `LIST`. A slow walk therefore credits the watch with time it did not watch. This task makes both observers use one helper, `TokenWatch`. Then `computeHeartbeatFloor` takes the clock function and dates every sighting with a sample taken after the read that produced it.

`CA` = `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`. Line numbers are from the worktree `/home/mfilimonov/workspace/ClickHouse/cas-95-1` at `41824f955e7`.

**Files:**
- Modify: `CA/Pool/CasServerRoot.h`: replace `struct MountIncarnationObservation` (`:347-356`) with `struct TokenWatch`. Retarget `MountObservationMap` (`:361`). Change the `computeHeartbeatFloor` declaration (`:409-411`) and its doc comment (`:373-376`, `:387-393`).
- Modify: `CA/Pool/CasServerRoot.cpp`: define `TokenWatch::sighted` and `TokenWatch::stableFor` before `mountObservationThresholdMs` (`:935`). Change the observation loop of `claimMountAwaitingExpiry` (`:960-1012`) and `computeHeartbeatFloor` (`:1020-1022`, decision `:1079-1094`).
- Modify: `CA/Gc/CasGc.cpp`: the two callers, `Gc::runRegularRound` (`:693`) and the rebuild pass (`:4707`).
- Test: `src/Disks/tests/gtest_cas_mount.cpp`: the new tests, plus a mechanical update of the existing `CASHeartbeatFloor` tests (`:1544-1793`).

**Enumeration of the sites.** Derived by `git grep -n "MountIncarnationObservation\|computeHeartbeatFloor(\|first_seen_mono_ms\|MountObservationMap"` over the whole worktree, plus `grep -n "/\*mono" src/Disks/tests/gtest_cas_mount.cpp`.
- Production:
  - the struct and the alias (`CasServerRoot.h:352`, `:361`);
  - the declaration and the definition of `computeHeartbeatFloor`;
  - the two callers in `CasGc.cpp`;
  - `Gc::mount_obs` (`CasGc.h:1007`). It is typed through the alias and needs no edit.
- `claimMountAwaitingExpiry` does not name the struct. It holds its own pair `observed` / `observed_since` (`CasServerRoot.cpp:960-961`), which moves to `TokenWatch`.
- Tests: only `gtest_cas_mount.cpp` names these symbols:
  - 12 `computeHeartbeatFloor` calls at `:1555, 1572, 1579, 1597, 1605, 1628, 1640, 1667, 1684, 1762, 1769, 1787`. All 12 pass the observer clock as a literal commented `/*mono*/` or `/*mono_now_ms*/`.
  - 2 reads of `.etag` on an observation entry (`:1599`, `:1610`).
  - 2 reads of `.first_seen_mono_ms` (`:1561`, `:1611`). They stay as they are.
- Comments only, with no code edit: `gtest_cas_heartbeat.cpp`, `gtest_cas_pool.cpp` and `gtest_cas_gc_ack_floor.cpp` mention `computeHeartbeatFloor` in comments.
- Before Step 12, re-run both greps. The expected result: no `MountIncarnationObservation` anywhere, and no `/*mono` left in `gtest_cas_mount.cpp`.

**Behaviour of existing GC tests under the fix.** A sighting can only get a later date, never an earlier one. Fewer fences in a round is the only possible effect. Only two GC-driven tests assert that a round fences:
- `runExpiredMountFenceOutScenario` (`gtest_cas_gc_ack_floor.cpp:921`), used by `CASGCAckFloor.ExpiredMountFencedOutAndExcluded` and `CASGcFenceOut.ThresholdUnchangedByUnsafeKnob`;
- `CASGCAckFloor.DefaultMonoClockTracksPoolsInjectedBootClockNotWallClock` (`:1033`).

Both hold the observer clock constant within a round (`gc_mono`, `fake_boot`), so a sample after the read equals the round-start sample and they stay green. `gtest_cas_gc_log.cpp:440` asserts zero fence-outs, which fewer fences cannot break.

**Interfaces:**
- Consumes: none.
- Produces:
  - `struct TokenWatch { Etag token; uint64_t first_seen_mono_ms; static TokenWatch sighted(Etag, uint64_t sample_after_read_ms); bool stableFor(uint64_t threshold_ms, uint64_t sample_before_read_ms) const; }` in `CA/Pool/CasServerRoot.h`.
  - `using MountObservationMap = std::map<String, TokenWatch>;`
  - `HeartbeatFloor computeHeartbeatFloor(CasOperation & op, const Layout & l, uint64_t now_ms, const std::function<uint64_t()> & mono_ms_fn, uint64_t stable_threshold_ms, MountObservationMap & obs);`

---

#### Cycle 1: the `TokenWatch` type

- [ ] **Step 1: Write the failing test**

In `src/Disks/tests/gtest_cas_mount.cpp`, insert this test right after the closing `}` of the anonymous namespace that ends with `renewMount` (`:1542`), before `TEST(CASHeartbeatFloor, FirstSightNeverFencesEvenIfStampLooksExpired)`:

```cpp
TEST(CASTokenWatch, CountsFromAfterTheRead)
{
    auto b = std::make_shared<InMemoryBackend>();
    Ops ops(b);
    const Etag t1 = std::get<Committed>(ops.op.create("k", "v1", Retry::standard())).etag;
    const Etag t2 = std::get<Committed>(ops.op.replace("k", "v2", t1, Retry::standard())).etag;
    constexpr uint64_t threshold_ms = 10'000;

    /// The read that returned `t1` ended at 5000: the watch counts from there.
    const TokenWatch watch = TokenWatch::sighted(t1, /*sample_after_read_ms=*/ 5'000);
    EXPECT_EQ(watch.token, t1);
    EXPECT_EQ(watch.first_seen_mono_ms, 5'000u);

    EXPECT_FALSE(watch.stableFor(threshold_ms, 14'999));
    EXPECT_TRUE(watch.stableFor(threshold_ms, 15'000));

    /// A sample older than the sighting proves nothing about how long the token held.
    EXPECT_FALSE(watch.stableFor(threshold_ms, 4'000));
    EXPECT_FALSE(watch.stableFor(0, 4'000));

    /// A changed token is a new watch, counted from its own read.
    const TokenWatch renewed = TokenWatch::sighted(t2, 15'000);
    EXPECT_NE(renewed.token, watch.token);
    EXPECT_FALSE(renewed.stableFor(threshold_ms, 15'000));
    EXPECT_TRUE(renewed.stableFor(threshold_ms, 25'000));
}
```

`stableFor(threshold_ms, 4'000)` is what catches a wrapping subtraction: `4000 - 5000` as `uint64_t` is far above any threshold.

- [ ] **Step 2: Build it to see it fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t1_c1_red.log 2>&1; echo NINJA_EXIT=$? >> build/t1_c1_red.log
tail -1 build/t1_c1_red.log
grep -a -n "error:" build/t1_c1_red.log | head -40
```

Expected: `NINJA_EXIT=1`. The errors are in `gtest_cas_mount.cpp`: `error: unknown type name 'TokenWatch'` or `use of undeclared identifier 'TokenWatch'`, and nothing else. An error in any other file means the tree was not at the base commit. Stop and report.

- [ ] **Step 3: Implement `TokenWatch`**

In `CA/Pool/CasServerRoot.h`, replace this block (`:347-356`):

```cpp
/// One `server_root_id`'s cross-round incarnation-stability observation,
/// owned by the GC leader instance (`Cas::Gc::mount_obs`) and threaded through consecutive
/// `computeHeartbeatFloor` calls — one GC round is one observation tick. Mirrors
/// `claimMountAwaitingExpiry`'s observation loop, but at heartbeat-gate granularity rather than a
/// tight poll loop.
struct MountIncarnationObservation
{
    Etag etag;
    uint64_t first_seen_mono_ms = 0;
};
```

with:

```cpp
/// One observed token and the instant it was first seen, on the observer's own monotonic clock.
struct TokenWatch
{
    Etag token;
    uint64_t first_seen_mono_ms = 0;

    /// `sample_after_read_ms` is a clock sample taken after the read that returned `token_`. An earlier
    /// sample would count the time the read took as time watched.
    static TokenWatch sighted(Etag token_, uint64_t sample_after_read_ms);

    /// True when the token has been watched for at least `threshold_ms`. `sample_before_read_ms` is a
    /// clock sample taken before the read that confirmed the token; false when it precedes the sighting.
    bool stableFor(uint64_t threshold_ms, uint64_t sample_before_read_ms) const;
};

/// TODO-CYCLE-2: the alias below still names the old struct until Step 5.
struct MountIncarnationObservation
{
    Etag etag;
    uint64_t first_seen_mono_ms = 0;
};
```

That temporary marker exists so Cycle 1 compiles on its own. Step 5 deletes both the marker comment and the old struct. Do not commit Cycle 1 with the marker in place (see Step 4).

In `CA/Pool/CasServerRoot.cpp`, insert before `uint64_t mountObservationThresholdMs(uint64_t ttl_ms, uint64_t cadence_ms)` (`:935`):

```cpp
TokenWatch TokenWatch::sighted(Etag token_, uint64_t sample_after_read_ms)
{
    return TokenWatch{.token = std::move(token_), .first_seen_mono_ms = sample_after_read_ms};
}

bool TokenWatch::stableFor(uint64_t threshold_ms, uint64_t sample_before_read_ms) const
{
    return sample_before_read_ms >= first_seen_mono_ms && sample_before_read_ms - first_seen_mono_ms >= threshold_ms;
}
```

- [ ] **Step 4: Build and run the test**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t1_c1_green.log 2>&1; echo NINJA_EXIT=$? >> build/t1_c1_green.log
tail -1 build/t1_c1_green.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASTokenWatch.*' > build/t1_c1_test.log 2>&1; echo GATE_EXIT=$? >> build/t1_c1_test.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t1_c1_test.log | tail -20
```

Expected: `NINJA_EXIT=0`, `[  PASSED  ] 1 test.`, `GATE_EXIT=0`. No commit yet. Cycle 2 removes the marker, and the first commit is at Step 8.

---

#### Cycle 2: both observers on `TokenWatch`, and the clock passed as a function

This cycle is a refactor with no behaviour change. `computeHeartbeatFloor` samples the function once at entry and uses that sample for both the stability test and the sighting, exactly as today. No red step: the existing tests are the fence. They must stay green without any change to their assertions.

- [ ] **Step 5: Change the header**

In `CA/Pool/CasServerRoot.h`, delete the temporary block added in Step 3:

```cpp
/// TODO-CYCLE-2: the alias below still names the old struct until Step 5.
struct MountIncarnationObservation
{
    Etag etag;
    uint64_t first_seen_mono_ms = 0;
};
```

Then replace (`:361`):

```cpp
using MountObservationMap = std::map<String, MountIncarnationObservation>;
```

with:

```cpp
using MountObservationMap = std::map<String, TokenWatch>;
```

In the doc comment of `HeartbeatFloor`, replace (`:372-376`):

```cpp
///   - otherwise, observation-based liveness (the same
///     principle `claimMountAwaitingExpiry` uses for a mount's OWN reopen, applied here to the GC's
///     fence-out): `obs` remembers, per srid, the incarnation last seen and the leader's OWN
///     monotonic clock reading (`mono_now_ms`) at the moment it first saw it. A body whose
///     CURRENT incarnation differs from (or is absent from) `obs` is (re)started fresh — counted `live`,
```

with:

```cpp
///   - otherwise, observation-based liveness (the same
///     principle `claimMountAwaitingExpiry` uses for a mount's OWN reopen, applied here to the GC's
///     fence-out): `obs` holds, per srid, a `TokenWatch` of the incarnation last seen. A body whose
///     CURRENT incarnation differs from (or is absent from) `obs` is (re)started fresh — counted `live`,
```

Replace (`:387-391`):

```cpp
/// `now_ms` is WALL clock, used only for the audit/diagnostic log line — it never participates in the
/// fence decision (mirrors `claimMountAwaitingExpiry`'s `now_ms_fn` vs `mono_ms_fn` split).
/// `mono_now_ms` is the OBSERVATION clock: the caller's OWN monotonic reading, never compared against
/// any other node's clock. `obs` is owned by the caller and threaded across consecutive calls (one GC
```

with:

```cpp
/// `now_ms` is WALL clock, used only for the audit/diagnostic log line — it never participates in the
/// fence decision (mirrors `claimMountAwaitingExpiry`'s `now_ms_fn` vs `mono_ms_fn` split).
/// `mono_ms_fn` is the OBSERVATION clock: the caller's OWN monotonic clock, never compared against
/// any other node's clock. `obs` is owned by the caller and threaded across consecutive calls (one GC
```

Replace the declaration (`:409-411`):

```cpp
HeartbeatFloor computeHeartbeatFloor(CasOperation & op, const Layout & l, uint64_t now_ms,
                                     uint64_t mono_now_ms, uint64_t stable_threshold_ms,
                                     MountObservationMap & obs);
```

with:

```cpp
HeartbeatFloor computeHeartbeatFloor(CasOperation & op, const Layout & l, uint64_t now_ms,
                                     const std::function<uint64_t()> & mono_ms_fn, uint64_t stable_threshold_ms,
                                     MountObservationMap & obs);
```

- [ ] **Step 6: Change the two observers and the callers**

In `CA/Pool/CasServerRoot.cpp`, `claimMountAwaitingExpiry`, replace (`:960-968`):

```cpp
    std::optional<Etag> observed;
    uint64_t observed_since = 0;
    size_t restarts = 0;

    while (true)
    {
        const bool threshold_met = observed && mono_ms_fn() - observed_since >= threshold_ms;
        MountClaimResult r = claimMount(op, l, srid, our_uuid, our_epoch, now_ms_fn(), ttl_ms,
            threshold_met ? observed : std::nullopt, sink, /*unsafe_reclaim_authorization=*/{});
```

with:

```cpp
    std::optional<TokenWatch> watch;
    size_t restarts = 0;

    while (true)
    {
        const bool threshold_met = watch && watch->stableFor(threshold_ms, mono_ms_fn());
        MountClaimResult r = claimMount(op, l, srid, our_uuid, our_epoch, now_ms_fn(), ttl_ms,
            threshold_met ? std::optional<Etag>(watch->token) : std::nullopt, sink,
            /*unsafe_reclaim_authorization=*/{});
```

And replace (`:1000-1007`):

```cpp
        if (!observed || *observed != *current_etag)
        {
            if (observed && ++restarts > kMaxObservationRestarts)
                /// The incarnation kept changing across bounded restarts — the holder is genuinely alive
                /// (actively renewing), not a dead predecessor. Report it rather than waiting forever.
                return r;
            observed = *current_etag;
            observed_since = mono_ms_fn();
```

with:

```cpp
        if (!watch || watch->token != *current_etag)
        {
            if (watch && ++restarts > kMaxObservationRestarts)
                /// The incarnation kept changing across bounded restarts — the holder is genuinely alive
                /// (actively renewing), not a dead predecessor. Report it rather than waiting forever.
                return r;
            watch = TokenWatch::sighted(*current_etag, mono_ms_fn());
```

`mono_ms_fn` is called at the same points and the same number of times as before. The `&&` still skips the sample when nothing is watched. The only difference is that `stableFor` is false where the old subtraction wrapped (a clock that went backwards).

In `computeHeartbeatFloor`, replace the signature and first line (`:1020-1024`):

```cpp
HeartbeatFloor computeHeartbeatFloor(CasOperation & op, const Layout & l, uint64_t now_ms,
                                     uint64_t mono_now_ms, uint64_t stable_threshold_ms,
                                     MountObservationMap & obs)
{
    HeartbeatFloor floor;
```

with:

```cpp
HeartbeatFloor computeHeartbeatFloor(CasOperation & op, const Layout & l, uint64_t now_ms,
                                     const std::function<uint64_t()> & mono_ms_fn, uint64_t stable_threshold_ms,
                                     MountObservationMap & obs)
{
    HeartbeatFloor floor;
    const uint64_t round_start_ms = mono_ms_fn();
```

And replace the decision (`:1084-1094`):

```cpp
                const auto it = obs.find(srid);
                const bool stable = it != obs.end() && it->second.etag == observed->etag
                    && mono_now_ms - it->second.first_seen_mono_ms >= stable_threshold_ms;

                if (!stable)
                {
                    if (it == obs.end() || it->second.etag != observed->etag)
                        obs.insert_or_assign(srid, MountIncarnationObservation{observed->etag, mono_now_ms});
                    ++floor.live;
                    return std::nullopt;
                }
```

with:

```cpp
                const auto it = obs.find(srid);
                const bool watched = it != obs.end() && it->second.token == observed->etag;
                if (!watched || !it->second.stableFor(stable_threshold_ms, round_start_ms))
                {
                    if (!watched)
                        obs.insert_or_assign(srid, TokenWatch::sighted(observed->etag, round_start_ms));
                    ++floor.live;
                    return std::nullopt;
                }
```

In `CA/Gc/CasGc.cpp`, `Gc::runRegularRound`, replace (`:693-694`):

```cpp
        const HeartbeatFloor floor = computeHeartbeatFloor(op, layout, now_ms_fn(), mono_ms_fn(),
                                                           stable_threshold_ms, mount_obs);
```

with:

```cpp
        const HeartbeatFloor floor = computeHeartbeatFloor(op, layout, now_ms_fn(), mono_ms_fn,
                                                           stable_threshold_ms, mount_obs);
```

In the rebuild pass, replace (`:4707`):

```cpp
    computeHeartbeatFloor(op, layout, now_ms_fn(), mono_ms_fn(), stable_threshold_ms, mount_obs);
```

with:

```cpp
    computeHeartbeatFloor(op, layout, now_ms_fn(), mono_ms_fn, stable_threshold_ms, mount_obs);
```

`Gc::mono_ms_fn` is the member `std::function<uint64_t()>` (`CasGc.h:971`). The constructor sets it to `store->bootMsNow()` when it is empty (`CasGc.cpp:341-342`), so it is never empty at either call.

- [ ] **Step 7: Update the existing tests mechanically**

In `src/Disks/tests/gtest_cas_mount.cpp`, add this helper to the anonymous namespace that holds `seedMount` and `renewMount`, right after `renewMount` (before the closing `}` at `:1542`):

```cpp
/// An observer clock that does not move during a call: a sighting and the round start read the same value.
std::function<uint64_t()> frozenClock(uint64_t ms)
{
    return [ms] { return ms; };
}
```

Then apply exactly these substitutions in the 12 calls listed in the enumeration, and nowhere else:

| Text | Becomes | Lines |
|---|---|---|
| `/*mono_now_ms*/ 0` | `frozenClock(0)` | `:1555` |
| `/*mono*/ 0` | `frozenClock(0)` | `:1572, 1597, 1628, 1667, 1762, 1787` |
| `/*mono*/ kStableThresholdMs` | `frozenClock(kStableThresholdMs)` | `:1579, 1605, 1640, 1684, 1769` |

And these two field accesses:

| Line | Text | Becomes |
|---|---|---|
| `:1599` | `const Etag first_etag = obs.at("s1").etag;` | `const Etag first_etag = obs.at("s1").token;` |
| `:1610` | `EXPECT_EQ(obs.at("s1").etag, renewed_etag);` | `EXPECT_EQ(obs.at("s1").token, renewed_etag);` |

Also change the comment at `:1504` from `` /// The fence-out threshold measured on the LEADER's OWN monotonic clock (`mono_now_ms`), independent `` to `` /// The fence-out threshold measured on the LEADER's OWN monotonic clock (`mono_ms_fn`), independent ``.

Check afterwards:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
grep -n "/\*mono" src/Disks/tests/gtest_cas_mount.cpp                 # expect no output
git grep -n "MountIncarnationObservation\|mono_now_ms\|TODO-CYCLE-2"   # expect no output
```

- [ ] **Step 8: Build, run the affected suites, commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t1_c2.log 2>&1; echo NINJA_EXIT=$? >> build/t1_c2.log
tail -1 build/t1_c2.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASTokenWatch.*:CASHeartbeatFloor.*:CASMountAwaitExpiry.*:CASMountObservation.*:CASGCAckFloor.*:CASGcFenceOut.*' > build/t1_c2_test.log 2>&1; echo GATE_EXIT=$? >> build/t1_c2_test.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t1_c2_test.log | tail -20
```

Expected: `NINJA_EXIT=0`, no `FAILED`, `GATE_EXIT=0`. A failure here means the refactor changed behaviour. Do not adjust a test to make it pass. Find which call moved.

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp \
  src/Disks/tests/gtest_cas_mount.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit \
  -m "Watch mount tokens through one TokenWatch helper" \
  -m "\`claimMountAwaitingExpiry\` and \`computeHeartbeatFloor\` each kept their own token and first-seen pair. Both now use \`TokenWatch\`, whose stability test is false for a sample older than the sighting instead of wrapping. \`computeHeartbeatFloor\` takes the observer clock as a function; it still samples it once per call." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

#### Cycle 3: a sighting is dated after its read

- [ ] **Step 9: Write the failing test**

In `src/Disks/tests/gtest_cas_mount.cpp`, add to the same anonymous namespace, after `frozenClock`:

```cpp
/// Moves the observer clock on every read of a mount slot, so a round spends time between its first
/// clock sample and the read it decides on. With `renew_before_next_fence` set, the holder renews once
/// just before the next guarded write of a mount slot lands, so a fence-out is refused and decided
/// again on the holder's new token.
class SightingClockBackend : public InMemoryBackend
{
public:
    explicit SightingClockBackend(uint64_t & mono_) : mono(mono_) {}

    uint64_t read_cost_ms = 0;
    bool renew_before_next_fence = false;

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (key.ends_with("/mount"))
            mono += read_cost_ms;
        return InMemoryBackend::read(key, access);
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value,
                                             TransportAccess & access) override
    {
        if (renew_before_next_fence && expected_value && key.ends_with("/mount"))
        {
            renew_before_next_fence = false;
            const auto got = InMemoryBackend::read(key, access);
            MountLease m = decodeMountLease(got->bytes);
            m.seq += 1;
            EXPECT_TRUE(InMemoryBackend::write(key, encodeMountLease(m), got->value, access).has_value());
        }
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }

private:
    uint64_t & mono;
};

constexpr uint64_t kWalkMs = 4'000;

void firstDecisionCountsFromAfterTheRead()
{
    uint64_t mono = 0;
    auto b = std::make_shared<SightingClockBackend>(mono);
    Layout l("p");
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ 10, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    const auto clock = [&mono] { return mono; };
    MountObservationMap obs;

    /// The round starts at 0 and reaches the slot at `kWalkMs`.
    b->read_cost_ms = kWalkMs;
    ASSERT_EQ(computeHeartbeatFloor(ops.op, l, kNowMs, clock, kStableThresholdMs, obs).fenced_now, 0u);
    ASSERT_TRUE(obs.contains("s1"));
    EXPECT_EQ(obs.at("s1").first_seen_mono_ms, kWalkMs);

    /// One threshold after the first round started, less than one after its read.
    b->read_cost_ms = 0;
    mono = kStableThresholdMs;
    const HeartbeatFloor early = computeHeartbeatFloor(ops.op, l, kNowMs, clock, kStableThresholdMs, obs);
    ASSERT_EQ(early.fenced_now, 0u) << "fenced a token watched for less than the threshold";
    EXPECT_EQ(early.live, 1u);

    mono = kWalkMs + kStableThresholdMs;
    EXPECT_EQ(computeHeartbeatFloor(ops.op, l, kNowMs, clock, kStableThresholdMs, obs).fenced_now, 1u);
}

void reDecisionCountsFromAfterItsRead()
{
    uint64_t mono = 0;
    auto b = std::make_shared<SightingClockBackend>(mono);
    Layout l("p");
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ 10, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    const auto clock = [&mono] { return mono; };
    MountObservationMap obs;

    computeHeartbeatFloor(ops.op, l, kNowMs, clock, kStableThresholdMs, obs);
    ASSERT_TRUE(obs.contains("s1"));
    const Etag first = obs.at("s1").token;

    /// Stable at this round's start, so it tries the fence-out. The holder renews first, the write is
    /// refused, and the round decides again on the new token after reading it.
    mono = kStableThresholdMs;
    b->read_cost_ms = kWalkMs;
    b->renew_before_next_fence = true;
    const HeartbeatFloor refused = computeHeartbeatFloor(ops.op, l, kNowMs, clock, kStableThresholdMs, obs);
    ASSERT_FALSE(b->renew_before_next_fence) << "the round never attempted the fence-out";
    ASSERT_EQ(refused.fenced_now, 0u);
    ASSERT_EQ(refused.live, 1u);
    ASSERT_NE(obs.at("s1").token, first);

    /// Nothing reads after the re-decision, so the clock still holds the value of its last read.
    const uint64_t reread_at = mono;
    ASSERT_GT(reread_at, kStableThresholdMs);
    EXPECT_EQ(obs.at("s1").first_seen_mono_ms, reread_at);

    /// One threshold after that round started, less than one after its re-read.
    b->read_cost_ms = 0;
    mono = kStableThresholdMs + kStableThresholdMs;
    ASSERT_EQ(computeHeartbeatFloor(ops.op, l, kNowMs, clock, kStableThresholdMs, obs).fenced_now, 0u)
        << "fenced the renewed token before it was watched for the threshold";

    mono = reread_at + kStableThresholdMs;
    EXPECT_EQ(computeHeartbeatFloor(ops.op, l, kNowMs, clock, kStableThresholdMs, obs).fenced_now, 1u);
}

void slowWalkDoesNotMoveTheStabilitySample()
{
    uint64_t mono = 0;
    auto b = std::make_shared<SightingClockBackend>(mono);
    Layout l("p");
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ 10, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    const auto clock = [&mono] { return mono; };
    MountObservationMap obs;

    computeHeartbeatFloor(ops.op, l, kNowMs, clock, kStableThresholdMs, obs);
    ASSERT_EQ(obs.at("s1").first_seen_mono_ms, 0u);

    /// The round starts one millisecond short of the threshold; its walk passes the threshold many times.
    mono = kStableThresholdMs - 1;
    b->read_cost_ms = 10 * kStableThresholdMs;
    const HeartbeatFloor slow = computeHeartbeatFloor(ops.op, l, kNowMs, clock, kStableThresholdMs, obs);
    EXPECT_EQ(slow.fenced_now, 0u);
    EXPECT_EQ(slow.live, 1u);
    EXPECT_EQ(obs.at("s1").first_seen_mono_ms, 0u) << "an unchanged token keeps its first sighting";
}
```

Then add the test after `TEST(CASHeartbeatFloor, FenceOutLosesTheIncarnationRaceAndReclassifiesLive)` (`:1750-1778`):

```cpp
TEST(CASHeartbeat, GcCountsASightingFromAfterItsRead)
{
    {
        SCOPED_TRACE("first decision");
        firstDecisionCountsFromAfterTheRead();
    }
    {
        SCOPED_TRACE("re-decision after a refused fence-out");
        reDecisionCountsFromAfterItsRead();
    }
    {
        SCOPED_TRACE("slow walk");
        slowWalkDoesNotMoveTheStabilitySample();
    }
}
```

These are the three cases of spec test 2. In the spec the walk exceeds `threshold − TTL`. Here the threshold is abstract, and any walk shorter than the threshold shows the early credit: `kWalkMs` (4000) against `kStableThresholdMs` (10000).

The third case passes on today's code as well. It guards the fix from the other side: a fix that moved the *stability* sample after the read would fence this slot, because the walk's clock passes the threshold.

- [ ] **Step 10: Build and run it to see it fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t1_c3_red.log 2>&1; echo NINJA_EXIT=$? >> build/t1_c3_red.log
tail -1 build/t1_c3_red.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASHeartbeat.GcCountsASightingFromAfterItsRead' > build/t1_c3_red_test.log 2>&1; echo GATE_EXIT=$? >> build/t1_c3_red_test.log )
grep -a -B2 -A8 "Failure" build/t1_c3_red_test.log | head -80
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t1_c3_red_test.log | tail -20
```

Expected: `NINJA_EXIT=0`, then four failures and `GATE_EXIT=1`:
- "first decision":
  - `obs.at("s1").first_seen_mono_ms` `Which is: 0` vs `kWalkMs` `Which is: 4000`;
  - then `early.fenced_now` `Which is: 1` vs `0u`, with `fenced a token watched for less than the threshold`. The case ends at that `ASSERT`.
- "re-decision after a refused fence-out":
  - `obs.at("s1").first_seen_mono_ms` `Which is: 10000` vs `reread_at`, which is 18000 when the round made two mount reads (the observe and the resolve read after the refusal);
  - then `fenced_now` `Which is: 1` with `fenced the renewed token before it was watched for the threshold`.
- "slow walk": no failure.

Paste the failure lines into the task report. If "first decision" passes, the clock function is not being called after the read: check that `SightingClockBackend::read` is reached (it is the `owner.backend->read` in `CasOperation::readUnder`, `CA/Backend/CasRequests.cpp:507`).

- [ ] **Step 11: Implement the post-read sighting**

In `CA/Pool/CasServerRoot.cpp`, `computeHeartbeatFloor`, replace the line added in Step 6:

```cpp
    const uint64_t round_start_ms = mono_ms_fn();
```

with:

```cpp
    /// Taken before any read, so it may confirm that a token held, but never date a sighting.
    const uint64_t round_start_ms = mono_ms_fn();
```

And replace the decision from Step 6:

```cpp
                const auto it = obs.find(srid);
                const bool watched = it != obs.end() && it->second.token == observed->etag;
                if (!watched || !it->second.stableFor(stable_threshold_ms, round_start_ms))
                {
                    if (!watched)
                        obs.insert_or_assign(srid, TokenWatch::sighted(observed->etag, round_start_ms));
                    ++floor.live;
                    return std::nullopt;
                }
```

with:

```cpp
                const auto it = obs.find(srid);
                const bool watched = it != obs.end() && it->second.token == observed->etag;
                if (!watched || !it->second.stableFor(stable_threshold_ms, round_start_ms))
                {
                    /// A sample from before this read would count the walk to the slot as time watched.
                    if (!watched)
                        obs.insert_or_assign(srid, TokenWatch::sighted(observed->etag, mono_ms_fn()));
                    ++floor.live;
                    return std::nullopt;
                }
```

The lambda captures by reference (`[&]`), so `mono_ms_fn` is reachable. `readModifyWrite` calls the lambda after the first read, and again after the resolve read of a refused write (`CA/Backend/CasRequests.cpp:1107-1162`, `CasOperation::readModifyWrite`). So both the first decision and the re-decision sample after their read.

In `CA/Pool/CasServerRoot.h`, extend the sentence from Step 5:

```cpp
/// `mono_ms_fn` is the OBSERVATION clock: the caller's OWN monotonic clock, never compared against
/// any other node's clock. `obs` is owned by the caller and threaded across consecutive calls (one GC
```

to:

```cpp
/// `mono_ms_fn` is the OBSERVATION clock: the caller's OWN monotonic clock, never compared against
/// any other node's clock. It is sampled once at entry for the stability test and again after the read
/// for every sighting. `obs` is owned by the caller and threaded across consecutive calls (one GC
```

- [ ] **Step 12: Build, run this task's tests, then the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t1_c3.log 2>&1; echo NINJA_EXIT=$? >> build/t1_c3.log
tail -1 build/t1_c3.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASTokenWatch.*:CASHeartbeat.GcCountsASightingFromAfterItsRead:CASHeartbeatFloor.*:CASMountAwaitExpiry.*:CASMountObservation.*:CASGCAckFloor.*:CASGcFenceOut.*' > build/t1_c3_test.log 2>&1; echo GATE_EXIT=$? >> build/t1_c3_test.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t1_c3_test.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CAS*' > build/t1_gate.log 2>&1; echo GATE_EXIT=$? >> build/t1_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t1_gate.log | tail -20
```

Expected: `NINJA_EXIT=0`, no `FAILED`, `GATE_EXIT=0` for both runs. The gate count is the base count plus the two new tests. Re-run the enumeration greps from Step 7: still no output.

- [ ] **Step 13: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp \
  src/Disks/tests/gtest_cas_mount.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit \
  -m "Date a GC token sighting after the read that produced it" \
  -m "\`computeHeartbeatFloor\` dated a first sighting with the clock sample taken before the \`LIST\`, so a slow walk counted as time the token was watched. A second round could then fence a slot whose holder still had authority. The sighting is now sampled after the read, in the first decision and in the re-decision after a refused fence-out; the stability test keeps the round-start sample." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

**Deviations from the brief**
- `CASHeartbeat.GcCountsASightingFromAfterItsRead` and `CASTokenWatch.CountsFromAfterTheRead` go in `gtest_cas_mount.cpp`, not `gtest_cas_heartbeat.cpp`. The existing `computeHeartbeatFloor` fixtures (`seedMount`, `renewMount`, the `Ops` with its own clock) and every other direct caller live there. The suite name `CASHeartbeat` is kept as the brief gives it; gtest allows one suite across files.
- The fixture is a new `SightingClockBackend`, not `RenewalScriptBackend`. `RenewalScriptBackend` is local to `gtest_cas_heartbeat.cpp`, and it scripts the holder's writes, not the observer's reads.
- The third case of spec test 2 (a slow walk does not fence) passes on today's code. It is there to stop a fix that moves the stability sample instead of the sighting sample.
- Cycle 1 uses a temporary `TODO-CYCLE-2` block so it compiles alone. Step 5 removes it before the first commit.
- Existing tests changed: the 12 `computeHeartbeatFloor` calls and 2 `.etag` reads in `gtest_cas_mount.cpp`. The changes are mechanical and no assertion changes. No test is deleted.

**Review focus**
- **Rebuild pass.** No test covers the rebuild-pass caller (`CasGc.cpp:4707`). A GC rebuild followed at once by a regular round should date sightings the same way as two regular rounds: no fence until one threshold after the read in the rebuild.
- **Backward clock.** No test covers a clock that goes backwards between rounds, for example an injected test clock that is reset. `stableFor` returns false until the clock passes the old sighting again. This delays the fence and never makes it early. Today's code wrapped and fenced at once.
- **`claimMountAwaitingExpiry`.** No new test covers it. It already sampled in the right order, and this task only changes the type it stores. A reviewer should confirm that the number and order of `mono_ms_fn` calls per iteration did not change. `CASMountAwaitExpiry.*` and `CASMountObservation.*` advance their clock only in the sleep function, so they would not notice a moved call. Expected: a reclaim happens one full threshold after the sample taken after the read that first saw the token, and no earlier.


---

### Task 2: The lease thread is not failed by the memory tracker; inline conditional `PUT`; body-sized buffer

Closes the two gaps of issue #2403 (CAS-292) that remain after the engine already retries a memory-limit
exception raised inside a request: an exception outside the request ends the renewal thread, and the
`PUT` runs on a pool thread with a 1 MiB buffer. Spec rule 12, spec tests 3 and 4.

Facts this task relies on (implementation worktree `cas-95-1`; `CA` = `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`):

- `CasMountRuntime::renewalLoop` (`CA/Pool/CasMountRuntime.cpp:643`) runs on a plain `ThreadFromGlobalPool`
  (`makeWorker`, `:578-583`). The pool gives it a fresh `ThreadStatus` (`src/Common/ThreadPool.cpp:1103`)
  whose `memory_tracker` (`src/Common/ThreadStatus.h:195`) has the global tracker as parent
  (`src/Common/MemoryTracker.cpp:155`). `renewal_before_driver_lock_hook_for_test` (`:655`),
  `renewal_admitted_hook_for_test` (`:707`) and the catch-all (`:717-738`, which trips the fence and
  returns, ending the thread) are outside `renewRenewerOnce`.
- `consumeRenewResult` (`CasMountRuntime.cpp:458`) calls `reportMountRenewCompletion` on `Committed`
  (`:489`). For a worker call the observability is not deferred (`renewRenewerOnce`, `:530-531`), so
  `reportMountRenewCompletion` (`CA/Pool/CasServerRoot.cpp:466`) delivers at once, and
  `deliverMountRenewObservability` (`CasServerRoot.cpp:336`) calls the event sink with a
  `WatermarkRenew` / `recovered` event when the renewal committed after more than one attempt
  (`:364-378`, `emitMountRenewEvent` `:280`, which swallows sink exceptions `:313-316`). A first-attempt
  success emits nothing.
- `MemoryTracker::allocImpl` throws `MEMORY_LIMIT_EXCEEDED` over the hard limit only when
  `memoryTrackerCanThrow(level, false)` (`MemoryTracker.cpp:419`), which is false while
  `LockMemoryExceptionInThread` blocks that level (`:52-55`); then the allocation is still added to
  `amount` (`:315`) and only marked "without check" (`:472-476`). `CurrentMemoryTracker::alloc` always
  enforces the limit (`src/Common/CurrentMemoryTracker.h:28-30`), but batches up to
  `untracked_memory_limit` = 4 MiB per thread before it reaches the tracker
  (`src/Common/CurrentMemoryTracker.cpp`, `allocImpl`; `ThreadStatus.h:201`). Above 16 MiB an ignored
  limit also sends a trace (`MemoryTracker.cpp:258-273`), so the test allocates 8 MiB.
- A `DB::Exception` with `MEMORY_LIMIT_EXCEEDED` from `Backend::write` is not a deterministic local
  failure (`isDeterministicLocalFailure`, `CA/Backend/CasRequests.cpp:226`), not a credential or refusal
  class, not a hint or fuse (`:945-995`): the attempt is ambiguous, settled by one read (`:1046-1049`),
  and reissued after a backoff (`pauseAndReissue`, `:1090`). So it is retried today, under the bounded
  `Retry::untilLeaseSafe` policy (`MountLeaseRenewer::renewOn`, `CasServerRoot.cpp:1615-1616`).
- `S3ObjectStorage::writeObject` (`src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:401`)
  builds a scheduler on the remote-FS writer pool only when `write_settings.s3_allow_parallel_part_upload`
  (`:446-447`; default `true`, `src/IO/WriteSettings.h:37`). Without a scheduler `TaskTracker` runs every
  task with its synchronous runner (`src/Common/ThreadPoolTaskTracker.cpp:15-16`), so the upload worker
  that calls `PutObject` (`WriteBufferFromS3::makeSinglepartUpload`, `src/IO/WriteBufferFromS3.cpp:764`,
  `task_tracker->add` `:842`) runs on the thread that calls `finalize`. The worker increments
  `ProfileEvents::S3PutObject` on that thread's counters before `PutObject`. `WriteBuffer::finalize`
  already holds `LockMemoryExceptionInThread(VariableContext::Global)` (`src/IO/WriteBuffer.cpp:92`),
  but only for the calling thread. A multipart conditional write (over the single-part ceiling) uploads
  its parts one after another; nothing else changes.
- `buf_size` reaches `WriteBufferFromS3` unchanged when `use_adaptive_write_buffer` is false
  (`S3ObjectStorage.cpp:466`; default false, `WriteSettings.h:46`), which `conditionalWriteSettings`
  never sets. The constructor caps it at `DBMS_DEFAULT_BUFFER_SIZE` (`WriteBufferFromS3.cpp:112`) and
  grows the first buffer by itself (`reallocateFirstBuffer`, `:332`). `WriteBuffer::write` asserts a
  non-empty working buffer even for zero bytes (`chassert(!working_buffer.empty())`,
  `WriteBuffer.cpp:49`), so a 0-byte buffer would abort an empty write in debug and sanitizer builds.
- `ObjectStorageBackend::nativeConditionalPut` (`CA/Backend/CasObjectStorageBackend.cpp:309-323`) opens
  the buffer with `DBMS_DEFAULT_BUFFER_SIZE`; `conditionalWriteSettings` is at `:805-829`, its test
  accessor `conditionalWriteSettingsForTest` at `CasObjectStorageBackend.h:204`.
- No CAS test fixture can see the thread id that issues an S3 request, and `cas_scripted_s3_server.h`
  does not exist in this worktree. `src/Disks/tests/gtest_cas_s3_single_attempt_client.cpp` has a real
  local HTTP server (`DelayedResponseServer`, `:171`) and a real `S3ObjectStorage` builder
  (`makeDispatchStorageForTest`, `:199-235`). The test observes the issuing thread through its own
  `S3PutObject` counter.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp` (include; top of `CasMountRuntime::renewalLoop`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp` (`ObjectStorageBackend::nativeConditionalPut`, `ObjectStorageBackend::conditionalWriteSettings`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h` (comment of `conditionalWriteSettings`)
- Test: `src/Disks/tests/gtest_cas_pool.cpp` (`RuntimeRenewBackend::Fault`, a helper and `CASMountRuntime.MemoryLimitDoesNotEndTheLeaseThread` after `CASPoolRemount.TeardownJoinsBothWorkersBeforeRelease`)
- Test: `src/Disks/tests/gtest_cas_s3_single_attempt_client.cpp` (`makeDispatchStorageForTest` becomes a template, `BufferSizeRecordingS3ObjectStorage`, `CASBackend.ConditionalPutRunsOnTheCallingThread`, `CASBackend.ConditionalPutBufferStartsAtTheBodySize`)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces (test code later tasks may reuse):
  - `RuntimeRenewBackend::Fault::ThrowMemoryLimitExceeded` in `gtest_cas_pool.cpp`: the next conditional write throws `DB::Exception(MEMORY_LIMIT_EXCEEDED)` before reaching the store.
  - `OverLimitAllocation allocateOverTheGlobalLimit()` and `kOverLimitAllocationBytes` in `gtest_cas_pool.cpp`.
  - `template <typename Storage = DB::S3ObjectStorage> makeDispatchStorageForTest(endpoint, base_request_timeout_ms)` and `BufferSizeRecordingS3ObjectStorage` in `gtest_cas_s3_single_attempt_client.cpp`.
  - Production: the lease thread holds `LockMemoryExceptionInThread` for its whole life; every Native conditional write has `s3_allow_parallel_part_upload = false` and a buffer of `max(1, body size)`.
- Task 4 changes the `RuntimeUnderTest` constructor and the worker's retry policy. This test needs no change for that: under `Retry::untilDefinitive` its one retry waits about one spacing interval of real time instead of the backoff.

- [ ] **Step 1: Write the failing test for the memory guard**

In `src/Disks/tests/gtest_cas_pool.cpp`:

Add to the includes (after `#include <Common/Exception.h>`, keep the block sorted as it is):

```cpp
#include <Common/CurrentMemoryTracker.h>
#include <Common/CurrentThread.h>
#include <Common/MemoryTracker.h>
#include <base/scope_guard.h>
```

Add to the `DB::ErrorCodes` block at the top of the file:

```cpp
extern const int MEMORY_LIMIT_EXCEEDED;
```

In `RuntimeRenewBackend` (`gtest_cas_pool.cpp:1997`), add the enumerator at the end of `Fault`:

```cpp
    enum class Fault : uint8_t
    {
        None,
        ThrowBefore,
        LandThenThrow,
        BlockThenDelegate,
        BlockThenThrow,
        ThrowMemoryLimitExceeded,
    };
```

and in `RuntimeRenewBackend::write`, right after the block that waits on `barrier` and before
`if (current == Fault::ThrowBefore || current == Fault::BlockThenThrow)`:

```cpp
        if (current == Fault::ThrowMemoryLimitExceeded)
            throw DB::Exception(DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED, "injected memory limit exceeded on the renewal request");
```

Insert after the closing brace of `TEST(CASPoolRemount, TeardownJoinsBothWorkersBeforeRelease)` and
before `TEST(CASPoolRemount, NaturalTerminalTransitionMakesBothPersistentWorkersSelfExit)`:

```cpp
namespace
{

/// Above the 4 MiB a thread batches before it reaches the tracker, below the 16 MiB at which an
/// allocation over an ignored limit also sends a trace.
constexpr Int64 kOverLimitAllocationBytes = 8 * 1024 * 1024;

struct OverLimitAllocation
{
    /// What the tracker threw; empty when it did not throw.
    String failure;
    /// How much the global tracker grew by the allocation.
    Int64 counted = 0;
};

/// One accounted allocation made while the global tracker is over its hard limit. The limit is restored
/// before this returns, whatever the allocation did.
OverLimitAllocation allocateOverTheGlobalLimit()
{
    DB::CurrentThread::flushUntrackedMemory();
    const Int64 saved_limit = total_memory_tracker.getHardLimit();
    SCOPE_EXIT({ total_memory_tracker.setHardLimit(saved_limit); });
    total_memory_tracker.setHardLimit(1);

    OverLimitAllocation result;
    const Int64 before = total_memory_tracker.get();
    try
    {
        std::ignore = CurrentMemoryTracker::alloc(kOverLimitAllocationBytes);
    }
    catch (...)
    {
        result.failure = DB::getCurrentExceptionMessage(/*with_stacktrace=*/false);
        return result;
    }
    result.counted = total_memory_tracker.get() - before;
    std::ignore = CurrentMemoryTracker::free(kOverLimitAllocationBytes);
    return result;
}

}

/// With the global tracker over its limit, an allocation on the lease thread before the request and
/// another while the result is consumed do not throw and are still counted; a memory-limit exception
/// raised inside the request is retried, does not trip the fence, and the thread goes on renewing.
TEST(CASMountRuntime, MemoryLimitDoesNotEndTheLeaseThread)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-memory-limit");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind, MountClaimResult::Claimed);
    const Int64 hard_limit_before = total_memory_tracker.getHardLimit();

    std::atomic<uint32_t> admissions{0};
    OverLimitAllocation before_request;
    DB::Cas::tests::ManualBarrier second_admission;

    std::optional<OverLimitAllocation> while_consumed;
    String outcome;
    String attempts_sent;
    String classification;
    DB::Cas::tests::ManualBarrier reported;
    CasEventSink sink = [&](CasEvent event)
    {
        if (event.type != CasEventType::WatermarkRenew || while_consumed)
            return;
        while_consumed = allocateOverTheGlobalLimit();
        outcome = event.outcome;
        attempts_sent = event.detail["attempts_sent"];
        classification = event.detail["classification"];
        reported.arriveAndWait();
    };

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms; },
            .renewal_admitted_hook_for_test = [&]
            {
                const uint32_t admission = ++admissions;
                if (admission == 1)
                    before_request = allocateOverTheGlobalLimit();
                else if (admission == 2)
                    second_admission.arriveAndWait();
            }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    /// Declared after the runtime so it runs first: a failed expectation must not leave the lease thread
    /// parked on a barrier while the runtime's destructor joins it.
    SCOPE_EXIT({
        reported.release();
        second_admission.release();
    });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    const uint64_t leases_lost_before = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();

    backend->fault = RuntimeRenewBackend::Fault::ThrowMemoryLimitExceeded;
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));

    reported.waitUntilArrived();
    const String reported_outcome = outcome;
    reported.release();
    ASSERT_EQ(reported_outcome, "recovered") << "a memory-limit exception inside the request must be retried";
    EXPECT_EQ(attempts_sent, "2");
    EXPECT_EQ(classification, "committed_after_retry");

    /// The worker is admitted for its next renewal: the thread outlived the injected failure.
    second_admission.waitUntilArrived();
    EXPECT_TRUE(runtime.mayMutate());
    EXPECT_EQ(runtime.lifecycle(), PoolLifecycle::Live);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load(), leases_lost_before);

    EXPECT_EQ(before_request.failure, "") << "the allocation before the request threw";
    EXPECT_GE(before_request.counted, kOverLimitAllocationBytes);
    ASSERT_TRUE(while_consumed.has_value());
    EXPECT_EQ(while_consumed->failure, "") << "the allocation while the result was consumed threw";
    EXPECT_GE(while_consumed->counted, kOverLimitAllocationBytes);
    EXPECT_EQ(total_memory_tracker.getHardLimit(), hard_limit_before);

    second_admission.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}
```

Why this is deterministic: the injected boot clock never moves, so the bounded policy never runs out of
budget; the fault is armed after `startRenewer`, so the first worker renewal consumes it; the first
renewal commits on its second attempt, which is what makes `deliverMountRenewObservability` call the
sink inside `consumeRenewResult` on the lease thread. The over-limit window is open only on the lease
thread, while the test thread waits on a barrier and the remount worker waits on `driver_cv`.

What makes it fail if the guard is narrowed: the admitted hook runs outside `renewRenewerOnce`, and the
sink runs inside `consumeRenewResult` after `MountLeaseRenewer::renew` returned. A guard around the
request alone covers neither; a guard around `renewRenewerOnce` alone misses the first. Step 4 proves
this by mutation.

The retry part (`outcome == "recovered"`, `mayMutate`, `Live`, no lease loss, second admission) passes
on today's code: it is a fence for behaviour that exists. If `MEMORY_LIMIT_EXCEEDED` ever became
terminal, the event would read `failed` and the `ASSERT_EQ` stops the test.

- [ ] **Step 2: Build and run it to see it fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t2_build_1.log 2>&1; echo NINJA_EXIT=$? >> build/t2_build_1.log
tail -1 build/t2_build_1.log     # NINJA_EXIT=0; on failure: grep -a -n "error:" build/t2_build_1.log | head -40
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASMountRuntime.MemoryLimitDoesNotEndTheLeaseThread' > build/t2_test_1.log 2>&1; echo GATE_EXIT=$? >> build/t2_test_1.log )
grep -a "FAILED\|PASSED\|GATE_EXIT\|threw\|Memory limit" build/t2_test_1.log | tail -20
```

Expected: `GATE_EXIT=1`, `[  FAILED  ] CASMountRuntime.MemoryLimitDoesNotEndTheLeaseThread`, and four
failed expectations: `before_request.failure` and `while_consumed->failure` each hold a message that
starts with `Memory limit` and contains `exceeded: would use`, with "the allocation before the request
threw" and "the allocation while the result was consumed threw"; both `counted` values are `0`, below
`8388608`. The outcome, attempt count, classification, `mayMutate`, lifecycle, lease-loss count and hard
limit expectations pass. If the outcome assertion fails instead, stop: the premise that the engine
retries the injected exception is wrong.

- [ ] **Step 3: Implement the guard**

In `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp`, add the
include after `#include <Common/Exception.h>`:

```cpp
#include <Common/LockMemoryExceptionInThread.h>
```

and start `CasMountRuntime::renewalLoop` with the guard:

```cpp
void CasMountRuntime::renewalLoop()
{
    setThreadName(ThreadName::CAS_LEASE_RENEWER);
    /// The tracker still counts this thread's allocations but never throws on it: a renewal failed by a
    /// memory limit costs the mount, and an exception outside the request ends this thread.
    LockMemoryExceptionInThread memory_exception_lock(VariableContext::Global);
    {
        std::unique_lock lock(driver_mutex);
        driver_cv.wait(lock, [this] { return worker_loops_released; });
        if (workers_stop_requested || remountTerminal())
            return;
    }
```

The rest of the function is unchanged. The guard is declared once for the thread's whole run, so it
spans every iteration: the cadence waits, the hooks, `renewRenewerOnce` and `consumeRenewResult`. It is
not named `lock` because the loop declares `std::unique_lock lock` in nested scopes and `-Wshadow` is an
error.

- [ ] **Step 4: Build, run the test, prove the fence by mutation, run the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t2_build_2.log 2>&1; echo NINJA_EXIT=$? >> build/t2_build_2.log
tail -1 build/t2_build_2.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASMountRuntime.MemoryLimitDoesNotEndTheLeaseThread' > build/t2_test_2.log 2>&1; echo GATE_EXIT=$? >> build/t2_test_2.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t2_test_2.log | tail -20
```

Expected: `[  PASSED  ] 1 test.`, `GATE_EXIT=0`.

Mutation (temporary, not committed): narrow the guard to the request only. Delete the
`memory_exception_lock` line from `renewalLoop`; in `MountLeaseRenewer::renewOn`
(`CA/Pool/CasServerRoot.cpp:1613-1617`) add `#include <Common/LockMemoryExceptionInThread.h>` at the top
of the file and change the `try` body to

```cpp
    try
    {
        LockMemoryExceptionInThread request_only_lock(VariableContext::Global);
        written = op.replace(key, body, precondition(),
            Retry::untilLeaseSafe(confirmed_deadline_boot_ms, static_cast<uint64_t>(lease_safety_margin.count())));
    }
```

Rebuild into `build/t2_build_mut.log`, run the filter into `build/t2_test_mut.log`. Expected: `FAILED`
with the same four failed expectations as Step 2. Then undo both edits by hand and check:

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 diff --stat
```

Expected: only `CasMountRuntime.cpp` and `gtest_cas_pool.cpp`. `CasServerRoot.cpp` must not appear.
Rebuild (`build/t2_build_3.log`) and rerun the filter (`build/t2_test_3.log`): `PASSED`.

Gate:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CAS*' > build/t2_gate_1.log 2>&1; echo GATE_EXIT=$? >> build/t2_gate_1.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t2_gate_1.log | tail -20
```

Expected: `GATE_EXIT=0`, no `FAILED`; the pass count is the count before this task plus one.

- [ ] **Step 5: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp \
    src/Disks/tests/gtest_cas_pool.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit \
    -m "Keep memory-limit exceptions off the CAS mount-lease thread" \
    -m "The tracker still counts the renewal thread's allocations but no longer throws on it. An allocation failure outside the request used to end the thread; one inside it already was, and stays, a retried transient failure.

Related: https://github.com/Altinity/ClickHouse/issues/2403" \
    -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

- [ ] **Step 6: Write the failing tests for the inline `PUT` and the buffer size**

In `src/Disks/tests/gtest_cas_s3_single_attempt_client.cpp`:

Add includes after `#include <Common/RemoteHostFilter.h>`:

```cpp
#include <Common/CurrentThread.h>
#include <Common/ProfileEvents.h>
#include <Common/ThreadPool.h>
```

and `#include <exception>`, `#include <variant>`, `#include <vector>` to the standard includes.

Before `namespace {` (`:52`), add:

```cpp
namespace ProfileEvents
{
extern const Event S3PutObject;
}
```

Turn `makeDispatchStorageForTest` (`:199-235`) into a template. Only the first line and the `return`
change:

```cpp
template <typename Storage = DB::S3ObjectStorage>
std::shared_ptr<Storage> makeDispatchStorageForTest(const std::string & endpoint, long base_request_timeout_ms)
```

```cpp
    return std::make_shared<Storage>(
        std::move(client), std::make_unique<DB::S3Settings>(),
        DB::S3::URI(endpoint + "/test-bucket/"), DB::S3Capabilities{},
        DB::ObjectStorageKeyGeneratorPtr{}, "disk");
```

Existing callers keep compiling unchanged.

After `contextForTest` (`:237-240`), still inside the anonymous namespace, add:

```cpp
/// A genuine `S3ObjectStorage` that remembers the buffer size each `writeObject` was opened with.
class BufferSizeRecordingS3ObjectStorage final : public DB::S3ObjectStorage
{
public:
    using DB::S3ObjectStorage::S3ObjectStorage;

    std::unique_ptr<DB::WriteBufferFromFileBase> writeObject(
        const DB::StoredObject & object,
        DB::WriteMode mode,
        std::optional<DB::ObjectAttributes> attributes,
        size_t buf_size,
        const DB::WriteSettings & write_settings) override
    {
        opened_buffer_sizes.push_back(buf_size);
        return DB::S3ObjectStorage::writeObject(object, mode, attributes, buf_size, write_settings);
    }

    std::vector<size_t> opened_buffer_sizes;
};

/// A server that accepts every `PUT` with a fixed `ETag`.
void acceptPut(Poco::Net::HTTPServerResponse & response)
{
    response.set("ETag", "\"put-etag\"");
    response.setContentLength(0);
    response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
    response.send();
}

/// A writable Native backend as `openPoolView` builds one: conditional writes are single-attempt.
std::shared_ptr<DB::Cas::ObjectStorageBackend> conditionalBackendForTest(DB::ObjectStoragePtr storage)
{
    return std::make_shared<DB::Cas::ObjectStorageBackend>(
        std::move(storage), DB::Cas::ObjectStorageBackend::Mode::Native,
        /*single_attempt_control_plane_=*/true, /*attempt_timeout_ms_=*/5000, /*connect_timeout_cap_ms_=*/5000);
}
```

Before the final `#endif` (`:731`), add:

```cpp
/// A conditional `PUT` is sent by the thread that asked for it, not by the remote-FS writer pool, so a
/// memory guard the caller holds covers it. `WriteBufferFromS3` counts `S3PutObject` on the thread that
/// calls `PutObject`, and a pool thread has counters of its own.
TEST(CASBackend, ConditionalPutRunsOnTheCallingThread)
{
    (void)contextForTest();

    DelayedResponseServer server(std::chrono::milliseconds(0), acceptPut);
    auto backend = conditionalBackendForTest(makeDispatchStorageForTest(server.getUrl(), 10000));
    EXPECT_FALSE(backend->conditionalWriteSettingsForTest().s3_allow_parallel_part_upload);

    DB::Cas::CasRequests requests(DB::Cas::BackendPtr(backend), DB::Cas::Fence::open());
    bool committed = false;
    uint64_t puts_on_calling_thread = 0;
    std::exception_ptr failure;
    ThreadFromGlobalPool caller([&]
    {
        try
        {
            const uint64_t before = DB::CurrentThread::getProfileEvents()[ProfileEvents::S3PutObject].load();
            auto op = requests.admit();
            committed = std::holds_alternative<DB::Cas::Committed>(op.create("put-key", "body", DB::Cas::Retry::once()));
            puts_on_calling_thread = DB::CurrentThread::getProfileEvents()[ProfileEvents::S3PutObject].load() - before;
        }
        catch (...)
        {
            failure = std::current_exception();
        }
    });
    caller.join();
    if (failure)
        std::rethrow_exception(failure);

    EXPECT_TRUE(committed);
    EXPECT_EQ(server.requestsSeen(), 1u);
    EXPECT_EQ(puts_on_calling_thread, 1u) << "the conditional PUT was sent by another thread";
}

/// A conditional `PUT` opens its buffer at the body size instead of 1 MiB, and an empty body at one
/// byte, which is what `WriteBuffer::write` needs to accept zero bytes.
TEST(CASBackend, ConditionalPutBufferStartsAtTheBodySize)
{
    (void)contextForTest();

    DelayedResponseServer server(std::chrono::milliseconds(0), acceptPut);
    auto storage = makeDispatchStorageForTest<BufferSizeRecordingS3ObjectStorage>(server.getUrl(), 10000);
    auto backend = conditionalBackendForTest(storage);
    DB::Cas::CasRequests requests(DB::Cas::BackendPtr(backend), DB::Cas::Fence::open());

    const std::string body(37, 'b');
    {
        auto op = requests.admit();
        EXPECT_TRUE(std::holds_alternative<DB::Cas::Committed>(op.create("small-key", body, DB::Cas::Retry::once())));
    }
    {
        auto op = requests.admit();
        EXPECT_TRUE(std::holds_alternative<DB::Cas::Committed>(op.create("empty-key", "", DB::Cas::Retry::once())));
    }
    EXPECT_EQ(storage->opened_buffer_sizes, (std::vector<size_t>{body.size(), 1}));
}
```

The empty-body case runs a real `WriteBufferFromS3` with the size the backend chose, so in a debug or
sanitizer build a 0-byte buffer would abort on `chassert(!working_buffer.empty())`.

- [ ] **Step 7: Build and run them to see them fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t2_build_4.log 2>&1; echo NINJA_EXIT=$? >> build/t2_build_4.log
tail -1 build/t2_build_4.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASBackend.ConditionalPut*' > build/t2_test_4.log 2>&1; echo GATE_EXIT=$? >> build/t2_test_4.log )
grep -a "FAILED\|PASSED\|GATE_EXIT\|Which is\|Actual" build/t2_test_4.log | tail -20
```

Expected: `GATE_EXIT=1`, both tests `FAILED`.
- `ConditionalPutRunsOnTheCallingThread`: `s3_allow_parallel_part_upload` is `true`, and
  `puts_on_calling_thread` is `0`, not `1`, with "the conditional PUT was sent by another thread".
  `committed` and `requestsSeen() == 1` pass.
- `ConditionalPutBufferStartsAtTheBodySize`: `opened_buffer_sizes` is `{ 1048576, 1048576 }`, not
  `{ 37, 1 }`. Both creates commit.

If either `committed` expectation fails, stop: the fixture does not reach the server, and the red is not
for the stated reason.

- [ ] **Step 8: Implement the inline `PUT` and the buffer size**

In `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp`,
`ObjectStorageBackend::nativeConditionalPut`:

```cpp
std::expected<String, Backend::RawConflict> ObjectStorageBackend::nativeConditionalPut(
    const String & key, const String & bytes, const WriteSettings & ws)
{
    /// Sized to the body rather than 1 MiB; `WriteBufferFromS3` grows it if needed. At least one byte,
    /// because `WriteBuffer::write` requires a non-empty buffer even for an empty body.
    auto buf = object_storage->writeObject(
        StoredObject(key), WriteMode::Rewrite, /*attributes=*/std::nullopt, std::max<size_t>(1, bytes.size()), ws);
    buf->write(bytes.data(), bytes.size());
```

The rest of the function is unchanged. `<algorithm>` is already included.

In `ObjectStorageBackend::conditionalWriteSettings`, after
`ws.s3_check_objects_after_upload_override = false;`:

```cpp
    ws.s3_check_objects_after_upload_override = false;
    /// Upload on the calling thread: the caller waits for the write anyway, and a pool thread would run
    /// outside the memory guard the mount-lease thread holds.
    ws.s3_allow_parallel_part_upload = false;
```

In `CasObjectStorageBackend.h`, the comment above `conditionalWriteSettings` (`:196-202`) lists what the
settings do. Replace its first sentence

```cpp
    /// Settings for a Native COMPARE/CREATE write (create-if-absent, compare-and-set): mark the request
    /// conditional, make exactly one attempt at every retry layer, skip the racy post-upload
    /// existence/size check, and force a single PUT on generation stores because GCS does not
    /// enforce the condition on multipart completion. `attempt_no` is the engine's own 1-based
```

with

```cpp
    /// Settings for a Native COMPARE/CREATE write (create-if-absent, compare-and-set): mark the request
    /// conditional, make exactly one attempt at every retry layer, skip the racy post-upload
    /// existence/size check, upload on the calling thread, and force a single PUT on generation stores
    /// because GCS does not enforce the condition on multipart completion. `attempt_no` is the engine's own 1-based
```

Reads (`readWholeObject`, `casSizedReadSettings`) and the emulated and publication writes are not
touched.

- [ ] **Step 9: Build, run the tests of this task, then the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t2_build_5.log 2>&1; echo NINJA_EXIT=$? >> build/t2_build_5.log
tail -1 build/t2_build_5.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASBackend.ConditionalPut*:CASMountRuntime.MemoryLimitDoesNotEndTheLeaseThread:CASEnvelopeWiring.*:S3SingleAttemptClient.*:CASObjectStorageBackend.*' > build/t2_test_5.log 2>&1; echo GATE_EXIT=$? >> build/t2_test_5.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t2_test_5.log | tail -20
```

Expected: `GATE_EXIT=0`, no `FAILED`. The extra suites cover the existing users of
`makeDispatchStorageForTest` and of `ObjectStorageBackend`'s write path.

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CAS*' > build/t2_gate_2.log 2>&1; echo GATE_EXIT=$? >> build/t2_gate_2.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t2_gate_2.log | tail -20
```

Expected: `GATE_EXIT=0`, no `FAILED`; the pass count is the count of Step 4's gate plus two. The
`CAS*` filter does not select `S3SingleAttemptClient.*`, which the previous command ran.

- [ ] **Step 10: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h \
    src/Disks/tests/gtest_cas_s3_single_attempt_client.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit \
    -m "Run CAS conditional PUTs on the calling thread with a body-sized buffer" \
    -m "A conditional PUT ran on the remote-FS writer pool, outside the memory guard of the mount-lease thread, with a 1 MiB buffer for an object of a few hundred bytes. It now uploads inline and its buffer starts at the body size, one byte for an empty body.

Related: https://github.com/Altinity/ClickHouse/issues/2403" \
    -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

**Existing tests changed:** none change behaviour or assertions. `RuntimeRenewBackend::Fault` gains an
enumerator at its end; `makeDispatchStorageForTest` becomes a template whose default argument keeps
every existing call unchanged.

**Deviations from the brief**
- The guard is declared once at the top of `renewalLoop`, not inside the `while`. It spans every
  iteration, including the waits and `consumeRenewResult`, and the initial wait for
  `worker_loops_released` too. It is named `memory_exception_lock`, not `lock`: the loop declares
  `std::unique_lock lock` in nested scopes and `-Wshadow` is an error.
- No fixture can capture the id of the thread that issues an S3 request, and `cas_scripted_s3_server.h`
  does not exist in this worktree. `ConditionalPutRunsOnTheCallingThread` observes it through the
  calling thread's own `S3PutObject` counter, which `WriteBufferFromS3`'s upload worker increments on
  the thread that calls `PutObject`. It uses the server and storage builder of
  `gtest_cas_s3_single_attempt_client.cpp`, and also asserts
  `conditionalWriteSettingsForTest().s3_allow_parallel_part_upload == false`.
- Spec test 4 is split into two tests: `CASBackend.ConditionalPutRunsOnTheCallingThread` and
  `CASBackend.ConditionalPutBufferStartsAtTheBodySize`, one behaviour each.
- `CASMountRuntime.MemoryLimitDoesNotEndTheLeaseThread` lives in `gtest_cas_pool.cpp`, where
  `RuntimeUnderTest` and `RuntimeRenewBackend` are; `gtest_cas_mount_runtime.cpp` has no renewer or
  workers. Its retry half is green on today's code and is a fence; the test is red for its two
  allocations.

**Review focus**
- An unwrapped `std::bad_alloc` thrown inside a request is still terminal (`CasRequests.cpp:1003-1004`
  rethrows a non-Poco `std::exception`; `renewOn` classifies it `DeterministicFailure`). Expected: the
  renewal fences and a remount follows, as today. No test here pins it.
- A conditional write above the single-part ceiling now uploads its parts sequentially on the caller.
  Expected: same result, slower, less memory. No test exercises a multipart conditional write.
- The remount worker (`remountLoop`) holds no guard, so a memory limit there can still fail a reclaim
  attempt. Expected: the attempt fails and the existing backoff retries it; the lease thread is not
  affected.


---

### Task 3: Retry spacing in the request engine

`C` below is `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`. Line numbers are from the implementation worktree `/home/mfilimonov/workspace/ClickHouse/cas-95-1` at `41824f955e7`; Tasks 1 and 2 do not touch these files.

**Files:**
- Modify: `C/Backend/CasRetry.h` (struct `Retry`: new field after `policy_deadline_ms`, `CasRetry.h:25`; new statics after `once`, `CasRetry.h:55`; `#include <limits>`)
- Modify: `C/Backend/CasRetry.cpp` (new `Retry::spacedPause` and `Retry::drawSpacing` after `Retry::backoff`, `CasRetry.cpp:11-18`)
- Modify: `C/Backend/CasRequests.h` (`CasOperation::WriteState`, `CasRequests.h:329-343`; the three sleeping pause siblings, `CasRequests.h:407-416`; new `reissuePause` next to `reservedFor`, `CasRequests.h:422-423`; template `CasOperation::readLoop`, `CasRequests.h:450-525`)
- Modify: `C/Backend/CasRequests.cpp` (new `CasOperation::reissuePause` after `fits`, `CasRequests.cpp:445-454`; `pauseAndReissue` / `pauseForConflict` / `pauseFlat`, `CasRequests.cpp:868-884`; `writeLoop` attempt start, `CasRequests.cpp:915-918`; the seven call sites of the siblings, `CasRequests.cpp:1022, 1034, 1090, 1146, 1203`)
- Test: `src/Disks/tests/gtest_cas_requests.cpp` (suite `CASRetrySpacing` after `CASRetry.BindSaturatesAndLeavesAnEqualLeaseOffTheLeaseSource`, `gtest_cas_requests.cpp:562-580`; suite `CASRequestsSpacing` at the end of the second `#if USE_AWS_S3` block, before its `#endif` at `gtest_cas_requests.cpp:2950`)

**Interfaces:**
- Consumes: nothing from Tasks 1 and 2.
- Produces (Task 4 relies on these):
  - `std::optional<uint64_t> Retry::attempt_spacing_ms = std::nullopt;`
  - `static Retry Retry::untilDefinitive(uint64_t attempt_spacing_ms_)` returning `{.window_ms = UINT64_MAX, .lease_deadline_ms = std::nullopt, .single_attempt = false, .attempt_spacing_ms = attempt_spacing_ms_}`.
  - `static uint64_t Retry::spacedPause(uint64_t spacing_draw_ms, uint64_t request_started_ms, uint64_t now_ms)`.
  - `static uint64_t Retry::drawSpacing(uint64_t attempt_spacing_ms_)`.
  - Engine behaviour: under a policy with `attempt_spacing_ms`, `CasOperation::replace` / `create` / `readModifyWrite*` and every verb served by `readLoop` space their reissues; the first-attempt fuse and the resolve read after an unclear `PUT` stay immediate. Without `attempt_spacing_ms` every pause, and every read of the clock, is what it is today.

#### Decisions this task makes

**Where the pause is computed.** `gatedPause` (`CasRequests.cpp:845-866`) keeps its signature: it already receives the pause and sleeps it. The three siblings that sleep compute the pause and now take the policy:

| Function | Today | After |
|---|---|---|
| `pauseAndReissue` | `(WriteState & state, const Retry::Bound & bound)` | `(WriteState & state, const Retry & policy, const Retry::Bound & bound)` |
| `pauseForConflict` | `(WriteState & state, const Retry::Bound & bound)` | `(WriteState & state, const Retry & policy, const Retry::Bound & bound)` |
| `pauseFlat` | `(WriteState & state, const Retry::Bound & bound)` | `(WriteState & state, const Retry & policy, const Retry::Bound & bound)` |
| `reissueAtOnce` | unchanged | unchanged: the fuse reissue is never spaced |
| `gatedPause` | unchanged | unchanged |

The request start lives in a new `WriteState::attempt_started_ms`, set by `writeLoop` before each `PUT`. A new private `CasOperation::reissuePause(policy, request_started_ms, unspaced_ms)` returns `unspaced_ms` for an unspaced policy and the spaced pause otherwise; the siblings and `readLoop` both call it.

Every call site of the siblings (the full list, from `grep -n "pauseAndReissue\|pauseForConflict\|pauseFlat\|reissueAtOnce" C/Backend/*.cpp C/Backend/*.h`, which finds no caller outside `CasRequests.cpp`; `CasHotKeys.cpp` only constructs a `WriteState`):

| Line | Function | Call |
|---|---|---|
| `CasRequests.cpp:1022` | `writeLoop`, credential-refresh reissue | `pauseAndReissue(state, bound)` |
| `CasRequests.cpp:1034` | `writeLoop`, connect-hint reissue | `pauseFlat(state, bound)` |
| `CasRequests.cpp:1086` | `writeLoop`, fuse reissue | `reissueAtOnce(state, bound)` (unchanged) |
| `CasRequests.cpp:1090` | `writeLoop`, ordinary reissue | `pauseAndReissue(state, bound)` |
| `CasRequests.cpp:1146` | `readModifyWrite` | `pauseAndReissue(state, bound)` / `pauseForConflict(state, bound)` |
| `CasRequests.cpp:1203` | `readModifyWriteOnPresence` | `pauseAndReissue(state, bound)` / `pauseForConflict(state, bound)` |

**The conflict pause is spaced too.** A renewal never reaches it (`replace` returns `Conflict`; only the two `readModifyWrite*` loops pause on one), but a policy with no window has nothing else bounding its request rate, so a spaced policy spaces every sleeping pause. Test: `CASRequestsSpacing.CleanConflictPausesAreSpacedUnderASpacedPolicy`.

**A request that already took longer than the draw: `sleep(0)` is called.** `gatedPause` already calls `sleep_ms(0)` for its ordinary callers (`CasRequests.cpp:858-864`), and `readLoop` calls `owner.sleep_ms(pause_ms)` unconditionally (`CasRequests.h:523`). Keeping that shape means one code path; on the mount plane `CasMountRuntime::sleepInterruptibly(0)` (`CasMountRuntime.cpp:452-456`) is a zero-length predicate wait and returns at once. Tests assert the recorded sleeps are `0`.

**The clock is read for the request start only under a spacing policy.** `CASRequestsFuse.GatesRefuseTheZeroPauseReissue` (`gtest_cas_requests.cpp:2786`) counts calls of the injected `now_ms`; an unconditional sample would shift it. Sampling only when `policy.attempt_spacing_ms` is set leaves every unspaced call reading the clock exactly as today.

#### Why `Retry::untilDefinitive` never refuses on an open fence

- `Retry::bind` (`CasRetry.cpp:20-30`): `policy_deadline_ms` is empty, so `own_deadline_ms = now_ms > max - window_ms ? max : now_ms + window_ms`. With `window_ms = max`, `max - window_ms == 0`: for `now_ms > 0` the first arm gives `max`; for `now_ms == 0` the second gives `0 + max == max`. `lease_deadline_ms` is empty, so the result is `{max, false}` for every `now_ms`.
- `CasOperation::freeze` (`CasRequests.cpp:436-443`) uses `saturatingAdd` (`CasRequests.cpp:129-132`): `max`. `untilDefinitive` is not frozen by `replace`, but a hand-written loop that freezes it gets the same bound.
- `CasOperation::reservedFor` (`CasRequests.cpp:428-434`) adds envelopes with `saturatingAdd`; with a pause of at most 1200 ms and two 7 s envelopes it is far from saturating.
- `CasOperation::fits` (`CasRequests.cpp:445-454`): `now >= max` is false for every clock value below `max`, and `needed_ms <= max - now` holds until `now` is within `needed_ms` of `max`. So the only horizon is the range of the clock (`CLOCK_BOOTTIME` milliseconds: about 584 million years).
- `CasOperation::gate` (`CasRequests.cpp:413-426`) on a plane built with `Fence::open()` (`CasFence.h:27-33`): `admit` always returns `Ok`, so `gate` can refuse only through the operation's `Liveness` returning false (`CasRequests.cpp:423-424`), which every write site reports as `GaveUp{FenceLost}` and every read site as `giveUpReadFenceLost`. That is how a stop ends a spaced renewal; nothing else on that plane refuses.

Fence: `CASRetrySpacing.UntilDefinitiveHasNoWindowNoLeaseAndASpacing` asserts `bind` at `0`, `1`, `1'000'000`, `max - 1` and `max`. `CASRequestsSpacing.UntilDefinitiveRefusesNothingBeforeTheEndOfTheClock` runs the engine 20 s below `max` with 7 s envelopes: it sends, and stops only when two envelopes no longer fit before `max`. A `bind` that wrapped would make the first `fits` fail and the test would see `sent_any == false`.

#### Worst-case request rate under spacing

Script of `CASRequestsSpacing.AnUnclearPutWithAFailingReadStaysUnderEightRequestsPerSecond`: every `PUT` times out (a fuse on the first attempt, ordinary after), every resolve read is a fuse, then an ordinary failure, then an answer; requests take no time.

- Instant `t0`: `PUT1`, read 1 (fuse), read 2 (fails). Spaced wait `d1`.
- `t0 + d1`: read 3 answers, `PUT2` (fuse reissue, immediate), read 1 (fuse), read 2 (fails). Spaced wait `d2`.
- From then on each `PUT` period is `max(d, e) >= 800` ms with 4 requests: the read that answers, the `PUT`, and the fuse and failing reads that follow the `PUT` at once.

At most two periods start inside any 1000 ms bucket, so a bucket holds at most 8 requests; the sustained rate is at most 4 per 800 ms, 5 per second. Today's engine puts at least 12 requests into the first 1000 ms of the same script (`backoff(1)` pauses of at most 200 ms each).

---

- [ ] **Step 1: Write the failing tests for the policy**

In `src/Disks/tests/gtest_cas_requests.cpp`, add `#include <algorithm>` to the standard includes (before `#include <atomic>`, `gtest_cas_requests.cpp:37`).

Insert after the closing brace of `TEST(CASRetry, BindSaturatesAndLeavesAnEqualLeaseOffTheLeaseSource)` (`gtest_cas_requests.cpp:580`), before `TEST(CASRequests, CreateThenReplaceThenRemove)`:

```cpp
TEST(CASRetrySpacing, SpacedPauseWaitsOutTheRestOfTheDraw)
{
    EXPECT_EQ(Retry::spacedPause(1'000, 5'000, 5'000), 1'000u);   /// failed at once
    EXPECT_EQ(Retry::spacedPause(1'000, 5'000, 5'300), 700u);
    EXPECT_EQ(Retry::spacedPause(1'000, 5'000, 6'000), 0u);       /// took exactly the draw
    EXPECT_EQ(Retry::spacedPause(1'000, 5'000, 10'000), 0u);      /// took longer: retried at once
    /// A sample before the start counts as no time taken, so the wait is never shortened by it.
    EXPECT_EQ(Retry::spacedPause(1'000, 5'000, 4'000), 1'000u);
}

TEST(CASRetrySpacing, DrawIsWithinAFifthOfTheSpacing)
{
    bool low = false;
    bool high = false;
    for (int i = 0; i < 2'000; ++i)
    {
        const uint64_t draw = Retry::drawSpacing(1'000);
        ASSERT_GE(draw, 800u);
        ASSERT_LE(draw, 1'200u);
        low = low || draw < 850;
        high = high || draw > 1'150;
    }
    /// The bounds alone cannot tell a draw from a constant, so both ends must be reached.
    EXPECT_TRUE(low);
    EXPECT_TRUE(high);
    EXPECT_EQ(Retry::drawSpacing(0), 0u);
    constexpr uint64_t largest = std::numeric_limits<uint64_t>::max();
    EXPECT_GE(Retry::drawSpacing(largest), largest - largest / 5) << "the top of the range must not wrap";
}

TEST(CASRetrySpacing, UntilDefinitiveHasNoWindowNoLeaseAndASpacing)
{
    constexpr uint64_t largest = std::numeric_limits<uint64_t>::max();
    const Retry policy = Retry::untilDefinitive(1'000);
    EXPECT_EQ(policy.window_ms, largest);
    EXPECT_FALSE(policy.lease_deadline_ms.has_value());
    EXPECT_FALSE(policy.single_attempt);
    EXPECT_FALSE(policy.policy_deadline_ms.has_value());
    EXPECT_EQ(policy.attempt_spacing_ms, std::optional<uint64_t>(1'000));
    for (const uint64_t now : {uint64_t{0}, uint64_t{1}, uint64_t{1'000'000}, largest - 1, largest})
    {
        const Retry::Bound bound = policy.bind(now);
        EXPECT_EQ(bound.deadline_ms, largest) << "now " << now;
        EXPECT_FALSE(bound.lease_bound) << "now " << now;
    }
    /// Every other policy keeps the engine's own backoff.
    EXPECT_FALSE(Retry::standard().attempt_spacing_ms.has_value());
    EXPECT_FALSE(Retry::within(1'000).attempt_spacing_ms.has_value());
    EXPECT_FALSE(Retry::once().attempt_spacing_ms.has_value());
    EXPECT_FALSE(Retry::untilLeaseSafe(2'000'000, 2'000).attempt_spacing_ms.has_value());
}
```

Insert before the `#endif` that closes the second `#if USE_AWS_S3` block (`gtest_cas_requests.cpp:2950`, right after `TEST(CASRequestsFuse, ReadRefreshedCredentialTextDoesNotDoubleCountTheFuse)`). `connectHint` (`gtest_cas_requests.cpp:2322`), `fuseTimeout` (`:2708`) and `s3Error` (`:1718`) are defined earlier in the same translation unit under `USE_AWS_S3`; `RaceMaker` and `appendX` (`:1494`, `:1526`) are unconditional.

```cpp
namespace
{

/// A store whose requests each take `duration_ms` of the injected clock and fail as `fault` says.
/// Every request is logged with the instant it started.
class TimedFaultBackend : public InMemoryBackend
{
public:
    enum class Verb : uint8_t { Put, Get };
    struct Sent
    {
        Verb verb;
        uint64_t started_ms;
    };
    /// The exception the request fails with, or null to serve it. `nth` counts requests of `verb` from 1.
    using Fault = std::function<std::exception_ptr(Verb verb, size_t nth, uint64_t started_ms)>;

    explicit TimedFaultBackend(FakeClock & clock_) : clock(clock_) {}

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        begin(Verb::Get);
        return InMemoryBackend::read(key, access);
    }

    std::expected<String, DB::Cas::Backend::RawConflict> write(const String & key, const String & bytes,
                                                               const std::optional<String> & expected_value,
                                                               DB::Cas::TransportAccess & access) override
    {
        begin(Verb::Put);
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }

    std::vector<uint64_t> startsOf(Verb verb) const
    {
        std::vector<uint64_t> starts;
        for (const Sent & request : sent)
            if (request.verb == verb)
                starts.push_back(request.started_ms);
        return starts;
    }

    Fault fault;
    uint64_t duration_ms = 0;
    std::vector<Sent> sent;

private:
    void begin(Verb verb)
    {
        const uint64_t started = clock.now.load();
        const size_t nth = 1 + static_cast<size_t>(std::count_if(sent.begin(), sent.end(),
                                                                 [&](const Sent & request) { return request.verb == verb; }));
        sent.push_back({verb, started});
        clock.now.fetch_add(duration_ms);
        if (auto error = fault ? fault(verb, nth, started) : nullptr)
            std::rethrow_exception(error);
    }

    FakeClock & clock;
};

using Verb = TimedFaultBackend::Verb;

/// A transport failure that is neither a connect-failure hint nor a first-attempt fuse.
std::exception_ptr ordinaryFault()
{
    return std::make_exception_ptr(DB::S3Exception(
        "Poco::Exception. Code: 1000, e.code() = 104, Connection reset by peer", Aws::S3::S3Errors::NETWORK_CONNECTION));
}

constexpr uint64_t kSpacingMs = 1'000;

/// Creates `k` and forgets the requests that did it.
Etag seedK(TimedFaultBackend & backend, CasOperation & op)
{
    const Etag seen = *orThrow(op.create("k", "v1", Retry::standard()), "seed");
    backend.sent.clear();
    return seen;
}

}

/// Spacing is measured on the injected clock, so a bind that wrapped at the top of the range would
/// refuse the first request; the only refusal is two envelopes no longer fitting before the clock ends.
TEST(CASRequestsSpacing, UntilDefinitiveRefusesNothingBeforeTheEndOfTheClock)
{
    constexpr uint64_t largest = std::numeric_limits<uint64_t>::max();
    FakeClock clock;
    clock.now = largest - 20'000;
    auto backend = std::make_shared<TimedFaultBackend>(clock);
    backend->fault = [](Verb verb, size_t, uint64_t) -> std::exception_ptr
    {
        return verb == Verb::Put ? connectHint() : nullptr;
    };
    auto requests = makeRequests(backend, clock);
    requests.setAttemptReservationForTest(7'000);
    auto op = requests.admit();

    const WriteResult result = op.create("k", "v", Retry::untilDefinitive(kSpacingMs));

    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    EXPECT_EQ(gave_up->why, GaveUp::Why::Deadline);
    EXPECT_EQ(gave_up->deadline_source, GaveUp::Source::Policy);
    EXPECT_TRUE(gave_up->sent_any);
    const auto puts = backend->startsOf(Verb::Put);
    ASSERT_GE(puts.size(), 2u);
    EXPECT_EQ(puts.front(), largest - 20'000);
    EXPECT_LE(puts.back(), largest - 14'000) << "every PUT was admitted with room for its two envelopes";
}
```

- [ ] **Step 2: Build to see it fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t3_policy_red.log 2>&1; echo NINJA_EXIT=$? >> build/t3_policy_red.log
tail -1 build/t3_policy_red.log
grep -a -n "error:" build/t3_policy_red.log | head -40
```

Expected: `NINJA_EXIT=1`, with errors in `gtest_cas_requests.cpp` such as `no member named 'spacedPause' in 'DB::Cas::Retry'`, `no member named 'drawSpacing' in 'DB::Cas::Retry'`, `no member named 'untilDefinitive' in 'DB::Cas::Retry'` and `no member named 'attempt_spacing_ms' in 'DB::Cas::Retry'`. No other file fails.

- [ ] **Step 3: Implement the policy**

`C/Backend/CasRetry.h`: add the include.

```cpp
#pragma once
#include <cstdint>
#include <limits>
#include <optional>
```

`C/Backend/CasRetry.h`: add the field after `policy_deadline_ms` (`CasRetry.h:20-25`).

```cpp
    std::optional<uint64_t> policy_deadline_ms = std::nullopt;
    /// When set, a reissue in `CasOperation`'s write and read loops waits until this long (drawn in
    /// [0.8, 1.2] of it) has passed since the failed request started, instead of the engine's backoff.
    /// A request that took longer is reissued at once. The first-attempt fuse is reissued at once either way.
    std::optional<uint64_t> attempt_spacing_ms = std::nullopt;
```

`C/Backend/CasRetry.h`: add the statics after `once` (`CasRetry.h:54-55`).

```cpp
    /// The standard policy, but at most one attempt is ever sent.
    static Retry once() { return {.window_ms = 90'000, .lease_deadline_ms = std::nullopt, .single_attempt = true}; }

    /// No window and no lease bound: retried until a definitive answer, or until the fence or the
    /// caller's liveness ends it. `bind` saturates, so the only horizon is the range of the clock.
    static Retry untilDefinitive(uint64_t attempt_spacing_ms_)
    {
        return {.window_ms = std::numeric_limits<uint64_t>::max(), .lease_deadline_ms = std::nullopt,
                .single_attempt = false, .attempt_spacing_ms = attempt_spacing_ms_};
    }

    /// The pause before a retry under `attempt_spacing_ms`: `spacing_draw_ms` minus what the failed
    /// request took, not below 0. A clock sample before the start counts as no time taken.
    static uint64_t spacedPause(uint64_t spacing_draw_ms, uint64_t request_started_ms, uint64_t now_ms);
    /// A uniform draw in [0.8, 1.2] of `attempt_spacing_ms_`, saturating at the top of the range.
    static uint64_t drawSpacing(uint64_t attempt_spacing_ms_);
```

`C/Backend/CasRetry.cpp`: add after `Retry::backoff` (`CasRetry.cpp:11-18`), before `Retry::bind`.

```cpp
uint64_t Retry::spacedPause(uint64_t spacing_draw_ms, uint64_t request_started_ms, uint64_t now_ms)
{
    const uint64_t taken_ms = now_ms > request_started_ms ? now_ms - request_started_ms : 0;
    return taken_ms >= spacing_draw_ms ? 0 : spacing_draw_ms - taken_ms;
}

uint64_t Retry::drawSpacing(uint64_t attempt_spacing_ms_)
{
    const uint64_t spread = attempt_spacing_ms_ / 5;
    const uint64_t low = attempt_spacing_ms_ - spread;
    const uint64_t high = attempt_spacing_ms_ > std::numeric_limits<uint64_t>::max() - spread
        ? std::numeric_limits<uint64_t>::max()
        : attempt_spacing_ms_ + spread;
    return low + thread_local_rng() % (high - low + 1);
}
```

- [ ] **Step 4: Build and run the policy tests, then the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t3_policy_green.log 2>&1; echo NINJA_EXIT=$? >> build/t3_policy_green.log
tail -1 build/t3_policy_green.log      # NINJA_EXIT=0
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASRetrySpacing.*:CASRequestsSpacing.*:CASRetry.*' > build/t3_policy_tests.log 2>&1; echo GATE_EXIT=$? >> build/t3_policy_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t3_policy_tests.log | tail -20
```

Expected: the three `CASRetrySpacing` tests and `CASRequestsSpacing.UntilDefinitiveRefusesNothingBeforeTheEndOfTheClock` pass, `GATE_EXIT=0`. The last one passes on today's engine: it fences the saturation of `bind`, `fits` and `reservedFor`, which already holds; it has no runtime red, only the compile red of Step 2.

Gate:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CAS*' > build/t3_policy_gate.log 2>&1; echo GATE_EXIT=$? >> build/t3_policy_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t3_policy_gate.log | tail -20
```

Expected: no `FAILED` line, `GATE_EXIT=0`.

- [ ] **Step 5: Commit the policy**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.cpp \
    src/Disks/tests/gtest_cas_requests.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit \
    -m "Add an unbounded retry policy with attempt spacing to \`Retry\`" \
    -m "\`Retry::untilDefinitive\` has no window and no lease bound; \`bind\` saturates, so it ends only on an answer, the fence or liveness. \`attempt_spacing_ms\` carries the spacing the engine will apply to its reissues." \
    -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

- [ ] **Step 6: Write the failing engine tests**

Append to the anonymous-namespace helpers and tests added in Step 1, still before the `#endif` of the second `#if USE_AWS_S3` block:

```cpp
/// Spec test 10, first case: a hinted PUT is reissued without a read, every reissue starts one draw
/// after the previous one started, and the call is still retrying after 30 s.
TEST(CASRequestsSpacing, FastConnectFailuresAreSpacedFromTheirStart)
{
    FakeClock clock;
    auto backend = std::make_shared<TimedFaultBackend>(clock);
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    const Etag seen = seedK(*backend, op);
    const uint64_t t0 = clock.now;
    backend->fault = [t0](Verb verb, size_t nth, uint64_t started) -> std::exception_ptr
    {
        if (verb != Verb::Put)
            return nullptr;
        if (nth == 1)
            return fuseTimeout();
        return started < t0 + 30'000 ? connectHint() : nullptr;
    };

    const WriteResult result = op.replace("k", "v2", seen, Retry::untilDefinitive(kSpacingMs));

    const auto * committed = std::get_if<Committed>(&result);
    ASSERT_NE(committed, nullptr);
    const auto puts = backend->startsOf(Verb::Put);
    ASSERT_GE(puts.size(), 3u);
    EXPECT_EQ(committed->attempts_sent, puts.size());
    EXPECT_GE(puts.back(), t0 + 30'000) << "the call must still be retrying after 30 s";
    /// The fuse: its settle read and its reissue both follow at once.
    EXPECT_EQ(backend->sent[1].verb, Verb::Get);
    EXPECT_EQ(backend->sent[1].started_ms, t0);
    EXPECT_EQ(puts[1], t0);
    EXPECT_EQ(backend->startsOf(Verb::Get).size(), 1u) << "a hinted PUT is reissued without a read";
    uint64_t min_gap = std::numeric_limits<uint64_t>::max();
    uint64_t max_gap = 0;
    for (size_t i = 2; i < puts.size(); ++i)
    {
        min_gap = std::min(min_gap, puts[i] - puts[i - 1]);
        max_gap = std::max(max_gap, puts[i] - puts[i - 1]);
    }
    EXPECT_GE(min_gap, 800u);
    EXPECT_LE(max_gap, 1'200u);
    EXPECT_EQ(clock.sleeps.size(), puts.size() - 2) << "one sleep per hinted reissue, none for the fuse";
}

/// Spec test 10, second case: the read after an unclear PUT is sent at once, the read's own fuse is
/// reissued at once, and the retries of the read and of the PUT are spaced.
TEST(CASRequestsSpacing, TheResolveReadFollowsAtOnceAndItsRetriesAreSpaced)
{
    FakeClock clock;
    auto backend = std::make_shared<TimedFaultBackend>(clock);
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    const Etag seen = seedK(*backend, op);
    const uint64_t t0 = clock.now;
    backend->fault = [t0](Verb verb, size_t nth, uint64_t started) -> std::exception_ptr
    {
        if (verb == Verb::Put)
            return started < t0 + 30'000 ? ordinaryFault() : nullptr;
        /// Every resolve read: a fuse, an ordinary failure, then the answer.
        switch (nth % 3)
        {
            case 1: return fuseTimeout();
            case 2: return ordinaryFault();
            default: return nullptr;
        }
    };

    const WriteResult result = op.replace("k", "v2", seen, Retry::untilDefinitive(kSpacingMs));

    ASSERT_TRUE(std::holds_alternative<Committed>(result));
    const auto & sent = backend->sent;
    const auto puts = backend->startsOf(Verb::Put);
    ASSERT_GE(puts.size(), 2u);
    EXPECT_GE(puts.back(), t0 + 30'000) << "the call must still be retrying after 30 s";
    ASSERT_EQ(sent.size(), puts.size() + 3 * (puts.size() - 1)) << "three reads per failed PUT";
    bool at_once = true;
    uint64_t min_spaced = std::numeric_limits<uint64_t>::max();
    uint64_t max_spaced = 0;
    for (size_t p = 0; p + 1 < puts.size(); ++p)
    {
        const size_t i = 4 * p;   /// this PUT, then its three reads, then the next PUT
        ASSERT_EQ(sent[i].verb, Verb::Put);
        ASSERT_EQ(sent[i + 1].verb, Verb::Get);
        ASSERT_EQ(sent[i + 2].verb, Verb::Get);
        ASSERT_EQ(sent[i + 3].verb, Verb::Get);
        at_once = at_once && sent[i + 1].started_ms == sent[i].started_ms
            && sent[i + 2].started_ms == sent[i + 1].started_ms;
        for (const uint64_t gap : {sent[i + 3].started_ms - sent[i + 2].started_ms, sent[i + 4].started_ms - sent[i].started_ms})
        {
            min_spaced = std::min(min_spaced, gap);
            max_spaced = std::max(max_spaced, gap);
        }
    }
    EXPECT_TRUE(at_once) << "the read follows its PUT at once, and the read's fuse is reissued at once";
    EXPECT_GE(min_spaced, 800u);
    EXPECT_LE(max_spaced, 1'200u);
}

/// Spec test 10, third case: a request that takes 5 s on the injected clock is followed by the next one
/// with no wait, for the read and for the PUT; the PUT's own fuse reissue does not sleep at all.
TEST(CASRequestsSpacing, ARequestThatTookLongerThanTheSpacingIsRetriedAtOnce)
{
    FakeClock clock;
    auto backend = std::make_shared<TimedFaultBackend>(clock);
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    const Etag seen = seedK(*backend, op);
    backend->duration_ms = 5'000;
    const uint64_t t0 = clock.now;
    backend->fault = [t0](Verb verb, size_t nth, uint64_t started) -> std::exception_ptr
    {
        if (verb == Verb::Put)
        {
            if (nth == 1)
                return fuseTimeout();
            return started < t0 + 60'000 ? ordinaryFault() : nullptr;
        }
        return nth % 2 == 1 ? ordinaryFault() : nullptr;
    };

    const WriteResult result = op.replace("k", "v2", seen, Retry::untilDefinitive(kSpacingMs));

    ASSERT_TRUE(std::holds_alternative<Committed>(result));
    const auto & sent = backend->sent;
    bool back_to_back = true;
    for (size_t i = 1; i < sent.size(); ++i)
        back_to_back = back_to_back && sent[i].started_ms == sent[i - 1].started_ms + 5'000;
    EXPECT_TRUE(back_to_back) << "every request starts when the previous one ends";
    EXPECT_EQ(backend->startsOf(Verb::Put),
              (std::vector<uint64_t>{t0, t0 + 15'000, t0 + 30'000, t0 + 45'000, t0 + 60'000}));
    /// Four read retries and three PUT reissues, each a zero-length sleep; the fuse reissue none.
    EXPECT_EQ(clock.sleeps, std::vector<uint64_t>(7, 0));
}

/// The worst case for request rate: every PUT is unclear and every resolve read hits its fuse and then
/// a failure before it answers. The fuse and the read after a PUT are immediate, so the bound comes from
/// the spacing alone: at most two PUT periods of 4 requests start in any second.
TEST(CASRequestsSpacing, AnUnclearPutWithAFailingReadStaysUnderEightRequestsPerSecond)
{
    FakeClock clock;
    auto backend = std::make_shared<TimedFaultBackend>(clock);
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    const Etag seen = seedK(*backend, op);
    const uint64_t t0 = clock.now;
    constexpr uint64_t outage_ms = 60'000;
    backend->fault = [t0](Verb verb, size_t nth, uint64_t started) -> std::exception_ptr
    {
        if (verb == Verb::Put)
            return started < t0 + outage_ms ? fuseTimeout() : nullptr;   /// a fuse on attempt 1, an ordinary timeout after
        switch (nth % 3)
        {
            case 1: return fuseTimeout();
            case 2: return ordinaryFault();
            default: return nullptr;
        }
    };

    const WriteResult result = op.replace("k", "v2", seen, Retry::untilDefinitive(kSpacingMs));

    ASSERT_TRUE(std::holds_alternative<Committed>(result));
    const auto & sent = backend->sent;
    ASSERT_FALSE(sent.empty());
    EXPECT_GE(sent.back().started_ms, t0 + outage_ms);
    std::vector<size_t> per_second((sent.back().started_ms - t0) / 1'000 + 1, 0);
    for (const auto & request : sent)
        ++per_second[(request.started_ms - t0) / 1'000];
    EXPECT_LE(*std::max_element(per_second.begin(), per_second.end()), 8u);
    EXPECT_LE(sent.size(), 4 * (outage_ms / 800 + 2)) << "at most 4 requests per 800 ms period";
}

TEST(CASRequestsSpacing, ACredentialRefreshReissueIsSpaced)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->setRefreshCredentialsResult(true);
    backend->failNextWriteWith("k", s3Error(Aws::S3::S3Errors::INVALID_CLIENT_TOKEN_ID, "ExpiredToken"));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    const WriteResult result = op.create("k", "v", Retry::untilDefinitive(kSpacingMs));

    const auto * committed = std::get_if<Committed>(&result);
    ASSERT_NE(committed, nullptr);
    EXPECT_EQ(committed->attempts_sent, 2u);
    EXPECT_EQ(backend->getTotal(), 0u) << "the credential answer owes no read";
    EXPECT_EQ(backend->refreshCredentialsCalls(), 1u);
    ASSERT_EQ(clock.sleeps.size(), 1u);
    EXPECT_GE(clock.sleeps[0], 800u);
    EXPECT_LE(clock.sleeps[0], 1'200u);
}

/// A spaced policy has no window, so the spacing is the only bound on a conflict loop's rate too.
TEST(CASRequestsSpacing, CleanConflictPausesAreSpacedUnderASpacedPolicy)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    (void)orThrow(op.create("k", "v", Retry::standard()), "seed");
    constexpr int K = 3;
    RaceMaker races(backend, clock, "k", K, /*ambiguous=*/false);

    const WriteResult result = op.readModifyWrite("k", appendX(), Retry::untilDefinitive(kSpacingMs));

    ASSERT_TRUE(std::holds_alternative<Committed>(result));
    ASSERT_EQ(clock.sleeps.size(), static_cast<size_t>(K));
    for (const uint64_t pause : clock.sleeps)
    {
        EXPECT_GE(pause, 800u);
        EXPECT_LE(pause, 1'200u);
    }
}

/// Spacing must not leak into a policy without it: per failed PUT, the read's `backoff(1)` and then the
/// PUT's growing `backoff(n)`.
TEST(CASRequestsSpacing, AnUnspacedPolicyKeepsTheGrowingBackoff)
{
    FakeClock clock;
    auto backend = std::make_shared<TimedFaultBackend>(clock);
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    const Etag seen = seedK(*backend, op);
    backend->fault = [](Verb verb, size_t nth, uint64_t) -> std::exception_ptr
    {
        if (verb == Verb::Put)
            return nth <= 3 ? ordinaryFault() : nullptr;
        return nth % 2 == 1 ? ordinaryFault() : nullptr;
    };

    const WriteResult result = op.replace("k", "v2", seen, Retry::standard());

    const auto * committed = std::get_if<Committed>(&result);
    ASSERT_NE(committed, nullptr);
    EXPECT_EQ(committed->attempts_sent, 4u);
    const std::vector<uint64_t> ceilings{200, 200, 200, 400, 200, 800};
    ASSERT_EQ(clock.sleeps.size(), ceilings.size());
    uint64_t total = 0;
    for (size_t i = 0; i < ceilings.size(); ++i)
    {
        EXPECT_LE(clock.sleeps[i], ceilings[i]) << "sleep " << i;
        total += clock.sleeps[i];
    }
    /// Six full-jitter draws that are all zero have a probability below 1e-13; a zero-wait leak does not.
    EXPECT_GT(total, 0u);
}
```

- [ ] **Step 7: Build and run them to see them fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t3_engine_red.log 2>&1; echo NINJA_EXIT=$? >> build/t3_engine_red.log
tail -1 build/t3_engine_red.log      # NINJA_EXIT=0: the tests compile against Step 3's API
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASRequestsSpacing.*' > build/t3_engine_red_tests.log 2>&1; echo GATE_EXIT=$? >> build/t3_engine_red_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT\|Expected\|actual\|Which is" build/t3_engine_red_tests.log | tail -60
```

Expected on today's engine, `GATE_EXIT=1`:

| Test | Red | Why |
|---|---|---|
| `FastConnectFailuresAreSpacedFromTheirStart` | `Expected: (min_gap) >= (800u), actual: 50 vs 800` | `pauseFlat` sleeps `kConnectHintPauseMs` |
| `TheResolveReadFollowsAtOnceAndItsRetriesAreSpaced` | `min_spaced` below 800 (at most 200) | the read retry sleeps `backoff(1)` |
| `ARequestThatTookLongerThanTheSpacingIsRetriedAtOnce` | `back_to_back` false; `clock.sleeps` holds non-zero draws | every pause is a backoff draw regardless of the 5 s request |
| `AnUnclearPutWithAFailingReadStaysUnderEightRequestsPerSecond` | `Expected: (*std::max_element(...)) <= (8u), actual: 12` or more | `backoff(1)` pauses put at least three PUT periods into the first second |
| `ACredentialRefreshReissueIsSpaced` | `Expected: (clock.sleeps[0]) >= (800u)`, actual at most 200 | `pauseAndReissue` draws `backoff(1)` |
| `CleanConflictPausesAreSpacedUnderASpacedPolicy` | `Expected: (pause) >= (800u)`, actual at most 200 | `pauseForConflict` draws `conflictBackoff` |
| `AnUnspacedPolicyKeepsTheGrowingBackoff` | passes | fence: fails once spacing leaks into an unspaced policy |
| `UntilDefinitiveRefusesNothingBeforeTheEndOfTheClock` | passes | fence from Step 1 |

- [ ] **Step 8: Implement spacing in the engine**

`C/Backend/CasRequests.h`, `CasOperation::WriteState` (`CasRequests.h:340-342`):

```cpp
        Observation last_seen = NotObserved{};
        uint32_t reissues = 0;
        bool refresh_attempted = false;
        /// When the latest attempt was sent; sampled only under `Retry::attempt_spacing_ms`.
        uint64_t attempt_started_ms = 0;
    };
```

`C/Backend/CasRequests.h`, the readLoop declaration comment (`CasRequests.h:361`). Replace

```cpp
    /// One read-class request under the policy: admission, attempt, classification, jittered reissue.
```

with

```cpp
    /// One read-class request under the policy: admission, attempt, classification, then a jittered
    /// reissue, or a spaced one under `Retry::attempt_spacing_ms`.
```

`C/Backend/CasRequests.h`, the three sleeping siblings (`CasRequests.h:407-416`). Replace

```cpp
    /// Admission, then the jittered sleep. A value means the call ended during it; nullopt means the
    /// caller may send another attempt.
    std::optional<WriteResult> pauseAndReissue(WriteState & state, const Retry::Bound & bound);
    /// The sibling for a clean lost race: the same admission and the same reservation, a flat
    /// `Retry::conflictBackoff` sleep, and `state.reissues` untouched, so a transport fault that follows
    /// starts its own schedule at the beginning.
    std::optional<WriteResult> pauseForConflict(WriteState & state, const Retry::Bound & bound);
    /// The sibling for a failure text that named a failed connection. The same admission and the same
    /// reservation, a flat `kConnectHintPauseMs` sleep, and `state.reissues` untouched.
    std::optional<WriteResult> pauseFlat(WriteState & state, const Retry::Bound & bound);
```

with

```cpp
    /// Admission, then the jittered sleep. A value means the call ended during it; nullopt means the
    /// caller may send another attempt. This and the two siblings below sleep the spaced pause instead
    /// under `Retry::attempt_spacing_ms`.
    std::optional<WriteResult> pauseAndReissue(WriteState & state, const Retry & policy, const Retry::Bound & bound);
    /// The sibling for a clean lost race: the same admission and the same reservation, a flat
    /// `Retry::conflictBackoff` sleep, and `state.reissues` untouched, so a transport fault that follows
    /// starts its own schedule at the beginning.
    std::optional<WriteResult> pauseForConflict(WriteState & state, const Retry & policy, const Retry::Bound & bound);
    /// The sibling for a failure text that named a failed connection. The same admission and the same
    /// reservation, a flat `kConnectHintPauseMs` sleep, and `state.reissues` untouched.
    std::optional<WriteResult> pauseFlat(WriteState & state, const Retry & policy, const Retry::Bound & bound);
```

`C/Backend/CasRequests.h`, before `reservedFor` (`CasRequests.h:422`):

```cpp
    /// The pause before a reissue: `unspaced_ms`, or under `Retry::attempt_spacing_ms` what is left of
    /// a fresh draw since `request_started_ms`.
    uint64_t reissuePause(const Retry & policy, uint64_t request_started_ms, uint64_t unspaced_ms) const;

    /// `sleep_ms` plus `envelopes` attempt reservations, saturating.
    uint64_t reservedFor(uint64_t sleep_ms, uint32_t envelopes) const;
```

`C/Backend/CasRequests.h`, `CasOperation::readLoop` (`CasRequests.h:454-455`). Replace

```cpp
    bool refresh_attempted = false;
    /// Two counters, deliberately kept separate: `attempt_no` is the PHYSICAL attempt count handed to
```

with

```cpp
    bool refresh_attempted = false;
    uint64_t attempt_started_ms = 0;
    /// Two counters, deliberately kept separate: `attempt_no` is the PHYSICAL attempt count handed to
```

`CasRequests.h:469-472`. Replace

```cpp
        if (!fits(reservation, bound))
            giveUpReadDeadline(verb, subject, bound, attempt_no - 1);

        detail::recordAttempt();
```

with

```cpp
        if (!fits(reservation, bound))
            giveUpReadDeadline(verb, subject, bound, attempt_no - 1);

        if (policy.attempt_spacing_ms)
            attempt_started_ms = owner.now_ms();
        detail::recordAttempt();
```

`CasRequests.h:512`. Replace

```cpp
        const uint64_t pause_ms = Retry::backoff(++ordinary_reissues);
```

with

```cpp
        const uint64_t pause_ms = reissuePause(policy, attempt_started_ms, Retry::backoff(++ordinary_reissues));
```

The fuse branch (`CasRequests.h:496-509`) is untouched: it `continue`s before this pause.

`C/Backend/CasRequests.cpp`, after `CasOperation::fits` (`CasRequests.cpp:445-454`):

```cpp
uint64_t CasOperation::reissuePause(const Retry & policy, uint64_t request_started_ms, uint64_t unspaced_ms) const
{
    if (!policy.attempt_spacing_ms)
        return unspaced_ms;
    return Retry::spacedPause(Retry::drawSpacing(*policy.attempt_spacing_ms), request_started_ms, owner.now_ms());
}
```

`C/Backend/CasRequests.cpp`, the siblings (`CasRequests.cpp:868-884`). Replace

```cpp
std::optional<WriteResult> CasOperation::pauseAndReissue(WriteState & state, const Retry::Bound & bound)
{
    return gatedPause(Retry::backoff(++state.reissues), 2, state, bound, detail::recordReissue, /*should_sleep=*/true);
}

std::optional<WriteResult> CasOperation::pauseForConflict(WriteState & state, const Retry::Bound & bound)
{
    return gatedPause(Retry::conflictBackoff(), 2, state, bound, detail::recordConflictPause, /*should_sleep=*/true);
}

/// A flat pause before reissuing an attempt whose failure text named a failed connection.
static constexpr uint64_t kConnectHintPauseMs = 50;

std::optional<WriteResult> CasOperation::pauseFlat(WriteState & state, const Retry::Bound & bound)
{
    return gatedPause(kConnectHintPauseMs, 2, state, bound, detail::recordReissue, /*should_sleep=*/true);
}
```

with

```cpp
std::optional<WriteResult> CasOperation::pauseAndReissue(WriteState & state, const Retry & policy, const Retry::Bound & bound)
{
    const uint64_t pause_ms = reissuePause(policy, state.attempt_started_ms, Retry::backoff(++state.reissues));
    return gatedPause(pause_ms, 2, state, bound, detail::recordReissue, /*should_sleep=*/true);
}

std::optional<WriteResult> CasOperation::pauseForConflict(WriteState & state, const Retry & policy, const Retry::Bound & bound)
{
    const uint64_t pause_ms = reissuePause(policy, state.attempt_started_ms, Retry::conflictBackoff());
    return gatedPause(pause_ms, 2, state, bound, detail::recordConflictPause, /*should_sleep=*/true);
}

/// A flat pause before reissuing an attempt whose failure text named a failed connection.
static constexpr uint64_t kConnectHintPauseMs = 50;

std::optional<WriteResult> CasOperation::pauseFlat(WriteState & state, const Retry & policy, const Retry::Bound & bound)
{
    const uint64_t pause_ms = reissuePause(policy, state.attempt_started_ms, kConnectHintPauseMs);
    return gatedPause(pause_ms, 2, state, bound, detail::recordReissue, /*should_sleep=*/true);
}
```

`C/Backend/CasRequests.cpp`, `writeLoop` (`CasRequests.cpp:915-918`). Replace

```cpp
        if (!fits(reservation, bound))
            return gaveUp(GaveUp::Why::Deadline, sourceFor(bound), state);

        detail::recordAttempt();
        ++state.attempts_sent;
```

with

```cpp
        if (!fits(reservation, bound))
            return gaveUp(GaveUp::Why::Deadline, sourceFor(bound), state);

        if (policy.attempt_spacing_ms)
            state.attempt_started_ms = owner.now_ms();
        detail::recordAttempt();
        ++state.attempts_sent;
```

The resolve read after an unclear `PUT` (`CasRequests.cpp:1046-1049`) and the fuse reissue (`CasRequests.cpp:1084-1089`, `reissueAtOnce`) are untouched.

`C/Backend/CasRequests.cpp`, the call sites. Every call passes `state, bound` today and gets `state, policy, bound`; `writeLoop`, `readModifyWrite` and `readModifyWriteOnPresence` all have `policy` in scope:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
F=src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp
grep -c "pauseAndReissue(state, bound)" $F     # 4: lines 1022, 1090, 1146, 1203
grep -c "pauseForConflict(state, bound)" $F    # 2: lines 1146, 1203
grep -c "pauseFlat(state, bound)" $F           # 1: line 1034
sed -i -e 's/pauseAndReissue(state, bound)/pauseAndReissue(state, policy, bound)/g' \
       -e 's/pauseForConflict(state, bound)/pauseForConflict(state, policy, bound)/g' \
       -e 's/pauseFlat(state, bound)/pauseFlat(state, policy, bound)/g' $F
grep -n "pauseAndReissue(\|pauseForConflict(\|pauseFlat(\|reissueAtOnce(" $F
```

The last `grep` must show the three definitions with `const Retry & policy`, `reissueAtOnce(state, bound)` at the fuse branch and in its own definition, and the seven calls above with `state, policy, bound`. Anything else is a call site this list missed: stop and report it.

- [ ] **Step 9: Build, run the tests of this task and their neighbours, then the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t3_engine_green.log 2>&1; echo NINJA_EXIT=$? >> build/t3_engine_green.log
tail -1 build/t3_engine_green.log      # NINJA_EXIT=0
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASRetrySpacing.*:CASRequestsSpacing.*:CASRetry.*:CASRequests.*:CASRequestsConnectHint.*:CASRequestsFuse.*' > build/t3_engine_tests.log 2>&1; echo GATE_EXIT=$? >> build/t3_engine_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t3_engine_tests.log | tail -20
```

Expected: no `FAILED` line, `GATE_EXIT=0`. `CASRequestsFuse.GatesRefuseTheZeroPauseReissue` stays green because the unspaced path reads the clock no more often than before.

Run each new randomized test repeatedly to rule out an unlucky draw:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASRetrySpacing.*:CASRequestsSpacing.*' --gtest_repeat=50 > build/t3_engine_repeat.log 2>&1; echo GATE_EXIT=$? >> build/t3_engine_repeat.log )
grep -a -c "FAILED" build/t3_engine_repeat.log; tail -1 build/t3_engine_repeat.log    # 0, GATE_EXIT=0
```

Gate:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CAS*' > build/t3_engine_gate.log 2>&1; echo GATE_EXIT=$? >> build/t3_engine_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t3_engine_gate.log | tail -20
```

Expected: no `FAILED` line, `GATE_EXIT=0`.

- [ ] **Step 10: Commit the engine change**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp \
    src/Disks/tests/gtest_cas_requests.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit \
    -m "Space engine reissues under \`Retry::attempt_spacing_ms\`" \
    -m "A spaced policy reissues a \`PUT\` (ordinary, connect-hint and credential-refresh), a conflict and a read no sooner than a spacing draw after the failed request started; a request that took longer is reissued at once. The first-attempt fuse and the read after an unclear \`PUT\` stay immediate. Policies without spacing keep their pauses and their clock reads." \
    -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

**Existing tests changed or deleted:** none. No existing test calls the private siblings, and the only behaviour change is under a policy no existing test uses.

**Deviations from the brief**

- `gatedPause` keeps its signature. The pause is computed by its three sleeping callers, which now take `const Retry & policy`; the request start is `WriteState::attempt_started_ms`. A new private `CasOperation::reissuePause` holds the rule for both loops. `reissueAtOnce` is unchanged, so the fuse cannot be spaced by construction.
- The doc comment of `Retry::attempt_spacing_ms` names the write and read loops instead of "every retry pause": the hand-written loops in `removeCurrent` (`CasRequests.cpp:642`) and `probeSentinel` (`CasRequests.cpp:701`) keep `Retry::backoff`. The spec puts those loops out of scope, and no caller passes `untilDefinitive` to them.
- The conflict pause is spaced too (see Decisions).
- The engine tests live in `gtest_cas_requests.cpp` inside `#if USE_AWS_S3`, because the fuse and the connect hint are classified from `S3Exception`. A build without S3 compiles and runs only `CASRetrySpacing`.
- The engine-level suite is `CASRequestsSpacing`; the renewer-level `CASHeartbeat.RenewalSpacesRetries` of spec test 10 belongs to Task 4.

**Review focus**

- A resolve read that never answers under `untilDefinitive` retries forever at one request per spacing, and the `PUT` is not reissued until the read answers. Expected: correct per the spec (the read belongs to the call that sent the `PUT`), but a stop must still end it: on the mount plane `sleepInterruptibly` wakes and the next `gate` refuses through `Liveness`. No test here drives a stop through a spaced read retry; Task 4's `StopDuringARetryWaitEndsTheRenewal` should include one.
- `readModifyWrite` under `untilDefinitive` with a key that keeps moving loops forever at one conflict per spacing. Expected: nobody uses that combination today; a reader would expect a caller of `untilDefinitive` to supply a `Liveness` that ends it.
- A spacing draw near the top of the `uint64_t` range: `drawSpacing` saturates and `fits` refuses the pause as not fitting, so the call gives up with `GaveUp{Deadline, Policy}` rather than sleeping. Expected: harmless for the 1000 ms constant; not tested beyond `DrawIsWithinAFifthOfTheSpacing`.


---

### Task 4: The worker's renewal without a deadline (CAS-298; spec rules 2 to 7, 9, 10)

The background worker's renewal stops being bounded by the lease. It runs on a new request plane with
no fence (`lease_requests`), under `Retry::untilDefinitive(kMountRenewRetrySpacingMs)`, and ends only on
a definitive answer or when its liveness predicate refuses (stop, park, terminal lifecycle, lost fence).
The startup, remount and direct renewals keep `Retry::untilLeaseSafe` on their current planes. The
engine gains a per-attempt observer so the renewing thread learns of each `PUT` and each failure as it
happens; Task 5 fills what the runtime does with it.

Paths below: `W` = `/home/mfilimonov/workspace/ClickHouse/cas-95-1`,
`CA` = `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`. Line numbers are from `W` at
`41824f955e7`; Tasks 1 to 3 land first and may shift them, so every hunk below also quotes its anchor.

**Files:**
- Modify: `CA/Backend/CasRequests.h` (`CasOperation`: public `RequestObserver`, `setRequestObserver` next to `hotKeys` ~:261; private `notifyRequest` and member `request_observer` next to `last_read_stop` ~:447)
- Modify: `CA/Backend/CasRequests.h` (`CasOperation::readLoop` catch ~:477; private member `resolve_read_put_no`)
- Modify: `CA/Backend/CasRequests.cpp` (`CasOperation::notifyRequest` before `writeLoop` ~:891; three calls in `CasOperation::writeLoop` ~:918, ~:945, ~:996; the resolve read in `CasOperation::writeLoop` ~:1047)
- Modify: `CA/Pool/CasServerRoot.h` (`MountRenewPolicy`, `kMountRenewRetrySpacingMs`, `MountRenewRequestEvent` after `MountRenewResult` ~:62; two fields in `MountRenewOperationEnvironment` ~:64-74; `MountLeaseRenewer` constructor ~:534, PLANES paragraph ~:525-530, `renew` doc ~:548, member ~:575)
- Modify: `CA/Pool/CasServerRoot.cpp` (`describeRequestFailure` before `MountLeaseRenewer::MountLeaseRenewer` ~:1308; constructor ~:1308-1329; `MountLeaseRenewer::renew` ~:1561; `MountLeaseRenewer::renewOn` ~:1611-1616; one comment in `MountLeaseRenewer::terminate` ~:1750)
- Modify: `CA/Pool/CasMountRuntime.h` (constructor ~:147-160; `renewalLive` doc ~:448; member ~:460)
- Modify: `CA/Pool/CasMountRuntime.cpp` (constructor ~:53-73; `CasMountRuntime::installRenewer` ~:369; `CasMountRuntime::renewalEnvironment` ~:422-432)
- Modify: `CA/Pool/CasPool.h` (`PoolConfig::retry_sleep_fn` doc ~:259-266; planes header ~:757; `setCasRetrySleepForTest` / `setCasRequestNowFnForTest` docs ~:1075-1091; `hot_keys` doc ~:1201; plane members ~:1206-1211)
- Modify: `CA/Pool/CasPool.cpp` (`Pool::Pool` ~:175-254; `Pool::setCasRetrySleepForTest` ~:1947; `Pool::setCasRequestNowFnForTest` ~:1960)
- Modify (constructor sweep): `src/Disks/tests/gtest_cas_heartbeat.cpp`, `gtest_cas_mount.cpp`, `gtest_cas_event_log.cpp`, `gtest_cas_gc_ack_floor.cpp`, `gtest_cas_mount_claim_conflicts.cpp`, `gtest_cas_mount_runtime.cpp`, `gtest_cas_pool.cpp`
- Test: `src/Disks/tests/gtest_cas_requests.cpp`, `src/Disks/tests/gtest_cas_heartbeat.cpp`, `src/Disks/tests/gtest_cas_pool.cpp`

**Interfaces:**
- Consumes (Task 3): `Retry::untilDefinitive(uint64_t attempt_spacing_ms_)`, `Retry::attempt_spacing_ms`, and the engine's spacing: under `attempt_spacing_ms` the ordinary, connect-hint and credential-refresh reissue of a `PUT` and every retry of the resolve read wait until a draw in [0.8, 1.2] of the spacing has passed since the failed request started; the read after an unclear `PUT` and the first-attempt fuse reissue go at once.
- Produces:
  - `enum class MountRenewPolicy : uint8_t { LeaseBound, UntilDefinitive }`, `inline constexpr uint64_t kMountRenewRetrySpacingMs = 1000`, `struct MountRenewRequestEvent { uint32_t request_no; bool failed; String failure_text; }` in `CA/Pool/CasServerRoot.h`.
  - `MountRenewOperationEnvironment::policy` (default `LeaseBound`) and `MountRenewOperationEnvironment::on_request` (`std::function<void(const MountRenewRequestEvent &)>`, may be empty).
  - `MountLeaseRenewer(CasRequests & mount_requests_, CasRequests & open_requests_, CasRequests & worker_requests_, const Layout & layout_, ...)`; `renew` runs `UntilDefinitive` on `worker_requests` with `Retry::untilDefinitive(kMountRenewRetrySpacingMs)`.
  - `CasOperation::RequestObserver` = `std::function<void(uint32_t attempt_no, const std::exception * failure)>`, `CasOperation::setRequestObserver(RequestObserver)`. Contract: once per `PUT` when sent (`failure == nullptr`, `attempt_no` = 1-based `PUT` count); once per `PUT` that throws (`failure != nullptr`, same `attempt_no`); once per failed resolve read of a write of that operation (`failure != nullptr`, `attempt_no` = the `PUT` count at that moment). Not reported: a refused precondition, a resolve read that succeeds, a read the caller issued itself, a refusal by the gate. A throwing observer is ignored. Unset, it costs one empty-`std::function` test per request.
  - In `MountRenewRequestEvent` a failed resolve read arrives as `failed = true` with `request_no` = that `PUT` count.
  - `CasMountRuntime(BackendPtr, CasRequests & mount_requests_, CasRequests & farewell_requests_, CasRequests & lease_requests_, const Layout &, ...)`.
  - `Pool::lease_requests` (private member; `Fence::open()`, the pool's boot clock, `config.retry_sleep_fn` or `mountPlaneSleepFn()`).
  - `CasMountRuntime::renewalEnvironment(worker_call)` sets `.policy = worker_call ? UntilDefinitive : LeaseBound` and `.on_request = {}`. Task 5 replaces the `{}`.
  - Test fixtures: `Ops::lease` (both test files that have an `Ops`); in `gtest_cas_heartbeat.cpp` `RenewalScriptBackend::{outage, outage_action, read_actions, read_outage, on_attempt, on_read, Action::ThrowFirstAttemptFuse}` and `UnboundedRenewalFixture`; in `gtest_cas_pool.cpp` `RuntimeUnderTest::setRetrySleepForTest` and `RuntimeRenewBackend::{outage, on_outage_write, outage_writes}`. Task 5's `ExpiryRefusesWritesAndResumesUnderTheSameEpoch` / `ExpiryIsReported` can drive the real worker loop with these.

**Enumeration of constructor call sites.** Derived with
`git -C $W grep -c -E "MountLeaseRenewer [a-z_0-9]+\(|make_unique<MountLeaseRenewer>\(|return MountLeaseRenewer\(" -- src/`
and `git -C $W grep -n -E "^\s*(DB::Cas::)?CasMountRuntime [a-z_]+;" -- src/`:

| Type | File | Sites | Change |
|---|---|---|---|
| `MountLeaseRenewer` | `CA/Pool/CasMountRuntime.cpp` (`installRenewer`) | 1 | pass `lease_requests` third |
| `MountLeaseRenewer` | `gtest_cas_heartbeat.cpp` | 30, all `ops.mount, ops.farewell,` | `ops.mount, ops.farewell, ops.lease,` |
| `MountLeaseRenewer` | `gtest_cas_mount.cpp` | 8 `ops.mount, ops.farewell,` + 4 `mount_requests, open_requests, l,` | `ops.lease` third; `open_requests` third |
| `MountLeaseRenewer` | `gtest_cas_event_log.cpp` (`DeepReentrancyPreservesDeterministicPhysicalAttemptTruth`) | 1 | `*planes[index]` a third time |
| `MountLeaseRenewer` | `gtest_cas_gc_ack_floor.cpp` | 2 | `renewer_requests` a third time |
| `MountLeaseRenewer` | `gtest_cas_mount_claim_conflicts.cpp` (`makeRenewer`) | 1 | `requests` a third time |
| `CasMountRuntime` | `CA/Pool/CasPool.h` member `mount_runtime`, built in `Pool::Pool` | 1 | pass `lease_requests` |
| `CasMountRuntime` | `gtest_cas_mount_runtime.cpp` `RuntimeFixture` | 1 | new member `lease` |
| `CasMountRuntime` | `gtest_cas_pool.cpp` `RuntimeUnderTest` | 1 | new member `lease` |

A missed site does not compile: the new parameter is a non-defaulted reference placed before
`const Layout &`, which a `Layout` argument cannot bind to. The build is the gate for completeness;
re-run both greps after the sweep and expect the same files and counts.

**What happens to existing tests that assert a deadline-terminal renewal.**

| Test | File | Path it drives | Fate |
|---|---|---|---|
| `CASHeartbeat.RenewalStopsBeforeTheCutoffWhenEveryAttemptConsumesTheEnvelope` | heartbeat | `renew(env)` with default env | kept: default policy is `LeaseBound` |
| `CASHeartbeat.DeadlineBeforeSendTerminalizesWithTypedFailure` | heartbeat | same | kept, same reason |
| `CASHeartbeat.RenewerStateAllowsOnlyActiveReleaseOrTerminal`, `LateDeliveryAfterTerminalCannotRearmOrOverwriteSuccessor`, `WallClockStepsAndBootSuspendCannotExtendAuthority` | heartbeat | same | kept, same reason |
| `CASEvent.AnAmbiguityPastTheLeaseBoundNeverStartsTheResolvingRead`, `CASEvent.TerminalRenewalDetailsPreservePhysicalTruthAndClassification` | event_log | `renewWatermarkOnce` (direct, `worker_call=false`) | kept: `LeaseBound` |
| `CASObservability.ExternalLeaseDeadlineCountsOnceWithoutReconstructingAttempts` | observability | `renewWatermarkOnce` | kept: `LeaseBound` |
| `CASPoolRemount.ParkedRedoFailureObservabilityPrecedesRemountResult` | pool | remount redo (`renewForRemount`) | kept: `LeaseBound` |
| `CASPool.DirectAndStartupTerminalFailuresRethrowTypedExceptions`, `CASPool.RenewWatermarkOnceRefreshesFenceAndDepositsOneFailure` | pool | startup / direct | kept: `LeaseBound` |
| `CASPoolRemount.TerminalDepositionDoesNotTouchRenewerAfterReplacement` | pool | worker, `ThrowBefore` + clock moved to the deadline | **retargeted** (Step 4): the slot is fenced, a definitive answer |
| `CASPoolRemount.ImmediatePostRemountRenewalFailureIsNotDropped` | pool | worker after remount, same fault shape | **retargeted** (Step 4): same |
| `CASPool.DeterministicWorkerFailureFencesWithoutWaitingForCadence` | pool | worker, same fault shape | **retargeted** (Step 4): same |
| `CASPoolRemount.RemountWaitsForRenewalParkedBeforeReplacement`, `ExternalLossDuringRenewalUsesOneRecoveryGeneration`, `DueWorkerAdmissionIsReservedBeforeParkRequest` | pool | worker ended by park / lost fence | kept: `renewalLive(true)` still refuses on `ParkRequested`, `Parked` and `mount_fence.lost` |
| `CASPoolRemount.DueWorkerAdmissionIsReservedBeforeStop`, `CASPoolShutdown.PreSendCancellationAllowsFarewellButAmbiguityDoesNot` | pool | worker ended by stop | kept: liveness and the `cancelled` sample are unchanged |
| `CASRefContiguousAlloc.SurvivingWriterIsFencedByTheRecreatedPoolsMount` | ref_contiguous_alloc | worker meets a foreign slot | kept: a definitive answer, terminal under both policies |

Every other worker-enabled test in `gtest_cas_pool.cpp`, `gtest_cas_forget.cpp` and
`gtest_cas_lifecycle_condition.cpp` injects no renewal fault the deadline used to end. If the gate
shows any other failure, stop and report it; do not retarget a test that is not in this table.

---

- [ ] **Step 1: Write the failing engine tests for the request observer**

Append to `$W/src/Disks/tests/gtest_cas_requests.cpp`, after `TEST(CASRequests, AmbiguousCreateThatNeverLandedIsReissued)` (~:693):

```cpp
/// The observer hears of each physical write attempt when it is sent, and of each one that throws.
TEST(CASRequests, RequestObserverSeesEachPutAndEachFailure)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->injectAmbiguousWrite("k");
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    std::vector<std::pair<uint32_t, bool>> seen;
    op.setRequestObserver([&](uint32_t attempt_no, const std::exception * failure)
    {
        seen.emplace_back(attempt_no, failure != nullptr);
    });

    WriteResult result = op.create("k", "v", Retry::standard());

    ASSERT_TRUE(std::holds_alternative<Committed>(result));
    const std::vector<std::pair<uint32_t, bool>> expected{{1, false}, {1, true}, {2, false}};
    EXPECT_EQ(seen, expected);
}

/// A deterministic local failure is reported before it propagates unchanged.
TEST(CASRequests, RequestObserverSeesADeterministicFailureBeforeItPropagates)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->failNextWriteWith("k", std::make_exception_ptr(DB::Exception(
        DB::ErrorCodes::CORRUPTED_DATA, "injected deterministic failure")));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    std::vector<std::pair<uint32_t, int>> seen;
    op.setRequestObserver([&](uint32_t attempt_no, const std::exception * failure)
    {
        const auto * db_failure = dynamic_cast<const DB::Exception *>(failure);
        seen.emplace_back(attempt_no, failure == nullptr ? 0 : (db_failure ? db_failure->code() : -1));
    });

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { (void)op.create("k", "v", Retry::standard()); });

    const std::vector<std::pair<uint32_t, int>> expected{{1, 0}, {1, DB::ErrorCodes::CORRUPTED_DATA}};
    EXPECT_EQ(seen, expected);
}

/// A refused precondition is the store's answer, not a failed request.
TEST(CASRequests, RequestObserverIsNotToldOfARefusedPrecondition)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->refuseNextWrite("k");
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    std::vector<std::pair<uint32_t, bool>> seen;
    op.setRequestObserver([&](uint32_t attempt_no, const std::exception * failure)
    {
        seen.emplace_back(attempt_no, failure != nullptr);
    });

    WriteResult result = op.create("k", "v", Retry::standard());

    EXPECT_TRUE(std::holds_alternative<Conflict>(result));
    const std::vector<std::pair<uint32_t, bool>> expected{{1, false}};
    EXPECT_EQ(seen, expected);
}

/// Each failed resolve read is reported with the count of `PUT`s sent so far; the read that succeeds is not.
TEST(CASRequests, RequestObserverSeesEachFailedResolveRead)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->injectAmbiguousWrite("k");
    backend->failNextReadWith("k", std::make_exception_ptr(Poco::TimeoutException("injected read failure")));
    backend->failNextReadWith("k", std::make_exception_ptr(Poco::TimeoutException("injected read failure")));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    std::vector<std::pair<uint32_t, bool>> seen;
    op.setRequestObserver([&](uint32_t attempt_no, const std::exception * failure)
    {
        seen.emplace_back(attempt_no, failure != nullptr);
    });

    WriteResult result = op.create("k", "v", Retry::standard());

    ASSERT_TRUE(std::holds_alternative<Committed>(result));
    /// PUT 1 sent, PUT 1 failed, two failed reads, PUT 2 sent.
    const std::vector<std::pair<uint32_t, bool>> expected{{1, false}, {1, true}, {1, true}, {1, true}, {2, false}};
    EXPECT_EQ(seen, expected);
}

/// A read the caller issues itself is not a resolve read and is not reported.
TEST(CASRequests, RequestObserverIsNotToldOfACallersOwnRead)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->failNextReadWith("k", std::make_exception_ptr(Poco::TimeoutException("injected read failure")));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    uint32_t calls = 0;
    op.setRequestObserver([&](uint32_t, const std::exception *) { ++calls; });

    EXPECT_FALSE(op.read("k", Retry::standard()).has_value());

    EXPECT_EQ(calls, 0u);
}

/// An observer that throws on every call changes neither the verdict nor the requests sent.
TEST(CASRequests, ThrowingRequestObserverChangesNothing)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->injectAmbiguousWrite("k");
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    uint32_t calls = 0;
    op.setRequestObserver([&](uint32_t, const std::exception *)
    {
        ++calls;
        throw std::runtime_error("injected observer failure");
    });

    WriteResult result = op.create("k", "v", Retry::standard());

    const auto * committed = std::get_if<Committed>(&result);
    ASSERT_NE(committed, nullptr);
    EXPECT_EQ(committed->attempts_sent, 2u);
    EXPECT_EQ(backend->getTotal(), 1u);
    EXPECT_EQ(calls, 3u);
}
```

- [ ] **Step 2: Build and run it to see it fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t4_s2_build.log 2>&1; echo NINJA_EXIT=$? >> build/t4_s2_build.log
tail -1 build/t4_s2_build.log; grep -a -n "error:" build/t4_s2_build.log | head -40
```

Expected: `NINJA_EXIT=1`, and `gtest_cas_requests.cpp` errors `no member named 'setRequestObserver' in 'DB::Cas::CasOperation'`. The observer is new API; a compile failure is its red.

- [ ] **Step 3: Implement the observer**

`$W/CA/Backend/CasRequests.h`, in `class CasOperation`, after `CasHotKeys & hotKeys() const { return *owner.hot_keys; }` (~:261):

```cpp
    /// The write lane for keys several writers of this pool share.
    CasHotKeys & hotKeys() const { return *owner.hot_keys; }

    /// Told of the requests of this operation's writes, on the issuing thread: each `PUT` when it is
    /// sent (null `failure`) and again if it throws, and each failed resolve read. `attempt_no` is the
    /// count of `PUT`s sent so far. A refused precondition is an answer, not a failure, and is not
    /// reported; nor is a read that succeeds or one the caller issued itself. What the observer throws
    /// is ignored.
    using RequestObserver = std::function<void(uint32_t attempt_no, const std::exception * failure)>;
    void setRequestObserver(RequestObserver observer) { request_observer = std::move(observer); }
```

Same class, private section, after the declaration of `gaveUpAfterFailedObservation` (~:399):

```cpp
    /// Calls `request_observer` if one is set. A report must not change a verdict, so whatever the
    /// observer throws is swallowed here.
    void notifyRequest(uint32_t attempt_no, const std::exception * failure) const noexcept;
```

Same class, members, after `std::optional<ReadStop> last_read_stop;` (~:447):

```cpp
    std::optional<ReadStop> last_read_stop;
    RequestObserver request_observer;
    /// The `PUT` count while a write's resolve read runs, 0 otherwise. A read failure is reported only
    /// when it is set, so the caller's own reads stay silent.
    uint32_t resolve_read_put_no = 0;
```

Same file, `CasOperation::readLoop` (~:477), first line of the `catch` (if Task 3 added a request-start
sample or other lines at the top of this `catch`, put this line first):

```cpp
        catch (const std::exception & e)
        {
            if (resolve_read_put_no != 0)
                notifyRequest(resolve_read_put_no, &e);
            bool refreshed = false;
```

`$W/CA/Backend/CasRequests.cpp`, immediately before `WriteResult CasOperation::writeLoop(` (~:891):

```cpp
void CasOperation::notifyRequest(uint32_t attempt_no, const std::exception * failure) const noexcept
{
    if (!request_observer)
        return;
    try
    {
        request_observer(attempt_no, failure);
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
    }
}
```

In `CasOperation::writeLoop`, three insertions. After the counter block (~:918-920; if Task 3 added a
request-start sample next to it, keep that and add this line after `state.sent_any = true;`):

```cpp
        detail::recordAttempt();
        ++state.attempts_sent;
        state.sent_any = true;
        notifyRequest(state.attempts_sent, nullptr);
```

First line of `catch (const Exception & e)` (~:945):

```cpp
        catch (const Exception & e)
        {
            notifyRequest(state.attempts_sent, &e);
            if (isDeterministicLocalFailure(e.code()))
                throw;
```

First line of `catch (const std::exception & e)` (~:996):

```cpp
        catch (const std::exception & e)
        {
            notifyRequest(state.attempts_sent, &e);
            /// Only the transport can have landed anything, and every exception it raises is a
```

The resolve read in `CasOperation::writeLoop` (~:1046-1049), replace

```cpp
        ProfileEvents::increment(ProfileEvents::CASRequestResolveRead);
        const Resolved resolved = resolve_refusal_with == ResolveWith::Presence && !state.any_ambiguous
            ? observePresence(key, policy, bound)
            : observe(key, policy, bound);
```

with

```cpp
        ProfileEvents::increment(ProfileEvents::CASRequestResolveRead);
        resolve_read_put_no = state.attempts_sent;
        Resolved resolved;
        {
            SCOPE_EXIT({ resolve_read_put_no = 0; });
            resolved = resolve_refusal_with == ResolveWith::Presence && !state.any_ambiguous
                ? observePresence(key, policy, bound)
                : observe(key, policy, bound);
        }
```

and add `#include <base/scope_guard.h>` to the includes of `CasRequests.cpp`. `Resolved` is an
aggregate of `Observation` (default `NotObserved`) and `std::optional<ReadStop>`, so it
default-constructs; `resolved` is read below exactly as the `const` was. The `SCOPE_EXIT` clears the
marker on the deterministic-failure path too, where `observe` rethrows. The other `observe` /
`observePresence` calls (in `readModifyWrite` and `readModifyWriteOnPresence`) are the loop's own
reads, not resolve reads of a `PUT`, and stay silent.

- [ ] **Step 4: Build, run the tests of this step, then the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t4_s4_build.log 2>&1; echo NINJA_EXIT=$? >> build/t4_s4_build.log
tail -1 build/t4_s4_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASRequests.RequestObserver*:CASRequests.ThrowingRequestObserver*' > build/t4_s4_tests.log 2>&1; echo GATE_EXIT=$? >> build/t4_s4_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t4_s4_tests.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CAS*' > build/t4_s4_gate.log 2>&1; echo GATE_EXIT=$? >> build/t4_s4_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t4_s4_gate.log | tail -20
```

Expected: `NINJA_EXIT=0`; six tests `PASSED`, `GATE_EXIT=0`; gate `GATE_EXIT=0`, no `FAILED`.
Prove the two guards are load-bearing, one at a time, rebuild, run, see `FAILED`, restore, rebuild:
- delete the `try`/`catch` in `notifyRequest` (call the observer directly): `CASRequests.ThrowingRequestObserverChangesNothing` fails with an uncaught `injected observer failure`;
- drop the `if (resolve_read_put_no != 0)` condition in `readLoop` (notify with `0`): `CASRequests.RequestObserverIsNotToldOfACallersOwnRead` fails with `calls` 1.

- [ ] **Step 5: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp \
  src/Disks/tests/gtest_cas_requests.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Report each CAS write attempt to an optional observer" \
  -m "A caller that renews the mount lease needs to see requests and failures while a write is still retrying. CasOperation::setRequestObserver is told of every physical PUT when sent, of every PUT that throws, and of every failed resolve read; a refused precondition, a successful read and the caller's own reads are not reported, and an observer that throws is ignored." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

- [ ] **Step 6: Plumb the lease plane (no behaviour change)**

This step adds the types, the third renewer plane, the runtime parameter and the pool member. Only
`UntilDefinitive` is routed to the new plane, and nothing sets it yet, so every existing caller keeps
its plane and its policy. There is no new test; the existing gate is the check.

`$W/CA/Pool/CasServerRoot.h`, after the closing `};` of `struct MountRenewResult` (~:62):

```cpp
/// How far a renewal may retry.
enum class MountRenewPolicy : uint8_t
{
    LeaseBound,        /// startup, remount and direct renewals: `Retry::untilLeaseSafe`
    UntilDefinitive,   /// the background worker: `Retry::untilDefinitive(kMountRenewRetrySpacingMs)`
};

/// The spacing of an `UntilDefinitive` renewal's retries.
inline constexpr uint64_t kMountRenewRetrySpacingMs = 1000;

/// One physical request of a renewal, reported as it happens: a `PUT` sent (`failed` false), a `PUT`
/// that failed, or a failed read that settles a `PUT` (both `failed` true).
struct MountRenewRequestEvent
{
    uint32_t request_no = 0;        /// 1-based count of `PUT`s sent by this renewal
    bool failed = false;
    String failure_text;            /// empty unless `failed`
};
```

Same file, `struct MountRenewOperationEnvironment` (~:64-74), add after the `cancelled` member:

```cpp
    std::function<bool()> cancelled;
    MountRenewPolicy policy = MountRenewPolicy::LeaseBound;
    /// Called on the renewing thread for every request sent and for every failure. May be empty.
    /// What it throws is ignored.
    std::function<void(const MountRenewRequestEvent &)> on_request;
};
```

Same file, `MountLeaseRenewer` constructor declaration (~:534-536):

```cpp
    MountLeaseRenewer(
        CasRequests & mount_requests_, CasRequests & open_requests_, CasRequests & worker_requests_,
        const Layout & layout_,
        const String & srid_, UInt128 server_uuid_,
```

Same class, members (~:574-575):

```cpp
    CasRequests & mount_requests;
    CasRequests & open_requests;
    /// The plane of an `UntilDefinitive` renewal: no lease budget, and a sleep a stop wakes.
    CasRequests & worker_requests;
    String key;
```

`$W/CA/Pool/CasServerRoot.cpp`, `MountLeaseRenewer::MountLeaseRenewer` (~:1308-1318):

```cpp
MountLeaseRenewer::MountLeaseRenewer(
    CasRequests & mount_requests_, CasRequests & open_requests_, CasRequests & worker_requests_,
    const Layout & layout_,
    const String & srid_, UInt128 server_uuid_,
    uint64_t writer_epoch_, std::chrono::milliseconds ttl_, std::function<uint64_t()> now_ms_fn_,
    std::function<uint64_t()> min_active_build_sequence_fn_,
    CasEventSink event_sink_,
    std::chrono::milliseconds lease_safety_margin_,
    std::function<uint64_t()> boot_ms_fn_)
    : mount_requests(mount_requests_)
    , open_requests(open_requests_)
    , worker_requests(worker_requests_)
    , key(layout_.mountKey(srid_))
```

`MountLeaseRenewer::renew` (~:1561-1564), whole function:

```cpp
MountRenewResult MountLeaseRenewer::renew(const MountRenewOperationEnvironment & environment)
{
    return renewOn(
        environment.policy == MountRenewPolicy::UntilDefinitive ? worker_requests : mount_requests, environment);
}
```

`$W/CA/Pool/CasMountRuntime.h`, constructor (~:147-153):

```cpp
    CasMountRuntime(
        BackendPtr backend_ptr_,
        /// The planes the `MountLeaseRenewer` runs on: a bounded renewal under the mount fence, the
        /// claim and the farewell on an open one, and the worker's renewal on `lease_requests_`, which
        /// has no lease budget and whose sleep a stop wakes. Owned by `Pool` and outliving this runtime.
        CasRequests & mount_requests_,
        CasRequests & farewell_requests_,
        CasRequests & lease_requests_,
        const Layout & layout_,
```

Same file, members (~:459-460):

```cpp
    CasRequests & mount_requests;
    CasRequests & farewell_requests;
    CasRequests & lease_requests;
    const Layout & layout;
```

`$W/CA/Pool/CasMountRuntime.cpp`, constructor (~:53-66):

```cpp
CasMountRuntime::CasMountRuntime(
    BackendPtr backend_ptr_,
    CasRequests & mount_requests_,
    CasRequests & farewell_requests_,
    CasRequests & lease_requests_,
    const Layout & layout_,
    MountConfig config_,
    String server_root_id_,
    const CasEventSink & event_sink_,
    CasRequestBudget cas_request_budget_,
    std::function<bool()> remount_attempt_)
    : backend_ptr(std::move(backend_ptr_))
    , mount_requests(mount_requests_)
    , farewell_requests(farewell_requests_)
    , lease_requests(lease_requests_)
    , layout(layout_)
```

`CasMountRuntime::installRenewer` (~:369-370):

```cpp
    auto replacement = std::make_unique<MountLeaseRenewer>(
        mount_requests, farewell_requests, lease_requests, layout, server_root_id, our_uuid, writer_epoch,
```

`$W/CA/Pool/CasPool.h`. `PoolConfig::retry_sleep_fn` doc (~:259-262), first sentence:

```cpp
    /// The inter-attempt sleep for the request planes (`mount_requests`, `lease_requests`,
    /// `farewell_requests`, `gc_requests`), installed at their CONSTRUCTION -- before this `Pool` has
    /// claimed or read anything. Empty = each plane's own production default (an interruptible real
    /// sleep for the mount, lease and GC planes, `CasRequests`'s own real sleep for the farewell plane). A test
```

(keep the rest of that comment). Planes header (~:757): `/// ---- the three request planes ----` becomes `/// ---- request planes ----`.
`setCasRetrySleepForTest` doc (~:1075-1079): replace "on all\n    /// three request planes" with "on every\n    /// request plane", and "On the three request planes an empty function" with "On the request planes an empty function".
`setCasRequestNowFnForTest` doc (~:1087): "on all three planes" becomes "on every plane".
`hot_keys` doc (~:1201): "declared before the three planes" becomes "declared before the planes".
Plane members (~:1206-1211):

```cpp
    /// The planes' engines, declared before every component that is handed one and after the
    /// config they take their clock from. `mutable` because issuing a request is not a change to the
    /// pool: a `const` observer still has to read the store.
    mutable CasRequests mount_requests;
    mutable CasRequests farewell_requests;
    mutable CasRequests gc_requests;
    /// The worker renewal's plane: no lease budget, because the renewal keeps trying after the lease
    /// expired; a stop, a park or a terminal lifecycle ends it through its liveness.
    mutable CasRequests lease_requests;
```

`$W/CA/Pool/CasPool.cpp`, `Pool::Pool`. Comment ~:175-178, last sentence "All three\n    /// planes take the fence's own clock" becomes "Every\n    /// plane takes the fence's own clock". After the `gc_requests(...)` initializer, which ends `&hot_keys)` (~:203), add:

```cpp
          config.retry_sleep_fn ? config.retry_sleep_fn : openPlaneSleepFn(),
          &hot_keys)
    , lease_requests(pool_backend, Fence::open(), config.boot_ms_fn,
          config.retry_sleep_fn ? config.retry_sleep_fn : mountPlaneSleepFn(),
          &hot_keys)
```

`mount_runtime(` initializer (~:250-251):

```cpp
    , mount_runtime(
          pool_backend, mount_requests, farewell_requests, lease_requests,
          pool_layout, config.mountConfig(), config.server_root_id, event_sink_,
```

`Pool::setCasRetrySleepForTest` (~:1947-1958), whole function:

```cpp
void Pool::setCasRetrySleepForTest(std::function<void(uint64_t)> sleep_fn)
{
    /// Every plane, not just the ledger's: a test that replaces the retry sleep must not be left with
    /// a real one on the plane the site under test happens to use.
    farewell_requests.setSleepFnForTest(sleep_fn);
    ref_ledger.setCasRetrySleepForTest(sleep_fn);
    /// `CasRequests` falls back to the engine's plain sleep for an empty argument -- which is none of
    /// the mount, lease or open plane's defaults. Re-install them, so clearing the seam cannot leave a
    /// parked or stopping renewal held for a whole wait, or the open plane deaf to a teardown.
    gc_requests.setSleepFnForTest(sleep_fn ? sleep_fn : openPlaneSleepFn());
    lease_requests.setSleepFnForTest(sleep_fn ? sleep_fn : mountPlaneSleepFn());
    mount_requests.setSleepFnForTest(sleep_fn ? std::move(sleep_fn) : mountPlaneSleepFn());
}
```

`Pool::setCasRequestNowFnForTest` (~:1960-1965), whole function:

```cpp
void Pool::setCasRequestNowFnForTest(std::function<uint64_t()> now_fn)
{
    mount_requests.setNowFnForTest(now_fn);
    farewell_requests.setNowFnForTest(now_fn);
    lease_requests.setNowFnForTest(now_fn);
    gc_requests.setNowFnForTest(std::move(now_fn));
}
```

**Test fixtures and the call-site sweep.**

`$W/src/Disks/tests/gtest_cas_heartbeat.cpp`, replace the `Ops` comment and class (~:37-67):

```cpp
/// The request planes this file's renewers run on. All are open-fence -- the exclusivity these tests
/// exercise is the mount protocol's own, not a fence's -- on the same injected boot clock the renewer's
/// lease deadline is expressed on, so they never disagree about how much budget is left.
/// `sleep_step_ms`, when set, makes one inter-attempt pause jump the clock past the lease bound: that
/// is how a test asks for exactly one physical attempt without a per-call attempt cap. It depends on
/// the engine checking the bound, sleeping, then checking again -- a reissue that slept first would
/// send a second attempt. `tests::OperationForTest` covers a fixture needing one operation, but
/// neither the planes a renewer takes nor this clock, which is why this stays local.
class Ops
{
public:
    Ops(std::shared_ptr<Backend> backend, uint64_t * boot_ms, uint64_t sleep_step_ms = 0)
        : mount(openRequestsForTest(backend))
        , farewell(openRequestsForTest(backend))
        , lease(openRequestsForTest(std::move(backend)))
        , op(mount.admit())
    {
        for (CasRequests * requests : {&mount, &farewell, &lease})
        {
            requests->setNowFnForTest([boot_ms] { return *boot_ms; });
            requests->setSleepFnForTest(
                [boot_ms, sleep_step_ms](uint64_t ms) { *boot_ms += sleep_step_ms ? sleep_step_ms : ms; });
        }
    }

    Ops(const Ops &) = delete;
    Ops & operator=(const Ops &) = delete;

    CasRequests mount;
    CasRequests farewell;
    CasRequests lease;
    CasOperation op;
};
```

Then in the same file replace every `ops.mount, ops.farewell,` with `ops.mount, ops.farewell, ops.lease,`
(30 sites; `grep -c "ops\.mount, ops\.farewell, ops\.lease," src/Disks/tests/gtest_cas_heartbeat.cpp` must print 30).

`$W/src/Disks/tests/gtest_cas_mount.cpp`, replace the `Ops` comment and class (~:62-95):

```cpp
/// The request planes a renewer in this file runs on, plus one operation for the protocol calls driven
/// directly. All planes are open-fence: these fixtures hold no mount lease, so nothing here should be
/// refused by a fence it does not have. The clock and the sleep are ALWAYS injected -- a fixture that
/// drives a lease deadline passes its own so a slow machine cannot run the bound out mid-test, and one
/// that does not still must not sleep for real when a fault sends the engine round again.
/// `tests::OperationForTest` covers the one-operation case but neither the planes nor the clock, which
/// is why this stays local.
class Ops
{
public:
    explicit Ops(std::shared_ptr<Backend> backend) : Ops(std::move(backend), nullptr) {}

    Ops(std::shared_ptr<Backend> backend, uint64_t * boot_ms)
        : mount(openRequestsForTest(backend))
        , farewell(openRequestsForTest(backend))
        , lease(openRequestsForTest(std::move(backend)))
        , op(mount.admit())
    {
        uint64_t * clock = boot_ms ? boot_ms : &own_clock;
        for (CasRequests * requests : {&mount, &farewell, &lease})
        {
            requests->setNowFnForTest([clock] { return *clock; });
            requests->setSleepFnForTest([clock](uint64_t ms) { *clock += ms; });
        }
    }

    Ops(const Ops &) = delete;
    Ops & operator=(const Ops &) = delete;

    CasRequests mount;
    CasRequests farewell;
    CasRequests lease;
    CasOperation op;

private:
    uint64_t own_clock = 0;
};
```

Same file: `ops.mount, ops.farewell,` becomes `ops.mount, ops.farewell, ops.lease,` (8 sites, ~:726, :746,
:802, :845, :1157, :1941, :2130, :2141); `mount_requests, open_requests, l,` becomes
`mount_requests, open_requests, open_requests, l,` (4 sites, ~:2180, :2183, :2221, :2281). The four
tests drive `start` and `renew` only, neither of which reaches the third plane under `LeaseBound`.

`$W/src/Disks/tests/gtest_cas_event_log.cpp` (~:373-375):

```cpp
        renewers[index] = std::make_unique<MountLeaseRenewer>(
            *planes[index],
            *planes[index],
            *planes[index],
            *layouts[index],
```

`$W/src/Disks/tests/gtest_cas_gc_ack_floor.cpp` (~:936 and ~:1064), both sites:

```cpp
    MountLeaseRenewer srid2_renewer(renewer_requests, renewer_requests, renewer_requests, layout, srid2, DB::UInt128(0x2222),
```

`$W/src/Disks/tests/gtest_cas_mount_claim_conflicts.cpp` (~:15-30): in the comment above `makeRenewer`
"Both\n/// of its planes are the same open-fence one" becomes "All\n/// of its planes are the same open-fence one", and:

```cpp
    return MountLeaseRenewer(
        requests,
        requests,
        requests,
        Layout("p"),
```

`$W/src/Disks/tests/gtest_cas_mount_runtime.cpp`, `RuntimeFixture` (~:21-50): initializer list and members:

```cpp
        , farewell(backend, Fence::open())
        , lease(backend, Fence::open())
        , runtime(
              backend, mount, farewell, lease, layout,
```

```cpp
    CasRequests mount;
    CasRequests farewell;
    CasRequests lease;
    CasMountRuntime runtime;
```

`$W/src/Disks/tests/gtest_cas_pool.cpp`, replace `RuntimeUnderTest` (~:2050-2098) whole:

```cpp
/// A directly-constructed `CasMountRuntime` plus the request planes it needs. `Pool` builds those
/// from its own members; a test has no `Pool`, so the mount plane's fence reaches the runtime through
/// this holder -- the closures run only once the runtime is issuing requests, well after construction.
class RuntimeUnderTest
{
public:
    template <typename BackendT, typename... Args>
    RuntimeUnderTest(const std::shared_ptr<BackendT> & backend, Args &&... args)
        : mount(backend, DB::Cas::Fence{
              [this] { return runtime.fenceGeneration(); },
              [this](uint64_t g, uint64_t needed) { return runtime.admit(g, needed); },
              [this](uint64_t g) { runtime.checkFenceOrThrow(g); }})
        , farewell(backend, DB::Cas::Fence::open())
        , lease(backend, DB::Cas::Fence::open())
        , runtime(backend, mount, farewell, lease, std::forward<Args>(args)...)
    {
        /// What the request engine reserves per attempt is the BACKEND's attempt timeout, not the
        /// budget field alone; every construction of this holder pairs the two via `runtimeRenewBudget`,
        /// the sole budget it is ever built with in this file.
        backend->setAttemptTimeoutMs(runtimeRenewBudget().attempt_timeout_ms);
        /// The runtime arms its lease deadline on ITS boot clock, and the engine measures that deadline
        /// against the clock it reads. Production runs both on `CLOCK_BOOTTIME`, so they agree; a test
        /// that injects one MUST inject the other, or `Retry::untilLeaseSafe` compares a synthetic
        /// deadline against real boottime, finds it long past, and refuses every request unsent.
        mount.setNowFnForTest([this] { return runtime.bootMsNow(); });
        farewell.setNowFnForTest([this] { return runtime.bootMsNow(); });
        lease.setNowFnForTest([this] { return runtime.bootMsNow(); });
        /// As `Pool` wires it: a stop wakes the worker renewal's wait.
        lease.setSleepFnForTest([this](uint64_t ms) { runtime.sleepInterruptibly(ms); });
    }

    /// The workers are joined HERE, not only by the tests that assert on teardown: `CasMountRuntime`
    /// aborts the process when it is destroyed with a worker still joinable, so an exception on any
    /// path out of a test body -- a barrier that timed out, an assertion that threw -- would take the
    /// whole binary down and hide every test after it.
    ~RuntimeUnderTest()
    {
        try
        {
            runtime.stopBackgroundWorkers();
        }
        catch (...)   // NOLINT(bugprone-empty-catch)
        {
        }
    }

    CasMountRuntime & operator*() { return runtime; }

    /// The retry wait of the mount and lease planes, the two a renewal can run on. Call before the
    /// workers start.
    void setRetrySleepForTest(const std::function<void(uint64_t)> & sleep_fn)
    {
        mount.setSleepFnForTest(sleep_fn);
        lease.setSleepFnForTest(sleep_fn);
    }

private:
    DB::Cas::CasRequests mount;
    DB::Cas::CasRequests farewell;
    DB::Cas::CasRequests lease;
    CasMountRuntime runtime;
};
```

Build and run the gate exactly as in Step 4 with logs `t4_s6_build.log` / `t4_s6_gate.log`. Expected:
`NINJA_EXIT=0`, gate `GATE_EXIT=0`, no `FAILED`. Re-run the two enumeration greps from the top of this
task; they must print the same files and counts. If a compile error names a constructor call site,
that site was missed by the sweep: fix it the same way, do not add a default argument.

Commit:

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp \
  src/Disks/tests/gtest_cas_heartbeat.cpp src/Disks/tests/gtest_cas_mount.cpp \
  src/Disks/tests/gtest_cas_event_log.cpp src/Disks/tests/gtest_cas_gc_ack_floor.cpp \
  src/Disks/tests/gtest_cas_mount_claim_conflicts.cpp src/Disks/tests/gtest_cas_mount_runtime.cpp \
  src/Disks/tests/gtest_cas_pool.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Give the CAS mount renewer a lease plane with no fence" \
  -m "Pool owns lease_requests (open fence, the mount plane's interruptible sleep) and hands it through CasMountRuntime to MountLeaseRenewer, which routes an UntilDefinitive renewal there. Adds MountRenewPolicy, kMountRenewRetrySpacingMs, MountRenewRequestEvent and the policy/on_request environment fields. Nothing sets UntilDefinitive yet, so no renewal changes plane or policy." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

- [ ] **Step 7: Write the failing renewer-level tests**

All in `$W/src/Disks/tests/gtest_cas_heartbeat.cpp`.

At the top, extend the error codes (~:20-24):

```cpp
namespace DB::ErrorCodes
{
    extern const int NETWORK_ERROR;
    extern const int ABORTED;
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
}
```

and add `#include <functional>` and `#include <memory>` to the standard includes (~:13-18).

Extend `RenewalScriptBackend` (~:88-173). Replace the class whole:

```cpp
/// Not `final`: `EnvelopeEatingBackend` (the envelope-cutoff test below) derives from it to
/// reuse its `Attempt`/`attempts` bookkeeping while overriding `write`/`read` with its own always-fail
/// behavior instead of the scripted-action queue.
class RenewalScriptBackend : public InMemoryBackend
{
public:
    enum class Action : uint8_t
    {
        Delegate,
        ThrowBefore,
        LandThenThrow,
        ReturnThenCancel,
        ThrowBeforeThenLandAfterResolve,
        ThrowConnectHint,
        ThrowFirstAttemptFuse,
        ThrowStoreRefusal,
    };

    struct Attempt
    {
        String key;
        String bytes;
        std::optional<String> expected;
    };

    std::deque<Action> actions;
    std::vector<Attempt> attempts;
    std::function<void()> cancel_after_write;
    uint64_t read_calls = 0;
    /// Consulted when `actions` is empty: TRUE fails the guarded write with `outage_action`.
    std::function<bool()> outage;
    Action outage_action = Action::ThrowBefore;
    /// Scripted answers for reads of a mount slot: `ThrowBefore` and `ThrowFirstAttemptFuse` fail the
    /// read, `Delegate` serves it. Consulted before `read_outage`.
    std::deque<Action> read_actions;
    /// TRUE fails a read of a mount slot with a transport timeout.
    std::function<bool()> read_outage;
    /// Called on every scripted write and on every read of a mount slot, before it is answered.
    std::function<void()> on_attempt;
    std::function<void()> on_read;

    /// Only a GUARDED write of a mount slot is scripted; the fixture's own seeding and every other
    /// key reach the store untouched.
    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value,
                                             TransportAccess & access) override
    {
        if (!expected_value || !key.ends_with("/mount"))
            return InMemoryBackend::write(key, bytes, expected_value, access);

        attempts.push_back({key, bytes, expected_value});
        if (on_attempt)
            on_attempt();
        Action action = Action::Delegate;
        if (!actions.empty())
        {
            action = actions.front();
            actions.pop_front();
        }
        else if (outage && outage())
        {
            action = outage_action;
        }

        if (action == Action::ThrowConnectHint)
        {
#if USE_AWS_S3
            throw DB::S3Exception("Poco::Exception. Code: 1000, e.code() = 99, Cannot assign requested address: 10.0.0.1:9000",
                                  Aws::S3::S3Errors::NETWORK_CONNECTION);
#else
            throw Poco::TimeoutException("connect timed out");
#endif
        }
        if (action == Action::ThrowFirstAttemptFuse)
            throwFirstAttemptFuse();
        if (action == Action::ThrowStoreRefusal)
        {
#if USE_AWS_S3
            throw DB::S3Exception("the store answered MalformedXML", Aws::S3::S3Errors::UNKNOWN, "MalformedXML");
#else
            throw DB::Exception(DB::ErrorCodes::ABORTED, "a store refusal needs USE_AWS_S3");
#endif
        }

        if (action == Action::ThrowBefore || action == Action::ThrowBeforeThenLandAfterResolve)
        {
            if (action == Action::ThrowBeforeThenLandAfterResolve)
                pending = Attempt{key, bytes, expected_value};
            throw Poco::TimeoutException("injected renewal response uncertainty before a result");
        }

        auto result = InMemoryBackend::write(key, bytes, expected_value, access);
        if (action == Action::LandThenThrow)
        {
            if (cancel_after_write)
                cancel_after_write();
            throw Poco::TimeoutException("injected renewal response loss after commit");
        }
        if (action == Action::ReturnThenCancel && cancel_after_write)
            cancel_after_write();
        return result;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        ++read_calls;
        if (key.ends_with("/mount"))
        {
            if (on_read)
                on_read();
            if (!read_actions.empty())
            {
                const Action action = read_actions.front();
                read_actions.pop_front();
                if (action == Action::ThrowFirstAttemptFuse)
                    throwFirstAttemptFuse();
                if (action == Action::ThrowBefore)
                    throw Poco::TimeoutException("injected renewal read failure");
            }
            else if (read_outage && read_outage())
            {
                throw Poco::TimeoutException("injected renewal read outage");
            }
        }
        std::optional<Raw> result = InMemoryBackend::read(key, access);
        if (pending && pending->key == key)
        {
            const Attempt delayed = *pending;
            pending.reset();
            const auto landed = InMemoryBackend::write(delayed.key, delayed.bytes, delayed.expected, access);
            if (!landed.has_value())
                throw DB::Exception(DB::ErrorCodes::ABORTED, "injected delayed renewal did not land");
        }
        return result;
    }

private:
    /// The adaptive first-attempt timeout the engine reissues at once.
    [[noreturn]] static void throwFirstAttemptFuse()
    {
#if USE_AWS_S3
        throw DB::S3Exception("Timeout", Aws::S3::S3Errors::NETWORK_CONNECTION);
#else
        throw Poco::TimeoutException("first-attempt fuse");
#endif
    }

    std::optional<Attempt> pending;
};
```

Append at the end of the file (after `RenewalStopsBeforeTheCutoffWhenEveryAttemptConsumesTheEnvelope`):

```cpp
namespace
{
/// The production shape at test scale: a 30 s lease, renewed one 10 s period after its anchor, with a
/// 2 s safety margin and the 7 s attempt envelope a bounded renewal reserves twice before each
/// request. That leaves a bounded renewal about 4 s of retries; an `UntilDefinitive` one has no bound.
class UnboundedRenewalFixture
{
public:
    static constexpr uint64_t ttl_ms = 30'000;
    static constexpr uint64_t period_ms = 10'000;
    static constexpr uint64_t margin_ms = 2'000;

    UnboundedRenewalFixture()
    {
        backend->setAttemptTimeoutMs(7'000);
        ops = std::make_unique<Ops>(backend, &boot_ms);
        seedOwnClaim(ops->op, layout, "test", UInt128{1}, 9, wall_ms, ttl_ms);
        renewer = std::make_unique<MountLeaseRenewer>(
            ops->mount, ops->farewell, ops->lease, layout, "test", UInt128{1}, 9, std::chrono::milliseconds(ttl_ms),
            [this] { return wall_ms; }, [] { return uint64_t{0}; },
            [this](CasEvent event) { events.push_back(std::move(event)); },
            std::chrono::milliseconds(margin_ms), [this] { return boot_ms; });
        anchor = renewer->start();
        backend->attempts.clear();
        backend->read_calls = 0;
        events.clear();
        boot_ms = anchor + period_ms;
        renewal_start = boot_ms;
    }

    UnboundedRenewalFixture(const UnboundedRenewalFixture &) = delete;
    UnboundedRenewalFixture & operator=(const UnboundedRenewalFixture &) = delete;

    MountRenewResult renew(
        MountRenewPolicy policy,
        const std::function<bool()> & live = {},
        const std::function<bool()> & cancelled = {},
        std::function<void(const MountRenewRequestEvent &)> on_request = {})
    {
        MountRenewOperationEnvironment environment = renewalEnvironment(boot_ms, live, cancelled);
        environment.policy = policy;
        environment.on_request = std::move(on_request);
        return renewer->renew(environment);
    }

    /// The `branch` of every `MountConflict` event, in order.
    std::vector<String> conflictBranches() const
    {
        std::vector<String> branches;
        for (const CasEvent & event : events)
            if (event.type == CasEventType::MountConflict)
                branches.push_back(event.detail.at("branch"));
        return branches;
    }

    String mountKey() const { return layout.mountKey("test"); }

    std::shared_ptr<RenewalScriptBackend> backend = std::make_shared<RenewalScriptBackend>();
    Layout layout{"pool"};
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    std::vector<CasEvent> events;
    std::unique_ptr<Ops> ops;
    std::unique_ptr<MountLeaseRenewer> renewer;
    uint64_t anchor = 0;
    uint64_t renewal_start = 0;
};

/// A request start (`P`) or a resolve read (`R`), with the boot clock when it was issued.
using RequestLog = std::vector<std::pair<char, uint64_t>>;

bool inSpacing(uint64_t gap_ms)
{
    return gap_ms >= kMountRenewRetrySpacingMs * 8 / 10 && gap_ms <= kMountRenewRetrySpacingMs * 12 / 10;
}
}

/// An outage longer than the cutoff a bounded renewal reserves for is retried to its end.
TEST(CASHeartbeat, RenewalOutlivesTheReservationCutoff)
{
    UnboundedRenewalFixture f;
    f.backend->outage = [&] { return f.boot_ms < f.renewal_start + 8'000; };

    const MountRenewResult result = f.renew(MountRenewPolicy::UntilDefinitive);

    ASSERT_EQ(result.outcome, MountRenewOutcome::Committed);
    EXPECT_EQ(f.renewer->state(), MountLeaseRenewerState::Active);
    /// One request per spacing interval of 800 to 1200 ms until the outage ends, then the one that lands.
    EXPECT_GE(f.backend->attempts.size(), 8u);
    EXPECT_LE(f.backend->attempts.size(), 11u);
    EXPECT_EQ(static_cast<size_t>(result.attempts_sent), f.backend->attempts.size());
}

TEST(CASHeartbeat, RenewalSendsOneTupleOnEveryAttempt)
{
    UnboundedRenewalFixture f;
    f.backend->outage = [&] { return f.boot_ms < f.renewal_start + 8'000; };

    const MountRenewResult result = f.renew(MountRenewPolicy::UntilDefinitive);

    ASSERT_EQ(result.outcome, MountRenewOutcome::Committed);
    const auto & attempts = f.backend->attempts;
    ASSERT_GT(attempts.size(), 1u);
    const UInt128 attempt_id = decodeMountLease(attempts.front().bytes).write_attempt_id;
    for (const auto & attempt : attempts)
    {
        EXPECT_EQ(attempt.key, attempts.front().key);
        EXPECT_EQ(attempt.bytes, attempts.front().bytes);
        EXPECT_EQ(attempt.expected, attempts.front().expected);
        EXPECT_EQ(decodeMountLease(attempt.bytes).write_attempt_id, attempt_id);
    }
    EXPECT_EQ(f.ops->op.read(f.mountKey(), Retry::standard())->bytes, attempts.front().bytes)
        << "the body that landed is the one every attempt carried";
}

TEST(CASHeartbeat, RenewalSucceedsPastTheDeadlineWithItsFirstStart)
{
    UnboundedRenewalFixture f;
    f.backend->outage = [&] { return f.boot_ms < f.anchor + UnboundedRenewalFixture::ttl_ms + 15'000; };

    const MountRenewResult result = f.renew(MountRenewPolicy::UntilDefinitive);

    ASSERT_EQ(result.outcome, MountRenewOutcome::Committed);
    EXPECT_EQ(result.attempt_start_boot_ms, f.renewal_start);
    EXPECT_EQ(f.renewer->lastCommittedAttemptStartBootMs(), f.renewal_start);
    EXPECT_LT(result.attempt_start_boot_ms + UnboundedRenewalFixture::ttl_ms, f.boot_ms)
        << "the lease this success confirms is already over";
}

TEST(CASHeartbeat, LandedAttemptIsAdoptedAfterALongOutage)
{
    UnboundedRenewalFixture f;
    f.backend->actions = {RenewalScriptBackend::Action::LandThenThrow};
    f.backend->read_outage = [&] { return f.boot_ms < f.anchor + UnboundedRenewalFixture::ttl_ms + 15'000; };
    std::vector<MountRenewRequestEvent> reported;

    const MountRenewResult result = f.renew(
        MountRenewPolicy::UntilDefinitive, {}, {}, [&](const MountRenewRequestEvent & event) { reported.push_back(event); });
    const uint64_t reads = f.backend->read_calls;

    ASSERT_EQ(result.outcome, MountRenewOutcome::Committed);
    EXPECT_TRUE(result.resolved_by_read);
    EXPECT_EQ(result.attempts_sent, 1u);
    EXPECT_GT(f.boot_ms, f.anchor + UnboundedRenewalFixture::ttl_ms) << "the adopting read came after the deadline";
    EXPECT_TRUE(f.conflictBranches().empty()) << "an adopted attempt is not a conflict such as same_epoch_state_uncertain";
    EXPECT_EQ(decodeMountLease(f.ops->op.read(f.mountKey(), Retry::standard())->bytes).write_attempt_id,
              decodeMountLease(f.backend->attempts.front().bytes).write_attempt_id);

    /// The one PUT sent, its failure, then one failed event per failed read; the last read succeeded.
    ASSERT_GE(reads, 2u);
    ASSERT_EQ(reported.size(), 2 + (reads - 1));
    EXPECT_FALSE(reported[0].failed);
    EXPECT_TRUE(reported[1].failed);
    EXPECT_NE(reported[1].failure_text.find("injected renewal response loss after commit"), String::npos)
        << reported[1].failure_text;
    for (size_t i = 2; i < reported.size(); ++i)
    {
        EXPECT_EQ(reported[i].request_no, 1u) << i;
        EXPECT_TRUE(reported[i].failed) << i << ": no request but the first PUT was sent";
        EXPECT_NE(reported[i].failure_text.find("injected renewal read outage"), String::npos)
            << i << ": " << reported[i].failure_text;
    }
}

/// Each definitive answer, met by a request sent past the deadline, ends the renewal with today's
/// classification.
TEST(CASHeartbeat, DefinitiveAnswersStayTerminalPastTheDeadline)
{
    enum class Answer : uint8_t { GcFenced, Foreign, NewerOwnEpoch, OwnEpochOtherBytes, Absent, StoreRefusal, LocalFailure };
    const auto run = [](Answer answer)
    {
        UnboundedRenewalFixture f;
        const String key = f.mountKey();
        const auto replace_slot = [&](const std::function<void(MountLease &)> & change)
        {
            const auto got = f.ops->op.read(key, Retry::standard());
            ASSERT_TRUE(got.has_value());
            MountLease lease = decodeMountLease(got->bytes);
            change(lease);
            ++lease.seq;
            mustCommit(f.ops->op.replace(key, encodeMountLease(lease), got->etag, Retry::standard()), "changed slot");
        };
        switch (answer)
        {
            case Answer::GcFenced:
                replace_slot([](MountLease & lease) { lease.gc_fenced = true; });
                break;
            case Answer::Foreign:
                replace_slot([](MountLease & lease) { lease.server_uuid = UInt128{2}; });
                break;
            case Answer::NewerOwnEpoch:
                replace_slot([](MountLease & lease) { lease.writer_epoch = 10; });
                break;
            case Answer::OwnEpochOtherBytes:
                replace_slot([](MountLease & lease) { lease.write_attempt_id = UInt128{0xAAAA}; });
                break;
            case Answer::Absent:
            {
                const auto got = f.ops->op.read(key, Retry::standard());
                ASSERT_TRUE(got.has_value());
                ASSERT_EQ(f.ops->op.remove(key, got->etag, Retry::standard()), Removal::Removed);
                break;
            }
            case Answer::StoreRefusal:
#if USE_AWS_S3
                f.backend->failNextWriteWith(key, std::make_exception_ptr(DB::S3Exception(
                    "the store answered MalformedXML", Aws::S3::S3Errors::UNKNOWN, "MalformedXML")));
#endif
                break;
            case Answer::LocalFailure:
                f.backend->failNextWriteWith(key, std::make_exception_ptr(DB::Exception(
                    DB::ErrorCodes::CORRUPTED_DATA, "injected deterministic local failure")));
                break;
        }
        f.backend->attempts.clear();
        f.events.clear();
        /// Past the lease: a bounded renewal would send nothing here.
        f.boot_ms = f.anchor + UnboundedRenewalFixture::ttl_ms + 1'000;

        const MountRenewResult result = f.renew(MountRenewPolicy::UntilDefinitive);

        const DB::Exception failure = terminalException(result);
        EXPECT_EQ(f.renewer->state(), MountLeaseRenewerState::RenewalTerminal);
        EXPECT_EQ(f.backend->attempts.size(), 1u) << "the answer came to a request sent past the deadline";
        const std::vector<String> branches = f.conflictBranches();
        switch (answer)
        {
            case Answer::GcFenced:
                EXPECT_EQ(branches, std::vector<String>{"fenced_by_gc"});
                break;
            case Answer::Foreign:
                EXPECT_EQ(branches, std::vector<String>{"foreign_writer"});
                break;
            case Answer::NewerOwnEpoch:
                EXPECT_EQ(branches, std::vector<String>{"superseded"});
                break;
            case Answer::OwnEpochOtherBytes:
                EXPECT_EQ(branches, std::vector<String>{"same_epoch_state_uncertain"});
                break;
            case Answer::Absent:
                EXPECT_EQ(branches, std::vector<String>{"vanished"});
                EXPECT_EQ(failure.code(), DB::ErrorCodes::FILE_DOESNT_EXIST) << failure.message();
                break;
            case Answer::StoreRefusal:
                EXPECT_TRUE(branches.empty());
                EXPECT_NE(failure.message().find("the store refused the renewal"), String::npos) << failure.message();
                break;
            case Answer::LocalFailure:
                EXPECT_TRUE(branches.empty());
                EXPECT_EQ(failure.code(), DB::ErrorCodes::CORRUPTED_DATA) << failure.message();
                EXPECT_NE(failure.message().find("injected deterministic local failure"), String::npos);
                break;
        }
    };
    for (Answer answer : {Answer::GcFenced, Answer::Foreign, Answer::NewerOwnEpoch, Answer::OwnEpochOtherBytes,
                          Answer::Absent, Answer::StoreRefusal, Answer::LocalFailure})
    {
#if !USE_AWS_S3
        if (answer == Answer::StoreRefusal)
            continue;
#endif
        SCOPED_TRACE(static_cast<int>(answer));
        run(answer);
    }
}

/// The cut-off node's story: renewals fail past the deadline, GC on a healthy node fences the slot,
/// and the first request that reaches the store reads the fence.
TEST(CASHeartbeat, AFenceSeenAfterALongOutageEndsTheRenewal)
{
    UnboundedRenewalFixture f;
    const uint64_t fenced_at = f.anchor + UnboundedRenewalFixture::ttl_ms + 15'000;
    bool fenced = false;
    f.backend->outage = [&] { return !fenced; };
    f.ops->lease.setSleepFnForTest([&](uint64_t ms)
    {
        f.boot_ms += ms;
        if (fenced || f.boot_ms < fenced_at)
            return;
        fenced = true;
        const auto got = f.ops->op.read(f.mountKey(), Retry::standard());
        ASSERT_TRUE(got.has_value());
        MountLease lease = decodeMountLease(got->bytes);
        lease.gc_fenced = true;
        ++lease.seq;
        mustCommit(f.ops->op.replace(f.mountKey(), encodeMountLease(lease), got->etag, Retry::standard()), "fence-out");
    });

    const MountRenewResult result = f.renew(MountRenewPolicy::UntilDefinitive);

    const DB::Exception failure = terminalException(result);
    EXPECT_NE(failure.message().find("fenced by GC"), String::npos) << failure.message();
    EXPECT_EQ(f.conflictBranches(), std::vector<String>{"fenced_by_gc"});
    EXPECT_GE(f.boot_ms, fenced_at);
}

#if USE_AWS_S3
/// After an unclear attempt the engine cannot tell whether that attempt will still land, so a refusal
/// is not an answer about the slot: the renewal keeps retrying at the spacing until a stop ends it.
TEST(CASHeartbeat, ARefusalAfterAnUnclearAttemptIsRetriedUntilStopped)
{
    UnboundedRenewalFixture f;
    f.backend->actions = {RenewalScriptBackend::Action::ThrowBefore};
    f.backend->outage = [] { return true; };
    f.backend->outage_action = RenewalScriptBackend::Action::ThrowStoreRefusal;
    std::vector<uint64_t> sent_at;
    f.backend->on_attempt = [&] { sent_at.push_back(f.boot_ms); };
    bool stopped = false;
    f.ops->lease.setSleepFnForTest([&](uint64_t ms)
    {
        f.boot_ms += ms;
        if (f.boot_ms >= f.renewal_start + 30'000)
            stopped = true;
    });
    std::vector<MountRenewRequestEvent> reported;

    const MountRenewResult result = f.renew(
        MountRenewPolicy::UntilDefinitive, /*live=*/[&] { return !stopped; }, /*cancelled=*/[&] { return stopped; },
        [&](const MountRenewRequestEvent & event) { reported.push_back(event); });

    const DB::Exception failure = terminalException(result);
    EXPECT_EQ(failure.code(), DB::ErrorCodes::NETWORK_ERROR) << failure.message();
    EXPECT_EQ(f.renewer->state(), MountLeaseRenewerState::RenewalTerminal);
    EXPECT_TRUE(f.conflictBranches().empty()) << "the read kept showing our own unchanged body";

    /// One PUT per spacing interval over the 30 s the store kept refusing.
    ASSERT_GE(sent_at.size(), 2u);
    for (size_t i = 1; i < sent_at.size(); ++i)
        EXPECT_TRUE(inSpacing(sent_at[i] - sent_at[i - 1])) << i << ": " << sent_at[i] - sent_at[i - 1];
    EXPECT_GE(sent_at.size(), 30'000 / (kMountRenewRetrySpacingMs * 12 / 10));
    EXPECT_LE(sent_at.size(), 30'000 / (kMountRenewRetrySpacingMs * 8 / 10) + 1);

    /// Every refusal was reported as a failed request.
    size_t refusals = 0;
    for (const MountRenewRequestEvent & event : reported)
        if (event.failed && event.failure_text.find("MalformedXML") != String::npos)
            ++refusals;
    EXPECT_EQ(refusals, sent_at.size() - 1) << "every PUT after the unclear first one was refused";
}
#endif

TEST(CASHeartbeat, RenewalSpacesRetries)
{
#if USE_AWS_S3
    {
        SCOPED_TRACE("fast connect failures");
        UnboundedRenewalFixture f;
        RequestLog log;
        f.backend->on_attempt = [&] { log.emplace_back('P', f.boot_ms); };
        f.backend->on_read = [&] { log.emplace_back('R', f.boot_ms); };
        f.backend->actions = {RenewalScriptBackend::Action::ThrowFirstAttemptFuse};
        f.backend->outage = [&] { return f.boot_ms < f.renewal_start + 30'500; };
        f.backend->outage_action = RenewalScriptBackend::Action::ThrowConnectHint;

        ASSERT_EQ(f.renew(MountRenewPolicy::UntilDefinitive).outcome, MountRenewOutcome::Committed);

        ASSERT_GE(log.size(), 4u);
        /// The fuse: its settling read and its reissue follow at once.
        EXPECT_EQ(log[0], std::make_pair('P', f.renewal_start));
        EXPECT_EQ(log[1], std::make_pair('R', f.renewal_start));
        EXPECT_EQ(log[2], std::make_pair('P', f.renewal_start));
        /// A connect failure is reissued without a read, one spacing interval after it started.
        for (size_t i = 3; i < log.size(); ++i)
        {
            EXPECT_EQ(log[i].first, 'P') << i;
            EXPECT_TRUE(inSpacing(log[i].second - log[i - 1].second)) << i << ": " << log[i].second - log[i - 1].second;
        }
        EXPECT_GE(log.back().second, f.renewal_start + 30'000) << "still renewing after 30 s";
    }
#endif
    {
        SCOPED_TRACE("unclear PUT with a failing read");
        UnboundedRenewalFixture f;
        RequestLog log;
        f.backend->on_attempt = [&] { log.emplace_back('P', f.boot_ms); };
        f.backend->on_read = [&] { log.emplace_back('R', f.boot_ms); };
        f.backend->read_actions = {RenewalScriptBackend::Action::ThrowBefore, RenewalScriptBackend::Action::ThrowBefore};
        f.backend->outage = [&] { return f.boot_ms < f.renewal_start + 30'500; };

        ASSERT_EQ(f.renew(MountRenewPolicy::UntilDefinitive).outcome, MountRenewOutcome::Committed);

        ASSERT_GE(log.size(), 6u);
        const uint64_t t0 = f.renewal_start;
        EXPECT_EQ(log[0], std::make_pair('P', t0));
        EXPECT_EQ(log[1], std::make_pair('R', t0)) << "the read that settles an unclear PUT is sent at once";
        EXPECT_EQ(log[2].first, 'R');
        EXPECT_TRUE(inSpacing(log[2].second - log[1].second)) << log[2].second - log[1].second;
        EXPECT_EQ(log[3].first, 'R');
        EXPECT_TRUE(inSpacing(log[3].second - log[2].second)) << log[3].second - log[2].second;
        EXPECT_EQ(log[4], std::make_pair('P', log[3].second))
            << "more than one interval has passed since the PUT started, so it is retried at once";
        uint64_t previous_put = log[4].second;
        for (size_t i = 5; i < log.size(); ++i)
        {
            if (log[i].first == 'R')
            {
                EXPECT_EQ(log[i].second, log[i - 1].second) << i;
                continue;
            }
            EXPECT_TRUE(inSpacing(log[i].second - previous_put)) << i << ": " << log[i].second - previous_put;
            previous_put = log[i].second;
        }
        EXPECT_GE(previous_put, t0 + 30'000) << "still renewing after 30 s";
    }
#if USE_AWS_S3
    {
        SCOPED_TRACE("first-attempt fuse of the read");
        UnboundedRenewalFixture f;
        RequestLog log;
        f.backend->on_attempt = [&] { log.emplace_back('P', f.boot_ms); };
        f.backend->on_read = [&] { log.emplace_back('R', f.boot_ms); };
        f.backend->actions = {RenewalScriptBackend::Action::ThrowBefore};
        f.backend->read_actions = {RenewalScriptBackend::Action::ThrowFirstAttemptFuse};

        ASSERT_EQ(f.renew(MountRenewPolicy::UntilDefinitive).outcome, MountRenewOutcome::Committed);

        ASSERT_EQ(log.size(), 4u);
        EXPECT_EQ(log[1], std::make_pair('R', f.renewal_start));
        EXPECT_EQ(log[2], std::make_pair('R', f.renewal_start)) << "a fused read is reissued at once";
        EXPECT_EQ(log[3].first, 'P');
        EXPECT_TRUE(inSpacing(log[3].second - log[0].second)) << log[3].second - log[0].second;
    }
#endif
    {
        SCOPED_TRACE("slow requests");
        UnboundedRenewalFixture f;
        RequestLog log;
        bool failing = false;
        f.backend->on_attempt = [&]
        {
            log.emplace_back('P', f.boot_ms);
            failing = f.boot_ms < f.renewal_start + 30'500;
            if (failing)
                f.boot_ms += 5'000;
        };
        f.backend->on_read = [&] { log.emplace_back('R', f.boot_ms); };
        f.backend->outage = [&] { return failing; };

        ASSERT_EQ(f.renew(MountRenewPolicy::UntilDefinitive).outcome, MountRenewOutcome::Committed);

        ASSERT_GE(log.size(), 3u);
        std::optional<uint64_t> previous_put;
        for (size_t i = 0; i < log.size(); ++i)
        {
            if (log[i].first == 'R')
            {
                EXPECT_EQ(log[i].second, log[i - 1].second + 5'000) << i;
                continue;
            }
            if (previous_put)
                EXPECT_EQ(log[i].second, *previous_put + 5'000) << i << ": a 5 s request is retried with no wait";
            previous_put = log[i].second;
        }
        EXPECT_GE(*previous_put, f.renewal_start + 30'000) << "still renewing after 30 s";
    }
}

TEST(CASHeartbeat, StopDuringARetryWaitEndsTheRenewal)
{
    {
        SCOPED_TRACE("a stop during the wait");
        UnboundedRenewalFixture f;
        bool stopped = false;
        std::vector<uint64_t> waits;
        f.backend->outage = [] { return true; };
        /// The stop wakes the wait, so the clock does not move.
        f.ops->lease.setSleepFnForTest([&](uint64_t ms)
        {
            waits.push_back(ms);
            stopped = true;
        });

        const MountRenewResult result = f.renew(
            MountRenewPolicy::UntilDefinitive, /*live=*/[&] { return !stopped; }, /*cancelled=*/[&] { return stopped; });

        const DB::Exception failure = terminalException(result);
        EXPECT_EQ(failure.code(), DB::ErrorCodes::NETWORK_ERROR) << failure.message();
        ASSERT_EQ(waits.size(), 1u);
        EXPECT_TRUE(inSpacing(waits[0])) << waits[0];
        EXPECT_EQ(f.backend->attempts.size(), 1u) << "nothing is sent after the stop";
        EXPECT_EQ(f.renewer->state(), MountLeaseRenewerState::RenewalTerminal);
        EXPECT_FALSE(f.renewer->canRelease());
    }
    {
        SCOPED_TRACE("a stop after the renewal began, before its first send");
        UnboundedRenewalFixture f;

        const MountRenewResult result = f.renew(
            MountRenewPolicy::UntilDefinitive, /*live=*/[] { return false; }, /*cancelled=*/[] { return false; });

        (void)terminalException(result);
        EXPECT_TRUE(f.backend->attempts.empty());
        EXPECT_FALSE(f.renewer->canRelease());
    }
}

TEST(CASHeartbeat, RenewalReportsEachRequestAsItHappens)
{
    UnboundedRenewalFixture f;
    f.backend->actions = {RenewalScriptBackend::Action::ThrowBefore, RenewalScriptBackend::Action::ThrowBefore};
    std::vector<MountRenewRequestEvent> reported;

    const MountRenewResult result = f.renew(
        MountRenewPolicy::UntilDefinitive, {}, {}, [&](const MountRenewRequestEvent & event) { reported.push_back(event); });

    ASSERT_EQ(result.outcome, MountRenewOutcome::Committed);
    const std::vector<std::pair<uint32_t, bool>> expected{{1, false}, {1, true}, {2, false}, {2, true}, {3, false}};
    ASSERT_EQ(reported.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_EQ(reported[i].request_no, expected[i].first) << i;
        EXPECT_EQ(reported[i].failed, expected[i].second) << i;
        if (reported[i].failed)
            EXPECT_NE(reported[i].failure_text.find("injected renewal response uncertainty"), String::npos)
                << reported[i].failure_text;
        else
            EXPECT_TRUE(reported[i].failure_text.empty()) << i;
    }
}

TEST(CASHeartbeat, AThrowingRequestReportChangesNoOutcome)
{
    UnboundedRenewalFixture f;
    f.backend->actions = {RenewalScriptBackend::Action::ThrowBefore, RenewalScriptBackend::Action::ThrowBefore};

    const MountRenewResult result = f.renew(
        MountRenewPolicy::UntilDefinitive, {}, {},
        [](const MountRenewRequestEvent &) { throw std::runtime_error("injected report failure"); });

    ASSERT_EQ(result.outcome, MountRenewOutcome::Committed);
    EXPECT_EQ(result.attempts_sent, 3u);
    EXPECT_EQ(f.renewer->state(), MountLeaseRenewerState::Active);
}

/// The startup, remount and direct renewals keep their lease bound.
TEST(CASHeartbeat, BoundedPathsStillStopAtTheLeaseDeadline)
{
    for (const bool remount : {false, true})
    {
        SCOPED_TRACE(remount ? "renewForRemount" : "renew");
        UnboundedRenewalFixture f;
        std::vector<uint64_t> sent_at;
        f.backend->on_attempt = [&] { sent_at.push_back(f.boot_ms); };
        /// Longer than any lease, so only the bound can end the renewal.
        f.backend->outage = [&] { return f.boot_ms < f.renewal_start + 60'000; };
        MountRenewOperationEnvironment environment = renewalEnvironment(f.boot_ms);
        environment.policy = MountRenewPolicy::LeaseBound;

        const MountRenewResult result = remount ? f.renewer->renewForRemount(environment) : f.renewer->renew(environment);

        const DB::Exception failure = terminalException(result);
        EXPECT_NE(failure.message().find("external_lease_deadline"), String::npos) << failure.message();
        ASSERT_TRUE(result.deadline_source.has_value());
        EXPECT_EQ(*result.deadline_source, GaveUp::Source::Lease);
        ASSERT_FALSE(sent_at.empty());
        const uint64_t lease_safe = f.anchor + UnboundedRenewalFixture::ttl_ms - UnboundedRenewalFixture::margin_ms;
        for (uint64_t at : sent_at)
            EXPECT_LT(at, lease_safe) << "no request starts past the lease-safe bound";
    }
}
```

- [ ] **Step 8: Build and run them to see them fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t4_s8_build.log 2>&1; echo NINJA_EXIT=$? >> build/t4_s8_build.log
tail -1 build/t4_s8_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASHeartbeat.*' > build/t4_s8_tests.log 2>&1; echo GATE_EXIT=$? >> build/t4_s8_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t4_s8_tests.log | tail -30
```

Expected `NINJA_EXIT=0` (the new names exist since Step 6). `renew` already routes `UntilDefinitive`
to `ops.lease`, but `renewOn` still applies `Retry::untilLeaseSafe`, so:

| Test | Red because |
|---|---|
| `RenewalOutlivesTheReservationCutoff`, `RenewalSendsOneTupleOnEveryAttempt`, `RenewalSucceedsPastTheDeadlineWithItsFirstStart` | `ASSERT_EQ(result.outcome, MountRenewOutcome::Committed)` fails: the renewal ends terminal (`external_lease_deadline`) after about 4 s of retries |
| `LandedAttemptIsAdoptedAfterALongOutage` | same assertion: the resolve read gives up at the lease bound |
| `DefinitiveAnswersStayTerminalPastTheDeadline` | every case: `attempts.size()` is 0 (pre-send `external_lease_deadline`) and the branch / message assertions fail |
| `AFenceSeenAfterALongOutageEndsTheRenewal` | the message is `external_lease_deadline`, not `fenced by GC`; branches empty |
| `ARefusalAfterAnUnclearAttemptIsRetriedUntilStopped` (S3 builds) | the renewal ends at the lease bound after about 4 s: the PUT gaps are `Retry::backoff`, not spacing, and the count is far below 25 |
| `LandedAttemptIsAdoptedAfterALongOutage` (its report half) | not reached in this run, because the `Committed` assertion stops it first; `renewOn` installs the observer only in Step 9 |
| `RenewalSpacesRetries` | `ASSERT_EQ(..., Committed)` fails in every case: terminal before 30 s |
| `StopDuringARetryWaitEndsTheRenewal` | first case: `inSpacing(waits[0])` fails; the wait is `Retry::backoff(1)`, 0 to 200 ms |
| `RenewalReportsEachRequestAsItHappens` | `ASSERT_EQ(reported.size(), expected.size())`: 0 vs 5 |
| `AThrowingRequestReportChangesNoOutcome`, `BoundedPathsStillStopAtTheLeaseDeadline` | PASS: these guard behaviour that must not change; their red is shown by sabotage in Step 10 |

Every existing `CASHeartbeat` test passes.

- [ ] **Step 9: Implement the policy and the report in `renewOn`**

`$W/CA/Pool/CasServerRoot.cpp`. Add `#include <Poco/Exception.h>` after `#include <Common/setThreadName.h>` (~:10).
Immediately before `MountLeaseRenewer::MountLeaseRenewer(` (~:1308), after `constexpr uint64_t kFarewellSlackMs = 2'000;`:

```cpp
namespace
{
/// The text of a failed request as an operator should read it: a `DB::Exception`'s message, a Poco
/// exception's display text (its `what` is only the class name), any other exception's `what`.
String describeRequestFailure(const std::exception & failure)
{
    if (const auto * db_failure = dynamic_cast<const Exception *>(&failure))
        return db_failure->message();
    if (const auto * poco_failure = dynamic_cast<const Poco::Exception *>(&failure))
        return poco_failure->displayText();
    return failure.what();
}
}
```

`MountLeaseRenewer::renewOn`, replace (~:1611-1617):

```cpp
    CasOperation op = plane.admit(environment.live);
    std::optional<WriteResult> written;
    try
    {
        written = op.replace(key, body, precondition(),
            Retry::untilLeaseSafe(confirmed_deadline_boot_ms, static_cast<uint64_t>(lease_safety_margin.count())));
    }
```

with:

```cpp
    CasOperation op = plane.admit(environment.live);
    if (environment.on_request)
        op.setRequestObserver([&on_request = environment.on_request](uint32_t attempt_no, const std::exception * failure)
        {
            on_request(MountRenewRequestEvent{
                .request_no = attempt_no,
                .failed = failure != nullptr,
                .failure_text = failure ? describeRequestFailure(*failure) : String{},
            });
        });
    const Retry policy = environment.policy == MountRenewPolicy::UntilDefinitive
        ? Retry::untilDefinitive(kMountRenewRetrySpacingMs)
        : Retry::untilLeaseSafe(confirmed_deadline_boot_ms, static_cast<uint64_t>(lease_safety_margin.count()));
    std::optional<WriteResult> written;
    try
    {
        written = op.replace(key, body, precondition(), policy);
    }
```

The rest of `renewOn` is unchanged: `GaveUp{FenceLost}` with nothing sent and `cancelled` sampled true
at entry stays `NotAttempted`; every other ending, including `GaveUp{Deadline}` and
`GaveUp{Unresolved}`, stays terminal with its current classification.

`MountLeaseRenewer::terminate` (~:1750-1752), the sentence that cites `renew`:

```cpp
    /// The derived window alone is not enough: mount-control activity must also never run past the
    /// point this node's own fence may already be gone (the rule a bounded renewal enforces with
    /// `Retry::untilLeaseSafe`). The precondition on this write already stops it from clobbering
```

`$W/CA/Pool/CasServerRoot.h`, the PLANES paragraph of the `MountLeaseRenewer` class comment (~:525-530), whole paragraph:

```cpp
/// PLANES. A bounded renewal is admitted under the mount fence, because it writes under the authority
/// the fence tracks. The worker's renewal (`UntilDefinitive`) runs on a plane with no lease budget: it
/// keeps trying after the lease expired, and a stop, a park or a terminal lifecycle reaches it through
/// its liveness. The claim and the farewell are admitted off the fence: a self-remount claims with the
/// fence already latched lost, so a claim gated on the fence could never reclaim, and a farewell
/// refused because the fence has run down would leave the slot looking live until GC fences it out.
/// Neither is unguarded: a claim's safety is its own conditional write, and a caller that has shutdown
/// facts hands them over as a `Liveness`.
```

Same class, `renew` doc (~:548):

```cpp
    /// The steady-state renewal. `LeaseBound` runs under the mount fence with `Retry::untilLeaseSafe`;
    /// `UntilDefinitive` runs on the worker plane with `Retry::untilDefinitive(kMountRenewRetrySpacingMs)`
    /// and ends only on a definitive answer or when `environment.live` refuses.
    MountRenewResult renew(const MountRenewOperationEnvironment & environment);
```

- [ ] **Step 10: Build, run the tests of this step, prove the guards can fail, then the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t4_s10_build.log 2>&1; echo NINJA_EXIT=$? >> build/t4_s10_build.log
tail -1 build/t4_s10_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASHeartbeat.*:CASRequests.*' > build/t4_s10_tests.log 2>&1; echo GATE_EXIT=$? >> build/t4_s10_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t4_s10_tests.log | tail -20
```

Expected: `NINJA_EXIT=0`, `GATE_EXIT=0`, no `FAILED`.

Sabotage, one at a time, rebuild, run the named test, see it fail, revert, rebuild:
- In `renewOn` write `true ? Retry::untilDefinitive(...) : ...` in place of the policy condition: `CASHeartbeat.BoundedPathsStillStopAtTheLeaseDeadline` fails (`terminalException` reports `Committed`).
- In `CasOperation::notifyRequest` call the observer without the `try`: `CASHeartbeat.AThrowingRequestReportChangesNoOutcome` fails (outcome `Terminal`, `deterministic_failure`).

Then the gate, logs `t4_s10_gate.log`, as in Step 4: `GATE_EXIT=0`, no `FAILED`.

- [ ] **Step 11: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp \
  src/Disks/tests/gtest_cas_heartbeat.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Renew the CAS mount lease until a definitive answer under UntilDefinitive" \
  -m "MountLeaseRenewer::renew with MountRenewPolicy::UntilDefinitive retries under Retry::untilDefinitive(kMountRenewRetrySpacingMs) on the worker plane, with no lease bound, and reports each PUT and each failure through MountRenewOperationEnvironment::on_request. Classification is unchanged. LeaseBound keeps Retry::untilLeaseSafe." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

- [ ] **Step 12: Write the failing runtime tests**

In `$W/src/Disks/tests/gtest_cas_pool.cpp`, extend `RuntimeRenewBackend` (~:1997-2046). Add the
members after `std::function<void()> before_throw;`:

```cpp
    /// While it answers TRUE, every conditional write throws a transport timeout, before `fault` is
    /// consulted. Read on the renewing thread; set it before the workers start.
    std::function<bool()> outage;
    /// Runs on every write `outage` fails, before it throws.
    std::function<void()> on_outage_write;
    std::atomic<uint64_t> outage_writes{0};
```

and in `write`, right after `if (!expected_value)\n            return DB::Cas::tests::CountingBackend::write(...);`:

```cpp
        if (outage && outage())
        {
            outage_writes.fetch_add(1, std::memory_order_relaxed);
            if (on_outage_write)
                on_outage_write();
            throw Poco::TimeoutException("injected runtime renewal outage");
        }
```

Append after `TEST(CASPool, DeterministicWorkerFailureFencesWithoutWaitingForCadence)` (~:4468):

```cpp
/// A remount request ends a worker renewal that is retrying past its lease, inside the wait or the
/// request it is in: no request and no wait starts after it.
TEST(CASMountRuntime, ParkEndsAnUnboundedRenewal)
{
    enum class ParkDuring : uint8_t { Wait, Request };
    const auto run = [](ParkDuring park_during)
    {
        auto backend = std::make_shared<RuntimeRenewBackend>();
        const Layout layout(park_during == ParkDuring::Wait ? "unbounded-park-in-wait" : "unbounded-park-in-request");
        const UInt128 uuid{1};
        uint64_t wall_ms = 1000;
        std::atomic<uint64_t> boot_ms{100};
        std::atomic<uint64_t> past_the_lease{std::numeric_limits<uint64_t>::max()};
        std::atomic<bool> held{false};
        std::atomic<uint64_t> waits{0};
        std::atomic<uint64_t> writes_at_remount{0};
        std::atomic<uint64_t> waits_at_remount{0};
        DB::Cas::tests::ManualBarrier holding;
        DB::Cas::tests::ManualBarrier remount_entered;
        const auto hold_once_past_the_lease = [&]
        {
            if (boot_ms.load() >= past_the_lease.load() && !held.exchange(true))
                holding.arriveAndWait();
        };
        ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
                  MountClaimResult::Claimed);
        CasEventSink sink;
        RuntimeUnderTest runtime_holder(
            backend, layout,
            MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                        .boot_ms_fn = [&] { return boot_ms.load(); }},
            "test", sink, runtimeRenewBudget(), [&]
            {
                writes_at_remount = backend->outage_writes.load();
                waits_at_remount = waits.load();
                remount_entered.arriveAndWait();
                return false;
            });
        CasMountRuntime & runtime = *runtime_holder;
        runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
        const uint64_t anchor = runtime.startRenewer();
        runtime.armMountFence(uuid, 1, anchor + 1000);
        /// Five lease lengths on: a renewal bounded by its lease ended long before.
        past_the_lease = anchor + 5'000;
        runtime_holder.setRetrySleepForTest([&](uint64_t ms)
        {
            ++waits;
            boot_ms += ms;
            if (park_during == ParkDuring::Wait)
                hold_once_past_the_lease();
        });
        if (park_during == ParkDuring::Request)
            backend->on_outage_write = hold_once_past_the_lease;
        backend->outage = [] { return true; };
        runtime.startBackgroundWorkers(std::chrono::milliseconds(0));

        holding.waitUntilArrived();
        const uint64_t writes_at_park = backend->outage_writes.load();
        const uint64_t waits_at_park = waits.load();
        runtime.scheduleRemount();
        EXPECT_EQ(runtime.renewalDriverStateForTest(), RenewalDriverState::ParkRequested);
        holding.release();
        remount_entered.waitUntilArrived();
        EXPECT_EQ(runtime.renewalDriverStateForTest(), RenewalDriverState::Parked);
        EXPECT_EQ(writes_at_remount.load(), writes_at_park) << "no request starts after the park";
        EXPECT_EQ(waits_at_remount.load(), waits_at_park) << "no wait starts after the park";
        remount_entered.release();
        runtime.stopBackgroundWorkers();
        runtime.finishTeardown(false);
    };
    run(ParkDuring::Wait);
    run(ParkDuring::Request);
}

/// FORGET while the worker's renewal retries past its lease: the intent then the trip, in the order
/// `Pool::forgetDisk` uses, end the renewal inside the wait it is in, both workers exit, and no remount
/// generation is raised.
TEST(CASMountRuntime, ForgetEndsAnUnboundedRenewal)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("unbounded-forget");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{100};
    std::atomic<uint64_t> past_the_lease{std::numeric_limits<uint64_t>::max()};
    std::atomic<bool> held{false};
    std::atomic<uint64_t> waits{0};
    DB::Cas::tests::ManualBarrier holding;
    WorkerExitLatch exits;
    RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
    {
        return ThreadFromGlobalPool([&, body = std::move(worker_body)]
        {
            body();
            exits.recordExit();
        });
    };
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms.load(); }, .worker_factory = factory},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    past_the_lease = anchor + 5'000;
    runtime_holder.setRetrySleepForTest([&](uint64_t ms)
    {
        ++waits;
        boot_ms += ms;
        if (boot_ms.load() >= past_the_lease.load() && !held.exchange(true))
            holding.arriveAndWait();
    });
    backend->outage = [] { return true; };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));

    holding.waitUntilArrived();
    const uint64_t writes_at_forget = backend->outage_writes.load();
    const uint64_t waits_at_forget = waits.load();
    const uint64_t generation_at_forget = runtime.remountRequestedGenerationForTest();
    runtime.publishVanishedIntent();
    runtime.tripMountLost();
    holding.release();

    const bool both_exited = exits.waitForAtLeast(2);
    EXPECT_TRUE(both_exited) << "the worker loops must exit on the published intent";
    EXPECT_EQ(backend->outage_writes.load(), writes_at_forget) << "no request starts after FORGET";
    EXPECT_EQ(waits.load(), waits_at_forget) << "no wait starts after FORGET";
    EXPECT_EQ(runtime.remountRequestedGenerationForTest(), generation_at_forget);
    EXPECT_FALSE(runtime.mayMutate());
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

TEST(CASMountRuntime, StopWakesTheRetryWaitOfAnUnboundedRenewal)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("unbounded-stop-in-wait");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    const uint64_t boot_ms = 100;
    std::promise<uint64_t> wait_entered;
    std::future<uint64_t> wait_requested = wait_entered.get_future();
    std::atomic<bool> first_wait{true};
    std::atomic<int64_t> first_wait_slept_ms{-1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    runtime_holder.setRetrySleepForTest([&](uint64_t ms)
    {
        const bool first = first_wait.exchange(false);
        if (first)
            wait_entered.set_value(ms);
        const auto started = std::chrono::steady_clock::now();
        runtime.sleepInterruptibly(ms);
        if (first)
            first_wait_slept_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
    });
    backend->outage = [] { return true; };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));

    ASSERT_EQ(wait_requested.wait_for(std::chrono::seconds(20)), std::future_status::ready);
    const uint64_t requested_ms = wait_requested.get();
    runtime.stopBackgroundWorkers();

    EXPECT_GE(requested_ms, kMountRenewRetrySpacingMs * 8 / 10);
    EXPECT_LE(requested_ms, kMountRenewRetrySpacingMs * 12 / 10);
    ASSERT_GE(first_wait_slept_ms.load(), 0);
    EXPECT_LT(static_cast<uint64_t>(first_wait_slept_ms.load()), requested_ms)
        << "the stop woke the wait instead of letting it run out";
    EXPECT_EQ(backend->outage_writes.load(), 1u) << "nothing is sent after the stop";
    runtime.finishTeardown(false);
}

/// A success whose lease is already over is followed by the next renewal with no cadence wait.
TEST(CASMountRuntime, AStaleSuccessIsFollowedAtOnceByTheNextRenewal)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("unbounded-stale-success");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{100};
    std::atomic<uint64_t> outage_until{0};
    std::vector<uint64_t> commit_boot_ms;
    std::vector<bool> may_mutate_at_commit;
    DB::Cas::tests::ManualBarrier second_commit;
    CasMountRuntime * runtime_ptr = nullptr;
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms.load(); }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    runtime_holder.setRetrySleepForTest([&](uint64_t ms) { boot_ms += ms; });
    /// The first renewal starts one period after the anchor and fails for three lease lengths.
    boot_ms = anchor + 500;
    outage_until = anchor + 3'500;
    backend->outage = [&] { return boot_ms.load() < outage_until.load(); };
    backend->after_commit = [&]
    {
        commit_boot_ms.push_back(boot_ms.load());
        may_mutate_at_commit.push_back(runtime_ptr->mayMutate());
        if (commit_boot_ms.size() == 2)
            second_commit.arriveAndWait();
    };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(500));

    second_commit.waitUntilArrived();
    ASSERT_EQ(commit_boot_ms.size(), 2u);
    EXPECT_GE(commit_boot_ms[0], anchor + 3'500) << "the first renewal outlived its own lease";
    EXPECT_EQ(commit_boot_ms[1], commit_boot_ms[0])
        << "no time passed: a cadence wait on this frozen clock would never have ended";
    EXPECT_FALSE(may_mutate_at_commit[1]) << "the stale success left the lease expired";
    second_commit.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}
```

- [ ] **Step 13: Build and run them to see them fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t4_s13_build.log 2>&1; echo NINJA_EXIT=$? >> build/t4_s13_build.log
tail -1 build/t4_s13_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASMountRuntime.ParkEndsAnUnboundedRenewal:CASMountRuntime.ForgetEndsAnUnboundedRenewal:CASMountRuntime.StopWakesTheRetryWaitOfAnUnboundedRenewal:CASMountRuntime.AStaleSuccessIsFollowedAtOnceByTheNextRenewal' > build/t4_s13_tests.log 2>&1; echo GATE_EXIT=$? >> build/t4_s13_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT\|timed out" build/t4_s13_tests.log | tail -20
```

Expected: `NINJA_EXIT=0`; `renewalEnvironment` does not set the policy yet, so the worker renews
`LeaseBound` on the mount plane, whose wait these tests also drive:
- `ParkEndsAnUnboundedRenewal`: `CAS test barrier timed out waiting for arrival` (both cases): the renewal ends terminal at its lease deadline, about 1 s on the injected clock, so it never reaches the hold placed five lease lengths on.
- `ForgetEndsAnUnboundedRenewal`: `CAS test barrier timed out waiting for arrival`, for the same reason.
- `StopWakesTheRetryWaitOfAnUnboundedRenewal`: `EXPECT_GE(requested_ms, 800)` fails: the wait is `Retry::backoff(1)`, 0 to 200 ms.
- `AStaleSuccessIsFollowedAtOnceByTheNextRenewal`: `CAS test barrier timed out waiting for arrival`: the first renewal ends terminal at its deadline, so nothing commits.

Each barrier timeout costs 20 s.

- [ ] **Step 14: Run the worker unbounded, and retarget the three tests that relied on the deadline**

`$W/CA/Pool/CasMountRuntime.cpp`, `CasMountRuntime::renewalEnvironment` (~:422-432), whole function:

```cpp
MountRenewOperationEnvironment CasMountRuntime::renewalEnvironment(bool worker_call)
{
    return MountRenewOperationEnvironment{
        .boot_ms = [this] { return bootMsNow(); },
        .live = [this, worker_call]
        {
            return config.renewal_live_for_test ? config.renewal_live_for_test() : renewalLive(worker_call);
        },
        .cancelled = [this] { return renewalCancelled(); },
        /// Only the worker keeps renewing past the lease; startup, remount and direct renewals stay
        /// bounded by it.
        .policy = worker_call ? MountRenewPolicy::UntilDefinitive : MountRenewPolicy::LeaseBound,
        .on_request = {},
    };
}
```

`renewalLive` keeps its body: its `mount_fence.lost` term is now the only fence-loss signal the
worker's renewal sees. `$W/CA/Pool/CasMountRuntime.h`, its doc (~:448-449):

```cpp
    /// The renewal's liveness: a shutdown request and, for the worker, a parked or park-requested
    /// driver, a pool that left `Live`, or a lost fence -- the worker's plane has no fence of its own.
    /// FALSE ends the renewal.
    bool renewalLive(bool worker_call) const;
```

Build and run the whole pool file to see the three retarget candidates fail first:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t4_s14_build.log 2>&1; echo NINJA_EXIT=$? >> build/t4_s14_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASPool*:CASMountRuntime.*' > build/t4_s14_tests.log 2>&1; echo GATE_EXIT=$? >> build/t4_s14_tests.log )
grep -a "FAILED\|GATE_EXIT" build/t4_s14_tests.log | tail -20
```

Expected `FAILED` exactly: `CASPoolRemount.TerminalDepositionDoesNotTouchRenewerAfterReplacement`,
`CASPoolRemount.ImmediatePostRemountRenewalFailureIsNotDropped`,
`CASPool.DeterministicWorkerFailureFencesWithoutWaitingForCadence` (each a barrier timeout: their
one-shot `ThrowBefore` is now retried and the renewal commits). The four new `CASMountRuntime`
tests pass. Any other failure: stop and report.

Retarget them to a definitive answer, which still ends the worker's renewal:

`CASPoolRemount.TerminalDepositionDoesNotTouchRenewerAfterReplacement` (~:3935-3940), replace

```cpp
    backend->fault = RuntimeRenewBackend::Fault::ThrowBefore;
    /// Expire the lease from inside the attempt. The fault alone no longer ends a renewal: the engine
    /// settles the ambiguity by reading and then reissues, and the reissue commits. With the clock past
    /// the deadline the renewal was admitted under, neither the settling read nor the reissue is
    /// admitted, so the renewal ends terminal -- which is what this test deposits.
    backend->before_throw = [&, deadline = anchor + 1000] { boot_ms = deadline; };
```

with

```cpp
    /// A definitive answer ends the worker's renewal; a transient fault would only be retried.
    fenceOutMount(*backend, layout.mountKey("test"));
```

`CASPoolRemount.ImmediatePostRemountRenewalFailureIsNotDropped` (~:4024-4029), replace

```cpp
                boot_ms = 2'000;
                backend->fault = RuntimeRenewBackend::Fault::ThrowBefore;
                /// Expire the fresh lease from inside the attempt, so the ambiguity can be neither
                /// settled by a read nor reissued: otherwise the engine reissues and the renewal
                /// commits, and there is no dropped failure to catch up on.
                backend->before_throw = [&, deadline = fresh_anchor + 10'000] { boot_ms = deadline; };
```

with

```cpp
                boot_ms = 2'000;
                /// A definitive answer for the fresh incarnation's first worker renewal; a transient
                /// fault would only be retried.
                fenceOutMount(*backend, layout.mountKey("test"));
```

`CASPool.DeterministicWorkerFailureFencesWithoutWaitingForCadence` (~:4457-4460), replace

```cpp
    backend->fault = RuntimeRenewBackend::Fault::ThrowBefore;
    /// Expire the lease from inside the attempt, so the ambiguity can be neither settled by a read nor
    /// reissued: without that the engine reissues and the renewal commits, and this worker never fences.
    backend->before_throw = [&, deadline = anchor + 1000] { boot_ms = deadline; };
```

with

```cpp
    /// A definitive answer ends the worker's renewal; a transient fault would only be retried.
    fenceOutMount(*backend, layout.mountKey("test"));
```

Each retargeted test keeps its assertions: the fenced slot ends the renewal terminal from
`WorkerIdle`, which trips the fence and raises a remount generation exactly as the deadline did.
`anchor` / `fresh_anchor` stay used by `armMountFence`.

- [ ] **Step 15: Build, run the tests of this task, then the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t4_s15_build.log 2>&1; echo NINJA_EXIT=$? >> build/t4_s15_build.log
tail -1 build/t4_s15_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASPool*:CASMountRuntime.*:CASHeartbeat.*:CASEvent.*:CASObservability.*:CASRequests.*' > build/t4_s15_tests.log 2>&1; echo GATE_EXIT=$? >> build/t4_s15_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t4_s15_tests.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CAS*' > build/t4_s15_gate.log 2>&1; echo GATE_EXIT=$? >> build/t4_s15_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t4_s15_gate.log | tail -20
```

Expected: `NINJA_EXIT=0`; both runs `GATE_EXIT=0`, no `FAILED`. A failure outside the table at the top
of this task: stop and report it with the log; do not retarget it.

- [ ] **Step 16: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp \
  src/Disks/tests/gtest_cas_pool.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Run the worker's CAS mount renewal without a lease deadline" \
  -m "The background worker renews with MountRenewPolicy::UntilDefinitive: it retries one second apart on the lease plane until the store answers, and a stop, a park or a terminal lifecycle still ends it. Startup, remount and direct renewals stay LeaseBound. Three worker tests that ended a renewal by moving the clock to the deadline now end it with a fenced slot, which is still terminal." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

**Deviations from the brief**

- `on_request`'s comment says "What it throws is ignored" instead of "Must not throw": the engine swallows it, and `CASHeartbeat.AThrowingRequestReportChangesNoOutcome` pins that.
- The observer also reports each failed resolve read, with the `PUT` count at that moment (contract v2). A `readLoop` failure is reported only while `resolve_read_put_no` is set, and `writeLoop` sets it around its resolve read, so a read the caller issues itself stays silent.
- `renewalLive` is unchanged and still ends the worker's renewal on a lost fence. Spec rule 7 says a lost fence alone does not; in part 1 that sentence exists for the readiness renewal (part 2). The term is load-bearing here: `Pool::forgetDisk` publishes the intent and then calls `tripMountLost`, and `renewalLive` does not read `vanished_intent`, so on the fence-less lease plane the lost fence is what ends a retrying renewal during FORGET (`CASMountRuntime.ForgetEndsAnUnboundedRenewal`). Part 2 must add the intent to the predicate before it drops the term.
- Spec test 9 is split. `DefinitiveAnswersStayTerminalPastTheDeadline` starts each case past the deadline, without a preceding outage. After an ambiguous attempt, a refusal is not definitive (`ARefusalAfterAnUnclearAttemptIsRetriedUntilStopped` pins that, by controller ruling). The outage story is `AFenceSeenAfterALongOutageEndsTheRenewal`.
- Spec test 10 at renewer level has four blocks. The `PUT` fuse is in "fast connect failures", the read fuse in its own block; both need `USE_AWS_S3`. "Slow requests" carries no fuse.
- The runtime tests live in `gtest_cas_pool.cpp` under suite `CASMountRuntime`, because `RuntimeUnderTest` is there. Spec rule 10 is `CASMountRuntime.AStaleSuccessIsFollowedAtOnceByTheNextRenewal`; spec test 11's runtime half is `CASMountRuntime.StopWakesTheRetryWaitOfAnUnboundedRenewal`.
- `RuntimeUnderTest::setRetrySleepForTest` drives both the mount and the lease plane, so the red runs show the old deadline ending rather than a renewal stuck on a frozen clock.
- Comments that counted the pool's planes ("three planes", "all three") are rewritten without a count.
- Step 6 already routes `UntilDefinitive` to the new plane. Without that the new member is never read, and an unread private field can fail the build under `-Werror`.
- `ARefusalAfterAnUnclearAttemptIsRetriedUntilStopped` exists only in `USE_AWS_S3` builds: without S3 no exception is classified as a store refusal.

**Noted behaviour (accepted for part 1)**

- A deterministic refusal after an unclear attempt of the same renewal is retried at the spacing until a stop, a park or another definitive answer. The engine cannot tell whether the unclear attempt will still land. This includes revoked credentials once the one refresh is spent. Pinned by `CASHeartbeat.ARefusalAfterAnUnclearAttemptIsRetriedUntilStopped`.

**Review focus**

1. A refused precondition is not reported through `on_request`. A conflict that ends the renewal carries its own classification, so `lifecycle_detail` keeps the text of the last failed request before it. A reasonable person may expect the final conflict there. Task 5 decides.
2. The `SCOPE_EXIT` around the resolve read in `writeLoop` must hold across Task 3's edits to the same lines. A marker left set after a deterministic failure would report a later caller's read as a failure of an old `PUT`. No test throws a deterministic failure out of a resolve read and then reads again on the same operation.


---

### Task 5: An expired lease is visible

Line numbers are from the base commit `41824f955e7`. Tasks 1 to 4 move some of them; every hunk below quotes enough surrounding code to be found by text.

**Files:**
- Modify: `src/Common/ProfileEvents.cpp` (new `CASMountLeaseExpired` after `CASMountLeaseLost`, `:960`)
- Modify: `CA/Pool/CasMountRuntime.h` (public expiry accessors after `refAppendFenceOk`, `:310`; private helpers after `renewalEnvironment`, `:439`; members after `fence_generation`, `:521`)
- Modify: `CA/Pool/CasMountRuntime.cpp` (`checkFenceOrThrow` `:120-136`, `armMountFence` `:170-184`, `renewalEnvironment` `:422-432` as Task 4 leaves it, `consumeRenewResult` `:458-520`, the `reportMountRenewCompletion` declaration `:37`, the `ProfileEvents` block `:22-32`; new functions after `refAppendFenceOk`)
- Modify: `CA/Pool/CasServerRoot.cpp` (`MountRenewObservabilityContext` `:117-136`, `emitMountRenewEvent` `:280-317`, `deliverMountRenewObservability` `:336-427`, `reportMountRenewCompletion` `:48` and `:466-488`)
- Modify: `CA/Pool/CasPool.h` (`Pool::LifecycleSnapshot`, `:541-547`), `CA/Pool/CasPool.cpp` (`Pool::lifecycleSnapshot` `:370-381`)
- Modify: `CA/ContentAddressedMetadataStorage.h` (`CasLifecycleSnapshot` doc, `:93-117`), `CA/ContentAddressedMetadataStorage.cpp` (`lifecycleSnapshot` `:467-472`)
- Modify: `src/Storages/System/StorageSystemContentAddressedMounts.cpp` (column descriptions `:57-59`)
- Test: `src/Disks/tests/gtest_cas_mount_runtime.cpp`, `src/Disks/tests/gtest_cas_pool.cpp`, `src/Disks/tests/gtest_cas_lifecycle_snapshot.cpp`, `src/Disks/tests/gtest_cas_event_log.cpp` (signature only), `src/Disks/tests/gtest_cas_mount.cpp` (`CASMountAudit.RenewalDefaultLogsAreBounded`, third block)

**Interfaces:**
- Consumes (Task 4):
  - `MountRenewRequestEvent { uint32_t request_no; bool failed; String failure_text; }`, `MountRenewOperationEnvironment::policy`, `MountRenewOperationEnvironment::on_request`, `MountRenewPolicy`.
  - The event contract (Task 4): for every `PUT` of a renewal whose `on_request` is set, `{request_no = n, failed = false}` when it is sent; `{request_no = n, failed = true, failure_text}` when that `PUT` throws; and `{request_no = <PUT count so far>, failed = true, failure_text}` for every FAILED resolve read. Successful reads and a refused precondition are not reported. The text is `DB::Exception::message()`, a Poco exception's `displayText()`, or `what()`. So `noteRenewRequest` counts attempts and retries only on `failed == false` events and stores the text of every `failed == true` event, a `PUT`'s or a read's.
  - `CasMountRuntime::renewalEnvironment(worker_call)` sets `policy` and `.on_request = {}`; this task replaces the latter.
  - The runtime constructor takes `CasRequests & lease_requests_` after `farewell_requests_`. `RuntimeFixture` and `RuntimeUnderTest` build that plane (`RuntimeUnderTest::lease`, `Fence::open()`, `setNowFnForTest([this] { return runtime.bootMsNow(); })`, default sleep `runtime.sleepInterruptibly`). This task's tests construct the runtime only through these fixtures, so they need no change for it. `RuntimeUnderTest::setRetrySleepForTest` exists but is not needed here: every failed `PUT` advances the clock past the largest spacing draw, so each retry sleeps `0`.
  - Task 3: a spaced retry whose failed request took longer than the draw sleeps `0`.
- Produces:
  - ProfileEvent `CASMountLeaseExpired`.
  - `std::optional<uint64_t> CasMountRuntime::leaseExpiredSinceBootMs() const`
  - `std::optional<String> CasMountRuntime::leaseExpiredRefusal(uint64_t admitted_generation) const`
  - `String CasMountRuntime::lastRenewFailure() const`
  - `void CasMountRuntime::noteRenewRequest(const MountRenewRequestEvent & event) noexcept`
  - `bool Pool::LifecycleSnapshot::lease_expired`
  - `watermark_renew` event: detail `expired_ms`, classification `committed_after_expiry`.
  - `system.cas_mounts`: `lifecycle = 'not_live'`, `lifecycle_reason = 'lease_expired'` while the lease is expired.

How the parts fit:

- **The expired state** is derived, never stored as a lifecycle: `leaseExpiredSinceBootMs` is set while the lifecycle is `Live`, `mount_fence.lost` is false and `bootMsNow() >= deadline`. A renewal that commits with a start more than a TTL ago moves the deadline to a value that is still past (rule 10). So the first expired deadline is kept in `lease_expired_at_boot_ms` until a renewal restores the lease; the accessor returns the smaller of the two. `armMountFence` clears it: a new incarnation starts with no expiry.
- **`lifecycle_since`** needs a wall-clock second. The runtime has two clocks: `bootMsNow` (the fence clock, injected in tests) and `std::chrono::system_clock` (what `wallClockNowSeconds` in `CasMountRuntime.cpp:46-50` already uses for every other `since`). `Pool::lifecycleSnapshot` samples the boot clock after reading the expiry instant and subtracts the elapsed boot time from the wall clock: `since = (wall_now_ms - (boot_now_ms - expired_at_boot_ms)) / 1000`. Only this server's own two clocks meet, at one instant.
- **Counters.** Today `consumeRenewResult` (`CasMountRuntime.cpp:468-472`) adds `CASMountRenewalAttempts` and `CASMountRenewalRetries` from `result.attempts_sent` after the renewal ends. After this task the worker's renewal (`active_state == RenewalDriverState::WorkerCall`) counts them per request in `noteRenewRequest` and `consumeRenewResult` skips them for it; the startup, remount and direct renewals have no `on_request` and keep counting from the result. Every other counter in `consumeRenewResult` is unchanged.
- **The restore edge** is detected where the deadline is published: `publishRenewedDeadline`, called from `consumeRenewResult` on `Committed`. It samples the clock once, reads the expiry state before the store, stores the new deadline, and reports a restore only when the lease was expired and the new deadline is in the future. A remount's renewal runs with the fence latched lost (`CasMountRuntime.cpp:532-535`), so it never counts as a restore.
- **The refusal text.** Only `CasMountRuntime` can tell "expired" from "too little budget left": it alone holds `lost`, the generation and the deadline. `admit` must keep returning `NoBudget` past the deadline (header constraint; spec test 12; `CASMountRuntime.AdmitRefusesAnExpiredLease`), so the engine cannot tell them apart and its texts do not change. The runtime gives the text through `leaseExpiredRefusal(admitted_generation)`, and only `CasMountRuntime::checkFenceOrThrow` uses it. No `Fence::Admit` value is added, so no `switch` over it changes.
  - Changes its text: `CasMountRuntime::checkFenceOrThrow` (the staged-upload finalize in `ContentAddressedTransaction.cpp:930` and the mount plane's `check_or_throw`). It is inside the runtime and can tell the case apart.
  - Keeps its text, because the engine cannot tell an expired lease from too little budget and this design adds no value for it: `throwReadRefused` ("no lease budget"), `orThrow`'s `GaveUp{Deadline, Lease}` arm ("gave up at the lease deadline"). Both already name the lease.
  - Keeps its text, because it has no handle on the runtime and the design adds no plumbing for it: `CasRefLedger::refuseUnlessAdmitted` and the `may_mutate` check in `CasRefLedger::flushRefBatch` (the ledger sees the fence only through `fence_ok_fn` / `may_mutate` / `fence_generation_fn`, none of which separates an expiry from a loss), and the sites with their own `op.admitted()` wording (`CasRefCatalog.cpp` x6, `CasRefCkpt.cpp` x2, `CasPartWriteTxn.cpp` x2).
  - So a refused `INSERT` fails with the transient class (`NETWORK_ERROR`) and a text that names the lease; only a refusal through `checkFenceOrThrow` says "expired". The operator-facing signal is `system.cas_mounts` and `CASMountLeaseExpired`.

- [ ] **Step 1: Write the failing test for the expired state**

Append to `src/Disks/tests/gtest_cas_mount_runtime.cpp`:

```cpp
/// An expiry is this server's own confirmed deadline passing while nothing else is wrong. A lost fence or
/// a lifecycle that left `Live` is a different state and reports as itself.
TEST(CASMountRuntime, LeaseExpiredOnlyWhileLiveAndNotLost)
{
    RuntimeFixture f(/*lease_safety_margin_ms=*/0);
    f.boot_ms = 1'000;
    EXPECT_FALSE(f->leaseExpiredSinceBootMs().has_value()) << "an unarmed fence has no deadline to pass";

    f->armMountFence(kUuid, 1, /*deadline_boot_ms=*/1'100);
    f.boot_ms = 1'099;
    EXPECT_FALSE(f->leaseExpiredSinceBootMs().has_value());
    f.boot_ms = 1'100;
    ASSERT_TRUE(f->leaseExpiredSinceBootMs().has_value()) << "the deadline instant is already past, as `admit` reads it";
    EXPECT_EQ(*f->leaseExpiredSinceBootMs(), 1'100u);
    EXPECT_TRUE(f->lastRenewFailure().empty()) << "no renewal request has failed";

    f->setLifecycleForTest(PoolLifecycle::IdentityLost);
    EXPECT_FALSE(f->leaseExpiredSinceBootMs().has_value()) << "only a `Live` pool reports an expiry";
    f->setLifecycleForTest(PoolLifecycle::Live);
    ASSERT_TRUE(f->leaseExpiredSinceBootMs().has_value());

    f->tripMountLost();
    EXPECT_FALSE(f->leaseExpiredSinceBootMs().has_value()) << "a lost fence is a lease loss, not an expiry";
}
```

- [ ] **Step 2: Build and see it fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t5_s2.log 2>&1; echo NINJA_EXIT=$? >> build/t5_s2.log
tail -1 build/t5_s2.log; grep -a -n "error:" build/t5_s2.log | head -40
```

Expected: `NINJA_EXIT=1`; errors only in `gtest_cas_mount_runtime.cpp`: `no member named 'leaseExpiredSinceBootMs' in 'DB::Cas::CasMountRuntime'` and `no member named 'lastRenewFailure' in 'DB::Cas::CasMountRuntime'`.

- [ ] **Step 3: Implement the expired state and register the counter**

`src/Common/ProfileEvents.cpp`, after the `CASMountLeaseLost` line (`:960`):

```cpp
    M(CASMountLeaseExpired, "Number of times a renewal restored a CAS mount lease that had expired. While the lease is expired this server refuses writes and system.cas_mounts shows lifecycle_reason = 'lease_expired'; the watermark_renew event of the restoring renewal carries expired_ms.", ValueType::Number) \
```

`CA/Pool/CasMountRuntime.h`: add `#include <optional>` after `#include <mutex>`. After the declaration of `refAppendFenceOk` (`:310`), add:

```cpp
    /// ---- lease expiry ----
    /// The instant this server's confirmed lease expired, on the fence clock, while it stays expired:
    /// the lifecycle is `Live`, the fence is not lost and `bootMsNow` has reached the deadline. A
    /// renewal that commits with a start more than a TTL ago does not end the expiry, so its start is
    /// kept until a renewal restores the lease. Empty otherwise.
    std::optional<uint64_t> leaseExpiredSinceBootMs() const;
    /// Text of the last failed request of the worker's renewal; empty when none failed.
    String lastRenewFailure() const;
```

After `MountRenewOperationEnvironment renewalEnvironment(bool worker_call);` (`:439`), add:

```cpp
    std::optional<uint64_t> leaseExpiredAt(uint64_t now_boot_ms) const;
```

After `std::function<void()> arm_mount_fence_interposition_hook_for_test;` (`:522`), add:

```cpp
    /// The first expired deadline of an expiry that a renewal committed past did not end. `UINT64_MAX`
    /// when there is none. Written by the renewal consumer and by `armMountFence`.
    std::atomic<uint64_t> lease_expired_at_boot_ms{std::numeric_limits<uint64_t>::max()};
    mutable std::mutex renew_failure_mutex;
    String last_renew_failure;
```

`CA/Pool/CasMountRuntime.cpp`, `armMountFence` (`:170-184`): clear the carried expiry with the deadline.

```cpp
void CasMountRuntime::armMountFence(UInt128 server_uuid, uint64_t writer_epoch, uint64_t deadline_boot_ms)
{
    mount_fence.server_uuid = server_uuid;
    mount_fence.writer_epoch = writer_epoch;
    mount_fence.deadline_boot_ms.store(deadline_boot_ms, std::memory_order_release);
    lease_expired_at_boot_ms.store(std::numeric_limits<uint64_t>::max(), std::memory_order_release);
    /// A fresh lease incarnation is a fresh generation too: a durable-effect caller admitted under the
```

(the rest of the function is unchanged). After `refAppendFenceOk` (`:154-163`), add:

```cpp
std::optional<uint64_t> CasMountRuntime::leaseExpiredAt(uint64_t now_boot_ms) const
{
    if (lifecycle() != PoolLifecycle::Live || mount_fence.lost.load(std::memory_order_acquire))
        return std::nullopt;
    const uint64_t deadline = mount_fence.deadline_boot_ms.load(std::memory_order_acquire);
    if (now_boot_ms < deadline)
        return std::nullopt;
    return std::min(deadline, lease_expired_at_boot_ms.load(std::memory_order_acquire));
}

std::optional<uint64_t> CasMountRuntime::leaseExpiredSinceBootMs() const
{
    return leaseExpiredAt(bootMsNow());
}

String CasMountRuntime::lastRenewFailure() const
{
    std::lock_guard lock(renew_failure_mutex);
    return last_renew_failure;
}
```

- [ ] **Step 4: Build and run**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t5_s4.log 2>&1; echo NINJA_EXIT=$? >> build/t5_s4.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASMountRuntime.*' > build/t5_s4_test.log 2>&1; echo GATE_EXIT=$? >> build/t5_s4_test.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t5_s4_test.log | tail -20
```

Expected: `NINJA_EXIT=0`, `GATE_EXIT=0`, `CASMountRuntime.LeaseExpiredOnlyWhileLiveAndNotLost` among the passed.

- [ ] **Step 5: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- src/Common/ProfileEvents.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp src/Disks/tests/gtest_cas_mount_runtime.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Derive the expired state of a CAS mount lease" -m "The lease is expired while the pool is Live, the fence is not lost and the confirmed deadline has passed. Add the accessor and the CASMountLeaseExpired counter that the restoring renewal will bump." -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

- [ ] **Step 6: Write the failing tests for the worker's expiry and its reporting**

Both tests drive the real worker loop (`startBackgroundWorkers`) over a scripted backend and an injected boot clock. Nothing sleeps: the clock moves only inside the scripted `PUT`s and in the loop-top hook, and the hook on the third loop pass is the barrier the test waits on.

The timeline, TTL 30 s, period 10 s:

| Boot ms | What happens |
|---|---|
| 100 000 | Claim and `startRenewer`; the fence is armed to 130 000 |
| 110 000 | Loop pass 1 moves the clock here; the first renewal starts (start 110 000) |
| 110 000 to 141 200 | `PUT`s 1 to 24 fail, each taking 1 300 ms, so no retry waits (Task 3 sleeps `0`) |
| 134 700 | `PUT` 20: the lease expired at 130 000; the test observes the runtime from inside the request |
| 141 200 | `PUT` 25 lands: the deadline becomes 140 000, still past |
| 141 200 | Loop pass 2: due at 120 000, so the second renewal starts at once; `PUT` 26 lands; deadline 171 200 |
| 141 200 | Loop pass 3: the lease is restored after 11 200 ms expired |

In `src/Disks/tests/gtest_cas_pool.cpp`, add to the `ProfileEvents` block at the top (`:42-51`):

```cpp
extern const Event CASMountLeaseExpired;
extern const Event CASMountRenewalAttempts;
extern const Event CASMountRenewalRetries;
```

Append at the end of `src/Disks/tests/gtest_cas_pool.cpp`:

```cpp
namespace
{

/// Fails the guarded mount `PUT`s that `on_put` says to fail, with a timeout raised before the store
/// applied anything, and the reads of the mount slot that `on_read` says to fail. Both run on the
/// renewing thread with the 1-based number of the request of their kind.
class ExpiryScriptBackend final : public DB::Cas::tests::CountingBackend
{
public:
    std::function<bool(uint32_t put_no)> on_put;
    std::function<bool(uint32_t read_no)> on_read;

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        if (on_read && key.ends_with("/mount") && on_read(++reads))
            throw Poco::TimeoutException("injected resolve read timeout");
        return DB::Cas::tests::CountingBackend::read(key, access);
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
        const std::optional<String> & expected_value, DB::Cas::TransportAccess & access) override
    {
        if (on_put && expected_value && key.ends_with("/mount") && on_put(++puts))
            throw Poco::TimeoutException("injected renewal timeout before the store applied it");
        return DB::Cas::tests::CountingBackend::write(key, bytes, expected_value, access);
    }

private:
    uint32_t puts = 0;
    uint32_t reads = 0;
};

constexpr uint64_t kExpiryTtlMs = 30'000;
constexpr uint64_t kExpiryPeriodMs = 10'000;
constexpr uint64_t kExpiryClaimBootMs = 100'000;
constexpr uint64_t kExpiryDeadlineBootMs = kExpiryClaimBootMs + kExpiryTtlMs;
constexpr uint64_t kExpiryFirstStartBootMs = kExpiryClaimBootMs + kExpiryPeriodMs;
/// Longer than the largest retry-spacing draw, so a failed request is retried at once.
constexpr uint64_t kExpiryFailedPutMs = 1'300;
/// Enough failures to carry the first renewal past its own start + TTL.
constexpr uint32_t kExpiryFailedPuts = 24;
constexpr uint64_t kExpiryRestoreBootMs = kExpiryFirstStartBootMs + kExpiryFailedPuts * kExpiryFailedPutMs;
/// A request sent while the lease is expired and the first renewal still retries.
constexpr uint32_t kExpiryObservedPut = 20;
static_assert(kExpiryFirstStartBootMs + (kExpiryObservedPut - 1) * kExpiryFailedPutMs > kExpiryDeadlineBootMs);
static_assert(kExpiryRestoreBootMs > kExpiryFirstStartBootMs + kExpiryTtlMs);

const char * expiryAdmitName(Fence::Admit verdict)
{
    switch (verdict)
    {
        case Fence::Admit::Ok: return "Ok";
        case Fence::Admit::LostOrRearmed: return "LostOrRearmed";
        case Fence::Admit::NoBudget: return "NoBudget";
    }
    return "unknown";
}

uint64_t eventCount(ProfileEvents::Event event)
{
    return ProfileEvents::global_counters[event].load();
}

struct ExpiryObservation
{
    uint64_t generation_before = 0;
    uint64_t attempts_before = 0;
    uint64_t retries_before = 0;
    uint64_t lease_expired_before = 0;
    uint64_t lease_lost_before = 0;

    /// Inside `PUT` number `kExpiryObservedPut`.
    Fence::Admit admit_during = Fence::Admit::Ok;
    bool may_mutate_during = true;
    PoolLifecycle lifecycle_during = PoolLifecycle::TransientNotLive;
    std::optional<uint64_t> expired_since_during;
    String last_failure_during;
    uint64_t attempts_during = 0;
    uint64_t retries_during = 0;

    /// At the loop pass after the renewal that committed with a start more than a TTL ago.
    Fence::Admit admit_after_stale = Fence::Admit::Ok;
    std::optional<uint64_t> expired_since_after_stale;
    uint64_t lease_expired_after_stale = 0;
    uint64_t boot_ms_after_stale = 0;
    uint64_t boot_ms_at_next_admission = 0;

    /// At the loop pass after the restoring renewal.
    Fence::Admit admit_after_restore = Fence::Admit::LostOrRearmed;
    std::optional<uint64_t> expired_since_after_restore;
    PoolLifecycle lifecycle_after_restore = PoolLifecycle::TransientNotLive;
    uint64_t generation_after_restore = 0;
    uint64_t lease_expired_after_restore = 0;
    uint64_t lease_lost_after_restore = 0;
    uint64_t attempts_after_restore = 0;
    uint64_t retries_after_restore = 0;

    uint64_t writer_epoch_on_store = 0;
    uint32_t remount_calls = 0;
    std::vector<CasEvent> events;
    String log;
};

void runExpiryScenario(const String & layout_prefix, ExpiryObservation & seen)
{
    /// Everything the hooks capture is declared before the runtime that stores them.
    auto backend = std::make_shared<ExpiryScriptBackend>();
    const Layout layout(layout_prefix);
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{kExpiryClaimBootMs};
    std::atomic<uint32_t> remount_calls{0};
    uint32_t loop_passes = 0;
    DB::Cas::tests::ManualBarrier third_pass;
    CasMountRuntime * runtime_ptr = nullptr;
    auto events = std::make_shared<DB::Cas::tests::SharedEventLog>();
    CasEventSink sink = [events](CasEvent event) { events->push(std::move(event)); };
    ScopedRemountLogCapture log_capture;

    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, kExpiryTtlMs).kind,
              MountClaimResult::Claimed);

    const auto observe_after_stale = [&]
    {
        CasMountRuntime & runtime = *runtime_ptr;
        seen.admit_after_stale = runtime.admit(seen.generation_before, 0);
        seen.expired_since_after_stale = runtime.leaseExpiredSinceBootMs();
        seen.lease_expired_after_stale = eventCount(ProfileEvents::CASMountLeaseExpired) - seen.lease_expired_before;
        seen.boot_ms_after_stale = boot_ms.load();
    };
    const auto observe_after_restore = [&]
    {
        CasMountRuntime & runtime = *runtime_ptr;
        seen.admit_after_restore = runtime.admit(seen.generation_before, 0);
        seen.expired_since_after_restore = runtime.leaseExpiredSinceBootMs();
        seen.lifecycle_after_restore = runtime.lifecycle();
        seen.generation_after_restore = runtime.fenceGeneration();
        seen.lease_expired_after_restore = eventCount(ProfileEvents::CASMountLeaseExpired) - seen.lease_expired_before;
        seen.lease_lost_after_restore = eventCount(ProfileEvents::CASMountLeaseLost) - seen.lease_lost_before;
        seen.attempts_after_restore = eventCount(ProfileEvents::CASMountRenewalAttempts) - seen.attempts_before;
        seen.retries_after_restore = eventCount(ProfileEvents::CASMountRenewalRetries) - seen.retries_before;
    };

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(kExpiryTtlMs),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms.load(); },
            .renewal_before_driver_lock_hook_for_test = [&]
            {
                ++loop_passes;
                if (loop_passes == 1)
                {
                    boot_ms.store(kExpiryFirstStartBootMs);
                }
                else if (loop_passes == 2)
                {
                    observe_after_stale();
                }
                else if (loop_passes == 3)
                {
                    observe_after_restore();
                    third_pass.arriveAndWait();
                }
            },
            .renewal_admitted_hook_for_test = [&]
            {
                if (loop_passes == 2)
                    seen.boot_ms_at_next_admission = boot_ms.load();
            }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            ++remount_calls;
            return false;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    ASSERT_EQ(anchor, kExpiryClaimBootMs);
    runtime.armMountFence(uuid, 1, anchor + kExpiryTtlMs);

    seen.generation_before = runtime.fenceGeneration();
    seen.attempts_before = eventCount(ProfileEvents::CASMountRenewalAttempts);
    seen.retries_before = eventCount(ProfileEvents::CASMountRenewalRetries);
    seen.lease_expired_before = eventCount(ProfileEvents::CASMountLeaseExpired);
    seen.lease_lost_before = eventCount(ProfileEvents::CASMountLeaseLost);

    backend->on_put = [&](uint32_t put_no)
    {
        if (put_no == kExpiryObservedPut)
        {
            seen.admit_during = runtime.admit(seen.generation_before, 0);
            seen.may_mutate_during = runtime.mayMutate();
            seen.lifecycle_during = runtime.lifecycle();
            seen.expired_since_during = runtime.leaseExpiredSinceBootMs();
            seen.last_failure_during = runtime.lastRenewFailure();
            seen.attempts_during = eventCount(ProfileEvents::CASMountRenewalAttempts) - seen.attempts_before;
            seen.retries_during = eventCount(ProfileEvents::CASMountRenewalRetries) - seen.retries_before;
        }
        if (put_no > kExpiryFailedPuts)
            return false;
        boot_ms.fetch_add(kExpiryFailedPutMs);
        return true;
    };

    runtime.startBackgroundWorkers(std::chrono::milliseconds(kExpiryPeriodMs));
    third_pass.waitUntilArrived();

    seen.writer_epoch_on_store = decodeMountLease(readObj(*backend, layout.mountKey("test"))->bytes).writer_epoch;
    seen.remount_calls = remount_calls.load();
    seen.events = events->snapshot();
    seen.log = log_capture.captured();

    third_pass.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// One renewal whose first `PUT` is unclear and whose resolve reads then fail long enough for the lease
/// to expire; the 19th read finds the slot unchanged and the reissued `PUT` lands.
constexpr uint32_t kReadOutageFailedReads = 18;
constexpr uint32_t kReadOutageObservedRead = 17;
static_assert(kExpiryFirstStartBootMs + (kReadOutageObservedRead - 1) * kExpiryFailedPutMs > kExpiryDeadlineBootMs);

struct ReadOutageObservation
{
    uint64_t attempts_before = 0;
    uint64_t retries_before = 0;
    /// Inside read number `kReadOutageObservedRead`.
    std::optional<uint64_t> expired_since_during;
    String last_failure_during;
    uint64_t attempts_during = 0;
    uint64_t retries_during = 0;
    /// At the loop pass after the renewal.
    uint64_t attempts_after = 0;
    uint64_t retries_after = 0;
};

void runReadOutageScenario(const String & layout_prefix, ReadOutageObservation & seen)
{
    auto backend = std::make_shared<ExpiryScriptBackend>();
    const Layout layout(layout_prefix);
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{kExpiryClaimBootMs};
    uint32_t loop_passes = 0;
    DB::Cas::tests::ManualBarrier second_pass;
    CasEventSink sink;

    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, kExpiryTtlMs).kind,
              MountClaimResult::Claimed);

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(kExpiryTtlMs),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms.load(); },
            .renewal_before_driver_lock_hook_for_test = [&]
            {
                ++loop_passes;
                if (loop_passes == 1)
                {
                    boot_ms.store(kExpiryFirstStartBootMs);
                }
                else if (loop_passes == 2)
                {
                    seen.attempts_after = eventCount(ProfileEvents::CASMountRenewalAttempts) - seen.attempts_before;
                    seen.retries_after = eventCount(ProfileEvents::CASMountRenewalRetries) - seen.retries_before;
                    second_pass.arriveAndWait();
                }
            }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + kExpiryTtlMs);

    seen.attempts_before = eventCount(ProfileEvents::CASMountRenewalAttempts);
    seen.retries_before = eventCount(ProfileEvents::CASMountRenewalRetries);
    backend->on_put = [](uint32_t put_no) { return put_no == 1; };
    backend->on_read = [&](uint32_t read_no)
    {
        if (read_no == kReadOutageObservedRead)
        {
            seen.expired_since_during = runtime.leaseExpiredSinceBootMs();
            seen.last_failure_during = runtime.lastRenewFailure();
            seen.attempts_during = eventCount(ProfileEvents::CASMountRenewalAttempts) - seen.attempts_before;
            seen.retries_during = eventCount(ProfileEvents::CASMountRenewalRetries) - seen.retries_before;
        }
        if (read_no > kReadOutageFailedReads)
            return false;
        boot_ms.fetch_add(kExpiryFailedPutMs);
        return true;
    };

    runtime.startBackgroundWorkers(std::chrono::milliseconds(kExpiryPeriodMs));
    second_pass.waitUntilArrived();
    second_pass.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

std::vector<CasEvent> renewEventsOf(const std::vector<CasEvent> & events)
{
    std::vector<CasEvent> renewals;
    for (const CasEvent & event : events)
        if (event.type == CasEventType::WatermarkRenew)
            renewals.push_back(event);
    return renewals;
}

}

/// Rules 9 and 10 over the real worker loop: an expiry refuses writes without touching the fence, a
/// renewal that commits with a start more than a TTL ago restores nothing and is followed at once, and
/// the next one restores writes under the same epoch and generation.
TEST(CASMountRuntime, ExpiryRefusesWritesAndResumesUnderTheSameEpoch)
{
    ExpiryObservation seen;
    ASSERT_NO_FATAL_FAILURE(runExpiryScenario("runtime-expiry-resume", seen));

    EXPECT_STREQ(expiryAdmitName(seen.admit_during), "NoBudget") << "refused, and not because the fence is lost";
    EXPECT_FALSE(seen.may_mutate_during);
    EXPECT_EQ(seen.lifecycle_during, PoolLifecycle::Live);

    EXPECT_STREQ(expiryAdmitName(seen.admit_after_stale), "NoBudget")
        << "a renewal whose start + TTL is already past restores nothing";
    /// The clock moves only when the test moves it, so a renewal admitted at all after the stale success
    /// was admitted without a cadence wait; a wait would have left loop pass 3 unreached.
    EXPECT_EQ(seen.boot_ms_at_next_admission, seen.boot_ms_after_stale);

    EXPECT_STREQ(expiryAdmitName(seen.admit_after_restore), "Ok");
    EXPECT_EQ(seen.lifecycle_after_restore, PoolLifecycle::Live);
    EXPECT_EQ(seen.generation_after_restore, seen.generation_before) << "an expiry is not a re-arm";
    EXPECT_EQ(seen.lease_lost_after_restore, 0u);
    EXPECT_EQ(seen.writer_epoch_on_store, 1u) << "the same epoch holds the slot";
    EXPECT_EQ(seen.remount_calls, 0u);
}

/// What an operator sees: the expiry and its last failure while it lasts, the attempt counters moving
/// during the outage, and one counter, event field and warning when a renewal restores the lease.
TEST(CASMountRuntime, ExpiryIsReported)
{
    ExpiryObservation seen;
    ASSERT_NO_FATAL_FAILURE(runExpiryScenario("runtime-expiry-report", seen));

    ASSERT_TRUE(seen.expired_since_during.has_value());
    EXPECT_EQ(*seen.expired_since_during, kExpiryDeadlineBootMs);
    EXPECT_NE(seen.last_failure_during.find("injected renewal timeout"), String::npos) << seen.last_failure_during;
    /// The request being sent is counted before it reaches the store.
    EXPECT_EQ(seen.attempts_during, kExpiryObservedPut) << "attempts advance as requests are sent";
    EXPECT_EQ(seen.retries_during, kExpiryObservedPut - 1);

    ASSERT_TRUE(seen.expired_since_after_stale.has_value());
    EXPECT_EQ(*seen.expired_since_after_stale, kExpiryDeadlineBootMs)
        << "a renewal committed past its own start + TTL keeps the expiry and when it began";
    EXPECT_EQ(seen.lease_expired_after_stale, 0u) << "that renewal is not a restore";

    EXPECT_FALSE(seen.expired_since_after_restore.has_value());
    EXPECT_EQ(seen.lease_expired_after_restore, 1u);
    /// 25 requests by the first renewal and one by the second, each counted once.
    EXPECT_EQ(seen.attempts_after_restore, kExpiryFailedPuts + 2);
    EXPECT_EQ(seen.retries_after_restore, kExpiryFailedPuts);

    const std::vector<CasEvent> renewals = renewEventsOf(seen.events);
    ASSERT_EQ(renewals.size(), 2u) << "the retried renewal and the restoring one";
    EXPECT_FALSE(renewals[0].detail.contains("expired_ms")) << "the first renewal left the lease expired";
    EXPECT_EQ(renewals[0].detail.at("classification"), "committed_after_retry");
    EXPECT_EQ(renewals[1].outcome, "recovered");
    EXPECT_EQ(renewals[1].detail.at("classification"), "committed_after_expiry");
    ASSERT_TRUE(renewals[1].detail.contains("expired_ms"));
    EXPECT_EQ(renewals[1].detail.at("expired_ms"), std::to_string(kExpiryRestoreBootMs - kExpiryDeadlineBootMs));

    EXPECT_NE(seen.log.find(fmt::format("expired for {} ms", kExpiryRestoreBootMs - kExpiryDeadlineBootMs)), String::npos)
        << seen.log;
    EXPECT_NE(seen.log.find("injected renewal timeout"), String::npos) << "the warning names the last failure: " << seen.log;

    /// An unclear `PUT` whose resolve reads then fail: the reads are what the operator needs to see,
    /// and they are not requests of the renewal.
    ReadOutageObservation reads;
    ASSERT_NO_FATAL_FAILURE(runReadOutageScenario("runtime-expiry-read-outage", reads));
    ASSERT_TRUE(reads.expired_since_during.has_value()) << "the read outage outlasted the lease, so the row shows this text";
    EXPECT_NE(reads.last_failure_during.find("injected resolve read timeout"), String::npos)
        << "lifecycle_detail carries the last failed read: " << reads.last_failure_during;
    EXPECT_EQ(reads.attempts_during, 1u) << "failed reads do not advance the attempt counter";
    EXPECT_EQ(reads.retries_during, 0u);
    EXPECT_EQ(reads.attempts_after, 2u) << "the unclear PUT and its reissue";
    EXPECT_EQ(reads.retries_after, 1u);
}
```

In `src/Disks/tests/gtest_cas_event_log.cpp`, the forward declaration (`:32`) and the call (`:348`) follow the new signature; this changes no assertion:

```cpp
void reportMountRenewCompletion(const MountRenewResult & result, std::optional<uint64_t> expired_ms) noexcept;
```

```cpp
        reportMountRenewCompletion(result, std::nullopt);
```

(Edit these two lines in Step 8, with the implementation, so Step 7 shows the test failures rather than a link error.)

- [ ] **Step 7: Build and see them fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t5_s7.log 2>&1; echo NINJA_EXIT=$? >> build/t5_s7.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASMountRuntime.Expiry*' > build/t5_s7_test.log 2>&1; echo GATE_EXIT=$? >> build/t5_s7_test.log )
grep -a "FAILED\|PASSED\|GATE_EXIT\|Failure\|Expected\|Which is" build/t5_s7_test.log | tail -40
```

Expected: `NINJA_EXIT=0`.
- `CASMountRuntime.ExpiryRefusesWritesAndResumesUnderTheSameEpoch` PASSES: it fences Task 4's behaviour and is a regression test. Record the pass. If it fails, Task 4 is not in place; stop and report.
- `CASMountRuntime.ExpiryIsReported` FAILS with: `last_failure_during` empty; `attempts_during` `0` against `20` and `retries_during` `0` against `19` (counted at completion today); `*seen.expired_since_after_stale` `140000` against `130000`; `lease_expired_after_restore` `0` against `1`; then `ASSERT_EQ(renewals.size(), 2u)` stops the test at `1` (the one-attempt restoring renewal emits no event today), so the event-field, log and read-outage assertions after it do not run yet (after Step 8 the read-outage block runs; on today's code it would fail on an empty `last_failure_during` and `attempts_during` `0`). `attempts_after_restore == 26` passes already: that pin guards the double count Step 8 could introduce.

- [ ] **Step 8: Implement per-request counting, the restore edge and the event field**

`CA/Pool/CasMountRuntime.h`, after `lastRenewFailure`:

```cpp
    /// Counts each `PUT` of the worker's renewal as it is sent and keeps the text of every failed
    /// `PUT` or resolve read. Runs on the renewing thread.
    void noteRenewRequest(const MountRenewRequestEvent & event) noexcept;
```

After `leaseExpiredAt`:

```cpp
    /// Publishes a committed renewal's deadline. Returns how long the lease had been expired when this
    /// deadline restores it; empty when it was not expired or is still expired.
    std::optional<uint64_t> publishRenewedDeadline(uint64_t deadline_boot_ms);
```

`CA/Pool/CasMountRuntime.cpp`. The `ProfileEvents` block (`:22-32`) gains `extern const Event CASMountLeaseExpired;`. The forward declaration (`:37`) becomes:

```cpp
void reportMountRenewCompletion(const MountRenewResult & result, std::optional<uint64_t> expired_ms) noexcept;
```

After `lastRenewFailure`, add:

```cpp
void CasMountRuntime::noteRenewRequest(const MountRenewRequestEvent & event) noexcept
{
    if (!event.failed)
    {
        ProfileEvents::incrementNoTrace(ProfileEvents::CASMountRenewalAttempts);
        if (event.request_no > 1)
            ProfileEvents::incrementNoTrace(ProfileEvents::CASMountRenewalRetries);
        return;
    }
    try
    {
        std::lock_guard lock(renew_failure_mutex);
        last_renew_failure = event.failure_text;
    }
    catch (...)   // NOLINT(bugprone-empty-catch)
    {
        /// A lost diagnostic must not end the renewal.
    }
}

std::optional<uint64_t> CasMountRuntime::publishRenewedDeadline(uint64_t deadline_boot_ms)
{
    const uint64_t now = bootMsNow();
    const std::optional<uint64_t> expired_at = leaseExpiredAt(now);
    setMountDeadline(deadline_boot_ms);
    if (expired_at && deadline_boot_ms <= now)
    {
        lease_expired_at_boot_ms.store(*expired_at, std::memory_order_release);
        return std::nullopt;
    }
    lease_expired_at_boot_ms.store(std::numeric_limits<uint64_t>::max(), std::memory_order_release);
    if (!expired_at)
        return std::nullopt;
    return now - *expired_at;
}
```

`renewalEnvironment` (as Task 4 leaves it, `:422-432`): the worker's environment counts through `noteRenewRequest`. The whole function:

```cpp
MountRenewOperationEnvironment CasMountRuntime::renewalEnvironment(bool worker_call)
{
    MountRenewOperationEnvironment environment{
        .boot_ms = [this] { return bootMsNow(); },
        .live = [this, worker_call]
        {
            return config.renewal_live_for_test ? config.renewal_live_for_test() : renewalLive(worker_call);
        },
        .cancelled = [this] { return renewalCancelled(); },
        .policy = worker_call ? MountRenewPolicy::UntilDefinitive : MountRenewPolicy::LeaseBound,
    };
    if (worker_call)
        environment.on_request = [this](const MountRenewRequestEvent & event) { noteRenewRequest(event); };
    return environment;
}
```

`consumeRenewResult` (`:458-520`), the whole function:

```cpp
void CasMountRuntime::consumeRenewResult(
    const MountRenewResult & result,
    RenewalDriverState active_state,
    RenewalDriverState returned_state,
    bool propagate_failure)
{
    /// Driver ownership has already been restored by `DriverLease::finish`; this is the single logical
    /// consumption boundary and it runs without `driver_mutex` or renewer access.
    /// The worker's renewal counts its requests as they are sent (`noteRenewRequest`), so an outage
    /// shows while it lasts. Every other renewal counts them here, from its result.
    if (active_state != RenewalDriverState::WorkerCall && result.attempts_sent > 0)
    {
        ProfileEvents::incrementNoTrace(ProfileEvents::CASMountRenewalAttempts, result.attempts_sent);
        ProfileEvents::incrementNoTrace(ProfileEvents::CASMountRenewalRetries, result.attempts_sent - 1);
    }
    if (result.resolved_by_read)
        ProfileEvents::incrementNoTrace(ProfileEvents::CASMountRenewalResolved);

    if (result.outcome == MountRenewOutcome::Committed && (result.attempts_sent > 1 || result.resolved_by_read))
        ProfileEvents::incrementNoTrace(ProfileEvents::CASMountRenewalRecovered);
    if (result.outcome == MountRenewOutcome::Terminal
        && result.deadline_source == GaveUp::Source::Lease)
        ProfileEvents::incrementNoTrace(ProfileEvents::CASMountRenewalDeadlineExceeded);

    if (result.outcome == MountRenewOutcome::Committed)
    {
        const uint64_t ttl_ms = static_cast<uint64_t>(config.mount_lease_ttl_ms.count());
        const std::optional<uint64_t> expired_ms = publishRenewedDeadline(
            result.attempt_start_boot_ms > std::numeric_limits<uint64_t>::max() - ttl_ms
                ? std::numeric_limits<uint64_t>::max()
                : result.attempt_start_boot_ms + ttl_ms);
        if (expired_ms)
        {
            ProfileEvents::incrementNoTrace(ProfileEvents::CASMountLeaseExpired);
            LOG_WARNING(getLogger("CasPool"),
                "CAS mount lease of '{}' was expired for {} ms; a renewal restored it and writes resume. "
                "Last failed renewal request: {}",
                server_root_id, *expired_ms, lastRenewFailure());
        }
        reportMountRenewCompletion(result, expired_ms);
        return;
    }
    if (result.outcome == MountRenewOutcome::NotAttempted)
    {
        reportMountRenewCompletion(result, std::nullopt);
        return;
    }

    if (!result.failure)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: terminal renewal has no failure");
    try
    {
        std::rethrow_exception(result.failure);
    }
    catch (const Exception & e)
    {
        if (e.code() == ErrorCodes::LOGICAL_ERROR)
            throw;
    }
    catch (...)
    {
    }

    reportMountRenewCompletion(result, std::nullopt);

    (void)returned_state;

    if (propagate_failure)
        std::rethrow_exception(result.failure);
}
```

`CA/Pool/CasServerRoot.cpp`. The forward declaration (`:48`):

```cpp
void reportMountRenewCompletion(const MountRenewResult & result, std::optional<uint64_t> expired_ms) noexcept;
```

`MountRenewObservabilityContext` (`:117-134`), after `bool resolved_by_read = false;`:

```cpp
    /// How long the lease had been expired when this renewal restored it; empty unless it did.
    std::optional<uint64_t> expired_ms;
```

In `emitMountRenewEvent` (`:280-317`), replace the `event.reason = ...` statement and add the field after the `remount_attempt_no` line:

```cpp
        if (outcome != "recovered")
            event.reason = "CAS mount renewal ended without retained authority and fenced the mount";
        else if (context.expired_ms)
            event.reason = "CAS mount renewal restored a lease that had expired";
        else
            event.reason = "CAS mount renewal committed after a retry or a resolving read";
```

```cpp
        if (remount_attempt_no != 0)
            event.detail["remount_attempt_no"] = std::to_string(remount_attempt_no);
        if (context.expired_ms)
            event.detail["expired_ms"] = std::to_string(*context.expired_ms);
        (*context.event_sink)(std::move(event));
```

The old reason said "before its confirmed lease-safety deadline"; a worker renewal now commits after that deadline too, so the sentence would be false.

In `deliverMountRenewObservability` (`:364-370`), replace the `recovered` test and the classification:

```cpp
        const bool recovered = context.outcome == MountRenewOutcome::Committed
            && (context.attempts_sent > 1 || context.resolved_by_read || context.expired_ms.has_value());
        if (recovered)
        {
            std::string_view classification = "committed_after_expiry";
            if (context.resolved_by_read)
                classification = "committed_by_read";
            else if (context.attempts_sent > 1)
                classification = "committed_after_retry";
```

`reportMountRenewCompletion` (`:466-488`):

```cpp
void reportMountRenewCompletion(const MountRenewResult & result, std::optional<uint64_t> expired_ms) noexcept
{
    if (mount_renew_observability.suppressed_depth != 0)
    {
        --mount_renew_observability.suppressed_depth;
        return;
    }
    MountRenewObservabilityContext * context = currentMountRenewObservability();
    if (!context || !context->active)
        return;
    context->completed = true;
    context->outcome = result.outcome;
    context->attempts_sent = std::max(context->attempts_sent, result.attempts_sent);
    context->resolved_by_read = result.resolved_by_read;
    context->expired_ms = expired_ms;
    if (context->deferred)
        return;
```

(the rest is unchanged). `static_assert(std::is_trivially_copyable_v<MountRenewObservabilityContext>)` still holds: `std::optional<uint64_t>` is trivially copyable.

Now edit the two lines of `gtest_cas_event_log.cpp` shown in Step 6.

- [ ] **Step 9: Build and run**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t5_s9.log 2>&1; echo NINJA_EXIT=$? >> build/t5_s9.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASMountRuntime.*:CASObservability.*:CASEvent.*:CASMountAudit.*:CASPoolRemount.*:CASHeartbeat.*' > build/t5_s9_test.log 2>&1; echo GATE_EXIT=$? >> build/t5_s9_test.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t5_s9_test.log | tail -20
```

Expected: `NINJA_EXIT=0`, `GATE_EXIT=0`. `CASObservability.RenewalCountersHaveExactPhysicalAndLogicalDeltas` and `ExternalLeaseDeadlineCountsOnceWithoutReconstructingAttempts` pass unchanged: the direct renewal still counts from its result.

- [ ] **Step 10: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp src/Disks/tests/gtest_cas_pool.cpp src/Disks/tests/gtest_cas_event_log.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Report a CAS mount lease expiry and its restore" -m "The worker's renewal counts its requests as they are sent and keeps the last failure text. A renewal that restores an expired lease bumps CASMountLeaseExpired, logs a warning with the duration and the last failure, and puts expired_ms into its watermark_renew event. A renewal that commits with a start already more than a TTL ago keeps the expiry and when it began." -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

- [ ] **Step 11: Write the failing test for the lifecycle snapshot**

Append to `src/Disks/tests/gtest_cas_lifecycle_snapshot.cpp` (add `#include <chrono>` to its includes):

```cpp
/// (g) An expired lease, while the lifecycle enum stays `Live`: `not_live` / `lease_expired`, `since` is the
/// deadline as wall time, and a deadline back in the future returns the row to `live`.
TEST(CASLifecycleSnapshot, ExpiredLeaseIsNotLiveUntilRestored)
{
    auto storage = openSnapshotStorage();
    commitOnePart(*storage);
    auto pool = storage->store();
    const auto wall_now_s = []
    {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    };

    const int64_t before_s = wall_now_s();
    pool->setMountDeadline(pool->bootMsNow() - 5'000);
    const CasLifecycleSnapshot expired = storage->lifecycleSnapshot();
    const int64_t after_s = wall_now_s();

    EXPECT_EQ(expired.lifecycle, "not_live");
    EXPECT_EQ(expired.reason, "lease_expired");
    EXPECT_GE(static_cast<int64_t>(expired.since), before_s - 6) << "the deadline passed about 5 s ago";
    EXPECT_LE(static_cast<int64_t>(expired.since), after_s - 4);
    EXPECT_TRUE(expired.detail.empty()) << "no renewal request failed, so there is no failure text: " << expired.detail;
    EXPECT_EQ(pool->lifecycle(), PoolLifecycle::Live) << "the lifecycle enum is not touched";

    pool->setMountDeadline(pool->bootMsNow() + 60'000);
    const CasLifecycleSnapshot restored = storage->lifecycleSnapshot();
    EXPECT_EQ(restored.lifecycle, "live");
    EXPECT_TRUE(restored.reason.empty()) << restored.reason;
    EXPECT_EQ(restored.since, 0);
}
```

- [ ] **Step 12: Build and see it fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t5_s12.log 2>&1; echo NINJA_EXIT=$? >> build/t5_s12.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASLifecycleSnapshot.*' > build/t5_s12_test.log 2>&1; echo GATE_EXIT=$? >> build/t5_s12_test.log )
grep -a "FAILED\|PASSED\|GATE_EXIT\|Expected\|Which is" build/t5_s12_test.log | tail -20
```

Expected: `NINJA_EXIT=0`; `ExpiredLeaseIsNotLiveUntilRestored` fails: `expired.lifecycle` is `"live"`, `expired.reason` is `""`, `expired.since` is `0`. The other `CASLifecycleSnapshot` tests pass.

- [ ] **Step 13: Implement the snapshot**

`CA/Pool/CasPool.h`, `Pool::LifecycleSnapshot` (`:541-547`):

```cpp
    struct LifecycleSnapshot
    {
        PoolLifecycle lifecycle = PoolLifecycle::Live;
        String detail;
        time_t since = 0;
        /// The lifecycle is `Live` but this server's lease expired and no renewal has restored it yet.
        /// `detail` is then the last failed renewal request and `since` the expired deadline.
        bool lease_expired = false;
    };
```

`CA/Pool/CasPool.cpp`. In the file's anonymous namespace (or a new one above `Pool::lifecycleSnapshot`):

```cpp
/// The wall-clock second of a past `CLOCK_BOOTTIME` instant: wall now minus how long ago it was on the
/// boot clock. Both clocks are this server's own, sampled at one moment.
time_t wallSecondOfPastBootInstant(uint64_t past_boot_ms, uint64_t now_boot_ms)
{
    const int64_t wall_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const uint64_t ago_ms = now_boot_ms > past_boot_ms ? now_boot_ms - past_boot_ms : 0;
    return static_cast<time_t>((wall_now_ms - static_cast<int64_t>(ago_ms)) / 1000);
}
```

`Pool::lifecycleSnapshot` (`:370-381`):

```cpp
Pool::LifecycleSnapshot Pool::lifecycleSnapshot() const
{
    /// Non-gated, I/O-free (spec §7). Read the lifecycle ONCE (acquire), then the detail/`since` it
    /// implies. `lifecycleReasonDetail` reads `vanishedReason()` only for a terminal state we have already
    /// acquire-observed here, and `since` was release-stored before the same transition — so this coherent
    /// triple never mixes a terminal state with a pre-terminal detail/timestamp.
    LifecycleSnapshot snap;
    snap.lifecycle = mount_runtime.lifecycle();
    snap.detail = lifecycleReasonDetail(snap.lifecycle);
    snap.since = mount_runtime.lifecycleSinceWallS();
    if (snap.lifecycle != PoolLifecycle::Live)
        return snap;
    const std::optional<uint64_t> expired_at = mount_runtime.leaseExpiredSinceBootMs();
    if (!expired_at)
        return snap;
    snap.lease_expired = true;
    snap.detail = mount_runtime.lastRenewFailure();
    snap.since = wallSecondOfPastBootInstant(*expired_at, mount_runtime.bootMsNow());
    return snap;
}
```

`CA/ContentAddressedMetadataStorage.cpp`, `lifecycleSnapshot` (`:467-470`):

```cpp
    const Cas::Pool::LifecycleSnapshot ps = pool->lifecycleSnapshot();
    snap.lifecycle = ps.lease_expired ? "not_live" : casLifecycleToString(ps.lifecycle);
    snap.reason = ps.lease_expired ? "lease_expired" : casLifecycleReasonWord(ps.lifecycle);
    snap.detail = ps.detail;
    snap.since = ps.since;
```

`CA/ContentAddressedMetadataStorage.h`, the `CasLifecycleSnapshot` doc: the `reason`, `detail` and `since` bullets become:

```cpp
///   - `reason`         — the ENUM-CLEAN sub-state word: `replaced` / `forgotten` for a `vanished` pool,
///                        `lease_expired` for a `not_live` row whose lifecycle enum is still `Live` but
///                        whose lease expired, empty otherwise. A small closed vocabulary, so a downstream
///                        `lifecycle || '(' || reason || ')'` yields e.g. exactly `vanished(forgotten)` —
///                        the [D5] free text lives in `detail`, never here.
///   - `detail`         — the full [D5] reason text naming the actual failure (the replaced
///                        diagnosis, the timestamped `FORGET` message, or the identity-loss message); spec §1
///                        requires it appear verbatim in the snapshot. For `lease_expired`, the text of the
///                        last failed renewal request. Empty while `live` and for a null pool.
///   - `since`          — wall-clock second the current non-`live` state was entered (for `lease_expired`,
///                        the confirmed deadline that passed); 0 while `live`/no pool.
```

`src/Storages/System/StorageSystemContentAddressedMounts.cpp` (`:57-59`):

```cpp
        {"lifecycle_reason", std::make_shared<DataTypeString>(), "The enum-clean sub-state word: replaced or forgotten for a vanished disk, lease_expired for a not_live disk whose mount lease expired and no renewal has restored it yet. Empty for every other lifecycle (so lifecycle || '(' || lifecycle_reason || ')' reads e.g. vanished(forgotten))."},
        {"lifecycle_detail", std::make_shared<DataTypeString>(), "The full typed reason text naming the actual cause when not live: the vanish diagnosis (data root replaced by a foreign pool / decommissioned by SYSTEM CAS FORGET at <time>), the identity-loss message, or for lease_expired the text of the last failed renewal request. Empty when live."},
        {"lifecycle_since", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeDateTime>()), "When this server entered the current non-live lifecycle state; for lease_expired, the confirmed lease deadline that passed. NULL when live (or the state is not backed by a live pool)."},
```

- [ ] **Step 14: Build and run**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t5_s14.log 2>&1; echo NINJA_EXIT=$? >> build/t5_s14.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASLifecycleSnapshot.*:CASMountRuntime.*' > build/t5_s14_test.log 2>&1; echo GATE_EXIT=$? >> build/t5_s14_test.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t5_s14_test.log | tail -20
```

Expected: `NINJA_EXIT=0`, `GATE_EXIT=0`.

- [ ] **Step 15: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Storages/System/StorageSystemContentAddressedMounts.cpp src/Disks/tests/gtest_cas_lifecycle_snapshot.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Show an expired CAS mount lease in system.cas_mounts" -m "While the lease is expired and the pool is Live, the row reads lifecycle = 'not_live', lifecycle_reason = 'lease_expired', lifecycle_since = the expired deadline as wall time and lifecycle_detail = the last failed renewal request. The PoolLifecycle enum and the state column are unchanged." -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

- [ ] **Step 16: Write the failing tests for the refusal text**

The one site that changes. In `src/Disks/tests/gtest_cas_mount_runtime.cpp`, add `#include <Common/Exception.h>` and `#include <functional>` and, above the anonymous namespace:

```cpp
namespace DB::ErrorCodes
{
extern const int NETWORK_ERROR;
}
```

In the anonymous namespace, after `admitName`:

```cpp
/// The message of the refusal `refuse` throws; a failure when it does not throw the transient class.
String refusalText(const std::function<void()> & refuse)
{
    try
    {
        refuse();
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::NETWORK_ERROR) << e.message();
        return e.message();
    }
    ADD_FAILURE() << "the fence check did not refuse";
    return {};
}
```

Append:

```cpp
/// Only a refusal caused by the expiry itself names it: an operator can wait for a renewal then, and for
/// nothing else.
TEST(CASMountRuntime, ExpiredLeaseRefusalSaysWritesResume)
{
    RuntimeFixture f(/*lease_safety_margin_ms=*/0);
    f.boot_ms = 1'000;
    f->armMountFence(kUuid, 1, /*deadline_boot_ms=*/1'100);
    const uint64_t generation = f->fenceGeneration();
    f.boot_ms = 1'100;

    const String expired = refusalText([&] { f->checkFenceOrThrow(generation); });
    EXPECT_NE(expired.find("lease expired"), String::npos) << expired;
    EXPECT_NE(expired.find("writes resume when a renewal succeeds"), String::npos) << expired;

    /// A re-arm moves the generation while the lease stays expired: the caller's incarnation is gone.
    f->armMountFence(kUuid, 1, /*deadline_boot_ms=*/1'100);
    const String moved = refusalText([&] { f->checkFenceOrThrow(generation); });
    EXPECT_EQ(moved.find("lease expired"), String::npos) << moved;
    EXPECT_NE(moved.find("mount fence tripped"), String::npos) << moved;

    f->tripMountLost();
    const String lost = refusalText([&] { f->checkFenceOrThrow(f->fenceGeneration()); });
    EXPECT_EQ(lost.find("lease expired"), String::npos) << lost;
    EXPECT_NE(lost.find("mount fence tripped"), String::npos) << lost;
}
```

- [ ] **Step 17: Build and see them fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t5_s17.log 2>&1; echo NINJA_EXIT=$? >> build/t5_s17.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASMountRuntime.ExpiredLeaseRefusalSaysWritesResume' > build/t5_s17_test.log 2>&1; echo GATE_EXIT=$? >> build/t5_s17_test.log )
grep -a "FAILED\|PASSED\|GATE_EXIT\|Which is\|mount fence\|no longer admitted\|ref-log" build/t5_s17_test.log | tail -30
```

Expected: `NINJA_EXIT=0`, `GATE_EXIT=1`. `ExpiredLeaseRefusalSaysWritesResume` fails on the first two assertions: the expired message is today's "mount fence tripped: the durable write is refused because this node no longer holds the mount incarnation ...", without "lease expired" or "writes resume when a renewal succeeds". The moved-generation and lost-fence assertions pass already.

- [ ] **Step 18: Implement the refusal text**

`CA/Pool/CasMountRuntime.h`, after `lastRenewFailure`:

```cpp
    /// The condition text for a request admitted under `admitted_generation` that is refused only
    /// because the lease expired; empty when the refusal has any other cause or there is none.
    std::optional<String> leaseExpiredRefusal(uint64_t admitted_generation) const;
```

`CA/Pool/CasMountRuntime.cpp`, after `leaseExpiredSinceBootMs`:

```cpp
std::optional<String> CasMountRuntime::leaseExpiredRefusal(uint64_t admitted_generation) const
{
    if (fenceGeneration() != admitted_generation || !leaseExpiredSinceBootMs())
        return std::nullopt;
    return String("the mount lease expired and no renewal has restored it yet; "
                  "writes resume when a renewal succeeds");
}
```

`checkFenceOrThrow` (`:120-136`), the whole function. The admitted case returns before any extra clock read:

```cpp
void CasMountRuntime::checkFenceOrThrow(uint64_t admitted_generation) const
{
    if (mayMutate() && fenceGeneration() == admitted_generation)
        return;
    const String subject = fmt::format("content-addressed pool '{}'", server_root_id);
    if (const std::optional<String> expired = leaseExpiredRefusal(admitted_generation))
        throwCasTransientUnavailable(subject, *expired);
    /// [D5]: tell only what is known here. A tripped fence (or a bumped generation) means this node no
    /// longer holds the mount incarnation the caller was admitted under -- but this same guard trips for a
    /// transient lease blip AND for a deliberate terminal decommission (FORGET) or a lost identity, and this
    /// code cannot tell them apart. So the CONDITION must NOT promise recovery ("temporarily unreachable"
    /// would misdiagnose the terminal case); it names both possibilities and points at the authoritative
    /// lifecycle. The CLASS is the write plane's uniform transient one (its 32 sibling write-transient sites
    /// already mint it): under genuine ambiguity the refusal must be retried, never consumed as damage.
    throwCasTransientUnavailable(
        subject,
        "mount fence tripped: the durable write is refused because this node no longer holds the mount "
        "incarnation it was admitted under -- either a lease loss the disk auto-recovers from, or a "
        "FORGET decommission / lost identity that does NOT recover; consult "
        "system.cas_mounts for the disk's lifecycle before retrying");
}
```

- [ ] **Step 19: Build and run**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t5_s19.log 2>&1; echo NINJA_EXIT=$? >> build/t5_s19.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASMountRuntime.*:CASPool*:CASRefWriter*' > build/t5_s19_test.log 2>&1; echo GATE_EXIT=$? >> build/t5_s19_test.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t5_s19_test.log | tail -20
```

Expected: `NINJA_EXIT=0`, `GATE_EXIT=0`.

- [ ] **Step 20: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp src/Disks/tests/gtest_cas_mount_runtime.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Say that writes resume when a CAS lease expiry is the only refusal" -m "checkFenceOrThrow asks the mount runtime whether the lease expired with the fence still held and the generation unchanged. Other refusals keep their text; admission is unchanged." -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

- [ ] **Step 21: Change the log test so it fails while the per-request `DEBUG` loop exists**

`src/Disks/tests/gtest_cas_mount.cpp`, `CASMountAudit.RenewalDefaultLogsAreBounded`, third block (`:356-364`):

```cpp
    {
        auto backend = std::make_shared<RenewalLogBackend>();
        auto boot_ms = std::make_shared<std::atomic<uint64_t>>(100);
        auto store = open_store(backend, boot_ms, "renewal-log-debug");
        ScopedRenewalLogCapture capture("debug");
        backend->throw_before_next_overwrite = true;
        EXPECT_NO_THROW(store->renewWatermarkOnce());
        EXPECT_EQ(countRenewalLogText(capture.captured(), "physical retry attempt"), 0u)
            << "requests are counted as they are sent, not replayed as log lines when the renewal ends";
    }
```

This is the only test that asserts those lines (`grep -rn "physical retry attempt" src/Disks/tests tests` finds this file only: the second block already expects `0` at `information` level, this block expected `1` at `debug`).

- [ ] **Step 22: Build and see it fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t5_s22.log 2>&1; echo NINJA_EXIT=$? >> build/t5_s22.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASMountAudit.RenewalDefaultLogsAreBounded' > build/t5_s22_test.log 2>&1; echo GATE_EXIT=$? >> build/t5_s22_test.log )
grep -a "FAILED\|PASSED\|GATE_EXIT\|Which is" build/t5_s22_test.log | tail -10
```

Expected: `GATE_EXIT=1`; the count is `1`, expected `0`.

- [ ] **Step 23: Remove the loop**

`CA/Pool/CasServerRoot.cpp`, `deliverMountRenewObservability`: delete the `for (uint32_t attempt_no = 2; attempt_no <= context.attempts_sent; ++attempt_no)` loop with its `LOG_DEBUG` (`:347-362`). The function then starts:

```cpp
void deliverMountRenewObservability(
    const MountRenewObservabilityContext & context, uint64_t remount_attempt_no) noexcept
{
    if (!context.active || !context.completed || !context.server_root_id)
        return;

    try
    {
        const uint64_t now_boot_ms = defaultBootMs();
        const String write_attempt_id = u128ToHex(context.write_attempt_id).substr(0, 12);

        const bool recovered = context.outcome == MountRenewOutcome::Committed
            && (context.attempts_sent > 1 || context.resolved_by_read || context.expired_ms.has_value());
```

- [ ] **Step 24: Build, run this task's tests, then the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t5_s24.log 2>&1; echo NINJA_EXIT=$? >> build/t5_s24.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CASMountAudit.*:CASMountRuntime.*:CASLifecycleSnapshot.*:CASEvent.*:CASObservability.*' > build/t5_s24_test.log 2>&1; echo GATE_EXIT=$? >> build/t5_s24_test.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t5_s24_test.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock build/src/unit_tests_dbms --gtest_filter='CAS*' > build/t5_gate.log 2>&1; echo GATE_EXIT=$? >> build/t5_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t5_gate.log | tail -20
stat -c %Y build/src/unit_tests_dbms
```

Expected: `NINJA_EXIT=0`, both `GATE_EXIT=0`. The gate passes the base count plus Tasks 1 to 4 plus this task's 5 new tests. The binary is newer than every file edited in this task.

- [ ] **Step 25: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp src/Disks/tests/gtest_cas_mount.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Drop the per-request DEBUG lines of a CAS mount renewal" -m "The attempt counters now advance as requests are sent, so replaying one line per request when the renewal ends adds nothing." -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

**Existing tests this task changes:**
- `CASMountAudit.RenewalDefaultLogsAreBounded` (`gtest_cas_mount.cpp`), third block: `"physical retry attempt 2"` count `1` becomes `"physical retry attempt"` count `0`.
- `gtest_cas_event_log.cpp`: the forward declaration and the one call of `reportMountRenewCompletion` take the new `expired_ms` argument (`std::nullopt`). No assertion changes.
- None deleted. `CASObservability.RenewalCountersHaveExactPhysicalAndLogicalDeltas`, `ExternalLeaseDeadlineCountsOnceWithoutReconstructingAttempts`, `CASEvent.WatermarkRenewEventsAreBoundedAndComplete` and `CASMountRuntime.AdmitRefusesAnExpiredLease` stay as they are and must stay green.
- `tests/integration/test_cas_mount_renewal_retry/test.py:372-373` and `:498-499` read the same totals: the worker's requests are counted once each, only earlier.

**Deviations from the brief**
- Spec test 13 asks `CASMountRuntime.ExpiryIsReported` to read the lifecycle snapshot. The snapshot belongs to `Pool` and `ContentAddressedMetadataStorage`; the runtime fixture has neither. The runtime test asserts what the snapshot is built from (`leaseExpiredSinceBootMs`, `lastRenewFailure`); `CASLifecycleSnapshot.ExpiredLeaseIsNotLiveUntilRestored` asserts the row itself.
- `leaseExpiredSinceBootMs` returns the first expired deadline, not the current one, when a renewal committed with a start more than a TTL ago (rule 10). Otherwise `lifecycle_since` and `expired_ms` would move forward by up to one period at that renewal and understate the expiry.
- The refusal text is narrower than the spec's table row ("Text of a write refused only because the lease expired"); the controller ruled it. Only `checkFenceOrThrow` names the expiry. The engine's texts, `refuseUnlessAdmitted`, `flushRefBatch` and the sites with their own `op.admitted()` wording keep theirs (list and reasons under "How the parts fit"). `admit` keeps returning `NoBudget`; no `Fence::Admit` value and no engine change. Task 6's integration test asserts only that a refused `INSERT` fails with the transient class and that its text names the lease.
- `emitMountRenewEvent`'s `recovered` reason text changes from "recovered before its confirmed lease-safety deadline" to "committed after a retry or a resolving read": a worker renewal can now commit after that deadline. No test asserts the reason.
- `docs/en/operations/system-tables/cas_mounts.md` (`lifecycle_reason = 'lease_expired'`, its `lifecycle_detail` and `lifecycle_since`) is left to Task 6.

**Review focus**
- A write refused during an expiry anywhere but `checkFenceOrThrow` (a ref append through `refuseUnlessAdmitted` or `flushRefBatch`, a cold namespace's catalog read, a blob `HEAD`): the text says "no lease budget", "no longer admitted" or "CAS mount lost / lease expired", never that writes resume after a renewal. A reasonable operator reads `system.cas_mounts` to learn it is an expiry; nothing tests that every such refusal is the transient class.
- A renewal restored by the startup, remount or direct path, or a terminal worker renewal during an expiry: the lease-lost path reports through `CASMountLeaseLost` and a remount, and `armMountFence` clears the carried expiry. Nothing here tests that a remount after an expiry does not count a `CASMountLeaseExpired`; one expects it not to.
- `lifecycle_since` against a wall clock step during the expiry: the conversion uses the wall clock at snapshot time, so an NTP step moves the reported `since` by the step. One expects `since` to name roughly when writes stopped, not to be exact across a step.


---

### Task 6: Integration tests and documentation

**Files:**
- Modify: `tests/integration/test_cas_mount_renewal_retry/test.py` (events tuple, four helpers, two tests inserted before `test_hard_restart_observes_then_the_unsafe_knob_skips_the_observation`)
- Modify: `docs/en/operations/system-tables/cas_mounts.md`, `docs/en/antalya/cas/architecture/mounts-and-leases.md`, `docs/en/antalya/cas/configuration.md`, `docs/en/antalya/cas/operations/troubleshooting.md` (all three exist in the worktree)
- Create: `tmp/pr-body.md` in `/home/mfilimonov/workspace/ClickHouse/cas-95-1` (text below; the PR is not opened here)

**Interfaces:**
- Consumes: Task 4 (worker renewal without a deadline: `CASMountRenewalRetries` advances per request), Task 5 (`CASMountLeaseExpired`, `lifecycle_reason = 'lease_expired'`, `expired_ms` in the `watermark_renew` row, the text of a write refused because the lease expired, the removed per-request `DEBUG` lines).
- Produces: nothing later tasks rely on.

Facts about the fixture, read from the worktree (`tests/integration/test_cas_mount_renewal_retry/`):

- One writable CAS node, `node`, with `cas_gc_enabled = false` (`configs/storage_conf.xml`). There is no second node and no GC leader, so nothing can fence the slot during a fault. The assertions below run on `node`; `budget_probe` has no mount under test.
- Lease timing: TTL 10000 ms, renew period 2000 ms, attempt timeout 500 ms, safety margin 500 ms (`MOUNT_LEASE_TTL_MS`, `MOUNT_RENEW_PERIOD_MS` in `test.py`).
- The proxy (`s3_fault_proxy.py`) is armed through `POST /config` with `rate`, `modes`, `methods`, `path_substring`, `remaining_faults`, `seed`. A `path_substring` without `remaining_faults` faults every matching request until `rate` is set back to `0.0`. `MOUNT_REQUEST_PATH` scopes the fault to the mount object, so data and ref-log writes still reach the store; that is what lets the long test see an `INSERT` refused by the lease and nothing else.
- Existing helpers: `_control`, `_wait_until` (probe gets a `_Deadline`), `_profile_events`, `_event_delta`, `_mount_snapshot`. The logger of the `fenced after` line is `CasMountLeaseRenewer` (`CasServerRoot.cpp`); the logger of the remount line `CAS whole-chain remount attempt` is `CasPool` (`CasPool.cpp`).
- Text-log queries must filter on `logger_name`: the text of a query that mentions a phrase is itself logged by `executeQuery` and would match its own `LIKE`.

- [ ] **Step 1: Add the events and four helpers**

In `test.py`, extend `RENEWAL_EVENTS` (system.events lists every event, and `_profile_events` defaults a missing row to 0, so this is safe before Task 5):

```python
RENEWAL_EVENTS = (
    "CASMountRenewalAttempts",
    "CASMountRenewalRetries",
    "CASMountRenewalResolved",
    "CASMountRenewalRecovered",
    "CASMountRenewalDeadlineExceeded",
    "CASMountLeaseLost",
    "CASMountLeaseExpired",
    "CASRemountAttempts",
    "CASRemountSucceeded",
    "CASRemountFailed",
)
```

Add after `_renewal_log_rows`:

```python
def _lease_row(node, timeout=None):
    row = node.query(
        "SELECT lifecycle, ifNull(lifecycle_reason, ''), ifNull(lifecycle_detail, '') "
        "FROM system.cas_mounts WHERE disk = '{}' AND server_root_id = '{}' LIMIT 1 FORMAT TSV".format(
            DISK, SERVER_ROOT_ID
        ),
        timeout=timeout,
    ).rstrip("\n")
    assert row, "the local CAS mount row must be visible"
    lifecycle, reason, detail = row.split("\t")
    return lifecycle, reason, detail


def _writer_epoch(node):
    return int(
        node.query(
            "SELECT writer_epoch FROM system.cas_mounts WHERE disk = '{}' AND server_root_id = '{}' "
            "LIMIT 1".format(DISK, SERVER_ROOT_ID)
        ).strip()
    )


def _text_log_count(node, logger, pattern, since):
    node.query("SYSTEM FLUSH LOGS")
    return int(
        node.query(
            "SELECT count() FROM system.text_log WHERE logger_name = '{}' "
            "AND message LIKE '%{}%' AND event_time_microseconds >= toDateTime64('{}', 6)".format(
                logger, pattern, since
            )
        ).strip()
    )


def _expired_ms_values(node, since):
    node.query("SYSTEM FLUSH LOGS")
    rows = node.query(
        "SELECT detail['expired_ms'] FROM system.cas_log "
        "WHERE event_type = 'watermark_renew' AND disk_name = '{}' "
        "AND detail['server_root_id'] = '{}' AND mapContains(detail, 'expired_ms') "
        "AND event_time_microseconds >= toDateTime64('{}', 6) "
        "ORDER BY event_time_microseconds FORMAT TSV".format(DISK, SERVER_ROOT_ID, since)
    )
    return [int(row) for row in rows.splitlines() if row]


def _arm_mount_put_outage(control_url, seed):
    _control(
        control_url,
        "/config",
        {
            "rate": 1.0,
            "modes": ["503"],
            "methods": ["PUT", "POST"],
            "path_substring": MOUNT_REQUEST_PATH,
            "seed": seed,
        },
    )
    return time.monotonic()


def _wait_for_a_renewal_to_commit(node):
    # Returns right after a renewal committed, so the next one starts a full period later. Polling
    # `renewal_sequence` pins the outage to a known place in the renewal cycle without a blind sleep.
    sequence_before = _mount_snapshot(node)["sequence"]
    return _wait_until(
        lambda deadline: (
            sequence
            if (sequence := _mount_snapshot(node, timeout=deadline.remaining())["sequence"]) > sequence_before
            else None
        ),
        timeout=30,
        interval=0.05,
    )
```

- [ ] **Step 2: Write the short-outage test**

Insert before `test_hard_restart_observes_then_the_unsafe_knob_skips_the_observation`:

```python
def test_short_put_outage_does_not_fence(start_cluster):
    # The outage is shorter than TTL - period (8 s) and starts right after a renewal committed, so the
    # next renewal starts inside it and keeps retrying until the store answers. The lease never
    # expires, nothing is fenced, and the epoch does not move.
    node = start_cluster["node"]
    control_url = start_cluster["control_url"]
    outage_s = 6
    _control(control_url, "/config", {"reset": True})
    _wait_until(lambda deadline: _lease_row(node, timeout=deadline.remaining())[0] == "live", timeout=60)
    epoch_before = _writer_epoch(node)
    counters_before = _profile_events(node)
    since = node.query("SELECT toString(now64(6))").strip()

    sequence_at_arm = _wait_for_a_renewal_to_commit(node)
    armed_at = _arm_mount_put_outage(control_url, seed=811)
    time.sleep(max(0.0, outage_s - (time.monotonic() - armed_at)))
    stats = _control(control_url, "/stats")
    _control(control_url, "/config", {"rate": 0.0})

    # Checked before waiting for recovery so a fence shows up as this message, not as a timeout below.
    assert _text_log_count(node, "CasMountLeaseRenewer", "fenced after", since) == 0
    assert stats["faults"] >= 1, "the outage did not cover a renewal start: {}".format(stats)

    _wait_until(
        lambda deadline: _mount_snapshot(node, timeout=deadline.remaining())["sequence"] > sequence_at_arm,
        timeout=30,
        interval=0.05,
    )
    delta = _event_delta(counters_before, _profile_events(node))

    assert _text_log_count(node, "CasMountLeaseRenewer", "fenced after", since) == 0
    assert delta["CASMountLeaseLost"] == 0, delta
    assert delta["CASMountLeaseExpired"] == 0, delta
    assert delta["CASMountRenewalRetries"] > 0, delta
    assert delta["CASRemountAttempts"] == 0, delta
    assert _lease_row(node)[0] == "live"
    assert _writer_epoch(node) == epoch_before

    node.query("INSERT INTO renewal_probe VALUES (101, 'after-short-outage')")
    assert node.query("SELECT count() FROM renewal_probe WHERE id = 101").strip() == "1"
```

- [ ] **Step 3: Write the long-outage test**

Insert after the test above:

```python
def test_long_put_outage_resumes_under_the_same_epoch(start_cluster):
    # The outage lasts TTL + half a period, so the lease expires while renewals keep retrying. Writes
    # are refused for as long as it is expired; the first renewal after the outage restores the lease
    # under the same writer_epoch, with no fence and no remount. GC is off on this disk, so nothing
    # can fence the slot meanwhile.
    node = start_cluster["node"]
    control_url = start_cluster["control_url"]
    outage_s = (MOUNT_LEASE_TTL_MS + MOUNT_RENEW_PERIOD_MS // 2) / 1000.0
    _control(control_url, "/config", {"reset": True})
    _wait_until(lambda deadline: _lease_row(node, timeout=deadline.remaining())[0] == "live", timeout=60)
    epoch_before = _writer_epoch(node)
    counters_before = _profile_events(node)
    since = node.query("SELECT toString(now64(6))").strip()

    _wait_for_a_renewal_to_commit(node)
    armed_at = _arm_mount_put_outage(control_url, seed=812)

    def expired(deadline):
        lifecycle, reason, detail = _lease_row(node, timeout=deadline.remaining())
        return (lifecycle, reason, detail) if (lifecycle, reason) == ("not_live", "lease_expired") else None

    _, _, detail = _wait_until(expired, timeout=40, interval=0.1)
    assert detail, "lifecycle_detail must carry the last failed request"

    error = node.query_and_get_error("INSERT INTO renewal_probe VALUES (301, 'during-outage')")
    assert "NETWORK_ERROR" in error and "lease" in error.lower(), error

    time.sleep(max(0.0, outage_s - (time.monotonic() - armed_at)))
    assert _lease_row(node)[:2] == ("not_live", "lease_expired")
    _control(control_url, "/config", {"rate": 0.0})

    _wait_until(lambda deadline: _lease_row(node, timeout=deadline.remaining())[0] == "live", timeout=40)
    delta = _event_delta(counters_before, _profile_events(node))

    assert _writer_epoch(node) == epoch_before
    assert delta["CASMountLeaseExpired"] == 1, delta
    assert delta["CASMountLeaseLost"] == 0, delta
    assert delta["CASMountRenewalRetries"] > 0, delta
    assert delta["CASRemountAttempts"] == 0, delta
    assert _text_log_count(node, "CasMountLeaseRenewer", "fenced after", since) == 0
    assert _text_log_count(node, "CasPool", "whole-chain remount attempt", since) == 0

    expired_ms = _wait_until(lambda deadline: _expired_ms_values(node, since), timeout=20)
    assert len(expired_ms) == 1 and expired_ms[0] > 0, expired_ms

    node.query("INSERT INTO renewal_probe VALUES (302, 'after-long-outage')")
    assert node.query("SELECT count() FROM renewal_probe WHERE id = 302").strip() == "1"
```

Notes on the tests, each checked against the fixture:
- The short test's arithmetic: the renewal that committed just before the outage holds the lease until start + 10 s. The outage ends 6 s after it was armed, well before. A request in flight at the end of the outage lands within one retry spacing (about 1 s).
- The long test polls for `lease_expired` instead of assuming when it starts: the lease expires about 10 s after the last committed renewal started, the outage lasts 11 s, and the hold sleep only pads the remainder. If the host is so slow that the state is not reached before the 40 s deadline, the test fails loudly rather than passing.
- The error assertion checks the transient class (`NETWORK_ERROR`, code 210) and the word "lease" only: the expiry is worded in `CasMountRuntime::checkFenceOrThrow`, while the request engine and the ref ledger keep their own texts ("no lease budget", "gave up at the lease deadline", "refusing to append ref-log transactions").
- Ids 101, 301 and 302 do not collide with the existing tests (`id = 0` only).
- Neither test depends on the other; each resets the proxy first and waits for `lifecycle = 'live'`.

- [ ] **Step 4: How to run, and what red looks like**

The binary must be at `ci/tmp/clickhouse`; praktika downloads a master binary otherwise. Put the worktree build there and check it:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build clickhouse > build/ninja_clickhouse_t6.log 2>&1; echo NINJA_EXIT=$? >> build/ninja_clickhouse_t6.log
tail -1 build/ninja_clickhouse_t6.log
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse && ls -la ci/tmp/clickhouse
```

Then run the whole module, foreground, with the longest tool timeout (the module takes several minutes, mostly the two hard restarts):

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && python3 -m ci.praktika run "integration" --test test_cas_mount_renewal_retry > build/it_cas_mount_renewal_retry.log 2>&1; echo IT_EXIT=$? >> build/it_cas_mount_renewal_retry.log
grep -a "passed\|failed\|PASSED\|FAILED\|IT_EXIT" build/it_cas_mount_renewal_retry.log | tail -20
```

Use one `--test` flag. Never claim green from a single test: the whole module must pass.

Red run: only meaningful on a binary built before Tasks 4 and 5. Expected on that binary:
- `test_short_put_outage_does_not_fence` fails at the first `assert _text_log_count(node, "CasMountLeaseRenewer", "fenced after", since) == 0` (prints `assert 1 == 0`): the bounded renewal gives up about 4.5 s after the outage starts and fences.
- `test_long_put_outage_resumes_under_the_same_epoch` fails with `AssertionError: condition did not become true within 40s; last=None` in `expired`: the mount is fenced and remounts, so `lifecycle_reason` never becomes `lease_expired`.

If the only available binary already contains Tasks 4 and 5, say so in the report: the red evidence for these two tests then comes from the unit-test reds of Tasks 4 and 5, and this step records the green run only.

- [ ] **Step 5: Documentation edits**

All headings below keep their existing `{#anchor}`. No new heading is added. Names are written without `()`.

**`docs/en/antalya/cas/architecture/mounts-and-leases.md`**

(a) Replace the whole bullet that starts `- **Absolute deadline.**` (it ends `... never extends authority.`) with:

```markdown
- **Lease deadline.** The lease is valid for `cas_mount_lease_ttl_ms` from the start of the last confirmed
  renewal. It is measured on `CLOCK_BOOTTIME`, not `CLOCK_MONOTONIC`, so a VM resumed from suspend
  correctly observes itself expired. The deadline decides about writes: none is admitted past it. The
  background renewal does not stop at the deadline. It retries timeouts, `5xx` answers and connection
  errors about a second apart until the store answers. The renewals at startup and after a remount stay
  bounded: they stop at the last confirmed deadline minus the safety margin. A retry, `GET`, response
  timestamp, or wall-clock step never extends authority.
```

(b) Replace the paragraph that starts `**Losing the lease is neither read-only mode nor a process abort.**` (through `... may retry only inside authority already proved by the last confirmed lease.`) with:

```markdown
**A transient renewal failure does not end the mount.** `MountLeaseRenewer` is a synchronous
durable-slot state machine. A committed result advances its token, sequence, confirmed BOOTTIME
deadline, and cadence anchor. The background renewal moves it to `RenewalTerminal` only on a definitive
answer: a confirmed foreign, successor or same-pair body, `gc_fenced`, an absent object, a request the
store refuses for good, or a stop or remount request. A terminal renewer cannot mint another body or
publish a clean farewell. Owner cancellation before any request is the only `NotAttempted` result and
leaves clean release possible. Cancellation after a request was sent is terminal because that request
may still land.

While retries continue past the lease deadline, the lease is *expired*. New durable writes are refused
with an error that says the lease expired, reads are not gated, and the pool does not remount. When a
renewal succeeds, writes are admitted again under the same `writer_epoch`. `system.cas_mounts` shows the
expired period as `lifecycle = 'not_live'`, `lifecycle_reason = 'lease_expired'`
(see [`system.cas_mounts`](#mounts-table)).

After the renewer call returns, `CasMountRuntime` consumes the result. A terminal result trips the
local fence (latches `lost`, bumps the fence generation, moves the in-process runtime to
`TransientNotLive`) and latches one self-remount generation. A confirmed foreign/successor or
same-pair conflict remains a typed fail-closed error; it is never adopted. A real fence still costs
only an epoch: recovery reclaims with a fresh one, bounded at three whole-chain attempts. This is the
general CAS posture: doubt about the source fails closed, while transport ambiguity may retry only
inside authority already proved by the last confirmed lease.
```

(c) In the paragraph that starts `GC's own view of a dead server is symmetric`, insert after its first sentence (the one ending `for \`TTL + floor(TTL/20) + period\`` and its dash clause, i.e. after `... \`TTL + floor(TTL/20) + max(1, floor(period/2))\`.`) this sentence:

```markdown
GC starts counting from a clock sample taken after the read that returned the token, so time the
round spent on a slow `LIST` or `GET` before that read is not credited as time spent watching.
```

(d) In the paragraph that starts `Every server sharing a pool must therefore run the identical`, replace the last sentence (from `With the defaults (TTL 30 s, period 10 s,` to the end of the paragraph) with:

```markdown
With the defaults (TTL 30 s, period 10 s, margin 2 s), the startup check
`period + 2 × envelope + margin < TTL` leaves `TTL − margin − period − 2 × envelope = 4 s` of slack: a
renewal that starts up to 4 s late still leaves time to admit a write before the next one. A later or
failing renewal does not fence the mount. Writes are refused only once the TTL has passed. Here
`envelope = attempt_timeout + 2 × cap` and `cap` is `attempt_timeout` when the disk's
`connect_timeout_ms` is `0`, else `min(connect_timeout_ms, attempt_timeout)` (7 s with defaults).
```

(e) In the `PoolLifecycle` diagram, replace the edge label `renewal failure, tripMountLost, lost=true` with `terminal renewal result, tripMountLost, lost=true`. (Part 2 reworks the rest of the diagram and the two-worker text; leave them.)

(f) In the `system.cas_mounts` table, replace the `lifecycle`, `lifecycle_reason`, `lifecycle_detail`, `lifecycle_since` row with:

```markdown
| `lifecycle`, `lifecycle_reason`, `lifecycle_detail`, `lifecycle_since` | the SQL surface for the in-process `PoolLifecycle` runtime above: `lifecycle` is one of `live`, `not_live`, `identity_lost`, `vanished`, `constructing`, `shutdown`; `lifecycle_reason` distinguishes `replaced` from `forgotten` for a `vanished` disk, and is `lease_expired` for a `not_live` disk whose lease deadline has passed while renewals keep failing; `lifecycle_detail` carries the full diagnosis text (for `lease_expired`, the text of the last failed renewal request); `lifecycle_since` is when the current non-live state began (for `lease_expired`, the passed deadline; `NULL` while live) |
```

and add this paragraph right after the table, before `The lifecycle snapshot is I/O-free and ungated`:

```markdown
`lifecycle_reason = 'lease_expired'` reports this server's own confirmed lease deadline. The `state`
column is a different signal: it is derived from the body's wall-clock `expires_at` plus a skew
allowance. The two need not agree.
```

**`docs/en/operations/system-tables/cas_mounts.md`** (exists; headings and anchors untouched)

Replace the `lifecycle_reason` bullet with:

```markdown
- `lifecycle_reason` ([String](/sql-reference/data-types/string)) — The enum-clean sub-state word: `replaced` or `forgotten` for a `vanished` disk, and `lease_expired` for a `not_live` disk whose own confirmed lease deadline has passed while renewals keep failing (writes are refused until a renewal succeeds; the writer epoch does not change). Empty for every other lifecycle, so `lifecycle || '(' || lifecycle_reason || ')'` reads e.g. `vanished(forgotten)`.
```

Replace the `lifecycle_detail` bullet with:

```markdown
- `lifecycle_detail` ([String](/sql-reference/data-types/string)) — The full typed reason text naming the actual cause when not live: the vanish diagnosis (a data root replaced by a foreign pool, or decommissioned by `SYSTEM CAS FORGET` at a given time), the identity-loss message, or, for `lease_expired`, the text of the last failed renewal request. Empty when live.
```

Replace the `lifecycle_since` bullet with:

```markdown
- `lifecycle_since` ([Nullable(DateTime)](/sql-reference/data-types/nullable)) — When this server entered the current non-live lifecycle state; for `lease_expired`, the lease deadline that passed. `NULL` when live, or when the state has no backing pool to date from.
```

Add after the paragraph that starts `` `lifecycle`/`lifecycle_reason` ``:

```markdown
`lifecycle_reason = 'lease_expired'` is about this server's own confirmed lease deadline; it differs from `state = 'expired'`, which is derived from the body's wall-clock `expires_at` plus a skew allowance.
```

**`docs/en/antalya/cas/configuration.md`**

(a) In the `cas_mount_lease_ttl_ms` table row, replace the description with:

```markdown
Milliseconds for which a mount lease remains valid after the start of a successful claim or renewal (≥ 1). Writes are never admitted past it. The background renewal keeps retrying after it until the store answers, so an outage longer than the TTL refuses writes but does not fence the mount. Lower values shorten stale-mount recovery but shorten the outage that writes ride out
```

(b) In the paragraph that starts `All servers sharing a pool must run the same`, insert after the sentence that ends `... \`cas_mount_renew_period_ms\`.` (the GC threshold sentence) this sentence:

```markdown
GC counts the observation from a clock sample taken after the read that returned the token, not from the start of its round.
```

(c) Replace the paragraph that starts `A shorter TTL reduces the tolerance for object-storage delays;` (through `... until \`confirmed deadline − cas_lease_safety_margin_ms\`.`) with:

```markdown
A longer TTL lengthens the outage that writes ride out without a refusal. A shorter renewal period starts
each renewal earlier, so more of the TTL is left when one fails, at the cost of more background traffic.
With the defaults, `cas_mount_lease_ttl_ms − cas_lease_safety_margin_ms − cas_mount_renew_period_ms −
2 × envelope = 4000` ms is the slack the startup check leaves: a renewal that starts up to that late
still leaves time to admit a write before the next one. Here `envelope = cas_attempt_timeout_ms + 2 × cap`
(7000 ms with defaults) and `cap` is `cas_attempt_timeout_ms` when the disk's `connect_timeout_ms` is `0`,
else `min(connect_timeout_ms, cas_attempt_timeout_ms)` (1000 ms with defaults). The background renewal
retries, about a second apart, until the store answers. It does not stop at the lease deadline. New
writes stop being admitted shortly before the TTL runs out and are refused once it has.
```

The `cas_lease_safety_margin_ms` row and the keep-alive section stay as they are.

**`docs/en/antalya/cas/operations/troubleshooting.md`**

(a) Add this row to the symptom table, directly after the row `A server keeps losing its mount lease and self-remounting`:

```markdown
| Writes fail with a `lease expired` error and recover on their own | `system.cas_mounts` shows `lifecycle = 'not_live'`, `lifecycle_reason = 'lease_expired'` for the disk; `lifecycle_detail` is the text of the last failed renewal request. `CASMountLeaseExpired` in `system.events` counts the expiries that ended, and the `watermark_renew` row that ended one carries `expired_ms` in `detail` | The server could not renew its lease for longer than `cas_mount_lease_ttl_ms`, so it refuses writes until a renewal succeeds; it keeps the same `writer_epoch` and does not remount. Fix the object-store path named in `lifecycle_detail` (reachability, throttling, credentials). `CASMountRenewalAttempts` and `CASMountRenewalRetries` keep rising during the outage. If `CASMountLeaseLost` rises as well, follow [the decision flow](#mount-renewal-remount-flow) |
```

(b) In the decision flow, replace the first sentence of item 2 (`**External lease-safety exhaustion.** The failed row has ... rise.`) so the item reads:

```markdown
2. **External lease-safety exhaustion.** Only the renewals at startup and after a remount are bounded by
   the lease; the background renewal is not, so this case means one of those ran out of lease. The failed
   row has `classification = 'external_lease_deadline'`; `CASMountRenewalDeadlineExceeded` and
   `CASMountLeaseLost` rise. The runtime correctly refused to manufacture authority beyond the last
   confirmed lease. Check object-store latency and BOOTTIME/suspend history, then follow the ensuing
   remount. `classification = 'request_deadline'` is the sibling case: the ninety-second request
   policy exhausted first rather than the lease's own safety margin.
```

(c) Insert a new item after item 1 and renumber nothing by using a lettered item, so the existing numbers other pages cite stay valid:

```markdown
1a. **Expired lease.** `CASMountLeaseExpired` rises by one when a renewal restores a lease that had
   expired; `CASMountLeaseLost` and the remount counters stay flat. The restoring `watermark_renew` row
   has `expired_ms` in `detail`: how long writes were refused. The server log has a `WARNING` with the
   same duration and the last failure. While the outage lasts, the same state is visible in
   `system.cas_mounts` (`lifecycle_reason = 'lease_expired'`).
```

(d) Replace the last paragraph (`The default-level log policy is intentionally bounded: ...` through `... when that extra transport detail is necessary.`) with:

```markdown
The default-level log policy is intentionally bounded: one warning on the first transition to retry,
then one recovery info, terminal fence warning or expired-lease warning, plus one final line per
whole-chain remount attempt. Individual physical retries are not logged. Use `system.cas_log` and the
counters to reconstruct the incident: `CASMountRenewalAttempts` and `CASMountRenewalRetries` advance as
requests are sent, so an outage shows while it lasts.
```

Not changed, and why: `docs/en/antalya/cas/operations/debugging.md` also mentions `external_lease_deadline` (the `watermark_renew` classification list); it stays true for the bounded renewals and is outside this task's three files. The implementer should `grep -n "external_lease_deadline" docs/en/antalya/cas/operations/debugging.md` and leave it unless the sentence says the background renewal produces it.

Check after editing: `grep -n "4 s\|4000\|Absolute deadline\|Losing the lease" docs/en/antalya/cas` shows only the new `4 s` / `4000` slack sentences, and `git diff --stat` lists exactly the four doc files.

- [ ] **Step 6: Write `tmp/pr-body.md`**

Content (the template's CI options block is copied verbatim from `.github/PULL_REQUEST_TEMPLATE.md` after the documentation entry):

```markdown
CAS mount lease renewal no longer fences the mount after a short object-store outage. The background renewal retries until the store answers; the lease TTL alone decides when writes stop, and they resume under the same `writer_epoch` when a renewal succeeds. GC now counts its fence-out wait from after the read that returned the token, and a memory limit no longer fails the lease thread.

Closes: https://github.com/Altinity/ClickHouse/issues/2431
Closes: https://github.com/Altinity/ClickHouse/issues/2403

### Changelog category (leave one):
- Bug Fix (user-visible misbehavior in an official stable release)

### Changelog entry (a user-readable short description of the changes that goes to CHANGELOG.md):
Fix a CAS mount lease being fenced, with a remount under a fresh epoch and a long wait, after a short object-store outage on the lease renewal. The renewal now retries until the store answers. Writes are refused only while the lease is actually expired and resume under the same `writer_epoch`. Add the `CASMountLeaseExpired` event and the `lease_expired` lifecycle reason in `system.cas_mounts`. Fix GC counting its wait for a mount token from before it saw the token, and the memory tracker failing the lease thread.

### Documentation entry for user-facing changes
- [x] Documentation is written (mandatory for new features)

Updated `docs/en/antalya/cas/architecture/mounts-and-leases.md`, `docs/en/antalya/cas/configuration.md` and `docs/en/antalya/cas/operations/troubleshooting.md`.
```

Then the `### CI/CD Options` block exactly as in the template. Copy it with `sed -n '/### CI\/CD Options/,$p' .github/PULL_REQUEST_TEMPLATE.md >> tmp/pr-body.md`. No session link and no generated-by line, per the repository owner's rule for PR text. `tmp/` is untracked scratch; do not `git add` it.

- [ ] **Step 7: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- tests/integration/test_cas_mount_renewal_retry/test.py
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Test CAS mount renewal across a short and a long mount PUT outage" -m "A 6 s outage that covers a renewal start must not fence; an outage longer than the TTL expires the lease, refuses writes and resumes under the same writer_epoch with no remount." -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- docs/en/operations/system-tables/cas_mounts.md docs/en/antalya/cas/architecture/mounts-and-leases.md docs/en/antalya/cas/configuration.md docs/en/antalya/cas/operations/troubleshooting.md
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Document CAS mount renewal without a lease deadline" -m "The background renewal retries until the store answers; an expired lease refuses writes and is reported in system.cas_mounts, CASMountLeaseExpired and expired_ms." -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

Exact files only. No amend, no rebase, no push.

**Deviations from the brief**

- The fixture has one node with GC disabled, not two nodes and a GC leader. "On every node" became "on `node`"; the GC-fence reasoning is a one-line comment in the long test only.
- The long test waits for `lease_expired` by polling, then holds until TTL + half a period has passed since arming, instead of a fixed 11 s window. A fixed window leaves about 1 s of slack against the expiry time on a slow host.
- `expired_ms` is looked up as "the one `watermark_renew` row since the marker that has the key", not "the last row": a later ordinary renewal row would be last and has no `expired_ms`.
- Troubleshooting gets an item numbered `1a`, not a renumbering, so references to items 2 to 6 stay valid.
- Docs: the lifecycle diagram edit is one edge label. The two-worker and parking text, the `cas_lease_safety_margin_ms` row and `debugging.md` are untouched.
- The red run for the integration tests exists only if a pre-Task-4 binary is available; see Step 4.

**Review focus**

1. A GC leader on a second node fencing the slot during the long outage. The fixture cannot show it (GC off); a reasonable person expects the renewal, once the store is back, to read `gc_fenced`, remount and reclaim at once, with `CASMountLeaseLost` rising and the epoch advancing.
2. An outage on `GET` of the mount object while `PUT` works, or a mix of `503` and timeouts. The tests only fault `PUT`/`POST` with `503`; a person expects the same no-fence result because the resolve read is retried with the same spacing.
3. A server restart during the expired period. A person expects the restart to reclaim through the token-stability observation as today, not to resume the old epoch; no test here covers it.


---

## Self-Review

**Spec coverage.** "Step one" and rule 11: Task 1. "Renewal under memory pressure" and rule 12: Task 2. Rule 8: Task 3 (engine) and Task 4 (renewer level). Rules 2 to 7 for the worker, the catch-up of rule 10: Task 4. "What you see", rule 9 and the restore edge of rule 10: Task 5. Spec tests 18 and 19 and "Documentation": Task 6. Rules 1, 13, 14, "Simpler code", "Further cleanups" and spec tests 14 to 17 are not in this plan; they are part 2. Spec test 20 (soak card S39 on every replica) lives outside the code worktree and is reported, not implemented, by this plan.

**What part 1 deliberately keeps.** `renewalLive` still ends the worker's renewal on a lost fence: `Pool::forgetDisk` relies on it, and `CASMountRuntime.ForgetEndsAnUnboundedRenewal` (Task 4) pins it. The refusal text changes only in `CasMountRuntime::checkFenceOrThrow`. A store refusal that follows an unclear request keeps retrying; `CASHeartbeat.ARefusalAfterAnUnclearAttemptIsRetriedUntilStopped` (Task 4) pins it.

**Type consistency.** `MountLeaseRenewer` takes `worker_requests_` as its third plane; `CasMountRuntime` and `Pool` call the same plane `lease_requests` (Task 4 defines both, Task 5 consumes `lease_requests_`). `CasOperation::RequestObserver` / `setRequestObserver` are Task 4's and are not named by Task 5, which sees only `MountRenewRequestEvent` through `MountRenewOperationEnvironment::on_request`. The event contract is the same in both tasks: one `failed = false` event per `PUT` sent, one `failed = true` event per `PUT` that throws and per failed resolve read; `noteRenewRequest` counts on `failed = false` only. `Retry::untilDefinitive(uint64_t)` is produced by Task 3 and consumed by Task 4 with `kMountRenewRetrySpacingMs`. `reportMountRenewCompletion` gains `expired_ms` in Task 5 only.

**Hunks are written against the base commit.** Every task section quotes code and line numbers from `41824f955e7`. Tasks 4 and 5 edit functions that earlier tasks change (`CasRequests.{h,cpp}` after Task 3, `renewalLoop` after Task 2, `CasServerRoot.{h,cpp}` after Task 1, `renewalEnvironment` and `consumeRenewResult` after Task 4). Their implementers apply the change to the code as it is at their start and report any hunk whose surroundings moved; the names and signatures above are the contract.

**Placeholder scan.** No TBD. The only `TODO` is the temporary `TODO-CYCLE-2` marker of Task 1, which its Step 5 removes before the first commit and greps for.

**Review Focus.** The five items each name the task and the test that pins them. Behaviour not pinned by a test, recorded for the final review: the rebuild pass of GC calling `computeHeartbeatFloor` (Task 1 changes the call; no GC-rebuild fixture drives a slow walk through it); a multipart conditional `PUT` now uploading its parts one after another (Task 2); an unwrapped `std::bad_alloc` staying terminal (Task 2).
