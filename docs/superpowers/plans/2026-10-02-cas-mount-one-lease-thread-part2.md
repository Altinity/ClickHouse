# CAS Mount Lease: One Lease Thread and One Renewal Path (Part 2) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One thread per writable CAS mount owns the lease: it renews and reclaims in sequence, arms the fence under one rule, and reports each renewal from its returned result. The two-worker parking protocol, the bounded re-anchor renewals and the thread-local observability stack go, with no change to any guarantee.

**Architecture:** Five tasks in strict order, each leaving the tree building and the `CAS*` gate green. (1) The reclaim moves into the lease loop behind `beginReclaim` and `armIfAdmissible`; the nine driver states and the remount worker go. (2) Readiness goes through the loop: a claim that does not admit a ref append leaves the fence latched and the next renewal arms it; a renewal ends on a stop, a pending remount request or a terminal lifecycle, not on a lost fence. (3) The re-anchors, the lease-bound policy and the fence-gated renewal plane are deleted. (4) `renew` returns what its report needs; the thread-local stack and the renewer's copy of the deadline go. (5) One definition per duplicated quantity and nine small cleanups.

**Tech Stack:** C++23 (ClickHouse, Allman braces), gtest (`unit_tests_dbms`), pytest integration tests through `ci.praktika`, TLA+ model `CaDiskLifecycle` (already describes the target loop; `docs/superpowers/models` on `cas-gc-rebuild`).

**Spec:** `/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-10-01-cas-mount-renewal-without-lease-deadline-design.md` on `cas-gc-rebuild`. This plan implements its "Order of work" steps 4 to 8, rules 1, 7 (as written), 13 and 14, "Simpler code" and "Further cleanups", and spec tests 14 to 17. Steps 1 to 3 are done (`docs/superpowers/plans/2026-10-02-cas-mount-renewal-without-lease-deadline-part1.md`).

**Contract:** `/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/plans/2026-10-02-cas-mount-one-lease-thread-part2-contract.md` on `cas-gc-rebuild` maps the design onto the code (today's shape, the target shape after each task with exact declarations, the test inventory) and ends with the rulings on its open points. It is binding for every task. The contract's ids T4, T5a, T5b, T6, T7+8 are this plan's Tasks 1 to 5.

**Worktree:** `/home/mfilimonov/workspace/ClickHouse/cas-95-1`, branch `refactor/antalya-26.6/cas-mount-one-lease-thread`, created from the head of part 1 (`7e243502408`, branch `fix/antalya-26.6/cas-mount-renewal-no-deadline`). Release build dir `build/`. The base passes the gate: 2604 tests in the `CAS*` suites, and the integration module `test_cas_mount_renewal_retry` (6 tests).

`CA` below means `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`.

## Global Constraints

- Allman braces in all C++ (CI style check).
- Build: `cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/<log> 2>&1; echo NINJA_EXIT=$? >> build/<log>`. No `-j`, no `nproc`. Read the last line for `NINJA_EXIT=0`; on failure `grep -a -n "error:" build/<log> | head -40`.
- Every gtest run: `cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='<filter>' > build/<log> 2>&1; echo GATE_EXIT=$? >> build/<log> )`. Read `GATE_EXIT=` and `grep -a "FAILED\|PASSED" build/<log> | tail -20`. `GATE_EXIT=124` is a hang: the last `[ RUN ]` line names the test.
- Long commands run in the foreground of the agent that needs them, with the maximum tool timeout. Never start one in the background and wait for a notification.
- The gate filter is EXACTLY `CAS*`. Every new suite name starts with `CAS`. A task is done only when the whole gate passes on its head commit, with the binary newer than the last source edit (`stat -c %Y build/src/unit_tests_dbms` against the edited files).
- Tests fail first: a task that adds behaviour writes its tests, builds, and records the red run before implementing. A test that guards behaviour which already holds is marked as a regression test and its green-on-base result is recorded. A deletion-only cycle is: delete, adapt the listed tests, gate.
- A test must fail, never hang: every fixture loop that waits for a renewal is bounded by request count and fails with a message at the bound. No `sleep` for ordering, in code or tests, and no dependence on real time: order with the injected clock, scripted backends, barriers and existing hooks.
- Hooks capturing test locals: declare the captured locals BEFORE the `Pool`, runtime or backend that stores the hook.
- `LOGICAL_ERROR` expectations are death tests whose child exits through `std::_Exit`; never `EXPECT_THROW` on a `LOGICAL_ERROR` site.
- Tests are deleted only where their subject becomes unreachable; a test whose guarantee stays reachable is adapted. The contract's section 4 is the list; a deviation from it is reported with the reason.
- No fallback paths: a failure propagates or is retried by the rule in the spec; nothing substitutes a default.
- No new setting. The on-store format does not change. Write admission of data writes (`admit`, `mayMutate`, `refAppendFenceOk`, the envelope reservation of every write), the claim protocol, the farewell's lease-bound policy (`Retry::untilLeaseSafe`), the GC leader lease and the data-write request engine do not change.
- Lock rule: `driver_mutex` is never held across a request, a reclaim, a log line, a callback or a join. `remount_mutex` → `driver_mutex` is the only nesting between them; `driver_mutex` → `renew_failure_mutex` is the only nesting below it.
- Changes outside `CA/` and `src/Disks/tests/` are limited to: `src/Common/setThreadName.h` (one entry), `src/Common/ProfileEvents.cpp` (one entry), `src/Core/` or `CA/` setting descriptions named by Task 5, `docs/en/antalya/cas/`, `docs/en/operations/system-tables/cas_mounts.md`.
- Commits: `git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- <exact files>` then `git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "<subject>" -m "<body>" -m "Co-Authored-By: <model name> <noreply@anthropic.com>"`. Exact file paths only, never a directory, never `-A`. No rebase, no amend, NO push. No session links in commit messages.
- Prose (comments, docs, commit messages) follows `~/.claude/skills/tech-prose/STYLE.md`. Comments give the reason, never history, plan, spec, rule number, backlog, review, issue or task references. Names of SQL identifiers, classes and functions go in backticks; functions written as `f`, not `f()`. Say "exception", not "crash", for logical errors. "ASan", not "ASAN". Docs headings keep an explicit `{#anchor}` and describe what is.
- Implementers do not dispatch subagents.

## Review Focus

Failure modes the spec implies and no task's tests exercise, most likely first. Reviewers of the named task check each by reading; the final review returns to all five.

1. **A stop or a FORGET while a reclaim sits in its observation wait** (Task 1). The join waits for `claimMountAwaitingExpiry` to return, up to the observation threshold plus one request, as it did for the remount worker. Expected: shutdown completes within that bound and writes no farewell for the unclaimed slot. `CASMountRuntime.StopDuringAReclaimJoinsTheThread` pins the runtime's side with a scripted reclaim; nothing holds a real observation wait.
2. **An interference report during a reclaim's quiesce** (Task 1). `reportImpossibleInterference` takes `driver_mutex` from the ref ledger while the lease thread holds `remount_mutex` and takes ledger locks to quiesce. Expected: no deadlock, because the lease thread never holds `driver_mutex` while taking `remount_mutex` or a ledger lock. Whether a ledger lock is held at that call is to be read by the reviewer of Task 1.
3. **A renewal that restores an expired lease right after a reclaim** (Tasks 1 and 2). The expiry bookkeeping (`lease_expired_at_boot_ms`, the failure text, `CASMountLeaseExpired`) must not count an expiry for a lease that a reclaim replaced. Expected: `armFence` resets the expired instant; the assertions added to the readiness tests cover the open and the remount, not a reclaim that follows a long outage.
4. **The test seam after Task 3.** `renewWatermarkOnce` has no request bound: a test that arms a persistent transient fault on the mount slot retries until the gate's `timeout` ends the binary. Expected: fixtures give a definitive answer or bound liveness; the sweep in Task 3 lists every existing site, and a new one is a reviewer's catch.
5. **A scheduled GC round still steals after cleanup 8.4** (Task 5). `allow_steal` follows from the trigger. A unit test pins that a manual round never steals; the mutation probe of 8.4 is the only check that a scheduled round does.


---

### Task 1: One lease thread (contract T4; spec rules 7, 13, 14)

The CAS mount lease keeper goes from two worker threads with a parking protocol to one lease thread
that renews and reclaims in sequence. A reclaim starts by latching the fence (`beginReclaim`) and
arms only under the arming rule (`armIfAdmissible`). The nine `RenewalDriverState` values,
`DriverLease`, `admitRenewerCall` and the remount worker go. The startup, remount and direct
renewals stay as plain calls (`RenewCaller::Startup`, `Remount`, `Direct`); Task 3 removes them.

Paths: `W` = `/home/mfilimonov/workspace/ClickHouse/cas-95-1` (branch
`refactor/antalya-26.6/cas-mount-one-lease-thread`, HEAD `7e243502408`),
`CA` = `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`, `R` = `CA/Pool/CasMountRuntime.cpp`,
`RH` = `CA/Pool/CasMountRuntime.h`, `P` = `CA/Pool/CasPool.cpp`, `PH` = `CA/Pool/CasPool.h`,
`SH` = `CA/Pool/CasServerRoot.h`, `T` = `src/Disks/tests`. Line numbers are from `W` at `7e243502408`,
each with its function; the function name is the anchor.

**Files:**
- Modify: `RH` (`RenewalDriverState` removed; `MountConfig` hooks; public `beginReclaim`, `armIfAdmissible`; private `RenewCaller`, `RestoredLease`, `canArm`, `armFence`, `checkRenewerOwner`; members `reclaim_generation`, `lease_thread_id`; comments that describe two workers or parking)
- Modify: `R` (`armMountFence` :258, new `armFence` / `beginReclaim` / `armIfAdmissible` / `canArm` / `checkRenewerOwner`; `renewWatermarkOnce` :287; everything from `DriverLease::DriverLease` :357 to `stopBackgroundWorkers` :952; `scheduleRemount` :1161, `beginShutdownForTest` :1182, `renewalDriverStateForTest` :1190, `waitForRenewalDriverStateForTest` :1196, `workersRunningForTest` :1206)
- Modify: `P` (`Pool::tryRemountOnce` :1254 step 0 and arm; `Pool::forgetDisk` :1146 chassert and comments; comments in `Pool::mountWritable` :915, `Pool::~Pool` :1024 and above `Pool::scheduleRemountForTest` :1598)
- Modify: `PH` (comments at :185, :552-565, :829-831, :1178-1180, :1214-1217, :1259-1266, :1278-1281)
- Modify: `SH` (two comments, :86 and :554) and `CA/Pool/CasServerRoot.cpp` (two comments, :150 and :1525 in `MountLeaseRenewer::throwRenewConflict`); see Deviations
- Modify: `src/Common/setThreadName.h` (:41, `CAS_REMOUNT` removed)
- Modify: `docs/en/antalya/cas/architecture/mounts-and-leases.md` (`#mount-state-machines` closing paragraph, `#mount-lifecycle` first three paragraphs)
- Test: `T/gtest_cas_pool.cpp`, `T/gtest_cas_forget.cpp`

**Interfaces:**
- Consumes: the tree at `7e243502408`.
- Produces (binding names, used by Tasks 2 to 5):
  - `RH` public: `void beginReclaim();` `bool armIfAdmissible(uint64_t deadline_boot_ms);`
  - `RH` private: `enum class RenewCaller : uint8_t { Loop, Startup, Remount, Direct };`
    `MountRenewResult renewRenewerOnce(RenewCaller caller);`
    `MountRenewOperationEnvironment renewalEnvironment(RenewCaller caller);`
    `bool renewalLive(RenewCaller caller) const;`
    `void consumeRenewResult(const MountRenewResult & result, RenewCaller caller);`
    `bool canArm(uint64_t deadline_boot_ms) const;` (requires `driver_mutex`; the parameter is unread in this task)
    `void armFence(uint64_t deadline_boot_ms, bool report_live);`
    `void checkRenewerOwner() const;` (requires `driver_mutex`)
    `struct RestoredLease { uint64_t expired_ms; String last_failure; };`
    `std::optional<RestoredLease> publishRenewedDeadline(uint64_t deadline_boot_ms);` (requires `driver_mutex`)
    `uint64_t reclaim_generation;` `std::thread::id lease_thread_id;` (both guarded by `driver_mutex`)
  - `MountConfig::lease_wait_predicate_false_hook_for_test` (replaces `renewal_parked_predicate_false_hook_for_test`; called in all three waits of the loop).
  - Unchanged signatures: `renewWatermarkOnce`, `renewRenewerForStartupOnce`, `renewRenewerForRemountOnce`, `startRenewer`, `installRenewer`, `renewerReset`, `startBackgroundWorkers`, `stopBackgroundWorkers`, `scheduleRemount`, `finishTeardown`, `armMountFence`, `workersRunningForTest`, `remountRequestedGenerationForTest`.
  - `Pool::tryRemountOnce` returns `true` for a reclaim that claimed and did not arm (C10, ruling O6 first sentence). The event's step name and text for that case land in Task 2.

**Enumeration of the removed symbols.** Derived with
`git -C $W grep -n -E "\b(RenewalDriverState|renewalDriverStateForTest|waitForRenewalDriverStateForTest|remount_parked_hook_for_test|renewal_terminal_deposited_hook_for_test|renewal_parked_predicate_false_hook_for_test|DriverLease|AdmittedRenewerCall|admitRenewerCall|renewalWorkerMayRenew|remountLoop|remount_worker|renewal_driver_state|workers_starting|worker_loops_released|CAS_REMOUNT)\b" -- src docs`,
untruncated. Hits by file: `R` 127, `RH` 25, `P` 1 (the `forgetDisk` chassert), `setThreadName.h` 1,
`T/gtest_cas_pool.cpp` 11, nothing in `docs`. The test hits sit in exactly these tests:
`DueWorkerAdmissionIsReservedBeforeParkRequest`, `DueWorkerAdmissionIsReservedBeforeStop`,
`RemountWaitsForRenewalParkedBeforeReplacement`, `ParkedRenewalCannotMissNaturalTerminalPublication`,
`WorkerConstructionRollbackFailsOpenClosed` (`workersRunningForTest`, which stays),
`TerminalDepositionDoesNotTouchRenewerAfterReplacement`, `ParkEndsAnUnboundedRenewal`. Re-run the grep
after cycle 2: it must return nothing outside `docs/superpowers`.

**Callers whose behaviour moves.** `git -C $W grep -n tryRemountOnce -- src/Disks/tests` returns
37 code lines in 9 files. `beginReclaim` now trips the fence and bumps the generation at step 0 where
`noteLeaseLost` did neither, and a successful reclaim bumps the generation twice instead of once. I
read every site: none asserts `mayMutate() == true` or an exact generation after a failed reclaim, and
none asserts an exact generation delta across a successful one. The two sites that run with a stop or
FORGET intent at arm time are `CASForget.ForgetReLatchesFenceAfterAReclaimReachesArmMountFence`
(adapted, cycle 1b) and `CASForget.ForgetIntentBlocksNaturalReplacedPromotion` (never reaches the arm).
Only `CASLifecycleCondition.RemountThreadSelfExitsOnceIdentityLost` calls `tryRemountOnce` directly
while a lease thread runs; it ends at the identity probe and never reaches `installRenewer`.

**Lock rules this task adds.** Every new critical section holds `driver_mutex` and touches atomics,
plain fields guarded by `driver_mutex`, and no-trace counters only: no I/O, no log, no join, no
callback other than a `_for_test` hook. New nesting: `driver_mutex` -> `renew_failure_mutex` inside
`publishRenewedDeadline`, whose `LOG_WARNING` moves after the unlock. `remount_mutex` -> `driver_mutex`
(`tryRemountOnce` calling `beginReclaim` / `armIfAdmissible`) is the existing order. The lease thread
never holds `driver_mutex` while it calls `remount_attempt`, a renewer method or a backend.

**Cycles.**

| Cycle | Content | Red run |
|---|---|---|
| 1a | `armFence` extracted; `beginReclaim` and `armIfAdmissible` with today's semantics; `tryRemountOnce` calls them | none: no behaviour change |
| 1b | the latch and the arming rule; four new tests, one adapted forget test | three new tests and the forget test fail on 1a; one is a regression test and passes |
| 2 | one thread: the loop takes the reclaim; driver states, remount worker, `CAS_REMOUNT` go | two new tests fail on 1b |
| 3 | `checkRenewerOwner` and `lease_thread_id` | one new test (split for sanitizer builds) fails on 2 |
| 4 | the mounts-and-leases doc | none: docs only |

Cycle 1 is split so that the tests of the arming rule run red against the real `armIfAdmissible`
before it gets the rule, instead of failing to compile.

---

#### Cycle 1a: extract the latch and the arm (no behaviour change)

- [ ] **Step 1: Extract `armFence`, add `beginReclaim` and `armIfAdmissible`, call them from `tryRemountOnce`**

This cycle has no red run: it moves code and keeps behaviour. The gate is the evidence.

`RH`, after the declaration of `armMountFence` (:187), replace

```cpp
    /// Arm a new lease incarnation and clear any loss latched for the prior incarnation.
    void armMountFence(UInt128 server_uuid, uint64_t writer_epoch, uint64_t deadline_boot_ms);
```

with

```cpp
    /// Arm a new lease incarnation and clear any loss latched for the prior incarnation.
    void armMountFence(UInt128 server_uuid, uint64_t writer_epoch, uint64_t deadline_boot_ms);
    /// Step 0 of a reclaim, before its identity probe: move a `Live` pool to `TransientNotLive`.
    void beginReclaim();
    /// End of a reclaim that claimed: publish `deadline_boot_ms`, arm the fence and report `Live`.
    /// Returns whether it armed.
    bool armIfAdmissible(uint64_t deadline_boot_ms);
```

`RH`, private, replace

```cpp
    void tripFenceWithoutOperationalLoss();
```

with

```cpp
    void tripFenceWithoutOperationalLoss();
    /// Publish `deadline_boot_ms` as a fresh lease incarnation and open the fence. With `report_live`,
    /// `Live` is published before the fence opens, so a reader that sees the fence armed reads `Live`.
    void armFence(uint64_t deadline_boot_ms, bool report_live);
```

`R`, replace the whole of `CasMountRuntime::armMountFence` (:258-273) with

```cpp
void CasMountRuntime::armMountFence(UInt128 server_uuid, uint64_t writer_epoch, uint64_t deadline_boot_ms)
{
    mount_fence.server_uuid = server_uuid;
    mount_fence.writer_epoch = writer_epoch;
    armFence(deadline_boot_ms, /*report_live=*/false);
}

void CasMountRuntime::armFence(uint64_t deadline_boot_ms, bool report_live)
{
    mount_fence.deadline_boot_ms.store(deadline_boot_ms, std::memory_order_release);
    lease_expired_at_boot_ms.store(std::numeric_limits<uint64_t>::max(), std::memory_order_release);
    /// A fresh lease incarnation is a fresh generation too: a durable-effect caller admitted under the
    /// PRIOR incarnation must re-check and abort rather than ride this re-arm through.
    fence_generation.fetch_add(1, std::memory_order_acq_rel);
    if (arm_mount_fence_interposition_hook_for_test)
        arm_mount_fence_interposition_hook_for_test();
    if (report_live)
        noteRemounted();
    /// Open the gate LAST. A caller that observes `lost == false` with acquire semantics must also see
    /// the fresh generation; publishing the latch first exposes one admission window in which the dead
    /// generation looks live again.
    mount_fence.lost.store(false, std::memory_order_release);
}

void CasMountRuntime::beginReclaim()
{
    (void)noteLeaseLost();
}

bool CasMountRuntime::armIfAdmissible(uint64_t deadline_boot_ms)
{
    armFence(deadline_boot_ms, /*report_live=*/false);
    noteRemounted();
    return true;
}
```

`armIfAdmissible` keeps today's order here (`lost = false`, then `Live`); cycle 1b moves `Live` first.

`P`, `Pool::tryRemountOnce`, step 0 (:1348-1349), replace

```cpp
    step = "lease_loss_transition";
    mount_runtime.noteLeaseLost();
```

with

```cpp
    step = "lease_loss_transition";
    mount_runtime.beginReclaim();
```

`P`, `Pool::tryRemountOnce`, the arm (:1570-1582), replace

```cpp
        /// No-throw commit section: publish the fence and lifecycle only after epoch, renewer, recovery
        /// cancellation, and ref-runtime quiescence are complete.
        step = "arm_fence";
        mount_runtime.armMountFence(
            our_uuid,
            writer_epoch,
            remount_anchor_boot_ms > std::numeric_limits<uint64_t>::max() - ttl_ms
                ? std::numeric_limits<uint64_t>::max()
                : remount_anchor_boot_ms + ttl_ms);
        step = "publish_live";
        mount_runtime.noteRemounted();
        succeeded = true;
        return true;
```

with

```cpp
        /// No-throw commit section: publish the fence and lifecycle only after epoch, renewer, recovery
        /// cancellation, and ref-runtime quiescence are complete.
        step = "arm_fence";
        const uint64_t deadline_boot_ms = remount_anchor_boot_ms > std::numeric_limits<uint64_t>::max() - ttl_ms
            ? std::numeric_limits<uint64_t>::max()
            : remount_anchor_boot_ms + ttl_ms;
        if (mount_runtime.armIfAdmissible(deadline_boot_ms))
            step = "publish_live";
        succeeded = true;
        return true;
```

`our_uuid` stays used by `installRenewer`; `writer_epoch` by `installRenewer` and `setLiveWriterEpoch`.

- [ ] **Step 2: Build, run the neighbouring suites, then the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t1_c1a_build.log 2>&1; echo NINJA_EXIT=$? >> build/t1_c1a_build.log
tail -1 build/t1_c1a_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASPoolRemount.*:CASMountRuntime.*:CASForget.*:CASLifecycleCondition.*:CASFenceGeneration.*' > build/t1_c1a_tests.log 2>&1; echo GATE_EXIT=$? >> build/t1_c1a_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t1_c1a_tests.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CAS*' > build/t1_c1a_gate.log 2>&1; echo GATE_EXIT=$? >> build/t1_c1a_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t1_c1a_gate.log | tail -20
```

Expected: `NINJA_EXIT=0`; both runs `GATE_EXIT=0`, no `FAILED`. On failure:
`grep -a -n "error:" build/t1_c1a_build.log | head -40`. A red test here means the extraction changed
behaviour: revert the cycle, save the diff under `.superpowers/sdd/task-1/`, report.

- [ ] **Step 3: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Route the CAS reclaim's lease-loss step and fence arm through the runtime" \
  -m "Pool::tryRemountOnce calls CasMountRuntime::beginReclaim at step 0 and armIfAdmissible at the end. armMountFence and the new arm share armFence. No behaviour change." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

#### Cycle 1b: a reclaim starts latched and arms only under the arming rule

- [ ] **Step 4: Write the failing tests**

`T/gtest_cas_pool.cpp`: append these four tests after `TEST(CASMountRuntime, AStaleSuccessIsFollowedAtOnceByTheNextRenewal)`
(ends at :4907), before `TEST(CASPool, RenewWatermarkOnceRefreshesFenceAndDepositsOneFailure)`.

```cpp
/// An interference report raised while a reclaim runs: that reclaim arms nothing, the next one starts
/// with the fence latched, and no write is admitted between the two.
TEST(CASMountRuntime, NothingArmsWhileARemountRequestIsPending)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-nothing-arms-while-pending");
    uint64_t wall_ms = 1000;
    const uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    std::atomic<uint32_t> calls{0};
    bool first_armed = true;
    bool may_mutate_after_first = true;
    bool may_mutate_at_second_entry = true;
    bool may_mutate_after_second_latch = true;
    PoolLifecycle lifecycle_after_second_latch = PoolLifecycle::Live;
    bool second_armed = false;
    bool may_mutate_after_second = false;
    DB::Cas::tests::ManualBarrier second_done;
    CasMountRuntime * runtime_ptr = nullptr;
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            CasMountRuntime & reclaiming = *runtime_ptr;
            if (++calls == 1)
            {
                reclaiming.beginReclaim();
                /// An interference report while this reclaim runs.
                reclaiming.tripMountLost();
                reclaiming.scheduleRemount();
                first_armed = reclaiming.armIfAdmissible(boot_ms + 1000);
                may_mutate_after_first = reclaiming.mayMutate();
                return true;
            }
            may_mutate_at_second_entry = reclaiming.mayMutate();
            reclaiming.beginReclaim();
            may_mutate_after_second_latch = reclaiming.mayMutate();
            lifecycle_after_second_latch = reclaiming.lifecycle();
            second_armed = reclaiming.armIfAdmissible(boot_ms + 1000);
            may_mutate_after_second = reclaiming.mayMutate();
            second_done.arriveAndWait();
            return true;
        });
    /// Declared after the runtime so it runs first: a failed expectation must not leave the reclaim
    /// parked on the barrier while the runtime's destructor joins the thread.
    SCOPE_EXIT({ second_done.release(); });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    runtime.startBackgroundWorkers(std::chrono::hours(1));
    runtime.tripMountLost();
    runtime.scheduleRemount();

    second_done.waitUntilArrived();
    EXPECT_FALSE(first_armed) << "a reclaim must not arm while a newer remount request is pending";
    EXPECT_FALSE(may_mutate_after_first);
    EXPECT_FALSE(may_mutate_at_second_entry) << "no write may be admitted between the two reclaims";
    EXPECT_FALSE(may_mutate_after_second_latch) << "a reclaim starts with the fence latched";
    EXPECT_EQ(lifecycle_after_second_latch, PoolLifecycle::TransientNotLive);
    EXPECT_TRUE(second_armed) << "the reclaim that served the last request arms";
    EXPECT_TRUE(may_mutate_after_second);
    EXPECT_EQ(calls.load(), 2u);
    second_done.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// One interference report produces one remount generation, one epoch change and one lease-loss
/// count, also when the reclaim fails twice before it succeeds. Two failures cost one and two seconds
/// of the loop's real backoff.
TEST(CASMountRuntime, AReclaimAcknowledgesOnlyTheGenerationItServed)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-reclaim-acknowledges");
    uint64_t wall_ms = 1000;
    const uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    std::atomic<uint32_t> calls{0};
    std::atomic<uint32_t> fresh_epochs{0};
    DB::Cas::tests::ManualBarrier reclaimed;
    CasMountRuntime * runtime_ptr = nullptr;
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            CasMountRuntime & reclaiming = *runtime_ptr;
            reclaiming.beginReclaim();
            if (++calls < 3)
                return false;
            fenceOutMount(*backend, layout.mountKey("test"));
            const MountClaimResult fresh
                = claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 2, wall_ms, 1000);
            EXPECT_EQ(fresh.kind, MountClaimResult::Claimed);
            if (fresh.kind != MountClaimResult::Claimed)
                return false;
            ++fresh_epochs;
            reclaiming.installRenewer(uuid, 2, [&] { return wall_ms; });
            const uint64_t fresh_anchor = reclaiming.startRenewer();
            reclaiming.setLiveWriterEpoch(2);
            EXPECT_TRUE(reclaiming.armIfAdmissible(fresh_anchor + 1000));
            reclaimed.arriveAndWait();
            return true;
        });
    SCOPE_EXIT({ reclaimed.release(); });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    const uint64_t lost_before = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();
    runtime.startBackgroundWorkers(std::chrono::hours(1));
    /// One interference report.
    runtime.tripMountLost();
    runtime.scheduleRemount();

    reclaimed.waitUntilArrived();
    EXPECT_EQ(calls.load(), 3u);
    EXPECT_EQ(fresh_epochs.load(), 1u);
    EXPECT_EQ(runtime.remountRequestedGenerationForTest(), 1u) << "a failed reclaim raises no generation";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load(), lost_before + 1)
        << "the latch of each attempt counts no further loss";
    EXPECT_TRUE(runtime.mayMutate());
    reclaimed.release();
    runtime.stopBackgroundWorkers();
    EXPECT_EQ(calls.load(), 3u) << "the served generation is not reclaimed again";
    runtime.finishTeardown(false);
}

/// The first two steps of `Pool::forgetDisk` land while a reclaim runs: the reclaim that finishes
/// after them arms nothing, so the fence is latched before FORGET's second trip, and the thread exits.
TEST(CASMountRuntime, AReclaimFinishedAfterTheForgetIntentArmsNothing)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-reclaim-after-intent");
    uint64_t wall_ms = 1000;
    const uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    WorkerExitLatch exits;
    uint64_t threads = 0;
    RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
    {
        ++threads;
        return ThreadFromGlobalPool([&, body = std::move(worker_body)]
        {
            body();
            exits.recordExit();
        });
    };
    DB::Cas::tests::ManualBarrier latched;
    bool armed = true;
    bool may_mutate_after_reclaim = true;
    CasMountRuntime * runtime_ptr = nullptr;
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }, .worker_factory = factory},
        "test", sink, runtimeRenewBudget(), [&]
        {
            CasMountRuntime & reclaiming = *runtime_ptr;
            reclaiming.beginReclaim();
            latched.arriveAndWait();
            armed = reclaiming.armIfAdmissible(boot_ms + 1000);
            may_mutate_after_reclaim = reclaiming.mayMutate();
            return true;
        });
    SCOPE_EXIT({ latched.release(); });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    runtime.startBackgroundWorkers(std::chrono::hours(1));
    runtime.tripMountLost();
    runtime.scheduleRemount();

    latched.waitUntilArrived();
    runtime.publishVanishedIntent();
    runtime.tripMountLost();
    latched.release();

    const bool exited = exits.waitForAtLeast(threads);
    EXPECT_TRUE(exited) << "the published intent must end the loop";
    EXPECT_FALSE(armed) << "a reclaim that finished after the intent must not arm the fence";
    EXPECT_FALSE(may_mutate_after_reclaim);
    EXPECT_FALSE(runtime.mayMutate()) << "the fence is latched before FORGET's second trip";
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// A stop requested during a reclaim: the join returns once the attempt returns, the reclaim arms
/// nothing, the stop counts no lease loss, and no farewell is written for the slot this runtime did
/// not claim back.
TEST(CASMountRuntime, StopDuringAReclaimJoinsTheThread)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-stop-during-reclaim");
    uint64_t wall_ms = 1000;
    const uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    WorkerExitLatch exits;
    uint64_t threads = 0;
    RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
    {
        ++threads;
        return ThreadFromGlobalPool([&, body = std::move(worker_body)]
        {
            body();
            exits.recordExit();
        });
    };
    DB::Cas::tests::ManualBarrier reclaim_entered;
    std::atomic<bool> stop_requested_by_test{false};
    bool stop_seen_by_reclaim = false;
    bool armed = true;
    std::atomic<bool> reclaim_returned{false};
    uint64_t lost_at_latch = 0;
    CasMountRuntime * runtime_ptr = nullptr;
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }, .worker_factory = factory},
        "test", sink, runtimeRenewBudget(), [&]
        {
            CasMountRuntime & reclaiming = *runtime_ptr;
            reclaiming.beginReclaim();
            lost_at_latch = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();
            reclaim_entered.arriveAndWait();
            /// Returns on the stop; the timeout only bounds a failing run.
            reclaiming.sleepInterruptibly(20'000);
            stop_seen_by_reclaim = stop_requested_by_test.load();
            armed = reclaiming.armIfAdmissible(boot_ms + 1000);
            reclaim_returned = true;
            return true;
        });
    SCOPE_EXIT({ reclaim_entered.release(); });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    const String key = layout.mountKey("test");
    /// A definitive answer ends the first renewal and requests the reclaim; nobody claims the slot back.
    fenceOutMount(*backend, key);
    const uint64_t lost_before = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));

    reclaim_entered.waitUntilArrived();
    stop_requested_by_test = true;
    auto stop = std::async(std::launch::async, [&] { runtime.stopBackgroundWorkers(); });
    reclaim_entered.release();
    stop.get();

    EXPECT_TRUE(reclaim_returned.load()) << "the join waits for the reclaim attempt to return";
    EXPECT_EQ(exits.count(), threads);
    EXPECT_TRUE(stop_seen_by_reclaim) << "the reclaim must finish after the stop was requested";
    EXPECT_FALSE(armed) << "a reclaim that finishes after a stop must not arm the fence";
    EXPECT_FALSE(runtime.mayMutate());
    EXPECT_EQ(lost_at_latch, lost_before + 1) << "the fenced-out renewal counts the one loss";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load(), lost_at_latch)
        << "the stop counts no lease loss";
    const auto slot_before_teardown = readObj(*backend, key);
    ASSERT_TRUE(slot_before_teardown.has_value());
    runtime.finishTeardown(true);
    const auto slot_after_teardown = readObj(*backend, key);
    ASSERT_TRUE(slot_after_teardown.has_value());
    EXPECT_EQ(slot_after_teardown->bytes, slot_before_teardown->bytes)
        << "no farewell for a slot this runtime did not claim back";
}
```

`T/gtest_cas_forget.cpp`: replace the comment block and body of
`TEST(CASForget, ForgetReLatchesFenceAfterAReclaimReachesArmMountFence)` (:399-427) with

```cpp
/// (b2) A reclaim that finishes after FORGET published its intent arms nothing. The reclaim runs from
/// FORGET's own GC-stop step, after the intent and the first trip; the mount is fenced out, so it claims a
/// fresh incarnation at once. FORGET's second trip stays as a backstop.
TEST(CASForget, AReclaimDuringForgetArmsNothing)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    auto store = DB::Cas::tests::openPoolForTest(backend);

    /// Make the current mount claimable so the reclaim claims at once.
    fenceOutMount(*backend, store->layout().mountKey(kSrid));

    bool reclaimed = false;
    bool may_mutate_after_reclaim = true;
    store->forgetDisk(
        [&]
        {
            reclaimed = store->tryRemountOnce();
            may_mutate_after_reclaim = store->mayMutate();
        },
        kForgetReason);

    /// Guard against a vacuous pass: a reclaim that never claimed reaches no arm at all.
    ASSERT_TRUE(reclaimed) << "the injected reclaim must claim, else this test proves nothing";
    EXPECT_FALSE(may_mutate_after_reclaim) << "a reclaim after the FORGET intent must not arm the fence";
    EXPECT_EQ(store->lifecycle(), PoolLifecycle::VanishedForgotten);
    EXPECT_FALSE(store->mayMutate());
}
```

Would each fail if the behaviour it names regressed? `NothingArms...` fails if `canArm` drops the
pending term or `beginReclaim` stops tripping. `AReclaimAcknowledges...` fails if a failed attempt
raises a generation or counts a loss, or if the served generation is reclaimed again.
`AReclaimFinishedAfterTheForgetIntent...` and `AReclaimDuringForgetArmsNothing` fail if `canArm` drops
the terminal term. `StopDuringAReclaim...` fails if `canArm` drops the stop term or the stop counts a
loss. `StopDuringAReclaim...` asserts that every created thread exited, not that one was created;
cycle 2's `ARemountRequestEndsTheRenewalAndTheSameThreadReclaims` pins one thread.

- [ ] **Step 5: Build and run them to see them fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t1_c1b_red_build.log 2>&1; echo NINJA_EXIT=$? >> build/t1_c1b_red_build.log
tail -1 build/t1_c1b_red_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASMountRuntime.NothingArmsWhileARemountRequestIsPending:CASMountRuntime.AReclaimAcknowledgesOnlyTheGenerationItServed:CASMountRuntime.AReclaimFinishedAfterTheForgetIntentArmsNothing:CASMountRuntime.StopDuringAReclaimJoinsTheThread:CASForget.AReclaimDuringForgetArmsNothing' > build/t1_c1b_red_tests.log 2>&1; echo GATE_EXIT=$? >> build/t1_c1b_red_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT\|Failure\|Expected\|Which is" build/t1_c1b_red_tests.log | tail -60
```

Expected: `NINJA_EXIT=0`, then:
- `NothingArmsWhileARemountRequestIsPending` FAILS: `first_armed` is true ("a reclaim must not arm while a newer remount request is pending"), `may_mutate_after_first`, `may_mutate_at_second_entry` ("no write may be admitted between the two reclaims") and `may_mutate_after_second_latch` ("a reclaim starts with the fence latched") are true. `lifecycle_after_second_latch` is `TransientNotLive` already: the pool runs not `Live` on an armed fence, which is the defect.
- `AReclaimAcknowledgesOnlyTheGenerationItServed` PASSES: regression test, green before the task. It takes about 3 s.
- `AReclaimFinishedAfterTheForgetIntentArmsNothing` FAILS: `armed` and `may_mutate_after_reclaim` are true; `runtime.mayMutate()` is true "before FORGET's second trip".
- `StopDuringAReclaimJoinsTheThread` FAILS: `armed` is true ("a reclaim that finishes after a stop must not arm the fence") and `runtime.mayMutate()` is true. The other assertions pass.
- `CASForget.AReclaimDuringForgetArmsNothing` FAILS: `may_mutate_after_reclaim` is true.

`GATE_EXIT=1`. A hang (`GATE_EXIT=124`): find the last `[ RUN ]`; each barrier times out after 20 s.

- [ ] **Step 6: Implement the latch and the arming rule**

`RH`, replace the two declarations added in cycle 1a with

```cpp
    /// Step 0 of a reclaim. One step under `driver_mutex`: record the requested generation this attempt
    /// serves and latch the fence.
    void beginReclaim();
    /// End of a reclaim that claimed. One step under `driver_mutex`: acknowledge the generation
    /// `beginReclaim` recorded, publish the deadline, and arm the fence and report `Live` if the arming
    /// rule holds. Returns whether it armed.
    bool armIfAdmissible(uint64_t deadline_boot_ms);
```

`RH`, private, after the `armFence` declaration add

```cpp
    /// The arming rule: no remount request pending, no stop requested, a lifecycle that is not
    /// terminal. Requires `driver_mutex`, the mutex that a stop, a request and a terminal publication
    /// take, so the check and the arm are one step.
    bool canArm(uint64_t deadline_boot_ms) const;
```

`RH`, members, after `uint64_t remount_handled_generation = 0;` add

```cpp
    /// The requested generation `beginReclaim` recorded; `armIfAdmissible` acknowledges it.
    uint64_t reclaim_generation = 0;
```

`RH`, the doc of `setArmMountFenceInterpositionHookForTest` (:188-190), replace

```cpp
    /// Test-only interposition at the publication boundary between the re-armed generation and the
    /// live fence. A caller admitted from this hook must be refused: the old generation is already
    /// dead, while the new generation is not live until `lost` is cleared.
```

with

```cpp
    /// Test-only interposition at the publication boundary between the re-armed generation and the
    /// live fence. A caller admitted from this hook must be refused: the old generation is already
    /// dead, while the new generation is not live until `lost` is cleared. Through `armIfAdmissible`
    /// it runs with `driver_mutex` held.
```

`R`, replace `CasMountRuntime::beginReclaim` and `CasMountRuntime::armIfAdmissible` from cycle 1a with

```cpp
void CasMountRuntime::beginReclaim()
{
    std::lock_guard lock(driver_mutex);
    reclaim_generation = remount_requested_generation;
    /// A request can come without a trip; latching here keeps the pool from running not `Live` on an
    /// armed fence. Atomics only.
    tripMountLost();
}

bool CasMountRuntime::armIfAdmissible(uint64_t deadline_boot_ms)
{
    std::lock_guard lock(driver_mutex);
    remount_handled_generation = std::max(remount_handled_generation, reclaim_generation);
    const bool arm = canArm(deadline_boot_ms);
    if (arm)
        armFence(deadline_boot_ms, /*report_live=*/true);
    else
        setMountDeadline(deadline_boot_ms);
    driver_cv.notify_all();
    return arm;
}

bool CasMountRuntime::canArm(uint64_t /*deadline_boot_ms*/) const
{
    return !workers_stop_requested
        && !remountTerminal()
        && remount_requested_generation <= remount_handled_generation;
}
```

Critical sections: `beginReclaim` and `armIfAdmissible` hold `driver_mutex`; inside are atomics,
`noteLeaseLost`'s `incrementNoTrace`, `noteRemounted`'s compare-exchange and the interposition hook
(test only). No I/O, no log, no join. `tryRemountOnce` calls both under `remount_mutex`, the existing
order. The acknowledgement uses the snapshot `beginReclaim` took, never `remount_requested_generation`:
acknowledging a newer request would drop it.

`P`, `Pool::tryRemountOnce`, the step-0 comment (:1342-1347), replace

```cpp
    /// ==== Step 0 (rev.7 §2): pool lifecycle identity gate — BEFORE any claim/allocate/mount write ====
    /// A remount attempt means the lease is presumed lost, so first ensure we are at least transient
    /// (production reaches here already transient via `tripMountLost`; a direct/forced call may still be
    /// `Live`). Then authoritatively probe the pool sentinels and dispatch per the §2 verdict table. Only
    /// `Recover` (a present `_pool_meta` whose identity matches, in a non-`IdentityLost` state) falls
    /// through to the existing recovery below; every other verdict resolves here and returns false.
```

with

```cpp
    /// ==== Step 0: pool lifecycle identity gate — BEFORE any claim/allocate/mount write ====
    /// A reclaim starts with the fence latched and records the remount generation it serves. Then it
    /// authoritatively probes the pool sentinels. Only `Recover` (a present `_pool_meta` whose identity
    /// matches, in a non-`IdentityLost` state) falls through to the recovery below; every other verdict
    /// resolves here and returns false.
```

`P`, `Pool::tryRemountOnce`, the comment above `step = "arm_fence";`, replace

```cpp
        /// No-throw commit section: publish the fence and lifecycle only after epoch, renewer, recovery
        /// cancellation, and ref-runtime quiescence are complete.
```

with

```cpp
        /// No-throw commit section: arm only after epoch, renewer, recovery cancellation, and ref-runtime
        /// quiescence are complete, and only under the arming rule. A reclaim that claimed and did not
        /// arm still returns true: its request is acknowledged, and a false would make the loop back off
        /// and claim yet another epoch.
```

`P`, `Pool::forgetDisk` (:1192-1198), replace

```cpp
    /// A remount attempt already IN FLIGHT when step 1 published the intent completes its current step
    /// before the loop bails (the "one step + one backend timeout" bound of §5), and a successful reclaim in
    /// that window re-arms the local fence (`lost = false`). Now that the remount worker is JOINED and can
    /// never run again, re-latch the fence so the terminal `mayMutate() == false` holds regardless of any
    /// such raced reclaim. Idempotent; the durable mount lease the reclaim wrote is retired by the
    /// `finishTeardown` below (it operates on whatever renewer is current — the reclaimed one).
    mount_runtime.tripMountLost();
```

with

```cpp
    /// A reclaim in flight when step 1 published the intent finishes its current step, and the arming
    /// rule refuses to arm after the intent. This second trip is a backstop: nothing can arm the fence
    /// once the workers are joined. Idempotent; the durable mount lease such a reclaim wrote is retired
    /// by the `finishTeardown` below (it operates on whatever renewer is current — the reclaimed one).
    mount_runtime.tripMountLost();
```

- [ ] **Step 7: Build, run the tests of this cycle and their neighbours, then the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t1_c1b_build.log 2>&1; echo NINJA_EXIT=$? >> build/t1_c1b_build.log
tail -1 build/t1_c1b_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASPoolRemount.*:CASMountRuntime.*:CASForget.*:CASLifecycleCondition.*:CASFenceGeneration.*:CASPool.*' > build/t1_c1b_tests.log 2>&1; echo GATE_EXIT=$? >> build/t1_c1b_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t1_c1b_tests.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CAS*' > build/t1_c1b_gate.log 2>&1; echo GATE_EXIT=$? >> build/t1_c1b_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t1_c1b_gate.log | tail -20
```

Expected: `NINJA_EXIT=0`; both runs `GATE_EXIT=0`, no `FAILED`. Prove the guard can fail: change
`canArm` to `return true;`, rebuild, run the five tests of Step 5, see the four of them that failed in
Step 5 fail again, restore `canArm`. `CASFenceGeneration.RearmPublishesTheNewGenerationBeforeOpeningTheFence`
must be green: `armFence` now publishes `Live` before `lost = false`.

- [ ] **Step 8: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp \
  src/Disks/tests/gtest_cas_pool.cpp \
  src/Disks/tests/gtest_cas_forget.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Latch the fence at the start of a CAS reclaim and arm only when admissible" \
  -m "beginReclaim records the remount generation the attempt serves and trips the fence. armIfAdmissible acknowledges that generation and arms only when no newer request is pending, no stop is requested and the lifecycle is not terminal; the check and the arm are one step under driver_mutex, and Live is published before the fence opens. A request raised during a reclaim no longer leaves the pool not Live on an armed fence, and a reclaim that finishes after a FORGET intent or a stop arms nothing. Pool::tryRemountOnce still returns true for a reclaim that claimed and did not arm." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

#### Cycle 2: one thread renews and reclaims

- [ ] **Step 9: Write the failing tests**

`T/gtest_cas_pool.cpp`: append after `TEST(CASMountRuntime, StopDuringAReclaimJoinsTheThread)`.

```cpp
/// An interference report while the renewal retries past its lease ends it inside the wait or the
/// request it is in: no request and no wait starts after the report. The thread that renewed runs the
/// reclaim and then renews under the new epoch.
TEST(CASMountRuntime, ARemountRequestEndsTheRenewalAndTheSameThreadReclaims)
{
    enum class RequestDuring : uint8_t { Wait, Request };
    const auto run = [](RequestDuring request_during)
    {
        const Layout layout(request_during == RequestDuring::Wait ? "same-thread-reclaim-in-wait" : "same-thread-reclaim-in-request");
        const UInt128 uuid{1};
        uint64_t wall_ms = 1000;
        std::atomic<uint64_t> boot_ms{100};
        std::atomic<uint64_t> past_the_lease{std::numeric_limits<uint64_t>::max()};
        std::atomic<bool> held{false};
        std::atomic<bool> outage_on{true};
        std::atomic<uint64_t> waits{0};
        std::atomic<uint64_t> writes_at_reclaim{0};
        std::atomic<uint64_t> waits_at_reclaim{0};
        std::atomic<uint64_t> worker_threads{0};
        std::atomic<bool> reclaimed{false};
        std::atomic<bool> renewed_after_reclaim{false};
        std::mutex thread_ids_mutex;
        std::thread::id renewing_thread;
        std::thread::id reclaiming_thread;
        std::thread::id renewing_after_reclaim_thread;
        DB::Cas::tests::ManualBarrier holding;
        DB::Cas::tests::ManualBarrier renewed;
        CasMountRuntime * runtime_ptr = nullptr;
        const auto record_thread = [&](std::thread::id & slot)
        {
            std::lock_guard lock(thread_ids_mutex);
            slot = std::this_thread::get_id();
        };
        const auto hold_once_past_the_lease = [&]
        {
            if (boot_ms.load() >= past_the_lease.load() && !held.exchange(true))
            {
                record_thread(renewing_thread);
                holding.arriveAndWait();
            }
        };
        RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
        {
            ++worker_threads;
            return ThreadFromGlobalPool(std::move(worker_body));
        };
        /// Declared after the locals its hooks capture.
        auto backend = std::make_shared<RuntimeRenewBackend>();
        ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
                  MountClaimResult::Claimed);
        CasEventSink sink;
        RuntimeUnderTest runtime_holder(
            backend, layout,
            MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                        .boot_ms_fn = [&] { return boot_ms.load(); }, .worker_factory = factory},
            "test", sink, runtimeRenewBudget(), [&]
            {
                CasMountRuntime & reclaiming = *runtime_ptr;
                writes_at_reclaim = backend->outage_writes.load();
                waits_at_reclaim = waits.load();
                record_thread(reclaiming_thread);
                outage_on = false;
                reclaiming.beginReclaim();
                fenceOutMount(*backend, layout.mountKey("test"));
                const MountClaimResult fresh
                    = claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 2, wall_ms, 1000);
                EXPECT_EQ(fresh.kind, MountClaimResult::Claimed);
                if (fresh.kind != MountClaimResult::Claimed)
                    return false;
                reclaiming.installRenewer(uuid, 2, [&] { return wall_ms; });
                const uint64_t fresh_anchor = reclaiming.startRenewer();
                reclaiming.setLiveWriterEpoch(2);
                EXPECT_TRUE(reclaiming.armIfAdmissible(fresh_anchor + 1000));
                reclaimed = true;
                return true;
            });
        SCOPE_EXIT({
            holding.release();
            renewed.release();
        });
        CasMountRuntime & runtime = *runtime_holder;
        runtime_ptr = &runtime;
        runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
        const uint64_t anchor = runtime.startRenewer();
        runtime.armMountFence(uuid, 1, anchor + 1000);
        /// Five lease lengths on: a renewal bounded by its lease ended long before.
        past_the_lease = anchor + 5'000;
        runtime_holder.setRetrySleepForTest([&](uint64_t ms)
        {
            ++waits;
            boot_ms += ms;
            if (request_during == RequestDuring::Wait)
                hold_once_past_the_lease();
        });
        if (request_during == RequestDuring::Request)
            backend->on_outage_write = hold_once_past_the_lease;
        backend->outage = [&] { return outage_on.load(); };
        backend->after_commit = [&]
        {
            if (reclaimed.load() && !renewed_after_reclaim.exchange(true))
            {
                record_thread(renewing_after_reclaim_thread);
                renewed.arriveAndWait();
            }
        };
        runtime.startBackgroundWorkers(std::chrono::milliseconds(0));

        holding.waitUntilArrived();
        const uint64_t writes_at_request = backend->outage_writes.load();
        const uint64_t waits_at_request = waits.load();
        /// An interference report while the renewal retries past its lease.
        runtime.tripMountLost();
        runtime.scheduleRemount();
        holding.release();
        renewed.waitUntilArrived();

        EXPECT_EQ(worker_threads.load(), 1u) << "one thread renews and reclaims";
        {
            std::lock_guard lock(thread_ids_mutex);
            EXPECT_EQ(reclaiming_thread, renewing_thread) << "the reclaim runs on the thread that renewed";
            EXPECT_EQ(renewing_after_reclaim_thread, renewing_thread) << "the same thread renews after the reclaim";
        }
        EXPECT_EQ(writes_at_reclaim.load(), writes_at_request) << "no request starts after the remount request";
        EXPECT_EQ(waits_at_reclaim.load(), waits_at_request) << "no wait starts after the remount request";
        EXPECT_EQ(decodeMountLease(readObj(*backend, layout.mountKey("test"))->bytes).writer_epoch, 2u)
            << "the renewal after the reclaim runs under the new epoch";
        EXPECT_EQ(runtime.remountRequestedGenerationForTest(), 1u);
        renewed.release();
        runtime.stopBackgroundWorkers();
        runtime.finishTeardown(false);
    };
    run(RequestDuring::Wait);
    run(RequestDuring::Request);
}

/// A renewal whose write committed before a remount request is consumed before the reclaim runs, so
/// its deadline cannot overwrite the one the reclaim arms. The renewal's last liveness check comes
/// after its write committed: the request is raised there, so the renewal still returns `Committed`.
TEST(CASMountRuntime, ACommitConsumedAfterARemountRequestCannotOverwriteTheReclaimedDeadline)
{
    const Layout layout("commit-consumed-after-request");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{100};
    std::atomic<uint64_t> worker_threads{0};
    std::atomic<std::thread::id> first_worker_thread{};
    std::atomic<bool> committed{false};
    std::atomic<bool> requested{false};
    std::atomic<bool> held{false};
    std::atomic<bool> hold_timed_out{false};
    std::atomic<bool> armed_by_reclaim{false};
    std::atomic<uint32_t> admissions{0};
    std::promise<void> reclaim_armed;
    std::shared_future<void> reclaim_armed_future = reclaim_armed.get_future().share();
    DB::Cas::tests::ManualBarrier second_admission;
    CasMountRuntime * runtime_ptr = nullptr;
    RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
    {
        const bool first = worker_threads.fetch_add(1) == 0;
        return ThreadFromGlobalPool([&, first, body = std::move(worker_body)]
        {
            if (first)
                first_worker_thread = std::this_thread::get_id();
            body();
        });
    };
    /// With a second thread the reclaim can run before the renewing thread consumes its result. Hold
    /// that consumption at its first clock read until the reclaim armed. With one thread this never
    /// waits: the consumption comes before the reclaim on the same thread.
    const auto hold_consumption_until_armed = [&]
    {
        if (worker_threads.load() == 2 && requested.load()
            && std::this_thread::get_id() == first_worker_thread.load() && !held.exchange(true))
        {
            if (reclaim_armed_future.wait_for(std::chrono::seconds(20)) != std::future_status::ready)
                hold_timed_out = true;
        }
    };
    /// Declared after the locals its hooks capture.
    auto backend = std::make_shared<RuntimeRenewBackend>();
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .background_watermark = true,
            .boot_ms_fn = [&]
            {
                hold_consumption_until_armed();
                return boot_ms.load();
            },
            .worker_factory = factory,
            .renewal_admitted_hook_for_test = [&]
            {
                if (++admissions == 2)
                    second_admission.arriveAndWait();
            },
            .renewal_live_for_test = [&]
            {
                if (committed.load() && !requested.exchange(true))
                    runtime_ptr->scheduleRemount();
                return true;
            }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            CasMountRuntime & reclaiming = *runtime_ptr;
            reclaiming.beginReclaim();
            fenceOutMount(*backend, layout.mountKey("test"));
            const MountClaimResult fresh
                = claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 2, wall_ms, 1000);
            EXPECT_EQ(fresh.kind, MountClaimResult::Claimed);
            if (fresh.kind != MountClaimResult::Claimed)
                return false;
            reclaiming.installRenewer(uuid, 2, [&] { return wall_ms; });
            boot_ms = 500;
            const uint64_t fresh_anchor = reclaiming.startRenewer();
            reclaiming.setLiveWriterEpoch(2);
            armed_by_reclaim = reclaiming.armIfAdmissible(fresh_anchor + 1000);
            reclaim_armed.set_value();
            return true;
        });
    SCOPE_EXIT({ second_admission.release(); });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    /// Set after the setup's own writes: only the loop's renewal may raise the request.
    backend->after_commit = [&] { committed = true; };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));

    second_admission.waitUntilArrived();
    EXPECT_FALSE(hold_timed_out.load());
    ASSERT_TRUE(armed_by_reclaim.load());
    /// Between the first renewal's deadline (100 + 1000) and the reclaim's (500 + 1000).
    boot_ms = 1200;
    EXPECT_TRUE(runtime.mayMutate())
        << "the deadline is the reclaim's, not the one of the renewal consumed after the request";
    second_admission.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}
```

The first renewal's writes and the setup's `startRenewer` go through `after_commit` too; it is set
after the setup, and the request is raised once (`requested.exchange`). `postCommit` in
`CasOperation::writeLoop` is the first liveness check after a committed write, and it reads no clock
after the check, so the held clock read is `publishRenewedDeadline`'s first statement.

- [ ] **Step 10: Build and run them to see them fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t1_c2_red_build.log 2>&1; echo NINJA_EXIT=$? >> build/t1_c2_red_build.log
tail -1 build/t1_c2_red_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASMountRuntime.ARemountRequestEndsTheRenewalAndTheSameThreadReclaims:CASMountRuntime.ACommitConsumedAfterARemountRequestCannotOverwriteTheReclaimedDeadline' > build/t1_c2_red_tests.log 2>&1; echo GATE_EXIT=$? >> build/t1_c2_red_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT\|Failure\|Expected\|Which is" build/t1_c2_red_tests.log | tail -40
```

Expected: `NINJA_EXIT=0`, then:
- `ARemountRequestEndsTheRenewalAndTheSameThreadReclaims` FAILS in both cases: `worker_threads` is 2 ("one thread renews and reclaims"); `reclaiming_thread` differs from `renewing_thread`; `renewing_after_reclaim_thread` equals `renewing_thread`. The request and wait counts, the epoch and the generation pass.
- `ACommitConsumedAfterARemountRequestCannotOverwriteTheReclaimedDeadline` FAILS: `runtime.mayMutate()` is false at 1200. The renewal thread published 1100 after the remount worker armed 1500. `hold_timed_out` is false and `armed_by_reclaim` is true.

`GATE_EXIT=1`.

- [ ] **Step 11: Header: one thread, no driver states**

`RH`, delete the whole `enum class RenewalDriverState` (:50-61).

`RH`, `MountConfig`, replace from `/// Deterministic test interposition after the remount worker has confirmed renewal is parked,`
(:78) through `std::function<void()> renewal_parked_predicate_false_hook_for_test = {};` (:96) with

```cpp
    /// Deterministic test interposition at the top of each pass of the lease loop, before it takes
    /// `driver_mutex`.
    std::function<void()> renewal_before_driver_lock_hook_for_test = {};
    /// Deterministic test interposition after the loop decided to renew and released `driver_mutex`,
    /// before the renewal's I/O starts.
    std::function<void()> renewal_admitted_hook_for_test = {};
    /// Deterministic test interposition after a wait predicate of the lease loop sampled false,
    /// immediately before the wait atomically releases `driver_mutex`.
    /// Runs with `driver_mutex` held: it must issue no backend request and never wait on the pool's
    /// hot-key lane, whose holders sleep under that mutex, or the test deadlocks itself.
    std::function<void()> lease_wait_predicate_false_hook_for_test = {};
```

`RH`, class doc (:138-144), replace `` `live_writer_epoch`, unclean-boundary marker, and both persistent workers. ``
with `` `live_writer_epoch`, unclean-boundary marker, and the lease thread. ``

`RH`, constructor parameter docs (:150-152 and :161-162), replace

```cpp
        /// The planes the `MountLeaseRenewer` runs on: a bounded renewal under the mount fence, the
        /// claim and the farewell on an open one, and the worker's renewal on `lease_requests_`, which
        /// has no lease budget and whose sleep a stop wakes. Owned by `Pool` and outliving this runtime.
```

with

```cpp
        /// The planes the `MountLeaseRenewer` runs on: a bounded renewal under the mount fence, the
        /// claim and the farewell on an open one, and the lease loop's renewal on `lease_requests_`,
        /// which has no lease budget and whose sleep a stop wakes. Owned by `Pool` and outliving this
        /// runtime.
```

and

```cpp
        /// One pool-level recovery attempt. The callback captures the owning `Pool` and is invoked only
        /// after construction, from the recovery thread.
```

with

```cpp
        /// One pool-level recovery attempt. The callback captures the owning `Pool` and is invoked only
        /// after construction, from the lease thread.
```

`RH`, replace these doc sentences, each in place. A sentence may wrap across `///` lines; match it
across the wrap and rewrap the result.

| Declaration | Old text | New text |
|---|---|---|
| section banner (:384) | `/// ---- mount-lease renewer and persistent workers ----` | `/// ---- mount-lease renewer and the lease thread ----` |
| `vanishedIntentPublished` (:225-230) | `Consulted alongside \`isVanished()\` by every background worker that must self-exit the moment the pool is (being driven) terminal — the renewer callback (\`scheduleRemount\`), the remount loop, and the GC scheduler.` | `Consulted alongside \`isVanished()\` wherever background work must stop the moment the pool is (being driven) terminal: \`scheduleRemount\`, the lease loop and the GC scheduler.` |
| `noteLeaseLost` (:233-237) | `` `tripMountLost` calls this (the lease-loss primitive), and the remount loop's identity gate calls it as its first step so a direct/forced remount attempt has a valid non-terminal predecessor state. `` | `` `tripMountLost` calls this (the lease-loss primitive). `` |
| `enterIdentityLost` (:244-251) | `so the remount worker self-exits (and the GC scheduler self-exits, through \`Pool\`)` | `so the lease thread exits (and the GC scheduler self-exits, through \`Pool\`)` |
| `publishVanishedIntent` (:259-266) | `makes the runtime terminal consumer stop latching remount generations and the remount loop bail at its next step boundary, so FORGET's subsequent worker joins are bounded to one step + one backend timeout.` | `makes the runtime stop latching remount generations and the lease loop exit at its next step boundary, so FORGET's join of the lease thread is bounded by one step and one backend timeout.` |
| `publishVanishedIntent` | `so a worker cannot miss the terminal edge between its predicate sample and wait.` | `so the lease loop cannot miss the terminal edge between its predicate sample and wait.` |
| `enterVanished` (:269-280) | `(so the runtime stops scheduling remount work and the remount loop exits at its next step boundary)` | `(so the runtime stops scheduling remount work and the lease loop exits at its next step boundary)` |
| `remountTerminal` (:331-337) | `a state on which the self-remount worker must stop:` | `a state on which the lease thread must stop:` |
| `remountTerminal` | `Consulted by \`scheduleRemount\` before arming and by the remount loop at every step boundary.` | `Consulted by \`scheduleRemount\`, by the arming rule and by the lease loop at every step boundary.` |
| `sleepInterruptibly` (:344-348) | `this one wakes on the same stop signal the workers watch. A park or a remount request does not wake it: the wait runs out, at most one spacing draw for the background renewal.` | `this one wakes on the stop signal the lease thread watches. A remount request does not wake it: the wait runs out, at most one spacing draw for the loop's renewal.` |
| `scheduleRemount` (:392) | `Latch a recovery generation. Persistent remount ownership means this never constructs a thread.` | `Latch a recovery generation for the lease thread. It never constructs a thread.` |
| `finishTeardown` (:408) | `Join both persistent workers before an \`Active\` renewer may write its clean farewell.` | `Join the lease thread before an \`Active\` renewer may write its clean farewell.` |
| `setWaitSleepForTest` (:414-419) | `safe only called from the test's own thread before any worker is running (no persistent renewal/remount worker reads \`config.wait_sleep_fn\` concurrently with this write).` | `safe only called from the test's own thread before the lease thread starts (nothing else reads \`config.wait_sleep_fn\` concurrently with this write).` |
| `armMountFence` (:186) | `Arm a new lease incarnation and clear any loss latched for the prior incarnation.` | `Arm a new lease incarnation and clear any loss latched for the prior incarnation. Unconditional and reports no \`Live\`; a reclaim arms through \`armIfAdmissible\`.` |

`RH`, public, delete

```cpp
    RenewalDriverState renewalDriverStateForTest() const;
    void waitForRenewalDriverStateForTest(RenewalDriverState expected) const;
```

`RH`, private, replace everything from `class DriverLease` (:427) through
`MountRenewOperationEnvironment renewalEnvironment(bool worker_call);` (:458) with

```cpp
    /// Who drives a renewal. Only the loop's renewal is unbounded, counts its requests as they are
    /// sent, and raises a remount request when it ends terminal.
    enum class RenewCaller : uint8_t
    {
        Loop,
        Startup,
        Remount,
        Direct,
    };

    /// A committed renewal that ended an expiry: how long the lease was expired, and the text of the
    /// last failed request.
    struct RestoredLease
    {
        uint64_t expired_ms = 0;
        String last_failure;
    };

    MountRenewResult renewRenewerOnce(RenewCaller caller);
    MountRenewOperationEnvironment renewalEnvironment(RenewCaller caller);
```

`RH`, private, replace

```cpp
    /// Publishes a committed renewal's deadline. A deadline in the future ends the current run of
    /// trouble: it clears the failure text and, when the lease was expired, counts and logs the
    /// restore. Returns how long the lease had been expired when this deadline restores it; empty
    /// when it was not expired or is still expired.
    std::optional<uint64_t> publishRenewedDeadline(uint64_t deadline_boot_ms);
    void consumeRenewResult(
        const MountRenewResult & result,
        RenewalDriverState active_state,
        RenewalDriverState returned_state,
        bool propagate_failure);
    void renewalLoop();
    void remountLoop();
    ThreadFromGlobalPool makeWorker(std::function<void()> body);
    /// The renewal's liveness: a shutdown request and, for the worker, a parked or park-requested
    /// driver, a pool that left `Live`, or a lost fence -- the worker's plane has no fence of its own.
    /// FALSE ends the renewal.
    bool renewalLive(bool worker_call) const;
```

with

```cpp
    /// Publishes a committed renewal's deadline. Requires `driver_mutex`. A deadline in the future ends
    /// the current run of trouble: it clears the failure text and, when the lease was expired, counts
    /// the restore and returns it for the caller to log after the unlock. Empty when the lease was not
    /// expired or is still expired.
    std::optional<RestoredLease> publishRenewedDeadline(uint64_t deadline_boot_ms);
    void consumeRenewResult(const MountRenewResult & result, RenewCaller caller);
    void renewalLoop();
    ThreadFromGlobalPool makeWorker(std::function<void()> body);
    /// The renewal's liveness: no stop requested and, for the loop, no pending remount request, a pool
    /// that is `Live` and a fence that is not lost -- the loop's plane has no fence of its own. FALSE
    /// ends the renewal.
    bool renewalLive(RenewCaller caller) const;
```

`RH`, members, replace the comment of `process_epoch`'s block sentence
`ATOMIC because a self-remount re-stamps it (kept equal to \`live_writer_epoch\`) from the runtime-owned remount worker while`
with `ATOMIC because a self-remount re-stamps it (kept equal to \`live_writer_epoch\`) from the lease thread while`.

`RH`, the comment of `mount_renewer` (:514-518), replace

```cpp
    /// Synchronous mount-lease protocol state. Constructed and started on a writable open after the
    /// owner/epoch/mount startup protocol; the runtime-owned renewal worker is its sole background
    /// driver and publishes successful anchors or terminal loss into the local fence. After both
    /// workers join, teardown releases an `Active` renewer so a same-server reopen can reclaim
    /// immediately. Null on a read-only open.
```

with

```cpp
    /// Synchronous mount-lease protocol state. Constructed and started on a writable open after the
    /// owner/epoch/mount startup protocol; while the lease thread runs it is the renewer's only driver
    /// and publishes successful anchors or terminal loss into the local fence. After the lease thread
    /// joins, teardown releases an `Active` renewer so a same-server reopen can reclaim immediately.
    /// Null on a read-only open.
```

`RH`, replace the driver block (:523-537)

```cpp
    /// One mutex/condition pair owns driver admission, worker lifecycle, cadence, and the remount
    /// generation latch, and terminal predicates paired with `driver_cv`. It is never held across
    /// renewer/backend calls, remount callbacks, logging, or joins.
    mutable std::mutex driver_mutex;
    mutable std::condition_variable driver_cv;
    RenewalDriverState renewal_driver_state = RenewalDriverState::Dormant;
    bool workers_starting = false;
    bool workers_started = false;
    bool worker_loops_released = false;
    bool workers_stop_requested = false;
    std::chrono::milliseconds renewal_period{0};
    uint64_t remount_requested_generation = 0;
    uint64_t remount_handled_generation = 0;
    ThreadFromGlobalPool renewal_worker;
    ThreadFromGlobalPool remount_worker;
```

with (keep the `reclaim_generation` member cycle 1b added after `remount_handled_generation`)

```cpp
    /// One mutex/condition pair guards the lease thread's lifecycle, the cadence, the remount
    /// generations, and the terminal predicates paired with `driver_cv`. It is never held across a
    /// renewer or backend call, the reclaim, logging or a join.
    mutable std::mutex driver_mutex;
    mutable std::condition_variable driver_cv;
    bool workers_started = false;
    bool workers_stop_requested = false;
    std::chrono::milliseconds renewal_period{0};
    uint64_t remount_requested_generation = 0;
    uint64_t remount_handled_generation = 0;
    /// The requested generation `beginReclaim` recorded; `armIfAdmissible` acknowledges it.
    uint64_t reclaim_generation = 0;
    ThreadFromGlobalPool renewal_worker;
```

`RH`, `pool_lifecycle` comment (:556-559): replace `terminal predicate publication is serialized with worker waits by \`driver_mutex\``
with `terminal predicate publication is serialized with the lease loop's waits by \`driver_mutex\``.

`src/Common/setThreadName.h`, delete the line `    M(CAS_REMOUNT, "CasRemount") \` (:41).

- [ ] **Step 12: `R`: the loop, the renewal step, start and stop**

Delete these whole functions from `R`: `CasMountRuntime::DriverLease::DriverLease` (:357-361),
`CasMountRuntime::renewalWorkerMayRenew` (:363-368), `CasMountRuntime::DriverLease::~DriverLease`
(:370-387), `CasMountRuntime::DriverLease::finish` (:389-426), `CasMountRuntime::admitRenewerCall`
(:428-450), `CasMountRuntime::remountLoop` (:837-921), `CasMountRuntime::renewalDriverStateForTest`
(:1190-1194), `CasMountRuntime::waitForRenewalDriverStateForTest` (:1196-1204).

Replace `CasMountRuntime::renewWatermarkOnce` (:287-294) with

```cpp
void CasMountRuntime::renewWatermarkOnce()
{
    (void)renewRenewerOnce(RenewCaller::Direct);
}
```

Replace `CasMountRuntime::installRenewer` (:452-472) with

```cpp
void CasMountRuntime::installRenewer(
    UInt128 our_uuid,
    uint64_t writer_epoch,
    const std::function<uint64_t()> & now_ms)
{
    std::unique_ptr<MountLeaseRenewer> replaced = std::make_unique<MountLeaseRenewer>(
        mount_requests, farewell_requests, lease_requests, layout, server_root_id, our_uuid, writer_epoch,
        config.mount_lease_ttl_ms, now_ms,
        [this] { return minActive(); },
        [this](CasEvent e) { emitEvent(std::move(e)); },
        std::chrono::milliseconds(cas_request_budget.lease_safety_margin_ms),
        [this] { return bootMsNow(); });

    /// The previous renewer is destroyed after the unlock.
    std::lock_guard lock(driver_mutex);
    std::swap(mount_renewer, replaced);
}
```

Replace `CasMountRuntime::startRenewer` (:474-508) with

```cpp
uint64_t CasMountRuntime::startRenewer()
{
    MountLeaseRenewer * renewer = nullptr;
    {
        std::lock_guard lock(driver_mutex);
        if (!mount_renewer)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: startRenewer without a renewer");
        if (mount_renewer->state() != MountLeaseRenewerState::New)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: startRenewer requires a New renewer");
        renewer = mount_renewer.get();
    }
    return renewer->start([this] { return !renewalCancelled(); });
}
```

Replace `CasMountRuntime::renewalEnvironment` (:510-528) and `CasMountRuntime::renewalLive` (:530-540) with

```cpp
MountRenewOperationEnvironment CasMountRuntime::renewalEnvironment(RenewCaller caller)
{
    const bool loop = caller == RenewCaller::Loop;
    return MountRenewOperationEnvironment{
        .boot_ms = [this] { return bootMsNow(); },
        .live = [this, caller]
        {
            return renewalLive(caller) && (!config.renewal_live_for_test || config.renewal_live_for_test());
        },
        .cancelled = [this] { return renewalCancelled(); },
        /// Only the loop keeps renewing past the lease; startup, remount and direct renewals stay
        /// bounded by it.
        .policy = loop ? MountRenewPolicy::UntilDefinitive : MountRenewPolicy::LeaseBound,
        /// The loop counts its requests as they are sent, so an outage shows while it lasts.
        .on_request = loop
            ? std::function<void(const MountRenewRequestEvent &)>(
                  [this](const MountRenewRequestEvent & event) { noteRenewRequest(event); })
            : nullptr,
    };
}

bool CasMountRuntime::renewalLive(RenewCaller caller) const
{
    std::lock_guard lock(driver_mutex);
    if (workers_stop_requested)
        return false;
    if (caller != RenewCaller::Loop)
        return true;
    return remount_requested_generation <= remount_handled_generation
        && lifecycle() == PoolLifecycle::Live
        && !mount_fence.lost.load(std::memory_order_acquire);
}
```

Replace `CasMountRuntime::publishRenewedDeadline` (:222-251) with

```cpp
std::optional<CasMountRuntime::RestoredLease> CasMountRuntime::publishRenewedDeadline(uint64_t deadline_boot_ms)
{
    const uint64_t now = bootMsNow();
    const std::optional<uint64_t> expired_at = leaseExpiredAt(now);
    if (deadline_boot_ms <= now)
    {
        /// The carried instant goes first: a concurrent snapshot reads the deadline and this instant
        /// separately, and must never see the new deadline without it.
        lease_expired_at_boot_ms.store(
            expired_at.value_or(std::numeric_limits<uint64_t>::max()), std::memory_order_release);
        setMountDeadline(deadline_boot_ms);
        return std::nullopt;
    }
    setMountDeadline(deadline_boot_ms);
    lease_expired_at_boot_ms.store(std::numeric_limits<uint64_t>::max(), std::memory_order_release);
    String ended_failure;
    {
        std::lock_guard lock(renew_failure_mutex);
        ended_failure.swap(last_renew_failure);
    }
    if (!expired_at)
        return std::nullopt;
    ProfileEvents::incrementNoTrace(ProfileEvents::CASMountLeaseExpired);
    return RestoredLease{.expired_ms = now - *expired_at, .last_failure = std::move(ended_failure)};
}
```

Replace `CasMountRuntime::consumeRenewResult` (:554-615) with

```cpp
void CasMountRuntime::consumeRenewResult(const MountRenewResult & result, RenewCaller caller)
{
    /// The loop counts its requests as they are sent (`noteRenewRequest`). Every other renewal counts
    /// them here, from its result.
    if (caller != RenewCaller::Loop && result.attempts_sent > 0)
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

    /// One step under `driver_mutex`. The loop reads the remount generations only after it, so no
    /// reclaim can arm between a commit and the publication of its deadline.
    std::optional<RestoredLease> restored;
    {
        std::lock_guard lock(driver_mutex);
        if (result.outcome == MountRenewOutcome::Committed)
        {
            const uint64_t ttl_ms = static_cast<uint64_t>(config.mount_lease_ttl_ms.count());
            restored = publishRenewedDeadline(
                result.attempt_start_boot_ms > std::numeric_limits<uint64_t>::max() - ttl_ms
                    ? std::numeric_limits<uint64_t>::max()
                    : result.attempt_start_boot_ms + ttl_ms);
        }
        else if (result.outcome == MountRenewOutcome::Terminal)
        {
            switch (caller)
            {
                case RenewCaller::Loop:
                    if (workers_stop_requested)
                    {
                        tripFenceWithoutOperationalLoss();
                        break;
                    }
                    tripMountLost();
                    schedule_remount_calls_for_test.fetch_add(1, std::memory_order_relaxed);
                    /// A pending request already covers this loss.
                    if (!remountTerminal() && remount_requested_generation == remount_handled_generation)
                        ++remount_requested_generation;
                    break;
                case RenewCaller::Direct:
                    tripMountLost();
                    schedule_remount_calls_for_test.fetch_add(1, std::memory_order_relaxed);
                    break;
                case RenewCaller::Startup:
                    tripFenceWithoutOperationalLoss();
                    break;
                case RenewCaller::Remount:
                    /// The reclaim latched the fence at its start and reports this failure itself.
                    if (workers_stop_requested)
                        tripFenceWithoutOperationalLoss();
                    break;
            }
        }
        driver_cv.notify_all();
    }

    if (restored)
        LOG_WARNING(getLogger("CasPool"),
            "CAS mount lease of '{}' was expired for {} ms; a renewal restored it and writes resume. "
            "Last failed renewal request: {}",
            server_root_id, restored->expired_ms, restored->last_failure);

    if (result.outcome == MountRenewOutcome::Committed)
    {
        std::optional<uint64_t> expired_ms;
        if (restored)
            expired_ms = restored->expired_ms;
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

    if (caller != RenewCaller::Loop)
        std::rethrow_exception(result.failure);
}
```

The terminal branches keep today's `DriverLease::finish` outcomes per caller: the loop trips and
raises one generation (C5, C6); a direct renewal trips and counts but never raises one (it is refused
on a background runtime); startup trips without a loss; a remount re-anchor trips only under a stop.
One change: a loop renewal that ends terminal while a request is pending trips the fence, where
`finish` left it alone; `tripMountLost` counts nothing on a pool that is not `Live`, and
`beginReclaim` trips it anyway.

Critical section: `driver_mutex`, then `renew_failure_mutex` inside `publishRenewedDeadline`;
atomics, a `String::swap`, a move and no-trace counters. `bootMsNow` calls `config.boot_ms_fn` under
the mutex, as the cadence check already does. The log, the report and the rethrow run after the unlock.

Replace `CasMountRuntime::renewRenewerOnce`, `renewRenewerForStartupOnce`,
`renewRenewerForRemountOnce` and `renewerReset` (:617-669) with

```cpp
MountRenewResult CasMountRuntime::renewRenewerOnce(RenewCaller caller)
{
    MountLeaseRenewer * renewer = nullptr;
    {
        std::lock_guard lock(driver_mutex);
        if (caller == RenewCaller::Direct && config.background_watermark)
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "CAS mount runtime: direct renewal is disabled when background ownership is configured");
        if (!mount_renewer)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: renewal without a renewer");
        if (mount_renewer->state() != MountLeaseRenewerState::Active)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: renewal requires an Active renewer");
        renewer = mount_renewer.get();
    }
    /// Configuration is pointer/POD-only. A remount re-anchor retains its completed observation for the
    /// whole-chain finalizer to deliver after `remount_mutex` is released.
    configureMountRenewObservability(&server_root_id, &event_sink, caller == RenewCaller::Remount);
    /// The remount re-anchor runs before `armIfAdmissible`, with the fence still latched, so it renews on
    /// the renewer's open plane: admitted under the mount fence it could only ever give up.
    const MountRenewResult result = caller == RenewCaller::Remount
        ? renewer->renewForRemount(renewalEnvironment(caller))
        : renewer->renew(renewalEnvironment(caller));
    consumeRenewResult(result, caller);
    return result;
}

uint64_t CasMountRuntime::renewRenewerForStartupOnce()
{
    return renewRenewerOnce(RenewCaller::Startup).attempt_start_boot_ms;
}

uint64_t CasMountRuntime::renewRenewerForRemountOnce()
{
    return renewRenewerOnce(RenewCaller::Remount).attempt_start_boot_ms;
}

void CasMountRuntime::renewerReset()
{
    std::unique_ptr<MountLeaseRenewer> released;
    std::lock_guard lock(driver_mutex);
    std::swap(mount_renewer, released);
}
```

`released` is declared before the `lock_guard`, so the renewer is destroyed after the unlock.

Replace `CasMountRuntime::startBackgroundWorkers` and `CasMountRuntime::renewalLoop` (:678-835) with

```cpp
void CasMountRuntime::startBackgroundWorkers(std::chrono::milliseconds period)
{
    {
        std::lock_guard lock(driver_mutex);
        if (workers_started || renewal_worker.joinable())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: the lease thread cannot start in the current state");
        if (!mount_renewer || mount_renewer->state() != MountLeaseRenewerState::Active)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: the lease thread requires an Active renewer");
        workers_started = true;
        workers_stop_requested = false;
        renewal_period = period;
    }

    ThreadFromGlobalPool worker;
    try
    {
        worker = makeWorker([this] { renewalLoop(); });
    }
    catch (...)
    {
        /// No thread exists, so nothing is joined.
        {
            std::lock_guard lock(driver_mutex);
            workers_stop_requested = true;
            workers_started = false;
            driver_cv.notify_all();
        }
        tripFenceWithoutOperationalLoss();
        throw;
    }

    std::lock_guard lock(driver_mutex);
    renewal_worker = std::move(worker);
}

void CasMountRuntime::renewalLoop()
{
    setThreadName(ThreadName::CAS_LEASE_RENEWER);
    /// The tracker still counts this thread's allocations but never throws on it: a renewal failed by a
    /// memory limit costs the mount, and an exception outside the request ends this thread.
    LockMemoryExceptionInThread memory_exception_lock(VariableContext::Global);

    /// The wake condition of every wait in this loop; the reclaim backoff ignores a request, which is
    /// the one it is retrying.
    const auto woken = [this](bool by_request)
    {
        const bool wake = workers_stop_requested
            || remountTerminal()
            || (by_request && remount_requested_generation > remount_handled_generation);
        if (!wake && config.lease_wait_predicate_false_hook_for_test)
            config.lease_wait_predicate_false_hook_for_test();
        return wake;
    };

    uint64_t backoff_ms = 1000;
    while (true)
    {
        if (config.renewal_before_driver_lock_hook_for_test)
            config.renewal_before_driver_lock_hook_for_test();

        bool reclaim = false;
        uint64_t snapshot = 0;
        {
            std::unique_lock lock(driver_mutex);
            if (workers_stop_requested || remountTerminal())
                return;
            if (remount_requested_generation > remount_handled_generation)
            {
                reclaim = true;
                snapshot = remount_requested_generation;
            }
            else if (!mount_renewer || mount_renewer->state() != MountLeaseRenewerState::Active)
            {
                driver_cv.wait(lock, [&] { return woken(/*by_request=*/true); });
                continue;
            }
            else
            {
                const uint64_t last_anchor = mount_renewer->lastCommittedAttemptStartBootMs();
                const uint64_t period_ms = static_cast<uint64_t>(std::max<int64_t>(0, renewal_period.count()));
                const uint64_t due = last_anchor > std::numeric_limits<uint64_t>::max() - period_ms
                    ? std::numeric_limits<uint64_t>::max()
                    : last_anchor + period_ms;
                const uint64_t now = bootMsNow();
                if (now < due)
                {
                    driver_cv.wait_for(lock, std::chrono::milliseconds(due - now), [&] { return woken(/*by_request=*/true); });
                    continue;
                }
            }
        }

        if (reclaim)
        {
            bool reclaimed = false;
            try
            {
                reclaimed = remount_attempt();
            }
            catch (...)
            {
                tryLogCurrentException(getLogger("CasPool"), "CAS self-remount attempt failed");
            }

            std::unique_lock lock(driver_mutex);
            if (reclaimed)
            {
                /// `armIfAdmissible` already acknowledged the generation the attempt served. This keeps a
                /// callback that returns true without it from reclaiming the same generation again.
                remount_handled_generation = std::max(remount_handled_generation, snapshot);
                backoff_ms = 1000;
                continue;
            }
            driver_cv.wait_for(lock, std::chrono::milliseconds(backoff_ms), [&] { return woken(/*by_request=*/false); });
            backoff_ms = std::min<uint64_t>(backoff_ms * 2, 30000);
            continue;
        }

        if (config.renewal_admitted_hook_for_test)
            config.renewal_admitted_hook_for_test();
        try
        {
            (void)renewRenewerOnce(RenewCaller::Loop);
        }
        catch (...)
        {
            /// The loop's renewal does not propagate renewal failures, so anything arriving here is this
            /// loop's own state machine reporting that it was driven out of contract. A background loop
            /// must not take the process down, but it must not keep driving a state machine that just
            /// proved wrong either: every later renewal would be unaudited.
            ///
            /// Exiting is safe because write admission is bounded by the fence deadline, not by this
            /// thread's existence -- `mayMutate` requires `bootMsNow() < mount_fence.deadline_boot_ms`, so
            /// writes stop being admitted within one TTL whether or not anyone is renewing. Tripping the
            /// fence first brings that boundary forward instead of waiting for the TTL to lapse.
            tripMountLost();
            tryLogCurrentException(getLogger("CasPool"), "CAS mount-lease renewal loop");
            chassert(false);
            return;
        }
    }
}
```

The old catch's "Residual" paragraph is deleted: it described an external remount re-arming the
fence after this thread exits, and after this task nothing serves a request once the thread is gone.

The loop's sections: the top section and the three waits hold `driver_mutex` and read guarded
fields, `remountTerminal`'s atomics, the renewer's state and anchor, and `bootMsNow`; the reclaim
section after the attempt writes `remount_handled_generation` and waits. The attempt, the renewal,
both test hooks outside the waits, and the log run with no lock held. The cadence wait now has a
predicate: it ends on a stop, a terminal lifecycle or a request (C7, rule 13), not on any
notification. Only this thread replaces the renewer while it runs, so no other notification can move
the anchor. The backoff ignores a newer request (C8, C11).

Replace `CasMountRuntime::stopBackgroundWorkers` (:923-952) with

```cpp
void CasMountRuntime::stopBackgroundWorkers()
{
    ThreadFromGlobalPool * to_join = nullptr;
    {
        std::lock_guard lock(driver_mutex);
        if (!workers_started && !renewal_worker.joinable())
            return;
        workers_stop_requested = true;
        driver_cv.notify_all();
        to_join = &renewal_worker;
    }

    if (to_join->joinable())
        to_join->join();

    {
        std::lock_guard lock(driver_mutex);
        workers_started = false;
        driver_cv.notify_all();
    }
}
```

Replace `CasMountRuntime::scheduleRemount` (:1161-1173) and `CasMountRuntime::beginShutdownForTest`
(:1182-1188) with

```cpp
void CasMountRuntime::scheduleRemount()
{
    schedule_remount_calls_for_test.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(driver_mutex);
    if (workers_stop_requested || remountTerminal())
        return;
    ++remount_requested_generation;
    driver_cv.notify_all();
}
```

and

```cpp
void CasMountRuntime::beginShutdownForTest()
{
    std::lock_guard lock(driver_mutex);
    workers_stop_requested = true;
    driver_cv.notify_all();
}
```

Replace `CasMountRuntime::workersRunningForTest` (:1206-1210) with

```cpp
bool CasMountRuntime::workersRunningForTest() const
{
    std::lock_guard lock(driver_mutex);
    return workers_started && renewal_worker.joinable();
}
```

`finishTeardown` (:1218) keeps its code: `stopBackgroundWorkers`, then the farewell on an `Active`
renewer when drained.

- [ ] **Step 13: `P`, `PH` and `SH`: the chassert and the comments about two workers and parking**

`P`, `Pool::forgetDisk` (:1148-1155), replace

```cpp
    /// Hazard C6: FORGET joins both mount-runtime workers (and, via `stop_and_join_gc`, the GC threads), so it
    /// MUST run on the admin/query thread — never a pool thread, whose join of itself would deadlock. The
    /// guard is a programming-error assertion (a self-join hangs; it never corrupts), so a chassert is the
    /// right severity, not a release fail-close.
    const ThreadName tn = getThreadName();
    chassert(tn != ThreadName::CAS_REMOUNT && tn != ThreadName::CAS_GC_SCHEDULER
             && tn != ThreadName::CAS_GC_HEARTBEAT
             && "SYSTEM CAS FORGET must not run on a CAS pool thread (self-join deadlock)");
```

with

```cpp
    /// FORGET joins the lease thread (and, via `stop_and_join_gc`, the GC threads), so it MUST run on the
    /// admin/query thread — never a pool thread, whose join of itself would deadlock. The guard is a
    /// programming-error assertion (a self-join hangs; it never corrupts), so a chassert is the right
    /// severity, not a release fail-close.
    const ThreadName tn = getThreadName();
    chassert(tn != ThreadName::CAS_LEASE_RENEWER && tn != ThreadName::CAS_GC_SCHEDULER
             && tn != ThreadName::CAS_GC_HEARTBEAT
             && "SYSTEM CAS FORGET must not run on a CAS pool thread (self-join deadlock)");
```

`P`, more comments, each replaced in place:

`P`, `Pool::mountWritable` (:915-920), replace

```cpp
    /// Gate the two persistent runtime workers with `background_watermark`: they run only in production
    /// (`background_watermark` = context != nullptr && !read_only), never in unit tests — which
    /// drive `renewWatermarkOnce` explicitly and rely on the armed sub-TTL deadline, never on a loop.
    /// The synchronous renewer is still started above (it must adopt the mount and arm the fence on
    /// every writable open); only the worker pair is conditional. The merged
    /// heartbeat renews at `mount_renew_period` — one beat now renews the lease and the floor.
```

with

```cpp
    /// Gate the lease thread with `background_watermark`: it runs only in production
    /// (`background_watermark` = context != nullptr && !read_only), never in unit tests — which
    /// drive `renewWatermarkOnce` explicitly and rely on the armed sub-TTL deadline, never on a loop.
    /// The synchronous renewer is still started above (it must adopt the mount and arm the fence on
    /// every writable open); only the lease thread is conditional. The merged
    /// heartbeat renews at `mount_renew_period` — one beat now renews the lease and the floor.
```

| Where | Old text | New text |
|---|---|---|
| `Pool::~Pool` (:1024) | `/// 1. Stop and join both persistent mount-runtime workers before draining or releasing the renewer.` | `/// 1. Stop and join the lease thread before draining or releasing the renewer.` |
| `Pool::forgetDisk` (:1189) | `/// (5a) Stop and join both persistent mount-runtime workers outside \`remount_mutex\`.` | `/// (5a) Stop and join the lease thread outside \`remount_mutex\`.` |
| `Pool::forgetDisk` (:1214) | `` `renewerReset` is safe now: both renewer-driving workers are joined. `` | `` `renewerReset` is safe now: the lease thread is joined. `` |
| `Pool::tryRemountOnce` (:1265) | `/// A parked redo completed while the whole-chain serializer was held.` | `/// A remount re-anchor completed while the whole-chain serializer was held.` |
| `Pool::tryRemountOnce` (:1397) | `` terminal-intent latch at step 1 (`publishVanishedIntent`), then joins the remount worker; `` | `` terminal-intent latch at step 1 (`publishVanishedIntent`), then joins the lease thread; `` |
| `Pool::tryRemountOnce` (:1413-1414) | `` fail-loud TERMINAL state — the remount worker self-exits at its next boundary (see `` | `` fail-loud TERMINAL state — the lease thread exits at its next boundary (see `` |
| `Pool::tryRemountOnce` (:1511-1512) | `/// The persistent renewal worker is parked before this callback is entered, so renewer` / `/// replacement cannot race any synchronous lease operation.` | `/// This runs on the lease thread, or with no lease thread running, so no renewal is in flight` / `/// while the renewer is replaced.` |
| `Pool::tryRemountOnce` (:1545) | `/// admits one synchronous parked redo while at least one physical attempt still fits the old` | `/// admits one synchronous re-anchor while at least one physical attempt still fits the old` |
| above `Pool::scheduleRemountForTest` (:1598-1600) | `/// The persistent self-remount and merged-heartbeat renewal workers live in \`mount_runtime\`` | `/// The lease thread, which renews and runs the self-remount, lives in \`mount_runtime\`` |

`PH`, each replaced in place:

| Where | Old text | New text |
|---|---|---|
| `PoolConfig::background_watermark` (:185) | `/// Tests drive \`renewWatermarkOnce\` explicitly; gates both persistent runtime workers.` | `/// Tests drive \`renewWatermarkOnce\` explicitly; gates the lease thread.` |
| `Pool::forgetDisk` doc (:555-556) | `(1) publish the terminal-intent latch FIRST (so both runtime worker loops bail at their` / `next step boundary, bounding the joins below)` | `(1) publish the terminal-intent latch FIRST (so the lease loop exits at its` / `next step boundary, bounding the joins below)` |
| `Pool::forgetDisk` doc (:558-559) | `` injected because the scheduler is owned above the Pool, a no-op in contexts that run none — and stop `` / `+ join both persistent workers;` | `` injected because the scheduler is owned above the Pool, a no-op in contexts that run none — and stop `` / `+ join the lease thread;` |
| `Pool::forgetDisk` doc (:562-563) | `MUST run on the admin/query thread, never a pool (remount/GC) thread` | `MUST run on the admin/query thread, never a pool (lease or GC) thread` |
| `Pool::scheduleRemountForTest` doc (:830) | `consumer calls \`scheduleRemount\`, while external loss paths may latch the same persistent worker.` | `consumer calls \`scheduleRemount\`, while external loss paths raise the same generation for the lease thread.` |
| `Pool::mountPlaneSleepFn` (:1178-1179) | `/// The mount plane's inter-attempt sleep: woken only by a stop of the workers, so a stopping renewal` / `/// is not held for a whole capped backoff. A park or a remount request does not wake it; the wait` | `/// The mount plane's inter-attempt sleep: woken only by a stop of the lease thread, so a stopping` / `/// renewal is not held for a whole capped backoff. A remount request does not wake it; the wait` |
| `Pool::lease_requests` (:1214-1215) | `/// The worker renewal's plane: no lease budget, because the renewal keeps trying after the lease` / `/// expired; a stop, a park or a terminal lifecycle ends it through its liveness.` | `/// The lease loop's renewal plane: no lease budget, because the renewal keeps trying after the lease` / `/// expired; a stop, a remount request or a terminal lifecycle ends it through its liveness.` |
| `Pool::mount_runtime` (:1262) | `/// persistent renewal and remount workers (with one driver mutex/condition pair). Injected with backend/layout` | `/// lease thread (with one driver mutex/condition pair). Injected with backend/layout` |
| `Pool::remount_mutex` (:1279) | `/// its guarded critical section: persistent worker ownership, fence atomics, and the build registry` | `/// its guarded critical section: lease-thread ownership, fence atomics, and the build registry` |

`SH` and `CA/Pool/CasServerRoot.cpp`, each replaced in place (rewrap the comment lines if needed):

| Where | Old text | New text |
|---|---|---|
| `SH`, `MountRenewOperationEnvironment::live` (:86) | `/// Facts the mount fence cannot see (park requested, pool no longer live, shutdown). FALSE ends` | `/// Facts the mount fence cannot see (a remount request, a pool no longer live, shutdown). FALSE ends` |
| `SH`, `MountLeaseRenewer` PLANES (:554) | `/// keeps trying after the lease expired, and a stop, a park or a terminal lifecycle reaches it through` | `/// keeps trying after the lease expired, and a stop, a remount request or a terminal lifecycle reaches it through` |
| `CasServerRoot.cpp`, above `struct MountRenewObservabilityStack` (:150) | `including while a parked redo holds` | `including while a remount re-anchor holds` |
| `CasServerRoot.cpp`, `MountLeaseRenewer::throwRenewConflict` (:1525) | `/// The renewal may be parked under \`remount_mutex\`; keep the increment trace-free.` | `/// The renewal may run under \`remount_mutex\`; keep the increment trace-free.` |

After this step, run the enumeration grep from the top of this task. Expected: no hit outside
`docs/superpowers`. Then run, untruncated,
`git -C $W grep -n -i -E "remount worker|both persistent|both workers|persistent worker|parked|park request|park-requested|worker pair" -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`.
After this step exactly these unrelated hits remain:
`CA/Backend/CasRequests.cpp` (a GC fold, :357), `CA/Gc/CasGcScheduler.cpp` and `CA/Gc/CasGcScheduler.h`
(the GC scheduler's own two workers), `CA/Pool/CasPool.h` (:1119, a ref-table recovery waiter),
`CA/Pool/CasRefLedger.cpp` and `CA/Pool/CasRefLedger.h` (ref-table recovery waiters). Any other hit:
fix it in this step.

- [ ] **Step 14: Tests: delete four, merge one into the new test, adapt nine**

Delete these tests from `T/gtest_cas_pool.cpp`, each with the comment directly above it:

| Test | Lines | Subject that goes |
|---|---|---|
| `CASPoolRemount.DirectRenewCannotRaceWorkerStartOrRenewerReplacement` | :3403-3432 | a driver state refusing start, install and reset during a direct call |
| `CASPoolRemount.DueWorkerAdmissionIsReservedBeforeParkRequest` | :3434-3472 | `WorkerCall` -> `ParkRequested` -> `Parked` |
| `CASPoolRemount.DueWorkerAdmissionIsReservedBeforeStop` | :3474-3504 | `Stopping` -> `Dormant` |
| `CASPoolRemount.TerminalDepositionDoesNotTouchRenewerAfterReplacement` | :4073-4120 | the window between `finish` and `consumeRenewResult` |

Delete `CASMountRuntime.ParkEndsAnUnboundedRenewal` (:4637-4710): its two cases and its assertion
"no request and no wait starts after the request" are `ARemountRequestEndsTheRenewalAndTheSameThreadReclaims`
(Step 9) without the state assertions.

The macro `EXPECT_RUNTIME_STATE_REJECTION` (:3397-3401) stays: `DirectRenewIsRefusedForBackgroundConfiguredRuntimeAfterStop` uses it.

Adapt, in `T/gtest_cas_pool.cpp`:

1. `CASPoolRemount.RemountWaitsForRenewalParkedBeforeReplacement` (:3532-3572) becomes, whole:

```cpp
TEST(CASPoolRemount, ReclaimWaitsForTheRenewalToEnd)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-reclaim-waits");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 10'000;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind, MountClaimResult::Claimed);
    DB::Cas::tests::ManualBarrier renewal_barrier;
    DB::Cas::tests::ManualBarrier remount_barrier;
    std::atomic<uint64_t> remount_calls{0};
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            ++remount_calls;
            remount_barrier.arriveAndWait();
            return false;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    backend->barrier = &renewal_barrier;
    backend->fault = RuntimeRenewBackend::Fault::BlockThenDelegate;
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));
    renewal_barrier.waitUntilArrived();
    runtime.tripMountLost();
    runtime.scheduleRemount();
    EXPECT_EQ(remount_calls.load(), 0u) << "the reclaim must wait until the renewal in flight ends";
    renewal_barrier.release();
    remount_barrier.waitUntilArrived();
    EXPECT_EQ(remount_calls.load(), 1u);
    remount_barrier.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}
```

Dropped: the two `renewalDriverStateForTest` / `waitForRenewalDriverStateForTest` lines. Kept: "the
reclaim callback has not run while the renewal request is in flight".

2. `CASPoolRemount.TeardownJoinsBothWorkersBeforeRelease` (:3574-3607): rename to
`TeardownJoinsTheLeaseThreadBeforeRelease` and replace `EXPECT_EQ(worker_exits.load(), 2u);` with
`EXPECT_EQ(worker_exits.load(), 1u);`. Kept: the farewell follows the join
(`min_active_build_sequence == UINT64_MAX`).

3. `CASPoolRemount.NaturalTerminalTransitionMakesBothPersistentWorkersSelfExit` (:3746-3802): rename to
`NaturalTerminalTransitionMakesTheLeaseThreadExit`; replace

```cpp
        const bool both_exited_without_stop = exits.waitForAtLeast(2);
        runtime.stopBackgroundWorkers();
        EXPECT_TRUE(both_exited_without_stop);
        EXPECT_EQ(exits.count(), 2u);
```

with

```cpp
        const bool exited_without_stop = exits.waitForAtLeast(1);
        runtime.stopBackgroundWorkers();
        EXPECT_TRUE(exited_without_stop);
        EXPECT_EQ(exits.count(), 1u);
```

The transition is now published from the reclaim on the lease thread; the callback is unchanged.

4. `CASPoolRemount.ParkedRenewalCannotMissNaturalTerminalPublication` (:3804-3915) becomes, whole:

```cpp
/// A terminal publication that races a wait of the lease loop is serialized by `driver_mutex`: it
/// cannot land between the wait predicate's sample and the wait, so the thread exits without a stop.
/// Two waits: the cadence wait and the reclaim backoff.
TEST(CASPoolRemount, ALeaseWaitCannotMissNaturalTerminalPublication)
{
    enum class Wait : uint8_t { Cadence, ReclaimBackoff };
    for (Wait wait : {Wait::Cadence, Wait::ReclaimBackoff})
    {
        for (PoolLifecycle terminal : {PoolLifecycle::IdentityLost, PoolLifecycle::VanishedReplaced})
        {
            auto backend = std::make_shared<RuntimeRenewBackend>();
            const Layout layout(fmt::format(
                "runtime-wait-terminal-{}-{}",
                wait == Wait::Cadence ? "cadence" : "backoff",
                terminal == PoolLifecycle::IdentityLost ? "identity-lost" : "vanished"));
            uint64_t wall_ms = 1000;
            uint64_t boot_ms = 100;
            const UInt128 uuid{1};
            ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
                      MountClaimResult::Claimed);
            WorkerExitLatch exits;
            std::once_flag pause_once;
            std::latch predicate_sampled_false{1};
            std::latch release_predicate{1};
            std::once_flag release_once;
            std::atomic<bool> waiter_holds_driver_mutex{false};
            std::atomic<bool> publication_entered_while_held{false};
            const auto release_waiter = [&]
            {
                std::call_once(release_once, [&] { release_predicate.count_down(); });
            };
            RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
            {
                return ThreadFromGlobalPool([&, body = std::move(worker_body)]
                {
                    body();
                    exits.recordExit();
                });
            };
            CasEventSink sink;
            RuntimeUnderTest runtime_holder(
                backend, layout,
                MountConfig{
                    .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
                    .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; },
                    .worker_factory = factory,
                    .lease_wait_predicate_false_hook_for_test = [&]
                    {
                        std::call_once(pause_once, [&]
                        {
                            waiter_holds_driver_mutex.store(true, std::memory_order_release);
                            predicate_sampled_false.count_down();
                            release_predicate.wait();
                            waiter_holds_driver_mutex.store(false, std::memory_order_release);
                        });
                    },
                    .terminal_publication_driver_lock_contended_hook_for_test = [&]
                    {
                        release_waiter();
                    },
                    .terminal_publication_driver_lock_acquired_hook_for_test = [&]
                    {
                        if (waiter_holds_driver_mutex.load(std::memory_order_acquire))
                            publication_entered_while_held.store(true, std::memory_order_release);
                        release_waiter();
                    }},
                "test", sink, runtimeRenewBudget(), [] { return false; });
            /// Declared after the runtime so it runs first: a failed expectation must not leave the
            /// lease thread inside the hook while the runtime's destructor joins it.
            SCOPE_EXIT({ release_waiter(); });
            CasMountRuntime & runtime = *runtime_holder;
            runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
            const uint64_t anchor = runtime.startRenewer();
            runtime.armMountFence(uuid, 1, anchor + 1000);
            if (wait == Wait::ReclaimBackoff)
            {
                /// A request before the start makes the first pass a reclaim, which fails and backs off.
                runtime.tripMountLost();
                runtime.scheduleRemount();
            }
            runtime.startBackgroundWorkers(std::chrono::hours(1));

            predicate_sampled_false.wait();
            if (terminal == PoolLifecycle::IdentityLost)
            {
                runtime.tripMountLost();
                runtime.enterIdentityLost();
            }
            else
            {
                runtime.enterVanished(PoolLifecycle::VanishedReplaced, "injected replacement during a lease wait");
            }
            const bool exited_without_stop = exits.waitForAtLeast(1);
            runtime.stopBackgroundWorkers();
            EXPECT_FALSE(publication_entered_while_held.load())
                << "the publication must not take driver_mutex while the waiter holds it";
            EXPECT_TRUE(exited_without_stop);
            EXPECT_EQ(exits.count(), 1u);
            runtime.finishTeardown(false);
        }
    }
}
```

Kept: the serialization assertion and "the thread exits without a stop", for both terminal states.
Changed: the hook sits in the loop's waits; the publisher is the test thread; two waits (spec test
17). The predicate hook runs under `driver_mutex`, so the publisher's `try_lock` in
`lockTerminalPublication` fails and its contended hook releases the waiter; without the
serialization the acquired hook would see the waiter still holding the mutex.

5. `CASPoolRemount.VanishedReasonPreparationFailureLeavesTerminalTransitionRetryable` (:3917-3976):
replace `exits.waitForAtLeast(2)` with `exits.waitForAtLeast(1)`, `both_exited_without_stop` with
`exited_without_stop` (both lines), and `EXPECT_EQ(exits.count(), 2u);` with
`EXPECT_EQ(exits.count(), 1u);`. The retry assertions stay.

6. `CASPoolRemount.WorkerConstructionRollbackFailsOpenClosed` (:3978-4010) becomes, whole:

```cpp
TEST(CASPoolRemount, WorkerConstructionRollbackFailsOpenClosed)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-worker-failure");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    RuntimeWorkerFactory factory = [](std::function<void()>) -> ThreadFromGlobalPool
    {
        throw DB::Exception(DB::ErrorCodes::NETWORK_ERROR, "injected runtime worker construction failure");
    };
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind, MountClaimResult::Claimed);
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }, .worker_factory = factory},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    EXPECT_THROW(runtime.startBackgroundWorkers(std::chrono::milliseconds(10)), DB::Exception);
    EXPECT_FALSE(runtime.mayMutate());
    EXPECT_FALSE(runtime.workersRunningForTest());
    runtime.finishTeardown(false);
}
```

Kept: a failed start closes the fence and leaves no thread. The second case (the second factory call
failing) is gone with the second thread.

7. `CASPoolRemount.ExternalLossDuringRenewalUsesOneRecoveryGeneration` (:4012-4071): replace the
remount callback body

```cpp
        {
            ++remount_calls;
            ++fresh_epochs;
            fenceOutMount(*backend, layout.mountKey("test"));
            const MountClaimResult fresh = claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 2, wall_ms, 1000);
            EXPECT_EQ(fresh.kind, MountClaimResult::Claimed);
            if (fresh.kind != MountClaimResult::Claimed)
                return false;
            runtime_ptr->installRenewer(uuid, 2, [&] { return wall_ms; });
            const uint64_t fresh_anchor = runtime_ptr->startRenewer();
            runtime_ptr->setProcessEpoch(2, std::memory_order_release);
            runtime_ptr->setLiveWriterEpoch(2);
            runtime_ptr->armMountFence(uuid, 2, fresh_anchor + 1000);
            runtime_ptr->noteRemounted();
            remount_barrier.arriveAndWait();
            return true;
        });
```

with

```cpp
        {
            ++remount_calls;
            runtime_ptr->beginReclaim();
            ++fresh_epochs;
            fenceOutMount(*backend, layout.mountKey("test"));
            const MountClaimResult fresh = claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 2, wall_ms, 1000);
            EXPECT_EQ(fresh.kind, MountClaimResult::Claimed);
            if (fresh.kind != MountClaimResult::Claimed)
                return false;
            runtime_ptr->installRenewer(uuid, 2, [&] { return wall_ms; });
            const uint64_t fresh_anchor = runtime_ptr->startRenewer();
            runtime_ptr->setProcessEpoch(2, std::memory_order_release);
            runtime_ptr->setLiveWriterEpoch(2);
            EXPECT_TRUE(runtime_ptr->armIfAdmissible(fresh_anchor + 1000));
            remount_barrier.arriveAndWait();
            return true;
        });
```

Kept: one call, one fresh epoch, generation 1, one loss count.

8. `CASPoolRemount.ImmediatePostRemountRenewalFailureIsNotDropped` (:4162-4221): in the `call == 1`
branch replace

```cpp
                runtime_ptr->installRenewer(uuid, 2, [&] { return wall_ms; });
                const uint64_t fresh_anchor = runtime_ptr->startRenewer();
                runtime_ptr->armMountFence(uuid, 2, fresh_anchor + 10'000);
                runtime_ptr->noteRemounted();
```

with

```cpp
                runtime_ptr->installRenewer(uuid, 2, [&] { return wall_ms; });
                const uint64_t fresh_anchor = runtime_ptr->startRenewer();
                EXPECT_TRUE(runtime_ptr->armIfAdmissible(fresh_anchor + 10'000));
```

and insert `runtime_ptr->beginReclaim();` as the first statement of the `if (call == 1)` block,
before `fenceOutMount`. Kept: two calls, generation 2.
`ConcurrentRemountRequestIsProcessedAfterActiveGeneration` stays as it is: its callback arms nothing
and the loop acknowledges its snapshot.

9. `CASMountRuntime.ForgetEndsAnUnboundedRenewal` (:4715-4774): replace

```cpp
    const bool both_exited = exits.waitForAtLeast(2);
    EXPECT_TRUE(both_exited) << "the worker loops must exit on the published intent";
```

with

```cpp
    const bool exited = exits.waitForAtLeast(1);
    EXPECT_TRUE(exited) << "the lease loop must exit on the published intent";
```

and in its doc comment replace `both workers exit` with `the lease thread exits`. The request and
wait counts, the generation and `mayMutate` stay.

Unchanged and expected green: `ConcurrentRemountRequestIsProcessedAfterActiveGeneration`,
`DirectRenewIsRefusedForBackgroundConfiguredRuntimeAfterStop`, `MemoryLimitDoesNotEndTheLeaseThread`,
`DeterministicWorkerFailureFencesWithoutWaitingForCadence`, `StopWakesTheRetryWaitOfAnUnboundedRenewal`,
`AStaleSuccessIsFollowedAtOnceByTheNextRenewal`, the five `CASMountRuntime.Expiry*` tests,
`PreSendCancellationAllowsFarewellButAmbiguityDoesNot`, `WholeChainResultsAreNumberedAndStepLabelled`,
`ThrowingEventSinkAfterCommitLeavesRuntimeLive`, `LeaseLossHasOneOperationalOwner`,
`LiveForgetDoesNotCountOperationalLeaseLoss`, `ShutdownGuardRefusesToArmRemount`, the four
re-anchor tests Task 2 deletes (`StaleRemountAnchorPerformsParkedRedo`,
`TheRenewerRedoRenewsOnTheOpenPlane`, `ParkedRedo*ObservabilityPrecedesRemountResult`: the re-anchor
now runs on the lease thread, which `checkRenewerOwner` admits from cycle 3 on), every
`CASHeartbeat*`, `CASEvent*`, `CASMountLease*`, `CASLifecycle*`, `CASForget*`.

- [ ] **Step 15: Build, run the tests of this task, then the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t1_c2_build.log 2>&1; echo NINJA_EXIT=$? >> build/t1_c2_build.log
tail -1 build/t1_c2_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASPoolRemount.*:CASMountRuntime.*:CASPoolShutdown.*:CASPool.*:CASForget.*:CASLifecycleCondition.*:CASLifecycleSnapshot.*:CASFenceGeneration.*:CASRefContiguousAlloc.*' > build/t1_c2_tests.log 2>&1; echo GATE_EXIT=$? >> build/t1_c2_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t1_c2_tests.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CAS*' > build/t1_c2_gate.log 2>&1; echo GATE_EXIT=$? >> build/t1_c2_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t1_c2_gate.log | tail -20
```

Expected: `NINJA_EXIT=0`; both runs `GATE_EXIT=0`, no `FAILED`. Step 10's red run is the evidence
for `ACommitConsumedAfterARemountRequestCannotOverwriteTheReclaimedDeadline`: on one thread the
consumption precedes the reclaim by construction, so no local mutation of this tree makes it fail. A
failure outside this task's tables: stop, revert the cycle, save the diff under
`.superpowers/sdd/task-1/`, report.

- [ ] **Step 16: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Common/setThreadName.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp \
  src/Disks/tests/gtest_cas_pool.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Run the CAS mount lease and its reclaim on one thread" \
  -m "The lease loop serves a pending remount request first, then renews at the cadence. A renewal ends early on a pending request, and its result is consumed under driver_mutex before the loop reads the generations again, so a commit can no longer overwrite the deadline a later reclaim armed. The remount worker, the nine RenewalDriverState values, DriverLease, admitRenewerCall and ThreadName::CAS_REMOUNT are removed; the startup, remount and direct renewals stay as plain calls. Tests of the parking protocol are deleted; tests of guarantees that remain are adapted to one thread." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

#### Cycle 3: only the lease thread drives the renewer while it runs

- [ ] **Step 17: Write the failing test**

`T/gtest_cas_pool.cpp`: add `#include <base/defines.h>` after `#include <base/scope_guard.h>` (:16),
`#include <cstdlib>` after `#include <condition_variable>` (:23), and `extern const int LOGICAL_ERROR;`
to the `DB::ErrorCodes` block (:34-45). Append after
`ACommitConsumedAfterARemountRequestCannotOverwriteTheReclaimedDeadline`:

```cpp
/// While the lease thread runs, no other thread may replace, reset or start the renewer. Constructing
/// a `LOGICAL_ERROR` exception aborts under a debug or sanitizer build, so there the same contract is
/// a death expectation.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASMountRuntime, OnlyTheLeaseThreadReplacesTheRenewerWhileItRuns)
#else
TEST(CASMountRuntimeDeathTest, OnlyTheLeaseThreadReplacesTheRenewerWhileItRuns)
#endif
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-renewer-owner");
    uint64_t wall_ms = 1000;
    const uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    DB::Cas::tests::ManualBarrier admitted;
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; },
                    .renewal_admitted_hook_for_test = [&] { admitted.arriveAndWait(); }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    SCOPE_EXIT({ admitted.release(); });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));
    /// The lease thread is held between its decision to renew and the renewal, with no lock held.
    admitted.waitUntilArrived();

#ifndef DEBUG_OR_SANITIZER_BUILD
    const auto expect_owner_refusal = [](const char * what, const std::function<void()> & call)
    {
        try
        {
            call();
            ADD_FAILURE() << what << " from a test thread must be refused while the lease thread runs";
        }
        catch (const DB::Exception & e)
        {
            EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR) << what;
            EXPECT_NE(e.message().find("only the lease thread"), String::npos) << what << ": " << e.message();
        }
    };
    expect_owner_refusal("installRenewer", [&] { runtime.installRenewer(uuid, 2, [&] { return wall_ms; }); });
    expect_owner_refusal("renewerReset", [&] { runtime.renewerReset(); });
    expect_owner_refusal("startRenewer", [&] { (void)runtime.startRenewer(); });
#else
    /// The child exits through `std::_Exit` if the call does not abort: running exit handlers in a
    /// forked child can block on a thread pool mutex a vanished thread held.
    EXPECT_DEATH({ runtime.installRenewer(uuid, 2, [&] { return wall_ms; }); std::_Exit(0); }, "only the lease thread");
    EXPECT_DEATH({ runtime.renewerReset(); std::_Exit(0); }, "only the lease thread");
    EXPECT_DEATH({ (void)runtime.startRenewer(); std::_Exit(0); }, "only the lease thread");
#endif

    admitted.release();
    runtime.stopBackgroundWorkers();
    /// With the thread joined the caller drives the renewer again.
    EXPECT_NO_THROW(runtime.renewerReset());
    runtime.finishTeardown(false);
}
```

- [ ] **Step 18: Build and run it to see it fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t1_c3_red_build.log 2>&1; echo NINJA_EXIT=$? >> build/t1_c3_red_build.log
tail -1 build/t1_c3_red_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASMountRuntime*.OnlyTheLeaseThreadReplacesTheRenewerWhileItRuns' > build/t1_c3_red_tests.log 2>&1; echo GATE_EXIT=$? >> build/t1_c3_red_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT\|Failure\|refused\|Which is\|Expected" build/t1_c3_red_tests.log | tail -30
```

Expected on `build` (not a sanitizer build, so the `#ifndef` arm runs): `NINJA_EXIT=0`; the test
FAILS. `installRenewer` and `renewerReset` do not throw ("... from a test thread must be refused while
the lease thread runs"); `startRenewer` throws `LOGICAL_ERROR` with "startRenewer without a renewer",
so the message assertion fails. Released, the lease thread finds no renewer and its rescue path
trips the fence and exits; in this build `chassert` does not abort. `GATE_EXIT=1`. Before cycle 2 the
same calls were refused by a driver state with "renewer replacement requires Dormant or Parked
renewal ownership"; cycle 2 removed that refusal, so the red run here shows no refusal at all.

- [ ] **Step 19: Implement the ownership check**

`RH`: add `#include <thread>` after `#include <set>` (:20).

`RH`, private, after the `canArm` declaration add

```cpp
    /// Throws `LOGICAL_ERROR` when a lease thread runs and the caller is not it. Requires `driver_mutex`.
    void checkRenewerOwner() const;
```

`RH`, members, after `bool workers_stop_requested = false;` add

```cpp
    /// The lease thread, from its first statement until `stopBackgroundWorkers` joins it.
    std::thread::id lease_thread_id;
```

`R`, add after `CasMountRuntime::canArm`

```cpp
void CasMountRuntime::checkRenewerOwner() const
{
    if (workers_started && lease_thread_id != std::this_thread::get_id())
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "CAS mount runtime: only the lease thread may drive the renewer while it runs");
}
```

`workers_started` is set before the thread exists and cleared after the join, and `lease_thread_id`
is empty until the thread sets it: in that window every caller is refused, the lease thread included,
and the lease thread calls nothing that checks before it sets its id.

`R`, `CasMountRuntime::installRenewer`, replace

```cpp
    /// The previous renewer is destroyed after the unlock.
    std::lock_guard lock(driver_mutex);
    std::swap(mount_renewer, replaced);
```

with

```cpp
    /// The previous renewer is destroyed after the unlock.
    std::lock_guard lock(driver_mutex);
    checkRenewerOwner();
    std::swap(mount_renewer, replaced);
```

`R`, `CasMountRuntime::renewerReset`, replace

```cpp
    std::unique_ptr<MountLeaseRenewer> released;
    std::lock_guard lock(driver_mutex);
    std::swap(mount_renewer, released);
```

with

```cpp
    std::unique_ptr<MountLeaseRenewer> released;
    std::lock_guard lock(driver_mutex);
    checkRenewerOwner();
    std::swap(mount_renewer, released);
```

`R`, `CasMountRuntime::startRenewer`, replace

```cpp
        std::lock_guard lock(driver_mutex);
        if (!mount_renewer)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: startRenewer without a renewer");
```

with

```cpp
        std::lock_guard lock(driver_mutex);
        checkRenewerOwner();
        if (!mount_renewer)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: startRenewer without a renewer");
```

`R`, `CasMountRuntime::renewRenewerOnce`, replace

```cpp
        if (caller == RenewCaller::Direct && config.background_watermark)
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "CAS mount runtime: direct renewal is disabled when background ownership is configured");
        if (!mount_renewer)
```

with

```cpp
        if (caller == RenewCaller::Direct && config.background_watermark)
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "CAS mount runtime: direct renewal is disabled when background ownership is configured");
        checkRenewerOwner();
        if (!mount_renewer)
```

The direct-renewal refusal stays first, so `DirectRenewIsRefusedForBackgroundConfiguredRuntimeAfterStop`
keeps its message.

`R`, `CasMountRuntime::startBackgroundWorkers`, replace

```cpp
        workers_started = true;
        workers_stop_requested = false;
        renewal_period = period;
```

with

```cpp
        workers_started = true;
        workers_stop_requested = false;
        lease_thread_id = {};
        renewal_period = period;
```

and in its `catch (...)` replace

```cpp
            workers_stop_requested = true;
            workers_started = false;
            driver_cv.notify_all();
```

with

```cpp
            workers_stop_requested = true;
            workers_started = false;
            lease_thread_id = {};
            driver_cv.notify_all();
```

`R`, `CasMountRuntime::renewalLoop`, replace

```cpp
    LockMemoryExceptionInThread memory_exception_lock(VariableContext::Global);

    /// The wake condition of every wait in this loop; the reclaim backoff ignores a request, which is
```

with

```cpp
    LockMemoryExceptionInThread memory_exception_lock(VariableContext::Global);
    {
        std::lock_guard lock(driver_mutex);
        lease_thread_id = std::this_thread::get_id();
    }

    /// The wake condition of every wait in this loop; the reclaim backoff ignores a request, which is
```

`R`, `CasMountRuntime::stopBackgroundWorkers`, replace

```cpp
        std::lock_guard lock(driver_mutex);
        workers_started = false;
        driver_cv.notify_all();
```

with

```cpp
        std::lock_guard lock(driver_mutex);
        workers_started = false;
        lease_thread_id = {};
        driver_cv.notify_all();
```

Critical sections: all under `driver_mutex`, reading or writing `workers_started` and
`lease_thread_id`; the throw unwinds the `lock_guard`. `installRenewer` and `renewerReset` destroy the
old renewer after the unlock. Who passes the check: the opening thread at an open (no thread yet:
`startRenewer`, `renewRenewerForStartupOnce`, `installRenewer`), the lease thread in a reclaim
(`installRenewer`, `startRenewer`, `renewRenewerForRemountOnce`), `finishTeardown` and FORGET's
`renewerReset` (after the join), a test thread when no lease thread runs.

- [ ] **Step 20: Build, list both arms, run, then the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t1_c3_build.log 2>&1; echo NINJA_EXIT=$? >> build/t1_c3_build.log
tail -1 build/t1_c3_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && build/src/unit_tests_dbms --gtest_list_tests --gtest_filter='CASMountRuntime*.OnlyTheLeaseThread*' > build/t1_c3_list.log 2>&1; cat build/t1_c3_list.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASMountRuntime*.OnlyTheLeaseThreadReplacesTheRenewerWhileItRuns:CASPoolRemount.*:CASMountRuntime.*' > build/t1_c3_tests.log 2>&1; echo GATE_EXIT=$? >> build/t1_c3_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t1_c3_tests.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CAS*' > build/t1_c3_gate.log 2>&1; echo GATE_EXIT=$? >> build/t1_c3_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t1_c3_gate.log | tail -20
```

Expected: `NINJA_EXIT=0`; the list shows `CASMountRuntime.` with
`OnlyTheLeaseThreadReplacesTheRenewerWhileItRuns` and no `CASMountRuntimeDeathTest.`; both runs
`GATE_EXIT=0`, no `FAILED`.

The sanitizer arm. Never run `cmake` or `ninja` in `build_asan`: the controller owns that build and
runs the `CAS*` gate under ASan on the plan's final tree. Check whether its binary exists and is newer
than the last source edit of this task:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ls -l --time-style=full-iso build_asan/src/unit_tests_dbms src/Disks/tests/gtest_cas_pool.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h 2>&1
```

If the binary exists and its time is later than all three sources, run only:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( flock build_asan/.wok-build.lock timeout 1200 build_asan/src/unit_tests_dbms --gtest_filter='CASMountRuntimeDeathTest.*' > build_asan/t1_c3_death.log 2>&1; echo GATE_EXIT=$? >> build_asan/t1_c3_death.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build_asan/t1_c3_death.log | tail -10
```

Expected: `CASMountRuntimeDeathTest.OnlyTheLeaseThreadReplacesTheRenewerWhileItRuns` runs and passes,
`GATE_EXIT=0`. Record the result in the task report. Otherwise report the death-test arm as "written,
not compiled here; verified by the controller's ASan gate" and continue.

What the release build already compiles: the whole test body except the expectation block. Only the
suite name and the three expectations sit under `DEBUG_OR_SANITIZER_BUILD`. This keeps the pattern
in `src/Disks/tests` (for example `CASRequests.KeyBindingThrowsBeforeAnyRequest` and
`CASRequestsDeathTest.KeyBindingThrowsBeforeAnyRequest`), where the death arm is compiled only in debug and
sanitizer builds. The three `EXPECT_DEATH` lines are therefore the only code the release build never
compiles.

- [ ] **Step 21: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp \
  src/Disks/tests/gtest_cas_pool.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Let only the CAS lease thread drive the renewer while it runs" \
  -m "installRenewer, renewerReset, startRenewer and every renewal check ownership under driver_mutex: while a lease thread runs, a call from any other thread is a LOGICAL_ERROR. Before the thread starts and after it is joined, the caller owns the renewer." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

#### Cycle 4: the doc

- [ ] **Step 22: Rewrite the passages about two workers and parking**

Docs only: no build and no red run; the gate of cycle 3 stands.

`docs/en/antalya/cas/architecture/mounts-and-leases.md`, section `{#mount-state-machines}`, replace

```markdown
`IdentityLost`, `VanishedReplaced` and `VanishedForgotten` are terminal and absorbing: the remount
and GC threads self-exit, and there is deliberately no auto-revive — an identity disappearing
under a live mount is an operator-level event.
```

with

```markdown
`IdentityLost`, `VanishedReplaced` and `VanishedForgotten` are terminal and absorbing: the lease
thread and the GC threads exit, and there is deliberately no auto-revive — an identity disappearing
under a live mount is an operator-level event.
```

Section `{#mount-lifecycle}`, replace

```markdown
**Writable open** runs in a strict order: bootstrap-residual proof, capability probe under a
random per-mount prefix, pool-meta create-or-validate, `validateServerRootId`, owner claim,
`allocateWriterEpoch`, mount claim and synchronous renewer start, arm the fence, then create and
release the runtime-owned renewal and remount workers before the writable pool becomes externally
visible. If the claim consumed the TTL, one fresh synchronous renewal re-anchors the deadline
before the fence is armed.
Failure to construct either worker joins the partial pair, closes the fence, and fails the writable
open. No incident path constructs a thread.

The renewal and remount workers are separate and long-lived under one stable `CasMountRuntime`.
`scheduleRemount` increments a requested-generation latch and wakes the persistent remount worker,
including while an older generation is active. Before renewer replacement, remount requests
`ParkRequested` and waits for the renewal driver to report `Parked`, which proves that no renewer call
is in flight. A successful remount handles only its snapshotted generation; a newer request is
processed before renewal resumes.

**Clean unmount:** request stop and join both persistent workers, drain the ref lanes, and only if
```

with

```markdown
**Writable open** runs in a strict order: bootstrap-residual proof, capability probe under a
random per-mount prefix, pool-meta create-or-validate, `validateServerRootId`, owner claim,
`allocateWriterEpoch`, mount claim and synchronous renewer start, arm the fence, then start the
runtime-owned lease thread before the writable pool becomes externally visible. If the claim consumed
the TTL, one fresh synchronous renewal re-anchors the deadline before the fence is armed.
Failure to start the lease thread closes the fence and fails the writable open. No incident path
constructs a thread.

One lease thread per writable mount renews the lease and runs the self-remount, one after the other.
`scheduleRemount` increments a requested-generation latch and wakes the thread; a pending request also
ends a renewal in progress. While the thread runs, only it replaces, starts or resets the renewer.

- A reclaim latches the fence first.
- It arms the fence, and reports `Live`, only when no newer request is pending, no stop is requested
  and the lifecycle is not terminal. The check and the arm are one step under the runtime's mutex,
  which a stop, a request and a FORGET intent also take.
- A reclaim acknowledges only the generation it served, so a request raised during a reclaim is served
  by the next reclaim before renewal resumes.
- A renewal's result is published before the thread looks at the requests again, so a renewal that
  finished before a request cannot overwrite the deadline of the reclaim that follows.

**Clean unmount:** request stop and join the lease thread, drain the ref lanes, and only if
```

Headings and anchors do not change.

- [ ] **Step 23: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- docs/en/antalya/cas/architecture/mounts-and-leases.md
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Describe the single CAS lease thread in the mounts and leases doc" \
  -m "Replace the text about separate renewal and remount workers and parking with the lease thread, the reclaim's latch and arming rule, and the generations." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

**Test summary**

| Kind | Tests |
|---|---|
| New (7) | `CASMountRuntime.NothingArmsWhileARemountRequestIsPending`, `.AReclaimAcknowledgesOnlyTheGenerationItServed` (regression, green before the task), `.AReclaimFinishedAfterTheForgetIntentArmsNothing`, `.StopDuringAReclaimJoinsTheThread`, `.ARemountRequestEndsTheRenewalAndTheSameThreadReclaims`, `.ACommitConsumedAfterARemountRequestCannotOverwriteTheReclaimedDeadline`, `.OnlyTheLeaseThreadReplacesTheRenewerWhileItRuns` (`CASMountRuntimeDeathTest.` under `DEBUG_OR_SANITIZER_BUILD`) |
| Deleted (4) | `DirectRenewCannotRaceWorkerStartOrRenewerReplacement`, `DueWorkerAdmissionIsReservedBeforeParkRequest`, `DueWorkerAdmissionIsReservedBeforeStop`, `TerminalDepositionDoesNotTouchRenewerAfterReplacement` |
| Merged (1) | `CASMountRuntime.ParkEndsAnUnboundedRenewal` into `ARemountRequestEndsTheRenewalAndTheSameThreadReclaims` |
| Adapted (10) | `ReclaimWaitsForTheRenewalToEnd` (was `RemountWaitsForRenewalParkedBeforeReplacement`), `TeardownJoinsTheLeaseThreadBeforeRelease` (was `TeardownJoinsBothWorkersBeforeRelease`), `NaturalTerminalTransitionMakesTheLeaseThreadExit` (was `...MakesBothPersistentWorkersSelfExit`), `ALeaseWaitCannotMissNaturalTerminalPublication` (was `ParkedRenewalCannotMissNaturalTerminalPublication`), `VanishedReasonPreparationFailureLeavesTerminalTransitionRetryable`, `WorkerConstructionRollbackFailsOpenClosed`, `ExternalLossDuringRenewalUsesOneRecoveryGeneration`, `ImmediatePostRemountRenewalFailureIsNotDropped`, `ForgetEndsAnUnboundedRenewal`, `CASForget.AReclaimDuringForgetArmsNothing` (was `ForgetReLatchesFenceAfterAReclaimReachesArmMountFence`); `ConcurrentRemountRequestIsProcessedAfterActiveGeneration` and `WholeChainResultsAreNumberedAndStepLabelled` compile and stay unchanged |

**Deviations from the contract**

1. Cycle 1 is split into 1a (extraction, no behaviour change) and 1b (the rule). Reason: the tests of
   the arming rule then fail against a real `armIfAdmissible` instead of failing to compile.
2. Five tests are renamed because their names describe two workers or parking (table above), and
   `ParkEndsAnUnboundedRenewal` is merged into the new test rather than kept beside it. Reason: a test
   name is prose that must not lie. Smallest adaptation if the names must stay: revert the renames;
   the bodies do not change.
3. `StopDuringAReclaimJoinsTheThread` asserts that every created thread exited, not that exactly one
   was created. Reason: it lands in cycle 1b, while two threads still exist; the one-thread property is
   `ARemountRequestEndsTheRenewalAndTheSameThreadReclaims`.
4. `lease_wait_predicate_false_hook_for_test` runs in all three waits of the loop (no `Active` renewer,
   cadence, reclaim backoff), not only the first as the contract's sketch shows. Reason: the adapted
   `ALeaseWaitCannotMissNaturalTerminalPublication` covers the cadence wait and the backoff (spec test
   17). The cadence wait therefore has a predicate (stop, terminal, request) and no longer wakes on
   every notification.
5. `publishRenewedDeadline` returns `std::optional<RestoredLease>` (`expired_ms`, `last_failure`)
   instead of `std::optional<uint64_t>`. Reason: the contract moves its log after the unlock and the
   log needs the failure text.
6. `installRenewer` and `renewerReset` destroy the replaced renewer after the unlock. Reason: no
   destructor runs under `driver_mutex`.
7. `canArm`'s parameter is unnamed in the definition (`uint64_t /*deadline_boot_ms*/`) because
   `-Weverything` rejects an unused parameter. Task 2 names it.
8. Comment edits outside the functions the contract lists: `Pool::mountWritable`, `Pool::~Pool`, the
   comment above `Pool::scheduleRemountForTest`, two comments in `SH` and two in `CasServerRoot.cpp`.
   Reason: they describe two workers or parking, which this task removes.
9. `Pool::mountWritable` still calls `armMountFence`; the contract's "production stops calling it" holds
   after Task 2, which owns `mountWritable`.
10. A reclaim that claimed and did not arm counts in `CASRemountSucceeded` and its MountRemount event
    reads `outcome = 'ok'`, `step = 'arm_fence'`, with the reason text of an armed reclaim. Task 2 sets
    `step = 'claimed_not_armed'` and the text (ruling O6). In this task the case is reached only on a
    stop, a terminal lifecycle or a newer pending request.
11. `docs/en/antalya/cas/operations/troubleshooting.md` (item 5, "remount park request", "parked
    result") and `monitoring.md` ("A parked terminal result") still mention parking. The brief limits
    this task's docs to `mounts-and-leases.md`; Task 3 edits both files and should replace these
    phrases with "a pending remount request".
12. `AReclaimAcknowledgesOnlyTheGenerationItServed` spends about 3 s of real time in two reclaim
    backoffs: the loop's backoff has no injected clock.

**Review focus**

1. A stop or FORGET during a reclaim's observation wait (`claimMountAwaitingExpiry`, up to TTL plus
   drift): the join waits for the observation, as it did for the remount worker. A reasonable person
   expects shutdown during a reclaim to finish within one observation window and one request; no test
   holds a reclaim inside the observation wait while stopping.
2. `reportImpossibleInterference` takes `driver_mutex` from the ref ledger while the lease thread may
   hold `remount_mutex` in a reclaim and quiesce ledger locks under it. The lease thread never holds
   `driver_mutex` while taking `remount_mutex` or a ledger lock, so the expected behaviour is no
   deadlock; whether a ledger lock is held at that call was not checked, and no test drives an
   interference report during a reclaim's quiesce.
3. A reclaim that throws `LOGICAL_ERROR` from inside `tryRemountOnce` is caught by the loop and retried
   with backoff, where the old remount worker did the same; under a sanitizer build such an exception
   aborts instead. A reasonable person expects a reclaim never to reach `checkRenewerOwner`'s refusal on
   the lease thread; no test calls `tryRemountOnce` from a test thread while a lease thread runs and
   reaches `installRenewer`.

---

### Task 2: Readiness through the loop (contract T5a; spec rules 1, 7, 14, test 14)

An open returns, and a reclaim reports `Live`, only when a ref append is admissible. A claim too old for
that leaves the fence latched; the lease thread's next renewal arms it under the arming rule. A loop
renewal ends early on a stop, a pending remount request or a terminal lifecycle, and no longer on a lost
fence. After this task `renewRenewerForStartupOnce` and `renewRenewerForRemountOnce` have no production
caller; Task 3 deletes them.

Paths: `W` = `/home/mfilimonov/workspace/ClickHouse/cas-95-1`, `CA` =
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`. Line numbers are from `W` at
`7e243502408`, each with its function. Where Task 1 rewrites a function, the hunk says "as left by
Task 1"; names and signatures are binding, positions are not.

**Files:**
- Modify: `CA/Pool/CasMountRuntime.h` (public `waitUntilArmed`; doc of `tripMountLost` ~:182; private `budgetAdmits`, `refAppendReservationMs` next to `canArm`; doc of `renewalLive`; doc of `MountConfig::renewal_live_for_test` ~:109)
- Modify: `CA/Pool/CasMountRuntime.cpp` (`admit` ~:146, `refAppendFenceOk` ~:162, `leaseExpiredRefusal` ~:188, `noteRenewRequest` ~:202, `canArm`, `armIfAdmissible`, the `Committed` branch of `consumeRenewResult`, `renewalLive`, new `budgetAdmits`, `refAppendReservationMs`, `waitUntilArmed`)
- Modify: `CA/Pool/CasPool.cpp` (`Pool::mountWritable` ~:865-922; `Pool::tryRemountOnce` finalizer ~:1263-1326 and tail ~:1544-1583)
- Modify: `docs/en/antalya/cas/architecture/mounts-and-leases.md`, `docs/en/antalya/cas/operations/monitoring.md`, `docs/en/antalya/cas/operations/debugging.md`
- Test: `src/Disks/tests/gtest_cas_pool.cpp`, `src/Disks/tests/gtest_cas_mount_runtime.cpp`

**Interfaces:**
- Consumes (Task 1): `beginReclaim`, `armIfAdmissible(uint64_t)`, `canArm(uint64_t)` (requires `driver_mutex`), `armFence(uint64_t, bool)`, `reclaim_generation`, `RenewCaller`, `renewalLive(RenewCaller)`, `consumeRenewResult(result, RenewCaller)` with its `driver_mutex` section, the one-thread `renewalLoop`, `startBackgroundWorkers` / `stopBackgroundWorkers`, `workers_started`, `lease_wait_predicate_false_hook_for_test`. `Pool::tryRemountOnce` calls `beginReclaim` first and `armIfAdmissible` last.
- Produces:
  - `bool CasMountRuntime::waitUntilArmed(uint64_t timeout_ms) const` (public).
  - `canArm` with the deadline term: `budgetAdmits(deadline, bootMsNow(), refAppendReservationMs()) == Fence::Admit::Ok`.
  - `armIfAdmissible` that latches the fence (`tripFenceWithoutOperationalLoss`) when it does not arm and the fence is not lost.
  - A committed renewal arms a lost fence under `canArm` (every `RenewCaller`).
  - `renewalLive(RenewCaller::Loop)` = no stop, no pending request, `!remountTerminal()`; `lost` and `lifecycle() != Live` are gone.
  - Private `Fence::Admit budgetAdmits(uint64_t deadline_boot_ms, uint64_t now_boot_ms, uint64_t needed_ms) const`, `uint64_t refAppendReservationMs() const`.
  - The MountRemount step `claimed_not_armed` (outcome `ok`, counted in `CASRemountSucceeded`).
  - Refusal text of `leaseExpiredRefusal`: "...; writes resume when a renewal restores it".

**Cycles and commits.** Four commits, each leaving the tree built and the gate green.

| Cycle | Subject | New tests | Deleted | Adapted |
|---|---|---|---|---|
| 1 | Arming rule gets its deadline term; the latch; a renewal arms; rule 7 | 5 | 0 | 0 |
| 2 | The expiry refusal text | 0 | 0 | 1 |
| 3 | The open and the reclaim go through the loop | 5 | 4 | 3 |
| 4 | Docs | 0 | 0 | 0 |

Test inventory for this task (contract section 4, rows T5a):

| Test | Kind | Cycle | Fails before the cycle because |
|---|---|---|---|
| `CASMountRuntime.AReadinessRenewalArmsOnlyWithRoomForARefAppend` | new; spec test 14, cases 3, 4, 5 | 1 | `armIfAdmissible` arms a claim with 30 ms left |
| `CASMountRuntime.ARenewalIsSentUnderALatchedFence` | new; rule 7 | 1 | `renewalLive(Loop)` ends the renewal on `lost`; it raises a remount |
| `CASMountRuntime.TheForgetIntentAloneEndsARenewal` | new; rule 7, `RenewNotAfterIntent` | 1 | with the intent and no trip the renewal retries to the request bound |
| `CASMountRuntime.ATripAloneIsRearmedByTheNextRenewal` | new; ruling O4 | 1 | the renewal ends on `lost`; a reclaim is requested; nothing re-arms |
| `CASMountRuntime.ARenewalArmsNothingWhileARequestIsPending` | new; rule 14, `NoArmWhileRequestPending` | 1 | regression: green before (no renewal arms); red shown once by dropping the pending term of `canArm` |
| `CASMountRuntime.ExpiredLeaseRefusalSaysWritesResume` | adapted (one assertion) | 2 | old text |
| `CASMountRuntime.OpenAndRemountReportReadyOnlyWhenWritable` | new; spec test 14, cases 1, 6, 7 | 3 | the open re-anchors on the opening thread; the reclaim re-anchors and reports `Live` at `publish_live` |
| `CASMountRuntime.AnOpenWhoseReadinessRenewalKeepsFailingFailsAfterOneTtl` | new; spec test 14, case 2 | 3 | the startup re-anchor gives up at its lease bound with `NETWORK_ERROR` after about 6 requests; no lease thread ran |
| `CASPool.AnOpenWithoutALeaseThreadFailsWhenItsClaimIsTooOld` | new; ruling O1 | 3 | the open re-anchors and succeeds |
| `CASMountRuntime.ADefinitiveAnswerDuringReadinessIsServedByAReclaim` | new; readiness after a definitive answer | 3 | `waitUntilArmed` does not exist (compile error) |
| `CASMountRuntime.AnOpenFailsWhenTheLeaseThreadEndedOnItsOwn` (release) / `CASMountRuntimeDeathTest.AnOpenFailsWhenTheLeaseThreadEndedOnItsOwn` (debug, sanitizer) | new; the loop's own error path during readiness | 3 | `waitUntilArmed` does not exist (compile error) |
| `CASPoolRemount.StaleRemountAnchorPerformsParkedRedo` | deleted | 3 | the re-anchor inside the remount is gone |
| `CASPoolRemount.TheRenewerRedoRenewsOnTheOpenPlane` | deleted | 3 | the same, on the open plane |
| `CASPoolRemount.ParkedRedoRecoveryObservabilityPrecedesRemountResult` | deleted | 3 | no renewal runs under `remount_mutex`, so no deferred report |
| `CASPoolRemount.ParkedRedoFailureObservabilityPrecedesRemountResult` | deleted | 3 | the same |
| `CASPool.StartupArmRedoesLeaseWriteWhenTheClaimConsumesTtl` | adapted | 3 | same counts; the third write is the loop's renewal; the open returns armed |
| `CASMountOpenWaits.PublicationHorizonUsesTheEnvelope` | adapted and moved | 3 | `background_watermark = true`; the window is two envelopes plus the margin, strict, no period term |
| `CASPoolRemount.RemountRenewerRedoUsesTheEnvelope` | adapted | 3 | asserts `Live` or not, and the step, at the MountRemount event |

Untouched and expected green after every cycle: `CASMountRuntime.ForgetEndsAnUnboundedRenewal` (the intent ends it now),
`StopWakesTheRetryWaitOfAnUnboundedRenewal`, `AStaleSuccessIsFollowedAtOnceByTheNextRenewal`, the five
`CASMountRuntime.Expiry*` / `ASuccessEndsTheFailureTextOfItsRun` / `EachRestoredExpiryIsCountedOnce` /
`AnExpiryEndedByAFenceIsNotCountedAsARestore` tests, `CASLifecycleSnapshot.*`,
`CASPoolRemount.RemountArmAnchorsAtClaimAttemptNotResponseTime` (see Cycle 3, why its clock queue does not
shift), `CASPoolRemount.WholeChainResultsAreNumberedAndStepLabelled` (a fresh claim arms: `publish_live`),
`CASForget.*`.

**Why the expiry bookkeeping keeps working through readiness and reclaims.** Each claim below is pinned by an
assertion in the tests of this task, not by a new test.
- `leaseExpiredSinceBootMs` reads empty while the fence is latched, because `leaseExpiredAt` requires `!lost`.
  Asserted before the thread starts, during the first readiness request, after the stale success and after the
  short success (`AReadinessRenewalArmsOnlyWithRoomForARefAppend`).
- A renewal that arms the fence is not a restore: `publishRenewedDeadline` computes `expired_at` while the fence
  is still lost, so `CASMountLeaseExpired` does not move. Asserted in the same test and in
  `AnOpenWhoseReadinessRenewalKeepsFailingFailsAfterOneTtl`.
- `armFence` resets `lease_expired_at_boot_ms` (Task 1's order), so an arm never carries an old expiry.
- The failure text: a stale success keeps it; a success with a future deadline clears it, armed or not.
  Asserted after the stale and after the short success. A failed open names it.
- `noteRenewRequest` keeps counting attempts and keeping the text; it now also wakes `waitUntilArmed`.

---

#### Cycle 1: the arming rule gets its deadline term; the latch; a renewal arms; rule 7

- [ ] **Step 1: Write the failing tests**

Append to `$W/src/Disks/tests/gtest_cas_pool.cpp`, at the end of the file (after
`TEST(CASMountRuntime, AnExpiryEndedByAFenceIsNotCountedAsARestore)`). They use `ExpiryScriptBackend`,
`RuntimeRenewBackend`, `RuntimeUnderTest`, `WorkerExitLatch`, `runtimeRenewBudget`, `expiryAdmitName`,
`eventCount` and the `kExpiry*` constants, all defined earlier in the same file.

```cpp
namespace
{
/// Clock values of the runtime readiness cases. With `runtimeRenewBudget` a ref append needs
/// 2 x 10 + 20 = 40 ms of lease. The claim starts at 100 s and the open decides with 30 ms left.
constexpr uint64_t kReadinessDecideBootMs = kExpiryDeadlineBootMs - 30;
/// The first readiness renewal fails 24 times, 1.3 s each, and commits with a start more than a TTL ago.
constexpr uint64_t kReadinessStaleCommitBootMs = kReadinessDecideBootMs + kExpiryFailedPuts * kExpiryFailedPutMs;
static_assert(kReadinessStaleCommitBootMs >= kReadinessDecideBootMs + kExpiryTtlMs);
/// The second commits with 35 ms of its lease left: in the future, but under the 40 ms a ref append needs.
constexpr uint64_t kReadinessShortCommitBootMs = kReadinessStaleCommitBootMs + kExpiryTtlMs - 35;
/// Far above any request count these tests reach, so only a regression hits it.
constexpr uint32_t kReadinessRequestBound = 100;

struct ReadinessView
{
    const char * admit = "";
    bool may_mutate = true;
    PoolLifecycle lifecycle = PoolLifecycle::IdentityLost;
    bool expired = true;
    String failure;
    uint64_t generation = 0;
    bool ref_append_ok = false;
};

ReadinessView readinessViewOf(CasMountRuntime & runtime)
{
    return ReadinessView{
        .admit = expiryAdmitName(runtime.admit(runtime.fenceGeneration(), 0)),
        .may_mutate = runtime.mayMutate(),
        .lifecycle = runtime.lifecycle(),
        .expired = runtime.leaseExpiredSinceBootMs().has_value(),
        .failure = runtime.lastRenewFailure(),
        .generation = runtime.fenceGeneration(),
        .ref_append_ok = runtime.refAppendFenceOk(),
    };
}
}

/// The open's closed state, at the runtime level: a claim with too little lease for a ref append leaves
/// the fence latched, and only a loop renewal whose own deadline leaves that room arms it. A stale success
/// and a success with too little room arm nothing, and each is followed at once by the next renewal.
TEST(CASMountRuntime, AReadinessRenewalArmsOnlyWithRoomForARefAppend)
{
    /// Everything the hooks capture is declared before the backend and the runtime that store them.
    const Layout layout("runtime-readiness-room");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{kExpiryClaimBootMs};
    std::atomic<uint32_t> puts{0};
    std::atomic<uint32_t> remount_calls{0};
    std::atomic<bool> request_bound_hit{false};
    uint32_t loop_passes = 0;
    std::vector<uint64_t> admitted_at;
    ReadinessView during_first_put;
    ReadinessView after_stale;
    ReadinessView after_short;
    ReadinessView after_arm;
    DB::Cas::tests::ManualBarrier armed_pass;
    CasMountRuntime * runtime_ptr = nullptr;
    const uint64_t lost_before = eventCount(ProfileEvents::CASMountLeaseLost);
    const uint64_t expired_before = eventCount(ProfileEvents::CASMountLeaseExpired);
    CasEventSink sink;
    auto backend = std::make_shared<ExpiryScriptBackend>();

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
                if (loop_passes == 2)
                    after_stale = readinessViewOf(*runtime_ptr);
                else if (loop_passes == 3)
                    after_short = readinessViewOf(*runtime_ptr);
                else if (loop_passes == 4)
                {
                    after_arm = readinessViewOf(*runtime_ptr);
                    armed_pass.arriveAndWait();
                }
            },
            .renewal_admitted_hook_for_test = [&] { admitted_at.push_back(boot_ms.load()); },
            .renewal_live_for_test = [&]
            {
                if (puts.load() < kReadinessRequestBound)
                    return true;
                request_bound_hit = true;
                return false;
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

    /// Case 5, before the thread starts: the claim leaves 30 ms of its lease, a ref append needs 40.
    boot_ms = kReadinessDecideBootMs;
    const uint64_t generation_before = runtime.fenceGeneration();
    EXPECT_FALSE(runtime.armIfAdmissible(anchor + kExpiryTtlMs)) << "a claim without room for a ref append arms nothing";
    const uint64_t latched_generation = runtime.fenceGeneration();
    EXPECT_EQ(latched_generation, generation_before + 1) << "the latch ends the unarmed incarnation";
    EXPECT_STREQ(expiryAdmitName(runtime.admit(latched_generation, 0)), "LostOrRearmed");
    EXPECT_FALSE(runtime.mayMutate());
    EXPECT_EQ(runtime.lifecycle(), PoolLifecycle::Live) << "the open's closed state; nothing was lost";
    EXPECT_FALSE(runtime.leaseExpiredSinceBootMs().has_value()) << "a latched fence is not an expiry";
    EXPECT_EQ(eventCount(ProfileEvents::CASMountLeaseLost), lost_before) << "the latch counts no lease loss";

    backend->on_put = [&](uint32_t put_no)
    {
        ++puts;
        if (put_no == 1)
            during_first_put = readinessViewOf(*runtime_ptr);
        if (put_no <= kExpiryFailedPuts)
        {
            boot_ms.fetch_add(kExpiryFailedPutMs);
            return true;
        }
        /// Put 25 commits the first renewal, stale. Put 26, the second renewal, lands with 35 ms left.
        if (put_no == kExpiryFailedPuts + 2)
            boot_ms.store(kReadinessShortCommitBootMs);
        return false;
    };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(kExpiryPeriodMs));

    bool arrived = true;
    try
    {
        armed_pass.waitUntilArrived();
    }
    catch (const DB::Exception &)
    {
        arrived = false;
    }
    if (!arrived)
    {
        runtime.stopBackgroundWorkers();
        runtime.finishTeardown(false);
        FAIL() << "no renewal armed the fence; request bound hit: " << request_bound_hit.load();
    }

    /// Case 5, while the readiness renewal itself is sent.
    EXPECT_STREQ(during_first_put.admit, "LostOrRearmed") << "the latched fence refuses a write while the renewal is sent";
    EXPECT_FALSE(during_first_put.may_mutate);
    EXPECT_FALSE(during_first_put.expired);

    /// Case 3: the first renewal started at 129.97 s and committed at 161.17 s, past its own start + TTL.
    EXPECT_STREQ(after_stale.admit, "LostOrRearmed") << "a stale success arms nothing";
    EXPECT_EQ(after_stale.lifecycle, PoolLifecycle::Live);
    EXPECT_EQ(after_stale.generation, latched_generation);
    EXPECT_FALSE(after_stale.expired) << "a latched fence reports no expiry, also after a stale success";
    EXPECT_NE(after_stale.failure.find("injected renewal timeout"), String::npos)
        << "a stale success does not end the run of trouble: " << after_stale.failure;
    ASSERT_EQ(admitted_at.size(), 3u);
    EXPECT_EQ(admitted_at[0], kReadinessDecideBootMs) << "the first renewal is due at once";
    EXPECT_EQ(admitted_at[1], kReadinessStaleCommitBootMs) << "a stale success is followed at once";

    /// Case 4: the second renewal started at 161.17 s and committed at 191.135 s with 35 ms left.
    EXPECT_STREQ(after_short.admit, "LostOrRearmed") << "a success with less lease than a ref append needs arms nothing";
    EXPECT_EQ(after_short.generation, latched_generation);
    EXPECT_TRUE(after_short.failure.empty()) << "a deadline in the future ends the run of trouble: " << after_short.failure;
    EXPECT_FALSE(after_short.expired);
    EXPECT_EQ(admitted_at[2], kReadinessShortCommitBootMs) << "a short success is followed at once";

    /// The third renewal started at 191.135 s with the whole TTL ahead: it arms, from its own start.
    EXPECT_STREQ(after_arm.admit, "Ok");
    EXPECT_TRUE(after_arm.ref_append_ok) << "the arm leaves room for a ref append";
    EXPECT_EQ(after_arm.lifecycle, PoolLifecycle::Live);
    EXPECT_EQ(after_arm.generation, latched_generation + 1) << "one arm, one new generation";
    EXPECT_FALSE(after_arm.expired);
    EXPECT_EQ(eventCount(ProfileEvents::CASMountLeaseLost), lost_before) << "readiness is not a lease loss";
    EXPECT_EQ(eventCount(ProfileEvents::CASMountLeaseExpired), expired_before) << "an arm is not a restore";
    EXPECT_EQ(puts.load(), kExpiryFailedPuts + 3);
    EXPECT_FALSE(request_bound_hit.load());
    EXPECT_EQ(remount_calls.load(), 0u);

    armed_pass.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// A trip that comes with no remount request, no intent and no stop does not end a loop renewal: its
/// requests keep going out under the latched fence until one commits.
TEST(CASMountRuntime, ARenewalIsSentUnderALatchedFence)
{
    const Layout layout("runtime-renew-under-latch");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{kExpiryClaimBootMs};
    std::atomic<uint32_t> puts{0};
    std::atomic<uint32_t> remount_calls{0};
    std::atomic<bool> request_bound_hit{false};
    uint32_t loop_passes = 0;
    std::vector<String> admit_at_put;
    DB::Cas::tests::ManualBarrier third_put;
    DB::Cas::tests::ManualBarrier second_pass;
    CasMountRuntime * runtime_ptr = nullptr;
    CasEventSink sink;
    auto backend = std::make_shared<ExpiryScriptBackend>();

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
                    boot_ms.store(kExpiryFirstStartBootMs);
                else if (loop_passes == 2)
                    second_pass.arriveAndWait();
            },
            .renewal_live_for_test = [&]
            {
                if (puts.load() < kReadinessRequestBound)
                    return true;
                request_bound_hit = true;
                return false;
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
    runtime.armMountFence(uuid, 1, anchor + kExpiryTtlMs);

    /// Puts 1 to 4 fail, 1.3 s each; put 5 commits at 115.2 s, inside the lease of the 100 s claim.
    backend->on_put = [&](uint32_t put_no)
    {
        ++puts;
        admit_at_put.push_back(expiryAdmitName(runtime_ptr->admit(runtime_ptr->fenceGeneration(), 0)));
        if (put_no == 3)
            third_put.arriveAndWait();
        if (put_no <= 4)
        {
            boot_ms.fetch_add(kExpiryFailedPutMs);
            return true;
        }
        return false;
    };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(kExpiryPeriodMs));

    third_put.waitUntilArrived();
    runtime.tripMountLost();
    third_put.release();
    bool arrived = true;
    try
    {
        second_pass.waitUntilArrived();
    }
    catch (const DB::Exception &)
    {
        arrived = false;
    }
    if (!arrived)
    {
        runtime.stopBackgroundWorkers();
        runtime.finishTeardown(false);
        FAIL() << "the renewal never finished; request bound hit: " << request_bound_hit.load();
    }

    EXPECT_EQ(puts.load(), 5u) << "the renewal kept sending after the trip until it committed";
    EXPECT_EQ(admit_at_put, (std::vector<String>{"Ok", "Ok", "Ok", "LostOrRearmed", "LostOrRearmed"}))
        << "puts 4 and 5 went out under the latched fence";
    EXPECT_EQ(runtime.remountRequestedGenerationForTest(), 0u) << "a lost fence alone requests no remount";
    EXPECT_EQ(remount_calls.load(), 0u);
    EXPECT_FALSE(request_bound_hit.load());

    second_pass.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// A FORGET intent published while the renewal retries ends it at its next admission check, with no trip
/// and no remount request; the loop exits.
TEST(CASMountRuntime, TheForgetIntentAloneEndsARenewal)
{
    const Layout layout("runtime-intent-alone");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{kExpiryClaimBootMs};
    std::atomic<uint32_t> puts{0};
    std::atomic<uint32_t> remount_calls{0};
    std::atomic<bool> request_bound_hit{false};
    uint32_t loop_passes = 0;
    DB::Cas::tests::ManualBarrier third_put;
    WorkerExitLatch exits;
    RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
    {
        return ThreadFromGlobalPool([&, body = std::move(worker_body)]
        {
            body();
            exits.recordExit();
        });
    };
    const uint64_t lost_before = eventCount(ProfileEvents::CASMountLeaseLost);
    CasEventSink sink;
    auto backend = std::make_shared<ExpiryScriptBackend>();

    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, kExpiryTtlMs).kind,
              MountClaimResult::Claimed);

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(kExpiryTtlMs),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms.load(); },
            .worker_factory = factory,
            .renewal_before_driver_lock_hook_for_test = [&]
            {
                if (++loop_passes == 1)
                    boot_ms.store(kExpiryFirstStartBootMs);
            },
            .renewal_live_for_test = [&]
            {
                if (puts.load() < kReadinessRequestBound)
                    return true;
                request_bound_hit = true;
                return false;
            }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            ++remount_calls;
            return false;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + kExpiryTtlMs);

    /// Every put fails; put 3 is held until the intent is published.
    backend->on_put = [&](uint32_t put_no)
    {
        ++puts;
        if (put_no == 3)
            third_put.arriveAndWait();
        boot_ms.fetch_add(kExpiryFailedPutMs);
        return true;
    };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(kExpiryPeriodMs));

    third_put.waitUntilArrived();
    const uint64_t generation_at_intent = runtime.remountRequestedGenerationForTest();
    runtime.publishVanishedIntent();
    third_put.release();

    const bool exited = exits.waitForAtLeast(1);
    EXPECT_TRUE(exited) << "the loop exits on the published intent";
    EXPECT_EQ(puts.load(), 3u) << "no request after the intent";
    EXPECT_FALSE(request_bound_hit.load()) << "the intent, not the request bound, ended the renewal";
    EXPECT_EQ(runtime.remountRequestedGenerationForTest(), generation_at_intent);
    EXPECT_EQ(remount_calls.load(), 0u);
    EXPECT_FALSE(runtime.mayMutate()) << "the early end trips the fence";
    EXPECT_EQ(eventCount(ProfileEvents::CASMountLeaseLost), lost_before) << "a FORGET is not a lease loss";

    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// A trip with no request, no intent and no stop: the next renewal that commits with room for a ref
/// append arms the fence again under the same epoch and reports `Live`.
TEST(CASMountRuntime, ATripAloneIsRearmedByTheNextRenewal)
{
    const Layout layout("runtime-trip-alone");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{kExpiryClaimBootMs};
    std::atomic<uint32_t> remount_calls{0};
    uint32_t loop_passes = 0;
    uint64_t generation_after_trip = 0;
    ReadinessView after_renewal;
    DB::Cas::tests::ManualBarrier second_pass;
    CasMountRuntime * runtime_ptr = nullptr;
    const uint64_t lost_before = eventCount(ProfileEvents::CASMountLeaseLost);
    CasEventSink sink;
    auto backend = std::make_shared<ExpiryScriptBackend>();

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
                    runtime_ptr->tripMountLost();
                    generation_after_trip = runtime_ptr->fenceGeneration();
                }
                else if (loop_passes == 2)
                {
                    after_renewal = readinessViewOf(*runtime_ptr);
                    second_pass.arriveAndWait();
                }
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
    runtime.armMountFence(uuid, 1, anchor + kExpiryTtlMs);
    runtime.startBackgroundWorkers(std::chrono::milliseconds(kExpiryPeriodMs));

    second_pass.waitUntilArrived();
    EXPECT_STREQ(after_renewal.admit, "Ok") << "the renewal at 110 s re-armed the fence";
    EXPECT_EQ(after_renewal.lifecycle, PoolLifecycle::Live);
    EXPECT_EQ(after_renewal.generation, generation_after_trip + 1);
    EXPECT_EQ(runtime.liveWriterEpoch(), 0u) << "no reclaim ran: the epoch the test never published is unchanged";
    EXPECT_EQ(runtime.remountRequestedGenerationForTest(), 0u);
    EXPECT_EQ(remount_calls.load(), 0u);
    EXPECT_EQ(eventCount(ProfileEvents::CASMountLeaseLost), lost_before + 1) << "the trip counted its loss once";
    second_pass.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// A renewal that commits while a remount request is pending does not arm a latched fence, whatever room
/// its deadline leaves: the reclaim that follows starts from a fence that refuses writes.
TEST(CASMountRuntime, ARenewalArmsNothingWhileARequestIsPending)
{
    const Layout layout("runtime-no-arm-while-pending");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    String admit_at_reclaim = "unobserved";
    bool may_mutate_at_reclaim = true;
    PoolLifecycle lifecycle_at_reclaim = PoolLifecycle::Live;
    DB::Cas::tests::ManualBarrier renewal_put;
    DB::Cas::tests::ManualBarrier reclaim_entered;
    CasMountRuntime * runtime_ptr = nullptr;
    CasEventSink sink;
    auto backend = std::make_shared<RuntimeRenewBackend>();
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            admit_at_reclaim = expiryAdmitName(runtime_ptr->admit(runtime_ptr->fenceGeneration(), 0));
            may_mutate_at_reclaim = runtime_ptr->mayMutate();
            lifecycle_at_reclaim = runtime_ptr->lifecycle();
            reclaim_entered.arriveAndWait();
            return false;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    const String key = layout.mountKey("test");
    const uint64_t writes_before = backend->putOverwriteCount(key);
    backend->barrier = &renewal_put;
    backend->fault = RuntimeRenewBackend::Fault::BlockThenDelegate;
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));

    renewal_put.waitUntilArrived();
    runtime.tripMountLost();
    runtime.scheduleRemount();
    renewal_put.release();

    reclaim_entered.waitUntilArrived();
    EXPECT_EQ(backend->putOverwriteCount(key), writes_before + 1) << "the renewal committed, with 1000 ms of lease ahead";
    EXPECT_EQ(admit_at_reclaim, "LostOrRearmed") << "a commit consumed under a pending request arms nothing";
    EXPECT_FALSE(may_mutate_at_reclaim);
    EXPECT_EQ(lifecycle_at_reclaim, PoolLifecycle::TransientNotLive);
    reclaim_entered.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}
```

Notes for the implementer:
- `ATripAloneIsRearmedByTheNextRenewal` calls `tripMountLost` from the loop's pass hook. `tripMountLost` touches atomics only and takes no lock, so it is safe on the lease thread with no lock held.
- `runtime.liveWriterEpoch()` is 0 in that fixture because nothing publishes an epoch; a reclaim would have run the callback, which counts. The assertion fails if any code path publishes one.

- [ ] **Step 2: Build and run them to see them fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t2_c1_red_build.log 2>&1; echo NINJA_EXIT=$? >> build/t2_c1_red_build.log
tail -1 build/t2_c1_red_build.log; grep -a -n "error:" build/t2_c1_red_build.log | head -40
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASMountRuntime.AReadinessRenewalArmsOnlyWithRoomForARefAppend:CASMountRuntime.ARenewalIsSentUnderALatchedFence:CASMountRuntime.TheForgetIntentAloneEndsARenewal:CASMountRuntime.ATripAloneIsRearmedByTheNextRenewal:CASMountRuntime.ARenewalArmsNothingWhileARequestIsPending' > build/t2_c1_red.log 2>&1; echo GATE_EXIT=$? >> build/t2_c1_red.log )
grep -a "FAILED\|PASSED\|GATE_EXIT\|Failure\|Expected\|Which is" build/t2_c1_red.log | tail -60
```

Expected: `NINJA_EXIT=0` (everything used exists after Task 1). Four tests fail, none hangs (`GATE_EXIT=1`):
- `AReadinessRenewalArmsOnlyWithRoomForARefAppend`: `Value of: runtime.armIfAdmissible(anchor + kExpiryTtlMs)` `Actual: true`, then `during_first_put.admit` is `"Ok"` instead of `"LostOrRearmed"`.
- `ARenewalIsSentUnderALatchedFence`: `puts.load()` is 3, `remountRequestedGenerationForTest()` is 1, `remount_calls` is 1.
- `TheForgetIntentAloneEndsARenewal`: `puts.load()` is 100 and `request_bound_hit` is true.
- `ATripAloneIsRearmedByTheNextRenewal`: `after_renewal.admit` is `"LostOrRearmed"`, `remount_calls` is 1, the requested generation is 1.
- `ARenewalArmsNothingWhileARequestIsPending` PASSES: it is a regression guard; no renewal arms before this task. Record the green run. Step 4 shows it red against a mutation.

Paste the failure lines into the task report.

- [ ] **Step 3: Implement**

`$W/CA/Pool/CasMountRuntime.h`, public section, after the declaration of `armIfAdmissible` (as left by Task 1):

```cpp
    /// Blocks until the fence is armed. Gives up after `timeout_ms` on the fence clock, on a stop, on a
    /// terminal lifecycle, or when no lease thread runs. Returns whether the fence is armed.
    bool waitUntilArmed(uint64_t timeout_ms) const;
```

Same header, replace the doc of `tripMountLost` (~:182, "Permanently latch the local fence as lost for this runtime incarnation."):

```cpp
    /// Latch the fence lost and count one lease loss. Every production caller pairs the trip with a
    /// remount request, a published FORGET intent or a stop. A trip that comes alone is re-armed by the
    /// next renewal that commits with room for a ref append.
    void tripMountLost();
```

Same header, private section, next to `canArm` (as left by Task 1):

```cpp
    /// `admit`'s budget verdict for a lease that ends at `deadline_boot_ms`, at `now_boot_ms`.
    Fence::Admit budgetAdmits(uint64_t deadline_boot_ms, uint64_t now_boot_ms, uint64_t needed_ms) const;
    /// What a ref append reserves: a write and the read that settles it, two attempt envelopes.
    uint64_t refAppendReservationMs() const;
```

Same header, replace the doc of `renewalLive` (as left by Task 1) with:

```cpp
    /// The renewal's liveness. Every caller ends on a stop. A loop renewal also ends on a pending remount
    /// request and on a terminal lifecycle, which includes a published FORGET intent; a lost fence alone
    /// does not end it. FALSE ends the renewal.
```

Same header, `MountConfig::renewal_live_for_test` (~:109), replace the last sentence of its comment
"FALSE ends it exactly as a lost fence does." with "FALSE ends the renewal early." The field stays.

`$W/CA/Pool/CasMountRuntime.cpp`, replace `CasMountRuntime::admit` and `CasMountRuntime::refAppendFenceOk`
(~:146-171) with:

```cpp
Fence::Admit CasMountRuntime::admit(uint64_t admitted_generation, uint64_t needed_ms) const
{
    if (mount_fence.lost.load(std::memory_order_acquire) || fenceGeneration() != admitted_generation)
        return Fence::Admit::LostOrRearmed;
    const uint64_t now = bootMsNow();
    return budgetAdmits(mount_fence.deadline_boot_ms.load(std::memory_order_acquire), now, needed_ms);
}

Fence::Admit CasMountRuntime::budgetAdmits(uint64_t deadline_boot_ms, uint64_t now_boot_ms, uint64_t needed_ms) const
{
    if (now_boot_ms >= deadline_boot_ms)
        return Fence::Admit::NoBudget;
    /// Compared by subtraction rather than as the sum `needed_ms + margin`, which can wrap for an
    /// absurd configuration and then read as if there were room.
    const uint64_t remaining = deadline_boot_ms - now_boot_ms;
    if (needed_ms >= remaining || cas_request_budget.lease_safety_margin_ms >= remaining - needed_ms)
        return Fence::Admit::NoBudget;
    return Fence::Admit::Ok;
}

uint64_t CasMountRuntime::refAppendReservationMs() const
{
    /// Two envelopes: a write and its settlement read, which is what `writeLoop` reserves.
    const uint64_t envelope_ms = cas_request_budget.attemptEnvelopeMs();
    return envelope_ms > std::numeric_limits<uint64_t>::max() / 2 ? std::numeric_limits<uint64_t>::max() : 2 * envelope_ms;
}

bool CasMountRuntime::refAppendFenceOk() const
{
    return admit(fenceGeneration(), refAppendReservationMs()) == Fence::Admit::Ok;
}
```

`admit` keeps its order of reads (the clock, then the deadline) and its verdicts; `refAppendFenceOk` reads
the clock once, as before.

Replace the body of `CasMountRuntime::canArm` (as left by Task 1, whose definition leaves the parameter
unnamed because it is unused there; name it `deadline_boot_ms`) with:

```cpp
bool CasMountRuntime::canArm(uint64_t deadline_boot_ms) const
{
    /// The clock is read last and only when every other term holds.
    return !workers_stop_requested
        && !remountTerminal()
        && remount_requested_generation <= remount_handled_generation
        && budgetAdmits(deadline_boot_ms, bootMsNow(), refAppendReservationMs()) == Fence::Admit::Ok;
}
```

Replace the body of `CasMountRuntime::armIfAdmissible` (as left by Task 1) with:

```cpp
bool CasMountRuntime::armIfAdmissible(uint64_t deadline_boot_ms)
{
    std::lock_guard lock(driver_mutex);
    remount_handled_generation = std::max(remount_handled_generation, reclaim_generation);
    const bool arm = canArm(deadline_boot_ms);
    if (arm)
    {
        armFence(deadline_boot_ms, /*report_live=*/true);
    }
    else
    {
        /// An open starts unarmed, which admits writes; a claim that does not admit a ref append must
        /// not leave it so. Latched before the deadline is published, so no reader sees the claim's
        /// deadline on an open fence. A reclaim arrives here already latched by `beginReclaim`.
        if (!mount_fence.lost.load(std::memory_order_acquire))
            tripFenceWithoutOperationalLoss();
        setMountDeadline(deadline_boot_ms);
    }
    driver_cv.notify_all();
    return arm;
}
```

Critical section: `driver_mutex` held; atomics and `bootMsNow` (an injected or `clock_gettime` read) only;
no I/O, no log, no join, no callback other than the test clock and the arm interposition hook Task 1
already runs here.

In `CasMountRuntime::consumeRenewResult`, inside the `std::lock_guard lock(driver_mutex);` section Task 1
writes, replace its `Committed` branch

```cpp
        if (result.outcome == MountRenewOutcome::Committed)
        {
            const uint64_t ttl_ms = static_cast<uint64_t>(config.mount_lease_ttl_ms.count());
            restored = publishRenewedDeadline(
                result.attempt_start_boot_ms > std::numeric_limits<uint64_t>::max() - ttl_ms
                    ? std::numeric_limits<uint64_t>::max()
                    : result.attempt_start_boot_ms + ttl_ms);
        }
```

with

```cpp
        if (result.outcome == MountRenewOutcome::Committed)
        {
            const uint64_t ttl_ms = static_cast<uint64_t>(config.mount_lease_ttl_ms.count());
            const uint64_t deadline_boot_ms = result.attempt_start_boot_ms > std::numeric_limits<uint64_t>::max() - ttl_ms
                ? std::numeric_limits<uint64_t>::max()
                : result.attempt_start_boot_ms + ttl_ms;
            restored = publishRenewedDeadline(deadline_boot_ms);
            /// A latched fence is armed by the first renewal whose own deadline leaves room for a ref append.
            if (mount_fence.lost.load(std::memory_order_acquire) && canArm(deadline_boot_ms))
                armFence(deadline_boot_ms, /*report_live=*/true);
        }
```

The section's `driver_cv.notify_all()` (after the `Terminal` switch) follows the arm. The arm applies to
every `RenewCaller`; only the loop and the test seam `renewWatermarkOnce` reach a committed result with a
lost fence after this task. The restore's `LOG_WARNING`, `reportMountRenewCompletion` and the rethrow stay
after the unlock; the arm adds no I/O, log or join to the section. `publishRenewedDeadline` runs first with
the fence still lost, so `leaseExpiredAt` reads empty and an arming renewal returns no `RestoredLease`.

Replace the body of `CasMountRuntime::renewalLive(RenewCaller caller)` (as left by Task 1) with:

```cpp
bool CasMountRuntime::renewalLive(RenewCaller caller) const
{
    std::lock_guard lock(driver_mutex);
    if (workers_stop_requested)
        return false;
    if (caller != RenewCaller::Loop)
        return true;
    /// A lost fence alone does not end it: the renewal that makes an open or a reclaim ready runs under
    /// one, and every trip that must end it comes with a request, an intent or a stop.
    return remount_requested_generation <= remount_handled_generation && !remountTerminal();
}
```

`config.renewal_live_for_test` stays where Task 1 evaluates it, ANDed in the `live` lambda of
`renewalEnvironment`; only the `Loop` terms change (Task 1's `lifecycle() == PoolLifecycle::Live` and
`!mount_fence.lost` go).

- [ ] **Step 4: Build, run the tests of this cycle, show the mutation, then the gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t2_c1_build.log 2>&1; echo NINJA_EXIT=$? >> build/t2_c1_build.log
tail -1 build/t2_c1_build.log; grep -a -n "error:" build/t2_c1_build.log | head -40
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASMountRuntime.*' > build/t2_c1_runtime.log 2>&1; echo GATE_EXIT=$? >> build/t2_c1_runtime.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t2_c1_runtime.log | tail -20
```

Expected: `NINJA_EXIT=0`, `GATE_EXIT=0`, every `CASMountRuntime.*` test passes.

Mutation for `ARenewalArmsNothingWhileARequestIsPending`: in `canArm`, delete the line
`&& remount_requested_generation <= remount_handled_generation`, rebuild (log `t2_c1_mut_build.log`), run
only that test (log `t2_c1_mut.log`). Expected: FAILED with
`Expected equality of these values: admit_at_reclaim Which is: "Ok"` and `lifecycle_at_reclaim` `Live`.
Paste the lines into the report, restore the line, confirm with `git -C $W diff -- CA/Pool/CasMountRuntime.cpp`
that the term is back, rebuild.

Gate:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CAS*' > build/t2_c1_gate.log 2>&1; echo GATE_EXIT=$? >> build/t2_c1_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t2_c1_gate.log | tail -20
```

Expected: `GATE_EXIT=0`, the passed count is Task 1's count plus 5. The Pool paths still re-anchor in this
cycle, so `canArm`'s new term does not refuse them: at an open or a reclaim whose window test passed, the
window (`period + 2 x envelope` before the safety-margined deadline) is stricter than the term; after a
re-anchor the deadline is fresh. Any other failure: stop, revert the cycle, save the diff under
`$W/.superpowers/sdd/t2-cycle1.diff`, report.

- [ ] **Step 5: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp src/Disks/tests/gtest_cas_pool.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Arm a latched CAS mount fence from a renewal with room for a ref append" -m "The arming rule gets its deadline term: a claim or a renewal arms the fence only when its deadline admits a ref append now. \`armIfAdmissible\` latches the fence when it does not arm, and a committed renewal arms a latched fence under the same rule. A loop renewal ends on a stop, a pending remount request or a terminal lifecycle; a lost fence alone no longer ends it." -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

#### Cycle 2: the refusal text of an expired lease

- [ ] **Step 1: Write the failing assertion**

`$W/src/Disks/tests/gtest_cas_mount_runtime.cpp`, `TEST(CASMountRuntime, ExpiredLeaseRefusalSaysWritesResume)`,
replace

```cpp
    EXPECT_NE(expired.find("writes resume when a renewal succeeds"), String::npos) << expired;
```

with

```cpp
    EXPECT_NE(expired.find("writes resume when a renewal restores it"), String::npos) << expired;
```

The rest of the test stays: the refusal names the expiry only while the caller's generation is current.

- [ ] **Step 2: Build and run it to see it fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t2_c2_red_build.log 2>&1; echo NINJA_EXIT=$? >> build/t2_c2_red_build.log
tail -1 build/t2_c2_red_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASMountRuntime.ExpiredLeaseRefusalSaysWritesResume' > build/t2_c2_red.log 2>&1; echo GATE_EXIT=$? >> build/t2_c2_red.log )
grep -a "FAILED\|PASSED\|GATE_EXIT\|writes resume" build/t2_c2_red.log | tail -20
```

Expected: FAILED; the printed text ends in `writes resume when a renewal succeeds`.

- [ ] **Step 3: Implement**

`$W/CA/Pool/CasMountRuntime.cpp`, `CasMountRuntime::leaseExpiredRefusal` (~:192):

```cpp
    return String("the mount lease expired and no renewal has restored it yet; "
                  "writes resume when a renewal restores it");
```

A renewal can succeed and leave the lease expired (a stale success); only a restore resumes writes.

- [ ] **Step 4: Build, run, gate**

Build to `t2_c2_build.log`; run `CASMountRuntime.*` to `t2_c2_runtime.log`; gate `CAS*` to `t2_c2_gate.log`,
with the commands of Cycle 1 Step 4. Expected: all pass; the gate count is unchanged from Cycle 1.

- [ ] **Step 5: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp src/Disks/tests/gtest_cas_mount_runtime.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Say that writes resume when a renewal restores the CAS mount lease" -m "A renewal can commit with a start more than a lease ago and leave the lease expired, so a success alone does not resume writes." -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

#### Cycle 3: the open and the reclaim go through the loop

**Ruling O1: production callers that open writable with `background_watermark = false`.** Enumerated in the
worktree with untruncated greps:

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 grep -n "background_watermark" -- src programs utils ':!src/Disks/tests'
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 grep -n -E "Pool::open\(|Pool::openForDecommission\(" -- src programs utils ':!src/Disks/tests'
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 grep -n -E "make_shared<ContentAddressedMetadataStorage>|ContentAddressedMetadataStorage\(" -- src programs utils ':!src/Disks/tests'
```

Hits that set or open (comments and readers omitted; the full output has 17, 4 and 4 lines):

| Hit | What it does |
|---|---|
| `ContentAddressedMetadataStorage.cpp:782` (`openPoolView`) | `background_watermark = context_available && !read_only` |
| `ContentAddressedMetadataStorage.cpp:836` (`openPoolView`) | the only `Pool::open` outside tests |
| `ContentAddressedMetadataStorage.cpp:877` (`startup`) | `openPoolView(static_cast<bool>(startup_context))`; `startup_context = context->lock()`, and an expired context throws `INVALID_STATE` (:848-853) |
| `MetadataStorageFactory.cpp:240` (`registerContentAddressedMetadataStorage`) | the only construction of `ContentAddressedMetadataStorage` outside tests; passes `Context::getGlobalContextInstance()`, already dereferenced at :234 |
| `CasPool.cpp:941` (`openForDecommission`) | forces `background_watermark = true` |
| `Tools/CasDecommission.cpp:172` | calls `openForDecommission` |

Verdict: no production caller opens writable without a lease thread. The context is never null, so
`context_available` is true whenever `startup` reaches `openPoolView`, and a writable open has
`background_watermark = true`. Ruling O1's failing open is reachable only from unit tests. No Deviation.

**Which tests O1 can break.** With `background_watermark = false`, today's re-anchor condition at the open and
the arming rule are the same inequality: the window fits iff `2 x envelope < deadline - margin - now`, iff
`2 x envelope + margin < deadline - now`, which is `budgetAdmits(deadline, now, 2 x envelope) == Ok`. So the
tests that O1 turns red are exactly the tests whose writable open without a lease thread re-anchors today.
Step 1 finds them by running the gate once with the re-anchor turned into a throw.

- [ ] **Step 1: Probe the tests that re-anchor an open without a lease thread**

`$W/CA/Pool/CasPool.cpp`, `Pool::mountWritable`, inside `if (!renewal_window_fits)`, before the line
`claim_anchor_boot_ms = store->mount_runtime.renewRenewerForStartupOnce();`, add temporarily:

```cpp
        if (!store->config.background_watermark)
            throw Exception(ErrorCodes::ABORTED, "READINESS-PROBE: an open without a lease thread re-anchored");
```

Build to `t2_c3_probe_build.log`, run the gate `CAS*` to `t2_c3_probe.log`, then
`grep -a "^\[  FAILED  \]" build/t2_c3_probe.log | sort -u`. Expected exactly one:
`CASMountOpenWaits.PublicationHorizonUsesTheEnvelope` (its `per_call_ms` 100 and 70 cases). Then
`git -C $W diff --stat` must show only `CasPool.cpp` with 2 insertions; restore it with
`git -C $W checkout -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp`.
If any other test fails: stop and report it with its name; do not adapt a test that is not in this task's
inventory.

- [ ] **Step 2: Write the failing tests, delete four, adapt three**

Delete from `$W/src/Disks/tests/gtest_cas_pool.cpp` (their subject, the re-anchor inside the remount and its
deferred report under `remount_mutex`, goes in Step 4 of this cycle):
- `TEST(CASPoolRemount, StaleRemountAnchorPerformsParkedRedo)` (~:4223-4264)
- `TEST(CASPoolRemount, ParkedRedoRecoveryObservabilityPrecedesRemountResult)` (~:4266-4339)
- `TEST(CASPoolRemount, ParkedRedoFailureObservabilityPrecedesRemountResult)` (~:4341-4415)
- `TEST(CASPoolRemount, TheRenewerRedoRenewsOnTheOpenPlane)` with its comment block (~:4993-5060)

After the deletion `ScopedParkedRenewalLogCapture` (~:2339) has no user; delete it too, or the build fails
on `-Wunused` in the anonymous namespace. Check with
`grep -n "ScopedParkedRenewalLogCapture" $W/src/Disks/tests/gtest_cas_pool.cpp`: zero lines after the
deletion.

Adapt `TEST(CASPool, StartupArmRedoesLeaseWriteWhenTheClaimConsumesTtl)` (~:2923-2988): replace its leading
comment and its two final assertions; the fixture and the counts stay.

```cpp
/// A claim path that consumed most of the lease must not arm the fence from that claim: the open waits for
/// the lease thread's first renewal, which writes the lease once more and arms from its own start.
TEST(CASPool, StartupArmRedoesLeaseWriteWhenTheClaimConsumesTtl)
```

and at its end:

```cpp
    auto store = DB::Cas::Pool::open(backend, cfg);
    ASSERT_NE(store, nullptr);

    ASSERT_EQ(backend->mount_writes.load(), 3)
        << "the fixture assumes exactly two mount writes before the open decides (the reclaim and the "
           "renewer's adopt, with the claim's start between them), then the loop's renewal";
    EXPECT_EQ(backend->mount_writes_after_stall.load(), 1)
        << "a claim that consumed the lease is followed by exactly one renewal before the open returns";
    EXPECT_TRUE(store->mayMutate()) << "the open returns with the fence armed";
```

Also replace the comment above `backend->on_second_mount_write` in that test with:

```cpp
    /// The renewer's adopt write stalls for 15 s of boot clock: 15 s of the lease are left, a ref append
    /// needs 2 x 7 s + 2 s = 16 s, so the open waits; the loop's renewal is due at once (10 s cadence).
```

Remove `TEST(CASMountOpenWaits, PublicationHorizonUsesTheEnvelope)` and its comment from its current place
(~:2758-2809). Its new version goes after `StartupArmRedoesLeaseWriteWhenTheClaimConsumesTtl`, because it uses
`StalledMountClaimBackend`, which is defined just above that test. A per-call clock (the old fixture) cannot
stay: with a lease thread, every read of the clock moves it, so the loop renews on its own and the write
count races. The new fixture ages the claim once, on the adopt write, and freezes the clock.

```cpp
/// The arming rule at an open reserves what a ref append needs: two attempt envelopes, connect cap
/// included, plus the margin, strictly, and no renewal period. With attempt 100 and cap 100 the envelope is
/// 300 ms, so a claim arms the fence only while more than 2 x 300 + 50 = 650 ms of its 1000 ms lease remain.
/// The 340 ms period keeps the lease thread from renewing an armed claim before the count is read.
TEST(CASMountOpenWaits, PublicationHorizonUsesTheEnvelope)
{
    /// Opens over a fenced predecessor whose adopt write ages the claim by `claim_age_ms`, and returns the
    /// conditional writes of the mount key made before the open returned.
    const auto mountWritesAtOpen = [](uint64_t claim_age_ms) -> int
    {
        auto backend = std::make_shared<StalledMountClaimBackend>();
        DB::Cas::Layout layout("pool");
        DB::Cas::tests::seedPoolMetaForRestart(*backend, "pool");
        backend->mount_key = layout.mountKey("s");
        MountLease prior;
        prior.server_uuid = DB::UInt128(0x42);
        prior.writer_epoch = 7;
        prior.seq = 7;
        prior.expires_at_ms = 1;
        prior.gc_fenced = true;
        prior.write_attempt_id = DB::UInt128{7};
        createObj(*backend, layout.mountKey("s"), encodeMountLease(prior));
        createObj(*backend, layout.epochKey("s"), encodeServerEpoch(ServerEpoch{.next_writer_epoch = 8}));
        /// Held in a shared atomic: the Pool can outlive this lambda's frame.
        auto fake_boot = std::make_shared<std::atomic<uint64_t>>(10'000);
        backend->on_second_mount_write = [fake_boot, claim_age_ms]
        {
            *fake_boot += claim_age_ms;
        };
        DB::Cas::PoolConfig cfg;
        cfg.pool_prefix = "pool";
        cfg.server_id = DB::UInt128(0x42);
        cfg.server_root_id = "s";
        cfg.background_watermark = true;
        cfg.mount_lease_ttl_ms = std::chrono::milliseconds(1000);
        cfg.mount_renew_period = std::chrono::milliseconds(340);
        cfg.cas_request_budget = CasRequestBudget{.attempt_timeout_ms = 100, .lease_safety_margin_ms = 50, .connect_timeout_cap_ms = 100};
        cfg.boot_ms_fn = [fake_boot]
        {
            return fake_boot->load();
        };
        cfg.wait_sleep_fn = [fake_boot](uint64_t ms)
        {
            *fake_boot += ms;
        };
        auto store = Pool::open(backend, cfg);
        EXPECT_TRUE(store && store->mayMutate()) << "claim_age_ms=" << claim_age_ms;
        return backend->mount_writes.load();
    };

    /// 1000 ms left: the claim arms; the reclaim and the adopt only.
    EXPECT_EQ(mountWritesAtOpen(0), 2);
    /// 700 ms left: the claim arms. A window with the period in it (340 + 600 = 940) would not fit here.
    EXPECT_EQ(mountWritesAtOpen(300), 2) << "the arming rule has no period term";
    /// 650 ms left, exactly the reservation: refused, strict like `admit`; the loop's renewal arms.
    EXPECT_EQ(mountWritesAtOpen(350), 3) << "an exact boundary must be refused";
    /// 600 ms left: two bare attempts plus the margin (250) would fit, two envelopes (650) do not.
    EXPECT_EQ(mountWritesAtOpen(400), 3) << "the reservation counts envelopes, not bare attempts";
}
```

Validation of that config: `340 + 2 x 300 + 50 = 990 < 1000`. In the two refused cases the loop's renewal is
due at once (`10'000 + 340 <= 10'350`), starts at the frozen clock, and arms with the whole lease ahead; the
next one is due 340 ms later on a clock that does not move, so the count is 3 when the open returns. In the
two armed cases the first renewal is due at 10'340 and the clock stays at or below 10'300.

Adapt `TEST(CASPoolRemount, RemountRenewerRedoUsesTheEnvelope)` (~:2811-2883): replace its comment and body.

```cpp
/// A reclaim arms the fence only when its claim still admits a ref append: more than two envelopes plus
/// the margin of lease, strictly, with no period term. With attempt 100 and cap 100 that is 650 ms of the
/// 1000 ms lease. A quiescence that leaves less reports success at step `claimed_not_armed` with the pool
/// not `Live`; the lease thread's next renewal arms it.
TEST(CASPoolRemount, RemountRenewerRedoUsesTheEnvelope)
{
    struct AtRemountEvent
    {
        String step;
        PoolLifecycle lifecycle = PoolLifecycle::IdentityLost;
        bool may_mutate = true;
    };
    /// One reclaim whose quiescence costs `quiesce_ms`; returns what the MountRemount event and the pool
    /// show while the lease thread is held inside the sink that reported it.
    const auto remountWithQuiesce = [](uint64_t quiesce_ms) -> AtRemountEvent
    {
        auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
        /// Held in shared state: the hooks below mutate it, and the Pool can outlive this lambda's frame.
        auto fake_boot = std::make_shared<std::atomic<uint64_t>>(1'000'000);
        auto committed = std::make_shared<DB::Cas::tests::ManualBarrier>();
        auto step = std::make_shared<String>();
        auto store = Pool::open(backend, PoolConfig{
            .pool_prefix = "remount-renewer-redo-envelope",
            .server_root_id = "test",
            .background_watermark = true,
            .event_sink = [committed, step](const CasEvent & event)
            {
                if (event.type == CasEventType::MountRemount && event.outcome == "ok")
                {
                    *step = event.detail.at("step");
                    committed->arriveAndWait();
                }
            },
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .mount_renew_period = std::chrono::milliseconds(100),
            .cas_request_budget = CasRequestBudget{.attempt_timeout_ms = 100, .lease_safety_margin_ms = 50, .connect_timeout_cap_ms = 100},
            .boot_ms_fn = [fake_boot]
            {
                return fake_boot->load();
            },
            .wait_sleep_fn = [fake_boot](uint64_t ms)
            {
                *fake_boot += ms;
            },
            .remount_quiesce_hook_for_test = [fake_boot, quiesce_ms]
            {
                *fake_boot += quiesce_ms;
            },
        });
        fenceOutMount(*backend, store->layout().mountKey("test"));
        EXPECT_TRUE(store->scheduleRemountForTest()) << "the remount must be latched with quiesce_ms=" << quiesce_ms;
        committed->waitUntilArrived();
        AtRemountEvent seen{.step = *step, .lifecycle = store->lifecycle(), .may_mutate = store->mayMutate()};
        committed->release();
        return seen;
    };

    for (uint64_t quiesce_ms : {0, 300})
    {
        const AtRemountEvent seen = remountWithQuiesce(quiesce_ms);
        EXPECT_EQ(seen.step, "publish_live") << "quiesce_ms=" << quiesce_ms;
        EXPECT_EQ(seen.lifecycle, PoolLifecycle::Live) << "quiesce_ms=" << quiesce_ms;
        EXPECT_TRUE(seen.may_mutate) << "quiesce_ms=" << quiesce_ms;
    }
    /// 350: 650 ms left, exactly the reservation, refused. 450: 550 ms left; two bare attempts plus the
    /// margin (250) would fit, two envelopes do not.
    for (uint64_t quiesce_ms : {350, 450})
    {
        const AtRemountEvent seen = remountWithQuiesce(quiesce_ms);
        EXPECT_EQ(seen.step, "claimed_not_armed") << "quiesce_ms=" << quiesce_ms;
        EXPECT_EQ(seen.lifecycle, PoolLifecycle::TransientNotLive) << "quiesce_ms=" << quiesce_ms;
        EXPECT_FALSE(seen.may_mutate) << "quiesce_ms=" << quiesce_ms;
    }
}
```

The sink writes `*step` before `arriveAndWait` and the test reads it after `waitUntilArrived`: the barrier's
mutex orders the two. The pool's state is read while the lease thread is held in the sink, after
`armIfAdmissible` and before the loop's next renewal.

Append the three new tests to `$W/src/Disks/tests/gtest_cas_pool.cpp`, at the end of the file (after the
tests of Cycle 1):

```cpp
namespace
{
/// Fails the conditional writes of the mount key that `on_mount_write` says to fail, with a timeout raised
/// before the store applied anything. The hook gets the 1-based number of the conditional write of the
/// mount key, counting every one: reclaims, adopts, renewals and a test's own fence-out.
class MountWriteScriptBackend final : public DB::Cas::InMemoryBackend
{
public:
    String mount_key;
    std::function<bool(uint32_t write_no)> on_mount_write;
    std::atomic<uint32_t> mount_writes{0};

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
        const std::optional<String> & expected_value, DB::Cas::TransportAccess & access) override
    {
        if (key == mount_key && expected_value)
        {
            const uint32_t write_no = ++mount_writes;
            if (on_mount_write && on_mount_write(write_no))
                throw Poco::TimeoutException("injected readiness renewal timeout");
        }
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }
};

const UInt128 kReadyUuid{0x42};
constexpr uint64_t kReadyClaimBootMs = 10'000;
/// The adopt write or the quiesce takes 20 s: the claim leaves 10 s of the default 30 s lease, and a ref
/// append needs 2 x 7 s + 2 s = 16 s with the default budget.
constexpr uint64_t kReadyClaimAgeMs = 20'000;
constexpr uint64_t kReadyTtlMs = 30'000;
constexpr uint64_t kReadyPeriodMs = 10'000;

/// Pool meta, and a fenced, long-expired predecessor under epoch 7 with the epoch object that minted it.
/// An open as `kReadyUuid` reclaims it at once and writes the mount key exactly twice (the reclaim, then
/// the renewer's adopt) before it decides whether to arm.
void seedFencedPredecessor(DB::Cas::Backend & backend, const String & pool_prefix)
{
    DB::Cas::tests::seedPoolMetaForRestart(backend, pool_prefix);
    const Layout layout(pool_prefix);
    MountLease prior;
    prior.server_uuid = kReadyUuid;
    prior.writer_epoch = 7;
    prior.seq = 7;
    prior.expires_at_ms = 1;
    prior.gc_fenced = true;
    prior.write_attempt_id = UInt128{7};
    createObj(backend, layout.mountKey("s"), encodeMountLease(prior));
    createObj(backend, layout.epochKey("s"), encodeServerEpoch(ServerEpoch{.next_writer_epoch = 8}));
}

/// A writable pool with a lease thread, the default lease, period and budget, and a boot clock that only
/// the test and the injected sleeps move.
PoolConfig readinessPoolConfig(const String & pool_prefix, const std::shared_ptr<std::atomic<uint64_t>> & fake_boot)
{
    PoolConfig config;
    config.pool_prefix = pool_prefix;
    config.server_id = kReadyUuid;
    config.server_root_id = "s";
    config.background_watermark = true;
    config.boot_ms_fn = [fake_boot]
    {
        return fake_boot->load();
    };
    config.wait_sleep_fn = [fake_boot](uint64_t ms)
    {
        *fake_boot += ms;
    };
    config.retry_sleep_fn = [fake_boot](uint64_t ms)
    {
        *fake_boot += ms;
    };
    return config;
}

/// What a readiness scenario records on the lease thread. Heap-owned and captured by value: a `Pool` can
/// outlive the test frame.
struct ReadinessSeen
{
    std::thread::id opening_thread;
    std::atomic<bool> renewal_on_opening_thread{true};
    std::atomic<uint64_t> renewal_boot_ms{0};
    std::atomic<uint32_t> failed_renewals{0};
    std::atomic<bool> request_bound_hit{false};
    std::atomic<Pool *> pool{nullptr};
    std::atomic<int> lifecycle_at_renewal{-1};
    std::atomic<int> may_mutate_at_renewal{-1};
    std::atomic<int> lifecycle_at_arm{-1};
    std::atomic<int> may_mutate_at_arm{-1};
    std::atomic<int> lifecycle_after_arm{-1};
    std::atomic<int> may_mutate_after_arm{-1};
    /// Written on the lease thread before the sink's barrier, read by the test after it.
    String remount_outcome;
    String remount_step;
};
}

/// Spec test 14, cases 1, 6 and 7. An open whose claim is too old returns only after the lease thread's
/// renewal armed the fence; a reclaim whose quiescence aged its claim the same way reports success with
/// the pool not `Live` and the fence latched, and the next renewal arms it. No sampled point sees the
/// fence armed while the pool is not `Live`.
TEST(CASMountRuntime, OpenAndRemountReportReadyOnlyWhenWritable)
{
    /// Case 1. The claim starts at 10 s and the adopt write ends at 30 s.
    {
        auto fake_boot = std::make_shared<std::atomic<uint64_t>>(kReadyClaimBootMs);
        auto seen = std::make_shared<ReadinessSeen>();
        seen->opening_thread = std::this_thread::get_id();
        auto backend = std::make_shared<MountWriteScriptBackend>();
        seedFencedPredecessor(*backend, "ready-open");
        backend->mount_key = Layout("ready-open").mountKey("s");
        backend->on_mount_write = [fake_boot, seen](uint32_t write_no)
        {
            if (write_no == 2)
            {
                *fake_boot += kReadyClaimAgeMs;
            }
            else if (write_no == 3)
            {
                seen->renewal_boot_ms = fake_boot->load();
                seen->renewal_on_opening_thread = std::this_thread::get_id() == seen->opening_thread;
            }
            return false;
        };
        auto store = Pool::open(backend, readinessPoolConfig("ready-open", fake_boot));
        ASSERT_TRUE(store);
        EXPECT_EQ(backend->mount_writes.load(), 3u) << "the reclaim, the adopt and one renewal";
        EXPECT_FALSE(seen->renewal_on_opening_thread.load()) << "the lease thread renews, not the opening thread";
        EXPECT_EQ(seen->renewal_boot_ms.load(), kReadyClaimBootMs + kReadyClaimAgeMs) << "the renewal is sent with no cadence wait";
        EXPECT_EQ(store->lifecycle(), PoolLifecycle::Live);
        EXPECT_TRUE(store->mayMutate());
        /// The claim's deadline (40 s) would leave 10 s at 30 s; only the renewal's (60 s) admits the append.
        EXPECT_NO_THROW(publishPart(store, "srv/ready-open", "x", "payload")) << "a ref append is admitted when the open returns";
    }

    /// Cases 6 and 7. A fresh open arms at once. Then a reclaim: claim at 10 s, quiescence until 30 s.
    {
        auto fake_boot = std::make_shared<std::atomic<uint64_t>>(kReadyClaimBootMs);
        auto seen = std::make_shared<ReadinessSeen>();
        auto remounted = std::make_shared<DB::Cas::tests::ManualBarrier>();
        auto renewed_after_arm = std::make_shared<DB::Cas::tests::ManualBarrier>();
        auto backend = std::make_shared<MountWriteScriptBackend>();
        seedFencedPredecessor(*backend, "ready-remount");
        const String mount_key = Layout("ready-remount").mountKey("s");
        backend->mount_key = mount_key;
        /// 1, 2: the open's reclaim and adopt. 3: the test's fence-out. 4, 5: the remount's claim and adopt.
        /// 6: the renewal that arms. 7: the renewal after it.
        backend->on_mount_write = [fake_boot, seen, renewed_after_arm](uint32_t write_no)
        {
            if (write_no == 6)
            {
                Pool * pool = seen->pool.load();
                seen->renewal_boot_ms = fake_boot->load();
                seen->lifecycle_at_renewal = static_cast<int>(pool->lifecycle());
                seen->may_mutate_at_renewal = pool->mayMutate();
            }
            else if (write_no == 7)
            {
                Pool * pool = seen->pool.load();
                seen->lifecycle_after_arm = static_cast<int>(pool->lifecycle());
                seen->may_mutate_after_arm = pool->mayMutate();
                renewed_after_arm->arriveAndWait();
            }
            return false;
        };
        PoolConfig config = readinessPoolConfig("ready-remount", fake_boot);
        config.event_sink = [seen, remounted](const CasEvent & event)
        {
            if (event.type != CasEventType::MountRemount)
                return;
            seen->remount_outcome = event.outcome;
            seen->remount_step = event.detail.at("step");
            remounted->arriveAndWait();
        };
        config.remount_quiesce_hook_for_test = [fake_boot]
        {
            *fake_boot += kReadyClaimAgeMs;
        };
        auto store = Pool::open(backend, config);
        ASSERT_TRUE(store);
        ASSERT_EQ(backend->mount_writes.load(), 2u) << "a fresh claim arms at once";
        seen->pool = store.get();
        /// Runs inside the arm, after the new generation and before `Live` and the open fence. It also makes
        /// the next renewal due at once, so its request shows the state the arm left.
        store->setArmMountFenceInterpositionHookForTest([seen, fake_boot]
        {
            Pool * pool = seen->pool.load();
            seen->lifecycle_at_arm = static_cast<int>(pool->lifecycle());
            seen->may_mutate_at_arm = pool->mayMutate();
            *fake_boot += kReadyPeriodMs;
        });
        const uint64_t succeeded_before = eventCount(ProfileEvents::CASRemountSucceeded);
        fenceOutMount(*backend, mount_key);
        ASSERT_TRUE(store->scheduleRemountForTest());

        remounted->waitUntilArrived();
        EXPECT_EQ(seen->remount_outcome, "ok");
        EXPECT_EQ(seen->remount_step, "claimed_not_armed");
        EXPECT_EQ(eventCount(ProfileEvents::CASRemountSucceeded), succeeded_before + 1);
        EXPECT_EQ(store->lifecycle(), PoolLifecycle::TransientNotLive) << "case 6: not Live after the reclaim";
        EXPECT_FALSE(store->mayMutate()) << "case 6: the fence stays latched";
        remounted->release();

        renewed_after_arm->waitUntilArrived();
        ASSERT_EQ(backend->mount_writes.load(), 7u);
        EXPECT_EQ(seen->renewal_boot_ms.load(), kReadyClaimBootMs + kReadyClaimAgeMs) << "the loop renewed at once";
        /// Case 7: at every sampled point, an armed fence comes with `Live`.
        EXPECT_EQ(seen->lifecycle_at_renewal.load(), static_cast<int>(PoolLifecycle::TransientNotLive));
        EXPECT_EQ(seen->may_mutate_at_renewal.load(), 0) << "the renewal is sent under the latched fence";
        EXPECT_EQ(seen->lifecycle_at_arm.load(), static_cast<int>(PoolLifecycle::TransientNotLive));
        EXPECT_EQ(seen->may_mutate_at_arm.load(), 0) << "inside the arm, before `Live`, the fence still refuses";
        EXPECT_EQ(seen->lifecycle_after_arm.load(), static_cast<int>(PoolLifecycle::Live));
        EXPECT_EQ(seen->may_mutate_after_arm.load(), 1) << "after the arm: `Live` and writable";
        renewed_after_arm->release();
    }
}

/// Spec test 14, case 2. Every readiness renewal fails: the open waits one lease on the fence clock, then
/// stops and joins the lease thread and fails, naming the last failed request.
TEST(CASMountRuntime, AnOpenWhoseReadinessRenewalKeepsFailingFailsAfterOneTtl)
{
    /// Longer than the largest spacing draw, so a failed request is retried at once.
    constexpr uint64_t failed_request_ms = 1'300;
    /// Far above the 24 failures one lease takes; reached only if the open never gives up.
    constexpr uint32_t max_failed_renewals = 10'000;
    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(kReadyClaimBootMs);
    auto seen = std::make_shared<ReadinessSeen>();
    auto exits = std::make_shared<WorkerExitLatch>();
    auto backend = std::make_shared<MountWriteScriptBackend>();
    seedFencedPredecessor(*backend, "ready-fail");
    backend->mount_key = Layout("ready-fail").mountKey("s");
    backend->on_mount_write = [fake_boot, seen](uint32_t write_no)
    {
        if (write_no == 2)
            *fake_boot += kReadyClaimAgeMs;
        if (write_no < 3)
            return false;
        if (seen->failed_renewals.load() >= max_failed_renewals)
        {
            seen->request_bound_hit = true;
            return false;
        }
        ++seen->failed_renewals;
        *fake_boot += failed_request_ms;
        return true;
    };
    PoolConfig config = readinessPoolConfig("ready-fail", fake_boot);
    config.worker_factory = [exits](std::function<void()> worker_body)
    {
        return ThreadFromGlobalPool([exits, body = std::move(worker_body)]
        {
            body();
            exits->recordExit();
        });
    };
    const uint64_t lost_before = eventCount(ProfileEvents::CASMountLeaseLost);
    const uint64_t expired_before = eventCount(ProfileEvents::CASMountLeaseExpired);

    int code = 0;
    String message;
    try
    {
        (void)Pool::open(backend, config);
        ADD_FAILURE() << "an open whose readiness renewal never succeeds must fail";
    }
    catch (const DB::Exception & e)
    {
        code = e.code();
        message = e.message();
    }

    EXPECT_EQ(code, DB::ErrorCodes::ABORTED) << message;
    EXPECT_NE(message.find("injected readiness renewal timeout"), String::npos) << "the failure names the last request: " << message;
    EXPECT_FALSE(seen->request_bound_hit.load()) << "the open never gave up";
    /// The wait starts at 30 s or later and ends once the fence clock passes one lease after its start:
    /// 23 failures reach 59.9 s, the 24th 61.2 s.
    EXPECT_GE(seen->failed_renewals.load(), 24u) << "the open waited one lease, not until a lease bound";
    EXPECT_EQ(exits->count(), 1u) << "the lease thread was joined before the open failed";
    EXPECT_EQ(eventCount(ProfileEvents::CASMountLeaseLost), lost_before) << "a failed open is not a lease loss";
    EXPECT_EQ(eventCount(ProfileEvents::CASMountLeaseExpired), expired_before);
}

/// Ruling O1: a writable open with no lease thread whose claim does not admit a ref append fails at once
/// with a retryable error, and sends no renewal.
TEST(CASPool, AnOpenWithoutALeaseThreadFailsWhenItsClaimIsTooOld)
{
    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(kReadyClaimBootMs);
    auto backend = std::make_shared<MountWriteScriptBackend>();
    seedFencedPredecessor(*backend, "ready-no-thread");
    backend->mount_key = Layout("ready-no-thread").mountKey("s");
    backend->on_mount_write = [fake_boot](uint32_t write_no)
    {
        if (write_no == 2)
            *fake_boot += kReadyClaimAgeMs;
        return false;
    };
    PoolConfig config = readinessPoolConfig("ready-no-thread", fake_boot);
    config.background_watermark = false;
    const uint64_t attempts_before = eventCount(ProfileEvents::CASMountRenewalAttempts);

    int code = 0;
    String message;
    try
    {
        (void)Pool::open(backend, config);
        ADD_FAILURE() << "an open with no lease thread and a claim too old must fail";
    }
    catch (const DB::Exception & e)
    {
        code = e.code();
        message = e.message();
    }

    EXPECT_EQ(code, DB::ErrorCodes::ABORTED) << message;
    EXPECT_NE(message.find("no lease thread"), String::npos) << message;
    EXPECT_EQ(eventCount(ProfileEvents::CASMountRenewalAttempts), attempts_before) << "nothing renewed the claim";
}
```

Why `max_failed_renewals` is 10'000: a smaller bound can be reached while the opening thread is not yet
scheduled after the clock passed its give-up point, and the test would then fail on a correct build. The
bound only has to end the test when the open never gives up: 10'000 in-memory requests take well under the
1200 s timeout and the test then fails with "the open never gave up".

Append two runtime-level tests of the open's wait, after the three above. They call `waitUntilArmed`
directly: the open's wait is that call, and the runtime fixture reaches states the `Pool` fixture cannot
(a scripted reclaim, the loop's own error path). Add `extern const int LOGICAL_ERROR;` to the
`namespace DB::ErrorCodes` block at the top of the file (~:34-45), and `#include <base/defines.h>` after
`#include <base/scope_guard.h>` (~:16): the second test is split on `DEBUG_OR_SANITIZER_BUILD`.

```cpp
namespace
{
/// What the open's wait and the reclaim saw when the readiness renewal met a definitive answer.
struct DefinitiveReadinessSeen
{
    bool armed = false;
    uint32_t puts_at_first_reclaim = 0;
    String admit_at_first_reclaim = "unobserved";
    bool may_mutate_at_first_reclaim = true;
    uint32_t reclaims = 0;
    uint64_t boot_before_wait = 0;
    uint64_t boot_after_wait = 0;
    ReadinessView after;
    uint64_t live_writer_epoch = 0;
    uint64_t requested_generation = 0;
    uint64_t lease_lost = 0;
};

/// The open's shape at the runtime level, with a lease of 1000 ms: the claim starts at 100 ms and the open
/// decides at 1070 ms, with 30 ms left and 40 ms needed. The slot is GC-fenced before the loop starts, so the
/// readiness renewal gets a definitive answer. The reclaim either claims epoch 2 and arms, or fails each
/// attempt and moves the fence clock 600 ms.
void runDefinitiveAnswerDuringReadiness(bool reclaim_arms, DefinitiveReadinessSeen & seen)
{
    const Layout layout(reclaim_arms ? "runtime-readiness-definitive-reclaimed" : "runtime-readiness-definitive-failing");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{100};
    std::atomic<uint32_t> puts{0};
    std::atomic<uint32_t> reclaims{0};
    CasMountRuntime * runtime_ptr = nullptr;
    const uint64_t lost_before = eventCount(ProfileEvents::CASMountLeaseLost);
    CasEventSink sink;
    auto backend = std::make_shared<ExpiryScriptBackend>();
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms.load(); }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            CasMountRuntime & reclaiming = *runtime_ptr;
            if (++reclaims == 1)
            {
                seen.puts_at_first_reclaim = puts.load();
                seen.admit_at_first_reclaim = expiryAdmitName(reclaiming.admit(reclaiming.fenceGeneration(), 0));
                seen.may_mutate_at_first_reclaim = reclaiming.mayMutate();
            }
            reclaiming.beginReclaim();
            if (!reclaim_arms)
            {
                boot_ms.fetch_add(600);
                return false;
            }
            if (claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 2, wall_ms, 1000).kind
                != MountClaimResult::Claimed)
                return false;
            reclaiming.installRenewer(uuid, 2, [&] { return wall_ms; });
            const uint64_t fresh_anchor = reclaiming.startRenewer();
            reclaiming.setLiveWriterEpoch(2);
            /// On a regression that does not arm, the clock passes the open's bound and the arm's
            /// notification ends the wait, so the test fails instead of hanging.
            if (!reclaiming.armIfAdmissible(fresh_anchor + 1000))
                boot_ms.fetch_add(2000);
            return true;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    boot_ms = 1070;
    ASSERT_FALSE(runtime.armIfAdmissible(anchor + 1000)) << "the claim leaves 30 ms, a ref append needs 40";
    fenceOutMount(*backend, layout.mountKey("test"));
    backend->on_put = [&](uint32_t put_no)
    {
        ++puts;
        /// A second renewal before any reclaim is the regression this test names. Fail it and move the
        /// clock past the open's bound: the failure notifies the wait, which then gives up.
        if (put_no >= 2 && reclaims.load() == 0)
        {
            boot_ms.fetch_add(2000);
            return true;
        }
        return false;
    };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(300));

    seen.boot_before_wait = boot_ms.load();
    seen.armed = runtime.waitUntilArmed(1000);
    seen.boot_after_wait = boot_ms.load();

    /// The join orders every write of the lease thread before the reads below.
    runtime.stopBackgroundWorkers();
    seen.after = readinessViewOf(runtime);
    seen.reclaims = reclaims.load();
    seen.live_writer_epoch = runtime.liveWriterEpoch();
    seen.requested_generation = runtime.remountRequestedGenerationForTest();
    seen.lease_lost = eventCount(ProfileEvents::CASMountLeaseLost) - lost_before;
    runtime.finishTeardown(false);
}

/// Throws a `LOGICAL_ERROR` from the renewal's conditional write of the mount key once `failing` is set: the
/// renewal hands it to the loop, which leaves through its own error path.
class LeaseLoopFailureBackend final : public DB::Cas::tests::CountingBackend
{
public:
    std::atomic<bool> failing{false};
    std::atomic<uint32_t> failures{0};

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
        const std::optional<String> & expected_value, DB::Cas::TransportAccess & access) override
    {
        if (failing.load() && expected_value && key.ends_with("/mount"))
        {
            /// Bounds a retry of the failure, should the engine ever retry one: the renewal then commits,
            /// the fence arms and the test fails on `armed` instead of hanging.
            if (++failures >= kReadinessRequestBound)
                failing = false;
            throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "injected lease-loop failure");
        }
        return DB::Cas::tests::CountingBackend::write(key, bytes, expected_value, access);
    }
};

struct LeaseThreadEndSeen
{
    uint32_t exits = 0;
    uint32_t failures = 0;
    bool armed = true;
    uint64_t boot_before_wait = 0;
    uint64_t boot_after_wait = 0;
    bool may_mutate = true;
};

/// The open's shape with a 1000 ms lease, as above; the readiness renewal's first request throws a
/// `LOGICAL_ERROR`. After the thread is gone, every read of the fence clock moves it 600 ms, so the wait's
/// bound is reached on that clock.
void runLeaseThreadEndsOnItsOwn(LeaseThreadEndSeen & seen)
{
    const Layout layout("runtime-readiness-thread-ended");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{100};
    std::atomic<bool> thread_gone{false};
    WorkerExitLatch exits;
    RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
    {
        return ThreadFromGlobalPool([&, body = std::move(worker_body)]
        {
            body();
            exits.recordExit();
        });
    };
    CasEventSink sink;
    auto backend = std::make_shared<LeaseLoopFailureBackend>();
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .background_watermark = true,
            .boot_ms_fn = [&] { return thread_gone.load() ? boot_ms.fetch_add(600) + 600 : boot_ms.load(); },
            .worker_factory = factory},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    boot_ms = 1070;
    ASSERT_FALSE(runtime.armIfAdmissible(anchor + 1000));
    backend->failing = true;
    runtime.startBackgroundWorkers(std::chrono::milliseconds(300));

    ASSERT_TRUE(exits.waitForAtLeast(1)) << "the loop must leave through its own error path";
    thread_gone = true;
    seen.boot_before_wait = boot_ms.load();
    seen.armed = runtime.waitUntilArmed(1000);
    seen.boot_after_wait = boot_ms.load();
    seen.may_mutate = runtime.mayMutate();
    seen.exits = static_cast<uint32_t>(exits.count());
    seen.failures = backend->failures.load();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}
}

/// The open's readiness renewal meets a definitive answer: the renewal is terminal and raises one remount
/// generation, the fence stays latched, and the loop reclaims before any further renewal. The open's wait
/// ends armed when the reclaim arms under the new epoch, and gives up after one lease on the fence clock when
/// every reclaim fails.
TEST(CASMountRuntime, ADefinitiveAnswerDuringReadinessIsServedByAReclaim)
{
    DefinitiveReadinessSeen reclaimed;
    ASSERT_NO_FATAL_FAILURE(runDefinitiveAnswerDuringReadiness(/*reclaim_arms=*/true, reclaimed));
    EXPECT_TRUE(reclaimed.armed) << "the reclaim armed the fence";
    EXPECT_EQ(reclaimed.puts_at_first_reclaim, 1u) << "the loop reclaimed before any further renewal";
    EXPECT_EQ(reclaimed.admit_at_first_reclaim, "LostOrRearmed") << "no write is admitted before the arm";
    EXPECT_FALSE(reclaimed.may_mutate_at_first_reclaim);
    EXPECT_EQ(reclaimed.reclaims, 1u);
    EXPECT_STREQ(reclaimed.after.admit, "Ok");
    EXPECT_TRUE(reclaimed.after.ref_append_ok);
    EXPECT_EQ(reclaimed.after.lifecycle, PoolLifecycle::Live);
    EXPECT_EQ(reclaimed.live_writer_epoch, 2u) << "armed under the new epoch";
    EXPECT_EQ(reclaimed.requested_generation, 1u) << "one definitive answer, one remount generation";
    EXPECT_EQ(reclaimed.lease_lost, 1u) << "the definitive answer counts one lease loss; the open's latch none";

    DefinitiveReadinessSeen failing;
    ASSERT_NO_FATAL_FAILURE(runDefinitiveAnswerDuringReadiness(/*reclaim_arms=*/false, failing));
    EXPECT_FALSE(failing.armed);
    EXPECT_GE(failing.boot_after_wait, failing.boot_before_wait + 1000) << "the wait gave up after one lease on the fence clock";
    EXPECT_EQ(failing.puts_at_first_reclaim, 1u);
    EXPECT_EQ(failing.admit_at_first_reclaim, "LostOrRearmed");
    EXPECT_GE(failing.reclaims, 1u);
    EXPECT_STREQ(failing.after.admit, "LostOrRearmed") << "nothing armed";
    EXPECT_FALSE(failing.after.may_mutate);
    EXPECT_EQ(failing.live_writer_epoch, 0u);
    EXPECT_EQ(failing.requested_generation, 1u);
    EXPECT_EQ(failing.lease_lost, 1u);
}

#ifndef DEBUG_OR_SANITIZER_BUILD
/// The lease thread leaves through its own error path while the open waits. The fence stays latched and the
/// wait returns false; nothing notifies it, so it ends at its bound of one lease on the fence clock.
TEST(CASMountRuntime, AnOpenFailsWhenTheLeaseThreadEndedOnItsOwn)
{
    LeaseThreadEndSeen seen;
    ASSERT_NO_FATAL_FAILURE(runLeaseThreadEndsOnItsOwn(seen));
    EXPECT_EQ(seen.exits, 1u);
    EXPECT_EQ(seen.failures, 1u) << "the first readiness request reached the loop's error path";
    EXPECT_FALSE(seen.armed);
    EXPECT_GE(seen.boot_after_wait, seen.boot_before_wait + 1000) << "the wait ended at its bound on the fence clock";
    EXPECT_FALSE(seen.may_mutate) << "the fence stays latched";
}
#else
/// A `LOGICAL_ERROR` aborts a debug or sanitizer build where it is constructed, so the loop's error path is
/// reached only in a release build. Here the scenario must end the process.
TEST(CASMountRuntimeDeathTest, AnOpenFailsWhenTheLeaseThreadEndedOnItsOwn)
{
    EXPECT_DEATH(
        {
            LeaseThreadEndSeen seen;
            runLeaseThreadEndsOnItsOwn(seen);
        },
        "injected lease-loop failure");
}
#endif
```

Why these hold on the code as specified:
- In the reclaimed run, `beginReclaim` trips a fence the terminal renewal already latched: `noteLeaseLost`'s
  compare-exchange from `Live` fails on `TransientNotLive`, so the loss is counted once. The open's latch
  (`tripFenceWithoutOperationalLoss`) counts none. The fresh claim's deadline is 2070 ms at 1070 ms, so
  `armIfAdmissible` arms and notifies; the next renewal is due at 1370 ms on a clock that does not move.
- In the failing run, a failed reclaim does not notify. The wait re-reads the clock at its real-time timeout
  (`give_up - now`, at most 1000 ms), and the reclaim attempts move the clock 600 ms each, at the loop's
  real-time backoff (1 s, then 2 s). The run takes up to about 3 s of real time; its outcome does not depend
  on it, because the clock moves only in the attempts and the wait gives up only on the clock.
- In the thread-ended run, the loop's catch trips the fence and returns, and nothing notifies `driver_cv`;
  `workers_started` stays true until the stop. The wait therefore ends only at its bound: each of its clock
  reads moves the clock 600 ms, so it returns false after one real-time timeout of at most 400 ms.

- [ ] **Step 3: Build and run them to see them fail**

First build: the two `waitUntilArmed` tests do not compile.

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t2_c3_red0_build.log 2>&1; echo NINJA_EXIT=$? >> build/t2_c3_red0_build.log
tail -1 build/t2_c3_red0_build.log; grep -a -n "error:" build/t2_c3_red0_build.log | head -40
```

Expected: `NINJA_EXIT=1`; every error is `no member named 'waitUntilArmed' in 'DB::Cas::CasMountRuntime'` in
`gtest_cas_pool.cpp`. That is the red of the two new runtime tests: the wait is new API. Any other error is
a fixture mistake: fix it before going on.

Then add `waitUntilArmed` alone: its header declaration and the definition from Step 4 (only that function).
Build and run every test of this cycle:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t2_c3_red_build.log 2>&1; echo NINJA_EXIT=$? >> build/t2_c3_red_build.log
tail -1 build/t2_c3_red_build.log; grep -a -n "error:" build/t2_c3_red_build.log | head -40
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASMountRuntime.OpenAndRemountReportReadyOnlyWhenWritable:CASMountRuntime.AnOpenWhoseReadinessRenewalKeepsFailingFailsAfterOneTtl:CASPool.AnOpenWithoutALeaseThreadFailsWhenItsClaimIsTooOld:CASPool.StartupArmRedoesLeaseWriteWhenTheClaimConsumesTtl:CASMountOpenWaits.PublicationHorizonUsesTheEnvelope:CASPoolRemount.RemountRenewerRedoUsesTheEnvelope:CASMountRuntime.ADefinitiveAnswerDuringReadinessIsServedByAReclaim:CASMountRuntime.AnOpenFailsWhenTheLeaseThreadEndedOnItsOwn' > build/t2_c3_red.log 2>&1; echo GATE_EXIT=$? >> build/t2_c3_red.log )
grep -a "FAILED\|PASSED\|GATE_EXIT\|Failure\|Which is\|Actual" build/t2_c3_red.log | tail -80
```

Expected: `NINJA_EXIT=0`. `ADefinitiveAnswerDuringReadinessIsServedByAReclaim` and
`AnOpenFailsWhenTheLeaseThreadEndedOnItsOwn` pass: they drive the runtime only, and Cycle 1 already gives
it the arming rule. Failures, each paste into the report:
- `OpenAndRemountReportReadyOnlyWhenWritable`: case 1 `renewal_on_opening_thread` is true (the startup re-anchor
  runs on the opening thread); the remount block has step `publish_live`, lifecycle `Live`, `mayMutate` true at
  the event (the remount re-anchor made the claim fresh), and `mount_writes` 7 is reached with the redo as write 6.
- `AnOpenWhoseReadinessRenewalKeepsFailingFailsAfterOneTtl`: `code` is `NETWORK_ERROR`, `failed_renewals` about 6,
  `exits->count()` 0.
- `AnOpenWithoutALeaseThreadFailsWhenItsClaimIsTooOld`: "must fail" (the open re-anchored), `CASMountRenewalAttempts`
  moved by 1.
- `PublicationHorizonUsesTheEnvelope`: the `claim_age_ms = 300` case returns 3 (the window with the period redoes).
- `RemountRenewerRedoUsesTheEnvelope`: the 350 and 450 cases show `publish_live` and `Live`.
- `StartupArmRedoesLeaseWriteWhenTheClaimConsumesTtl` PASSES on this code too (the re-anchor writes the third
  time); it is adapted, not new. Record it green.

- [ ] **Step 4: Implement**

`$W/CA/Pool/CasMountRuntime.cpp`, new function after `armIfAdmissible` (added in Step 3 already, with its declaration; it stays as written here):

```cpp
bool CasMountRuntime::waitUntilArmed(uint64_t timeout_ms) const
{
    const uint64_t started = bootMsNow();
    const uint64_t give_up = started > std::numeric_limits<uint64_t>::max() - timeout_ms
        ? std::numeric_limits<uint64_t>::max()
        : started + timeout_ms;
    std::unique_lock lock(driver_mutex);
    while (true)
    {
        if (!mount_fence.lost.load(std::memory_order_acquire))
            return true;
        if (workers_stop_requested || remountTerminal() || !workers_started)
            return false;
        const uint64_t now = bootMsNow();
        if (now >= give_up)
            return false;
        /// The fence clock can be injected and move with no real time passing, so every wake-up re-reads
        /// it: an arm, a failed renewal request, a stop and a terminal publication all notify.
        driver_cv.wait_for(lock, std::chrono::milliseconds(give_up - now));
    }
}
```

Critical section: `driver_mutex` held only by the wait; atomics and the clock read under it; no I/O, no
log, no join. The open's stop and join run after it returns, with no lock held.

`CasMountRuntime::noteRenewRequest` (~:202), replace the `try` block of the failed branch with:

```cpp
    try
    {
        {
            std::lock_guard lock(renew_failure_mutex);
            last_renew_failure = event.failure_text;
        }
        /// `waitUntilArmed` re-reads the fence clock on this wake-up. Passing through `driver_mutex` orders
        /// the notification after its predicate check, so the wake-up is not lost.
        {
            std::lock_guard lock(driver_mutex);
        }
        driver_cv.notify_all();
    }
    catch (...)   // NOLINT(bugprone-empty-catch)
    {
        /// A lost diagnostic must not end the renewal.
    }
```

`noteRenewRequest` runs on the renewing thread from the request observer, never under `driver_mutex` (no
renewal runs under it), so taking it here cannot self-deadlock. The two locks are taken one after the other,
not nested.

`$W/CA/Pool/CasPool.cpp`, `Pool::mountWritable`: replace everything from the comment
`/// Arm the local write fence: cache (uuid, epoch) and set the boottime deadline at the claim` (~:865) to the
end of the function (~:922, the `startBackgroundWorkers` call and its closing brace), as left by Task 1.
Task 1 leaves this block as today's code with two differences that do not matter here: it still arms with
`store->armMountFence(our_uuid, writer_epoch, ...)`, and the comment above `startBackgroundWorkers` begins
"Gate the lease thread with `background_watermark`". The whole block, comment included, becomes:

```cpp
    /// The claim's deadline is its attempt's start plus the TTL, never a later instant: a post-I/O instant
    /// would authorize writes under a deadline the durable lease does not back. The fence is armed from it
    /// only if it still admits a ref append; otherwise it stays latched until a renewal arms it.
    const uint64_t ttl_ms_u = static_cast<uint64_t>(store->config.mount_lease_ttl_ms.count());
    const uint64_t claim_deadline_boot_ms = claim_anchor_boot_ms > std::numeric_limits<uint64_t>::max() - ttl_ms_u
        ? std::numeric_limits<uint64_t>::max()
        : claim_anchor_boot_ms + ttl_ms_u;
    store->mount_runtime.setLiveWriterEpoch(writer_epoch);
    const bool armed = store->mount_runtime.armIfAdmissible(claim_deadline_boot_ms);
    if (!store->config.background_watermark)
    {
        /// Nothing would renew this claim, so a latched fence would refuse writes for good.
        if (!armed)
            throw Exception(ErrorCodes::ABORTED,
                "CAS mount '{}': the mount claim left too little of the {} ms lease to admit a write, and this "
                "mount has no lease thread to renew it; retry the open", srid, ttl_ms_u);
        return;
    }
    store->mount_runtime.startBackgroundWorkers(store->config.mount_renew_period);
    if (armed)
        return;
    LOG_WARNING(getLogger("CasPool"),
        "Content-addressed mount {}: the mount claim left too little of the {} ms lease to admit a write; "
        "the open waits for the lease thread's first renewal", srid, ttl_ms_u);
    if (store->mount_runtime.waitUntilArmed(ttl_ms_u))
        return;
    /// Joined before the failure propagates, so no renewal of this open runs after it.
    store->mount_runtime.stopBackgroundWorkers();
    const String last_failure = store->mount_runtime.lastRenewFailure();
    throw Exception(ErrorCodes::ABORTED,
        "CAS mount '{}': no renewal left enough of the {} ms lease to admit a write within one lease after "
        "the mount claim; last failed renewal request: {}",
        srid, ttl_ms_u, last_failure.empty() ? String("none") : last_failure);
}
```

A remount request raised between `armIfAdmissible` and the thread start cannot exist: no caller has the pool
yet, and `reportImpossibleInterference` needs one.

`$W/CA/Pool/CasPool.cpp`, `Pool::tryRemountOnce`:

1. Next to `bool succeeded = false;` (~:1260), add:

```cpp
    bool armed = false;
```

2. In the `SCOPE_EXIT` finalizer, replace the `event.reason = ...` statement (~:1282-1284) with:

```cpp
                event.reason = !succeeded
                    ? "whole-chain remount returned without restoring Live"
                    : (armed ? "whole-chain remount restored Live under a fresh mount incarnation"
                             : "whole-chain remount claimed a fresh mount incarnation; the next renewal arms the fence");
```

The outcome stays `ok` for both, and `CASRemountSucceeded` counts both: the request is served and the loop
must not back off.

3. Change `uint64_t remount_anchor_boot_ms = mount_runtime.startRenewer();` (~:1516) to
`const uint64_t remount_anchor_boot_ms = mount_runtime.startRenewer();`.

4. Replace the tail of the `try` block, from the comment `/// Quiescence may consume most of the new lease.`
(~:1544) to `return true;` (~:1582), as left by Task 1: today's window test and `renewer_redo` step, unchanged,
followed by Task 1's arm

```cpp
        /// No-throw commit section: publish the fence and lifecycle only after epoch, renewer, recovery
        /// cancellation, and ref-runtime quiescence are complete.
        step = "arm_fence";
        const uint64_t deadline_boot_ms = remount_anchor_boot_ms > std::numeric_limits<uint64_t>::max() - ttl_ms
            ? std::numeric_limits<uint64_t>::max()
            : remount_anchor_boot_ms + ttl_ms;
        if (mount_runtime.armIfAdmissible(deadline_boot_ms))
            step = "publish_live";
        succeeded = true;
        return true;
```

Under Task 1 a reclaim that did not arm reports `step = 'arm_fence'` with the "restored Live" reason.
Replace the whole tail with:

```cpp
        /// No-throw commit section. The fence and the lifecycle are published only after epoch, renewer,
        /// recovery cancellation and ref-runtime quiescence are complete. A claim whose deadline no longer
        /// admits a ref append leaves the fence latched; the lease thread's next renewal arms it.
        step = "arm_fence";
        armed = mount_runtime.armIfAdmissible(
            remount_anchor_boot_ms > std::numeric_limits<uint64_t>::max() - ttl_ms
                ? std::numeric_limits<uint64_t>::max()
                : remount_anchor_boot_ms + ttl_ms);
        step = armed ? "publish_live" : "claimed_not_armed";
        succeeded = true;
        return true;
```

After this step `renewRenewerForRemountOnce` and `renewRenewerForStartupOnce` have no production caller
(`git -C $W grep -n "renewRenewerFor" -- src ':!src/Disks/tests'` lists only their declarations and
definitions). Task 3 deletes them.

Why `CASPoolRemount.RemountArmAnchorsAtClaimAttemptNotResponseTime` stays green untouched: after
`startRenewer` reads queued value #1 (10000), the removed window test read the clock once (#2, 11000); the new
`canArm` reads it once, in the same position, and only then (every other term holds on that path). The arm is
at 40000; `remaining = 29000 > 16000`; the probe at 40000 refuses.

- [ ] **Step 5: Build, run the tests of this cycle, then the gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t2_c3_build.log 2>&1; echo NINJA_EXIT=$? >> build/t2_c3_build.log
tail -1 build/t2_c3_build.log; grep -a -n "error:" build/t2_c3_build.log | head -40
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASMountRuntime.*:CASPool.*:CASMountOpenWaits.*:CASPoolRemount.*:CASForget.*:CASLifecycle*' > build/t2_c3_tests.log 2>&1; echo GATE_EXIT=$? >> build/t2_c3_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t2_c3_tests.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CAS*' > build/t2_c3_gate.log 2>&1; echo GATE_EXIT=$? >> build/t2_c3_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t2_c3_gate.log | tail -20
```

Prove which arm of the `DEBUG_OR_SANITIZER_BUILD` split compiled in the release build:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && build/src/unit_tests_dbms --gtest_list_tests --gtest_filter='CASMountRuntime*.AnOpenFailsWhenTheLeaseThreadEndedOnItsOwn'
```

Expected: the release build lists `CASMountRuntime.` with `AnOpenFailsWhenTheLeaseThreadEndedOnItsOwn` and no
`CASMountRuntimeDeathTest.` entry.

The sanitizer arm. Never run `cmake` or `ninja` in `build_asan`: the controller owns that build. If
`build_asan/src/unit_tests_dbms` exists and is newer than the last source edit of this cycle, run only the two
tests on it (no `ulimit -v`: an ASan binary reserves a large address space):

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( flock build_asan/.wok-build.lock timeout 1200 build_asan/src/unit_tests_dbms --gtest_filter='CASMountRuntime*.AnOpenFailsWhenTheLeaseThreadEndedOnItsOwn:CASMountRuntime.ADefinitiveAnswerDuringReadinessIsServedByAReclaim' > build_asan/t2_c3_asan.log 2>&1; echo GATE_EXIT=$? >> build_asan/t2_c3_asan.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build_asan/t2_c3_asan.log | tail -10
```

Otherwise report the death-test arm as "written, not compiled here; verified by the controller's ASan gate" and
continue.

Expected for the release build and gate: `NINJA_EXIT=0`, `GATE_EXIT=0` both times; the gate count is Cycle 1's plus 5 minus 4. Any other
failure: stop, revert the cycle, save the diff under `$W/.superpowers/sdd/t2-cycle3.diff`, report.

- [ ] **Step 6: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp src/Disks/tests/gtest_cas_pool.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Make a CAS mount ready through the lease thread instead of a re-anchor" -m "An open whose claim does not admit a ref append latches the fence, starts the lease thread and waits up to one lease for a renewal to arm it; then it stops and joins the thread and fails with ABORTED. A writable open without a lease thread fails at once in that case. A reclaim whose claim is too old reports success at step claimed_not_armed and the next renewal arms the fence. The startup and remount re-anchors have no production caller left." -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

#### Cycle 4: docs

Docs-only cycle: no red run. Edit, then check the anchors and that the gate binary is unaffected (no source
change).

- [ ] **Step 1: `docs/en/antalya/cas/architecture/mounts-and-leases.md`**

In "The mount lease" (`{#mount-lease}`), the list after "The background renewal ends, and moves the renewer to
`RenewalTerminal`, only on one of these:". Replace the three bullets

```markdown
- a stop or a remount request;
- a deterministic local failure;
- a lifecycle other than `Live`;
- a lost fence.
```

with

```markdown
- a stop or a remount request;
- a deterministic local failure;
- a terminal lifecycle, which includes a published `FORGET` intent.

A lost fence alone does not end it: the renewal that makes a mount ready runs under a latched fence. Every
other trip of the fence comes with a remount request, a `FORGET` intent or a stop.
```

In "Mount, unmount, crash" (`{#mount-lifecycle}`), delete the sentence "If the claim consumed the TTL, one
fresh synchronous renewal re-anchors the deadline before the fence is armed." from the "Writable open"
paragraph (as left by Task 1), and insert after that paragraph:

```markdown
**Readiness.** An open arms the fence from its claim only when the claim's deadline, its start plus the
TTL, still leaves room for a ref append: `2 × envelope + margin` (16 s with the defaults). Otherwise the
fence stays latched, the lease thread renews at once, and the first renewal whose own deadline leaves that
room arms the fence; then the open returns. A renewal that commits with less room arms nothing, and the
next one follows at once. If no renewal arms the fence within one TTL, the open stops and joins the lease
thread and fails with `ABORTED`, naming the last failed renewal request. A reclaim follows the same rule: a
claim with too little lease left keeps the pool `TransientNotLive` with the fence latched, and the next
renewal arms the fence and reports `Live`. The `mount_remount` row of such a reclaim has `outcome = 'ok'`
and `step = 'claimed_not_armed'`.
```

In the `PoolLifecycle` diagram of "Behavioral mount-slot model" (`{#mount-state-machines}`), replace

```text
    Live --> Live: mountWritable arms the fence
```

with

```text
    Live --> Live: the open's claim or first renewal arms the fence
```

and

```text
    TransientNotLive --> Live: self-remount succeeds with a fresh epoch
```

with

```text
    TransientNotLive --> Live: the reclaim or the next renewal arms the fence under a fresh epoch
```

Mermaid lines carry no `;`.

- [ ] **Step 2: `docs/en/antalya/cas/operations/monitoring.md` and `debugging.md`**

`monitoring.md`, the counter table: replace the `CASRemountSucceeded` and `CASRemountFailed` rows with

```markdown
| `CASRemountSucceeded` | One per whole-chain attempt that claimed the mount under a fresh writer epoch | Must be a subset of `CASRemountAttempts`. Includes a reclaim whose claim left too little lease to arm the fence (`step = 'claimed_not_armed'`); the next renewal arms it |
| `CASRemountFailed` | One per whole-chain attempt that returned without claiming the mount | Includes a named step exception or a step that returned transiently |
```

Same file, after the sentence that ends "and details `attempt_no`, `step`, `server_root_id`, optional
`writer_epoch`, and optional `error`.", add:

```markdown
An `ok` row has `step = 'publish_live'` when the reclaim armed the fence and reported `Live`, or
`step = 'claimed_not_armed'` when its claim left too little lease to admit a write; the pool then stays
`not_live` until the next renewal arms the fence.
```

`debugging.md`, the bullet that begins "A following `mount_remount` row names the whole-chain `attempt_no` and
final `step`." Replace its second sentence "An `ok` row restored `Live` under the reported fresh
`writer_epoch`; a `failed` row's `step` and optional `error` identify where that whole-chain attempt stopped."
with

```markdown
An `ok` row with `step = 'publish_live'` restored `Live` under the reported fresh `writer_epoch`; with
`step = 'claimed_not_armed'` the claim succeeded with too little lease to admit a write, and the next
renewal arms the fence and reports `Live`. A `failed` row's `step` and optional `error` identify where that
whole-chain attempt stopped.
```

- [ ] **Step 3: Check**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && grep -n "^#" docs/en/antalya/cas/architecture/mounts-and-leases.md docs/en/antalya/cas/operations/monitoring.md docs/en/antalya/cas/operations/debugging.md | grep -v "{#" ; echo "headings without anchor: above (expect none)"
grep -rn "re-anchors the deadline\|a lost fence\.\|lifecycle other than" docs/en/antalya/cas/ ; echo "stale readiness text: above (expect none)"
grep -rn "claimed_not_armed" docs/en/antalya/cas/ | wc -l   # expect 4
```

- [ ] **Step 4: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- docs/en/antalya/cas/architecture/mounts-and-leases.md docs/en/antalya/cas/operations/monitoring.md docs/en/antalya/cas/operations/debugging.md
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Describe CAS mount readiness through the lease thread" -m "An open or a reclaim whose claim leaves too little lease for a ref append keeps the fence latched until a renewal arms it. Document the remount step claimed_not_armed and what ends a renewal early." -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

**Arguments for what the model does not cover** (`CaDiskLifecycle` has no time and no open).

- **The open's wait against the thread it started.** `waitUntilArmed` returns on an arm, a stop, a terminal
  lifecycle, no thread, or one TTL on the fence clock. On the TTL the open calls `stopBackgroundWorkers`
  with no lock held: the stop wakes the renewal's retry wait, the renewal ends at its next admission check,
  and the join returns. A request or a credential refresh already under way is not interrupted, so the
  failure comes up to one attempt envelope after the TTL, plus the join. The open's renewal that ends this
  way is terminal and forbids the farewell; `~Pool` then writes none.
- **The cadence during a reclaim.** The observation wait and the backoff run on the lease thread and no
  renewer is `Active` meanwhile; the fence is latched by `beginReclaim`, so nothing is admitted. After a
  reclaim that did not arm, the next renewal is due at once: its claim deadline leaves less than
  `2 × envelope + margin`, so `now ≥ start + TTL − 2 × envelope − margin > start + period`, because
  `validateCasRequestBudget` enforces `period + 2 × envelope + margin < TTL` for every mount with a lease thread.
  The same arithmetic makes the next renewal due at once after a stale or a short success.
- **A stale success followed by the next renewal.** A stale success leaves the fence as it was: `canArm` fails
  on its deadline, and `publishRenewedDeadline` keeps the failure text and the expiry instant it computed with
  the fence lost (none). Pinned by `AReadinessRenewalArmsOnlyWithRoomForARefAppend`.
- **`waitUntilArmed` with an injected clock.** It re-reads the clock on every `driver_cv` wake-up and at the
  real timeout. A failed renewal request notifies through `noteRenewRequest`; a commit notifies from the consume
  step; a stop and a terminal publication notify. A fixture whose renewal makes no request and does not commit
  never wakes the open: the tests bound such loops by request count.

**Deviations from the contract**

1. **Two private helpers, not one.** `budgetAdmits(deadline, now, needed)` holds `admit`'s arithmetic and
   `refAppendReservationMs()` the two envelopes; `admit`, `refAppendFenceOk` and `canArm` use them. One helper
   would have duplicated either the arithmetic of `admit` or the reservation. `admit` keeps its read order.
2. **Spec test 14 is three tests.** One body would exceed 150 lines. `OpenAndRemountReportReadyOnlyWhenWritable`
   keeps the contract's name and holds cases 1, 6 and 7 at the `Pool` level;
   `AnOpenWhoseReadinessRenewalKeepsFailingFailsAfterOneTtl` holds case 2;
   `AReadinessRenewalArmsOnlyWithRoomForARefAppend` holds cases 3, 4 and 5 at the runtime level. Cases 3 and 4
   cannot run through an open: a stale success needs a renewal longer than a TTL, and the open gives up after
   one TTL.
3. **`gtest_cas_mount_runtime.cpp` changes one assertion** (the contract lists it untouched): the refusal-text
   ruling changes the text that `ExpiredLeaseRefusalSaysWritesResume` pins.
4. **`gtest_cas_forget.cpp` does not change** (the contract lists it for T5a). Task 1's
   `CASForget.AReclaimDuringForgetArmsNothing` (renamed from `ForgetReLatchesFenceAfterAReclaimReachesArmMountFence`)
   already asserts that the reclaim inside the FORGET window returns `true` with `mayMutate()` false; this
   task changes only its step to `claimed_not_armed`, which that test does not read.
5. **`noteRenewRequest` passes through `driver_mutex` before it notifies.** A bare `notify_all` can land
   between `waitUntilArmed`'s clock check and its wait, and the open would then sleep its real timeout. The
   pass-through is an empty critical section, taken after `renew_failure_mutex` is released.
6. **`PublicationHorizonUsesTheEnvelope` is rewritten and moved**, not only switched to
   `background_watermark = true`: its per-call clock lets a running lease thread renew on its own reads, which
   makes the write count race. The new fixture ages the claim once and freezes the clock.
7. **`RemountRenewerRedoUsesTheEnvelope` moves its boundary** from `period + 2 × envelope` (700 ms) to
   `2 × envelope + margin` (650 ms) and gains a no-period case (300 ms), since the arming rule has no period
   term.
8. **The step `claimed_not_armed` and the event reason are written here** (ruling O6 assigns no task; the
   contract lists the event among T5a's hazards). Task 1 leaves a reclaim that did not arm at step
   `arm_fence` with the "restored Live" reason; Cycle 3 Step 4 sets both.
9. **The open waits its full bound when the lease thread ended on its own.** An earlier return would need a
   notification from the loop's exit path, which this task does not add; `AnOpenFailsWhenTheLeaseThreadEndedOnItsOwn`
   pins the bound. The loop's error path is reachable only through a `LOGICAL_ERROR`, so that test is a death test
   on debug and sanitizer builds.
10. **`ADefinitiveAnswerDuringReadinessIsServedByAReclaim` spends up to about 3 s of real time** in the loop's
    real-time reclaim backoff and the wait's real-time timeout; its outcome depends only on the injected clock.

**Review focus**

1. **A remount request arriving during the open's wait** (from a ledger that already holds the pool through a
   path not yet known). Expected: the loop serves it before any renewal and `armIfAdmissible` decides; the open
   returns only when the fence is armed. The contract argues no caller has the pool before the open returns;
   no test checks that.

---

### Task 3: Remove the bounded paths

After Tasks 1 and 2 the startup and remount re-anchors have no production caller, and the only renewal
production runs is the lease thread's. This task deletes the re-anchors, `RenewCaller`, the renewer's
remount renewal, the lease-bound policy and the fence-gated renewal plane in the renewer and the runtime,
then the counter and the two classifications that can no longer occur. `renewWatermarkOnce` stays as the
test seam and runs the lease thread's step, `renewOnce`. No behaviour is added: every cycle is delete,
adapt the listed tests, gate. There is no red run.

Paths below: `W` = `/home/mfilimonov/workspace/ClickHouse/cas-95-1`,
`CA` = `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`, `R` = `CA/Pool/CasMountRuntime.cpp`,
`RH` = its header, `S` = `CA/Pool/CasServerRoot.cpp`, `SH` = its header, `P` = `CA/Pool/CasPool.cpp`,
`PH` = its header, `T` = `src/Disks/tests`. Line numbers are from `W` at `7e243502408`, each with its
function or test name. Code that Tasks 1 and 2 rewrite is quoted "as left by Task N" and located by name.

**Files:**
- Modify: `RH` (`renewWatermarkOnce` doc; the declarations of `renewRenewerForStartupOnce`, `renewRenewerForRemountOnce`, `RenewCaller`, `renewRenewerOnce`, `renewalEnvironment`, `renewalLive`, `consumeRenewResult`; the constructor ~:148-160; member `mount_requests` ~:485; `noteRenewRequest` doc ~:327)
- Modify: `R` (constructor ~:55-74; `renewWatermarkOnce` ~:287; `installRenewer` ~:457; `renewalEnvironment` ~:513; `renewalLive` ~:529; `consumeRenewResult` ~:557; `renewRenewerOnce` ~:617; `renewRenewerForStartupOnce` ~:644; `renewRenewerForRemountOnce` ~:653; the call in `renewalLoop`; the `ProfileEvents` extern ~:33)
- Modify: `SH` (`MountRenewResult::deadline_source` ~:57-60; `MountRenewPolicy` ~:64-69; `kMountRenewRetrySpacingMs` doc ~:71; `MountRenewOperationEnvironment::policy` ~:93; `MountLeaseRenewer` PLANES paragraph ~:552-559, constructor ~:563-572, `renew` doc ~:578-581, `renewForRemount` ~:582-587, `renewOn` ~:599-600, members ~:606-609)
- Modify: `S` (`MountRenewTerminalClassification` ~:103-114; `terminalClassificationName` ~:328-342; `MountLeaseRenewer::MountLeaseRenewer` ~:1326-1347; `MountLeaseRenewer::renew` ~:1581; `MountLeaseRenewer::renewForRemount` ~:1587; `MountLeaseRenewer::renewOn` ~:1592-1745)
- Modify: `P` (`Pool::Pool`: the `ref_ledger` comment ~:234 and the `mount_runtime` construction ~:257-267; in `Pool::mountWritable` the comment on `claim_anchor_boot_ms` ~:712 if Task 2 left it)
- Modify: `CA/Pool/CasRefLedger.h` (comment on the `mount_requests_` parameter ~:94)
- Modify: `src/Common/ProfileEvents.cpp` (`CASMountRenewalDeadlineExceeded` :959)
- Modify: `docs/en/antalya/cas/operations/monitoring.md`, `troubleshooting.md`, `debugging.md`; `docs/en/antalya/cas/architecture/mounts-and-leases.md` (the bounded-renewal sentences, if Task 2 left them)
- Modify: `tests/integration/test_cas_mount_renewal_retry/test.py` (the counter name and two asserts)
- Test: `T/gtest_cas_pool.cpp`, `T/gtest_cas_heartbeat.cpp`, `T/gtest_cas_mount.cpp`, `T/gtest_cas_event_log.cpp`, `T/gtest_cas_observability.cpp`, `T/gtest_cas_gc_ack_floor.cpp`, `T/gtest_cas_mount_claim_conflicts.cpp`, `T/gtest_cas_mount_runtime.cpp`

**Interfaces:**
- Consumes (Tasks 1 and 2): `RenewCaller`; `renewRenewerOnce(RenewCaller)` returning `MountRenewResult`;
  `renewalEnvironment(RenewCaller)`; `renewalLive(RenewCaller)` whose `Loop` arm is `!workers_stop_requested`,
  no pending remount request, `!remountTerminal()`; `consumeRenewResult(const MountRenewResult &, RenewCaller)`
  (consume under `driver_mutex`, report with no lock held); `renewWatermarkOnce` refusing a
  background-configured runtime and calling `renewRenewerOnce(RenewCaller::Direct)`; no production caller
  of `renewRenewerForStartupOnce` or `renewRenewerForRemountOnce`.
- Produces: `MountRenewResult CasMountRuntime::renewOnce()`; `renewalEnvironment()`; `renewalLive() const`;
  `consumeRenewResult(const MountRenewResult &)`; `void renewWatermarkOnce()` over `renewOnce`;
  `CasMountRuntime(BackendPtr, CasRequests & farewell_requests_, CasRequests & lease_requests_, const Layout &, MountConfig, String, const CasEventSink &, CasRequestBudget, std::function<bool()>)`;
  `MountLeaseRenewer(CasRequests & open_requests_, CasRequests & lease_requests_, const Layout &, ...)` with member
  `lease_requests` (renamed from `worker_requests`); `MountLeaseRenewer::renew` with the former body of `renewOn`,
  on `lease_requests` under `Retry::untilDefinitive(kMountRenewRetrySpacingMs)`;
  `MountRenewOperationEnvironment` with members `boot_ms`, `live`, `cancelled`, `on_request` in that order;
  `MountRenewTerminalClassification` with seven values (no `ExternalLeaseDeadline`, no `RequestDeadline`);
  `MountRenewResult` without `deadline_source`. In tests: `RuntimeUnderTest` without a mount plane,
  `UnboundedRenewalFixture::renew(live, cancelled, on_request)`, `Ops(backend, boot_ms)` in
  `gtest_cas_heartbeat.cpp`, test `CASPool.DirectTerminalFailureRethrowsTypedException`.

#### What changes for the test seam, and every call site it reaches

After cycle 1 `renewWatermarkOnce` runs the lease thread's renewal: on the lease plane, with no lease bound,
retried until a definitive answer, a stop, a pending remount request or a terminal lifecycle. A caller that
injects a transient fault and expects a throw now sees the renewal retry and, for a one-shot fault, commit;
for a persistent fault it would retry until the 1200 s timeout. Enumeration, run in `W` at `7e243502408`,
output not truncated:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
grep -rn 'renewWatermarkOnce' src/Disks/tests | wc -l                       # 221
# sites whose result is checked: a throw expectation or a try/catch around the call
grep -rn -B10 -A6 'renewWatermarkOnce' src/Disks/tests | grep -E 'EXPECT_THROW|EXPECT_NO_THROW|expectThrows|try$|catch'
# fault backends of every test that calls the seam: each fault keyed to a non-mount key or to a one-shot
grep -rn -E 'ends_with\("/mount"\)|fault_key_substr *=|watch\(' src/Disks/tests
```

Helpers that call the seam (followed to their callers, two levels): `drive`, `runRoundCapturingGate`,
`buildCrossNamespaceScenario`, `buildPoolWithWorkAtEverySite` (`gtest_cas_gc_frontier_gate.cpp`),
`publishThenDrop`, `runRedeleteScenario`, `driveUntilAllGraduated` (`gtest_cas_gc_redelete_concurrency.cpp`),
`runGcToFixpoint` (five files), `displaceAndGc`, `runRounds`, `reclaim`, `populate`, `runFolds`,
`driveToFixpoint`, `runToFixpoint`, `runExpiredMountFenceOutScenario`,
`verifyForeignConflictSinkIsNonInterfering`, `runRoundsUntilAbsent` (`cas_test_helpers.h:791`). Every
fault backend their callers construct keys its fault to a non-mount key (`.meta`, `outcomes/`, `gc/state`,
a blob key, a `_log/` prefix, a list page) or to a read off the owner thread; none fails the mount slot's
conditional write.

| Class | Lines | What happens |
|---|---:|---|
| Not a call (comment text) | 6 | none: `gtest_cas_gc_leak.cpp:57`, `gtest_cas_truncate_reclaim.cpp:85`, `gtest_cas_gc_log.cpp:116`, `gtest_cas_pool.cpp:1903`, `gtest_cas_gc_round.cpp:163`, `cas_test_helpers.h:786` |
| Deleted with its test | 3 | `gtest_cas_pool.cpp:3424` (`DirectRenewCannotRaceWorkerStartOrRenewerReplacement`, Task 1); `gtest_cas_event_log.cpp:314` (`AnAmbiguityPastTheLeaseBoundNeverStartsTheResolvingRead`, cycle 1); `gtest_cas_observability.cpp:205` (`ExternalLeaseDeadlineCountsOnceWithoutReconstructingAttempts`, cycle 1) |
| Expects a give-up at a deadline: adapt | 4 | listed below |
| Definitive answer: untouched | 5 | `gtest_cas_event_log.cpp:466` (store rejection), `:566` (vanished slot); `gtest_cas_pool.cpp:1637` (GC fence-out), `:1888` (superseded), `:2219` (foreign successor) |
| One-shot transient fault, expects success: untouched | 7 | `gtest_cas_event_log.cpp:257`, `:430`, `:523`, `:559`; `gtest_cas_mount.cpp:351`, `:364`; `gtest_cas_observability.cpp:170`. Each retry now waits one spacing draw of real time (800 to 1200 ms): these pools freeze the boot clock and set no `retry_sleep_fn` |
| Refused by the seam: untouched | 1 | `gtest_cas_pool.cpp:3526` (`DirectRenewIsRefusedForBackgroundConfiguredRuntimeAfterStop`) |
| Healthy store: untouched | 195 | every other line |

The adapt group, in full (each fails rather than hangs on cycle 1's code: none injects a persistent fault):

| Site at `7e243502408` | Test | Today it ends at | Replacement |
|---|---|---|---|
| `gtest_cas_event_log.cpp:489` | `CASEvent.TerminalRenewalDetailsPreservePhysicalTruthAndClassification`, second block | the lease bound before the first attempt (`boot_ms = 1090`) | a definitive answer: the slot vanishes under the renewal (`vanish_on_next_write`); `attempts_sent` `"1"`, classification `"vanished"` |
| `gtest_cas_mount.cpp:380` | `CASMountAudit.RenewalDefaultLogsAreBounded`, fourth block | the lease bound (`boot_ms = 1080`) | a definitive answer: the store rejects the renewal deterministically (new `RenewalLogBackend::refuse_next_overwrite`); the log assertions stay |
| `gtest_cas_pool.cpp:4527` | `CASPool.DirectAndStartupTerminalFailuresRethrowTypedExceptions` | the lease bound (`PreAttemptDeadline`), and the startup re-anchor | the direct `RefusedAfterSend` case only, a request bound through `renewal_live_for_test`; renamed `CASPool.DirectTerminalFailureRethrowsTypedException` |
| `gtest_cas_pool.cpp:4941` | `CASPool.RenewWatermarkOnceRefreshesFenceAndDepositsOneFailure` | the lease bound reached inside the attempt (`before_throw` moves the clock to 1500) | a definitive answer: GC fences the slot out (`fenceOutMount`); the code is `ABORTED`, the fence and the deposit assertions stay |

#### The same hazard one level down: renewer tests that relied on the lease bound

Cycle 3 removes `MountRenewPolicy`, so every `MountLeaseRenewer::renew` in a test runs
`Retry::untilDefinitive` (the default policy was `LeaseBound`). Enumeration:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
grep -rn -E '(\.|->)renew\(|renewOrThrow\(' src/Disks/tests | grep -v 'renewWatermarkOnce'
grep -rn -E 'sleep_step_ms=|EnvelopeEatingBackend|outage *=|read_outage *=' src/Disks/tests/gtest_cas_heartbeat.cpp
```

Every direct renewal in `gtest_cas_mount.cpp` (4 `renewOrThrow`, one `renew`), `gtest_cas_event_log.cpp:347`,
`gtest_cas_gc_ack_floor.cpp:1009` and `gtest_cas_heartbeat.cpp` renews over a healthy slot, meets a definitive
answer, or is bounded by its liveness or by `UnboundedRenewalFixture`'s request count, except these, which
stop only at the lease bound:

| Test | Shape | Fate |
|---|---|---|
| `CASHeartbeat.DeadlineBeforeSendTerminalizesWithTypedFailure` (:1053) | refused by the lease before sending | deleted (contract) |
| `CASHeartbeat.RenewalStopsBeforeTheCutoffWhenEveryAttemptConsumesTheEnvelope` (:1396) | persistent fault, ends at the cutoff; would hang | deleted (contract), with `EnvelopeEatingBackend` |
| `CASHeartbeat.BoundedPathsStillStopAtTheLeaseDeadline` (:1973) | both bounded entry points | deleted (contract), in cycle 2 |
| `CASHeartbeat.WallClockStepsAndBootSuspendCannotExtendAuthority` (:1330) | suspend case refused by the lease | adapted (contract): the renewal commits with its own start |
| `CASHeartbeat.RenewerStateAllowsOnlyActiveReleaseOrTerminal` (:922), block `"terminal"` | `sleep_step_ms` jumps past the bound so one ambiguous attempt is the only one | adapted: live until the resolving read has run |
| `CASHeartbeat.LateDeliveryAfterTerminalCannotRearmOrOverwriteSuccessor` (:1257), both blocks | same | adapted: same |
| `CASMountLease.FarewellRunsOnAnOpenFenceAfterTheMountFenceIsLost` (`gtest_cas_mount.cpp:2337`) | a renewal refused by a lost mount fence | adapted (contract): the refused-renewal half goes |

`CASHeartbeat.RenewalOverConnectFailuresRecoversWithoutASettleRead` (:1020, `USE_AWS_S3` only) keeps its
assertions: the 60 connect failures are now spaced (one draw each on the injected clock) instead of 50 ms
apart, and the renewal still commits on the 61st attempt. Only its comment changes.

#### Mechanical sweeps

| Sweep | Before | After | Find | Sites | Verify (must print nothing) |
|---|---|---|---|---:|---|
| `MountLeaseRenewer` construction, `Ops` | `ops.mount, ops.farewell, ops.lease,` | `ops.farewell, ops.lease,` | `grep -c 'ops\.mount, ops\.farewell, ops\.lease,'` | heartbeat 28 (30 less the two deleted tests), mount 8 | `git grep -n -E 'ops(\.\|->)mount, ops(\.\|->)farewell' -- src/Disks/tests` |
| same, the heartbeat fixture | `ops->mount, ops->farewell, ops->lease,` | `ops->farewell, ops->lease,` | same file | 1 | same |
| same, explicit planes in `gtest_cas_mount.cpp` | `mount_requests, open_requests, open_requests,` | `open_requests, mount_requests,` | `grep -c` | 2 after cycle 2 (`departing`, `ClaimIsNotAdmittedUnderTheMountFence`) | `git grep -n 'mount_requests, open_requests' -- src/Disks/tests` |
| same, `gtest_cas_gc_ack_floor.cpp` | `renewer_requests, renewer_requests, renewer_requests,` | `renewer_requests, renewer_requests,` | `grep -c` | 2 | `git grep -n 'renewer_requests, renewer_requests, renewer_requests' -- src/` |
| same, by hand | `gtest_cas_event_log.cpp:373` (three `*planes[index],` lines), `gtest_cas_mount_claim_conflicts.cpp:27` (three `requests,` lines), `R` `installRenewer` | one argument fewer | | 3 | the build |
| `UnboundedRenewalFixture::renew` call lines | `MountRenewPolicy::UntilDefinitive, ` and `MountRenewPolicy::UntilDefinitive)` | removed / `)` | `grep -c 'MountRenewPolicy::UntilDefinitive' src/Disks/tests/gtest_cas_heartbeat.cpp` | 15 | `git grep -n MountRenewPolicy -- src/` |
| `renewalEnvironment(boot_ms, ...)` in heartbeat | unchanged at the call; the helper drops `.policy` | | `grep -c 'renewalEnvironment('` | 23 (20 calls after the deletions) | none |
| `CasMountRuntime` construction | `backend, mount, farewell, lease,` / `pool_backend, mount_requests, farewell_requests, lease_requests,` | without the first plane | `git grep -n -E 'runtime\(\|mount_runtime\(' -- src/` | 3 (`RuntimeUnderTest`, `RuntimeFixture`, `Pool::Pool`) | the build |

`MountLeaseRenewer` constructions at `7e243502408`: 48 (`R` 1, heartbeat 31, mount 12, gc_ack_floor 2,
event_log 1, claim_conflicts 1), derived with
`git grep -c -E "MountLeaseRenewer [a-z_0-9]+\(|make_unique<MountLeaseRenewer>\(|return MountLeaseRenewer\(" -- src/`.
This task deletes four of them with their tests (`DeadlineBeforeSend...`, `RenewalStopsBefore...`,
`RemountRenewalIsAdmittedOffTheMountFence`, and the `renewing` renewer of `FarewellRuns...`). A missed site
does not compile: the third argument becomes `const Layout &`, which a `CasRequests` cannot bind to.

---

#### Cycle 1: the runtime's re-anchors, `RenewCaller` and the seam over `renewOnce`

- [ ] **Step 1: Re-derive the enumeration on the tree as left by Task 2**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
git grep -n -E 'renewRenewerForStartupOnce|renewRenewerForRemountOnce|RenewCaller::(Startup|Remount|Direct)' -- src/
grep -rn 'renewWatermarkOnce' src/Disks/tests | wc -l
grep -rn -B10 -A6 'renewWatermarkOnce' src/Disks/tests | grep -E 'EXPECT_THROW|expectThrows|try$|catch'
```

Expected: the first grep names only the declarations and definitions in `RH`/`R` and the `Direct` call in
`renewWatermarkOnce` (no `P` line: Task 2 removed both callers). The count is 220 (221 less
`DirectRenewCannotRaceWorkerStartOrRenewerReplacement`) plus any line Tasks 1 and 2 added. Every throwing
site in the third output is in the tables above; a new one is classified the same way before continuing.

- [ ] **Step 2: Implement**

`$W/RH`. Delete the public declarations of `renewRenewerForStartupOnce` and `renewRenewerForRemountOnce`
(at `7e243502408` RH:387-388, next to `startRenewer`). Delete `enum class RenewCaller` and its comment.
Replace the private declarations of `renewRenewerOnce(RenewCaller)`, `renewalEnvironment(RenewCaller)`,
`renewalLive(RenewCaller)` and `consumeRenewResult(const MountRenewResult &, RenewCaller)`, as left by
Task 2, with:

```cpp
    /// The renewal step of the lease thread: renew, consume the result under `driver_mutex`, then report
    /// it with no lock held.
    MountRenewResult renewOnce();
    MountRenewOperationEnvironment renewalEnvironment();
    /// The renewal's liveness: no stop, no pending remount request, no terminal lifecycle. FALSE ends it.
    bool renewalLive() const;
    void consumeRenewResult(const MountRenewResult & result);
```

`$W/RH`, the public `renewWatermarkOnce` (RH:175-177):

```cpp
    /// Runs the lease thread's renewal step once on the calling thread and rethrows a terminal failure:
    /// the test seam. Refused when the runtime is configured for a lease thread, and on a read-only
    /// runtime, which has no renewer.
    void renewWatermarkOnce();
```

`$W/RH`, the doc of `noteRenewRequest` (RH:327):

```cpp
    /// Counts each `PUT` of a renewal as it is sent and keeps the text of every failed `PUT` or resolve
    /// read. Runs on the renewing thread.
    void noteRenewRequest(const MountRenewRequestEvent & event) noexcept;
```

`$W/R`. Delete the definitions of `CasMountRuntime::renewRenewerForStartupOnce` and
`CasMountRuntime::renewRenewerForRemountOnce` (as left by Task 2; R:644-660 at `7e243502408`).

`CasMountRuntime::renewalEnvironment`, the whole function:

```cpp
MountRenewOperationEnvironment CasMountRuntime::renewalEnvironment()
{
    return MountRenewOperationEnvironment{
        .boot_ms = [this] { return bootMsNow(); },
        .live = [this]
        {
            return renewalLive() && (!config.renewal_live_for_test || config.renewal_live_for_test());
        },
        .cancelled = [this] { return renewalCancelled(); },
        .policy = MountRenewPolicy::UntilDefinitive,
        /// Counts the requests as they are sent, so an outage shows while it lasts.
        .on_request = [this](const MountRenewRequestEvent & event) { noteRenewRequest(event); },
    };
}
```

The `.policy` line is needed until cycle 3: the field still defaults to `LeaseBound`.

`CasMountRuntime::renewalLive`: the `Loop` arm as left by Task 2, without the parameter. The contract's
form, which Task 2 produced:

```cpp
bool CasMountRuntime::renewalLive() const
{
    std::lock_guard lock(driver_mutex);
    return !workers_stop_requested
        && remount_requested_generation <= remount_handled_generation
        && !remountTerminal();
}
```

Lock: `driver_mutex` only, around reads of a flag, two counters and two atomics; no I/O, no log, no
callback. The engine calls it on the renewing thread with no lock held. `renewal_live_for_test` runs in
the lambda above, outside the lock, as before.

`CasMountRuntime::renewRenewerOnce` becomes `CasMountRuntime::renewOnce`. Take the body as left by
Task 2 and make these edits, nothing else:

- drop the parameter;
- the observability call keeps the arguments of the `Loop` path:
  `configureMountRenewObservability(&server_root_id, &event_sink, /*deferred=*/false);` (delete the
  comment above it about the parked redo, if one is left);
- the renewal call is the plain one, with no `renewForRemount` branch:
  `const MountRenewResult result = renewer->renew(renewalEnvironment());` (the renewer pointer keeps the
  name Task 1 gave it);
- `consumeRenewResult(result, caller)` becomes `consumeRenewResult(result)`;
- every other `caller == RenewCaller::Loop` becomes `true` and every comparison with `Startup`, `Remount`
  or `Direct` becomes `false`; delete the branches that become dead.

`CasMountRuntime::consumeRenewResult`: drop the parameter and apply the same rule. Concretely:

- delete the block that counts attempts from the result for the other callers (at `7e243502408`
  R:565-569, the comment "The worker's renewal counts its requests as they are sent ... Every other
  renewal counts them here, from its result" with its `if`); every renewal now counts its requests in
  `noteRenewRequest`, which `renewalEnvironment` installs for all of them;
- on `Terminal`, keep only the `Loop` rule: under `driver_mutex`, a stop trips the fence with
  `tripFenceWithoutOperationalLoss`; otherwise `tripMountLost`, `++schedule_remount_calls_for_test`, and a
  new remount generation if the lifecycle is not terminal and none is pending. Delete the `Startup`
  (`tripFenceWithoutOperationalLoss` and rethrow), `Remount` (rethrow only) and `Direct` arms. If Task 2's
  `Loop` rule carries a `config.background_watermark` term, keep it;
- `Committed` and `NotAttempted` keep the `Loop` handling unchanged, including the arm on a commit from
  Task 2.

Leave the `CASMountRenewalDeadlineExceeded` increment in place until cycle 4.

`CasMountRuntime::renewalLoop`: the renewal call, as left by Task 2:

```cpp
        if (config.renewal_admitted_hook_for_test)
            config.renewal_admitted_hook_for_test();
        (void)renewOnce();
```

`CasMountRuntime::renewWatermarkOnce`: the refusals as left by Task 2, then the step:

```cpp
void CasMountRuntime::renewWatermarkOnce()
{
    {
        std::lock_guard lock(driver_mutex);
        if (config.background_watermark)
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "CAS mount runtime: direct renewal is disabled when background ownership is configured");
        if (!mount_renewer)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: renewal without a renewer");
        if (mount_renewer->state() != MountLeaseRenewerState::Active)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: renewal requires an Active renewer");
    }
    const MountRenewResult result = renewOnce();
    if (result.outcome == MountRenewOutcome::Terminal)
        std::rethrow_exception(result.failure);
}
```

If Task 2 left these checks in another shape, keep its shape; only the call becomes `renewOnce()`. The
messages must keep the prefix "CAS mount runtime": `EXPECT_RUNTIME_STATE_REJECTION` matches it.

Lock rule: no new critical section. The refusals above run under `driver_mutex` with no I/O; the lock is
released before `renewOnce`.

What the seam changes, beyond the plane and the policy, on a runtime with no lease thread:

- Its liveness now also ends it on a pending remount request or a terminal lifecycle. No test raises a
  request (`scheduleRemountForTest`, `reportImpossibleInterference`) and then expects a seam renewal to
  commit: `git grep -n -E "scheduleRemountForTest|reportImpossibleInterference" -- src/Disks/tests` and the
  seam call sites share no test.
- A terminal seam renewal raises a remount generation unless Task 2's rule keeps a `background_watermark`
  term. The tests that continue after a terminal seam renewal are `FenceOutThenSelfRemountRestoresWrites`
  (its `tryRemountOnce` acknowledges the generation in `armIfAdmissible`, so it still arms),
  `SupersededIncarnationDoesNotReclaimALiveSuccessor` and `verifyForeignConflictSinkIsNonInterfering`;
  none reads the generation.
- A failed request now sets `lastRenewFailure`.

`$W/P`, `Pool::mountWritable`, the comment on `claim_anchor_boot_ms` (P:712-714 at `7e243502408`), if
Task 2 left it:

```cpp
    /// The pre-I/O boot-clock instant of the claim attempt FINALLY adopted below -- survives the
    /// `break`: the fence deadline after the open starts from it.
    uint64_t claim_anchor_boot_ms = 0;
```

Then `grep -n -i "re-anchor" $W/CA/Pool/CasPool.cpp $W/CA/Pool/CasMountRuntime.cpp` must print nothing.

- [ ] **Step 3: Delete and adapt the tests**

Delete (subject unreachable: the seam has no lease bound):
- `CASEvent.AnAmbiguityPastTheLeaseBoundNeverStartsTheResolvingRead` (`gtest_cas_event_log.cpp:288-323`, with its comment).
- `CASObservability.ExternalLeaseDeadlineCountsOnceWithoutReconstructingAttempts` (`gtest_cas_observability.cpp:180-210`).

`$W/T/gtest_cas_event_log.cpp`, `TerminalRenewalDetailsPreservePhysicalTruthAndClassification`: replace the
comment above the test and the second block. The first block (deterministic rejection) stays:

```cpp
/// Two terminal endings and what their rows carry: the store rejecting the renewal outright, and the
/// slot vanishing under it.
TEST(CASEvent, TerminalRenewalDetailsPreservePhysicalTruthAndClassification)
```

```cpp
    {
        auto backend = std::make_shared<RenewalEventBackend>();
        auto boot_ms = std::make_shared<std::atomic<uint64_t>>(100);
        /// Heap-owned, not a plain local: the Pool can outlive this stack frame (a background publish
        /// holds `shared_from_this()`), so a by-reference capture of a local would dangle.
        auto events = std::make_shared<DB::Cas::tests::SharedEventLog>();
        auto store = openRenewalEventPool(backend, boot_ms, renewalEventBudget(), "renewal-vanished-details");
        store->setEventSink([events](CasEvent event)
        {
            events->push(std::move(event));
        });
        backend->vanish_on_next_write = true;

        EXPECT_THROW(store->renewWatermarkOnce(), DB::Exception);
        const std::optional<CasEvent> failed = one_failed_event(events->snapshot());
        ASSERT_TRUE(failed.has_value()) << "the vanished slot must reach the event log";
        EXPECT_EQ(failed->detail.at("attempts_sent"), "1");
        EXPECT_EQ(failed->detail.at("classification"), "vanished");
    }
```

Kept assertion: a terminal row carries the request count and the classification.

`$W/T/gtest_cas_mount.cpp`, class `RenewalLogBackend` (:236-253):

```cpp
class RenewalLogBackend final : public InMemoryBackend
{
public:
    bool throw_before_next_overwrite = false;
    bool refuse_next_overwrite = false;

    /// The faults live on the primitive every write reaches the store through, keyed to the mount
    /// slot so the pool's other conditional writes pass untouched.
    std::expected<String, RawConflict> write(
        const String & key,
        const String & bytes,
        const std::optional<String> & expected_value,
        TransportAccess & access) override
    {
        if (expected_value && key.ends_with("/mount"))
        {
            if (std::exchange(refuse_next_overwrite, false))
                throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "injected deterministic renewal rejection");
            if (std::exchange(throw_before_next_overwrite, false))
                throw Poco::TimeoutException("injected renewal timeout before commit");
        }
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }
};
```

Same file, `CASMountAudit.RenewalDefaultLogsAreBounded`, the fourth block (:371-385):

```cpp
    {
        auto backend = std::make_shared<RenewalLogBackend>();
        auto boot_ms = std::make_shared<std::atomic<uint64_t>>(100);
        auto store = open_store(backend, boot_ms, "renewal-log-fenced");
        ScopedRenewalLogCapture capture("information");
        backend->refuse_next_overwrite = true;
        EXPECT_THROW(store->renewWatermarkOnce(), DB::Exception);
        const String output = capture.captured();
        EXPECT_EQ(countRenewalLogText(output, "CAS mount renewal"), 1u) << output;
        EXPECT_EQ(countRenewalLogText(output, "fenced"), 1u) << output;
    }
```

Kept assertion: a terminal renewal logs one line at the default level, and it is the fence line.

`$W/T/gtest_cas_pool.cpp`, replace `TEST(CASPool, DirectAndStartupTerminalFailuresRethrowTypedExceptions)`
(:4492-4539) with:

```cpp
/// The test seam rethrows a terminal renewal as the typed failure it ended with.
TEST(CASPool, DirectTerminalFailureRethrowsTypedException)
{
    std::atomic<bool> renewal_live{true};
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("typed-direct");
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind, MountClaimResult::Claimed);
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .boot_ms_fn = [&] { return boot_ms; },
            .renewal_live_for_test = [&] { return renewal_live.load(std::memory_order_acquire); }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    /// The renewal lands, then its liveness ends before the commit is confirmed.
    backend->after_commit = [&] { renewal_live.store(false, std::memory_order_release); };
    try
    {
        runtime.renewWatermarkOnce();
        ADD_FAILURE() << "terminal renewal did not propagate";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::NETWORK_ERROR) << e.message();
    }
    runtime.finishTeardown(false);
}
```

The locals the hooks capture are declared before the backend and the runtime that store them. If Task 1
or 2 changed `armMountFence`'s arguments, use theirs. Kept assertion: a terminal renewal after a send
propagates as `NETWORK_ERROR`. The `PreAttemptDeadline` case goes: a renewal has no lease bound to refuse
it; the startup case goes with `renewRenewerForStartupOnce`.

Same file, `CASPool.RenewWatermarkOnceRefreshesFenceAndDepositsOneFailure` (:4909-4945), the part after
`EXPECT_TRUE(store->mayMutate()) << "direct success must refresh the local fence from attempt start";`:

```cpp
    fake_boot->store(1200);
    EXPECT_TRUE(store->mayMutate()) << "direct success must refresh the local fence from attempt start";

    /// GC fences the slot out: a definitive answer, so the renewal ends on its first attempt.
    fenceOutMount(*backend, store->layout().mountKey("test"));
    const uint64_t schedules_before = store->scheduleRemountCallCountForTest();
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { store->renewWatermarkOnce(); });
    EXPECT_FALSE(store->mayMutate());
    EXPECT_EQ(store->scheduleRemountCallCountForTest(), schedules_before + 1);
}
```

`fenceOutMount` is the anonymous-namespace helper at :1612, above this test. Kept assertions: a success
refreshes the fence from its start; a terminal renewal trips the fence and deposits one failure.

- [ ] **Step 4: Build, run the tests of this cycle, then the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/p2t3_c1_build.log 2>&1; echo NINJA_EXIT=$? >> build/p2t3_c1_build.log
tail -1 build/p2t3_c1_build.log; grep -a -n "error:" build/p2t3_c1_build.log | head -40
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASEvent.*:CASObservability.*:CASMountAudit.*:CASPool.DirectTerminalFailureRethrowsTypedException:CASPool.RenewWatermarkOnceRefreshesFenceAndDepositsOneFailure:CASPoolRemount.FenceOutThenSelfRemountRestoresWrites:CASMountRemount.*:CASPoolRemount.*ForeignConflictSink*:CASPoolRemount.DirectRenewIsRefusedForBackgroundConfiguredRuntimeAfterStop:CASMountRuntime.*' > build/p2t3_c1_tests.log 2>&1; echo GATE_EXIT=$? >> build/p2t3_c1_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/p2t3_c1_tests.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CAS*' > build/p2t3_c1_gate.log 2>&1; echo GATE_EXIT=$? >> build/p2t3_c1_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/p2t3_c1_gate.log | tail -20
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 grep -n -E 'RenewCaller|renewRenewerOnce|renewRenewerFor(Startup|Remount)Once' -- src/
```

Expected: `NINJA_EXIT=0`; both runs `GATE_EXIT=0`, no `FAILED`; the gate has two tests fewer than Task 2
left; the last grep prints nothing.

- [ ] **Step 5: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp \
  src/Disks/tests/gtest_cas_event_log.cpp \
  src/Disks/tests/gtest_cas_observability.cpp \
  src/Disks/tests/gtest_cas_mount.cpp \
  src/Disks/tests/gtest_cas_pool.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Renew the CAS mount lease through one step, also from the test seam" \
  -m "The startup and remount re-anchors had no caller left. renewOnce is the lease thread's renewal step: it renews on the lease plane with no lease bound, consumes the result and reports it. renewWatermarkOnce runs that step on the calling thread for tests. Tests that relied on the seam stopping at the lease deadline now end on a definitive answer or on their liveness." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

Omit `CasPool.cpp` from `add` if Step 2 left it unchanged.

---

#### Cycle 2: `MountLeaseRenewer::renewForRemount`

- [ ] **Step 6: Delete it and its tests**

`$W/SH`, in `class MountLeaseRenewer`, delete the comment and declaration (SH:582-587):

```cpp
    /// The remount's re-anchor, which is bootstrap control rather than steady state: a remount renews
    ...
    MountRenewResult renewForRemount(const MountRenewOperationEnvironment & environment = {});
```

and replace the comment on `renewOn` (SH:599) with:

```cpp
    /// One renewal admitted on `plane`.
    MountRenewResult renewOn(CasRequests & plane, const MountRenewOperationEnvironment & environment);
```

`$W/S`: delete `MountLeaseRenewer::renewForRemount` (S:1587-1590).

Delete the tests (subject unreachable):
- `CASHeartbeat.BoundedPathsStillStopAtTheLeaseDeadline` (`gtest_cas_heartbeat.cpp:1972-1997`, with its one-line comment): both bounded entry points.
- `CASMountLease.RemountRenewalIsAdmittedOffTheMountFence` (`gtest_cas_mount.cpp:2443-2474`, with its comment): `renewForRemount`.

- [ ] **Step 7: Build, run, gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/p2t3_c2_build.log 2>&1; echo NINJA_EXIT=$? >> build/p2t3_c2_build.log
tail -1 build/p2t3_c2_build.log; grep -a -n "error:" build/p2t3_c2_build.log | head -40
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASHeartbeat.*:CASMountLease.*' > build/p2t3_c2_tests.log 2>&1; echo GATE_EXIT=$? >> build/p2t3_c2_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/p2t3_c2_tests.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CAS*' > build/p2t3_c2_gate.log 2>&1; echo GATE_EXIT=$? >> build/p2t3_c2_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/p2t3_c2_gate.log | tail -20
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 grep -n renewForRemount -- src/
```

Expected: `NINJA_EXIT=0`; `GATE_EXIT=0` twice, no `FAILED`; two tests fewer than cycle 1; the grep prints
nothing.

- [ ] **Step 8: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp \
  src/Disks/tests/gtest_cas_heartbeat.cpp \
  src/Disks/tests/gtest_cas_mount.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Remove MountLeaseRenewer::renewForRemount" \
  -m "No remount renews before it arms the fence, so the renewal on the claim-and-farewell plane has no caller." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

#### Cycle 3: `MountRenewPolicy`, `renewOn` and the fence-gated renewal plane

The renewer's `mount_requests`, the policy and the runtime's `mount_requests` go in one cycle: once `renew`
stops choosing a plane, the renewer's member is unread, and once `installRenewer` stops passing it, so is
the runtime's; an unread private reference fails the build under `-Werror`.

- [ ] **Step 9: Implement**

`$W/SH`. Delete `enum class MountRenewPolicy` with its comment (SH:64-69). The spacing constant:

```cpp
/// The spacing of a renewal's retries.
inline constexpr uint64_t kMountRenewRetrySpacingMs = 1000;
```

In `struct MountRenewOperationEnvironment` delete `MountRenewPolicy policy = MountRenewPolicy::LeaseBound;`
(SH:93). The members are then `boot_ms`, `live`, `cancelled`, `on_request`, in that order.

`class MountLeaseRenewer`: replace the PLANES paragraph of the class comment (SH:552-559) with:

```cpp
/// PLANES. A renewal runs on `lease_requests`, which has no lease budget: it keeps trying after the
/// lease expired, and a stop, a remount request or a terminal lifecycle reaches it through its
/// liveness. The claim and the farewell are admitted on `open_requests`, off the fence: a self-remount
/// claims with the fence already latched lost, so a claim gated on the fence could never reclaim, and a
/// farewell refused because the fence has run down would leave the slot looking live until GC fences it
/// out. Neither is unguarded: a claim's safety is its own conditional write, and a caller that has
/// shutdown facts hands them over as a `Liveness`.
```

The constructor declaration (SH:563-572):

```cpp
    MountLeaseRenewer(
        CasRequests & open_requests_, CasRequests & lease_requests_,
        const Layout & layout_,
        const String & srid_, UInt128 server_uuid_,
        uint64_t writer_epoch_, std::chrono::milliseconds ttl_, std::function<uint64_t()> now_ms_fn_,
        std::function<uint64_t()> min_active_build_sequence_fn_,
        CasEventSink event_sink_ = {},
        std::chrono::milliseconds lease_safety_margin_ = std::chrono::milliseconds(2000),
        /// boot-domain clock for the on_renew_ok anchor; empty = real CLOCK_BOOTTIME. Injectable for
        /// tests and wired by CasMountRuntime::installRenewer.
        std::function<uint64_t()> boot_ms_fn_ = {});
```

`renew` (SH:578-581):

```cpp
    /// One renewal on `lease_requests` under `Retry::untilDefinitive(kMountRenewRetrySpacingMs)`. Ends on
    /// a definitive answer, a deterministic local failure, or when `environment.live` refuses.
    MountRenewResult renew(const MountRenewOperationEnvironment & environment);
```

Delete the declaration of `renewOn` with its comment. The members (SH:606-609):

```cpp
    CasRequests & open_requests;
    /// The plane of a renewal: no lease budget, and a sleep a stop wakes.
    CasRequests & lease_requests;
```

`$W/S`, `MountLeaseRenewer::MountLeaseRenewer` (S:1326-1347):

```cpp
MountLeaseRenewer::MountLeaseRenewer(
    CasRequests & open_requests_, CasRequests & lease_requests_,
    const Layout & layout_,
    const String & srid_, UInt128 server_uuid_,
    uint64_t writer_epoch_, std::chrono::milliseconds ttl_, std::function<uint64_t()> now_ms_fn_,
    std::function<uint64_t()> min_active_build_sequence_fn_,
    CasEventSink event_sink_,
    std::chrono::milliseconds lease_safety_margin_,
    std::function<uint64_t()> boot_ms_fn_)
    : open_requests(open_requests_)
    , lease_requests(lease_requests_)
    , key(layout_.mountKey(srid_))
    , srid(srid_)
    , server_uuid(server_uuid_)
    , writer_epoch(writer_epoch_)
    , ttl(ttl_)
    , now_ms_fn(std::move(now_ms_fn_))
    , min_active_build_sequence_fn(std::move(min_active_build_sequence_fn_))
    , event_sink(std::move(event_sink_))
    , lease_safety_margin(lease_safety_margin_)
    , boot_ms_fn(boot_ms_fn_ ? std::move(boot_ms_fn_) : defaultBootMs)
{
}
```

`MountLeaseRenewer::renew` and `MountLeaseRenewer::renewOn` (S:1581-1594): delete the forwarding `renew`
and rename `renewOn` to `renew`, so the function starts:

```cpp
MountRenewResult MountLeaseRenewer::renew(const MountRenewOperationEnvironment & environment)
{
    const MountRenewObservabilityRegistration observability_registration = beginMountRenewObservabilityCall();
```

In its body, two hunks. The admission (S:1631):

```cpp
    CasOperation op = lease_requests.admit(environment.live);
```

The policy (S:1642-1644):

```cpp
    const Retry policy = Retry::untilDefinitive(kMountRenewRetrySpacingMs);
```

The rest of the body is unchanged.

`$W/RH`, the constructor (RH:148-160):

```cpp
    CasMountRuntime(
        BackendPtr backend_ptr_,
        /// The planes the `MountLeaseRenewer` runs on: the claim and the farewell on an open-fence one,
        /// the renewal on `lease_requests_`, which has no lease budget and whose sleep a stop wakes.
        /// Owned by `Pool` and outliving this runtime.
        CasRequests & farewell_requests_,
        CasRequests & lease_requests_,
        const Layout & layout_,
```

and delete the member `CasRequests & mount_requests;` (RH:485).

`$W/R`, the constructor (R:55-74): delete the parameter `CasRequests & mount_requests_,` and the
initializer `, mount_requests(mount_requests_)`. `CasMountRuntime::installRenewer`:

```cpp
    auto replacement = std::make_unique<MountLeaseRenewer>(
        farewell_requests, lease_requests, layout, server_root_id, our_uuid, writer_epoch,
```

`CasMountRuntime::renewalEnvironment`: delete the line `.policy = MountRenewPolicy::UntilDefinitive,`.

`$W/P`, `Pool::Pool`. The `ref_ledger` comment (P:234-235), first sentence:

```cpp
    /// Ref-log / ref-table subsystem, on the MOUNT plane. Injected with the
```

The runtime (P:257-267):

```cpp
    /// Mount / write-fence / build-watermark / self-remount runtime. Injected with
    /// backend/layout + the farewell and lease planes + the `MountConfig` slice + `server_root_id` + the event-sink reference + the pool
    /// `cas_request_budget` + the `remount_attempt` callback (== `Pool::tryRemountOnce`, whose claim/
    /// recovery ORCHESTRATION stays on Pool). The callback captures `this`; it is invoked only at runtime
    /// (post-construction). Declared/constructed AFTER `ref_ledger`, preserving the original member order
    /// verbatim (mount destroyed first, ledger last; both orders proven safe -- see the header note).
    , mount_runtime(
          pool_backend, farewell_requests, lease_requests,
          pool_layout, config.mountConfig(), config.server_root_id, event_sink_,
          config.cas_request_budget,
          [this] { return tryRemountOnce(); })
```

`$W/CA/Pool/CasRefLedger.h`, the comment on the first constructor parameter (:94-95):

```cpp
        /// The mount plane. Every request this ledger makes is admitted on it.
        CasRequests & mount_requests_,
```

- [ ] **Step 10: Sweep and adapt the tests**

Fixtures:

`$W/T/gtest_cas_pool.cpp`, `RuntimeUnderTest` (:2074-2136, as left by Task 2): delete the member
`DB::Cas::CasRequests mount;`, its initializer `: mount(backend, DB::Cas::Fence{...})` and the line
`mount.setNowFnForTest([this] { return runtime.bootMsNow(); });`. The first initializer becomes
`farewell`, and the runtime is built without the mount plane:

```cpp
/// A directly-constructed `CasMountRuntime` plus the request planes `Pool` would give it.
class RuntimeUnderTest
{
public:
    template <typename BackendT, typename... Args>
    RuntimeUnderTest(const std::shared_ptr<BackendT> & backend, Args &&... args)
        : farewell(backend, DB::Cas::Fence::open())
        , lease(backend, DB::Cas::Fence::open())
        , runtime(backend, farewell, lease, std::forward<Args>(args)...)
```

`setRetrySleepForTest`:

```cpp
    /// The retry wait of the lease plane, where the renewal runs. Call before the lease thread starts.
    void setRetrySleepForTest(const std::function<void(uint64_t)> & sleep_fn)
    {
        lease.setSleepFnForTest(sleep_fn);
    }
```

`$W/T/gtest_cas_mount_runtime.cpp`, `RuntimeFixture` (:25-60): delete the member `CasRequests mount;` and its
initializer `, mount(backend, Fence{...})`; the runtime:

```cpp
        , farewell(backend, Fence::open())
        , lease(backend, Fence::open())
        , runtime(
              backend, farewell, lease, layout,
```

`$W/T/gtest_cas_heartbeat.cpp`:

- `Ops` (:41-71): the class comment loses its `sleep_step_ms` sentences, and the parameter goes:

```cpp
/// The request planes this file's renewers run on. All are open-fence -- the exclusivity these tests
/// exercise is the mount protocol's own, not a fence's -- on the same injected boot clock the renewer's
/// lease deadline is expressed on, so they never disagree about how much budget is left.
/// `tests::OperationForTest` covers a fixture needing one operation, but neither the planes a renewer
/// takes nor this clock, which is why this stays local.
class Ops
{
public:
    Ops(std::shared_ptr<Backend> backend, uint64_t * boot_ms)
        : mount(openRequestsForTest(backend))
        , farewell(openRequestsForTest(backend))
        , lease(openRequestsForTest(std::move(backend)))
        , op(mount.admit())
    {
        for (CasRequests * requests : {&mount, &farewell, &lease})
        {
            requests->setNowFnForTest([boot_ms] { return *boot_ms; });
            requests->setSleepFnForTest([boot_ms](uint64_t ms) { *boot_ms += ms; });
        }
    }
```

- `RenewalScriptBackend` (:91-94): nothing derives from it once `EnvelopeEatingBackend` goes; delete the
  three-line "Not `final`" comment and declare `class RenewalScriptBackend final : public InMemoryBackend`.
- `renewalEnvironment` (:241-253): delete `.policy = MountRenewPolicy::LeaseBound,`.
- Delete `CASHeartbeat.DeadlineBeforeSendTerminalizesWithTypedFailure` (:1053-1079): a renewal refused by
  its lease before sending.
- Delete the comment, the anonymous namespace holding `EnvelopeEatingBackend`, and
  `CASHeartbeat.RenewalStopsBeforeTheCutoffWhenEveryAttemptConsumesTheEnvelope` (:1356-1415): a lease-bound
  renewal; `RenewalOutlivesTheReservationCutoff` covers the unbounded one.
- `UnboundedRenewalFixture` (:1418-1501): the class comment and `renew`:

```cpp
/// The production shape at test scale: a 30 s lease, renewed one 10 s period after its anchor, with a
/// 2 s safety margin and a 7 s attempt envelope.
class UnboundedRenewalFixture
```

```cpp
    MountRenewResult renew(
        const std::function<bool()> & live = {},
        const std::function<bool()> & cancelled = {},
        std::function<void(const MountRenewRequestEvent &)> on_request = {})
    {
        /// The test clock moves only through the sleep seam, so a regression that zeroes every pause would
        /// loop forever; the request count bounds the renewal and the check below fails it instead.
        const auto bounded_live = [this, live]
        {
            if (backend->attempts.size() + backend->read_calls >= max_requests)
            {
                request_bound_hit = true;
                return false;
            }
            return !live || live();
        };
        MountRenewOperationEnvironment environment = renewalEnvironment(boot_ms, bounded_live, cancelled);
        environment.on_request = std::move(on_request);
        MountRenewResult result = renewer->renew(environment);
        EXPECT_FALSE(request_bound_hit) << "the renewal sent " << max_requests
            << " requests without the clock reaching the end of the outage: the spaced pauses are not advancing it";
        return result;
    }
```

- Delete the comment line above `CASHeartbeat.RenewalOutlivesTheReservationCutoff` (:1511), which speaks
  of a bounded renewal's cutoff.
- The call lines: `sed -i -E 's/MountRenewPolicy::UntilDefinitive, //; s/MountRenewPolicy::UntilDefinitive\)/)/' src/Disks/tests/gtest_cas_heartbeat.cpp`
  (15 lines, counted before with `grep -c 'MountRenewPolicy::UntilDefinitive'`; afterwards
  `grep -c MountRenewPolicy` prints 0).
- `CASHeartbeat.RenewalOverConnectFailuresRecoversWithoutASettleRead`, the comment before the loop:
  `/// Sixty connect failures, then the store answers.`
- `CASHeartbeat.RenewerStateAllowsOnlyActiveReleaseOrTerminal`, block `"terminal"` (:957-976):

```cpp
    {
        auto backend = std::make_shared<RenewalScriptBackend>();
        uint64_t wall_ms = 1000;
        uint64_t boot_ms = 100;
        Ops ops(backend, &boot_ms);
        seedOwnClaim(ops.op, layout, "terminal", uuid, 9, wall_ms, 1000);
        MountLeaseRenewer renewer(
            ops.farewell, ops.lease, layout, "terminal", uuid, 9, std::chrono::milliseconds(1000),
            [&] { return wall_ms; }, [] { return uint64_t{7}; }, {}, std::chrono::milliseconds(20),
            [&] { return boot_ms; });
        renewer.start();
        backend->actions = {RenewalScriptBackend::Action::ThrowBefore};
        backend->read_calls = 0;
        /// Live until the ambiguous attempt's resolving read has run, so that attempt is the only one
        /// sent and the renewal ends terminal with it unsettled.
        const MountRenewResult result = renewer.renew(
            renewalEnvironment(boot_ms, /*live=*/[&] { return backend->read_calls == 0; }));
        EXPECT_EQ(result.outcome, MountRenewOutcome::Terminal);
        EXPECT_NE(result.failure, nullptr);
        EXPECT_EQ(renewer.state(), MountLeaseRenewerState::RenewalTerminal);
        EXPECT_RENEWER_STATE_REJECTION(renewer.start());
        EXPECT_RENEWER_STATE_REJECTION(renewer.renew(renewalEnvironment(boot_ms)));
        EXPECT_RENEWER_STATE_REJECTION(renewer.release());
    }
```

  Kept assertion: a terminal renewer refuses `start`, `renew` and `release`.
- `CASHeartbeat.LateDeliveryAfterTerminalCannotRearmOrOverwriteSuccessor` (:1257-1328), both blocks: each
  `Ops ops(backend, &boot_ms, /*sleep_step_ms=*/10'000);` becomes `Ops ops(backend, &boot_ms);`; in the first
  block the comment above it becomes the comment of the renewal, and both renewals take a liveness bound.
  First block:

```cpp
        Ops ops(backend, &boot_ms);
        seedOwnClaim(ops.op, layout, "before-reclaim", UInt128{1}, 9, wall_ms, 1000);
        MountLeaseRenewer renewer(
            ops.farewell, ops.lease, layout, "before-reclaim", UInt128{1}, 9, std::chrono::milliseconds(1000),
            [&] { return wall_ms; }, [] { return uint64_t{0}; }, CasEventSink{}, std::chrono::milliseconds(20),
            [&] { return boot_ms; });
        renewer.start();
        backend->actions = {RenewalScriptBackend::Action::ThrowBeforeThenLandAfterResolve};
        backend->read_calls = 0;
        /// Live until the resolving read has run: the delayed write lands during that read, and the
        /// renewal ends terminal without a second attempt.
        const MountRenewResult result = renewer.renew(
            renewalEnvironment(boot_ms, /*live=*/[&] { return backend->read_calls == 0; }));
        EXPECT_EQ(result.outcome, MountRenewOutcome::Terminal);
```

  Second block, the renewal after `delayed_precondition` was read:

```cpp
        backend->actions = {RenewalScriptBackend::Action::ThrowBefore};
        backend->read_calls = 0;
        const MountRenewResult result = renewer.renew(
            renewalEnvironment(boot_ms, /*live=*/[&] { return backend->read_calls == 0; }));
        ASSERT_EQ(result.outcome, MountRenewOutcome::Terminal);
```

  Kept assertions: the late delivery does not revive the renewer; replaying it against the incarnation it
  named is refused once a successor holds the slot.
- `CASHeartbeat.WallClockStepsAndBootSuspendCannotExtendAuthority`, the suspend case (:1349-1355):

```cpp
    backend->attempts.clear();
    boot_ms += 10'000;
    const MountRenewResult suspended = renewer.renew(renewalEnvironment(boot_ms));
    ASSERT_EQ(suspended.outcome, MountRenewOutcome::Committed);
    EXPECT_EQ(suspended.attempt_start_boot_ms, boot_ms)
        << "a renewal after a suspend anchors at its own start, never at the deadline it last confirmed";
    EXPECT_EQ(renewer.lastCommittedAttemptStartBootMs(), boot_ms);
    EXPECT_EQ(backend->attempts.size(), 1u);
```

  Kept assertion: neither a wall-clock step nor a suspend extends authority; the authority after the
  suspend starts at the renewal's own start. Write admission across the suspend is
  `CASMountRuntime.AdmitRefusesAnExpiredLease` (`gtest_cas_mount_runtime.cpp`), untouched.
- The constructor sweep: `sed -i 's/ops\.mount, ops\.farewell, ops\.lease,/ops.farewell, ops.lease,/; s/ops->mount, ops->farewell, ops->lease,/ops->farewell, ops->lease,/' src/Disks/tests/gtest_cas_heartbeat.cpp`
  (28 + 1 lines).

`$W/T/gtest_cas_mount.cpp`:

- `CASMountLease.FarewellRunsOnAnOpenFenceAfterTheMountFenceIsLost` (:2332-2380), comment and test:

```cpp
/// The farewell is admitted on the claim-and-farewell plane, never on the renewal plane, so a renewal
/// plane that refuses everything cannot stop it.
TEST(CASMountLease, FarewellRunsOnAnOpenFenceAfterTheMountFenceIsLost)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout l("p");
    uint64_t now = 1000;
    uint64_t boot = 0;
    bool fence_lost = false;

    CasRequests mount_requests(backend, Fence{
        [] { return uint64_t{0}; },
        [&fence_lost](uint64_t, uint64_t) { return fence_lost ? Fence::Admit::LostOrRearmed : Fence::Admit::Ok; },
        [&fence_lost](uint64_t)
        {
            if (fence_lost)
                throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "mount fence lost");
        }});
    mount_requests.setNowFnForTest([&boot] { return boot; });
    mount_requests.setSleepFnForTest([&boot](uint64_t ms) { boot += ms; });
    CasRequests open_requests = openRequestsForTest(backend);
    open_requests.setNowFnForTest([&boot] { return boot; });
    open_requests.setSleepFnForTest([&boot](uint64_t ms) { boot += ms; });
    CasOperation seed = open_requests.admit();

    ASSERT_EQ(claimMount(seed, l, "departing", UInt128(1), 7, now, /*ttl*/ 1000).kind, MountClaimResult::Claimed);

    MountLeaseRenewer departing(open_requests, mount_requests, l, "departing", UInt128(1), 7,
                               std::chrono::milliseconds(1000), [&] { return now; }, [] { return uint64_t{0}; },
                               {}, std::chrono::milliseconds(0), [&] { return boot; });
    departing.start();

    fence_lost = true;

    EXPECT_NO_THROW(departing.release());
    const MountLease farewell = decodeMountLease(seed.read(l.mountKey("departing"), Retry::standard())->bytes);
    EXPECT_EQ(farewell.min_active_build_sequence, std::numeric_limits<uint64_t>::max());
}
```

  The fence callback's `LOGICAL_ERROR` is never constructed: nothing is admitted on that plane. Kept
  assertion: the farewell lands while the other plane refuses. It fails if `release` admits on the
  renewal plane.
- `CASMountLease.ClaimIsNotAdmittedUnderTheMountFence` (:2387): the refusing plane becomes the renewal plane,
  so the test still tells the two apart:

```cpp
    MountLeaseRenewer renewer(open_requests, mount_requests, l, "r", UInt128(1), 7,
                            std::chrono::milliseconds(1000), [&] { return now; }, [] { return uint64_t{0}; },
                            {}, std::chrono::milliseconds(0), [&] { return boot; });
```

- The constructor sweep: `sed -i 's/ops\.mount, ops\.farewell, ops\.lease,/ops.farewell, ops.lease,/' src/Disks/tests/gtest_cas_mount.cpp` (8 lines).

`$W/T/gtest_cas_event_log.cpp`, `DeepReentrancyPreservesDeterministicPhysicalAttemptTruth` (:373-376): delete
one of the three `*planes[index],` lines.

`$W/T/gtest_cas_gc_ack_floor.cpp`:
`sed -i 's/renewer_requests, renewer_requests, renewer_requests,/renewer_requests, renewer_requests,/' src/Disks/tests/gtest_cas_gc_ack_floor.cpp` (2 lines: `runExpiredMountFenceOutScenario`, `DefaultMonoClockTracksPoolsInjectedBootClockNotWallClock`).

`$W/T/gtest_cas_mount_claim_conflicts.cpp`, `makeRenewer` (:17-37): delete one of the three `requests,`
lines, and end the comment after its second sentence:

```cpp
/// One renewer for the mount slot of server-root "r", under (uuid=1, epoch=7) unless overridden. All
/// of its planes are the same open-fence one: what these tests exercise is the mount protocol's own
/// exclusivity, not a fence's.
```

- [ ] **Step 11: Build, run, gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/p2t3_c3_build.log 2>&1; echo NINJA_EXIT=$? >> build/p2t3_c3_build.log
tail -1 build/p2t3_c3_build.log; grep -a -n "error:" build/p2t3_c3_build.log | head -40
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASHeartbeat.*:CASMountLease.*:CASMountAudit.*:CASMountObservation.*:CASEvent.*:CASGCAckFloor.*:CASGcFenceOut.*:CASMountClaimConflicts.*:CASMountRuntime.*:CASPool.*:CASPoolRemount.*' > build/p2t3_c3_tests.log 2>&1; echo GATE_EXIT=$? >> build/p2t3_c3_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/p2t3_c3_tests.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CAS*' > build/p2t3_c3_gate.log 2>&1; echo GATE_EXIT=$? >> build/p2t3_c3_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/p2t3_c3_gate.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
git grep -n -E '\bMountRenewPolicy\b|\bLeaseBound\b|\bUntilDefinitive\b|renewOn\b|worker_requests|sleep_step_ms|EnvelopeEatingBackend' -- src/
git grep -n -E 'ops(\.|->)mount, ops(\.|->)farewell|mount_requests, open_requests|renewer_requests, renewer_requests, renewer_requests' -- src/Disks/tests
git grep -n mount_requests -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp
git grep -c -E "MountLeaseRenewer [a-z_0-9]+\(|make_unique<MountLeaseRenewer>\(|return MountLeaseRenewer\(" -- src/
```

Expected: `NINJA_EXIT=0`; both runs `GATE_EXIT=0`, no `FAILED`; two tests fewer than cycle 2; the three
greps print nothing; the count is `R` 1, heartbeat 29, mount 10, gc_ack_floor 2, event_log 1,
claim_conflicts 1. `CASHeartbeat.RenewalOverConnectFailuresRecoversWithoutASettleRead` exists only in
`USE_AWS_S3` builds; `build/` has S3.

- [ ] **Step 12: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h \
  src/Disks/tests/gtest_cas_pool.cpp \
  src/Disks/tests/gtest_cas_mount_runtime.cpp \
  src/Disks/tests/gtest_cas_heartbeat.cpp \
  src/Disks/tests/gtest_cas_mount.cpp \
  src/Disks/tests/gtest_cas_event_log.cpp \
  src/Disks/tests/gtest_cas_gc_ack_floor.cpp \
  src/Disks/tests/gtest_cas_mount_claim_conflicts.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Run every CAS mount renewal on the lease plane without a lease bound" \
  -m "The only renewal is the lease thread's, so MountRenewPolicy and the fence-gated renewal plane in MountLeaseRenewer and CasMountRuntime go. MountLeaseRenewer takes the claim-and-farewell plane and the lease plane, and renew retries with Retry::untilDefinitive. Renewer tests that stopped after one attempt at the lease bound now end through their liveness." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

#### Cycle 4: the unreachable counter and classifications, and their docs

Every name, grepped over `src`, `tests`, `docs` (without `docs/superpowers`, which records history) and
`utils` at `7e243502408`:

| Name | Hit | What happens |
|---|---|---|
| `CASMountRenewalDeadlineExceeded` | `src/Common/ProfileEvents.cpp:959` | deleted |
| | `R:33` extern, `R:574-576` increment in `consumeRenewResult` | deleted |
| | `T/gtest_cas_observability.cpp:27`, `:105`, `:122`, the helper's parameter, `:172` | deleted with the field; `ExternalLeaseDeadlineCounts...` went in cycle 1 |
| | `tests/integration/test_cas_mount_renewal_retry/test.py:28`, `:460`, `:587` | deleted |
| | `docs/en/antalya/cas/operations/monitoring.md:62`, `:80`; `troubleshooting.md:53` | deleted |
| `external_lease_deadline` / `ExternalLeaseDeadline` | `S:111`, `S:336`, `S:1726` | deleted |
| | `T/gtest_cas_event_log.cpp:322`, `:493` | gone in cycle 1 |
| | `T/gtest_cas_heartbeat.cpp:1073`, `:1989` | gone in cycles 2 and 3 |
| | `T/gtest_cas_pool.cpp:4408` (`ParkedRedoFailureObservabilityPrecedesRemountResult`) | gone in Task 2 |
| | `troubleshooting.md:52`, `debugging.md:136`, `:150` | deleted |
| `request_deadline` / `RequestDeadline` | `S:112`, `S:337`, `S:1727` | deleted |
| | `troubleshooting.md:55`, `debugging.md:138`, `:150` | deleted |
| `MountRenewResult::deadline_source` | `SH:57-60`, `S:1704-1705` | deleted |
| | `R:575` | deleted with the increment |
| | `T/gtest_cas_heartbeat.cpp:1075-1076`, `:1990-1991` | gone in cycles 2 and 3 |
| `GaveUp::deadline_source` (engine) | `Backend/`, `gtest_cas_requests.cpp`, `gtest_cas_hot_keys.cpp` | untouched: a different field |

Command (must print only the untouched engine field afterwards):

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && git grep -n -E "CASMountRenewalDeadlineExceeded|external_lease_deadline|request_deadline|ExternalLeaseDeadline|RequestDeadline|deadline_source" -- src tests docs utils ':!docs/superpowers'
```

- [ ] **Step 13: Implement**

`$W/src/Common/ProfileEvents.cpp`: delete the line `M(CASMountRenewalDeadlineExceeded, ...)` (:959).

`$W/R`: delete `extern const Event CASMountRenewalDeadlineExceeded;` (:33) and, in `consumeRenewResult`,

```cpp
    if (result.outcome == MountRenewOutcome::Terminal
        && result.deadline_source == GaveUp::Source::Lease)
        ProfileEvents::incrementNoTrace(ProfileEvents::CASMountRenewalDeadlineExceeded);
```

`$W/SH`, `struct MountRenewResult`: delete `deadline_source` with its two comment lines (:57-60).

`$W/S`, `enum class MountRenewTerminalClassification` (:103-114): delete `ExternalLeaseDeadline,` and
`RequestDeadline,`. `terminalClassificationName` (:328-342): delete their two `case` lines.
`MountLeaseRenewer::renew`, the `GaveUp` branch: delete

```cpp
        if (gave_up->why == GaveUp::Why::Deadline)
            result.deadline_source = gave_up->deadline_source;
```

and the `switch` becomes:

```cpp
        MountRenewTerminalClassification classification = MountRenewTerminalClassification::Unresolved;
        switch (gave_up->why)
        {
            case GaveUp::Why::FenceLost:
                classification = cancelled
                    ? MountRenewTerminalClassification::Cancelled
                    : MountRenewTerminalClassification::FenceOrLifecycleLost;
                break;
            /// Reachable only at the end of the clock's range: the policy has no lease bound and no window.
            case GaveUp::Why::Deadline:
            case GaveUp::Why::Unresolved:
                classification = MountRenewTerminalClassification::Unresolved;
                break;
        }
```

`$W/T/gtest_cas_observability.cpp`: delete `extern const Event CASMountRenewalDeadlineExceeded;` (:27); the
helpers become:

```cpp
struct RenewalCounterSnapshot
{
    uint64_t attempts;
    uint64_t retries;
    uint64_t resolved;
    uint64_t recovered;
};

RenewalCounterSnapshot renewalCounters()
{
    using ProfileEvents::global_counters;
    return {
        .attempts = global_counters[ProfileEvents::CASMountRenewalAttempts].load(),
        .retries = global_counters[ProfileEvents::CASMountRenewalRetries].load(),
        .resolved = global_counters[ProfileEvents::CASMountRenewalResolved].load(),
        .recovered = global_counters[ProfileEvents::CASMountRenewalRecovered].load(),
    };
}

void expectRenewalCounterDelta(
    const RenewalCounterSnapshot & before,
    const RenewalCounterSnapshot & after,
    uint64_t attempts,
    uint64_t retries,
    uint64_t resolved,
    uint64_t recovered)
{
    EXPECT_EQ(after.attempts - before.attempts, attempts);
    EXPECT_EQ(after.retries - before.retries, retries);
    EXPECT_EQ(after.resolved - before.resolved, resolved);
    EXPECT_EQ(after.recovered - before.recovered, recovered);
}
```

and in `RenewalCountersHaveExactPhysicalAndLogicalDeltas`:
`expectRenewalCounterDelta(before, after, attempts, retries, resolved, recovered);`.

`$W/tests/integration/test_cas_mount_renewal_retry/test.py`: delete `"CASMountRenewalDeadlineExceeded",` from
`RENEWAL_EVENTS` (:28) and both `assert delta["CASMountRenewalDeadlineExceeded"] == 0, delta` lines (:460,
:587).

`$W/docs/en/antalya/cas/operations/monitoring.md`, section `{#mount-renewal-remount-counters}`: delete the
`CASMountRenewalDeadlineExceeded` row (:62); `` `CASMountLeaseLost` complements those nine counters. ``
becomes `` `CASMountLeaseLost` complements those counters. ``; the second line of the `IN` list becomes

```sql
    'CASMountRenewalRecovered', 'CASMountLeaseLost',
```

`$W/docs/en/antalya/cas/operations/troubleshooting.md`:

- the first row of the symptom table (:19), last cell:

```markdown
| Read the failed renewal's `classification` before changing anything — it says why (see [the decision flow](#mount-renewal-remount-flow)); see [the mount lease](/antalya/cas/architecture/mounts-and-leases#mount-lease) |
```

- in `{#mount-renewal-remount-flow}` delete item 2, "**External lease-safety exhaustion.**" (:50-57), and
  renumber the items after it: `3.` → `2.`, `4.` → `3.`, `5.` → `4.`, `6.` → `5.`. Nothing in `docs/en`
  refers to these items by number (`grep -rn "item [0-9]\|case [0-9]" docs/en/antalya`).

`$W/docs/en/antalya/cas/operations/debugging.md`, the `outcome = 'failed'` bullet (:136-151). Replace

```markdown
- `outcome = 'failed'` carries the decisive `classification`: `external_lease_deadline` (the
  confirmed lease's own safety margin, not the request policy, ran out first — check object-store
  latency or `BOOTTIME` advancement before anything else), `request_deadline` (the ninety-second
  request policy exhausted first), `unresolved` (every attempt was ambiguous and never settled by
```

with

```markdown
- `outcome = 'failed'` carries the decisive `classification`: `unresolved` (every attempt was ambiguous and never settled by
```

and

```markdown
  condition. Do not collapse these into a generic timeout — the action
  differs by classification, and only `external_lease_deadline` and `request_deadline` are about a
  deadline at all.
```

with

```markdown
  condition. Do not collapse these into a generic timeout — the action
  differs by classification.
```

`$W/docs/en/antalya/cas/architecture/mounts-and-leases.md`, if Task 2 left them (they are false since
cycle 3):

- in the "Lease deadline" bullet, delete the sentence "The renewals at startup, after a remount and the
  direct renewal stay bounded: they stop at the last confirmed deadline minus the safety margin." (:91-93);
- after "doubt about the source fails closed." (:141-143), replace the two sentences that follow with:
  "Fenced mutations retry transport ambiguity only inside authority already proved by the last confirmed
  lease. The renewal may keep retrying after that lease has expired; write authority stays bounded by the
  start of the renewal that last succeeded plus the TTL."

Parking, which Task 1 removed, in the operations pages. After Task 1 one lease thread renews and
reclaims; after Task 2 a renewal ends early on a stop, a pending remount request or a terminal lifecycle
(including a published FORGET intent), not on a lost fence (contract 2.2, `renewalLive`). A stop sampled
before the write gives `cancelled`; a pending request or a terminal lifecycle gives
`fence_or_lifecycle_lost`. A terminal renewal trips the fence (`tripMountLost` counts the loss once per
`Live -> TransientNotLive`) and raises a generation only if none is pending (contract 2.1, consume step).
Hits at `7e243502408` (`grep -rn -i -E "parked|park request|remount worker" docs/en/antalya/cas/operations/`):

- `monitoring.md:70-71`, in `{#mount-renewal-remount-counters}`: replace
  "A parked terminal result and shutdown do not duplicate the count." with
  "A renewal ended by a pending remount request does not duplicate the count, and neither does shutdown."
- `troubleshooting.md:66-68`, item "**Fence or lifecycle loss.**" (item 4 after the renumbering): replace
  its first two sentences with
  "`classification = 'fence_or_lifecycle_lost'` means a pending remount request or a terminal lifecycle,
  including a published FORGET intent, ended the renewal. A renewal ended by a pending request raises no
  second recovery generation and does not count `CASMountLeaseLost` again."
  The sentence on `unresolved` stays.
- `debugging.md:141-144`, in the `outcome = 'failed'` bullet: "`cancelled` (a renewal in flight was
  cancelled by shutdown or a remount park request; expected during graceful shutdown)" becomes
  "`cancelled` (a renewal in flight was cancelled by shutdown; expected during graceful shutdown)", and
  "`fence_or_lifecycle_lost` (another local fence loss or a terminal lifecycle transition closed admission
  while the operation was active)" becomes "`fence_or_lifecycle_lost` (a pending remount request or a
  terminal lifecycle, including a published FORGET intent, ended the renewal)". A lost fence alone no
  longer ends one.

If Task 1 already rewrote any of these sentences, check its text against the two facts above instead.

Then `grep -rn -i -E "bounded renewal|direct renewal|startup, remount|parked|park request|remount worker" docs/en/antalya/cas/` prints nothing. At
`7e243502408` the grep also hits `mounts-and-leases.md:263-272` ("renewal and remount workers",
`ParkRequested`, `Parked`). Task 1 rewrites that passage. If a hit remains there, report it rather than
rewriting Task 1's text here.

- [ ] **Step 14: Build, run, gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/p2t3_c4_build.log 2>&1; echo NINJA_EXIT=$? >> build/p2t3_c4_build.log
tail -1 build/p2t3_c4_build.log; grep -a -n "error:" build/p2t3_c4_build.log | head -40
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASObservability.*:CASEvent.*:CASHeartbeat.*:CASMountAudit.*' > build/p2t3_c4_tests.log 2>&1; echo GATE_EXIT=$? >> build/p2t3_c4_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/p2t3_c4_tests.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CAS*' > build/p2t3_c4_gate.log 2>&1; echo GATE_EXIT=$? >> build/p2t3_c4_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/p2t3_c4_gate.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && git grep -n -E "CASMountRenewalDeadlineExceeded|external_lease_deadline|request_deadline|ExternalLeaseDeadline|RequestDeadline|deadline_source" -- src tests docs utils ':!docs/superpowers'
python3 -m py_compile tests/integration/test_cas_mount_renewal_retry/test.py && echo PY_OK
```

Expected: `NINJA_EXIT=0`; both runs `GATE_EXIT=0`, no `FAILED`; the gate count equals cycle 3's; the grep
prints only `GaveUp::deadline_source` lines under `Backend/` and in `gtest_cas_requests.cpp` and
`gtest_cas_hot_keys.cpp`; `PY_OK`. The integration module is not part of the gate; its change deletes a
name and two asserts and needs no run here.

- [ ] **Step 15: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Common/ProfileEvents.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp \
  src/Disks/tests/gtest_cas_observability.cpp \
  tests/integration/test_cas_mount_renewal_retry/test.py \
  docs/en/antalya/cas/operations/monitoring.md \
  docs/en/antalya/cas/operations/troubleshooting.md \
  docs/en/antalya/cas/operations/debugging.md \
  docs/en/antalya/cas/architecture/mounts-and-leases.md
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Remove the CAS mount renewal deadline counter and classifications" \
  -m "No renewal is bounded by the lease or by a policy window, so CASMountRenewalDeadlineExceeded, the external_lease_deadline and request_deadline classifications and MountRenewResult::deadline_source cannot occur. A counter that cannot move reads as healthy, so they go, with their rows in the operations docs." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

Omit `mounts-and-leases.md` from `add` if Task 2 had already removed both passages.

- [ ] **Step 16: Re-derive the enumeration on the task's head**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
git grep -n -E 'RenewCaller|renewRenewer|renewForRemount|renewOn\b|MountRenewPolicy|LeaseBound|worker_requests' -- src/
grep -rn 'renewWatermarkOnce' src/Disks/tests | wc -l
grep -rn -B10 -A6 'renewWatermarkOnce' src/Disks/tests | grep -E 'EXPECT_THROW|expectThrows|try$|catch'
stat -c '%Y %n' build/src/unit_tests_dbms src/Disks/tests/gtest_cas_heartbeat.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp
```

Expected: the first grep prints nothing; the count is 218 plus whatever Tasks 1 and 2 added (221 less
the three deleted call lines); every throwing site is a definitive answer from the tables above; the
binary is newer than both sources.

**Deviations from the contract**

- Settled by ruling: the `GaveUp::Why::Deadline` arm of the renewal's classification maps to
  `Unresolved`. The contract removes the two deadline values without naming that arm's result. With no
  lease bound and a `UINT64_MAX` window the arm is reachable only at the end of the clock's range; a
  terminal `Unresolved` fails closed (the fence trips and a remount is requested) and is true of such a
  renewal. A `LOGICAL_ERROR` would abort debug and sanitizer builds for a state the type still allows.
- Files beyond the contract's list: `gtest_cas_mount_runtime.cpp` (`RuntimeFixture` builds the runtime with
  the removed plane), `CasRefLedger.h` (a comment that claims renewals share the ref ledger's fence),
  `tests/integration/test_cas_mount_renewal_retry/test.py` (the removed counter, O2),
  `docs/en/antalya/cas/architecture/mounts-and-leases.md` and the first row of `troubleshooting.md` (they
  describe the bounded renewals).
- Tests adapted beyond the contract's list, because they stop after one attempt only at the lease bound:
  `CASHeartbeat.RenewerStateAllowsOnlyActiveReleaseOrTerminal` (block `"terminal"`) and both blocks of
  `CASHeartbeat.LateDeliveryAfterTerminalCannotRearmOrOverwriteSuccessor`. They take a liveness bound
  ("live until the resolving read has run"), and `Ops` loses its `sleep_step_ms` parameter.
  `CASMountLease.ClaimIsNotAdmittedUnderTheMountFence` keeps its refusing plane as the renewal plane.
  `RenewalScriptBackend` becomes `final`.
- `CASEvent.TerminalRenewalDetailsPreservePhysicalTruthAndClassification` replaces its deadline block with a
  vanished slot rather than dropping it, so a terminal row's `attempts_sent` stays asserted.
- Counts differ from the contract's section 4: 48 `MountLeaseRenewer` constructions (the contract's 45
  omit `R`'s `installRenewer` and the two in `gtest_cas_gc_ack_floor.cpp`); 15 `UnboundedRenewalFixture::renew`
  lines carry the policy and 20 `renewalEnvironment` calls need no change; the `renewWatermarkOnce`
  classification is the one in the table above, not "210 untouched outside `gtest_cas_pool.cpp`".
- The doc corrections land in cycle 4 together with O2, so cycles 1 to 3 leave the bounded-renewal sentences
  in `docs/en` stale for three commits.

**Review focus**

1. The seam has no request bound. A future test that arms a persistent transient fault on the mount slot
   and calls `renewWatermarkOnce` hangs until the 1200 s gtest timeout instead of failing; with a frozen
   boot clock and no `retry_sleep_fn` each retry also sleeps 800 to 1200 ms of real time. A reasonable
   person expects such a test to fail fast; today only the fixture discipline (definitive answers or
   `renewal_live_for_test`) prevents it.
2. A terminal seam renewal on a runtime with no lease thread raises a remount generation that nothing
   serves, unless Task 2's rule keeps a `background_watermark` term. Until a `tryRemountOnce`, every later
   seam renewal ends at once on the pending request. A reasonable person expects a second seam renewal after
   a failure to be refused, not silently skipped; no test makes that second call.
3. `consumeRenewResult` after the fold: the `Loop` rule now also consumes the seam's results, and a reviewer
   should check that no `Direct`-only behaviour a test relied on was dropped silently (the result-based
   attempt counting, the `background_watermark` term, the rethrow). `CASObservability.RenewalCountersHaveExactPhysicalAndLogicalDeltas`
   pins the counting; nothing pins the generation on the seam path.

---

### Task 4: The renewal returns its result (contract T6)

`MountLeaseRenewer::renew` returns everything its report needs: the identity of the body it wrote or
tried to write, why it ended, and how long it took. `renewOnce` builds the event and the log line from
that result, with no lock held. The thread-local observability stack, its deferred delivery and its
nesting go, and so does the renewer's copy of the lease deadline: the farewell receives the deadline as
an argument.

Paths below: `W` = `/home/mfilimonov/workspace/ClickHouse/cas-95-1`, `CA` =
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`, `S` = `CA/Pool/CasServerRoot.cpp`,
`SH` = `CA/Pool/CasServerRoot.h`, `R` = `CA/Pool/CasMountRuntime.cpp`, `P` = `CA/Pool/CasPool.cpp`.
Line numbers are from `W` at `7e243502408`, each with its function. Tasks 1 to 3 rewrite parts of these
functions; a hunk that touches such code quotes it "as left by Task N" and is located by the quoted
text, not by the number.

**Files:**
- Modify: `SH` (`MountRenewTerminalClassification` moved in before `struct MountRenewResult` ~:49; five fields in `MountRenewResult`; declaration of `reportMountRenewCompletion` after it; `MountLeaseRenewer::release` ~:588; `throwRenewConflict` ~:602; `terminate` ~:604; member `confirmed_deadline_boot_ms` ~:628 removed)
- Modify: `S` (forward declarations ~:49-52; the anonymous-namespace block from `enum class MountRenewTerminalClassification` ~:100 through `deliverMountRenewObservability` ~:420; `configureMountRenewObservability`, `reportMountRenewCompletion`, `deliverDeferredMountRenewObservability` ~:448-492; `MountLeaseRenewer::start` ~:1475-1482; `MountLeaseRenewer::throwRenewConflict` ~:1486; `MountLeaseRenewer::renew` (the body of `renewOn` ~:1592, as left by Task 3); `MountLeaseRenewer::terminate` ~:1751; `MountLeaseRenewer::release` ~:1824)
- Modify: `R` (forward declarations ~:39-41; `CasMountRuntime::renewOnce`, as left by Task 3; `CasMountRuntime::finishTeardown` ~:1218)
- Modify: `P` (forward declaration ~:73; the `SCOPE_EXIT` of `Pool::tryRemountOnce` ~:1264-1268)
- Modify: `src/Disks/tests/gtest_cas_heartbeat.cpp` (11 `release` lines), `src/Disks/tests/gtest_cas_mount.cpp` (2 `release` lines)
- Modify: `src/Disks/tests/gtest_cas_event_log.cpp` (file-scope declarations ~:28-33; delete `CASEvent.DeepReentrancyPreservesDeterministicPhysicalAttemptTruth` ~:325-414)
- Test: `src/Disks/tests/gtest_cas_heartbeat.cpp` (`CASHeartbeat.RenewReturnsWhatItsReportNeeds`, `CASHeartbeat.FarewellIsBoundByTheDeadlineItIsGiven`), `src/Disks/tests/gtest_cas_event_log.cpp` (`CASEvent.TheReportIsBuiltFromTheResult`)

`CA/Pool/CasMountRuntime.h` and `src/Disks/tests/gtest_cas_pool.cpp` are in the contract's file list for
T6 and need no change here: `renewOnce` keeps its signature, and the two `remount_attempt_no`
assertions of `gtest_cas_pool.cpp` left with their tests in Task 2.

**Interfaces:**
- Consumes (Tasks 1 to 3): `CasMountRuntime::renewOnce()` (renew, consume under `driver_mutex`, then report with no lock held); `consumeRenewResult(const MountRenewResult &)`; `MountLeaseRenewer::renew(const MountRenewOperationEnvironment &)` on the lease plane with `Retry::untilDefinitive`, its body being the former `renewOn`; the two-plane constructor `MountLeaseRenewer(CasRequests & open_requests_, CasRequests & lease_requests_, const Layout &, ...)`; `MountRenewTerminalClassification` without `ExternalLeaseDeadline` and `RequestDeadline`; `MountRenewResult` without `deadline_source`; the expiry `WARNING` that Task 1 moved out of `publishRenewedDeadline`, logged in `renewOnce` after the unlock.
- Produces:
  - `enum class MountRenewTerminalClassification : uint8_t { Unclassified, DeterministicFailure, Conflict, Vanished, Cancelled, FenceOrLifecycleLost, Unresolved }` in `SH`.
  - `MountRenewResult::{writer_epoch, seq, write_attempt_id, classification, elapsed_ms}`.
  - `void reportMountRenewCompletion(const MountRenewResult & result, const String & server_root_id, const CasEventSink & event_sink, uint64_t lease_deadline_before_boot_ms, std::optional<uint64_t> expired_ms) noexcept` in `SH`.
  - `void MountLeaseRenewer::release(uint64_t lease_deadline_boot_ms)`.
  - Private: `[[noreturn]] void throwRenewConflict(const Observation & seen, MountRenewTerminalClassification & classification) const`; `void terminate(CasOperation & op, uint64_t lease_deadline_boot_ms)`.

**The `watermark_renew` event and the log lines: where each value comes from.** The event and both
renewal log lines are written by `deliverMountRenewObservability` (S:344) and `emitMountRenewEvent`
(S:283) from a `MountRenewObservabilityContext` that `initializeMountRenewObservability` (S:238) filled
before the write and `reportMountRenewCompletion` (S:459) completed after it. After this task one function
builds them from the result and four values the caller passes.

| Item | Today | After this task |
|---|---|---|
| `type` | `CasEventType::WatermarkRenew` | unchanged |
| `outcome` | `recovered` when `Committed` and (`attempts_sent > 1` or `resolved_by_read` or `expired_ms` set); `failed` when `Terminal`; no event otherwise | the same rule over `result` and the `expired_ms` argument |
| `reason` | three fixed texts, picked by `outcome` and `expired_ms` (S:299-304) | the same three texts, same rule |
| `server_root_id` | `context.server_root_id`, configured by `renewRenewerOnce` as `&CasMountRuntime::server_root_id` | the `server_root_id` argument: `renewOnce` passes `CasMountRuntime::server_root_id` |
| `writer_epoch` | renewer's `writer_epoch`, copied at initialization | `result.writer_epoch`, set by `renew` from the renewer's `writer_epoch` |
| `seq` | `next_seq` of the renewal, copied at initialization | `result.seq` = `next_seq` |
| `write_attempt_id` | first 12 hex digits of the id minted for the body | first 12 hex digits of `result.write_attempt_id`, the same id |
| `attempts_sent` | `max(0, result.attempts_sent)` (S:471) | `result.attempts_sent` |
| `elapsed_ms` | `defaultBootMs()` at delivery minus `defaultBootMs()` at initialization: the real `CLOCK_BOOTTIME` both times | `result.elapsed_ms`: the renewal's boot clock at the return of `renew` minus `attempt_start_boot_ms`. In production the renewal's boot clock is `bootMsNow`, which is `CLOCK_BOOTTIME`, so the value differs only by the time of the consume step and of the lines between the start sample and the old initialization. Under an injected clock it is the injected time |
| `remaining_confirmed_budget_ms` | `(confirmed_deadline_boot_ms − attempt_start_boot_ms) − elapsed_ms`, each difference floored at 0; `confirmed_deadline_boot_ms` is the renewer's copy as the renewal started | the same arithmetic with `lease_deadline_before_boot_ms`: `mount_fence.deadline_boot_ms` as `renewOnce` read it before `renew`. Every production path that installs a renewer publishes the fence deadline from the same start plus TTL before the next renewal (`armIfAdmissible` arms or sets it, `publishRenewedDeadline` sets it after a commit), so the two values are equal there. They differ in tests that arm by hand or never arm (`UINT64_MAX`) |
| `classification` | `committed_by_read`, `committed_after_retry`, `committed_after_expiry` for `recovered`; `terminalClassificationName(context.terminal_classification)` for `failed`, set through the thread-local by `markMountRenewTermination` | the same names; `failed` reads `result.classification`, set by `renew` |
| `expired_ms` | `context.expired_ms`, the argument of the old `reportMountRenewCompletion` | the `expired_ms` argument; `renewOnce` passes the value Task 1 passes today |
| `remount_attempt_no` | set when a deferred report was drained by `tryRemountOnce`'s finalizer | removed (contract 2.3); its only producer, the remount re-anchor, left in Task 3 |
| `INFO` on `CasMountLeaseRenewer`: `CAS mount renewal '{}' recovered after {} physical attempts in {} ms (classification={}, confirmed_deadline_boot_ms={})` | `server_root_id`, `attempts_sent`, `elapsed_ms`, `classification` as in the event; the last value is the renewer's `confirmed_deadline_boot_ms` | same text; the last value is `lease_deadline_before_boot_ms` |
| `WARNING` on `CasMountLeaseRenewer`: `CAS mount renewal '{}' fenced after {} physical attempts in {} ms (classification={}, confirmed_deadline_boot_ms={})` | as above | as above |
| `WARNING` on `CasPool`: `CAS mount lease of '{}' was expired for {} ms; a renewal restored it and writes resume. Last failed renewal request: {}` | `server_root_id`, `expired_ms` and the swapped `last_renew_failure` of `publishRenewedDeadline`, logged after the unlock (Task 1) | not touched by this task |

What proves byte compatibility: the existing tests stay green without edits, except where this section
names an edit. They are `CASEvent.FirstAttemptRenewalIsSilent`, `WatermarkRenewEventsAreBoundedAndComplete`
(all eight detail keys, `seq`, `writer_epoch`, the landed `write_attempt_id`),
`WatermarkRenewSinkFailureCannotChangeOutcome`, `TerminalRenewalDetailsPreservePhysicalTruthAndClassification`
(as left by Task 3), `ReentrantRenewalSinkPreservesOuterObservationIdentity`,
`PreCompletionConflictReentrancyPreservesOuterTerminalObservation`, `CASMountAudit.RenewalDefaultLogsAreBounded`
(log line counts), `CASMountRuntime.ExpiryIsReported` (`committed_after_retry`, `committed_after_expiry`,
`expired_ms`, the expiry `WARNING`), `CASMountRuntime.MemoryLimitDoesNotEndTheLeaseThread` (the sink
allocates on the lease thread while the result is consumed) and `CASMountRemount.CutoffFencesWithoutRenewals`
(`conflict`). The integration module `test_cas_mount_renewal_retry` greps `CasMountLeaseRenewer` for
`fenced after`; the new `CASEvent.TheReportIsBuiltFromTheResult` pins both renewal log texts verbatim.

**The classification of a conflict.** `throwRenewConflict` (S:1486) classifies a refused precondition
through the thread-local today, at the top of each of its three arms, and `emitMountEvent` inside it
calls the event sink while the renewal is still running. It has one caller: the `Conflict` branch of
`renew` (S:1680). It gets an out-parameter, `MountRenewTerminalClassification & classification`, set
where `markMountRenewTermination` stands today, and the caller passes `result.classification`. A
classifying catch was rejected because the exception does not carry the arm:
- the `Object` arm sets `Conflict` before `decodeMountLease`, which throws `CORRUPTED_DATA` for an
  unreadable occupant; a catch would have to classify that code as a conflict too;
- `MountFencedException`, the same-epoch and superseded arms and the foreign arm all throw `ABORTED`,
  while `Unresolved` throws the same retry-later exception as the `GaveUp` path.

With the out-parameter the classification lives in `renew`'s local `result` before the sink runs, so a
sink called from `emitMountEvent` cannot reach it.

**The two reentrancy tests.** Both are kept unchanged; both pass on a by-value result.
- `CASEvent.ReentrantRenewalSinkPreservesOuterObservationIdentity`: the pool has no lease thread, so the
  sink's nested `renewWatermarkOnce` runs from inside `reportMountRenewCompletion` on the test thread. It
  passes because the event is built from the outer result before the sink runs, and `renewOnce` reports
  with no lock held. After this task it guards the second fact only: a report moved under
  `driver_mutex` would make the nested renewal block on the mutex its own thread holds, which shows as a
  hang (`GATE_EXIT=124`), not a failure.
- `CASEvent.PreCompletionConflictReentrancyPreservesOuterTerminalObservation`: the outer renewal's
  `MountConflict` sink renews a different pool while the outer renewal is still inside
  `throwRenewConflict`. It passes because the outer classification is a local of the outer `renew`.
  It fails if the classification moves back into state shared by the thread.

**The `release` call lines.** Derived with
`grep -rnE "\b(renewer|k|departing)\.release\(\)" src/Disks/tests` in `W`: 13 lines, 11 in
`gtest_cas_heartbeat.cpp` and 2 in `gtest_cas_mount.cpp` (a check by type: the other `MountLeaseRenewer`
locals in `src/Disks/tests`, `renewing`, `srid2_renewer`, `renewers[]` and the one `makeRenewer` returns,
are never released). Production has one more, `mount_renewer->release()` in `finishTeardown` (R:1228).
The compiler is the completeness gate: `release` loses its zero-argument form, so a missed line does
not build. Pattern: `X.release()` becomes `X.release(X.lastCommittedAttemptStartBootMs() + <TTL X was
constructed with>)`, which is the value of `confirmed_deadline_boot_ms` at that point; no test is near
the overflow the old saturating sum guarded.

| # | File, test | TTL | After |
|---|---|---|---|
| 1 | heartbeat `StopStampsExpiredAndFarewellSentinel` | 100 | `renewer.release(renewer.lastCommittedAttemptStartBootMs() + 100);` |
| 2 | heartbeat `FarewellIsAdmittedUnderTheDefaultBudget` | 30000 | `EXPECT_NO_THROW(renewer.release(renewer.lastCommittedAttemptStartBootMs() + 30000))` |
| 3 | heartbeat `FarewellIsAdmittedUnderADifferentEnvelope` | 40000 | `EXPECT_NO_THROW(renewer.release(renewer.lastCommittedAttemptStartBootMs() + 40000))` |
| 4 | heartbeat `FarewellIsRefusedWhenTheLeaseExpiresBeforeItsDerivedWindow` | 5000 | `renewer.release(renewer.lastCommittedAttemptStartBootMs() + 5000);` |
| 5 | heartbeat `ForeignIncarnationDuringFarewellLeavesTheSuccessorUntouchedAndReportsTheConflict` | 100 | `renewer.release(renewer.lastCommittedAttemptStartBootMs() + 100);` |
| 6 | heartbeat `CASMountAudit.RenewerAdoptEmitsClaimAndTerminateEmitsRelease` | 100 | `renewer.release(renewer.lastCommittedAttemptStartBootMs() + 100);` |
| 7-9 | heartbeat `RenewerStateAllowsOnlyActiveReleaseOrTerminal`, block `"released"`: two `EXPECT_RENEWER_STATE_REJECTION(renewer.release());` and one `renewer.release();` | 1000 | `renewer.release(renewer.lastCommittedAttemptStartBootMs() + 1000)` inside the same statement |
| 10 | same test, block `"terminal"`: `EXPECT_RENEWER_STATE_REJECTION(renewer.release());` | 1000 | same argument |
| 11 | heartbeat `CancellationBeforeSendIsNotAttemptedAndAllowsRelease` | 1000 | `EXPECT_NO_THROW(renewer.release(renewer.lastCommittedAttemptStartBootMs() + 1000));` |
| 12 | mount `CASMountLease.TerminateAfterVanishedBackingStoreIsNoOpRelease` | 100 | `EXPECT_NO_THROW(k.release(k.lastCommittedAttemptStartBootMs() + 100))` |
| 13 | mount `FarewellRunsOnAnOpenFenceAfterTheMountFenceIsLost` (as left by Task 3) | 1000 | `EXPECT_NO_THROW(departing.release(departing.lastCommittedAttemptStartBootMs() + 1000));` |

The rejection lines (7, 9, 10) keep their `#if defined(DEBUG_OR_SANITIZER_BUILD)` split: the argument
is evaluated before the state check, and the check still raises the `LOGICAL_ERROR` the macro expects.

**Tests.**

| Test | Kind | Fails before this task because |
|---|---|---|
| `CASHeartbeat.RenewReturnsWhatItsReportNeeds` | new | the five fields and `MountRenewTerminalClassification` in `SH` do not exist (compile failure) |
| `CASEvent.TheReportIsBuiltFromTheResult` | new | the five-parameter `reportMountRenewCompletion` does not exist; the old one ignores a renewal nobody configured (compile failure) |
| `CASHeartbeat.FarewellIsBoundByTheDeadlineItIsGiven` | new | `release` takes no argument (compile failure) |
| `CASEvent.DeepReentrancyPreservesDeterministicPhysicalAttemptTruth` | deleted | its subject, the eight-slot stack, is gone; it calls `configureMountRenewObservability`. The attempt count it also checked rides `MountRenewResult::attempts_sent`, which `CASHeartbeat.RenewalRetriesOneImmutableBodyAndAdoptsLostResponse` and `CASEvent.TheReportIsBuiltFromTheResult` assert |
| the 13 `release` lines | adapted | pass the deadline; every assertion stays |
| `CASEvent.ReentrantRenewalSinkPreservesOuterObservationIdentity`, `PreCompletionConflictReentrancyPreservesOuterTerminalObservation` | kept unchanged | see above |

Gate counts: cycle 1 adds one test to the count Task 3 left, cycle 2 adds one and deletes one, cycle 3
adds one.

---

- [ ] **Step 1: Write the failing test for the result fields**

`$W/src/Disks/tests/gtest_cas_heartbeat.cpp`. First, a fixture in a new anonymous namespace placed
immediately before `TEST(CASHeartbeat, GcFenceAndVanishedMountStayTerminal)` (~:1222). It uses `Ops`,
`seedOwnClaim` and `RenewalScriptBackend` from the top of the file, and the two-plane constructor as left
by Task 3:

```cpp
namespace
{
/// One renewer over a scripted store, started on its own seeded claim, for the cases of
/// `RenewReturnsWhatItsReportNeeds`. The clocks come first: hooks stored in `backend` capture them.
struct ReportFieldsCase
{
    explicit ReportFieldsCase(String srid_)
        : srid(std::move(srid_))
    {
        seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, wall_ms, /*ttl_ms=*/1000);
        renewer.start();
        backend->attempts.clear();
    }

    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    String srid;
    UInt128 uuid{1};
    Layout layout{"pool"};
    std::shared_ptr<RenewalScriptBackend> backend = std::make_shared<RenewalScriptBackend>();
    Ops ops{backend, &boot_ms};
    MountLeaseRenewer renewer{
        ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9, std::chrono::milliseconds(1000),
        [this] { return wall_ms; }, [] { return uint64_t{0}; }, CasEventSink{}, std::chrono::milliseconds(20),
        [this] { return boot_ms; }};
};
}
```

Then the test, immediately after `TEST(CASHeartbeat, GcFenceAndVanishedMountStayTerminal)`:

```cpp
/// A renewal returns what its report names: the body it wrote or tried to write, why it ended, and
/// how long it took on its own boot clock.
TEST(CASHeartbeat, RenewReturnsWhatItsReportNeeds)
{
    {
        ReportFieldsCase c("commit");
        c.backend->on_attempt = [&boot_ms = c.boot_ms] { boot_ms += 250; };
        const MountRenewResult result = c.renewer.renew(renewalEnvironment(c.boot_ms));
        ASSERT_EQ(result.outcome, MountRenewOutcome::Committed);
        ASSERT_EQ(c.backend->attempts.size(), 1u);
        const MountLease sent = decodeMountLease(c.backend->attempts.back().bytes);
        EXPECT_EQ(result.writer_epoch, 9u);
        EXPECT_EQ(result.seq, 2u);
        EXPECT_EQ(sent.seq, 2u);
        EXPECT_NE(result.write_attempt_id, UInt128{});
        EXPECT_EQ(result.write_attempt_id, sent.write_attempt_id);
        EXPECT_EQ(result.classification, MountRenewTerminalClassification::Unclassified);
        EXPECT_EQ(result.attempt_start_boot_ms, 100u);
        EXPECT_EQ(result.elapsed_ms, 250u);
    }

    {
        ReportFieldsCase c("conflict");
        const String key = c.layout.mountKey(c.srid);
        const auto got = c.ops.op.read(key, Retry::standard());
        ASSERT_TRUE(got.has_value());
        MountLease foreign = decodeMountLease(got->bytes);
        foreign.server_uuid = UInt128{2};
        foreign.seq = 40;
        foreign.write_attempt_id = UInt128{0xF0F0};
        mustCommit(c.ops.op.replace(key, encodeMountLease(foreign), got->etag, Retry::standard()), "foreign slot");
        c.backend->attempts.clear();
        c.backend->on_attempt = [&boot_ms = c.boot_ms] { boot_ms += 250; };
        const MountRenewResult result = c.renewer.renew(renewalEnvironment(c.boot_ms));
        ASSERT_EQ(result.outcome, MountRenewOutcome::Terminal);
        ASSERT_EQ(c.backend->attempts.size(), 1u);
        const MountLease sent = decodeMountLease(c.backend->attempts.back().bytes);
        EXPECT_EQ(result.classification, MountRenewTerminalClassification::Conflict);
        EXPECT_EQ(result.writer_epoch, 9u);
        EXPECT_EQ(result.seq, 2u) << "the seq this renewal tried to write, not the occupant's";
        EXPECT_EQ(result.write_attempt_id, sent.write_attempt_id);
        EXPECT_EQ(result.elapsed_ms, 250u);
    }

    {
        ReportFieldsCase c("vanished");
        const String key = c.layout.mountKey(c.srid);
        const auto got = c.ops.op.read(key, Retry::standard());
        ASSERT_TRUE(got.has_value());
        ASSERT_EQ(c.ops.op.remove(key, got->etag, Retry::standard()), Removal::Removed);
        const MountRenewResult result = c.renewer.renew(renewalEnvironment(c.boot_ms));
        ASSERT_EQ(result.outcome, MountRenewOutcome::Terminal);
        EXPECT_EQ(result.classification, MountRenewTerminalClassification::Vanished);
        EXPECT_EQ(result.seq, 2u);
        EXPECT_NE(result.write_attempt_id, UInt128{});
    }

    {
        /// Ended before its first request by a liveness that refuses with no stop requested.
        ReportFieldsCase c("refused");
        const MountRenewResult result = c.renewer.renew(renewalEnvironment(
            c.boot_ms, /*live=*/[] { return false; }, /*cancelled=*/[] { return false; }));
        ASSERT_EQ(result.outcome, MountRenewOutcome::Terminal);
        EXPECT_TRUE(c.backend->attempts.empty());
        EXPECT_EQ(result.classification, MountRenewTerminalClassification::FenceOrLifecycleLost);
        EXPECT_EQ(result.writer_epoch, 9u);
        EXPECT_EQ(result.seq, 2u);
        EXPECT_NE(result.write_attempt_id, UInt128{}) << "the id is minted before the renewal is admitted";
        EXPECT_EQ(result.elapsed_ms, 0u);
    }

    {
        /// Ended before its first request by a stop.
        ReportFieldsCase c("stopped");
        const MountRenewResult result = c.renewer.renew(renewalEnvironment(
            c.boot_ms, /*live=*/[] { return false; }, /*cancelled=*/[] { return true; }));
        ASSERT_EQ(result.outcome, MountRenewOutcome::NotAttempted);
        EXPECT_EQ(result.classification, MountRenewTerminalClassification::Cancelled);
        EXPECT_EQ(result.seq, 2u);
    }
}
```

- [ ] **Step 2: Build and run it to see it fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/p2t4_c1_red.log 2>&1; echo NINJA_EXIT=$? >> build/p2t4_c1_red.log
tail -1 build/p2t4_c1_red.log; grep -a -n "error:" build/p2t4_c1_red.log | head -40
```

Expected: `NINJA_EXIT=1`; errors in `gtest_cas_heartbeat.cpp` only: `no member named 'writer_epoch' in
'DB::Cas::MountRenewResult'` (and `seq`, `write_attempt_id`, `classification`, `elapsed_ms`), and
`use of undeclared identifier 'MountRenewTerminalClassification'`: the enum lives in an anonymous
namespace of `S`. Record the output.

- [ ] **Step 3: Implement the result fields (the stack stays)**

`$W/SH`: insert before `struct MountRenewResult` (~:49), and replace the struct as left by Task 3 with
the one below. Task 3 removed `deadline_source`; the struct keeps its other members and their order.

```cpp
/// Why a renewal ended without retaining the lease, in the words of its audit event. `renew` sets it in
/// the arm of the write's verdict that ended the renewal.
enum class MountRenewTerminalClassification : uint8_t
{
    Unclassified,
    DeterministicFailure,
    Conflict,
    Vanished,
    Cancelled,
    FenceOrLifecycleLost,
    Unresolved,
};

struct MountRenewResult
{
    MountRenewOutcome outcome = MountRenewOutcome::Terminal;
    uint64_t attempt_start_boot_ms = 0;
    /// Physical attempts this renewal sent, for every terminal ending the engine can count: a commit,
    /// a give-up, a conflict, or a store refusal.
    uint32_t attempts_sent = 0;
    bool resolved_by_read = false;
    bool sent_any = false;
    std::exception_ptr failure;
    /// The body this renewal wrote or tried to write.
    uint64_t writer_epoch = 0;
    uint64_t seq = 0;
    UInt128 write_attempt_id{};
    /// `Unclassified` for a committed renewal.
    MountRenewTerminalClassification classification = MountRenewTerminalClassification::Unclassified;
    /// From `attempt_start_boot_ms` to the return of `renew`, on the renewal's boot clock.
    uint64_t elapsed_ms = 0;
};
```

Same file, `class MountLeaseRenewer`, private section, replace
`[[noreturn]] void throwRenewConflict(const Observation & seen) const;` (~:602) with:

```cpp
    /// Sets `classification` for the arm it takes, then throws what ended the renewal. The caller
    /// holds the classification before the event sink runs, so the sink cannot change it.
    [[noreturn]] void throwRenewConflict(const Observation & seen, MountRenewTerminalClassification & classification) const;
```

`$W/S`, anonymous namespace: delete the definition of `enum class MountRenewTerminalClassification`
with its comment (S:100-114, as left by Task 3: seven values). Delete `markMountRenewTermination`
(S:172-176); after this step it has no caller.

Same file, `reportMountRenewCompletion` (the two-parameter one, S:459). The stack still delivers, so it
takes the classification from the result; insert one line after `context->completed = true;`:

```cpp
    context->completed = true;
    context->terminal_classification = result.classification;
    context->outcome = result.outcome;
```

`MountLeaseRenewer::throwRenewConflict` (S:1486): new signature, and each of its three
`markMountRenewTermination(X);` lines becomes `classification = X;` in the same place:

```cpp
[[noreturn]] void MountLeaseRenewer::throwRenewConflict(
    const Observation & seen, MountRenewTerminalClassification & classification) const
{
    if (const Object * occupant = std::get_if<Object>(&seen))
    {
        classification = MountRenewTerminalClassification::Conflict;
        const MountLease current = decodeMountLease(occupant->bytes);
```

```cpp
    if (std::holds_alternative<ProvenAbsent>(seen))
    {
        classification = MountRenewTerminalClassification::Vanished;
        emitMountEvent(
```

```cpp
    /// The precondition was refused but nothing identifiable was read back: neither the successor nor
    /// an absence is established, so the only honest verdict is that this renewal settled nothing.
    classification = MountRenewTerminalClassification::Unresolved;
    throwCasWriteRetryLater(fmt::format(
```

`MountLeaseRenewer::renew` as left by Task 3 (the former body of `renewOn`). Seven hunks.

(1) After `result.attempt_start_boot_ms = attempt_start_boot_ms;`:

```cpp
    MountRenewResult result;
    result.attempt_start_boot_ms = attempt_start_boot_ms;
    result.writer_epoch = writer_epoch;
    result.seq = next_seq;
    result.write_attempt_id = write_attempt_id;
    const auto finished = [&boot_clock, attempt_start_boot_ms](MountRenewResult done)
    {
        done.elapsed_ms = elapsedSince(attempt_start_boot_ms, boot_clock());
        return done;
    };
```

(2) The `catch (...)` around `op.replace`:

```cpp
    catch (...)
    {
        /// The engine surfaces a deterministic local failure unchanged rather than reissuing it.
        result.classification = MountRenewTerminalClassification::DeterministicFailure;
        result.failure = std::current_exception();
        return finished(terminalResult(std::move(result)));
    }
```

(3) The `Committed` branch: its `return result;` becomes `return finished(std::move(result));`.

(4) The `Conflict` branch:

```cpp
    if (const Conflict * conflict = std::get_if<Conflict>(&*written))
    {
        result.sent_any = true;
        result.attempts_sent = conflict->attempts_sent;
        try
        {
            throwRenewConflict(conflict->seen, result.classification);
        }
        catch (...)
        {
            result.failure = std::current_exception();
        }
        return finished(terminalResult(std::move(result)));
    }
```

(5) The `Refused` branch: `markMountRenewTermination(MountRenewTerminalClassification::DeterministicFailure);`
becomes `result.classification = MountRenewTerminalClassification::DeterministicFailure;`, and
`return terminalResult(std::move(result));` becomes `return finished(terminalResult(std::move(result)));`.

(6) The `NotAttempted` early return in the `GaveUp` branch:

```cpp
        if (gave_up->why == GaveUp::Why::FenceLost && !gave_up->sent_any && cancelled)
        {
            result.classification = MountRenewTerminalClassification::Cancelled;
            result.outcome = MountRenewOutcome::NotAttempted;
            return finished(std::move(result));
        }
```

(7) The end of the `GaveUp` branch: `markMountRenewTermination(classification);` becomes
`result.classification = classification;`, and its `return terminalResult(std::move(result));` becomes
`return finished(terminalResult(std::move(result)));`. The `switch` that computes `classification`
stays as Task 3 left it.

`elapsedSince` (S:270) is defined above `renew` in the same file and stays. Check that every return of
`renew` goes through `finished` and no `markMountRenewTermination` is left:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && F=src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp
grep -n "markMountRenewTermination" $F
awk '/^MountRenewResult MountLeaseRenewer::renew\(/,/^}/' $F | grep -n "return "
```

Expected: no line from the first grep; from the second, six `return finished(` lines and nothing else.

- [ ] **Step 4: Build, run the tests of this cycle, then the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/p2t4_c1_build.log 2>&1; echo NINJA_EXIT=$? >> build/p2t4_c1_build.log
tail -1 build/p2t4_c1_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASHeartbeat.*:CASEvent.*:CASMountAudit.*' > build/p2t4_c1_tests.log 2>&1; echo GATE_EXIT=$? >> build/p2t4_c1_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/p2t4_c1_tests.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CAS*' > build/p2t4_c1_gate.log 2>&1; echo GATE_EXIT=$? >> build/p2t4_c1_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/p2t4_c1_gate.log | tail -20
```

Expected: `NINJA_EXIT=0`; both runs `GATE_EXIT=0`, no `FAILED`; the gate has one test more than Task 3
left. The event tests stay green because the stack now takes `terminal_classification` from the result.

Show the new test is load-bearing, one change at a time (rebuild, run
`--gtest_filter='CASHeartbeat.RenewReturnsWhatItsReportNeeds'`, see `FAILED`, restore, rebuild):
- set `result.seq = seq;` in hunk (1): the commit case fails with `result.seq` 1;
- in hunk (1), sample the elapsed time when `result` is built instead of in `finished`
  (`result.elapsed_ms = elapsedSince(attempt_start_boot_ms, boot_clock());` and `finished` returning
  `done` unchanged): the commit and conflict cases fail with `elapsed_ms` 0.

- [ ] **Step 5: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp \
  src/Disks/tests/gtest_cas_heartbeat.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Return the identity, ending and duration of a CAS mount renewal" \
  -m "MountLeaseRenewer::renew fills writer_epoch, seq, write_attempt_id, classification and elapsed_ms in MountRenewResult, so the report of a renewal can be built from its result. throwRenewConflict hands its classification back through an out-parameter instead of a thread-local." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

- [ ] **Step 6: Write the failing test for a report built from the result**

`$W/src/Disks/tests/gtest_cas_event_log.cpp`. Add includes after `#include <Common/Exception.h>`:

```cpp
#include <Common/Exception.h>
#include <Common/Logger.h>
#include <Common/logger_useful.h>
#include <Poco/AutoPtr.h>
#include <Poco/StreamChannel.h>
#include <sstream>
```

In the anonymous namespace that holds `watermarkRenewEvents` (~:158), after `watermarkRenewEvents`:

```cpp
/// Captures what the renewer's logger writes at `information` and above while it lives.
class ScopedRenewerLogCapture
{
public:
    ScopedRenewerLogCapture()
        : logger(getLogger("CasMountLeaseRenewer"))
        , channel(new Poco::StreamChannel(stream))
        , old_channel(logger->getChannel(), /*shared=*/true)
        , old_level(logger->getLevel())
    {
        logger->setChannel(channel.get());
        logger->setLevel("information");
    }

    ~ScopedRenewerLogCapture()
    {
        logger->setChannel(old_channel);
        logger->setLevel(old_level);
    }

    String captured() const { return stream.str(); }

private:
    LoggerPtr logger;
    std::ostringstream stream; // STYLE_CHECK_ALLOW_STD_STRING_STREAM
    Poco::AutoPtr<Poco::StreamChannel> channel;
    /// A real reference (shared=true), so the previous channel cannot die while ours is installed.
    Poco::AutoPtr<Poco::Channel> old_channel;
    int old_level;
};
```

The test, after `TEST(CASEvent, PreCompletionConflictReentrancyPreservesOuterTerminalObservation)`:

```cpp
/// A report needs nothing registered before the renewal: every field of the event and of the log line
/// comes from the result and from what the caller passes.
TEST(CASEvent, TheReportIsBuiltFromTheResult)
{
    /// Everything the plane, the renewer and the sink capture is declared before them.
    uint64_t wall_ms = 100;
    uint64_t boot_ms = 100;
    const String server_root_id = "reporting";
    std::vector<CasEvent> events;
    const CasEventSink sink = [&events](CasEvent event) { events.push_back(std::move(event)); };
    auto backend = std::make_shared<RenewalEventBackend>();
    const Layout layout("report-from-result");
    CasRequests plane(backend, Fence::open(), [&] { return boot_ms; }, [&](uint64_t ms) { boot_ms += ms; });
    MountLeaseRenewer renewer(
        plane, plane, layout, server_root_id, UInt128(0x77), 7, std::chrono::milliseconds(1000),
        [&] { return wall_ms; }, [] { return uint64_t{0}; }, CasEventSink{}, std::chrono::milliseconds(0),
        [&] { return boot_ms; });
    renewer.start();
    const ScopedRenewerLogCapture log;

    /// The first attempt's answer is lost and the reissue commits.
    backend->throw_before_next_write = true;
    const MountRenewResult retried = renewer.renew(MountRenewOperationEnvironment{});
    ASSERT_EQ(retried.outcome, MountRenewOutcome::Committed);
    ASSERT_EQ(retried.attempts_sent, 2u);
    const uint64_t retried_deadline = retried.attempt_start_boot_ms + retried.elapsed_ms + 777;
    reportMountRenewCompletion(retried, server_root_id, sink, retried_deadline, std::nullopt);

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, CasEventType::WatermarkRenew);
    EXPECT_EQ(events[0].outcome, "recovered");
    EXPECT_EQ(events[0].reason, "CAS mount renewal committed after a retry or a resolving read");
    EXPECT_EQ(events[0].detail.at("server_root_id"), server_root_id);
    EXPECT_EQ(events[0].detail.at("writer_epoch"), "7");
    EXPECT_EQ(events[0].detail.at("seq"), "2");
    EXPECT_EQ(events[0].detail.at("write_attempt_id"), u128ToHex(retried.write_attempt_id).substr(0, 12));
    EXPECT_EQ(events[0].detail.at("attempts_sent"), "2");
    EXPECT_EQ(events[0].detail.at("elapsed_ms"), std::to_string(retried.elapsed_ms));
    EXPECT_EQ(events[0].detail.at("remaining_confirmed_budget_ms"), "777");
    EXPECT_EQ(events[0].detail.at("classification"), "committed_after_retry");
    EXPECT_EQ(events[0].detail.size(), 8u) << "no expired_ms and no remount_attempt_no";
    EXPECT_NE(log.captured().find(fmt::format(
                  "CAS mount renewal '{}' recovered after 2 physical attempts in {} ms "
                  "(classification=committed_after_retry, confirmed_deadline_boot_ms={})",
                  server_root_id, retried.elapsed_ms, retried_deadline)),
              String::npos)
        << log.captured();

    /// A first-attempt commit that restored an expired lease.
    events.clear();
    const MountRenewResult restoring = renewer.renew(MountRenewOperationEnvironment{});
    ASSERT_EQ(restoring.outcome, MountRenewOutcome::Committed);
    ASSERT_EQ(restoring.attempts_sent, 1u);
    reportMountRenewCompletion(restoring, server_root_id, sink, restoring.attempt_start_boot_ms, 5);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].outcome, "recovered");
    EXPECT_EQ(events[0].reason, "CAS mount renewal restored a lease that had expired");
    EXPECT_EQ(events[0].detail.at("seq"), "3");
    EXPECT_EQ(events[0].detail.at("classification"), "committed_after_expiry");
    EXPECT_EQ(events[0].detail.at("expired_ms"), "5");
    EXPECT_EQ(events[0].detail.at("remaining_confirmed_budget_ms"), "0");

    /// A first-attempt commit and a renewal that never ran report nothing.
    events.clear();
    const MountRenewResult quiet = renewer.renew(MountRenewOperationEnvironment{});
    ASSERT_EQ(quiet.outcome, MountRenewOutcome::Committed);
    ASSERT_EQ(quiet.attempts_sent, 1u);
    reportMountRenewCompletion(quiet, server_root_id, sink, quiet.attempt_start_boot_ms + 1000, std::nullopt);
    const MountRenewResult skipped = renewer.renew(MountRenewOperationEnvironment{
        .live = [] { return false; },
        .cancelled = [] { return true; },
    });
    ASSERT_EQ(skipped.outcome, MountRenewOutcome::NotAttempted);
    reportMountRenewCompletion(skipped, server_root_id, sink, skipped.attempt_start_boot_ms + 1000, std::nullopt);
    EXPECT_TRUE(events.empty());

    /// A terminal renewal: the slot vanished under it.
    backend->vanish_on_next_write = true;
    const MountRenewResult vanished = renewer.renew(MountRenewOperationEnvironment{});
    ASSERT_EQ(vanished.outcome, MountRenewOutcome::Terminal);
    const uint64_t vanished_deadline = vanished.attempt_start_boot_ms + 1000;
    reportMountRenewCompletion(vanished, server_root_id, sink, vanished_deadline, std::nullopt);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].outcome, "failed");
    EXPECT_EQ(events[0].reason, "CAS mount renewal ended without retained authority and fenced the mount");
    EXPECT_EQ(events[0].detail.at("classification"), "vanished");
    EXPECT_EQ(events[0].detail.at("attempts_sent"), std::to_string(vanished.attempts_sent));
    EXPECT_NE(log.captured().find(fmt::format(
                  "CAS mount renewal '{}' fenced after {} physical attempts in {} ms "
                  "(classification=vanished, confirmed_deadline_boot_ms={})",
                  server_root_id, vanished.attempts_sent, vanished.elapsed_ms, vanished_deadline)),
              String::npos)
        << log.captured();
}
```

`MountRenewOperationEnvironment`'s designated initializers follow its member order as left by Task 3
(`boot_ms`, `live`, `cancelled`, `on_request`).

- [ ] **Step 7: Build and run it to see it fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/p2t4_c2_red.log 2>&1; echo NINJA_EXIT=$? >> build/p2t4_c2_red.log
tail -1 build/p2t4_c2_red.log; grep -a -n "error:" build/p2t4_c2_red.log | head -40
```

Expected: `NINJA_EXIT=1`; errors in `gtest_cas_event_log.cpp` only: `no matching function for call to
'reportMountRenewCompletion'`, with the two-parameter declaration of ~:32 as the candidate. Record the
output.

- [ ] **Step 8: Implement the report from the result and remove the stack**

`$W/SH`, immediately after `struct MountRenewResult`:

```cpp
/// One event and one log line for a renewal that retried, was resolved by a read, restored an expired
/// lease, or ended terminal; silent otherwise. `lease_deadline_before_boot_ms` is the lease deadline
/// the renewal started under. Never throws.
void reportMountRenewCompletion(
    const MountRenewResult & result,
    const String & server_root_id,
    const CasEventSink & event_sink,
    uint64_t lease_deadline_before_boot_ms,
    std::optional<uint64_t> expired_ms) noexcept;
```

`$W/S`: delete the three forward declarations at the top of `namespace DB::Cas` (S:49-52):

```cpp
void reportMountRenewCompletion(const MountRenewResult & result, std::optional<uint64_t> expired_ms) noexcept;
void configureMountRenewObservability(
    const String * server_root_id, const CasEventSink * event_sink, bool deferred) noexcept;
void deliverDeferredMountRenewObservability(uint64_t remount_attempt_no) noexcept;
```

Same file, anonymous namespace: replace everything from the comment `/// One logical renewal's audit
snapshot.` (S:116) through the closing brace of `deliverMountRenewObservability` (S:420) with the block
below. It keeps `elapsedSince` and `terminalClassificationName` (seven values, as left by Task 3) and
deletes `MountRenewObservabilityContext`, its `static_assert`, `MountRenewObservabilityConfiguration`,
`MountRenewObservabilityStack`, the `thread_local`, `currentMountRenewObservability`,
`MountRenewObservabilityRegistration`, `beginMountRenewObservabilityCall`,
`abandonMountRenewObservabilityCall`, `MountRenewObservabilityCallGuard`,
`initializeMountRenewObservability` and `deliverMountRenewObservability`. `defaultBootMs` above it
stays (the constructor uses it), and so does `describeMountHolder` below it.

```cpp
uint64_t elapsedSince(uint64_t start_boot_ms, uint64_t now_boot_ms)
{
    return now_boot_ms >= start_boot_ms ? now_boot_ms - start_boot_ms : 0;
}

/// What was left of the lease the renewal started under when the renewal returned.
uint64_t remainingConfirmedBudget(const MountRenewResult & result, uint64_t lease_deadline_before_boot_ms)
{
    const uint64_t initial_budget_ms = lease_deadline_before_boot_ms > result.attempt_start_boot_ms
        ? lease_deadline_before_boot_ms - result.attempt_start_boot_ms
        : 0;
    return initial_budget_ms > result.elapsed_ms ? initial_budget_ms - result.elapsed_ms : 0;
}

void emitMountRenewEvent(
    const MountRenewResult & result,
    const String & server_root_id,
    const CasEventSink & event_sink,
    uint64_t lease_deadline_before_boot_ms,
    std::optional<uint64_t> expired_ms,
    std::string_view outcome,
    std::string_view classification) noexcept
{
    if (!event_sink)
        return;
    try
    {
        CasEvent event;
        event.type = CasEventType::WatermarkRenew;
        event.outcome = String{outcome};
        if (outcome != "recovered")
            event.reason = "CAS mount renewal ended without retained authority and fenced the mount";
        else if (expired_ms)
            event.reason = "CAS mount renewal restored a lease that had expired";
        else
            event.reason = "CAS mount renewal committed after a retry or a resolving read";
        event.detail = {
            {"server_root_id", server_root_id},
            {"writer_epoch", std::to_string(result.writer_epoch)},
            {"seq", std::to_string(result.seq)},
            {"write_attempt_id", u128ToHex(result.write_attempt_id).substr(0, 12)},
            {"attempts_sent", std::to_string(result.attempts_sent)},
            {"elapsed_ms", std::to_string(result.elapsed_ms)},
            {"remaining_confirmed_budget_ms",
             std::to_string(remainingConfirmedBudget(result, lease_deadline_before_boot_ms))},
            {"classification", String{classification}},
        };
        if (expired_ms)
            event.detail["expired_ms"] = std::to_string(*expired_ms);
        event_sink(std::move(event));
    }
    catch (...)
    {
        /// Event construction and sink failures are diagnostic-only.
    }
}

constexpr std::string_view terminalClassificationName(MountRenewTerminalClassification classification)
{
    switch (classification)
    {
        case MountRenewTerminalClassification::DeterministicFailure: return "deterministic_failure";
        case MountRenewTerminalClassification::Conflict: return "conflict";
        case MountRenewTerminalClassification::Vanished: return "vanished";
        case MountRenewTerminalClassification::Cancelled: return "cancelled";
        case MountRenewTerminalClassification::FenceOrLifecycleLost: return "fence_or_lifecycle_lost";
        case MountRenewTerminalClassification::Unresolved: return "unresolved";
        case MountRenewTerminalClassification::Unclassified: return "terminal_unclassified";
    }
    return "terminal_unclassified";
}
```

Same file, outside the anonymous namespace: replace `configureMountRenewObservability`, the
two-parameter `reportMountRenewCompletion` and `deliverDeferredMountRenewObservability` (S:448-492) with:

```cpp
void reportMountRenewCompletion(
    const MountRenewResult & result,
    const String & server_root_id,
    const CasEventSink & event_sink,
    uint64_t lease_deadline_before_boot_ms,
    std::optional<uint64_t> expired_ms) noexcept
{
    try
    {
        const bool recovered = result.outcome == MountRenewOutcome::Committed
            && (result.attempts_sent > 1 || result.resolved_by_read || expired_ms.has_value());
        if (recovered)
        {
            std::string_view classification = "committed_after_expiry";
            if (result.resolved_by_read)
                classification = "committed_by_read";
            else if (result.attempts_sent > 1)
                classification = "committed_after_retry";
            emitMountRenewEvent(
                result, server_root_id, event_sink, lease_deadline_before_boot_ms, expired_ms, "recovered", classification);
            try
            {
                LOG_INFO(
                    getLogger("CasMountLeaseRenewer"),
                    "CAS mount renewal '{}' recovered after {} physical attempts in {} ms "
                    "(classification={}, confirmed_deadline_boot_ms={})",
                    server_root_id,
                    result.attempts_sent,
                    result.elapsed_ms,
                    classification,
                    lease_deadline_before_boot_ms);
            }
            catch (...)
            {
            }
        }
        else if (result.outcome == MountRenewOutcome::Terminal)
        {
            const std::string_view classification = terminalClassificationName(result.classification);
            emitMountRenewEvent(
                result, server_root_id, event_sink, lease_deadline_before_boot_ms, expired_ms, "failed", classification);
            try
            {
                LOG_WARNING(
                    getLogger("CasMountLeaseRenewer"),
                    "CAS mount renewal '{}' fenced after {} physical attempts in {} ms "
                    "(classification={}, confirmed_deadline_boot_ms={})",
                    server_root_id,
                    result.attempts_sent,
                    result.elapsed_ms,
                    classification,
                    lease_deadline_before_boot_ms);
            }
            catch (...)
            {
            }
        }
    }
    catch (...)
    {
        /// Formatting, logger, and event-sink failures are diagnostic-only.
    }
}
```

`MountLeaseRenewer::renew`: delete the first two lines of the body,

```cpp
    const MountRenewObservabilityRegistration observability_registration = beginMountRenewObservabilityCall();
    const MountRenewObservabilityCallGuard observability_guard(observability_registration);
```

and the block after `const String body = encodeBody(...)`:

```cpp
    if (observability_registration != MountRenewObservabilityRegistration::Ignored)
    {
        initializeMountRenewObservability(
            srid,
            writer_epoch,
            next_seq,
            write_attempt_id,
            attempt_start_boot_ms,
            confirmed_deadline_boot_ms,
            event_sink);
    }
```

Leave the includes of `S` as they are.

`$W/R`: delete the two forward declarations at the top of `namespace DB::Cas` (R:39-41):

```cpp
void reportMountRenewCompletion(const MountRenewResult & result, std::optional<uint64_t> expired_ms) noexcept;
void configureMountRenewObservability(
    const String * server_root_id, const CasEventSink * event_sink, bool deferred) noexcept;
```

`CasMountRuntime::renewOnce`, as left by Task 3. Three edits, located by the calls they touch:

```diff
-    configureMountRenewObservability(&server_root_id, &event_sink, /*deferred=*/false);
```
(delete the call in whatever argument form Task 3 left it, with the comment above it about the parked
redo, if one is left);

```diff
+    const uint64_t lease_deadline_before_boot_ms = mount_fence.deadline_boot_ms.load(std::memory_order_acquire);
     const MountRenewResult result = renewer->renew(renewalEnvironment());
```
(the sample goes on the line before the `renew` call, whatever Task 1 named the renewer pointer; it is
an atomic load and needs no lock);

```diff
-    reportMountRenewCompletion(result, expired_ms);
+    reportMountRenewCompletion(result, server_root_id, event_sink, lease_deadline_before_boot_ms, expired_ms);
```
(the second argument of the old call, whatever Task 1 named it, becomes the last argument of the new
one; if Task 1 left more than one call, each gets the same edit). Each call must sit after the scope
that holds `driver_mutex` has closed: the report calls the event sink and the logger, and no lock is
held across either. Check:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && grep -n "reportMountRenewCompletion\|configureMountRenewObservability" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp
```

Expected: only the new five-argument call line(s), inside `renewOnce`.

`$W/P`: delete the forward declaration (P:73) `void deliverDeferredMountRenewObservability(uint64_t
remount_attempt_no) noexcept;` with the blank line after it, and in `Pool::tryRemountOnce`'s
`SCOPE_EXIT` delete the comment and the call:

```cpp
        /// A parked redo completed while the whole-chain serializer was held. Drain its POD snapshot
        /// first, after lock destruction, so renewal recovery/failure precedes and correlates with the
        /// containing remount result without any callback or allocation under `remount_mutex`.
        deliverDeferredMountRenewObservability(attempt_no);

```

so the `SCOPE_EXIT` starts with `if (succeeded)`.

`$W/src/Disks/tests/gtest_cas_event_log.cpp`: delete the file-scope declarations (~:28-33):

```cpp
namespace DB::Cas
{
void configureMountRenewObservability(
    const String * server_root_id, const CasEventSink * event_sink, bool deferred) noexcept;
void reportMountRenewCompletion(const MountRenewResult & result, std::optional<uint64_t> expired_ms) noexcept;
}
```

and delete `TEST(CASEvent, DeepReentrancyPreservesDeterministicPhysicalAttemptTruth)` with the comment
above it (~:325-414). Its subject, the eight-slot stack, is gone.

Check that nothing of the stack is left:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && grep -rn "MountRenewObservab\|configureMountRenewObservability\|deliverDeferredMountRenewObservability\|markMountRenewTermination\|remount_attempt_no\|DeepReentrancyPreserves" src/
```

Expected: no line.

- [ ] **Step 9: Build, run the tests of this cycle, then the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/p2t4_c2_build.log 2>&1; echo NINJA_EXIT=$? >> build/p2t4_c2_build.log
tail -1 build/p2t4_c2_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASEvent.*:CASHeartbeat.*:CASMountAudit.*:CASMountRuntime.*:CASMountRemount.*:CASPoolRemount.*' > build/p2t4_c2_tests.log 2>&1; echo GATE_EXIT=$? >> build/p2t4_c2_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/p2t4_c2_tests.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CAS*' > build/p2t4_c2_gate.log 2>&1; echo GATE_EXIT=$? >> build/p2t4_c2_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/p2t4_c2_gate.log | tail -20
```

Expected: `NINJA_EXIT=0`; both runs `GATE_EXIT=0`, no `FAILED`; the gate count unchanged from cycle 1
(one test added, one deleted). The event tests listed in the table's paragraph pass without edits; that
is the byte-compatibility proof. A `GATE_EXIT=124` whose last `[ RUN ]` is
`CASEvent.ReentrantRenewalSinkPreservesOuterObservationIdentity` means a report runs under
`driver_mutex`: move it out, do not edit the test.

Show the new test is load-bearing, one change at a time (rebuild, run
`--gtest_filter='CASEvent.TheReportIsBuiltFromTheResult'`, see `FAILED`, restore, rebuild):
- in `remainingConfirmedBudget`, return `initial_budget_ms`: fails with `remaining_confirmed_budget_ms` `777 + elapsed`;
- pass `result.attempt_start_boot_ms` instead of `lease_deadline_before_boot_ms` to the `LOG_INFO`: fails on the log text.

- [ ] **Step 10: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp \
  src/Disks/tests/gtest_cas_event_log.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Report CAS mount renewals from their result, without a thread-local stack" \
  -m "renewOnce reads the fence deadline before the renewal and, after the consume step and with no lock held, passes it with the result to reportMountRenewCompletion. The event fields and log texts are unchanged; the remount_attempt_no detail goes, since no renewal runs inside a remount. The per-thread observation stack, its deferred delivery from tryRemountOnce and its nesting are removed, together with the test of the eight-slot stack." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

---

- [ ] **Step 11: Write the failing test for the farewell's deadline argument**

`$W/src/Disks/tests/gtest_cas_heartbeat.cpp`, after
`TEST(CASHeartbeat, FarewellIsRefusedWhenTheLeaseExpiresBeforeItsDerivedWindow)` (~:502). It uses
`DefaultEnvelopeBackend` from the anonymous namespace above those tests.

```cpp
/// The farewell is bounded by the deadline its caller passes. A near deadline refuses it although the
/// renewer's own lease would admit it, and a far one admits it although that lease would refuse it.
TEST(CASHeartbeat, FarewellIsBoundByTheDeadlineItIsGiven)
{
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);

    {
        auto backend = std::make_shared<DefaultEnvelopeBackend>();
        uint64_t now_ms = 1000;
        uint64_t boot_ms = 100;
        Ops ops(backend, &boot_ms);
        seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/30000);
        MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                                  std::chrono::milliseconds(30000), [&] { return now_ms; },
                                  [] { return uint64_t{5}; }, {}, std::chrono::milliseconds(2000),
                                  [&] { return boot_ms; });
        renewer.start();

        now_ms = 2000;
        String message;
        int code = 0;
        try
        {
            /// A TTL of 30000 admits this farewell (`FarewellIsAdmittedUnderTheDefaultBudget`); the
            /// deadline passed here leaves too little past the margin for the write's reservation.
            renewer.release(boot_ms + 5000);
            ADD_FAILURE() << "a farewell must be refused when the deadline it is given cannot admit it";
        }
        catch (const DB::Exception & e)
        {
            message = e.message();
            code = e.code();
        }
        EXPECT_EQ(code, DB::ErrorCodes::NETWORK_ERROR) << message;
        EXPECT_NE(message.find("gave up at the lease deadline after zero attempt(s)"), String::npos) << message;
        EXPECT_NE(decodeMountLease(ops.op.read(layout.mountKey(srid), Retry::standard())->bytes).min_active_build_sequence,
                  std::numeric_limits<uint64_t>::max())
            << "the refused farewell must not have landed";
    }

    {
        auto backend = std::make_shared<DefaultEnvelopeBackend>();
        uint64_t now_ms = 1000;
        uint64_t boot_ms = 100;
        Ops ops(backend, &boot_ms);
        seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/5000);
        MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                                  std::chrono::milliseconds(5000), [&] { return now_ms; },
                                  [] { return uint64_t{5}; }, {}, std::chrono::milliseconds(2000),
                                  [&] { return boot_ms; });
        renewer.start();

        now_ms = 2000;
        /// A TTL of 5000 refuses this farewell (`FarewellIsRefusedWhenTheLeaseExpiresBeforeItsDerivedWindow`).
        EXPECT_NO_THROW(renewer.release(boot_ms + 30000));
        const MountLease farewell = decodeMountLease(ops.op.read(layout.mountKey(srid), Retry::standard())->bytes);
        EXPECT_LE(farewell.expires_at_ms, now_ms);
        EXPECT_EQ(farewell.min_active_build_sequence, std::numeric_limits<uint64_t>::max());
    }
}
```

- [ ] **Step 12: Build and run it to see it fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/p2t4_c3_red.log 2>&1; echo NINJA_EXIT=$? >> build/p2t4_c3_red.log
tail -1 build/p2t4_c3_red.log; grep -a -n "error:" build/p2t4_c3_red.log | head -40
```

Expected: `NINJA_EXIT=1`; two errors in `gtest_cas_heartbeat.cpp`, both in the new test:
`too many arguments to function call, expected 0, have 1`. Record the output.

- [ ] **Step 13: Implement `release(deadline)` and remove the renewer's copy of the deadline**

`$W/SH`, `class MountLeaseRenewer`. Replace `void release();` (~:588) with:

```cpp
    /// The farewell. It gives up rather than run past `lease_deadline_boot_ms` less the safety margin;
    /// the deadline is on the boot clock this renewer anchors its renewals on.
    void release(uint64_t lease_deadline_boot_ms);
```

Replace `void terminate(CasOperation & op);` (~:604) with
`void terminate(CasOperation & op, uint64_t lease_deadline_boot_ms);`, and delete the member
`uint64_t confirmed_deadline_boot_ms = 0;` (~:628).

`$W/S`, `MountLeaseRenewer::start` (~:1475-1482): the end of the function becomes

```cpp
    seq = 1;
    last_etag = etag;
    last_committed_attempt_start_boot_ms = attempt_start_boot_ms;
    renewer_state = MountLeaseRenewerState::Active;
    return attempt_start_boot_ms;
}
```

(the `ttl_ms` local and the `confirmed_deadline_boot_ms` assignment go).

`MountLeaseRenewer::renew`, the `Committed` branch:

```cpp
    if (Committed * committed = std::get_if<Committed>(&*written))
    {
        seq = next_seq;
        last_etag = std::move(committed->etag);
        last_committed_attempt_start_boot_ms = attempt_start_boot_ms;
        result.outcome = MountRenewOutcome::Committed;
        result.attempts_sent = committed->attempts_sent;
        result.resolved_by_read = committed->resolved_by_read;
        result.sent_any = committed->attempts_sent != 0;
        return finished(std::move(result));
    }
```

`MountLeaseRenewer::terminate` (~:1751): the signature becomes
`void MountLeaseRenewer::terminate(CasOperation & op, uint64_t lease_deadline_boot_ms)`. In its long
comment, delete the parenthetical `(see the
`expires_at_ms`/`confirmed_deadline_boot_ms` arithmetic above)`, so that sentence reads
"Saturating, like every other deadline computation on this path: an operator-configured envelope is
not bounds-checked ...". In the second comment, delete its last three lines, from
`` `confirmed_deadline_boot_ms` is set at `start()` `` through `` which `start` alone establishes). ``.
The write becomes:

```cpp
    WriteResult written = op.replace(key, body, precondition(),
        Retry::untilLeaseSafe(lease_deadline_boot_ms, static_cast<uint64_t>(lease_safety_margin.count()), farewell_window_ms));
```

`MountLeaseRenewer::release` (~:1824):

```cpp
void MountLeaseRenewer::release(uint64_t lease_deadline_boot_ms)
{
    if (renewer_state != MountLeaseRenewerState::Active)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount-lease: release is allowed only in Active state for key '{}'", key);
    renewer_state = MountLeaseRenewerState::Released;
    /// Off the mount fence, for the same reason the claim is: a departing mount whose lease has already
    /// run down still has to hand the slot back, and refusing the write there would leave the slot
    /// looking live until GC fences it out.
    CasOperation op = open_requests.admit();
    terminate(op, lease_deadline_boot_ms);
}
```

`$W/R`, `CasMountRuntime::finishTeardown` (R:1218). The deadline is the start of the renewer's last
committed renewal plus the TTL, which is the value the renewer kept before this change in every case,
including a reclaim that started a renewer and failed before it armed the fence. It is read after
`stopBackgroundWorkers` joined the lease thread, so nothing else touches the renewer:

```cpp
    if (drained && mount_renewer->state() == MountLeaseRenewerState::Active)
    {
        const uint64_t ttl_ms = static_cast<uint64_t>(config.mount_lease_ttl_ms.count());
        const uint64_t start_boot_ms = mount_renewer->lastCommittedAttemptStartBootMs();
        try
        {
            mount_renewer->release(start_boot_ms > std::numeric_limits<uint64_t>::max() - ttl_ms
                ? std::numeric_limits<uint64_t>::max()
                : start_boot_ms + ttl_ms);
        }
        catch (...)
        {
            tryLogCurrentException(getLogger("CasPool"), "CAS mount-lease: release during Pool teardown failed");
        }
    }
```

Tests: the 13 `release` lines, as the table above lists them, in `gtest_cas_heartbeat.cpp` and
`gtest_cas_mount.cpp`.

Checks:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
grep -rn "confirmed_deadline_boot_ms" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed
grep -rnE "\b(renewer|k|departing)\.release\(\)" src/Disks/tests
grep -rn "lastCommittedAttemptStartBootMs() + " src/Disks/tests | grep -c "release("
```

Expected: the first grep prints exactly two lines, the two `confirmed_deadline_boot_ms={}` format texts
in `reportMountRenewCompletion`; the second prints nothing; the count is `13`.

- [ ] **Step 14: Build, run the tests of this cycle, then the `CAS*` gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/p2t4_c3_build.log 2>&1; echo NINJA_EXIT=$? >> build/p2t4_c3_build.log
tail -1 build/p2t4_c3_build.log
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CASHeartbeat.*:CASMountAudit.*:CASMountLease.*:CASPoolRemount.*:CASPoolShutdown.*:CASForget.*' > build/p2t4_c3_tests.log 2>&1; echo GATE_EXIT=$? >> build/p2t4_c3_tests.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/p2t4_c3_tests.log | tail -20
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='CAS*' > build/p2t4_c3_gate.log 2>&1; echo GATE_EXIT=$? >> build/p2t4_c3_gate.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/p2t4_c3_gate.log | tail -20
```

Expected: `NINJA_EXIT=0`; both runs `GATE_EXIT=0`, no `FAILED`; the gate has one test more than after
cycle 2. The three farewell tests of `gtest_cas_heartbeat.cpp` pass with only their `release` argument
changed, and `CASPoolRemount.TeardownJoinsBothWorkersBeforeRelease` (as adapted by Task 1) shows the
production farewell still lands.

Show the new test is load-bearing (rebuild, run `--gtest_filter='CASHeartbeat.FarewellIsBoundByTheDeadlineItIsGiven'`,
see `FAILED`, restore, rebuild): in `release`, pass `last_committed_attempt_start_boot_ms +
static_cast<uint64_t>(ttl.count())` to `terminate` instead of the argument; both blocks of the test fail.

- [ ] **Step 15: Commit**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp \
  src/Disks/tests/gtest_cas_heartbeat.cpp \
  src/Disks/tests/gtest_cas_mount.cpp
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "Pass the lease deadline to the CAS mount farewell" \
  -m "MountLeaseRenewer::release takes the deadline that bounds the farewell, and the renewer no longer keeps its own copy of the lease deadline. finishTeardown passes the start of the last committed renewal plus the TTL, the value the copy held." \
  -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"
```

**Deviations from the contract**

- `CA/Pool/CasMountRuntime.h` and `src/Disks/tests/gtest_cas_pool.cpp` are in T6's file list and are
  not changed. `renewOnce` keeps its signature, and the pool tests that read `remount_attempt_no` are
  deleted by Task 2 (`ParkedRedoRecoveryObservabilityPrecedesRemountResult`,
  `ParkedRedoFailureObservabilityPrecedesRemountResult`). If either survived Task 2, delete its
  `remount_attempt_no` assertion in cycle 2 and say so in the commit body.
- `elapsed_ms` and `remaining_confirmed_budget_ms` keep their names and arithmetic but change their
  clock in tests that inject one: they read the renewal's boot clock, as the contract's definition of
  `elapsed_ms` says, where the stack read the real `CLOCK_BOOTTIME`. No existing test asserts either
  value.
- `CASEvent.ReentrantRenewalSinkPreservesOuterObservationIdentity` stays unchanged, although it hangs
  rather than fails if a report moves under `driver_mutex`. It cannot be made to fail without real time
  or a new production hook:
  - a `try_lock` probe needs `driver_mutex`, which is private to `CasMountRuntime`. The test reaches
    it only through a `Pool`, and no `Pool` or runtime method tries the lock without blocking. The one
    `try_to_lock` site, `lockTerminalPublication`, runs only inside terminal publications
    (`enterVanished`, `enterIdentityLost`). Its `MountConfig` hooks would end the pool the test goes on
    to renew;
  - a nested call on a helper thread with a future fails at no bound measured in requests. A helper
    blocked on the mutex sends no request, so the injected clock never moves, and only a real-time
    `wait_for` could end the wait.

  What remains is the gate's `timeout 1200`: such a regression shows as `GATE_EXIT=124`, with this test
  as the last `[ RUN ]` line. Step 9 names it.

**Review focus**

- A renewal whose fence was armed by hand or never armed: `lease_deadline_before_boot_ms` is then the
  hand-set value or `UINT64_MAX`, and `remaining_confirmed_budget_ms` and the logged
  `confirmed_deadline_boot_ms` show it. Expected: production never renews before a deadline is
  published, so it reports the same values as before; no test pins this.
- An event sink that renews the same pool from the report. It is not supported, but
  `ReentrantRenewalSinkPreservesOuterObservationIdentity` does it. Expected: the report runs with no
  lock held, so the nested renewal completes. If the report moved under `driver_mutex`, that test hangs
  instead of failing.
- A farewell after a reclaim installed and started a new renewer and failed before `armIfAdmissible`.
  Expected: the farewell is bounded by the new renewer's claim plus the TTL, not by the old fence
  deadline. `finishTeardown` reads it from the renewer, and no test drives this path.

---

### Task 5: One definition each; further cleanups

Twelve commits, one per item: 7a, 7b, 7c, then 8.1 to 8.9. No behaviour changes, except 8.7, which drops a duplicate log line. Every commit leaves the tree building and its suites green. The whole `CAS*` gate runs after 7c, 8.4, 8.6 and at the end.

`CA` = `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`. `T` = `src/Disks/tests`. `R`, `RH`, `P`, `PH`, `S`, `SH` are the contract's abbreviations (`CA/Pool/CasMountRuntime.cpp`, its header, `CasPool`, `CasServerRoot`). Positions are today's (HEAD `7e243502408`) and carry the function name. Tasks 1 to 4 rewrite parts of `R`, `RH`, `P`, `S`, `SH`. Where a hunk touches such code, it says "as left by Task N", quotes the contract's target shape and anchors on a grep. Find the code by the grep, not by the line.

**Files:**
- Modify: `CA/Backend/CasRequestBudget.h`, `CA/Backend/CasRequestBudget.cpp`, `CA/Backend/CasRetry.h` (7a, 7b, 7c)
- Modify: `CA/ContentAddressedSettings.cpp`, `CA/ContentAddressedMetadataStorage.h`, `CA/ContentAddressedMetadataStorage.cpp` (7b, 7c, 8.4), `docs/en/antalya/cas/configuration.md` (7c)
- Modify: `CA/Pool/CasMountRuntime.h`, `CasMountRuntime.cpp`, `CasPool.h`, `CasPool.cpp`, `CasServerRoot.h`, `CasServerRoot.cpp` (7a to 8.9), `CA/Pool/CasPartWriteTxn.h`, `CA/Pool/CasRefLedger.h` (comments, 8.1)
- Modify: `CA/Gc/CasGcScheduler.h`, `CA/Gc/CasGcScheduler.cpp` (8.4, 8.5, 8.8)
- Modify: `tests/integration/test_cas_mount_renewal_retry/test.py` (8.7)
- Test: `T/gtest_cas_mount.cpp` (7a new test, 8.1, 8.6), `T/gtest_cas_pool.cpp`, `T/gtest_cas_heartbeat.cpp`, `T/gtest_cas_mount_runtime.cpp`, `T/gtest_cas_fence_generation.cpp`, `T/cas_test_helpers.h`, `T/gtest_cas_ref_wedge_every_attempt.cpp`, the other ten files of the 8.1 sweep, the four files of the 8.4 sweep, `T/gtest_cas_gc_teardown_stop.cpp` and `T/gtest_cas_operation_gate.cpp` (8.8)

**Interfaces:**
- Consumes: `armFence` and `armMountFence` as left by Task 1; the two-plane `MountLeaseRenewer` constructor and `CasMountRuntime` constructor of Task 3; the extended `MountRenewResult` of Task 4.
- Produces: `CasRequestBudget::writeAndSettlementReadMs`, `kDefaultLeaseSafetyMarginMs`, `kStandardWriteWindowMs`, `CasMountRuntime::startBackgroundWorkers()` with no parameter, `CasGcScheduler::runOneRoundNow()` with no parameter, `Pool::remountTerminal`, `armMountFence(uint64_t deadline_boot_ms)`. Nothing later in this plan relies on them.

## Claims checked at HEAD

Each "no production code reads it" claim was checked with an untruncated grep over `src`, `programs`, `utils`, `docs` and `tests`. Re-run the grep named in the commit before deleting: Tasks 1 to 4 may have added a reader.

| Symbol | Readers at HEAD | Verdict |
|---|---|---|
| `process_epoch`, `epoch`, `writerEpoch` (`CasMountRuntime`, `Pool`) | none in production. Tests: 64 `writerEpoch()` lines in 12 files, 1 `setProcessEpoch` line (`gtest_cas_pool.cpp`, test `ExternalLossDuringRenewalUsesOneRecoveryGeneration`'s callback), no `epoch()` call | done in 8.1 |
| `terminal_state_published` | `enterVanished` (twice), `setLifecycleForTest`; nothing else | done in 8.2 |
| `renewal_period` | the loop's due-time computation only; `startBackgroundWorkers` is its only writer | done in 8.3 |
| `MountRenewOperationEnvironment::boot_ms` | `MountLeaseRenewer::renewOn`, which falls back to the renewer's own `boot_ms_fn`. `CasMountRuntime::renewalEnvironment` sets it to the same `bootMsNow` that `installRenewer` passes as `boot_ms_fn`. The one test helper that sets it, `renewalEnvironment` in `gtest_cas_heartbeat.cpp`, is used only with renewers built with a `boot_ms_fn` over the same local (all 31 constructions checked) | done in 8.3 |
| `MountFence::server_uuid`, `MountFence::writer_epoch` | written in `armMountFence`, never read. `CasRefCatalog.h` and `CasServerRoot.h` mention `fence.writer_epoch` in comments about `CreatorFence`, which is another type | done in 8.6 |
| `CasMountRuntime::backend_ptr` | none. It is a constructor parameter, so two construction sites change: the `Pool` member initializer and `RuntimeUnderTest` | done in 8.6 |
| `Pool::farewellRequests` | none, in `src` or `T` | done in 8.6 |
| `MountRenewResult::sent_any` | none in production (the `sent_any` reads in `CasRefLedger.cpp`, `CasHotKeys.cpp`, `CasWriteResult.h`, `CasServerRoot.cpp` are `GaveUp::sent_any`). Tests: `gtest_cas_heartbeat.cpp` (2 lines), `gtest_cas_mount.cpp` (1 line) | done in 8.6 |
| `CasGcScheduler::setRoundInFlightForTest` | `gtest_cas_operation_gate.cpp`, 2 lines | done in 8.8 |
| `Pool::vanishedIntentPublished` | only the two GC scheduler loops that 8.5 changes | removed in 8.5 (a deviation) |

Not touched, as the spec decides: one request plane for GC, renewal and farewell; one claim policy for open, remount and decommission; GC leadership without the heartbeat object, the acquisition pulse or the `i_am_leader` hint; a simpler steal rule. Soak card S39 is not part of this task.

## Common commands

Build (`<id>` names the log):

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && flock build/.wok-build.lock ninja -C build unit_tests_dbms > build/t5_<id>_build.log 2>&1; echo NINJA_EXIT=$? >> build/t5_<id>_build.log
tail -1 build/t5_<id>_build.log
```

On failure: `grep -a -n "error:" build/t5_<id>_build.log | head -40`. Read `NINJA_EXIT=0` before trusting any test output.

Tests (`<filter>` is a gtest filter):

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && ( ulimit -v 33554432; flock build/.wok-build.lock timeout 1200 build/src/unit_tests_dbms --gtest_filter='<filter>' > build/t5_<id>_run.log 2>&1; echo GATE_EXIT=$? >> build/t5_<id>_run.log )
grep -a "FAILED\|PASSED\|GATE_EXIT" build/t5_<id>_run.log | tail -20
```

`GATE_EXIT=124` is a hang: find the last `[ RUN ]` line. The gate is the same command with `--gtest_filter='CAS*'`.

Commit: `git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 add -- <exact files>`, then `git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 commit -m "<subject>" -m "<body>" -m "Co-Authored-By: <the implementer's model> <noreply@anthropic.com>"`. Exact files only; check `git status --short -- <files>` first.

No test in this task expects `LOGICAL_ERROR`, so no death-test split is needed.

---

## Commit 7a: `2 x envelope` in one place

A deletion-and-extraction commit. The one new test is a pin for the extracted method, so it has a red run (compile failure) and a green run.

**Where the doubling lives at HEAD**, found with `grep -rn "max() / 2" CA`:
- `CasMountRuntime::refAppendFenceOk` (`R:168`);
- `validateCasRequestBudget` (`CasRequestBudget.cpp:53`);
- `Pool::mountWritable` (`P:877`, `two_envelopes_ms`) and `Pool::tryRemountOnce` (`P:1554`): both are the window tests that Task 5a removes;
- `MountLeaseRenewer::terminate` (`S:1775`): it doubles the plane's `attemptReservationMs`, a different quantity. It keeps its own arithmetic and its slack.

After Task 5a the arming rule has "one private helper" shared with `refAppendFenceOk` (contract 2.2). If that helper holds a doubling, it is replaced here like `refAppendFenceOk`.

- [ ] **Step 7a.1: Write the failing test**

In `T/gtest_cas_mount.cpp`, insert after the closing brace of `TEST(CASRequestBudget, ValidateAcceptsDefaultsAndRejectsAnOverflowingSumWithoutWrapping)`:

```cpp
TEST(CASRequestBudget, WriteAndSettlementReadIsTwoEnvelopesAndSaturates)
{
    const CasRequestBudget defaults{};
    EXPECT_EQ(defaults.attemptEnvelopeMs(), 7000u);
    EXPECT_EQ(defaults.writeAndSettlementReadMs(), 14000u);

    /// No S3 client: the envelope is the attempt alone.
    const CasRequestBudget no_connect_cap{.attempt_timeout_ms = 5000, .connect_timeout_cap_ms = std::nullopt};
    EXPECT_EQ(no_connect_cap.writeAndSettlementReadMs(), 10'000u);

    /// The largest envelope that doubles without wrapping, and the first one that does not.
    constexpr uint64_t half_max = std::numeric_limits<uint64_t>::max() / 2;
    const CasRequestBudget fits{.attempt_timeout_ms = half_max - 2000};
    ASSERT_EQ(fits.attemptEnvelopeMs(), half_max);
    EXPECT_EQ(fits.writeAndSettlementReadMs(), 2 * half_max);
    const CasRequestBudget wraps{.attempt_timeout_ms = half_max - 1999};
    ASSERT_EQ(wraps.attemptEnvelopeMs(), half_max + 1);
    EXPECT_EQ(wraps.writeAndSettlementReadMs(), std::numeric_limits<uint64_t>::max());
}
```

- [ ] **Step 7a.2: Build; expect the compile failure**

Build with `<id>` = `7a_red`. Expected: `NINJA_EXIT=1`, and the only errors are in `gtest_cas_mount.cpp`: `no member named 'writeAndSettlementReadMs' in 'DB::Cas::CasRequestBudget'`. Paste that line into the report. An error anywhere else means the tree is not at the expected base: stop and report.

- [ ] **Step 7a.3: Implement**

`CA/Backend/CasRequestBudget.h`, in `struct CasRequestBudget`, after `attemptEnvelopeMs`:

```cpp
    /// What a conditional write can cost end to end: its attempt and the read that settles it, two
    /// envelopes, saturating. Ref-log appends are admitted against this, and so is the renewal cadence rule
    /// of `validateCasRequestBudget`.
    uint64_t writeAndSettlementReadMs() const;
```

`CA/Backend/CasRequestBudget.cpp`, after `attemptEnvelopeMs`:

```cpp
uint64_t CasRequestBudget::writeAndSettlementReadMs() const
{
    const uint64_t envelope = attemptEnvelopeMs();
    return envelope > std::numeric_limits<uint64_t>::max() / 2 ? std::numeric_limits<uint64_t>::max() : 2 * envelope;
}
```

`validateCasRequestBudget`, inside `if (background_renewal)`. Before:

```cpp
        /// A renewal is a write: two envelopes (the attempt and its settlement read) after one period.
        /// Saturating doubling first (matching the production horizon checks' own arithmetic), then
        /// subtraction-based comparisons against the TTL -- no truncating division, so this enforces
        /// exactly the inequality the exception message states, not an off-by-one-tighter one.
        const uint64_t two_envelope = envelope > std::numeric_limits<uint64_t>::max() / 2
            ? std::numeric_limits<uint64_t>::max() : 2 * envelope;
```

After:

```cpp
        /// A renewal is a write: two envelopes (the attempt and its settlement read) after one period.
        /// Subtraction-based comparisons against the TTL, no truncating division, so this enforces
        /// exactly the inequality the exception message states, not an off-by-one-tighter one.
        const uint64_t two_envelope = budget.writeAndSettlementReadMs();
```

`CasMountRuntime::refAppendFenceOk` in `R`. Before:

```cpp
    /// Two envelopes' worth of room under the live generation -- a write and its settlement read, which
    /// is what `writeLoop` reserves -- so a ref-log attempt is not started when it cannot plausibly
    /// finish, safety margin included, before the lease expires.
    const uint64_t envelope_ms = cas_request_budget.attemptEnvelopeMs();
    const uint64_t needed_ms = envelope_ms > std::numeric_limits<uint64_t>::max() / 2
        ? std::numeric_limits<uint64_t>::max() : 2 * envelope_ms;
    return admit(fenceGeneration(), needed_ms) == Fence::Admit::Ok;
```

After:

```cpp
    /// A write and its settlement read, which is what `writeLoop` reserves: a ref-log attempt is not
    /// started when it cannot plausibly finish, safety margin included, before the lease expires.
    return admit(fenceGeneration(), cas_request_budget.writeAndSettlementReadMs()) == Fence::Admit::Ok;
```

If Task 5a moved this arithmetic into a helper that takes a deadline, replace its two-line doubling by `cas_request_budget.writeAndSettlementReadMs()` the same way and keep its comment to one sentence.

If `P` still has the two `two_envelopes_ms` blocks (Task 5a deletes both windows; check), replace each doubling by `store->config.cas_request_budget.writeAndSettlementReadMs()` / `config.cas_request_budget.writeAndSettlementReadMs()`.

- [ ] **Step 7a.4: Prove nothing else doubles an envelope; build; run**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && grep -rn "max() / 2" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed
```

Expected hits, nothing else: `CasRequestBudget.cpp` in `attemptEnvelopeMs` (the doubled connect cap) and in `writeAndSettlementReadMs`; `CasServerRoot.cpp` in `MountLeaseRenewer::terminate`. Any other hit is a doubling of an envelope that the commit missed: replace it.

Build `7a`. Run filter `CASRequestBudget*:CASMountRuntime*:CASMountOpenWaits*:CASPoolRemount*:CASPool.*`. Expected: `PASSED`, and the new test listed.

- [ ] **Step 7a.5: Commit**

Files: `CA/Backend/CasRequestBudget.h`, `CA/Backend/CasRequestBudget.cpp`, `CA/Pool/CasMountRuntime.cpp`, `T/gtest_cas_mount.cpp`, plus `CA/Pool/CasPool.cpp` if step 7a.3 touched it.
Subject: `Define the two-envelope write cost once in CasRequestBudget`.
Body: `refAppendFenceOk` and `validateCasRequestBudget` each doubled the attempt envelope with their own saturation. Both call `writeAndSettlementReadMs` now. `MountLeaseRenewer::terminate` keeps its own reservation: it doubles the plane's reservation, not the budget's envelope.`

---

## Commit 7b: one constant per literal

A deletion-and-extraction commit. No red run: the existing suites are the proof.

**Literals found** with `grep -rnE "90'000|\b2000\b|2'000\b" CA --include=*.h --include=*.cpp`:
- `2000` as the safety margin default: `CasRequestBudget.h` (`lease_safety_margin_ms`), `ContentAddressedSettings.cpp` (`lease_safety_margin_ms`), `SH` (`MountLeaseRenewer` constructor default, `std::chrono::milliseconds(2000)`).
- `2'000` in `S` (`kFarewellSlackMs`) is a separate quantity and stays its own. `CasObjectStorageBackend.cpp` (`2'000'000'000ULL`) is unrelated.
- `90'000` in `CasRetry.h`: `standard`, the default `window_ms` of `untilLeaseSafe`, and `once`.

- [ ] **Step 7b.1: Edit**

`CA/Backend/CasRequestBudget.h`, before `struct CasRequestBudget` (inside `namespace DB::Cas`):

```cpp
/// The default margin kept between a request and the mount lease deadline.
inline constexpr uint64_t kDefaultLeaseSafetyMarginMs = 2000;
```

Same file, the member: `uint64_t lease_safety_margin_ms = kDefaultLeaseSafetyMarginMs;`.

Same file, `recovery_retry_budget_ms` comment. Before: `... policy window (`Retry::standard()`'s 90s): one recovery attempt may itself burn ~90s inside a single seal PUT.` After: `... policy window (`Retry::standard()`, `kStandardWriteWindowMs`): one recovery attempt may itself spend a whole window inside a single seal PUT.` Add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>` only if the comment is meant to link: it is prose, so add no include.

`CA/ContentAddressedSettings.cpp`: `DECLARE(UInt64, lease_safety_margin_ms, Cas::kDefaultLeaseSafetyMarginMs, "<description unchanged in this commit>", 0) \`. Add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestBudget.h>` next to the other `ContentAddressed` includes.

`SH`: add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestBudget.h>` after the `CasRequests.h` include, and in the `MountLeaseRenewer` constructor: `std::chrono::milliseconds lease_safety_margin_ = std::chrono::milliseconds(static_cast<int64_t>(kDefaultLeaseSafetyMarginMs)),`.

`CA/Backend/CasRetry.h`, before `struct Retry`:

```cpp
/// The window of the default write policy.
inline constexpr uint64_t kStandardWriteWindowMs = 90'000;
```

In `Retry`, replace each literal: `within(kStandardWriteWindowMs)` in `standard`; `uint64_t window_ms = kStandardWriteWindowMs` in `untilLeaseSafe`; `.window_ms = kStandardWriteWindowMs` in `once`. The two comments that say `90'000` and "standard 90 s write budget" become "the default write policy" and "the default write window, `kStandardWriteWindowMs`".

- [ ] **Step 7b.2: Gate grep, build, run**

Re-run the grep of the section head. Expected remaining hits: the definition of `kDefaultLeaseSafetyMarginMs`, the definition of `kStandardWriteWindowMs`, `kFarewellSlackMs`, and `EMU_TOKEN_STALE_AGE_NS`. Comments that say "90s" or "90 s" (`grep -rnE "90 ?s\b" CA`) must be gone.

Build `7b`. Run `CASRequestBudget*:CASRequests*:CASRetry*:CASContentAddressedSettings*:CASSettings*:CASHeartbeat*:CASMountRuntime*`. Expected: `PASSED`.

- [ ] **Step 7b.3: Commit**

Files: the four edited headers and sources above (`CasRequestBudget.h`, `ContentAddressedSettings.cpp`, `CasServerRoot.h`, `CasRetry.h`).
Subject: `Name the default safety margin and the default write window`.
Body: `2000 stood in three places and 90'000 in three. `kFarewellSlackMs` is a different quantity and keeps its own value.`

---

## Commit 7c: stale text

Prose only. No red run. Each hunk below is a statement checked against today's code; a statement that is right stays. The list is what the read found, not a count carried from elsewhere.

- [ ] **Step 7c.1: Replace the margin text in four places**

The text "Startup-only margin ... not consulted at runtime" is wrong: `CasMountRuntime::admit` reads `lease_safety_margin_ms` on every admission, and `MountLeaseRenewer::terminate` passes it to `Retry::untilLeaseSafe`.

`CA/Backend/CasRequestBudget.h`, member. Before:

```cpp
    /// Startup-only margin folded into `validateCasRequestBudget`'s inequality against the mount lease
    /// TTL. Not consulted at runtime by the engine itself -- the caller's fence (backed by the local
    /// write fence's own deadline) is what actually gates lease-relative timing per attempt.
    uint64_t lease_safety_margin_ms = kDefaultLeaseSafetyMarginMs;
```

After:

```cpp
    /// Room kept between the end of a request and the mount lease deadline. `CasMountRuntime::admit`
    /// refuses a request unless its need plus this margin fits in the remaining lease, and the farewell's
    /// lease bound stops short of the deadline by it. `validateCasRequestBudget` checks it against the
    /// lease TTL when a writable mount opens.
    uint64_t lease_safety_margin_ms = kDefaultLeaseSafetyMarginMs;
```

`CA/ContentAddressedSettings.cpp`, description of `lease_safety_margin_ms`. Before: `"Startup-only margin validated against the mount lease TTL: attempt envelope + this must be strictly less than the TTL, and renew period + 2 × envelope + this too"`. After: `"Room kept between a request and the mount lease deadline: a write is admitted only while the requests it may send plus this margin fit in the remaining lease. Validated against the mount lease TTL when a writable mount opens: attempt envelope + this must be strictly less than the TTL, and renew period + 2 × envelope + this too"`.

`CA/ContentAddressedMetadataStorage.h`, member `cas_lease_safety_margin_ms`. Before:

```cpp
    /// Startup-only margin validated against the mount lease TTL; feeds
    /// `Cas::PoolConfig::cas_request_budget.lease_safety_margin_ms`.
```

After:

```cpp
    /// Room kept between a request and the mount lease deadline; feeds
    /// `Cas::PoolConfig::cas_request_budget.lease_safety_margin_ms`.
```

`docs/en/antalya/cas/configuration.md`, row `cas_lease_safety_margin_ms`. Replace the text after `| `2000` |` with: `Room kept between a request and the mount lease deadline: a write is admitted only while the requests it may send plus this margin fit in the remaining lease. Validated against the mount lease TTL when a writable mount opens: the attempt envelope + `cas_lease_safety_margin_ms` must be strictly less than the mount lease TTL, and `cas_mount_renew_period_ms` + 2 × envelope + `cas_lease_safety_margin_ms` too, or the disk refuses to open writable |`.

- [ ] **Step 7c.2: Delete the grace-period text**

The post-reclaim materialization grace is gone (`CasPool.h`, the comment on `PoolConfig::wait_sleep_fn`, says so; `mountObservationThresholdMs` is `ttl + ttl / 20 + cadence` with no grace term).

`CA/Backend/CasRequestBudget.h`, the doc comment of `validateCasRequestBudget`: delete the blank `///` line and the paragraph below it, from `/// A successor mounting over an unclean predecessor waits ...` through `/// check is needed here.` That paragraph argues about the successor's wait, which this function does not compute. The sentence "(a renewal is a write: two envelopes for the attempt and its settlement read)" above it stays.

`CA/Pool/CasMountRuntime.h`, comment of `waitSleep`: delete the second sentence, "`Pool` claim observation and materialization grace waits share this seam so tests control both." The first sentence stays.

- [ ] **Step 7c.3: Fix the lease-timing comment of `PoolConfig`**

"`renew_time + this`" does not say that the deadline counts from the start of the attempt. `CA/Pool/CasPool.h`, `PoolConfig::mount_lease_ttl_ms`. Before:

```cpp
    /// Mount-lease TTL: how long a freshly-renewed mount lease is valid. The local
    /// write fence's monotonic deadline is `renew_time + this`, so a superseded/paused writer is fenced
    /// once `this` elapses with no successful renew. The background renewer runs every
    /// `mount_renew_period` (default ttl/3) so a healthy mount renews well before expiry.
```

After:

```cpp
    /// Mount-lease TTL: how long a claim or renewal keeps the lease valid. The local write fence's
    /// deadline is the start of the last committed claim or renewal attempt plus this, so a superseded
    /// or paused writer is fenced once `this` elapses with no successful renewal. The background renewer
    /// starts a renewal every `mount_renew_period` (default ttl/3) so a healthy mount renews well before
    /// expiry.
```

- [ ] **Step 7c.4: Remove the markers that do not exist**

The runtime has no unclean-boundary marker and no unclean-epoch high-water mark. Find them:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && grep -rn "unclean-boundary marker\|unclean-epoch high-water-mark\|both persistent workers\|persistent renewal and remount workers" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool
```

- `CasMountRuntime.h`, class comment (`R:140` at HEAD): delete the words "unclean-boundary marker, ". The two-worker words must already be gone after Task 1 (`both persistent workers`); if the grep still hits, replace them with "the lease thread" and report it as a miss of Task 1.
- `CasPool.h`, comment of the `mount_runtime` member: delete "the unclean-epoch high-water-mark, and the" and leave "and the" in its place, so the list reads "... `live_writer_epoch`, and the ...". If Task 1 left "persistent renewal and remount workers (with one driver mutex/condition pair)", replace that phrase with "lease thread (with one driver mutex/condition pair)". Do not touch the `process_epoch` words in the same comment: 8.1 edits them.

The `CasRefLedger.h` class comment also says "unclean-boundary observation"; it is a claim about a different class and is not verified or changed here.

- [ ] **Step 7c.5: Fix the `forgetDisk` step (2) comment**

`tripMountLost` counts a loss and moves `Live` to `TransientNotLive` only when no terminal intent is published. FORGET publishes the intent at step (1), so a live pool stays `Live` until `enterVanished` at step (6). `CA/Pool/CasPool.cpp`, `Pool::forgetDisk`. Before:

```cpp
    /// (2) Trip the local fence — the deliberate decommission act (allowed on a live disk). No durable-
    /// effect write admits past this point (the fence-generation gate), and a live pool moves to
    /// `TransientNotLive`, so store-class access already fails loud during the teardown window below.
```

After:

```cpp
    /// (2) Trip the local fence — the deliberate decommission act (allowed on a live disk). No durable-
    /// effect write admits past this point (the fence-generation gate). The published intent keeps the
    /// trip from counting a lease loss, so a live pool stays `Live` until step (6).
```

- [ ] **Step 7c.6: Build, then the whole gate**

Build `7c`. Run the gate (`CAS*`), id `7c_gate`. Expected: `PASSED`, `GATE_EXIT=0`, the same test count as the base of this plan plus the tasks' new tests and this task's one new test. Compare the count with the last gate of Task 4 (read it from that task's log; do not carry a number into the commit message).

- [ ] **Step 7c.7: Commit**

Files: `CA/Backend/CasRequestBudget.h`, `CA/ContentAddressedSettings.cpp`, `CA/ContentAddressedMetadataStorage.h`, `docs/en/antalya/cas/configuration.md`, `CA/Pool/CasMountRuntime.h`, `CA/Pool/CasPool.h`, `CA/Pool/CasPool.cpp`.
Subject: `Correct the lease margin, grace and FORGET trip descriptions`.
Body: `The safety margin is read on every admission and by the farewell, not only at startup. The materialization grace and the unclean-boundary marker no longer exist. FORGET's trip leaves a live pool `Live`: the published intent suppresses the loss transition.`

---

## Commit 8.1: delete `process_epoch` and its accessors

A deletion plus a mechanical sweep. No red run.

**Read-only check (ruling O11).** On a read-only pool `writerEpoch()` was a random number and `liveWriterEpoch()` is 0, because only `mountWritable` and `tryRemountOnce` publish a live epoch. A swept line is at risk only if it runs on a read-only pool. Prove there is none:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1/src/Disks/tests && grep -n "read_only" $(grep -rl "writerEpoch()" .) cas_test_helpers.h
```

Expected, nothing else: `gtest_cas_mount.cpp` (the `PoolConfig{... .read_only = true}` in `CASMountReadOnly.ForeignOwnedPoolOpensWithoutMutation`, a test that has no `writerEpoch` line) and `cas_test_helpers.h` (`read_only_` of `LocalObjectStorageSettings`, not a pool). Tests that compare epochs for inequality (`ASSERT_GT(successor->writerEpoch(), predecessor_epoch)` and its siblings in `gtest_cas_writer_duties.cpp`, `gtest_cas_ref_ckpt.cpp`, `gtest_cas_mount.cpp`, `gtest_cas_decommission.cpp`, `gtest_cas_ref_contiguous_alloc.cpp`, `gtest_cas_ref_writer.cpp`) compare two writable pools, so they compare two durable epochs. If the grep shows any other `read_only` hit, that line is a read-only line: replace it with an explicit expectation, `EXPECT_EQ(<pool>->liveWriterEpoch(), 0u)`, and list it in Deviations.

Also confirm no test sets only one of the two epochs: `grep -rn "setLiveWriterEpochForTest\|setLiveWriterEpoch(" T` must not name any of the 12 files. At HEAD it names `gtest_cas_detached_work.cpp`, `gtest_cas_ref_wedge_every_attempt.cpp` and `gtest_cas_pool.cpp`. `gtest_cas_ref_wedge_every_attempt.cpp` has no `writerEpoch()` line; `gtest_cas_pool.cpp` sets both epochs in one callback, handled below.

- [ ] **Step 8.1.1: Production edits**

`CA/Pool/CasMountRuntime.h`:
- Replace the block (`RH:165-169`)

```cpp
    /// ---- per-server watermark and identity ----
    /// `process_epoch` is random and nonzero for this pool incarnation. GC compares it for equality,
    /// never ordering; a different value means that the previous writer incarnation is no longer live.
    uint64_t epoch() const { return process_epoch.load(std::memory_order_acquire); }
    uint64_t writerEpoch() const { return process_epoch.load(std::memory_order_acquire); }
```

with the single line `    /// ---- per-server watermark ----`.
- Replace the declarations (`RH:375-382`)

```cpp
    /// ---- process epoch (identity) ----
    /// Mint the random nonzero process identity used by GC's equality check.
    void mintRandomProcessEpoch();
    /// Set `process_epoch` to the durable `writer_epoch`. The caller supplies the memory order because
    /// the initial writable claim and a later self-remount have different publication requirements.
    void setProcessEpoch(uint64_t v, std::memory_order order);
    /// Publish the live-incarnation `live_writer_epoch` with release ordering.
    void setLiveWriterEpoch(uint64_t v);
```

with:

```cpp
    /// ---- live writer epoch ----
    /// Publish the live-incarnation `live_writer_epoch` with release ordering.
    void setLiveWriterEpoch(uint64_t v);
```

- Replace the member comment and member (`RH:495-505`, from `/// Per-server build watermark. `process_epoch` is a random` through `std::atomic<uint64_t> process_epoch{0};`) with:

```cpp
    /// Per-server build watermark. `next_build_seq` is a strictly-increasing per-process counter
    /// (monotonicity is load-bearing: a seq is never reused or lowered); `active_build_seqs` holds the
    /// seqs of in-flight builds, so `minActive` yields the GC floor. The floor is published by the
    /// merged `mount_renewer` beat (there is no standalone watermark object).
```

  The lines that follow (`std::mutex builds_mutex;` and on) stay.

`CA/Pool/CasMountRuntime.cpp`: delete `mintRandomProcessEpoch` and `setProcessEpoch` (`R:335-350`) and the include `#include <Common/thread_local_rng.h>`. Check first: `grep -n thread_local_rng CA/Pool/CasMountRuntime.cpp` must show only those two places; if a Task 1 to 4 line uses it, keep the include.

`CA/Pool/CasPool.h`:
- Replace the block (`PH:456-464`, from `/// ---- per-server watermark surface ----` through `uint64_t writerEpoch() const { ... }`) with the heading line `    /// ---- per-server watermark surface ----`; the `minActive` comment and declaration below it stay.
- In the comment of `liveWriterEpoch` (`PH:807`) add a last sentence: `/// Zero on a read-only pool, which claims no mount.`
- In the comment of the `mount_runtime` member (`PH:1260`), replace "(`process_epoch` + the `builds_mutex`-guarded seq/registry)" with "(the `builds_mutex`-guarded seq/registry)".

`CA/Pool/CasPool.cpp`:
- `Pool::open`: delete the comment and call (`P:574-577`)

```cpp
    /// Per-server watermark: mint the random NONZERO `process_epoch`
    /// once per Pool (GC checks it for equality only -- a different epoch == a dead incarnation). The
    /// masking/redraw detail lives in `CasMountRuntime::mintRandomProcessEpoch`.
    store->mount_runtime.mintRandomProcessEpoch();
```

  and the blank line after it.
- `Pool::mountWritable`: the comment of step 3 (`P:636-639`). Before:

```cpp
    /// 3. Durable-monotone writer_epoch — CAS-bump the sticky `epoch` object. THE BRIDGE: this
    ///    durable value REPLACES the random `process_epoch` for identity, so the watermark + every
    ///    manifest ref carries it (the random mint above stays for the read-only
    ///    path, which never reaches here). The epoch-aware sweep reads this value.
```

  After: `    /// 3. Durable-monotone writer_epoch — CAS-bump the sticky `epoch` object.`
- Delete the three lines `store->mount_runtime.setProcessEpoch(writer_epoch, std::memory_order_relaxed);` (after the first `allocateWriterEpoch`, and in the two fence-recovery loops) and the line `mount_runtime.setProcessEpoch(writer_epoch, std::memory_order_release);` in `Pool::tryRemountOnce`.
- `Pool::openForDecommission`: delete the paragraph "No random `process_epoch` mint here: ... default." (`P:990-993`).
- `Pool::tryRemountOnce`, comment of step 1 (`P:1523`): delete the sentence "Keep `process_epoch` (the identity accessors) equal to it." As left by Tasks 1 and 5a this comment may be reworded; delete the sentence wherever the grep finds it.

`CA/Pool/CasPartWriteTxn.h`, member `epoch`: the trailing comment `/// owning Pool's process_epoch` becomes `/// the owning Pool's live writer epoch when the build began`. `Pool::beginPartWrite` passes `liveWriterEpoch()` to the constructor.

`CA/Pool/CasRefLedger.h` (the comment of `allocateRefTxnId`, the "epoch component" paragraph). Before:

```cpp
    /// The epoch component is the live mount incarnation's writer epoch, not the open-time
    /// `process_epoch`: a self-remount allocates a strictly-greater durable writer_epoch, so every ref
```

After:

```cpp
    /// The epoch component is the live mount incarnation's writer epoch: a self-remount allocates a
    /// strictly-greater durable writer_epoch, so every ref
```

- [ ] **Step 8.1.2: Test sweep**

The sweep is one pattern over the 12 files:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1/src/Disks/tests
FILES=$(grep -rl "writerEpoch()" .)
echo "$FILES" | wc -l                       # 12 files at HEAD
grep -rc "liveWriterEpoch()" $FILES | awk -F: '{s+=$2} END {print "before:", s}'
sed -i 's/\bwriterEpoch()/liveWriterEpoch()/g' $FILES
grep -rc "liveWriterEpoch()" $FILES | awk -F: '{s+=$2} END {print "after:", s}'
```

`after` minus `before` must be the number of lines `grep -rn "writerEpoch()"` showed before the sed (64 at HEAD). The `\b` keeps `liveWriterEpoch()` untouched: its `W` is a capital.

`T/gtest_cas_pool.cpp`, the remount callback of `ExternalLossDuringRenewalUsesOneRecoveryGeneration`: delete the line `runtime_ptr->setProcessEpoch(2, std::memory_order_release);`. The next line, `runtime_ptr->setLiveWriterEpoch(2);`, stays.

- [ ] **Step 8.1.3: Gate grep**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && grep -rnE "process_epoch|ProcessEpoch|\bwriterEpoch\(\)|[.>]epoch\(\)" src docs programs utils
```

Expected: no output. (`time_since_epoch()` does not match: no `.` or `>` before `epoch`.) Any hit is a reader the claims table missed: stop and report.

- [ ] **Step 8.1.4: Build, run, commit**

Build `81`. Run `CASDecommission*:CASEvent*:CASFenceGeneration*:CASMount*:CASClaimMount*:CASHeartbeat*:CASPartWrite*:CASRefCatalogBirthWiring*:CASRefWriter*:CASRefCheckpoint*:CASRefContiguousAlloc*:CASRefSnapshotPublishOrdering*:CASWriterDuties*:CASPool*`. Expected: `PASSED`.

Files: `CA/Pool/CasMountRuntime.h`, `CA/Pool/CasMountRuntime.cpp`, `CA/Pool/CasPool.h`, `CA/Pool/CasPool.cpp`, `CA/Pool/CasPartWriteTxn.h`, `CA/Pool/CasRefLedger.h`, the files named by `git diff --name-only -- src/Disks/tests`.
Subject: `Delete process_epoch and the epoch accessors nothing reads`.
Body: `Identity uses the allocated writer epoch and incarnation checks use liveWriterEpoch. The 64 test lines that read writerEpoch read liveWriterEpoch; none runs on a read-only pool, where the value would change from a random number to 0.`

---

## Commit 8.2: drop `terminal_state_published`

A deletion. No red run; `CASPoolRemount.VanishedReasonPreparationFailureLeavesTerminalTransitionRetryable` is the guard and stays untouched: a preparation failure before the lock leaves the lifecycle non-terminal, a second call completes, a third with another `Vanished*` value is ignored. The `Vanished*` lifecycle is stored last under `driver_mutex`, so testing `isVanished` is the same predicate.

- [ ] **Step 8.2.1: Edit**

`CasMountRuntime::enterVanished` (`R`): replace the early return

```cpp
    if (terminal_state_published.load(std::memory_order_acquire))
        return;
```

by `    if (isVanished())\n        return;`. Replace the comment and the guard inside the locked block. Before:

```cpp
    /// The guard is published LAST. Reason preparation above is the only potentially-throwing step;
    /// once this block begins, the statically-proven-noexcept move and atomic stores either publish
    /// one complete terminal transition or observe that an earlier transition already completed.
    {
        auto lock = lockTerminalPublication();
        if (!terminal_state_published.load(std::memory_order_acquire))
        {
```

After:

```cpp
    /// The lifecycle store is the LAST step. Reason preparation above is the only potentially-throwing
    /// step; once this block begins, the statically-proven-noexcept move and atomic stores either
    /// publish one complete terminal transition or observe that an earlier transition already completed.
    {
        auto lock = lockTerminalPublication();
        if (!isVanished())
        {
```

Delete the line `terminal_state_published.store(true, std::memory_order_release);` after `pool_lifecycle.store(which, std::memory_order_release);`.

`CasMountRuntime::setLifecycleForTest`: delete the comment "Keep the terminal-state guard consistent with the forced state, ..." and the store `terminal_state_published.store(true, std::memory_order_release);`. The braces around the `Vanished*` branch then hold only `vanished_intent.store(true, ...)`; keep them.

`CA/Pool/CasMountRuntime.h`: delete the member `terminal_state_published` with its five-line comment ("Idempotency guard for the terminal STATE transition ..."). In the comment of `enterVanished` replace "(a dedicated latch keyed separately from `vanished_intent`, because FORGET publishes that intent latch early at step 1)" with "(keyed on the `Vanished*` lifecycle value, not on `vanished_intent`, because FORGET publishes that intent early at step 1)".

- [ ] **Step 8.2.2: Gate grep, build, run, commit**

`grep -rn "terminal_state_published" src docs` returns nothing. Build `82`. Run `CASPoolRemount*:CASMountRuntime*:CASLifecycle*:CASForget*:CASOperationGate*`. Expected: `PASSED`.
Files: `CA/Pool/CasMountRuntime.h`, `CA/Pool/CasMountRuntime.cpp`.
Subject: `Test the Vanished lifecycle instead of a shadow flag in enterVanished`.
Body: `terminal_state_published repeated what the Vanished lifecycle values already say. enterVanished and setLifecycleForTest test isVanished under driver_mutex; the publication order is unchanged.`

---

## Commit 8.3: one source for the renewal period and the boot clock

A signature change plus a sweep. No red run.

- [ ] **Step 8.3.1: Production edits**

`CA/Pool/CasMountRuntime.h`: `void startBackgroundWorkers();` replaces the declaration with the `period` parameter (adjust its comment: the period is `config.mount_renew_period`). Delete the member `std::chrono::milliseconds renewal_period{0};`.

`CA/Pool/CasMountRuntime.cpp`: in `startBackgroundWorkers` delete the line `renewal_period = period;` and the parameter. In the loop body, as left by Task 1, the due-time computation reads `renewal_period`. Before:

```cpp
const uint64_t period_ms = static_cast<uint64_t>(std::max<int64_t>(0, renewal_period.count()));
```

After:

```cpp
const uint64_t period_ms = static_cast<uint64_t>(std::max<int64_t>(0, config.mount_renew_period.count()));
```

`CA/Pool/CasPool.cpp`, `Pool::mountWritable`: `store->mount_runtime.startBackgroundWorkers(store->config.mount_renew_period);` becomes `store->mount_runtime.startBackgroundWorkers();`. `PoolConfig::mountConfig` copies `mount_renew_period` into the `MountConfig`, so the value is the same.

`CA/Pool/CasServerRoot.h`: delete `std::function<uint64_t()> boot_ms;` from `MountRenewOperationEnvironment`. `CA/Pool/CasMountRuntime.cpp`: delete `.boot_ms = [this] { return bootMsNow(); },` from `renewalEnvironment` (as left by Task 5b it has no `worker_call` / `policy`). `CA/Pool/CasServerRoot.cpp`, `MountLeaseRenewer::renew` (the one renewal step, `renewOn` at HEAD): delete `const auto boot_clock = environment.boot_ms ? environment.boot_ms : boot_ms_fn;` and replace each `boot_clock()` with `boot_ms_fn()`. The renewer's `boot_ms_fn` is wired to `bootMsNow` by `installRenewer`, the same function the environment carried.

- [ ] **Step 8.3.2: Sweep `startBackgroundWorkers` in the tests**

Every call in `T/gtest_cas_pool.cpp` passes a period, and every test builds a `MountConfig` whose default `mount_renew_period` is 10 s. A call `startBackgroundWorkers(P)` becomes `startBackgroundWorkers()` and the same `P` goes to `.mount_renew_period = P` in the test's `MountConfig` (designated initializers: right after `.mount_lease_ttl_ms`, which is declared before it).

Tests that Task 1 deletes (`DirectRenewCannotRaceWorkerStartOrRenewerReplacement`, `DueWorkerAdmissionIsReservedBeforeParkRequest`, `DueWorkerAdmissionIsReservedBeforeStop`, `TerminalDepositionDoesNotTouchRenewerAfterReplacement`) and tests that Tasks 1 to 5 add are covered by the same rule: the script reads the tree, not a list.

Save as `tmp/t5_sweep_period.py` in the worktree's `tmp` directory (do not commit it) and run it from the worktree root:

```python
import pathlib, re, sys

path = pathlib.Path("src/Disks/tests/gtest_cas_pool.cpp")
text = path.read_text()
chunks = re.split(r"(?m)^(?=TEST(?:_F)?\()", text)
call_re = re.compile(r"startBackgroundWorkers\(\s*(std::chrono::[a-z]+\([^()]*\))\s*\)")
cfg_re = re.compile(r"(\.mount_lease_ttl_ms\s*=\s*std::chrono::milliseconds\([^()]*\),)")
failed = False
swept_calls = 0
for index, chunk in enumerate(chunks):
    values = set(call_re.findall(chunk))
    if not values:
        continue
    name = chunk.split("\n", 1)[0]
    if len(values) != 1:
        print("MANUAL (several periods in one test):", name, sorted(values))
        failed = True
        continue
    value = values.pop()
    swept_calls += len(call_re.findall(chunk))
    chunk, edits = cfg_re.subn(lambda m: m.group(1) + " .mount_renew_period = " + value + ",", chunk)
    if edits == 0:
        print("MANUAL (no MountConfig with mount_lease_ttl_ms):", name)
        failed = True
        continue
    chunks[index] = call_re.sub("startBackgroundWorkers()", chunk)
if failed:
    print("fix the MANUAL tests by hand, then re-run: the script is idempotent on the rest")
path.write_text("".join(chunks))
print("calls swept:", swept_calls)
sys.exit(1 if failed else 0)
```

Hand edits the script reports at HEAD: `CASPoolShutdown.PreSendCancellationAllowsFarewellButAmbiguityDoesNot` starts workers with `0 ms` in one branch and `1 h` in the other, from one `MountConfig` inside the `run` lambda. Write `.mount_renew_period = std::chrono::milliseconds(ambiguous ? 0 : 3'600'000),` after `.mount_lease_ttl_ms = std::chrono::milliseconds(1000),` and call `runtime.startBackgroundWorkers();` in both branches. A test that starts workers through a `MountConfig` built outside the test (a helper) is also reported: set the period in the helper's parameter list.

Check the result: `git diff --stat -- src/Disks/tests/gtest_cas_pool.cpp` shows only call lines and `MountConfig` lines.

- [ ] **Step 8.3.3: Drop the boot clock from the heartbeat helper**

`T/gtest_cas_heartbeat.cpp`, `renewalEnvironment` (as left by Task 5b it has no `policy`). The `boot_ms` parameter becomes unused once `.boot_ms` goes, so it goes too. New helper:

```cpp
MountRenewOperationEnvironment renewalEnvironment(
    const std::function<bool()> & live = {},
    const std::function<bool()> & cancelled = {})
{
    return MountRenewOperationEnvironment{
        .live = live,
        .cancelled = cancelled,
        .on_request = {},
    };
}
```

Sweep the call sites (single and multi-line forms, including the fixture's `renewalEnvironment(f.boot_ms)`):

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && python3 - <<'PY'
import pathlib, re
path = pathlib.Path("src/Disks/tests/gtest_cas_heartbeat.cpp")
text = path.read_text()
text, n = re.subn(r"renewalEnvironment\(\s*(?:\w+\.)?boot_ms\s*(?:,\s*)?", "renewalEnvironment(", text)
path.write_text(text)
print("call sites rewritten:", n)
PY
grep -nE "renewalEnvironment\(\s*(\w+\.)?boot_ms" src/Disks/tests/gtest_cas_heartbeat.cpp
```

The count is the number of call sites at the time (a few dozen); the last grep must print nothing. The locals `boot_ms` stay: each renewer is constructed with `[&] { return boot_ms; }`.

- [ ] **Step 8.3.4: Gate grep, build, run, commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && grep -rnE "startBackgroundWorkers\([^)]" src; grep -rn "renewal_period\|environment\.boot_ms\|boot_clock" src/Disks
```

Both print nothing. Build `83`. Run `CASPoolRemount*:CASPoolShutdown*:CASMountRuntime*:CASPool.*:CASHeartbeat*:CASMountAudit*:CASEvent*:CASMountLease*`. Expected: `PASSED`.
Files: `CA/Pool/CasMountRuntime.h`, `CA/Pool/CasMountRuntime.cpp`, `CA/Pool/CasPool.cpp`, `CA/Pool/CasServerRoot.h`, `CA/Pool/CasServerRoot.cpp`, `T/gtest_cas_pool.cpp`, `T/gtest_cas_heartbeat.cpp`.
Subject: `Read the renewal period and the boot clock from one place`.
Body: `startBackgroundWorkers reads MountConfig::mount_renew_period instead of keeping a copy of its argument. MountRenewOperationEnvironment loses boot_ms: the renewer already holds the same clock as boot_ms_fn. Tests set the period in their MountConfig.`

---

## Commit 8.4: `runRoundLogged` takes only the trigger

A signature change plus a sweep. No red run. At HEAD `runOneRoundNow` has 22 call lines in 4 files; 3 of them (`gtest_cas_gc_stop_start.cpp`) already pass no argument, so 19 lose one.

- [ ] **Step 8.4.1: Production edits**

`CA/Gc/CasGcScheduler.h`:

```cpp
    /// Run ONE manual round synchronously on the caller's thread. Returns the round report; emits a
    /// Start + Finish record. A manual round acquires a free lease or renews its own but never steals a
    /// live incumbent's: dead-incumbent recovery stays the loop's job.
    Cas::RoundReport runOneRoundNow();
```

replaces the comment and declaration `runOneRoundNow(GcRoundLogRecord::Trigger trigger = ...)`. In the private section, replace the last two lines of the `runRoundLogged` comment and its declaration. Before:

```cpp
    /// `allow_steal` is forwarded to `Cas::Gc::runRegularRound` verbatim (see its doc comment).
    Cas::RoundReport runRoundLogged(Cas::Gc & round_gc, GcRoundLogRecord::Trigger trigger,
                                     std::function<void()> on_lease_acquired = {}, bool allow_steal = true);
```

After:

```cpp
    /// A `Scheduled` round may steal an incumbent's lease; a `Manual` one never does (see
    /// `Cas::Gc::runRegularRound`).
    Cas::RoundReport runRoundLogged(Cas::Gc & round_gc, GcRoundLogRecord::Trigger trigger,
                                     std::function<void()> on_lease_acquired = {});
```

`CA/Gc/CasGcScheduler.cpp`: the definition loses the parameter and derives it, right after the `using Rec` line:

```cpp
Cas::RoundReport CasGcScheduler::runRoundLogged(Cas::Gc & round_gc, GcRoundLogRecord::Trigger trigger,
                                                 std::function<void()> on_lease_acquired)
{
    using Rec = GcRoundLogRecord;
    const bool allow_steal = trigger == Rec::Trigger::Scheduled;
```

`runOneRoundNow`:

```cpp
Cas::RoundReport CasGcScheduler::runOneRoundNow()
{
    std::lock_guard round_lock(gc_round_mutex);
    const Cas::RoundReport report = runRoundLogged(gc, GcRoundLogRecord::Trigger::Manual, [this] { onLeaseAcquired(); });
    i_am_leader.store(report.acquired_lease, std::memory_order_relaxed);
    return report;
}
```

Its comment block ("allow_steal=false: a manual round ...") is deleted; the header comment above carries the reason. In `loop`, the comment ending "allow_steal defaults to true here (the loop is the ONLY caller allowed to execute the steal CAS)." becomes "A `Scheduled` round is the only one that may execute the steal CAS."

`CA/ContentAddressedMetadataStorage.cpp`: `return snapshot->runOneRoundNow(Cas::GcRoundLogRecord::Trigger::Manual);` becomes `return snapshot->runOneRoundNow();`.

- [ ] **Step 8.4.2: Sweep**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1/src/Disks/tests
FILES=$(grep -rl "runOneRoundNow(" .)
grep -rcE "runOneRoundNow\((Rec|GcRoundLogRecord)::Trigger::Manual\)" $FILES | awk -F: '{s+=$2} END {print "to rewrite:", s}'   # 19 at HEAD
sed -i -E 's/runOneRoundNow\((Rec|GcRoundLogRecord)::Trigger::Manual\)/runOneRoundNow()/g' $FILES
grep -rnE "runOneRoundNow\([^)]" . ../DiskObjectStorage
```

The last grep must print nothing. The file-scope `using Rec = ...` in `gtest_cas_gc_log.cpp` stays used by the other tests of the file.

- [ ] **Step 8.4.3: Prove the derived `allow_steal` is pinned (one-off probe)**

`CASGCSchedulerSteal.ManualRoundNeverStealsEvenADeadIncumbent` fails if a manual round may steal. Prove it with a mutation, run under the build lock so no other agent builds the mutated file:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && F=src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp && flock build/.wok-build.lock bash -c '
cp '"$F"' build/t5_84_backup.cpp
trap "cp build/t5_84_backup.cpp '"$F"'" EXIT
sed -i "s/const bool allow_steal = trigger == Rec::Trigger::Scheduled;/const bool allow_steal = true;/" '"$F"'
grep -c "const bool allow_steal = true;" '"$F"'
ninja -C build unit_tests_dbms > build/t5_84_probe_build.log 2>&1; echo NINJA_EXIT=$? >> build/t5_84_probe_build.log
( ulimit -v 33554432; timeout 1200 build/src/unit_tests_dbms --gtest_filter="CASGCSchedulerSteal.*" > build/t5_84_probe_run.log 2>&1; echo GATE_EXIT=$? >> build/t5_84_probe_run.log )
'
cmp build/t5_84_backup.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp && echo restored
tail -1 build/t5_84_probe_build.log; grep -a "FAILED\|PASSED\|GATE_EXIT" build/t5_84_probe_run.log | tail
```

Expected: `1` from the `grep -c`, `restored`, `NINJA_EXIT=0`, `ManualRoundNeverStealsEvenADeadIncumbent` listed as `FAILED`. Paste the lines. If it passes, the unit tests do not pin the derivation: report that and keep the probe's result in Review focus. The trap restores the file from the backup, which holds this commit's uncommitted edits; that is why the check is `cmp`, not `git diff`. After the probe, rebuild (`84`) before the gate.

- [ ] **Step 8.4.4: Build, then the whole gate, then commit**

Build `84`, run the gate (`CAS*`, id `84_gate`). Expected: `PASSED`.
Files: `CA/Gc/CasGcScheduler.h`, `CA/Gc/CasGcScheduler.cpp`, `CA/ContentAddressedMetadataStorage.cpp`, and the four test files `git diff --name-only -- src/Disks/tests` lists.
Subject: `Derive allow_steal from the round trigger in CasGcScheduler`.
Body: `runRoundLogged took both a trigger and allow_steal, and the two always agreed: scheduled rounds steal, manual rounds never do. runOneRoundNow always ran a manual round, so it loses its parameter.`

---

## Commit 8.5: one predicate for "background work must stop"

A deletion. No red run.

The GC scheduler repeats `isVanished() || vanishedIntentPublished() || lifecycle() == IdentityLost`. `CasMountRuntime::remountTerminal` is `vanished_intent || lifecycle() == IdentityLost`. Both `Vanished*` lifecycle values store `vanished_intent` first (`enterVanished`) or in the same call (`setLifecycleForTest`), so the first term is subsumed.

- [ ] **Step 8.5.1: Edit**

Check the premise: `grep -n "pool_lifecycle.store" CA/Pool/CasMountRuntime.cpp` names `setLifecycleForTest` and `enterVanished` only.

`CA/Pool/CasPool.h`: replace the declaration and comment of `vanishedIntentPublished` (and nothing else of the block above it):

```cpp
    /// Whether background work must stop: a terminal intent is published (a natural `enterVanished` or
    /// FORGET's early publication) or the pool is `IdentityLost`. See `CasMountRuntime::remountTerminal`.
    bool remountTerminal() const { return mount_runtime.remountTerminal(); }
```

`Pool::vanishedIntentPublished` has no caller once the scheduler changes (`grep -rn "vanishedIntentPublished()" src | grep -v mount_runtime` shows only the two scheduler loops and one test that calls it on a runtime), so it goes.

`CA/Gc/CasGcScheduler.cpp`, `loop` and `heartbeatLoop`: both conditions become `if (store->remountTerminal())`. The long comment in `loop` keeps its reasons; replace its sentence "We also exit on a published FORGET intent (`vanishedIntentPublished`, still pre-terminal) — earliest-signal discipline — and on `IdentityLost`" with "`remountTerminal` is true from the moment FORGET publishes its intent (still pre-terminal) and on `IdentityLost`".

- [ ] **Step 8.5.2: Gate grep, build, run, commit**

`grep -rn "vanishedIntentPublished()" src` must show the runtime's own definition and uses and the one test line on a runtime. Build `85`. Run `CASGCStopStart*:CASGCTeardownStop*:CASForget*:CASLifecycle*:CASOperationGate*:CASGCScheduler*:CASGCHealth*`. Expected: `PASSED`.
Files: `CA/Pool/CasPool.h`, `CA/Gc/CasGcScheduler.cpp`.
Subject: `Use remountTerminal in the GC scheduler loops`.
Body: `The scheduler repeated the three conditions of CasMountRuntime::remountTerminal. A Vanished lifecycle always has the intent published, so the isVanished term was redundant. Pool::vanishedIntentPublished had no other caller and goes.`

---

## Commit 8.6: drop the write-only fields

A deletion plus sweeps. No red run.

**Read the claims table again.** All five are checked there. The deviation from the contract's test impact: removing the `BackendPtr` constructor parameter also edits `Pool`'s initializer and `RuntimeUnderTest`.

- [ ] **Step 8.6.1: Production edits**

`CA/Pool/CasMountRuntime.h`:
- `struct MountFence`: delete `UInt128 server_uuid{};` and `uint64_t writer_epoch = 0;`.
- Delete the member `BackendPtr backend_ptr;` and the constructor parameter `BackendPtr backend_ptr_,`.
- `void armMountFence(uint64_t deadline_boot_ms);`
- In the class comment replace "The runtime receives its backend, layout, configuration" with "The runtime receives its request planes, layout, configuration".

`CA/Pool/CasMountRuntime.cpp`: constructor: delete the parameter and the initializer `backend_ptr(std::move(backend_ptr_))`; the next initializer (`mount_requests` or, as left by Task 5b, `farewell_requests`) takes its place as the first in the list (`: farewell_requests(farewell_requests_)`). `armMountFence`: signature `(uint64_t deadline_boot_ms)`; delete the two stores `mount_fence.server_uuid = server_uuid;` and `mount_fence.writer_epoch = writer_epoch;`. As left by Task 1 the function may be `armFence(deadline_boot_ms, /*report_live=*/false)` preceded by those stores: delete them wherever the grep finds them.

`CA/Pool/CasPool.h` and `CA/Pool/CasPool.cpp`: `Pool::armMountFence(uint64_t deadline_boot_ms)` forwards `mount_runtime.armMountFence(deadline_boot_ms)`. Delete `CasRequests & farewellRequests() { return farewell_requests; }` with its comment. In the `Pool` constructor the `mount_runtime(...)` initializer drops its first argument `pool_backend`. In the comment of the `mount_runtime` member delete "backend/" from "Injected with backend/layout".

`CA/Pool/CasServerRoot.h`: delete `bool sent_any = false;` from `MountRenewResult` (and any comment that says it is removed here). `CA/Pool/CasServerRoot.cpp`: delete each `result.sent_any = ...;` line in `MountLeaseRenewer::renew` (four at HEAD).

If a production call of `armMountFence` with three arguments remains (`grep -n "armMountFence(" CA/Pool/CasPool.cpp CA/Pool/CasMountRuntime.cpp`; Tasks 1 and 5a should have replaced both), change it to the one-argument form.

- [ ] **Step 8.6.2: Sweep `armMountFence` in the tests**

All 44 code lines have the shape `armMountFence(<uuid>, <epoch>, <deadline>)` with `uuid`, `kUuid` or `DB::UInt128{0, 1}` first. One substitution:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1/src/Disks/tests
FILES=$(grep -rlE "armMountFence\(" .)
grep -rcE "armMountFence\((DB::UInt128\{0, 1\}|uuid|kUuid), [^,]+, " $FILES | awk -F: '{s+=$2} END {print "to rewrite:", s}'
sed -i -E 's/armMountFence\((DB::UInt128\{0, 1\}|uuid|kUuid), [^,]+, /armMountFence(/' $FILES
grep -rnE "armMountFence\(.*," .
```

The count printed is the number of three-argument lines (44 at HEAD, the shared helper `rearmMountFenceAfterAnomalyForTest` in `cas_test_helpers.h` included); the last grep must print nothing. Locals that were only arguments are still used elsewhere in each test (`uuid` by `installRenewer`, `kUuid` by the fixture, `writer_epoch` by `setLiveWriterEpochForTest`); the build reports any that are not.

`T/gtest_cas_pool.cpp`, `RuntimeUnderTest`: `runtime(backend, mount, farewell, lease, std::forward<Args>(args)...)` (as left by Task 5b: `runtime(backend, farewell, lease, ...)`) becomes `runtime(farewell, lease, std::forward<Args>(args)...)`. `backend` is still used by the three planes.

`sent_any` on `MountRenewResult`, three test lines. `attempts_sent` is set for every ending that sent, and `GaveUp::sent_any` is exactly `attempts_sent != 0`:
- `T/gtest_cas_heartbeat.cpp`, `CASHeartbeat.CancellationAfterSendIsTerminalAndForbidsRelease`: `EXPECT_TRUE(result.sent_any);` becomes `EXPECT_GE(result.attempts_sent, 1u);`. The test named `DeadlineBeforeSendTerminalizesWithTypedFailure` is deleted by Task 5b; if it is still there, `EXPECT_FALSE(result.sent_any);` becomes `EXPECT_EQ(result.attempts_sent, 0u);`.
- `T/gtest_cas_mount.cpp`, the renewer that is refused by a lost fence: `EXPECT_FALSE(refused.sent_any);` becomes `EXPECT_EQ(refused.attempts_sent, 0u);`.

- [ ] **Step 8.6.3: Gate grep, build, whole gate, commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && grep -rnE "mount_fence\.(server_uuid|writer_epoch)|\bbackend_ptr\b|farewellRequests|result\.sent_any|refused\.sent_any" src
```

Prints nothing. (`gave_up->sent_any` hits are `GaveUp` and are not in this pattern.) The shared helper header `cas_test_helpers.h` reaches nearly every CAS test, so run the whole gate: build `86`, gate id `86_gate`, expected `PASSED`.
Files: `CA/Pool/CasMountRuntime.h`, `CA/Pool/CasMountRuntime.cpp`, `CA/Pool/CasPool.h`, `CA/Pool/CasPool.cpp`, `CA/Pool/CasServerRoot.h`, `CA/Pool/CasServerRoot.cpp`, and the test files `git diff --name-only -- src/Disks/tests` lists.
Subject: `Remove the write-only fence, runtime and renewal result fields`.
Body: `MountFence::server_uuid and writer_epoch were stored on every arm and never read; backend_ptr, Pool::farewellRequests and MountRenewResult::sent_any had no readers either. armMountFence takes the deadline only. Tests that read sent_any read attempts_sent.`

---

## Commit 8.7: drop the duplicate observation log line

A log-line removal. No unit test reads either text; one integration test pins one of them.

At the start of an observation, `claimMountAwaitingExpiry` calls `on_wait_start` and then logs. The opening caller's `on_wait_start` (`Pool::mountWritable`) already logs the holder's identity and the threshold ("a stale-looking mount lease is held by uuid=... observing its write-token for up to ~N ms before reclaiming"), and the remount caller's does the same ("observing a stale-looking mount's write-token ... for up to ~N ms before reclaiming"). The line inside `claimMountAwaitingExpiry` ("... waiting ~N ms (token-stability observation) ...") repeats the threshold and nothing else, so it is the one that goes.

`tests/integration/test_cas_mount_renewal_retry/test.py`, test `test_hard_restart_observes_then_the_unsafe_knob_skips_the_observation`, reads the removed text (`observation = "waiting ~{} ms (token-stability observation)"`, asserted `== 1` after a hard restart and `== 0` after the unsafe-knob restart).

- [ ] **Step 8.7.1: Edit**

`CA/Pool/CasServerRoot.cpp`, `claimMountAwaitingExpiry`: delete

```cpp
            LOG_INFO(getLogger("CasMountLease"),
                "Attempting to mount content-addressed server root {} after node change or hard "
                "restart; waiting ~{} ms (token-stability observation) to confirm the previous "
                "incarnation's operations are all finalized", srid, threshold_ms);
```

After the deletion the block reads `watch = TokenWatch::sighted(...)`, then `if (on_wait_start && r.body) on_wait_start(*r.body, threshold_ms);`.

`tests/integration/test_cas_mount_renewal_retry/test.py`: replace the line

```python
    observation = "waiting ~{} ms (token-stability observation)".format(threshold_ms)
```

with

```python
    observation = "observing its write-token for up to ~{} ms before reclaiming".format(threshold_ms)
```

The pattern is a literal fragment of the opening caller's line; it contains no regex or shell metacharacters that `grep -c -- '%s'` treats specially (`~` is literal).

- [ ] **Step 8.7.2: Gate grep, build, run, commit**

`grep -rn "token-stability observation) to confirm\|Attempting to mount content-addressed" src docs tests` prints nothing. Build `87`. Run `CASMountStartup*:CASMountAwaitExpiry*:CASMountObservation*:CASPool.*`. Expected: `PASSED`.

Run the integration test once (it takes minutes): `cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && python3 -m ci.praktika run "integration" --test test_cas_mount_renewal_retry/test.py::test_hard_restart_observes_then_the_unsafe_knob_skips_the_observation > build/t5_87_integration.log 2>&1; tail -20 build/t5_87_integration.log`. It needs a `clickhouse` binary built from this tree: if none is available, do not claim it ran; state "integration test not run" in the report and let the controller decide.

Files: `CA/Pool/CasServerRoot.cpp`, `tests/integration/test_cas_mount_renewal_retry/test.py`.
Subject: `Log the stale-mount observation once per window start`.
Body: `claimMountAwaitingExpiry logged the threshold right after the opening and remount callers had logged it with the holder's identity. The integration test counts the caller's line.`

---

## Commit 8.8: replace `setRoundInFlightForTest` by assertions over a parked real round

A test change plus a deletion. The new assertions have a red run against a mutated scheduler.

`CASGCTeardownStop.BackgroundRoundIsCutAtItsNextRequest` (`T/gtest_cas_gc_teardown_stop.cpp`) already parks a real background round inside a request (`ParkFirstReadBackend`, `entered`) and ends with `EXPECT_TRUE(sched.isQuiescent())` after `stop`. `CASOperationGate.GcSchedulerIsQuiescentReflectsRoundInFlight` forces the flag by the seam. Two assertions go into the real test, the forced one goes away.

- [ ] **Step 8.8.1: Add the assertions**

In `BackgroundRoundIsCutAtItsNextRequest`, before `sched.start();`:

```cpp
    EXPECT_TRUE(sched.isQuiescent()) << "no round has started yet";
```

and after `entered->wait("entered");`:

```cpp
    EXPECT_FALSE(sched.isQuiescent()) << "a round parked inside a request must not read as GC-quiescent";
```

The existing `EXPECT_TRUE(sched.isQuiescent());` after `sched.stop()` stays: it is the third state of the old test.

Build `88a`. Run `CASGCTeardownStop.BackgroundRoundIsCutAtItsNextRequest`. Expected: `PASSED` (the seam still exists).

- [ ] **Step 8.8.2: Show the assertion fails when the flag is not set (one-off probe)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1 && F=src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp && flock build/.wok-build.lock bash -c '
cp '"$F"' build/t5_88_backup.cpp
trap "cp build/t5_88_backup.cpp '"$F"'" EXIT
sed -i "s/^    round_in_flight.store(true, std::memory_order_release);/    (void)0;/" '"$F"'
grep -c "^    (void)0;" '"$F"'
ninja -C build unit_tests_dbms > build/t5_88_probe_build.log 2>&1; echo NINJA_EXIT=$? >> build/t5_88_probe_build.log
( ulimit -v 33554432; timeout 1200 build/src/unit_tests_dbms --gtest_filter="CASGCTeardownStop.BackgroundRoundIsCutAtItsNextRequest" > build/t5_88_probe_run.log 2>&1; echo GATE_EXIT=$? >> build/t5_88_probe_run.log )
'
cmp build/t5_88_backup.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp && echo restored
tail -1 build/t5_88_probe_build.log; grep -a "a round parked\|FAILED\|PASSED\|GATE_EXIT" build/t5_88_probe_run.log | tail
```

Expected: `1`, `NINJA_EXIT=0`, the message "a round parked inside a request must not read as GC-quiescent" under `FAILED`, then `restored`. Paste the lines.

- [ ] **Step 8.8.3: Delete the seam and the forced test**

`T/gtest_cas_operation_gate.cpp`: delete `CASOperationGate.GcSchedulerIsQuiescentReflectsRoundInFlight` with the two comment lines above it ("(i) `CasGcScheduler::isQuiescent` reflects ..."). `CA/Gc/CasGcScheduler.h`: delete `setRoundInFlightForTest` with its two-line comment.

`grep -rn "setRoundInFlightForTest" src docs` prints nothing. Build `88`. Run `CASGCTeardownStop*:CASOperationGate*:CASGCStopStart*:CASForget*`. Expected: `PASSED`; the deleted test is no longer listed.

- [ ] **Step 8.8.4: Commit**

Files: `CA/Gc/CasGcScheduler.h`, `T/gtest_cas_gc_teardown_stop.cpp`, `T/gtest_cas_operation_gate.cpp`.
Subject: `Assert GC quiescence over a parked real round`.
Body: `CASGCTeardownStop.BackgroundRoundIsCutAtItsNextRequest parks a real round and now checks isQuiescent before the start and while parked. The test that forced round_in_flight through setRoundInFlightForTest and the seam are deleted.`

---

## Commit 8.9: rename the renewer's `open_requests`

A rename. No red run. The renewer's `open_requests` is the plane of the claim and the farewell; the pool's "open plane" is the GC plane, so the old name misleads.

- [ ] **Step 8.9.1: Edit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool
grep -n "open_requests" CasServerRoot.h CasServerRoot.cpp          # constructor parameter, member, uses, comments
sed -i 's/\bopen_requests_\b/claim_farewell_requests_/g; s/\bopen_requests\b/claim_farewell_requests/g' CasServerRoot.h CasServerRoot.cpp
grep -rn "open_requests" /home/mfilimonov/workspace/ClickHouse/cas-95-1/src/Disks/DiskObjectStorage
```

The last grep prints nothing. The local variables named `open_requests` in `gtest_cas_mount.cpp` and `gtest_cas_slot_occupy.cpp` are test locals, not the renewer's member: leave them. Constructor calls are positional and need no edit.

Comments that call the renewer's plane the "open plane" or "an open one": `grep -n "open plane\|an open one" CasServerRoot.h CasServerRoot.cpp CasMountRuntime.h CasMountRuntime.cpp`. For each hit that names the claim-and-farewell plane of the renewer, replace "open plane" by "claim and farewell plane" (as left by Task 5b the `renewForRemount` comment that held one is gone). Hits in `CasPool.cpp`, `CasPool.h` and `ContentAddressedMetadataStorage.cpp` name the pool's GC plane and stay.

- [ ] **Step 8.9.2: Build, run, commit**

Build `89`. Run `CASHeartbeat*:CASMountLease*:CASMountAudit*:CASPoolRemount*:CASPoolShutdown*`. Expected: `PASSED`.
Files: `CA/Pool/CasServerRoot.h`, `CA/Pool/CasServerRoot.cpp`, and `CA/Pool/CasMountRuntime.h` / `CasMountRuntime.cpp` if a comment changed.
Subject: `Name the renewer's claim and farewell plane for what it is`.
Body: `MountLeaseRenewer::open_requests is the plane of the claim and the farewell. It is now claim_farewell_requests; the pool's open plane is the GC plane.`

---

## Final gate

- [ ] **Step F.1: Whole gate on the final tree**

Build `final`; run the gate (`CAS*`, id `final_gate`). Expected: `PASSED`, `GATE_EXIT=0`, no `FAILED` line.

- [ ] **Step F.2: Check the history and the tree**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 log --oneline -12
git -C /home/mfilimonov/workspace/ClickHouse/cas-95-1 status --short -- src docs tests
```

Twelve commits in the order 7a, 7b, 7c, 8.1 to 8.9. The status prints nothing for the files of this task. Style: Allman braces in every C++ hunk above; `git diff -U0 <first>^..HEAD -- '*.cpp' '*.h' | grep -nE "^\+.*\) *\{ *$"` should show no new brace-on-the-same-line control statement.

- [ ] **Step F.3: Report**

Report the 12 commit hashes, the last gate line, the two probe results (8.4, 8.6's none, 8.8), whether the integration test ran, and the Deviations below.

---

**Deviations from the contract**

1. **8.4 call-line count.** The contract says 22 `runOneRoundNow(` lines lose the argument. At HEAD 22 lines call it but 3 (`gtest_cas_gc_stop_start.cpp`) already pass none, so 19 change. The sweep command prints its own count.
2. **8.7 test impact.** The contract found no test impact. `test_cas_mount_renewal_retry/test.py` counts the text of the line inside `claimMountAwaitingExpiry` (`== 1` after a hard restart, `== 0` after the unsafe-knob restart). The plan drops that line, which carries only the threshold, and retargets the test to the opening caller's line, which carries the holder's identity and the threshold. The alternative, keeping the generic line and dropping the callers', loses the holder's identity at open and remount.
3. **8.1 read-only lines.** Ruling O11 expects read-only lines to be rewritten. None of the 64 lines runs on a read-only pool; the plan gives the grep that proves it and the replacement to use if one is found at execution.
4. **7a extra test.** `CASRequestBudget.WriteAndSettlementReadIsTwoEnvelopesAndSaturates` is not in the contract. It pins the new method, so the extraction has a red run.
5. **7c list.** The contract speaks of six timing comments and two setting descriptions. The read found: the `lease_safety_margin_ms` member comment, setting description, `ContentAddressedMetadataStorage.h` comment and docs row (one fact, four places); the grace-period paragraph of `validateCasRequestBudget` and the `waitSleep` sentence; the `PoolConfig::mount_lease_ttl_ms` comment; the unclean-boundary marker and unclean-epoch high-water-mark in two class comments; the `forgetDisk` step (2) comment. The plan carries no count. The docs row is an addition to the contract's file list. Texts that state a deadline, a period or an envelope and are right (`admit`, `refAppendFenceOk`, `mountObservationThresholdMs`, `claimMountAwaitingExpiry`) were read and left.
6. **8.1 comment fixes.** `CasPartWriteTxn.h` and `CasRefLedger.h` name `process_epoch`; the plan edits them so no comment points at a deleted symbol.
7. **8.5 extra deletion.** `Pool::vanishedIntentPublished` loses its last caller and is deleted with the scheduler change.
8. **8.6 test impact.** The `BackendPtr` constructor parameter also changes the `Pool` initializer and `RuntimeUnderTest`; `sent_any` is replaced by `attempts_sent` in three test lines (one is already deleted by Task 5b). The whole gate runs after this commit because `cas_test_helpers.h` changes.
9. **8.8 placement.** The two assertions go to `CASGCTeardownStop.BackgroundRoundIsCutAtItsNextRequest`, the existing test that parks a real round; the contract names only the two lines in `gtest_cas_operation_gate.cpp`. The test that used them is deleted.
10. **8.9 scope.** Only the renewer's member, parameter and the comments that describe the renewer's plane change. The pool's "open plane" wording in `CasPool.*` is left.

**Review focus**

1. **A scheduled round still steals.** 8.4 derives `allow_steal` from the trigger. A unit test pins that a manual round never steals; none at the scheduler level pins that a `Scheduled` round does (the loop-driven steal tests run `Cas::Gc` directly). A reviewer should read the one derived line, and the soak should show a dead incumbent's lease taken over by the loop.
2. **The operator's log grep after 8.7.** Anyone who greps the old text "waiting ~N ms (token-stability observation)" finds nothing. Expected: one line per observation start at an open ("a stale-looking mount lease is held by ...") and one per remount, each with the threshold.
3. **`liveWriterEpoch` on a read-only pool.** It is 0, where `writerEpoch` was random. Expected: no caller treats it as an identity; a test that wants "this pool has no writer epoch" says `EXPECT_EQ(pool->liveWriterEpoch(), 0u)`, and nothing compares a read-only pool's epoch for inequality against a writable one.

---

## Self-Review

**Spec coverage.** Step 4 and rules 13, 14 (reclaim side): Task 1. Step 5, rule 1, rule 7 as written, rule 14 (renewal side), spec test 14: Tasks 2 and 3. Step 6: Task 4. Step 7 and "Further cleanups" 1 to 9: Task 5. Spec tests 15 and 16: Task 1. Spec test 17 (tests adapted to the loop): Tasks 1 to 4, per the contract's test inventory. Spec test 20 (soak card S39 queries every replica) is outside the code worktree and is done by the controller on `cas-gc-rebuild`. Rules 2 to 6 and 8 to 12 landed with part 1; their tests stay green and are adapted only where the contract says so.

**Model invariants to tests.** `NoArmWhileRequestPending`: `NothingArmsWhileARemountRequestIsPending` (Task 1), `ARenewalArmsNothingWhileARequestIsPending` (Task 2). `NoJoinWindowReclaim`: `AReclaimFinishedAfterTheForgetIntentArmsNothing`, `CASForget.AReclaimDuringForgetArmsNothing` (Task 1). `RenewNotAfterIntent`: `TheForgetIntentAloneEndsARenewal` (Task 2). `LiveOnlyWhenAdmissible`: `OpenAndRemountReportReadyOnlyWhenWritable` and its two companions (Task 2). `I7RequestNotLost`: `AReclaimAcknowledgesOnlyTheGenerationItServed` (Task 1).

**Type consistency.** Task 1 produces `beginReclaim`, `armIfAdmissible`, `canArm`, `armFence`, `checkRenewerOwner`, `reclaim_generation`, `lease_thread_id`, `RenewCaller`, `renewRenewerOnce(RenewCaller)`, `consumeRenewResult(result, RenewCaller)` (under `driver_mutex` for every caller), `publishRenewedDeadline` returning `std::optional<RestoredLease>`, and `lease_wait_predicate_false_hook_for_test`. Task 2 consumes them, names `canArm`'s parameter, adds `waitUntilArmed` and the step `claimed_not_armed`. Task 3 removes `RenewCaller` and folds the entry points into `renewOnce`, `renewalEnvironment`, `renewalLive`, `consumeRenewResult(result)`; `renewWatermarkOnce` stays as the test seam. Task 4 consumes `renewOnce` and produces the extended `MountRenewResult`, `reportMountRenewCompletion` with five parameters and `release(uint64_t)`, fed with `lastCommittedAttemptStartBootMs() + ttl`. Task 5 consumes `armFence` and the constructors Task 3 leaves.

**Hunks are written against the base commit plus the contract.** Every section quotes today's code (`7e243502408`) with the function name, and the contract's target shape where an earlier task rewrites the code ("as left by Task N"). Implementers apply each change to the code as it is at their start; names, signatures, orders and lock rules are binding, line numbers are not.

**Rulings carried by the plan** (full table at the end of the contract file): a writable open with no lease thread and a claim that does not admit a ref append fails (no production caller opens that way; Task 2 lists the grep); `CASMountRenewalDeadlineExceeded` and the two deadline classifications are removed (Task 3); the engine's `GaveUp::Why::Deadline` arm maps to a terminal `Unresolved` result; a trip that comes with no request, intent or stop is re-armed by the next renewal, pinned by `ATripAloneIsRearmedByTheNextRenewal`; `renewWatermarkOnce` keeps its name; the implementer never builds `build_asan`.

**What stays unverified by the tasks' own gates.** The sanitizer arm of `OnlyTheLeaseThreadReplacesTheRenewerWhileItRuns` (a death test) compiles only under a debug or sanitizer build; the controller's ASan gate on the final tree covers it. `CASEvent.ReentrantRenewalSinkPreservesOuterObservationIdentity` shows a regression as the gate's timeout (`GATE_EXIT=124`), not as a failure: the mutex it would need is private. `AReclaimAcknowledgesOnlyTheGenerationItServed` spends about 3 s of real time in two reclaim backoffs, which have no injected clock. Cleanup 8.7 changes a log line an integration test counts; the plan retargets the test, and the controller runs that module on the final tree.

**Placeholder scan.** No TBD, no TODO, no "similar to above" in the five sections.

**Review Focus.** Five items at the top, each naming the task that owns the code. Item 2 (the lock held by the ref ledger at an interference report) is a named risk for the reviewer of Task 1.
