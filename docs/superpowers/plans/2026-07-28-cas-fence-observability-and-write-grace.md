# CAS fence-window observability + opt-in write grace — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use `superpowers:subagent-driven-development`
> (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the CAS mount fence→self-remount lifecycle diagnosable from `err.log` alone (Part A:
text-log timeline + `ProfileEvents` counters, no behavior change), and add an opt-in, default-off,
bounded, event-driven wait so a durable write can ride out a brief fence→remount window instead of
failing instantly (Part B).

**Architecture:** Spec
`docs/superpowers/specs/2026-07-28-cas-fence-observability-and-write-grace-design.md`. Two parts:
- **Part A** — make BOTH fence-loss modes observable. **Mode 1** (throwing renewal → self-remount):
  fence-arm + self-remount begin/complete lines and the classified fence reason at the keeper, plus
  `CasMountFenceArmed`/`CasSelfRemountCompleted`/`CasSelfRemountMicroseconds`. **Mode 2** (the dominant
  real incident: a hung renewal whose deadline silently expires then re-arms — no throw, no remount,
  zero logging today): an expiry `Warning` detected at the admission site (off the hot `mayMutate`
  path), a re-arm `Information` with the outage in `setMountDeadline`, a slow-renewal-landed line in
  `onRenewSucceeded`, plus `CasMountFenceExpired`/`CasMountFenceExpiredMicroseconds`. No new
  rate-limiter is needed (every line is edge-triggered); the base-class throwing-mode lines already
  exist and are left alone. Part A lands first because its counters are how Part B is measured.
- **Part B** — one disk setting `write_grace_ms` (default 0 = off = today's behavior), one new
  event-driven wait primitive `CasMountRuntime::waitForWriteGrace` (dedicated CV, bounded, wakes on
  `armMountFence` / teardown / terminal), inserted at the two write-admission ENTRY gates (plain
  object 668, ref-log append 210) BEFORE the caller captures its fence generation — so a write that
  waited out a remount re-admits under the fresh incarnation via the UNCHANGED admission check. Never
  bypasses admission (spec §B-safety); never holds a lock the remount needs (spec §B-nodeadlock).

**Tech Stack:** C++ (`src/Disks/.../ContentAddressed/`), googletest (`unit_tests_dbms`).

## Global Constraints {#global-constraints}

- Branch: `cas-gc-rebuild`. New commits only — never rebase or amend. Never push.
- C++ style: Allman braces (CI style check enforces). Em-dash `—` in operator-visible strings. In
  comments/messages write function names as `f`, not `f()`; say "exception" not "crash" for logical
  errors; `ASan` not `ASAN`.
- Never use sleep to fix a race. Tests are event-driven (signal from a second thread + a bounded
  test-level deadline poll) or use the injected fake clocks (`boot_ms_fn`, `wait_sleep_fn`).
- No protocol change: no new object-store request, no change to lease/fence/remount semantics
  (standing user veto). Part B is a client-side wait around the UNCHANGED admission check only.
- Build: `ninja -C build > build/ninja_<task>.log 2>&1` — no `-j`, no `nproc`; always redirect to a
  log in the build dir; always have a subagent summarize the log rather than reading it inline.
- Unit-test binary: `build/src/unit_tests_dbms`. Redirect each run to a unique
  `build/test_<name>.log`; have a subagent summarize.
- CA gtest gate filter (release gate for CAS work): `--gtest_filter='Cas*:CA*'`.
- Commits end with exactly:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01GKmSZa7T87WbRGKkNkSXky`
- Shared worktree: stage only the files each task names, by explicit path (`git add <paths>`), never
  `git add -A`. After each commit, `git log -1 --stat` to confirm the commit holds exactly those
  files; interleaved foreign commits from other sessions are normal — do not touch them.

## Task overview & ordering {#task-overview}

Part A first (its counters measure Part B), then Part B (setting → primitive → the two surfaces):

1. **Task 1 — Part A ProfileEvents + fence lifecycle logging, both modes** (`CasMountRuntime` +
   `CasPool` + `CasServerRoot` + `ProfileEvents.cpp`): Mode 1 = A3/A4/A6 + three counters; Mode 2 =
   M1 expiry (admission-site) / M2 slow-renewal / M3 re-arm (`setMountDeadline`) + two counters.
   gtest: counters move through a real Mode-1 fence→remount AND a Mode-2 silent expiry→re-arm.
2. **Task 2 — Part A classified fence reason at the keeper** (`MountLeaseKeeper` A2 line). gtest: the
   classified reason reaches text/the stored member.
3. **Task 3 — Part B setting `write_grace_ms`** (declare + wire through to `MountConfig`). gtest/build:
   the value reaches `CasMountRuntime`.
4. **Task 4 — Part B wait primitive `waitForWriteGrace`** (`CasMountRuntime`: dedicated CV + signalers
   at `armMountFence`/teardown/terminal). gtest: the five §B-tests scenarios at the runtime level.
5. **Task 5 — Part B surface 1 (plain object, 668)**: insert the wait before the generation capture
   at the staging/finalize entry gate. gtest through the pool/open path.
6. **Task 6 — Part B surface 2 (ref-log append, 210)**: insert the wait at the append entry choke
   point before the `may_mutate` gate. gtest through the ref lane.

Each task is TDD: write the failing test first, run it red, implement, run it green, run the
`Cas*:CA*` gate, commit. Exact files, code, and commands are in each task below.

---

### Task 1: Part A — ProfileEvents + fence/remount lifecycle logging (A3, A4, A6) {#task-1}

**Files:**
- Modify: `src/Common/ProfileEvents.cpp` (five counters after the CAS block at `:893-895`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h`
  (a `fence_tripped_boot_ms` member + getter; a `fence_expiry_reported_boot_ms` latch)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp`
  (Mode 1: A3 line + `CasMountFenceArmed` + stamp in `tripMountLost` `:87`; Mode 2: M1 expiry
  detection in `checkFenceOrThrow` `:98`; M3 re-arm detection in `setMountDeadline` `:128`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp`
  (A4 begin line at `tryRemountOnce` entry `~:988`; A6 duration + two counters at the completion
  line `:1072`/`:1086`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp`
  (Mode 2: M2 slow-renewal-landed line in `MountLeaseKeeper::onRenewSucceeded` `:883`)
- Test: `src/Disks/tests/gtest_cas_pool.cpp` (`CasPoolObservability` — Mode 1 and Mode 2)

**Interfaces:**
- Consumes: `ProfileEvents::increment`, `CasMountRuntime::bootMsNow` (`CasMountRuntime.h:132`),
  `Pool::tripMountLost`/`tryRemountOnce`/`layout()` forwarders, the `fenceOutMount` helper
  (`gtest_cas_pool.cpp:1241`).
- Produces: `ProfileEvents::CasMountFenceArmed`, `CasSelfRemountCompleted`,
  `CasSelfRemountMicroseconds`; `CasMountRuntime::fenceTrippedBootMs()`.

- [ ] **Step 1: Write the failing test.** Append to `gtest_cas_pool.cpp`:

```cpp
TEST(CasPoolObservability, FenceTripAndSelfRemountMoveProfileEvents)
{
    auto backend = std::make_shared<InMemoryBackend>();
    uint64_t fake_boot = 1000;
    auto s = Pool::open(backend, PoolConfig{
        .pool_prefix = "p", .server_root_id = "test",
        .boot_ms_fn = [&] { return fake_boot; }});

    ProfileEvents::Counters::Snapshot before = ProfileEvents::global_counters.getPartiallyAtomicSnapshot();

    s->tripMountLost();                 // A3: CasMountFenceArmed +1, stamps fence_tripped_boot_ms=1000
    fake_boot = 1500;                   // 500 ms fenced
    fenceOutMount(*backend, s->layout().mountKey("test"));
    ASSERT_TRUE(s->tryRemountOnce());   // A6: CasSelfRemountCompleted +1, +500000 us

    ProfileEvents::Counters::Snapshot after = ProfileEvents::global_counters.getPartiallyAtomicSnapshot();
    EXPECT_EQ(after[ProfileEvents::CasMountFenceArmed] - before[ProfileEvents::CasMountFenceArmed], 1);
    EXPECT_EQ(after[ProfileEvents::CasSelfRemountCompleted] - before[ProfileEvents::CasSelfRemountCompleted], 1);
    EXPECT_EQ(after[ProfileEvents::CasSelfRemountMicroseconds] - before[ProfileEvents::CasSelfRemountMicroseconds],
              500u * 1000u);
}
```
And the Mode-2 (silent expiry / non-remount recovery) counters — driven entirely by the injectable
boot clock and the `setMountDeadline`/`checkFenceOrThrow` Pool forwarders, no remount:

```cpp
TEST(CasPoolObservability, SilentDeadlineExpiryAndReArmMoveProfileEvents)
{
    auto backend = std::make_shared<InMemoryBackend>();
    uint64_t fake_boot = 1000;
    auto s = Pool::open(backend, PoolConfig{
        .pool_prefix = "p", .server_root_id = "test",
        .boot_ms_fn = [&] { return fake_boot; }});

    // Arm a real deadline, then let boot time pass it WITHOUT a trip/remount (the Mode-2 hang shape).
    s->setMountDeadline(fake_boot + 1000);          // live until boot 2000
    const uint64_t g0 = s->fenceGeneration();
    fake_boot = 2500;                               // deadline passed (1500 ms overdue) -> mayMutate false
    ASSERT_FALSE(s->mayMutate());

    ProfileEvents::Counters::Snapshot before = ProfileEvents::global_counters.getPartiallyAtomicSnapshot();
    EXPECT_THROW(s->checkFenceOrThrow(g0), DB::Exception);   // M1: refused write -> CasMountFenceExpired +1
    fake_boot = 3000;
    s->setMountDeadline(fake_boot + 1000);          // M3: late renewal re-arms; outage = 3000 - 2000 = 1000 ms
    ProfileEvents::Counters::Snapshot after = ProfileEvents::global_counters.getPartiallyAtomicSnapshot();

    EXPECT_EQ(after[ProfileEvents::CasMountFenceExpired] - before[ProfileEvents::CasMountFenceExpired], 1);
    EXPECT_EQ(after[ProfileEvents::CasMountFenceExpiredMicroseconds] - before[ProfileEvents::CasMountFenceExpiredMicroseconds],
              1000u * 1000u);
    EXPECT_EQ(s->fenceGeneration(), g0) << "Mode-2 re-arm must NOT bump the fence generation";
    EXPECT_TRUE(s->mayMutate());
}
```
The expiry stamp is `deadline_boot_ms` (2000), so the outage is measured from the true expiry instant
(3000 − 2000 = 1000 ms), not from the first refused write. Add `#include <Common/ProfileEvents.h>` and
the `extern const Event` declarations to the test file's `ProfileEvents` namespace block (mirror how
`gtest_cas_heartbeat.cpp` declares `extern const int` error codes) if not already present.

- [ ] **Step 2: Build the test target, run — must FAIL to compile (the events don't exist yet).**

```bash
ninja -C build src/unit_tests_dbms > build/ninja_task1_test.log 2>&1; echo "exit=$?"
```
Expected: compile error (unknown `ProfileEvents::CasMountFenceArmed`). That is the red state for a
new-symbol test.

- [ ] **Step 3: Implement.**

3a. `src/Common/ProfileEvents.cpp`, after `:895` (five counters — three Mode-1, two Mode-2):
```cpp
    M(CasMountFenceArmed, "Counts CAS local write-fence trips (Mode 1): a renewal threw, the runtime latched its write fence lost, and a self-remount follows. Runtime-side counterpart of CasMountLeaseLost.", ValueType::Number) \
    M(CasSelfRemountCompleted, "Counts successful CAS self-remounts (Mode 1): after a fence trip the node reclaimed the mount under a fresh writer_epoch and durable writes resumed.", ValueType::Number) \
    M(CasSelfRemountMicroseconds, "Total time (CLOCK_BOOTTIME) CAS durable writes were refused across Mode-1 self-remounts. Divide by CasSelfRemountCompleted for the average window.", ValueType::Microseconds) \
    M(CasMountFenceExpired, "Counts CAS Mode-2 fence-expiry episodes: the mount-lease deadline passed WITHOUT a lost-latch/remount (a hung renewal), first observed at a refused durable write. The dominant real fence-window shape; recovery is a late renewal landing, not a remount.", ValueType::Number) \
    M(CasMountFenceExpiredMicroseconds, "Total time (CLOCK_BOOTTIME) CAS durable writes were refused across Mode-2 (non-remount) recoveries. Divide by CasMountFenceExpired for the average silent-outage length. This is the path the opt-in write_grace_ms feature most often rides.", ValueType::Microseconds) \
```

3b. `CasMountRuntime.h`: add a private member and a getter (near `mount_fence`):
```cpp
    /// A3/A6 observability: CLOCK_BOOTTIME ms at which the fence last tripped (Part A). 0 = not
    /// tripped since the last completed remount. Read at the remount-complete site to report how long
    /// writes were refused. Boot clock (not wall) so an NTP step or VM suspend cannot distort it.
    std::atomic<uint64_t> fence_tripped_boot_ms{0};
```
and a public accessor:
```cpp
    uint64_t fenceTrippedBootMs() const { return fence_tripped_boot_ms.load(std::memory_order_acquire); }
```

3c. `CasMountRuntime.cpp` `tripMountLost` (`:87`), add at the top of the body:
```cpp
    ProfileEvents::increment(ProfileEvents::CasMountFenceArmed);
    fence_tripped_boot_ms.store(bootMsNow(), std::memory_order_release);
    LOG_WARNING(getLogger("CasMountLease"),
        "CAS mount fence tripped for server root '{}' (writer_epoch={}): durable writes are refused "
        "until a self-remount re-admits this node", server_root_id, mount_fence.writer_epoch);
```
Add `extern const Event CasMountFenceArmed; extern const Event CasSelfRemountCompleted; extern const Event CasSelfRemountMicroseconds;`
to this file's `namespace ProfileEvents` block (`:22-26`).

3d. `CasPool.cpp` A4 begin, at `tryRemountOnce` entry just before the `try` at `:988`:
```cpp
    LOG_INFO(getLogger("CasMountLease"),
        "CAS self-remount starting for server root '{}': reclaiming the mount under a fresh "
        "writer_epoch after a fence loss", srid);
```

3e. `CasPool.cpp` A6, replace the existing `:1072` `LOG_INFO` with the duration-carrying form and the
counters (place right after `armMountFence`/`noteRemounted`, using `mount_runtime.fenceTrippedBootMs`):
```cpp
        ProfileEvents::increment(ProfileEvents::CasSelfRemountCompleted);
        const uint64_t tripped_at = mount_runtime.fenceTrippedBootMs();
        const uint64_t fenced_ms = tripped_at ? (remount_anchor_boot_ms - tripped_at) : 0;
        if (tripped_at)
            ProfileEvents::increment(ProfileEvents::CasSelfRemountMicroseconds, fenced_ms * 1000);
        LOG_INFO(getLogger("CasPool"),
            "CAS self-remount '{}': recovered as writer_epoch {} (fresh incarnation; older builds fail "
            "closed){}", srid, writer_epoch,
            tripped_at ? fmt::format("; writes were fenced for {} ms", fenced_ms) : "");
```
Reset the stamp after reading it so the next episode measures fresh: in `noteRemounted`
(`CasMountRuntime.cpp:324`) add `fence_tripped_boot_ms.store(0, std::memory_order_release);`. Add the
`extern const Event` block to `CasPool.cpp`'s `ProfileEvents` namespace and `#include <Common/ProfileEvents.h>`
if absent.

3f. **Mode 2 — the silent expiry/re-arm path (spec §A.5).** `CasMountRuntime.h`, add a latch member
next to `fence_tripped_boot_ms`:
```cpp
    /// Mode-2 expiry latch (Part A §A.5): the true expiry instant (deadline_boot_ms) of an
    /// unreported silent fence-deadline expiry, or 0. CAS'd 0->deadline at the admission site that
    /// first refuses a write; read+reset by setMountDeadline on the late-renewal re-arm.
    std::atomic<uint64_t> fence_expiry_reported_boot_ms{0};
```
`CasMountRuntime.cpp` `checkFenceOrThrow` (`:98`), inside the refusal branch BEFORE the throw, detect
a Mode-2 expiry (fence not lost, deadline passed) and report it once — this is the M1 line and it is
NOT on the hot `mayMutate` read path (it runs only when a durable write is being refused):
```cpp
    if (!mayMutate() || fenceGeneration() != admitted_generation)
    {
        const uint64_t deadline = mount_fence.deadline_boot_ms.load(std::memory_order_acquire);
        if (!mount_fence.lost.load(std::memory_order_acquire) && bootMsNow() >= deadline)
        {
            uint64_t expected = 0;
            if (fence_expiry_reported_boot_ms.compare_exchange_strong(expected, deadline,
                    std::memory_order_acq_rel, std::memory_order_acquire))
            {
                ProfileEvents::increment(ProfileEvents::CasMountFenceExpired);
                LOG_WARNING(getLogger("CasMountLease"),
                    "CAS mount fence deadline expired for server root '{}' (writer_epoch={}): durable "
                    "writes are refused; the lease renewer has not confirmed within the renew period and "
                    "the deadline passed {} ms ago — the object store is likely slow or unreachable (no "
                    "self-remount; recovery is a late renewal landing)",
                    server_root_id, mount_fence.writer_epoch, bootMsNow() - deadline);
            }
        }
        throw Exception(ErrorCodes::INVALID_STATE, ...unchanged message...);
    }
```
`CasMountRuntime.cpp` `setMountDeadline` (`:128`), detect the M3 re-arm and record the outage:
```cpp
void CasMountRuntime::setMountDeadline(uint64_t deadline_boot_ms)
{
    mount_fence.deadline_boot_ms.store(deadline_boot_ms, std::memory_order_release);
    const uint64_t expired_at = fence_expiry_reported_boot_ms.exchange(0, std::memory_order_acq_rel);
    if (expired_at != 0)
    {
        const uint64_t outage_ms = bootMsNow() - expired_at;
        ProfileEvents::increment(ProfileEvents::CasMountFenceExpiredMicroseconds, outage_ms * 1000);
        LOG_INFO(getLogger("CasMountLease"),
            "CAS mount fence re-armed for server root '{}' after a {} ms expiry window (a late lease "
            "renewal landed; recovered without a self-remount)", server_root_id, outage_ms);
    }
    /// Task 4 also adds write_grace_cv.notify_all() here (the Mode-2 grace wake).
}
```

3g. **M2 — slow renewal landed.** `CasServerRoot.cpp` `MountLeaseKeeper::onRenewSucceeded` (`:883`),
after the existing body, log when the just-landed renewal exceeded the renew period. The keeper has
`last_attempt_boot_ms` (`:637`) and `ttl`; the renew period is `ttl/3` by convention, so pass the
period into the keeper or compare against a fraction of `ttl`. Minimal form using the existing members:
```cpp
    const uint64_t elapsed = boot_ms_fn() - last_attempt_boot_ms;
    if (elapsed > static_cast<uint64_t>(ttl.count()) / 3)
        LOG_INFO(getLogger("CasMountLeaseKeeper"),
            "CAS mount-lease renewal for server root '{}' landed after {} ms — the object store was slow",
            srid, elapsed);
```

Add the `extern const Event CasMountFenceExpired; extern const Event CasMountFenceExpiredMicroseconds;`
declarations to the `ProfileEvents` namespace block of `CasMountRuntime.cpp`.

- [ ] **Step 4: Build and run green.**
```bash
ninja -C build > build/ninja_task1_impl.log 2>&1; echo "exit=$?"
build/src/unit_tests_dbms --gtest_filter='CasPoolObservability.*' > build/test_task1.log 2>&1; echo "exit=$?"
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*' > build/test_task1_gate.log 2>&1; echo "exit=$?"
```
Expected: both exit 0. (Have a subagent summarize each log.)

- [ ] **Step 5: Commit.**
```bash
git add src/Common/ProfileEvents.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp src/Disks/tests/gtest_cas_pool.cpp
git commit -m "ca: Part A — fence/remount lifecycle observability (Mode 1 remount + Mode 2 silent expiry)

Mode 1 (throwing renewal -> self-remount): CasMountFenceArmed /
CasSelfRemountCompleted / CasSelfRemountMicroseconds; A3 fence-arm WARNING in
tripMountLost; A4 self-remount-begin INFO; A6 recover line gains the fenced
duration. Mode 2 (the dominant real incident: a hung renewal whose deadline
silently expired then re-armed with no log, no remount): CasMountFenceExpired /
CasMountFenceExpiredMicroseconds; M1 expiry WARNING detected at the admission
site (off the hot mayMutate path); M3 re-arm INFO + outage in setMountDeadline;
M2 slow-renewal-landed INFO in onRenewSucceeded. Boot-clock stamps so windows
are measurable and NTP/suspend-proof.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01GKmSZa7T87WbRGKkNkSXky"
```

---

### Task 2: Part A — classified fence reason at the keeper (A2) {#task-2}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h`
  (`MountLeaseKeeper`: a `last_fence_reason` member)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp`
  (`onRenewMismatch` stashes the reason at each branch `:905-986`; `onRenewFailed` `:892` logs A2)
- Test: `src/Disks/tests/gtest_cas_heartbeat.cpp` (assert the stashed reason after a classified fence)

**Interfaces:**
- Consumes: the existing `onRenewMismatch` branch strings; `seedOwnClaim`; the fault pattern of
  driving a confirmed mismatch (advance the slot's token under the keeper — see the Phase-A tests at
  `gtest_cas_heartbeat.cpp:125+`).
- Produces: `MountLeaseKeeper::lastFenceReasonForTest()`.

- [ ] **Step 1: Failing test.** Append to `gtest_cas_heartbeat.cpp` (reuse the
`SameEpochUnfencedTouchIsUncertainNotFatal` setup — same-uuid/same-epoch advanced token → the
`same_epoch_state_uncertain` branch):

```cpp
TEST(CasHeartbeat, ClassifiedFenceReasonIsRecordedForLogging)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    MountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9, std::chrono::milliseconds(100),
                            [&] { return now_ms; }, [] { return uint64_t{5}; });
    keeper.start();

    const HeadResult h = backend->head(layout.mountKey(srid));
    ASSERT_TRUE(h.exists);
    MountLease advanced; advanced.server_uuid = uuid; advanced.writer_epoch = 9; advanced.seq = 99;
    backend->putOverwrite(layout.mountKey(srid), encodeMountLease(advanced), h.token);

    EXPECT_THROW(keeper.renewOnce(), DB::Exception);   // ABORTED, not fatal
    EXPECT_NE(keeper.lastFenceReasonForTest().find("state uncertain"), String::npos)
        << keeper.lastFenceReasonForTest();
}
```

- [ ] **Step 2: Build + run — must FAIL (no `lastFenceReasonForTest`).**
```bash
ninja -C build src/unit_tests_dbms > build/ninja_task2_test.log 2>&1; echo "exit=$?"
```
Expected: compile error (red).

- [ ] **Step 3: Implement.**

3a. `CasServerRoot.h`, `MountLeaseKeeper` private members + accessor:
```cpp
    /// Part A (A2): the human reason the last confirmed mismatch classified into, stashed by
    /// onRenewMismatch before it throws so onRenewFailed can name it in the fence WARNING. Written and
    /// read on the single renewal driver thread (see renewOnce), so it needs no synchronization.
    String last_fence_reason;
public:
    const String & lastFenceReasonForTest() const { return last_fence_reason; }
```

3b. `CasServerRoot.cpp` `onRenewMismatch`: set `last_fence_reason = "<branch reason>";` in each of the
`same_epoch_state_uncertain`, `superseded`, `foreign_writer`, and `vanished` branches, immediately
before each `throw` (reuse the reason strings already passed to `emitMountEvent`).

3c. `CasServerRoot.cpp` `onRenewFailed` (`:892`), add the A2 log after the event emit:
```cpp
    LOG_WARNING(getLogger("CasMountLeaseKeeper"),
        "CAS mount-lease for server root '{}' fenced: {} — the write fence is latched lost; a "
        "self-remount under a fresh writer_epoch will follow", srid,
        last_fence_reason.empty() ? "renewal could not be confirmed" : last_fence_reason);
```

- [ ] **Step 4: Build + run green.**
```bash
ninja -C build > build/ninja_task2_impl.log 2>&1; echo "exit=$?"
build/src/unit_tests_dbms --gtest_filter='CasHeartbeat.*' > build/test_task2.log 2>&1; echo "exit=$?"
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*' > build/test_task2_gate.log 2>&1; echo "exit=$?"
```
Expected: exit 0.

- [ ] **Step 5: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp src/Disks/tests/gtest_cas_heartbeat.cpp
git commit -m "ca: Part A — surface the classified mount-fence reason as a keeper WARNING (A2)

onRenewMismatch stashes the branch it classified (state uncertain / superseded
/ vanished / foreign); onRenewFailed logs it — the fence reason was previously
audit-event-only, invisible in err.log.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01GKmSZa7T87WbRGKkNkSXky"
```

---

### Task 3: Part B — the `write_grace_ms` setting {#task-3}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp`
  (`LIST_OF_CONTENT_ADDRESSED_SETTINGS`, next to `materialization_grace_ms` at `:91`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`
  (member init at `:287`; `pool_config.write_grace_ms = ...` at `:740`) + the header member decl
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h`
  (`PoolConfig::write_grace_ms` + `MountConfig` projection in `mountConfig()` at `:227`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h`
  (`MountConfig::write_grace_ms`)
- Test: `src/Disks/tests/gtest_cas_pool.cpp` (the value reaches the runtime via a new accessor)

**Interfaces:**
- Produces: `MountConfig::write_grace_ms` (default 0); `CasMountRuntime::writeGraceMs()` accessor
  (used by Task 4).

- [ ] **Step 1: Failing test.**
```cpp
TEST(CasPoolWriteGrace, SettingReachesRuntimeDefaultOff)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s0 = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    EXPECT_EQ(s0->writeGraceMsForTest(), 0u);   // default OFF

    auto b2 = std::make_shared<InMemoryBackend>();
    auto s1 = Pool::open(b2, PoolConfig{.pool_prefix = "p", .server_root_id = "t2", .write_grace_ms = 250});
    EXPECT_EQ(s1->writeGraceMsForTest(), 250u);
}
```

- [ ] **Step 2: Build + run — FAIL (no `write_grace_ms` field / accessor).**
```bash
ninja -C build src/unit_tests_dbms > build/ninja_task3_test.log 2>&1; echo "exit=$?"
```

- [ ] **Step 3: Implement the wiring.**

3a. `ContentAddressedSettings.cpp`, add to the settings list:
```cpp
    DECLARE(UInt64, write_grace_ms, 0, "Bounded wait (ms) for a durable write to ride out a mount fence->self-remount window before failing; 0 = off = fail immediately (today's behavior)", 0) \
```

3b. `ContentAddressedMetadataStorage.cpp`: declare the member (`.h`) `uint64_t write_grace_ms;` next
to `materialization_grace_ms`; init at `:287` `, write_grace_ms(settings_[ContentAddressedSetting::write_grace_ms].value)`;
set at `:740` `pool_config.write_grace_ms = write_grace_ms;`. Add
`extern const ContentAddressedSettingsUInt64 write_grace_ms;` to the settings-extern block at `:71`.

3c. `CasPool.h`: add `uint64_t write_grace_ms = 0;` to `PoolConfig` (next to
`materialization_grace_ms` at `:152`), and to the `mountConfig()` projection (`:227`):
`.write_grace_ms = write_grace_ms,`.

3d. `CasMountRuntime.h`: add `uint64_t write_grace_ms = 0;` to `MountConfig` (`:55` area) and a public
accessor `uint64_t writeGraceMs() const { return config.write_grace_ms; }`. Add the `Pool` forwarder
`uint64_t writeGraceMsForTest() const { return mount_runtime.writeGraceMs(); }` in `CasPool.h`.

- [ ] **Step 4: Build + run green.**
```bash
ninja -C build > build/ninja_task3_impl.log 2>&1; echo "exit=$?"
build/src/unit_tests_dbms --gtest_filter='CasPoolWriteGrace.*:Cas*:CA*' > build/test_task3.log 2>&1; echo "exit=$?"
```
Expected: exit 0.

- [ ] **Step 5: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h src/Disks/tests/gtest_cas_pool.cpp
git commit -m "ca: Part B — add opt-in write_grace_ms disk setting (default 0 = off), wired to CasMountRuntime

Declares the setting and threads it ContentAddressedSettings -> metadata
storage -> PoolConfig -> MountConfig -> CasMountRuntime. No behavior yet
(Task 4 adds the wait primitive that reads it).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01GKmSZa7T87WbRGKkNkSXky"
```

---

### Task 4: Part B — the `waitForWriteGrace` primitive + signalers {#task-4}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h`
  (`waitForWriteGrace` decl + `write_grace_cv`/`write_grace_mutex` members)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp`
  (`waitForWriteGrace` body; `notify_all` in `armMountFence` `:133`, `stopRemountThread` `:486`,
  `finishTeardown` `:504`, `enterVanished` `:372`, `enterIdentityLost` `:337`,
  `publishVanishedIntent` `:421`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h`
  (a `Pool::waitForWriteGrace()` forwarder)
- Test: `src/Disks/tests/gtest_cas_pool.cpp` (the five §B-tests scenarios at the Pool level)

**Interfaces:**
- Consumes: `mayMutate` (`CasMountRuntime.cpp:81`), `remount_shutting_down`, `remountTerminal`
  (`CasMountRuntime.h:361`), `armMountFence`, the `boot_ms_fn`/`wait_sleep_fn` test seams,
  `fenceOutMount`, `tryRemountOnce`, `beginShutdownForTest`, `setLifecycleForTest`.
- Produces: `void CasMountRuntime::waitForWriteGrace() const;` and `Pool::waitForWriteGrace()`.

- [ ] **Step 1: Failing tests.** Append to `gtest_cas_pool.cpp` (all event-driven — signal from a
second thread, plus a generous test-level deadline to guard against a hang):

```cpp
/// (1) fence down + a remount from another thread => the waiter returns and the write admits.
TEST(CasPoolWriteGrace, WaitReturnsWhenRemountReArmsFence)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                            .write_grace_ms = 5000});
    s->tripMountLost();
    fenceOutMount(*backend, s->layout().mountKey("test"));
    ASSERT_FALSE(s->mayMutate());

    std::atomic<bool> returned{false};
    std::thread w([&] { s->waitForWriteGrace(); returned = true; });
    ASSERT_TRUE(s->tryRemountOnce());   // re-arms the fence -> armMountFence notifies the grace CV

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!returned.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_TRUE(returned.load());
    EXPECT_TRUE(s->mayMutate());        // proceeds normally under the fresh incarnation
    w.join();
}

/// (2) bound expires before any remount => returns after >= bound, and the write still fails.
TEST(CasPoolWriteGrace, WaitTimesOutThenFenceCheckStillThrows)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                            .write_grace_ms = 60});
    s->tripMountLost();
    const auto t0 = std::chrono::steady_clock::now();
    s->waitForWriteGrace();             // no remount -> waits out the 60 ms bound
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 60);
    EXPECT_THROW(s->checkFenceOrThrow(s->fenceGeneration()), DB::Exception);   // INVALID_STATE, as today
}

/// (3) shutdown during the wait => prompt unblock, no hang.
TEST(CasPoolWriteGrace, WaitUnblocksPromptlyOnShutdown)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                            .write_grace_ms = 60000});
    s->tripMountLost();
    std::atomic<bool> returned{false};
    std::thread w([&] { s->waitForWriteGrace(); returned = true; });
    s->beginShutdownForTest();          // latches remount_shutting_down + notifies the grace CV
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!returned.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_TRUE(returned.load()) << "shutdown must unblock the grace wait well under the 60 s bound";
    w.join();
}

/// (4) terminal lifecycle => immediate return (well under the bound), no wait.
TEST(CasPoolWriteGrace, WaitReturnsImmediatelyWhenTerminal)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                            .write_grace_ms = 60000});
    s->tripMountLost();
    s->setLifecycleForTest(PoolLifecycle::VanishedForgotten);
    const auto t0 = std::chrono::steady_clock::now();
    s->waitForWriteGrace();
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count(), 1000);
}

/// (5) feature OFF => immediate return even with the fence down.
TEST(CasPoolWriteGrace, WaitIsNoOpWhenDisabled)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});  // write_grace_ms=0
    s->tripMountLost();
    const auto t0 = std::chrono::steady_clock::now();
    s->waitForWriteGrace();
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count(), 1000);
}

/// (6) Mode-2 (the DOMINANT real path): the fence deadline expires with NO trip and NO remount, and a
/// late renewal re-arms via setMountDeadline. The waiter must wake on that re-arm — not on
/// armMountFence — and proceed with an UNCHANGED fence generation (spec §A.5, §B.4). This is the path
/// the whole feature exists for (the msan incident).
TEST(CasPoolWriteGrace, WaitRidesOutSilentDeadlineExpiryWithoutRemount)
{
    auto backend = std::make_shared<InMemoryBackend>();
    uint64_t fake_boot = 1000;
    auto s = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                            .write_grace_ms = 5000,
                                            .boot_ms_fn = [&] { return fake_boot; }});
    s->setMountDeadline(fake_boot + 1000);          // live until boot 2000
    const uint64_t g0 = s->fenceGeneration();
    fake_boot = 2500;                               // Mode-2 silent expiry: mayMutate false, no trip
    ASSERT_FALSE(s->mayMutate());

    std::atomic<bool> returned{false};
    std::thread w([&] { s->waitForWriteGrace(); returned = true; });
    // A late renewal lands and re-arms (setMountDeadline) — the ONLY signaler on this path.
    fake_boot = 3000;
    s->setMountDeadline(fake_boot + 1000);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!returned.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_TRUE(returned.load()) << "the Mode-2 re-arm (setMountDeadline) must wake the grace wait";
    EXPECT_TRUE(s->mayMutate());
    EXPECT_EQ(s->fenceGeneration(), g0) << "Mode-2 re-arm preserves the generation; the waited write "
                                           "admits under the same still-valid incarnation";
    w.join();
}
```
Faithful-integration variant (optional, same assertion): instead of driving `setMountDeadline`
directly, open writable with `background_watermark = true` and a fault backend whose `putOverwrite`
on the mount key BLOCKS on a releasable latch (rather than throwing) — the real keeper's renewal hangs,
the injected boot clock is advanced past the deadline, the waiter blocks, and releasing the latch lets
the hung PUT land → `onRenewSucceeded → setMountDeadline` re-arms and wakes the waiter. The direct
form above is the deterministic primary; this variant reproduces the exact incident mechanics.

- [ ] **Step 2: Build + run — FAIL (no `waitForWriteGrace`).**
```bash
ninja -C build src/unit_tests_dbms > build/ninja_task4_test.log 2>&1; echo "exit=$?"
```

- [ ] **Step 3: Implement.**

3a. `CasMountRuntime.h`: declare
```cpp
    /// Part B (opt-in write grace). If the write fence is held, returns at once. Otherwise, when
    /// write_grace_ms > 0, blocks event-driven until the fence re-arms (a self-remount completed),
    /// the bound elapses, teardown is signaled, or the pool is terminal. NEVER changes admission — the
    /// caller still runs its own fence check on return. Holds only write_grace_mutex; takes no ref/
    /// remount lock and is never called inside a ref-append leader tenure (spec B-nodeadlock).
    void waitForWriteGrace() const;
```
and add the members:
```cpp
    mutable std::mutex write_grace_mutex;
    mutable std::condition_variable write_grace_cv;
```

3b. `CasMountRuntime.cpp`:
```cpp
void CasMountRuntime::waitForWriteGrace() const
{
    const uint64_t bound = config.write_grace_ms;
    if (bound == 0 || mayMutate())
        return;
    std::unique_lock lk(write_grace_mutex);
    write_grace_cv.wait_for(lk, std::chrono::milliseconds(bound),
        [this] { return mayMutate() || remount_shutting_down.load() || remountTerminal(); });
}
```
Add `<condition_variable>` if not already included (the header already includes it).

3c. Signal the CV (one line each) at every site that changes what the predicate observes:
- **`setMountDeadline` (`:128`) — the Mode-2 re-arm, the MOST IMPORTANT signaler** (a hung renewal
  landed and extended the deadline; §A.5 / Task 1 step 3f already edits this function). Without it a
  waiter sleeps out its whole bound through exactly the dominant incident. `write_grace_cv.notify_all();`
- `armMountFence` (`:133`) — the Mode-1 re-arm, after clearing `lost`: `write_grace_cv.notify_all();`
- `stopRemountThread` (`:486`) after latching `remount_shutting_down`, and `finishTeardown` (`:504`)
  start: `write_grace_cv.notify_all();`
- `enterVanished` (`:372`), `enterIdentityLost` (`:337`), `publishVanishedIntent` (`:421`), after
  publishing the terminal latch/state: `write_grace_cv.notify_all();`

3d. `CasPool.h` forwarder (next to `mayMutate`/`checkFenceOrThrow`):
```cpp
    void waitForWriteGrace() const { mount_runtime.waitForWriteGrace(); }
```

- [ ] **Step 4: Build + run green (ASan/TSan-clean — the tests spawn threads).**
```bash
ninja -C build > build/ninja_task4_impl.log 2>&1; echo "exit=$?"
build/src/unit_tests_dbms --gtest_filter='CasPoolWriteGrace.*' > build/test_task4.log 2>&1; echo "exit=$?"
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*' > build/test_task4_gate.log 2>&1; echo "exit=$?"
```
Expected: exit 0, no hang, no sanitizer report.

- [ ] **Step 5: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h src/Disks/tests/gtest_cas_pool.cpp
git commit -m "ca: Part B — event-driven bounded waitForWriteGrace primitive (opt-in)

Dedicated CV woken by armMountFence (remount complete), teardown, and terminal
transitions; bounded by write_grace_ms; predicate re-checks mayMutate/terminal.
Holds only its own mutex — never a ref/remount lock, never inside a ref-append
leader tenure — so it cannot stall the remount it waits for. No admission
change: the caller still runs its own fence check on return.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01GKmSZa7T87WbRGKkNkSXky"
```

---

### Task 5: Part B — surface 1 (plain-object / staging-finalize, 668) {#task-5}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp`
  (insert `waitForWriteGrace` before the generation capture at `:887`; and any sibling
  `fenceGeneration()`-capture entry gates the sweep in Step 3 finds)
- Test: `src/Disks/tests/gtest_cas_pool.cpp` or `gtest_cas_part_write.cpp` (a durable write that
  waited out a remount succeeds; with the feature off it fails)

**Interfaces:**
- Consumes: `Pool::waitForWriteGrace` (Task 4), `Pool::fenceGeneration`/`checkFenceOrThrow`.
- Produces: no new symbol — a wait inserted at the entry gate.

- [ ] **Step 1: Identify every ENTRY-gate `fenceGeneration()` capture** (not the mid-operation
re-checks). Run:
```bash
grep -rn "fenceGeneration()" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/
```
The entry gates to wrap are the ones that capture-then-write with no incarnation-tied work in
between — `ContentAddressedTransaction.cpp:887` (the staging write-buffer construction). The
`CasPartWriteTxn.cpp:705`/`:747` displacement re-checks are MID-operation and MUST NOT be wrapped
(a moved generation there means abort). Record the classification inline in the commit.

- [ ] **Step 2: Failing test.** A pool with `write_grace_ms > 0`: trip the fence, spawn the durable
write on a thread, remount from the main thread, assert the write succeeds; a second case with
`write_grace_ms == 0` asserts it throws `INVALID_STATE`. (Model the durable-write call on the nearest
existing part-write test in `gtest_cas_part_write.cpp`; if a full part write is too heavy, assert the
narrower contract that `waitForWriteGrace` is invoked before `fenceGeneration()` at the gate by a
spy, mirroring `ProbeWatchingBackend`.)

- [ ] **Step 3: Implement.** At `ContentAddressedTransaction.cpp:887`, insert before the capture:
```cpp
            pool->waitForWriteGrace();   // Part B (opt-in): ride out a fence->remount window
            const uint64_t admitted_generation = pool->fenceGeneration();
```
(unchanged `checkFenceOrThrow` at `:901`).

- [ ] **Step 4: Build + run green + gate.**
```bash
ninja -C build > build/ninja_task5_impl.log 2>&1; echo "exit=$?"
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*' > build/test_task5_gate.log 2>&1; echo "exit=$?"
```

- [ ] **Step 5: Commit** (message: "ca: Part B — surface 1 (plain-object write) waits out the fence
window before admission"; standard trailers).

---

### Task 6: Part B — surface 2 (ref-log append entry, 210) {#task-6}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp`
  (`appendRefOps`, the entry before the `ref_queue_mutex`/`leader_active` acquisition at `:1119-1165`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h`
  (inject a `wait_for_write_grace` `std::function<void()>` next to `may_mutate` at `:370`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp`
  (wire the callback where `may_mutate` is injected, `~:171`)
- Test: `src/Disks/tests/gtest_cas_ref_*` (an append that waited out a remount lands on a fresh
  runtime and commits)

**Interfaces:**
- Consumes: `Pool::waitForWriteGrace`; the `appendRefOps` structure (`CasRefLedger.cpp:1112-1170`).
- Produces: `CasRefLedger::wait_for_write_grace` injected callback.

> **`leader_active` caveat (load-bearing — keep visible).** The `may_mutate` fence refusal
> (`CasRefLedger.cpp:1335`) runs INSIDE the append-leader tenure (`leader_active = true` at `:1165`).
> The wait MUST go at the entry BEFORE the lane becomes leader — never at the `may_mutate` line —
> because the self-remount's own `refLanesSettledForRemount` (`:837`, called at `CasPool.cpp:1022`)
> and clean-shutdown's `drainRefLanesForShutdown` (`:1011`) both block on every lane being
> `!leader_active`. A wait held inside leader tenure would keep `leader_active` set while waiting for
> the fence to re-arm, but the fence only re-arms after those settle-waits complete — the remount
> would stall until the bound expired (no benefit, and a delayed remount/shutdown). Placed before
> leadership, the waiter holds no `leader_active` and no ledger lock, the remount settles + re-arms +
> wakes it, and it then takes leadership and appends under the fresh (re-recovered) runtime — so the
> `superseded_by_remount` refusal (`:1349`) is not hit either. The bound (`write_grace_ms` < TTL)
> stays as defense-in-depth.

- [ ] **Step 1: Failing test.** In a ref-lane gtest: open with `write_grace_ms > 0`, enqueue an
append while the fence is down, remount from another thread, assert the append completes (no
`NETWORK_ERROR`); a second case with the feature off asserts the append fails with the retry-later
`NETWORK_ERROR`.

- [ ] **Step 2: Build + run — red.**
```bash
ninja -C build src/unit_tests_dbms > build/ninja_task6_test.log 2>&1; echo "exit=$?"
```

- [ ] **Step 3: Implement.**
3a. `CasRefLedger.h`: add `std::function<void()> wait_for_write_grace;` next to `may_mutate` (`:370`)
and to the ctor parameter list (`:101` area).
3b. `CasRefLedger.cpp` `appendRefOps`, at the very top (before `std::unique_lock lk(ref_queue_mutex)`
at `:1119`, so the wait holds no lane lock and no leadership):
```cpp
    /// Part B (opt-in): if the fence is transiently down, ride out the self-remount window BEFORE
    /// taking the queue lock or leadership — never while leader_active is set (spec B-nodeadlock: the
    /// remount's refLanesSettledForRemount blocks on !leader_active). A no-op when write_grace_ms==0
    /// or the fence is held. On return the may_mutate gate below is unchanged.
    if (wait_for_write_grace && !may_mutate())
        wait_for_write_grace();
```
3c. `CasPool.cpp` where the ledger is constructed (the `may_mutate`/`checkFenceOrThrow` lambda block
at `:171`): pass `[this] { mount_runtime.waitForWriteGrace(); }` as `wait_for_write_grace`.

- [ ] **Step 4: Build + run green + gate.**
```bash
ninja -C build > build/ninja_task6_impl.log 2>&1; echo "exit=$?"
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*' > build/test_task6_gate.log 2>&1; echo "exit=$?"
```

- [ ] **Step 5: Commit** (message: "ca: Part B — surface 2 (ref-log append) waits out the fence
window at the entry, before leader tenure"; note the `leader_active` caveat in the body; standard
trailers).

---

## Plan self-review checklist {#self-review}

- Coverage vs. spec: Part A = Tasks 1–2 (Mode 1 A2/A3/A4/A6 + Mode 2 M1/M2/M3 + 5 counters);
  Part B = Tasks 3–6 (setting → primitive → surface 1 → surface 2). Every spec §B-tests scenario maps
  to a Task-4 test (incl. the Mode-2 ride-out); every §A / §A.5 line maps to Task-1/Task-2.
- Names match the spec exactly: `write_grace_ms`, `waitForWriteGrace`, `CasMountFenceArmed`,
  `CasSelfRemountCompleted`, `CasSelfRemountMicroseconds`, `CasMountFenceExpired`,
  `CasMountFenceExpiredMicroseconds`, `fence_tripped_boot_ms`, `fence_expiry_reported_boot_ms`.
- rev.2 (silent-expiry path): Mode 2 is covered in Task 1 (M1/M2/M3 + 2 counters) and the
  `setMountDeadline` signaler + Mode-2 ride-out test are in Task 4. The `setMountDeadline` signaler is
  the load-bearing addition — without it Part B never wakes on the dominant path.
- No placeholders, no "TBD"; every task has exact files, complete snippets, commands, and a commit.
- Sanitizer discipline: no test `EXPECT_THROW`s a `LOGICAL_ERROR`; Part B failures are
  `INVALID_STATE`/`NETWORK_ERROR` (safe to `EXPECT_THROW`).
- The `leader_active` caveat is stated verbatim in Task 6 and in spec §B.3/§B.5.
