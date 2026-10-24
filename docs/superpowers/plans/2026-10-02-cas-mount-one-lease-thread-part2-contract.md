# Contract for plan part 2: one lease thread, one renewal path

Companion of `docs/superpowers/plans/2026-10-02-cas-mount-one-lease-thread-part2.md`. The rulings on its open points are at the end.

Binding for the five task writers. Spec: `docs/superpowers/specs/2026-10-01-cas-mount-renewal-without-lease-deadline-design.md`.
Model: `docs/superpowers/models/CaDiskLifecycle.tla`. Code: worktree `/home/mfilimonov/workspace/ClickHouse/cas-95-1`,
branch `fix/antalya-26.6/cas-mount-renewal-no-deadline`, HEAD `ee58abb6969` plus uncommitted edits to
`CasRequests.cpp`, `gtest_cas_heartbeat.cpp`, `gtest_cas_pool.cpp` (test line numbers may shift by a few lines; the function or test name is the anchor).
The plan's base is `7e243502408` on `refactor/antalya-26.6/cas-mount-one-lease-thread`: the four commits after `ee58abb6969` change tests, comments in two headers and docs only, and no production `.cpp`.

Paths: `CA` = `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`.
`R` = `CA/Pool/CasMountRuntime.cpp`, `RH` = its header, `P` = `CA/Pool/CasPool.cpp`, `PH` = its header,
`S` = `CA/Pool/CasServerRoot.cpp`, `SH` = its header, `T` = `src/Disks/tests`.

Tasks are named after the spec's steps: **T4**, **T5a**, **T5b**, **T6**, **T7+8**.

## 1. Today's shape (after step 3) {#today}

### 1.1 Threads

| Thread | Body | Created | Joined |
|---|---|---|---|
| `CAS_LEASE_RENEWER` | `CasMountRuntime::renewalLoop` R:736 | `startBackgroundWorkers` R:699, from `Pool::mountWritable` P:921 iff `background_watermark` | `stopBackgroundWorkers` R:940, from `~Pool` phase 1 P:1029, `Pool::forgetDisk` (5a) P:1190, `finishTeardown` R:1220 |
| `CAS_REMOUNT` | `CasMountRuntime::remountLoop` R:837 | R:700, same call | R:942, same call |
| opening thread | claim, `startRenewer` P:805, `renewRenewerForStartupOnce` P:906, `armMountFence` P:909 | caller of `Pool::open` | n/a |
| test thread | `renewWatermarkOnce` R:287 | tests | n/a |

Both workers wait for `worker_loops_released` (R:744, R:842) so neither runs before both exist. `background_watermark` is
`context_available && !read_only` (`ContentAddressedMetadataStorage.cpp:782`); `openForDecommission` forces it on (P:941).

### 1.2 `RenewalDriverState` (RH:50, guarded by `driver_mutex`)

| From | To | Where |
|---|---|---|
| `Dormant` | `StartupCall`, `DirectCall` | `admitRenewerCall` R:447; `startRenewer` R:487 |
| `StartupCall`, `DirectCall` | `Dormant` | `DriverLease::finish` R:400; `~DriverLease` R:385 |
| `Dormant` | `WorkerIdle` | `startBackgroundWorkers` R:692 |
| `WorkerIdle` | `WorkerCall` | `renewalLoop` R:798 |
| `WorkerCall` | `WorkerIdle` | `finish` R:400 |
| `WorkerCall` | `ParkRequested` | `scheduleRemount` R:1169; `remountLoop` R:864 |
| `WorkerIdle` | `Parked` | `scheduleRemount` R:1171; `remountLoop` R:866; `finish` on a terminal result that raised a generation R:414 |
| `ParkRequested` | `Parked` | `renewalLoop` top R:761; `finish` R:398; `~DriverLease` R:379 |
| `Parked` | `RemountCall` | `startRenewer` R:492; `admitRenewerCall` from `renewRenewerForRemountOnce` R:655 |
| `RemountCall` | `Parked` | `finish`; `~DriverLease` R:383 |
| `Parked` | `WorkerIdle` | `remountLoop` after an acknowledged reclaim, if `Live` and renewer `Active` R:916 |
| any | `Stopping` | `stopBackgroundWorkers` R:934; failed start R:708; `beginShutdownForTest` R:1186; `finish` R:395 and `~DriverLease` R:376 under a stop |
| `Stopping` | `Dormant` | `stopBackgroundWorkers` R:949; failed start R:719 |

`installRenewer` (R:466) and `renewerReset` (R:665) require `Dormant` or `Parked`.

### 1.3 Renewal entry points

| Entry | Caller | Plane, policy | On `Terminal` |
|---|---|---|---|
| `renewalLoop` -> `renewRenewerOnce(WorkerCall)` R:807 | lease thread | `lease_requests`, `UntilDefinitive` | `finish` R:406: `tripMountLost`, generation +1 if background, not stopping, not terminal |
| `renewRenewerForStartupOnce` R:644 | `mountWritable` P:906 | `mount_requests`, `LeaseBound` | `tripFenceWithoutOperationalLoss` R:420; rethrown |
| `renewRenewerForRemountOnce` R:653 | `tryRemountOnce` P:1567 | `farewell_requests` through `renewForRemount` S:1587, `LeaseBound`, report deferred | rethrown only; the reclaim fails and is retried |
| `renewWatermarkOnce` R:287 through `Pool::renewWatermarkOnce` P:1611 | tests only: 221 call sites in 33 files | `mount_requests`, `LeaseBound` | `tripMountLost`, counted; generation only if background; rethrown |
| `startRenewer` R:474 -> `MountLeaseRenewer::start` S:1461 (a claim, not a renewal) | `mountWritable` P:805; `tryRemountOnce` P:1516 | `farewell_requests`, `Retry::standard` | throws |

All four renewals end in `MountLeaseRenewer::renewOn` S:1592. `renewRenewerOnce` R:617 calls `DriverLease::finish` (R:637)
before `consumeRenewResult` (R:640), so a parked worker's committed deadline is published after the remount worker was
released to run.

### 1.4 Where the fence is armed or tripped

| Action | Function | Callers |
|---|---|---|
| arm | `armMountFence` R:258: deadline, `lease_expired_at`, generation +1, then `lost = false` | `mountWritable` P:909; `tryRemountOnce` P:1573; tests (44 call lines) |
| report `Live` | `noteRemounted` R:1002 | `tryRemountOnce` P:1580, after `armMountFence`: today `lost` is cleared before `Live` |
| deadline only | `publishRenewedDeadline` R:222 | `consumeRenewResult` R:581 |
| deadline only | `setMountDeadline` R:253 | tests through `Pool::setMountDeadline` |
| trip, loss counted | `tripMountLost` R:105 (skips the count under a published intent R:114) | `DriverLease::finish` R:406; `renewalLoop` catch R:829; `Pool::reportImpossibleInterference` P:1798; `Pool::forgetDisk` P:1180 and P:1198; `Pool::tripMountLost` P:319 (tests) |
| trip, no count | `tripFenceWithoutOperationalLoss` R:118 | `finish` R:420; failed worker start R:721 |
| lifecycle only | `noteLeaseLost` R:981 | `tripMountLost` R:115; `tryRemountOnce` step 0 P:1349 (no fence trip there) |

### 1.5 Remount generations (`remount_requested_generation`, `remount_handled_generation`, RH:533)

| Role | Where |
|---|---|
| produce | `scheduleRemount` R:1167, from `reportImpossibleInterference` P:1799 and `scheduleRemountForTest` R:1177 |
| produce | `DriverLease::finish` R:412 on a terminal worker or direct renewal |
| consume | `remountLoop`: waits `requested > handled` R:857, snapshots R:861, acknowledges `max(handled, snapshot)` R:909, loops on a newer one R:910 |
| read | `scheduleRemountForTest` R:1179; `remountRequestedGenerationForTest` R:1215 |
| not read | `renewalLive` R:530 (uses `ParkRequested` / `Parked`); `armMountFence`; `tryRemountOnce` |

### 1.6 Lock order (outer to inner)

| Lock | Holder | Inner locks taken under it |
|---|---|---|
| `ContentAddressedMetadataStorage::lifecycle_mutex` | FORGET (`...MetadataStorage.cpp:1053`), GC STOP / START, FSCK | all below |
| `Pool::remount_mutex` PH:1282 | `tryRemountOnce` P:1330; `forgetDisk` step 6 P:1222 | ref-ledger locks (`quiesceRefTablesForRemount` P:1540), `driver_mutex` (`enterVanished`, `enterIdentityLost` through `lockTerminalPublication` R:1015) |
| hot-key lane lock | a mount-plane retry pause | `driver_mutex`, in `sleepInterruptibly` R:548 (RH:78) |
| `driver_mutex` RH:525 | both workers, every renewal's admission, terminal publication | none today. Never held across renewer I/O, the remount callback, logging or a join |
| `renew_failure_mutex` RH:552 | `noteRenewRequest` R:213, `publishRenewedDeadline` R:239, `lastRenewFailure` R:198 | none |
| `builds_mutex` RH:505 | `minActive` R:277, called from the renewal body on the renewing thread | none |

The ref ledger calls `reportImpossibleInterference`, which takes `driver_mutex` (P:1799). Whether a ledger lock is held at
that call was not checked.

### 1.7 Request planes (P:189, PH:1212)

| Plane | Fence | Sleep | Users |
|---|---|---|---|
| `mount_requests` | generation, `admit`, `checkFenceOrThrow` | `mountPlaneSleepFn` -> `sleepInterruptibly`: wakes on a stop only | bounded renewals (S:1584), plain objects, manifest reader, ref ledger, catalog listings P:1904 |
| `farewell_requests` | `Fence::open` | engine default, not interruptible | renewer `open_requests`: `start` S:1472, `renewForRemount` S:1589, `release` S:1832 |
| `lease_requests` | `Fence::open` | `mountPlaneSleepFn` | the worker's renewal (`worker_requests`, S:1584) |
| `gc_requests` (`openRequests` PH:774) | generation 0; refuses after `beginTeardown` P:209 | `openPlaneSleepFn` | GC, tools, probes, every owner / epoch / claim step of open (P:609 to P:814) and remount (P:1366 to P:1474) |

FORGET does not arm the `gc_requests` fence (P:1166); server shutdown does.

## 2. Target shape {#target}

### 2.1 After T4 (step 4): one thread, reclaim in the loop

Removed:

- `enum class RenewalDriverState`; `class DriverLease`; `struct AdmittedRenewerCall`; `admitRenewerCall`; `renewalWorkerMayRenew`.
- `remountLoop`; `remount_worker`; `renewal_driver_state`; `workers_starting`; `worker_loops_released`.
- `renewalDriverStateForTest`; `waitForRenewalDriverStateForTest`.
- `MountConfig::remount_parked_hook_for_test`; `MountConfig::renewal_terminal_deposited_hook_for_test`.
- `ThreadName::CAS_REMOUNT` (`src/Common/setThreadName.h:41`, a fork-added entry in a shared file).

Changed:

```cpp
// RH, private. `RenewCaller` replaces the `RenewalDriverState` arguments and is removed in T5b.
enum class RenewCaller : uint8_t { Loop, Startup, Remount, Direct };
MountRenewResult renewRenewerOnce(RenewCaller caller);            // was (AdmittedRenewerCall, RenewalDriverState, bool) -> uint64_t
MountRenewOperationEnvironment renewalEnvironment(RenewCaller caller);   // was (bool worker_call)
bool renewalLive(RenewCaller caller) const;                        // was (bool worker_call)
void consumeRenewResult(const MountRenewResult & result, RenewCaller caller);   // was 4 parameters
// RH, MountConfig: the hook of the loop's wait predicate keeps its place and loses the word "parked".
std::function<void()> lease_wait_predicate_false_hook_for_test = {};    // was renewal_parked_predicate_false_hook_for_test
```

`renewWatermarkOnce`, `renewRenewerForStartupOnce`, `renewRenewerForRemountOnce`, `startRenewer`, `installRenewer`,
`renewerReset`, `startBackgroundWorkers`, `stopBackgroundWorkers`, `scheduleRemount`, `finishTeardown` keep their signatures.
`armMountFence` keeps its signature and stays the unconditional primitive tests use; production stops calling it.

Added:

```cpp
// RH, public
/// Step 0 of a reclaim. One step under `driver_mutex`: snapshot the requested generation and latch the fence.
void beginReclaim();
/// End of a successful reclaim, and (from T5a) of an open's claim. One step under `driver_mutex`: acknowledge the
/// snapshot, publish the deadline, and arm the fence and report `Live` if the arming rule holds. Returns whether it armed.
bool armIfAdmissible(uint64_t deadline_boot_ms);
// RH, private
bool canArm(uint64_t deadline_boot_ms) const;                      // requires driver_mutex; the arming rule
void armFence(uint64_t deadline_boot_ms, bool report_live);        // shared body of armMountFence and the production arm
void checkRenewerOwner() const;                                    // requires driver_mutex; LOGICAL_ERROR off the lease thread while it runs
uint64_t reclaim_generation = 0;                                   // guarded by driver_mutex
std::thread::id lease_thread_id;                                   // guarded by driver_mutex
```

Loop (members are real; error handling as today's `renewalLoop` catch):

```cpp
void CasMountRuntime::renewalLoop()
{
    setThreadName(ThreadName::CAS_LEASE_RENEWER);
    LockMemoryExceptionInThread memory_exception_lock(VariableContext::Global);
    uint64_t backoff_ms = 1000;
    while (true)
    {
        if (config.renewal_before_driver_lock_hook_for_test) config.renewal_before_driver_lock_hook_for_test();
        bool reclaim = false;
        uint64_t snapshot = 0;
        {
            std::unique_lock lock(driver_mutex);
            if (workers_stop_requested || remountTerminal()) return;
            if (remount_requested_generation > remount_handled_generation)
            {
                reclaim = true;
                snapshot = remount_requested_generation;
            }
            else if (!mount_renewer || mount_renewer->state() != MountLeaseRenewerState::Active)
            {
                driver_cv.wait(lock, [&] { /* lease_wait_predicate_false_hook_for_test when false */
                    return workers_stop_requested || remountTerminal()
                        || remount_requested_generation > remount_handled_generation; });
                continue;
            }
            else
            {
                const uint64_t due = saturating(mount_renewer->lastCommittedAttemptStartBootMs() + period_ms);
                const uint64_t now = bootMsNow();
                if (now < due) { driver_cv.wait_for(lock, std::chrono::milliseconds(due - now)); continue; }
            }
        }
        if (reclaim)
        {
            bool reclaimed = false;
            try { reclaimed = remount_attempt(); } catch (...) { tryLogCurrentException(...); }
            std::unique_lock lock(driver_mutex);
            if (reclaimed)
            {
                /// As today. `armIfAdmissible` acknowledged the later snapshot already; this one keeps a
                /// callback that returns true without it from spinning the loop.
                remount_handled_generation = std::max(remount_handled_generation, snapshot);
                backoff_ms = 1000;
                continue;
            }
            driver_cv.wait_for(lock, std::chrono::milliseconds(backoff_ms),
                [&] { return workers_stop_requested || remountTerminal(); });
            backoff_ms = std::min<uint64_t>(backoff_ms * 2, 30000);
            continue;                                             // the top of the loop decides about the exit
        }
        if (config.renewal_admitted_hook_for_test) config.renewal_admitted_hook_for_test();
        (void)renewRenewerOnce(RenewCaller::Loop);                // renew, consume; no hand-off follows
    }
}

void CasMountRuntime::beginReclaim()
{
    std::lock_guard lock(driver_mutex);
    reclaim_generation = remount_requested_generation;
    tripMountLost();                    // atomics only; counts a loss once per Live -> TransientNotLive, none under an intent
}

bool CasMountRuntime::armIfAdmissible(uint64_t deadline_boot_ms)
{
    std::lock_guard lock(driver_mutex);
    remount_handled_generation = std::max(remount_handled_generation, reclaim_generation);
    const bool arm = canArm(deadline_boot_ms);
    if (arm) armFence(deadline_boot_ms, /*report_live=*/true);
    else setMountDeadline(deadline_boot_ms);
    driver_cv.notify_all();
    return arm;
}

// consumeRenewResult(result, RenewCaller::Loop), under driver_mutex; the report follows outside it:
//   Committed:    expired_ms = publishRenewedDeadline(start + ttl)
//   NotAttempted: nothing
//   Terminal:     if (workers_stop_requested) tripFenceWithoutOperationalLoss();
//                 else { tripMountLost(); ++schedule_remount_calls_for_test;
//                        if (!remountTerminal() && remount_requested_generation == remount_handled_generation)
//                            ++remount_requested_generation; }
//   notify_all
```

| Item | T4 rule |
|---|---|
| `canArm` | `!workers_stop_requested && !remountTerminal() && remount_requested_generation <= remount_handled_generation`. The deadline term comes in T5a; in T4 the re-anchor keeps the claim fresh |
| `armFence` order | deadline, `lease_expired_at_boot_ms` reset, `fence_generation` +1, interposition hook, `noteRemounted` if `report_live`, `lost = false` |
| `renewalLive(Loop)` | `!workers_stop_requested`, no pending request, `lifecycle() == Live`, `!lost`, test hook. The last two stay until T5a; FORGET's trip still ends the renewal through `lost` |
| `renewalLive(other)` | `!workers_stop_requested`, test hook: unchanged |
| `scheduleRemount` | counter, then under `driver_mutex`: return on stop or terminal, generation +1, `notify_all`. No state edits |
| `Pool::tryRemountOnce` | `mount_runtime.beginReclaim()` replaces `noteLeaseLost()` at P:1349; `mount_runtime.armIfAdmissible(deadline)` replaces `armMountFence` + `noteRemounted` at P:1573 to P:1580; still returns `true` for a reclaim that claimed and did not arm |
| `installRenewer`, `renewerReset`, `startRenewer` | `checkRenewerOwner` replaces the `Dormant` / `Parked` tests |
| `renewWatermarkOnce` | keeps refusing a background-configured runtime (R:433); calls `renewRenewerOnce(RenewCaller::Direct)` and rethrows a terminal failure |
| start | one thread; on a factory failure: stop flag, no join needed, `tripFenceWithoutOperationalLoss`, rethrow |
| `Pool::forgetDisk` chassert P:1153 | names `CAS_LEASE_RENEWER` instead of `CAS_REMOUNT`: FORGET must not run on the lease thread |

New critical sections, all under `driver_mutex`, atomics and no-trace counters only, no I/O, no log, no join:
`beginReclaim`, `armIfAdmissible`, the consume step. New nesting: `driver_mutex` -> `renew_failure_mutex` inside
`publishRenewedDeadline`; its `LOG_WARNING` (R:246) moves after the unlock (return the text, log in the caller).
`remount_mutex` -> `driver_mutex` is the existing order and covers both new calls from `tryRemountOnce`.

### 2.2 After T5a and T5b (step 5): one renewal path

Added in T5a:

```cpp
// RH, public
/// Blocks until the fence is armed. Gives up after `timeout_ms` on the fence clock, on a stop, on a terminal
/// lifecycle, or when no lease thread runs. Returns whether the fence is armed.
bool waitUntilArmed(uint64_t timeout_ms) const;
```

Changed in T5a:

| Item | Rule |
|---|---|
| `canArm` | adds: the deadline admits a ref append now, by the arithmetic of `refAppendFenceOk` (R:162) applied to `deadline_boot_ms` instead of the fence. One private helper serves both |
| `armIfAdmissible` | when it does not arm and the fence is not lost, it latches it with `tripFenceWithoutOperationalLoss` (the open's closed state) |
| consume, `Committed` | after `publishRenewedDeadline`: `if (mount_fence.lost && canArm(deadline)) armFence(deadline, true)` |
| `renewalLive(Loop)` | `!workers_stop_requested`, no pending request, `!remountTerminal()`, test hook. `lost` and `lifecycle() != Live` go (rule 7) |
| `noteRenewRequest` | `driver_cv.notify_all()` on a failed request, so `waitUntilArmed` re-reads an injected clock |
| `Pool::mountWritable` P:865 to P:922 | `setLiveWriterEpoch`; `armed = armIfAdmissible(claim start + ttl)`; start the thread; if not armed: `waitUntilArmed(ttl)`, and on failure `stopBackgroundWorkers` and throw `ABORTED` with `lastRenewFailure`. No lease thread and not armed: see O1 |
| `Pool::tryRemountOnce` P:1544 to P:1568 | the window test and the `renewer_redo` step go; `armIfAdmissible` decides |

After T5a `renewRenewerForStartupOnce` and `renewRenewerForRemountOnce` have no production caller.

Removed in T5b:

- `renewRenewerForStartupOnce`; `renewRenewerForRemountOnce`; `RenewCaller`.
- `MountLeaseRenewer::renewForRemount`; `MountLeaseRenewer::renewOn` (its body becomes `renew`); `enum class MountRenewPolicy`;
  `MountRenewOperationEnvironment::policy`; the renewer's `mount_requests` member and constructor parameter.
- `CasMountRuntime::mount_requests` and its constructor parameter.
- `MountRenewResult::deadline_source`; the classifications `ExternalLeaseDeadline` and `RequestDeadline`;
  `ProfileEvents::CASMountRenewalDeadlineExceeded`. All three are unreachable once no renewal is lease-bound. See O2.

Changed in T5b:

```cpp
// RH, private
MountRenewResult renewOnce();                              // the one renewal step: renew, consume, report
MountRenewOperationEnvironment renewalEnvironment();
bool renewalLive() const;
void consumeRenewResult(const MountRenewResult & result);
// RH, public: the test seam. Refuses a background-configured runtime; renewOnce(); rethrows a terminal failure.
void renewWatermarkOnce();
// RH
CasMountRuntime(BackendPtr backend_ptr_, CasRequests & farewell_requests_, CasRequests & lease_requests_,
    const Layout & layout_, MountConfig config_, String server_root_id_, const CasEventSink & event_sink_,
    CasRequestBudget cas_request_budget_, std::function<bool()> remount_attempt_);
// SH: `worker_requests` is renamed `lease_requests`; `open_requests` keeps its name until T7+8
MountLeaseRenewer(CasRequests & open_requests_, CasRequests & lease_requests_, const Layout & layout_,
    const String & srid_, UInt128 server_uuid_, uint64_t writer_epoch_, std::chrono::milliseconds ttl_,
    std::function<uint64_t()> now_ms_fn_, std::function<uint64_t()> min_active_build_sequence_fn_,
    CasEventSink event_sink_ = {}, std::chrono::milliseconds lease_safety_margin_ = std::chrono::milliseconds(2000),
    std::function<uint64_t()> boot_ms_fn_ = {});
MountRenewResult renew(const MountRenewOperationEnvironment & environment);   // lease plane, Retry::untilDefinitive
```

Loop after step 5: the T4 loop with `renewOnce()` in place of `renewRenewerOnce(RenewCaller::Loop)`. No branch for
"renew at once": whenever a deadline does not admit a ref append, `last start + period` has already passed, because
`validateCasRequestBudget` enforces `period + 2 x envelope + margin < TTL` for every mount with a lease thread.

### 2.3 After T6 (step 6): the renewal returns what the report needs

Removed: `MountRenewObservabilityContext`, `MountRenewObservabilityConfiguration`, `MountRenewObservabilityStack`, the
`thread_local` (S:163), `currentMountRenewObservability`, `markMountRenewTermination`, `MountRenewObservabilityRegistration`,
`beginMountRenewObservabilityCall`, `abandonMountRenewObservabilityCall`, `MountRenewObservabilityCallGuard`,
`initializeMountRenewObservability`, `configureMountRenewObservability`, `deliverDeferredMountRenewObservability` and its call at
P:1268, the `remount_attempt_no` detail of a renew event, `MountLeaseRenewer::confirmed_deadline_boot_ms`.

Changed:

```cpp
// SH: moved from S:103, without the two deadline values
enum class MountRenewTerminalClassification : uint8_t
    { Unclassified, DeterministicFailure, Conflict, Vanished, Cancelled, FenceOrLifecycleLost, Unresolved };
struct MountRenewResult
{
    MountRenewOutcome outcome = MountRenewOutcome::Terminal;
    uint64_t attempt_start_boot_ms = 0;
    uint32_t attempts_sent = 0;
    bool resolved_by_read = false;
    bool sent_any = false;                                   // removed in T7+8
    std::exception_ptr failure;
    uint64_t writer_epoch = 0;                               // what the audit event names
    uint64_t seq = 0;
    UInt128 write_attempt_id{};
    MountRenewTerminalClassification classification = MountRenewTerminalClassification::Unclassified;
    uint64_t elapsed_ms = 0;                                 // boot clock at return minus attempt_start_boot_ms
};
/// One event and one log line for a renewal that retried, was resolved by a read, restored an expired lease, or
/// ended terminal; silent otherwise. Never throws.
void reportMountRenewCompletion(const MountRenewResult & result, const String & server_root_id,
    const CasEventSink & event_sink, uint64_t lease_deadline_before_boot_ms, std::optional<uint64_t> expired_ms) noexcept;
// SH, MountLeaseRenewer
void release(uint64_t lease_deadline_boot_ms);               // was release(); bounds the farewell as confirmed_deadline_boot_ms did
```

`renewOnce` samples `mount_fence.deadline_boot_ms` before `renew`, consumes under `driver_mutex`, then calls
`reportMountRenewCompletion` with no lock held. Event fields and log texts stay byte-compatible:
`remaining_confirmed_budget_ms` and `confirmed_deadline_boot_ms` in the text are computed from
`lease_deadline_before_boot_ms`. `finishTeardown` passes the deadline to `release`: see O5.

### 2.4 Publication order

| Sequence | Order |
|---|---|
| arm (`armFence`) | deadline, `lease_expired_at_boot_ms`, `fence_generation`, `Live`, `lost = false`, `notify_all`; all inside the caller's `driver_mutex` section |
| committed renewal | `publishRenewedDeadline` (its own two orders, R:226 to R:236, unchanged), arm if the rule holds, `notify_all`, unlock, report. The loop reads the request generations only after this, so no reclaim can run between a commit and its deadline |
| terminal renewal | trip, then the generation, one section |
| reclaim | `beginReclaim`, identity probe, owner, epoch, claim, `installRenewer`, `startRenewer`, `setLiveWriterEpoch`, cancel recoveries, quiesce, `armIfAdmissible` |
| FORGET | intent (under `driver_mutex`), trip, GC stop and join, stop and join the lease thread, trip, drain, farewell, `renewerReset`, `Vanished`. Unchanged; after the intent no section can arm |

### 2.5 Choices the spec's pseudocode leaves open

| # | Choice | Recommendation | Reason | Cost if wrong |
|---|---|---|---|---|
| C1 | Where "latch the fence" and the snapshot live | In `Pool::tryRemountOnce` through `beginReclaim`, not in the loop | 37 test call lines drive `tryRemountOnce` directly; the latch and the snapshot stay one section | none for safety |
| C2 | Where the generation is acknowledged | In `armIfAdmissible`, the same section as the arm, with `beginReclaim`'s snapshot; the loop repeats it with its own, older snapshot | the model's `LReclaim` is one step; a direct `tryRemountOnce` has no loop | an acknowledgement of anything newer than the snapshot drops a request (`sab_ackrequested`) |
| C3 | What "the fence is not armed" means for a renewal | `mount_fence.lost`, not the lifecycle | at an open the lifecycle is `Live` with the fence latched; the model's `LRenewOk` tests `Transient` and has no open | see O4 |
| C4 | "the loop renews at once" | no branch; the cadence is already due (2.2) | one rule for the cadence | a mount opened with an unvalidated budget waits up to one period |
| C5 | "unless one is already pending" | `remount_requested_generation == remount_handled_generation` in the consume section | the model's `Pending` | a second generation for one loss: one extra reclaim |
| C6 | Trip on an early end | stop: `tripFenceWithoutOperationalLoss`; anything else: `tripMountLost` | `tripMountLost` already skips the count under an intent and is a no-op count under `IdentityLost` | a FORGET counted as a lease loss |
| C7 | No `Active` renewer and no request | wait for a request, a stop or a terminal lifecycle | the model's `I8RenewsActiveRenewer`; today's parked wait | none; the state is not reachable by the model |
| C8 | Does a newer request cut the reclaim backoff | no | today's behaviour | up to 30 s before a second reclaim |
| C9 | Clock of the open's readiness wait | fence clock, re-read on `driver_cv` wake-ups and at the real timeout | works with an injected clock; no polling | a fixture whose renewal makes no request never wakes the open: bound fixtures by request count |
| C10 | Result of a reclaim that claimed and did not arm | `tryRemountOnce` returns `true`; event per O6 | the loop must not back off; the request is acknowledged | a retry loop of fresh epochs if it returned `false` |
| C11 | Does a remount request wake the retry wait | no | rule 8; see O7 | up to 1.2 s more before a reclaim |

## 3. Task split {#tasks}

Split of step 5: T5a changes behaviour (readiness, rule 7) and is reviewed as lease-keeper code; T5b only deletes paths
T5a left without callers and sweeps about 60 test sites. A reviewer can reject either alone. Merge of steps 7 and 8:
neither has a failing-first test; both are one commit per item with the unit gate as the only cycle.

Gate after every task: the tree builds and the gtest filter `CAS*` passes on that build.

### T4. One thread {#t4}

**Goal.** One lease thread renews and reclaims in sequence; the driver states and the remount worker go. The reclaim
starts by latching the fence and arms only under the arming rule; the three bounded renewals stay as plain calls.

| | |
|---|---|
| Files | `RH`, `R`, `P` (`tryRemountOnce`, `forgetDisk` comments and chassert), `PH` comments, `src/Common/setThreadName.h`, `T/gtest_cas_pool.cpp`, `T/gtest_cas_forget.cpp`, `docs/en/antalya/cas/architecture/mounts-and-leases.md` (two-worker and parking text) |
| Consumes | the tree at HEAD |
| Produces | `beginReclaim`, `armIfAdmissible`, `canArm`, `armFence`, `checkRenewerOwner`, `reclaim_generation`, `lease_thread_id`, `RenewCaller`, `renewRenewerOnce(RenewCaller)`, `consumeRenewResult(result, RenewCaller)`, `lease_wait_predicate_false_hook_for_test` |

New tests (all in `gtest_cas_pool.cpp`, fixture `RuntimeUnderTest` + `RuntimeRenewBackend`):

| Test | Pins | Fails before T4 because |
|---|---|---|
| `CASMountRuntime.ARemountRequestEndsTheRenewalAndTheSameThreadReclaims` | rules 7, 13; spec test 15 first half | the renewal `PUT` and the reclaim callback run on two thread ids; `worker_factory` is called twice |
| `CASMountRuntime.NothingArmsWhileARemountRequestIsPending` | rule 14; `NoArmWhileRequestPending` (`sab_reclaimarmpending`), `I4aTransientImpliesLost` (`sab_reclaimnotrip`) | a request raised inside the first reclaim: the first reclaim re-arms and a write is admitted before the second |
| `CASMountRuntime.AReclaimAcknowledgesOnlyTheGenerationItServed` | rule 13; `I7RequestNotLost` (`sab_ackrequested`). One report: one generation, one epoch change, one loss count, also over failed retries | passes today (regression); it is `ConcurrentRemountRequestIsProcessedAfterActiveGeneration` plus the counts |
| `CASMountRuntime.AReclaimFinishedAfterTheForgetIntentArmsNothing` | rule 14; `NoJoinWindowReclaim` (`sab_armafterintent`) | the reclaim arms after `publishVanishedIntent`; `mayMutate` is true until the second trip |
| `CASMountRuntime.StopDuringAReclaimJoinsTheThread` | rule 13; spec test 16: join when the attempt returns, fence tripped with no loss count, no farewell for the unclaimed slot | a reclaim finishing after the stop arms the fence |
| `CASMountRuntime.ACommitConsumedAfterARemountRequestCannotOverwriteTheReclaimedDeadline` | the deferred review finding | the worker's `publishRenewedDeadline` runs after `finish` released the remount worker; with the fixture's clock gate the deadline after the reclaim is the older renewal's |
| `CASMountRuntime.OnlyTheLeaseThreadReplacesTheRenewerWhileItRuns` | rule 13 | `installRenewer` from a test thread is refused by a driver state, not by ownership: the message assertion fails |

Existing tests: see section 4, column T4. Keep green untouched: every `CASHeartbeat*`, `CASEvent*`, `CASMountLease*`,
`CASLifecycle*`, `CASForget*` except the one adapted, and all tests of T5a's list (the re-anchors still run).

Hazards for the reviewer:

- `armFence` publishes `Live` before `lost = false`; `RearmPublishesTheNewGenerationBeforeOpeningTheFence` must stay green.
- `tryRemountOnce` runs under the thread's `LockMemoryExceptionInThread`: a reclaim no longer fails on a memory limit.
- The deferred report of `renewRenewerForRemountOnce` now lands on the lease thread's stack; `tryRemountOnce`'s finalizer still drains it.
- A stop cannot interrupt `claimMountAwaitingExpiry`'s `waitSleep`: the join waits for the observation, as it does today for the remount worker.
- No `sleep` and no real-time bound in new fixtures; bound outage loops by request count through `renewal_live_for_test`.
- `WorkerConstructionRollbackFailsOpenClosed` loops over one factory call, not two.
- `ACommitConsumedAfterARemountRequest...`: its clock gate holds the renewing thread inside `publishRenewedDeadline` until the
  reclaim armed. Arm the gate only when `worker_factory` created two threads; on one thread it must never wait, or the
  test hangs instead of passing.

### T5a. Readiness through the loop {#t5a}

**Goal.** An open returns and a remount reports `Live` only when a ref append is admissible; a too-old claim leaves the
fence latched and the loop's next renewal arms it. A renewal ends early on a stop, a pending request or a terminal
lifecycle, and no longer on a lost fence.

| | |
|---|---|
| Files | `RH`, `R`, `P` (`mountWritable`, `tryRemountOnce`), `T/gtest_cas_pool.cpp`, `T/gtest_cas_forget.cpp`, `docs/en/antalya/cas/architecture/mounts-and-leases.md` |
| Consumes | `armIfAdmissible`, `canArm`, `armFence`, `beginReclaim`, `RenewCaller`, `renewalLive(RenewCaller)` |
| Produces | `waitUntilArmed`; `canArm` with the deadline term; `armIfAdmissible` that latches; `renewalLive(Loop)` without `lost` |

| Test | Pins | Fails before T5a because |
|---|---|---|
| `CASMountRuntime.OpenAndRemountReportReadyOnlyWhenWritable` | rules 1, 14; `LiveOnlyWhenAdmissible` (`sab_liveunready`); spec test 14, its seven cases | the open re-anchors on the opening thread; the remount reports `Live` from its own re-anchor; a stale success arms nothing today but nothing arms later either |
| `CASMountRuntime.ARenewalIsSentUnderALatchedFence` | rule 7 | `renewalLive` ends the renewal on `lost` |
| `CASMountRuntime.TheForgetIntentAloneEndsARenewal` | rule 7; `RenewNotAfterIntent` (`sab_renewafterintent`) | with the intent published and no trip, the renewal keeps retrying until the request bound |
| `CASMountRuntime.ARenewalArmsNothingWhileARequestIsPending` | rule 14; `NoArmWhileRequestPending` (`sab_livependingrequest`) | it does not: no renewal arms before T5a. It guards the new arm: it fails on T5a's code when `canArm` drops the pending term, which the task must show once |
| `CASPool.AnOpenWithoutALeaseThreadFailsWhenItsClaimIsTooOld` | O1, if ruled that way | the open re-anchors and succeeds |

Keep green untouched: `ForgetEndsAnUnboundedRenewal` (the intent now ends it), `StopWakesTheRetryWaitOfAnUnboundedRenewal`,
`AStaleSuccessIsFollowedAtOnceByTheNextRenewal`, the five `CASMountRuntime.Expiry*` tests, `CASLifecycleSnapshot.*`.

Hazards:

- Expiry bookkeeping across a reclaim: `leaseExpiredAt` needs `Live` and `!lost`, so it stays empty during readiness; `armFence` resets `lease_expired_at_boot_ms`; `publishRenewedDeadline` still clears the failure text.
- `waitUntilArmed` holds `driver_mutex` only inside the wait; the open's stop and join run with no lock held.
- A request raised between `armIfAdmissible` and the thread start at an open cannot exist: no caller has the pool yet.
- O4: a trip with no request is re-armed by the next successful renewal.
- The MountRemount event of a reclaim that did not arm (O6).

### T5b. Remove the bounded paths {#t5b}

**Goal.** Delete the startup and remount re-anchors, the lease-bound policy and the fence-gated renewal plane.
`renewWatermarkOnce` stays as the test seam over `renewOnce`.

| | |
|---|---|
| Files | `RH`, `R`, `SH`, `S`, `P`, `PH`, `src/Common/ProfileEvents.cpp` (O2), `T/gtest_cas_pool.cpp`, `T/gtest_cas_heartbeat.cpp`, `T/gtest_cas_mount.cpp`, `T/gtest_cas_mount_claim_conflicts.cpp`, `T/gtest_cas_event_log.cpp`, `T/gtest_cas_observability.cpp`, `T/gtest_cas_gc_ack_floor.cpp`, `docs/en/antalya/cas/operations/{monitoring,troubleshooting,debugging}.md` |
| Consumes | T5a's readiness (no caller of the re-anchors is left) |
| Produces | `renewOnce`, `renewalEnvironment()`, `renewalLive()`, `consumeRenewResult(result)`, the two-plane `MountLeaseRenewer` constructor, the `CasMountRuntime` constructor without `mount_requests_` |

New tests: none. The task's cycle is: delete, adapt, gate.

Hazards:

- `renewWatermarkOnce` no longer gives up at the lease: a caller that injects a transient fault and expects a throw now
  retries with the plane's real sleep. Every such site is listed in section 4; each needs a definitive answer or a
  request bound.
- 45 `MountLeaseRenewer` constructions and `RuntimeUnderTest` lose one argument; `renewalEnvironment` in
  `gtest_cas_heartbeat.cpp` loses `policy`.
- `terminate` still reads `confirmed_deadline_boot_ms`; it goes in T6.

### T6. The renewal returns its result {#t6}

**Goal.** `renew` fills the fields the report needs and `renewOnce` reports them; the thread-local stack and the
renewer's copy of the deadline go.

| | |
|---|---|
| Files | `SH`, `S`, `RH`, `R`, `P` (the finalizer of `tryRemountOnce`), `T/gtest_cas_event_log.cpp`, `T/gtest_cas_heartbeat.cpp`, `T/gtest_cas_mount.cpp`, `T/gtest_cas_pool.cpp` |
| Consumes | `renewOnce`, `consumeRenewResult(result)`, `MountLeaseRenewer::renew` on one plane |
| Produces | the extended `MountRenewResult`, `MountRenewTerminalClassification` in `SH`, `reportMountRenewCompletion` (five parameters), `release(uint64_t)` |

| Test | Pins | Fails before T6 because |
|---|---|---|
| `CASHeartbeat.RenewReturnsWhatItsReportNeeds` | step 6: `seq`, `write_attempt_id`, `writer_epoch`, `classification`, `elapsed_ms` for a commit, a conflict, a vanished slot, an early end | the fields do not exist |
| `CASHeartbeat.FarewellIsBoundByTheDeadlineItIsGiven` | the farewell's lease bound with the deadline as an argument | `release` takes no argument |
| `CASEvent.TheReportIsBuiltFromTheResult` | the report needs no per-thread registration: `MountLeaseRenewer::renew` followed by `reportMountRenewCompletion` emits one event whose fields come from the result | today a renewal nobody configured is `Ignored` (S:189) and the five-parameter function does not exist |

Keep green untouched: `FirstAttemptRenewalIsSilent`, `WatermarkRenewEventsAreBoundedAndComplete`,
`WatermarkRenewSinkFailureCannotChangeOutcome`, the three farewell tests of `gtest_cas_heartbeat.cpp` except for the
`release` argument, `CASMountRuntime.ExpiryIsReported`.

Hazards: `throwRenewConflict` (S:1486) sets the classification through the thread-local today; it needs an out-parameter
or a classifying catch. `emitMountEvent` inside it still runs during the renewal, before any result exists.

### T7+8. One definition each; further cleanups {#t78}

**Goal.** One commit per item, no behaviour change except the dropped duplicate log line.

| Commit | Change | Test impact |
|---|---|---|
| 7a | `2 x envelope` in one place: a `CasRequestBudget` method used by `refAppendFenceOk` (R:167) and `validateCasRequestBudget` (`CasRequestBudget.cpp:53`); `terminate` (S:1774) keeps its own reservation and slack | none |
| 7b | literals: `2000` in `CasRequestBudget.h:23`, `ContentAddressedSettings.cpp:85`, `SH:570` to one constant, `kFarewellSlackMs` (S:1310) stays its own; `90'000` three times in `CasRetry.h:46,53,60` to one | none |
| 7c | stale text: "Startup-only" (`ContentAddressedSettings.cpp:85`, `CasRequestBudget.h:20`, `ContentAddressedMetadataStorage.h:657`), "unclean-boundary marker" (RH:140), `forgetDisk` step (2) comment (P:1177), six timing comments | none |
| 8.1 | delete `process_epoch`, `mintRandomProcessEpoch`, `setProcessEpoch`, `epoch`, `writerEpoch` (RH:168, PH:458, PH:464, P:577, P:653, P:770, P:817, P:1527) | 64 `writerEpoch()` lines in 12 test files become `liveWriterEpoch()`; `gtest_cas_pool.cpp:4043` loses a call |
| 8.2 | drop `terminal_state_published`; `enterVanished` and `setLifecycleForTest` test `isVanished()` under `driver_mutex` | `VanishedReasonPreparationFailureLeavesTerminalTransitionRetryable` untouched |
| 8.3 | drop `renewal_period` (`startBackgroundWorkers()` reads `config.mount_renew_period`) and `MountRenewOperationEnvironment::boot_ms` | 27 `startBackgroundWorkers(` lines set `MountConfig::mount_renew_period` instead; the heartbeat helper drops `.boot_ms` |
| 8.4 | `runRoundLogged(Cas::Gc &, GcRoundLogRecord::Trigger, std::function<void()> on_lease_acquired = {})`; `runOneRoundNow()` | 22 `runOneRoundNow(` lines in 4 files lose the argument |
| 8.5 | `bool Pool::remountTerminal() const`; `CasGcScheduler.cpp:369` and `:485` use it | none |
| 8.6 | drop `MountFence::server_uuid`, `MountFence::writer_epoch` (`armMountFence(uint64_t deadline_boot_ms)`), `backend_ptr`, `Pool::farewellRequests`, `MountRenewResult::sent_any` | 44 `armMountFence(` lines; `gtest_cas_heartbeat.cpp:1074`, `:1128`, `gtest_cas_mount.cpp:2376` |
| 8.7 | drop the second observation log line in `claimMountAwaitingExpiry` | none found |
| 8.8 | drop `setRoundInFlightForTest` (`CasGcScheduler.h:165`) | 2 lines in `gtest_cas_operation_gate.cpp` become assertions over a parked real round |
| 8.9 | rename the renewer's `open_requests` to `claim_farewell_requests` | none |
| 20 | soak card S39 queries every replica (spec test 20) | not in this branch: see O8 |

Consumes: `armFence`, the T5b constructors, the T6 result. Produces nothing later tasks need.
Hazard: 8.1 on a read-only pool: `writerEpoch()` returned the random value, `liveWriterEpoch()` returns 0.

## 4. Test inventory {#tests}

Commands, run in the worktree, output not truncated:

```bash
P='RenewalDriverState|renewalDriverStateForTest|waitForRenewalDriverStateForTest|workersRunningForTest|renewWatermarkOnce|renewRenewerForStartupOnce|renewRenewerForRemountOnce|renewForRemount|remount_parked_hook_for_test|renewal_before_driver_lock_hook_for_test|renewal_admitted_hook_for_test|renewal_terminal_deposited_hook_for_test|renewal_parked_predicate_false_hook_for_test|renewal_live_for_test|MountRenewPolicy|LeaseBound|UntilDefinitive|configureMountRenewObservability|deliverDeferredMountRenewObservability|reportMountRenewCompletion|confirmed_deadline_boot_ms|remountRequestedGenerationForTest|scheduleRemountForTest|beginShutdownForTest|scheduleRemountCallCountForTest|startBackgroundWorkers|stopBackgroundWorkers|startRenewer|installRenewer|renewerReset|armMountFence|setMountDeadline|remount_quiesce_hook_for_test|worker_factory|setArmMountFenceInterpositionHookForTest'
grep -rnE "\b($P)\b" src/Disks/tests        # 519 lines in 45 files; saved as tmp/issue-2431/plan2/grep-step4-6.txt
grep -rn 'tryRemountOnce' src/Disks/tests    # 37 code lines in 9 files
grep -rn 'writerEpoch()' src/Disks/tests     # 64 lines in 12 files
```

Counts by symbol: `renewWatermarkOnce` 221, `armMountFence` 52 (44 code lines), `installRenewer` 33, `startRenewer` 30,
`startBackgroundWorkers` 27, `stopBackgroundWorkers` 27, `scheduleRemountCallCountForTest` 25, `MountRenewPolicy` 18,
`setMountDeadline` 18, `scheduleRemountForTest` 10, `RenewalDriverState` 8, the rest 7 or fewer each.
`MountLeaseRenewer` constructions: 45 (heartbeat 31, mount 12, claim conflicts 1, event log 1).

### Delete (subject unreachable)

| Task | Test | Subject that goes |
|---|---|---|
| T4 | `CASPoolRemount.DirectRenewCannotRaceWorkerStartOrRenewerReplacement` | a driver state refusing start, install and reset during a direct call |
| T4 | `CASPoolRemount.DueWorkerAdmissionIsReservedBeforeParkRequest` | `WorkerCall` -> `ParkRequested` -> `Parked` |
| T4 | `CASPoolRemount.DueWorkerAdmissionIsReservedBeforeStop` | `Stopping` -> `Dormant` |
| T4 | `CASPoolRemount.TerminalDepositionDoesNotTouchRenewerAfterReplacement` | the window between `finish` and `consumeRenewResult` |
| T5a | `CASPoolRemount.StaleRemountAnchorPerformsParkedRedo` | the re-anchor inside the remount |
| T5a | `CASPoolRemount.TheRenewerRedoRenewsOnTheOpenPlane` | the same, on the open plane |
| T5a | `CASPoolRemount.ParkedRedoRecoveryObservabilityPrecedesRemountResult` | deferred report of a renewal under `remount_mutex` |
| T5a | `CASPoolRemount.ParkedRedoFailureObservabilityPrecedesRemountResult` | the same |
| T5b | `CASHeartbeat.DeadlineBeforeSendTerminalizesWithTypedFailure` | a renewal refused by its lease before sending |
| T5b | `CASHeartbeat.RenewalStopsBeforeTheCutoffWhenEveryAttemptConsumesTheEnvelope` | a lease-bound renewal; `RenewalOutlivesTheReservationCutoff` replaced it in step 3 |
| T5b | `CASHeartbeat.BoundedPathsStillStopAtTheLeaseDeadline` | both bounded entry points |
| T5b | `CASMountLease.RemountRenewalIsAdmittedOffTheMountFence` | `renewForRemount` |
| T5b | `CASEvent.AnAmbiguityPastTheLeaseBoundNeverStartsTheResolvingRead` | a resolving read refused by the lease bound |
| T5b | `CASObservability.ExternalLeaseDeadlineCountsOnceWithoutReconstructingAttempts` | `CASMountRenewalDeadlineExceeded` (O2) |
| T6 | `CASEvent.DeepReentrancyPreservesDeterministicPhysicalAttemptTruth` | the eight-slot stack; it calls `configureMountRenewObservability` |

### Adapt (guarantee stays reachable)

| Task | Test | What changes |
|---|---|---|
| T4 | `CASPoolRemount.RemountWaitsForRenewalParkedBeforeReplacement` | drop the two state assertions; keep "the reclaim callback has not run while the renewal request is in flight" |
| T4 | `CASMountRuntime.ParkEndsAnUnboundedRenewal` | drop the state assertions; keep "no request and no wait starts after the request"; it becomes the second half of `ARemountRequestEndsTheRenewalAndTheSameThreadReclaims` |
| T4 | `CASPoolRemount.TeardownJoinsBothWorkersBeforeRelease` | one worker exit; the farewell follows the join |
| T4 | `CASPoolRemount.NaturalTerminalTransitionMakesBothPersistentWorkersSelfExit` | one exit; the transition is published from the reclaim on the lease thread |
| T4 | `CASPoolRemount.ParkedRenewalCannotMissNaturalTerminalPublication` | the renamed hook sits in the loop's wait; two cases: the cadence wait and the reclaim backoff (spec test 17) |
| T4 | `CASPoolRemount.VanishedReasonPreparationFailureLeavesTerminalTransitionRetryable`, `CASMountRuntime.ForgetEndsAnUnboundedRenewal` | `exits.waitForAtLeast(1)` |
| T4 | `CASPoolRemount.WorkerConstructionRollbackFailsOpenClosed` | `throw_on` is 1 only |
| T4 | `CASPoolRemount.ExternalLossDuringRenewalUsesOneRecoveryGeneration`, `ImmediatePostRemountRenewalFailureIsNotDropped`, `ConcurrentRemountRequestIsProcessedAfterActiveGeneration` | they compile and should pass unchanged: their callbacks arm with `armMountFence` + `noteRemounted` and the loop acknowledges. Switch the two arming callbacks to `beginReclaim` + `armIfAdmissible` so they exercise the production order |
| T4 | `CASForget.ForgetReLatchesFenceAfterAReclaimReachesArmMountFence` | assert inside the GC-stop callback that the reclaim returned `true` and `mayMutate()` is false; the second trip is a backstop |
| T4 | `CASPoolRemount.WholeChainResultsAreNumberedAndStepLabelled` | untouched if O6 keeps `publish_live` for an armed reclaim |
| T5a | `CASPool.StartupArmRedoesLeaseWriteWhenTheClaimConsumesTtl` | same counts; the third write is the loop's renewal and the open returns armed |
| T5a | `CASMountOpenWaits.PublicationHorizonUsesTheEnvelope` | `background_watermark = true`; the window is two envelopes plus the margin, strict, with no period term |
| T5a | `CASPoolRemount.RemountRenewerRedoUsesTheEnvelope` | assert `Live` or not at the MountRemount event instead of counting writes |
| T5a | `CASPoolRemount.RemountArmAnchorsAtClaimAttemptNotResponseTime` | untouched unless the clock queue shifts by the reads of `armIfAdmissible` |
| T5b | `CASPool.DirectAndStartupTerminalFailuresRethrowTypedExceptions` | keep the direct `RefusedAfterSend` case; rename without "Startup" |
| T5b | `CASPool.RenewWatermarkOnceRefreshesFenceAndDepositsOneFailure` | the failure is a definitive answer, not a lease expiry |
| T5b | `CASHeartbeat.WallClockStepsAndBootSuspendCannotExtendAuthority` | the suspend case: the renewal commits with a start after the suspend; admission is `CASMountRuntime.AdmitRefusesAnExpiredLease` |
| T5b | `CASMountLease.FarewellRunsOnAnOpenFenceAfterTheMountFenceIsLost` | drop the refused-renewal half |
| T5b | `CASMountAudit.RenewalDefaultLogsAreBounded`, `CASEvent.TerminalRenewalDetailsPreservePhysicalTruthAndClassification` | the lease-deadline block becomes a definitive answer or goes |
| T5b | every `CASHeartbeat.*` using `renewalEnvironment` or `UnboundedRenewalFixture::renew(policy, ...)` (40 call lines) | no `policy`; constructor with two planes |
| T6 | `CASEvent.ReentrantRenewalSinkPreservesOuterObservationIdentity`, `PreCompletionConflictReentrancyPreservesOuterTerminalObservation` | the spec lists nested renewals from a sink as deleted; both should pass unchanged on a by-value result. Keep if green, else delete |
| T6 | the 13 `release()` call lines | pass the deadline |
| T7+8 | per commit, see T7+8 |

### Untouched

- 210 `renewWatermarkOnce` lines outside `gtest_cas_pool.cpp` that renew over a healthy store, or expect a definitive
  answer (`gtest_cas_pool.cpp:1637`, `gtest_cas_event_log.cpp:466`, `:566`; T5b re-reads each).
- `gtest_cas_mount_runtime.cpp` (10 tests), `gtest_cas_lifecycle_snapshot.cpp` (7), `gtest_cas_lifecycle_condition.cpp`,
  `gtest_cas_fence_generation.cpp`, `gtest_cas_mount_claim_conflicts.cpp` (constructor only).
- `CASPoolRemount.DirectRenewIsRefusedForBackgroundConfiguredRuntimeAfterStop`: the seam keeps that refusal.
- `CASPoolShutdown.PreSendCancellationAllowsFarewellButAmbiguityDoesNot`, `CASMountRuntime.MemoryLimitDoesNotEndTheLeaseThread`,
  `CASPool.DeterministicWorkerFailureFencesWithoutWaitingForCadence`, `CASPool.DisabledBackgroundDoesNotReserveRenewalCadence`,
  `CASPoolRemount.LeaseLossHasOneOperationalOwner`, `LiveForgetDoesNotCountOperationalLeaseLoss`,
  `ThrowingEventSinkAfterCommitLeavesRuntimeLive`, `SameRuntimeReentrantForeignConflictSink...`, `ThrowingForeignConflictSink...`.

Model invariant to named test:

| Invariant | Test | Task |
|---|---|---|
| `NoArmWhileRequestPending` | `NothingArmsWhileARemountRequestIsPending`; `ARenewalArmsNothingWhileARequestIsPending` | T4; T5a |
| `NoJoinWindowReclaim` | `AReclaimFinishedAfterTheForgetIntentArmsNothing`; `CASForget.ForgetReLatchesFence...` adapted | T4 |
| `RenewNotAfterIntent` | `TheForgetIntentAloneEndsARenewal`; `ForgetEndsAnUnboundedRenewal` | T5a |
| `LiveOnlyWhenAdmissible` | `OpenAndRemountReportReadyOnlyWhenWritable` | T5a |
| `I7RequestNotLost` | `AReclaimAcknowledgesOnlyTheGenerationItServed` | T4 |

## 5. Open points {#open}

| # | Point | Options | Recommendation | Who rules |
|---|---|---|---|---|
| O1 | A writable open with no lease thread (`background_watermark = false`, reachable only with a null `startup_context`; which tools pass one was not established) whose claim does not admit a ref append. The spec's readiness assumes the thread; today the open re-anchors (`PublicationHorizonUsesTheEnvelope` pins it) | (a) fail the open; (b) run `renewOnce` on the opening thread, bounded by one TTL through an extra liveness term | (a). Rule 1 stays literal and no code is added. Such a mount stops admitting writes about 14 s after its claim anyway, because nothing renews it. Cost if wrong: an open that used to succeed fails with a retryable error | owner |
| O2 | After T5b no renewal can end at a deadline: `CASMountRenewalDeadlineExceeded`, the classifications `external_lease_deadline` and `request_deadline`, and `MountRenewResult::deadline_source` are unreachable. The counter and the classification are documented (`monitoring.md:62`, `troubleshooting.md:52`, `debugging.md:136`). "What goes away" does not list them | remove the counter, the values and the doc rows; or keep them as values that never occur | remove. A counter that cannot move reads as "healthy" | owner: a `system.events` name disappears |
| O3 | `renewWatermarkOnce` has 221 test call sites. The spec says tests call the loop's renewal step | keep the name as the seam over `renewOnce`; or rename and sweep | keep | controller |
| O4 | With rule 7 and C3, a trip that comes with no request, no intent and no stop is re-armed by the next successful renewal under the same epoch. The model's `LRenewOk` arms only from `Transient`; the code has `Live` with a latched fence at an open. After T4 production has no such trip: `reportImpossibleInterference` pairs it with `scheduleRemount`, FORGET with the intent, the loop's own catch exits the thread; `Pool::tripMountLost` is called by tests only | accept and state the pairing on `tripMountLost`; or add a flag set only by the open and by an unarmed reclaim | accept. Not arming would leave such a mount refusing writes for good with no reclaim requested. Cost if wrong: a re-arm with un-quiesced ref runtimes, which the generation bump already fails closed | controller |
| O5 | The deadline `finishTeardown` passes to `release`. The spec says the fence deadline is the only copy. They differ when a reclaim installed and started a renewer and failed before `armIfAdmissible`, and in tests that arm by hand | fence deadline; or `mount_renewer->lastCommittedAttemptStartBootMs() + ttl`, computed by the runtime | the second: it is today's value in every case. The renewer keeps `last_committed_attempt_start_boot_ms` either way, since the cadence reads it | controller |
| O6 | The MountRemount event and `CASRemountSucceeded` for a reclaim that claimed and did not arm | `outcome = 'ok'`, `step = 'claimed_not_armed'`, counted as succeeded; or `failed` | the first; `publish_live` stays the step of an armed reclaim | controller |
| O7 | Waking the renewal's retry wait on a remount request or an intent. It needs a second sleep function for the lease plane (`sleepInterruptibly` also serves the mount plane, whose waits must not end on a request), wired in `Pool` (P:214, P:1978) and `RuntimeUnderTest`; the notifications exist | add it; or leave rule 8 as written | leave it. The gain is at most 1.2 s before a reclaim and before FORGET's join | controller |
| O8 | Spec test 20: `utils/ca-soak/scenarios/cards/s39_lease_fault_tolerance.py` exists in the `master` worktree only, not in this branch | a separate commit there | separate commit, outside the five tasks' gate | controller |
| O9 | "The closed initial state" (order of work, step 5) against "the runtime starts with the fence unarmed" (readiness) | construct the runtime with `lost = true`; or latch only at an open whose claim is too old | the second, as the readiness section says. `CASMountRuntime.AdmitAllowsAnUnarmedFence` and every fixture that never arms depend on the open default | controller |
| O10 | `armMountFence` after T4 | keep it as the test primitive that does not report `Live`; or let it report `Live` | keep. `rearmMountFenceAfterAnomalyForTest` and `bumpFenceGeneration` leave a pool armed and `TransientNotLive`, a state production cannot reach; changing that is not this plan's subject | controller |
| O11 | Spec cleanup 1 says no production code reads `epoch` / `writerEpoch`. True; 64 test lines do | sweep to `liveWriterEpoch()` | sweep in 8.1 | controller |

Not covered by the model (no time, no open), to be argued in the plan text:

- The open's readiness wait against the thread it started: the stop and the join; a request under way delays the failure by up to one attempt envelope.
- The cadence: a reclaim's observation wait and its backoff run on the lease thread; no renewer is `Active` during them.
- A stale success followed at once by the next renewal (`XExpire` abstracts it).
- `waitUntilArmed` with an injected clock (C9).

Limits of this contract: the lock held by the ref ledger at `reportImpossibleInterference` was not checked; the six timing
comments and two setting descriptions of cleanup 7 are not enumerated; no code was built or run; the tests marked
"untouched" were classified from their text, not by running them against a patch.

## 6. Size check {#size}

Estimate from line ranges of the functions named above, not from a patch.

| File | Lines now | Go | Come | Net |
|---|---:|---:|---:|---:|
| `CasMountRuntime.cpp` | 1243 | 590 | 365 | -225 |
| `CasServerRoot.cpp` | 1875 | 275 | 75 | -200 |
| `CasPool.cpp` | 2199 | 110 | 50 | -60 |
| three main files | 5317 | 975 | 490 | -485 |
| `CasMountRuntime.h` | 584 | 130 | 50 | -80 |
| `CasServerRoot.h`, `CasPool.h` | 1959 | 45 | 20 | -25 |
| GC scheduler, budget, retry, settings, `ProfileEvents.cpp`, `setThreadName.h` | | 25 | 10 | -15 |
| production total | | 1175 | 570 | about -600 |

Spec: about 1,000 go and 450 come in the three main files, about -480 across production. The three-file figures agree.
The production total here is larger because headers lose comments with the code they describe.

| Measure | Now | After |
|---|---:|---:|
| driver states | 9 | 0 |
| renewal entry points | 4 | 1 (`renewOnce`, plus the test seam that calls it) |
| lease threads | 2 | 1 |
| request planes the renewer holds | 3 | 2 |
| `_for_test` hooks in `MountConfig` | 10 | 8 |

Tests: about 750 lines of deleted tests, about 900 lines of new ones, and mechanical edits at about 330 call sites.
The tree gets smaller and the ownership protocol goes. Two places grow: the arming rule is new code in three callers
(open, reclaim, renewal), and `MountRenewResult` carries five more fields.

---

# Rulings on the open points of CONTRACT.md (binding for the plan writers)

| # | Ruling | Why | Cost if wrong |
|---|---|---|---|
| O1 | A writable open with no lease thread whose claim does not admit a ref append FAILS with a retryable error (option a). The writer of the readiness task first enumerates every production caller that opens writable with `background_watermark = false` (untruncated grep, listed in the task section); if one exists whose claim can be too old in normal operation, the section says so in its Deviations and still implements (a) | rule 1 stays literal, no code is added; a mount with no lease thread stops admitting writes about 14 s after its claim anyway | an open that used to succeed fails with a retryable error; reported to the owner |
| O2 | Remove `CASMountRenewalDeadlineExceeded`, the classifications `external_lease_deadline` and `request_deadline`, `MountRenewResult::deadline_source`, and their doc rows, in the task that removes the bounded paths | a counter that cannot move reads as "healthy" | a `system.events` name disappears; one line to restore; reported to the owner |
| O3 | Keep the name `renewWatermarkOnce` as the test seam over `renewOnce` | 221 call sites | none |
| O4 | Accept: a trip that comes with no request, no intent and no stop is re-armed by the next successful renewal. State the pairing rule in the comment on `tripMountLost` (every production trip comes with a remount request, an intent or a stop) and pin the behaviour in Task 2 with the test `CASMountRuntime.ATripAloneIsRearmedByTheNextRenewal` | not arming would leave a mount refusing writes for good with no reclaim requested | a re-arm over un-quiesced ref runtimes, which the generation bump already fails closed |
| O5 | `finishTeardown` passes `mount_renewer->lastCommittedAttemptStartBootMs() + ttl` to `release` | today's value in every case | none |
| O6 | A reclaim that claimed and did not arm: MountRemount event `outcome = 'ok'`, `step = 'claimed_not_armed'`, counted in `CASRemountSucceeded`; `publish_live` stays the step of an armed reclaim | the loop must not back off | an operator reads one more step name; documented in the same task |
| O7 | A remount request does not wake the renewal's retry wait (rule 8 as written) | at most 1.2 s gained for a second sleep function on the lease plane | none |
| O8 | Soak card S39 (spec test 20) is a separate commit on `cas-gc-rebuild` by the controller, outside the five tasks | the card is not in the code branch | none |
| O9 | The runtime is constructed with the fence unarmed (open); only an open whose claim does not admit a ref append latches it | the readiness section of the spec; existing fixtures depend on the open default | none |
| O10 | `armMountFence` stays the unconditional test primitive that does not report `Live` | not this plan's subject | none |
| O11 | Sweep the 64 `writerEpoch()` test lines to `liveWriterEpoch()` in cleanup 8.1; on a read-only pool the value changes from a random number to 0: every swept line that runs on a read-only pool is checked | spec cleanup 1 | a test that compared two random epochs for inequality now compares 0 with 0: each such line is found and rewritten |
| C1 to C11 | As recommended in section 2.5 | see there | see there |
| extra | The refusal text of `leaseExpiredRefusal` becomes "… writes resume when a renewal restores it" in the readiness task (the file is edited there anyway) | precise also after a stale success | none |
| extra | The deferred review finding (a worker commit consumed after a remount armed the fence overwrites the new deadline) is closed by the one-thread task, with the test the contract names | removed by construction | none |
