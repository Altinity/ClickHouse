# CAS Mount-Lease Abort Hardening + Explicit Disk Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A CAS background thread must never abort the server when its backing store vanishes, and tests/operators get explicit idempotent `SYSTEM CONTENT ADDRESSED MOUNT / UNMOUNT / FSCK` commands so a pool can be cleanly quiesced, verified for leftovers, deleted, and reused.

**Architecture:** Part 1 reclassifies "mount object vanished" from `LOGICAL_ERROR` (aborts debug/ASan at exception construction) to an environmental error on both the renewal and terminate paths, plus a `CasMountLeaseLost` counter. Parts 2–4 add an explicit lifecycle state machine (`Mounted`/`Unmounting`/`Dormant`) on `ContentAddressedMetadataStorage` with a synchronous draining `UNMOUNT`, an atomic-publish `MOUNT`, a dormant-only `FSCK` verb over a temporary observe-only pool, and `pending_*` drain columns on `GC RUN`; the no-leftovers tests move to `GC RUN → UNMOUNT → FSCK → rm -rf`. Part 5 renames the offline applet to `ca-fsck` keeping `fsck` as a deprecated alias.

**Tech Stack:** C++ (ClickHouse fork; CAS subsystem under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`, SYSTEM-verb wiring in `src/Parsers` + `src/Interpreters` + `src/Access`), GoogleTest (`src/Disks/tests/`), bash stateless tests.

**Spec:** `docs/superpowers/specs/2026-07-21-cas-mount-lease-abort-and-disk-lifecycle-design.md` (rev.2 — read it; it records WHY each mechanism is shaped this way, including the 11 review findings it answers).

## Global Constraints

- Branch `cas-gc-rebuild`. Never rebase or amend; new commits only. Never commit to `master`. NO `git push` without fresh explicit per-instance authorization.
- Allman braces in C++. Never use `sleep` in C++ to fix a race (the drain wait below is a bounded wait for an external condition, with an explicit comment saying so). Say "exception" not "crash" for logical errors; "ASan" not "ASAN".
- Build via `ninja -C build <target>` with output redirected to a log file under `build/`; NEVER pass `-j`/`nproc`; analyze logs via a subagent. Tests likewise: unique log file per run under `build/`, summarize via subagent.
- Temporary files go in `tmp/` under the repo root, never `/tmp`.
- No CA-specific fields/logic added to generic code beyond the existing `CONTENT_ADDRESSED_*` SYSTEM-verb family pattern (enum values, parser cases, AccessTypes, interpreter dispatch — the family already exists; new verbs are sibling entries only).
- The full CA gtest gate is `./build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:RefWriter*:RefLedger*'` — must be green at the end of every task that touches C++.
- New SYSTEM verbs change `tests/queries/0_stateless/01271_show_privileges.reference` — update it in the same task that adds the AccessTypes, or the style/stateless gate breaks.

---

## Task 1: Part 1a — renewal-path hardening + `CasMountLeaseLost`

**Files:**
- Modify: `src/Common/ProfileEvents.cpp` (new event, near the other `CasMount*`/`CasGc*` events ~line 860)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp` (`ErrorCodes` block ~line top; `MountLeaseKeeper::onRenewMismatch` — the `if (got) {...}` classification block ends and falls through to `SingleWriterSlot::onRenewMismatch(mismatched_key);` at ~line 856)
- Test: `src/Disks/tests/gtest_cas_mount.cpp` (mirror the existing mount-classification tests' harness)

**Interfaces:**
- Consumes: `emitMountEvent(event_sink, CasEventType::MountConflict, srid, <verb>, ...)` (existing, used by sibling branches); `backend->get(mismatched_key)` absent ⇒ `!got`.
- Produces: ProfileEvent `CasMountLeaseLost` (used again by Task 2); mount-event verb string `"vanished"`.

- [ ] **Step 1: Register the ProfileEvent**

In `src/Common/ProfileEvents.cpp`, next to the existing CAS mount/GC events (grep `CasConditionalWriteFenceLostPostWrite`, ~line 880), add:

```cpp
    M(CasMountLeaseLost, "Counts CAS mount-lease terminal losses: the mount slot object vanished (backing store deleted under a live mount), was superseded by a newer incarnation, or was taken by a foreign server. The keeper stopped renewing and latched its write fence to lost; non-zero values mean a mount was lost -- investigate via system.content_addressed_log MountConflict rows.", ValueType::Number) \
```

- [ ] **Step 2: Write the failing test**

In `src/Disks/tests/gtest_cas_mount.cpp`, find how existing tests build a `MountLeaseKeeper`/pool over a test backend (grep `claimMount`, `renewOnce`, or open a pool via the file's `openPool`-style helper) and mirror the harness. Add:

```cpp
TEST(CasMountLease, VanishedBackingStoreStopsRenewalWithoutLogicalError)
{
    /// rm -rf of the pool dir under a live mount: the mount slot object is deleted while the
    /// keeper holds a lease. The next renew must fail closed (stop renewing, fence lost) WITHOUT
    /// constructing a LOGICAL_ERROR -- that aborts debug/ASan builds. STID 3982-3b48 regression.
    auto backend = std::make_shared<...TestBackend...>();      // per the file's existing harness
    auto pool = openPool(backend);                              // claims the mount lease
    const String mount_key = ...;                               // layout.mountKey(srid) per harness

    using ProfileEvents::global_counters;
    const auto lost_before = global_counters[ProfileEvents::CasMountLeaseLost].load();

    backend->eraseKeyForTest(mount_key);   // simulate rm -rf: mount object gone (add helper if absent)

    /// Drive one renewal synchronously (the harness exposes renewOnce via the keeper or a test hook).
    try
    {
        driveOneRenew(pool);
        FAIL() << "renew against a vanished mount object must throw";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::FILE_DOESNT_EXIST) << e.message();
        EXPECT_NE(e.code(), DB::ErrorCodes::LOGICAL_ERROR);
    }
    EXPECT_EQ(global_counters[ProfileEvents::CasMountLeaseLost].load(), lost_before + 1);
}
```

Implementer notes: the sketch's `openPool`/`driveOneRenew`/`eraseKeyForTest` are placeholders for the file's REAL harness — read `gtest_cas_mount.cpp` first and reuse its exact fixture (it already constructs keepers and forces renew outcomes for the fenced/superseded/foreign classification tests; model this test on the nearest sibling). If the backend lacks a raw key-erase helper, add a minimal one to the test backend class in that file. Also add `extern const Event CasMountLeaseLost;` to the test file's `ProfileEvents` extern block and `extern const int FILE_DOESNT_EXIST;` to its `ErrorCodes` block if missing.

- [ ] **Step 3: Run the test to verify it FAILS with LOGICAL_ERROR**

```bash
ninja -C build unit_tests_dbms > build/build_p1a.log 2>&1
./build/src/unit_tests_dbms --gtest_filter='CasMountLease.VanishedBackingStoreStopsRenewalWithoutLogicalError' > build/test_p1a_red.log 2>&1
```

Expected: FAIL — the thrown exception carries `LOGICAL_ERROR` (in a non-abort build the catch sees the wrong code; the counter is also not bumped).

- [ ] **Step 4: Implement the vanished branch**

In `CasServerRoot.cpp`: add to the `ErrorCodes` block `extern const int FILE_DOESNT_EXIST;`; add to the `ProfileEvents` block (create one if the file has none — grep `ProfileEvents::increment` first) `extern const Event CasMountLeaseLost;`. In `MountLeaseKeeper::onRenewMismatch`, immediately before the final `SingleWriterSlot::onRenewMismatch(mismatched_key);` line, insert:

```cpp
    if (!got)
    {
        /// The mount slot object VANISHED (backing store deleted under a live mount -- e.g. an
        /// operator or test rm -rf'd the pool dir). This is an ENVIRONMENTAL condition, not a logic
        /// error: there is no foreign writer to fail closed against. Stop renewing (fail-closed: the
        /// write fence latches to lost, we never re-mint) WITHOUT aborting the server --
        /// LOGICAL_ERROR here aborts debug/ASan builds at exception construction.
        ProfileEvents::increment(ProfileEvents::CasMountLeaseLost);
        emitMountEvent(event_sink, CasEventType::MountConflict, srid, "vanished", nullptr,
            "mount slot object vanished (backing store deleted under a live mount) -- stopping renewal, fail-closed");
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "CAS mount-lease: key '{}' vanished (backing store deleted under a live mount) -- "
            "stopping renewal, fail-closed (never re-minting)", mismatched_key);
    }
```

NOTE: `if (!got)` must be inserted AFTER the existing `const auto got = backend->get(mismatched_key);` and its `if (got) { ...classification... }` block, replacing the unconditional fall-through — the base call remains only for the got-but-unclassifiable case (same uuid+epoch, unfenced). Also add `ProfileEvents::increment(ProfileEvents::CasMountLeaseLost);` as the first line of the existing `superseded` and `foreign_writer` branches (NOT `fenced_by_gc`).

- [ ] **Step 5: Run the test to verify it PASSES; run the CA gate**

```bash
ninja -C build unit_tests_dbms > build/build_p1a2.log 2>&1
./build/src/unit_tests_dbms --gtest_filter='CasMountLease.*' > build/test_p1a_green.log 2>&1
./build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:RefWriter*:RefLedger*' > build/test_p1a_gate.log 2>&1
```

Expected: new test PASS, all existing mount tests PASS, gate green. If an existing test pinned the old fall-through `LOGICAL_ERROR` for the absent case, update it to the new contract (that is the intended behavior change) and say so in the commit message.

- [ ] **Step 6: Commit**

```bash
git add src/Common/ProfileEvents.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp src/Disks/tests/gtest_cas_mount.cpp
git commit -m "cas: vanished mount slot stops renewal without LOGICAL_ERROR (STID 3982-3b48, part 1a)

A background renewal against a backing store that was deleted under a live
mount (rm -rf of the pool dir) now throws FILE_DOESNT_EXIST instead of falling
through to SingleWriterSlot's LOGICAL_ERROR, which aborts debug/ASan builds at
exception construction. Fail-close semantics unchanged: the loop stops,
onRenewFailed latches the write fence to lost, never re-mints. New ProfileEvent
CasMountLeaseLost counts terminal losses (vanished/superseded/foreign, not the
recoverable fenced_by_gc)."
```

---

## Task 2: Part 1b — terminate-path hardening

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp` (`MountLeaseKeeper::terminate`, ~lines 859–900)
- Test: `src/Disks/tests/gtest_cas_mount.cpp`

**Interfaces:**
- Consumes: `CasMountLeaseLost` (Task 1); the existing `gc_fenced` no-op-release precedent inside `terminate`.
- Produces: absent-lease terminate = no-op release (returns, no throw).

- [ ] **Step 1: Write the failing test**

Model on Task 1's test: claim a mount, erase the mount key, then drive the TERMINAL release (`terminate` — reached via the keeper's `stop()`/`doTerminate()` path or the pool's destructor; reuse however `gtest_cas_mount.cpp` drives a clean release today). Assert: no exception escapes (or specifically NOT `LOGICAL_ERROR` if the drive helper surfaces exceptions), and `CasMountLeaseLost` is NOT double-counted if the renewal path already counted this loss (drive terminate WITHOUT a prior failed renew → expect +1; the combined renew-then-terminate flow may total +1 or +2 — pin whichever the implementation produces and document it in the test comment; the spec allows either as long as it is deterministic).

```cpp
TEST(CasMountLease, TerminateAfterVanishedBackingStoreIsNoOpRelease)
{
    auto backend = ...; auto pool_or_keeper = ...;    // harness as in Task 1
    backend->eraseKeyForTest(mount_key);
    EXPECT_NO_THROW(driveTerminate(...))
        << "clean release against a vanished store must be a no-op, not a LOGICAL_ERROR abort";
}
```

- [ ] **Step 2: Run to verify FAIL** (currently throws `LOGICAL_ERROR "release ... hit a foreign incarnation — the world is broken"` — under a non-abort build the EXPECT_NO_THROW fails).

- [ ] **Step 3: Implement**

In `MountLeaseKeeper::terminate`, the failure block currently re-reads and no-ops only on `gc_fenced`. Extend it — after the `if (const auto got = backend->get(key))` block, replace the unconditional final throw with:

```cpp
        if (const auto got = backend->get(key))
        {
            const MountLease current = decodeMountLease(got->bytes);
            if (current.gc_fenced)
            {
                LOG_INFO(getLogger("CasMountLeaseKeeper"),
                    "CAS mount-lease: '{}' was fenced out by GC (expired lease); release is a no-op", key);
                return;
            }
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS mount-lease: release of key '{}' hit a foreign incarnation — the world is broken", key);
        }
        /// The lease object is ABSENT: the backing store was deleted under us (rm -rf of the pool
        /// dir -- the same environmental condition the renewal path classifies as "vanished").
        /// The desired end state of a release is "no live lease object", which is already true, so
        /// this is a clean no-op release, never a LOGICAL_ERROR (which aborts debug/ASan builds).
        ProfileEvents::increment(ProfileEvents::CasMountLeaseLost);
        emitMountEvent(event_sink, CasEventType::MountRelease, srid, "vanished", nullptr,
            "mount slot object already gone at release (backing store deleted) -- no-op release");
        LOG_INFO(getLogger("CasMountLeaseKeeper"),
            "CAS mount-lease: '{}' is already gone at release (backing store deleted); release is a no-op", key);
        return;
```

(The genuine present-body foreign case keeps its `LOGICAL_ERROR` — moved inside the `got` block as shown.)

- [ ] **Step 4: Run to verify PASS + CA gate green.** Same commands as Task 1 Step 5, log files `build/{build,test}_p1b*.log`.

- [ ] **Step 5: Commit** (message: `cas: absent mount lease at clean release is a no-op, not a LOGICAL_ERROR (part 1b)` + body explaining the rm-rf → renew-dies → teardown-release sequence that used to abort).

---

## Task 3: `startup()` atomic-publish refactor

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.{h,cpp}` (`startup()`)
- Test: `src/Disks/tests/gtest_ca_wiring.cpp` (or the gtest file that already constructs `ContentAddressedMetadataStorage` directly — `gtest_ca_transaction.cpp`'s `openTxStorage` shows the harness: `makeSettingsForTest` + `makeLocalObjectStorageForTest` + `startup()`)

**Interfaces:**
- Produces: `startup()` that publishes `cas_store`/`part_access`/`pool_uuid`/`conditional_copy_supported`/`gc_scheduler` in ONE `pointer_mutex` section as its LAST action; on any throw nothing is published. Task 5 (MOUNT) depends on this exact property.

- [ ] **Step 1: Write the failing test** — inject a failure late in startup and prove nothing is published. The clean seam: `Pool::open` succeeds but the capability probe / facade construction throws. Simplest injectable seam without new hooks: construct the storage with a `cas_part_folder_cache_*` setting that makes `CachedPartFolderAccess`'s ctor throw, if such a validating setting exists — otherwise add a tiny test-only hook `std::function<void()> startup_fault_injection_for_test` called right before the publish step. With the hook:

```cpp
TEST(CaWiring, StartupFailureLatePublishesNothing)
{
    auto storage = ...make storage as in openTxStorage but WITHOUT calling startup()...;
    storage->startup_fault_injection_for_test = [] { throw std::runtime_error("injected late-startup failure"); };
    EXPECT_ANY_THROW(storage->startup());
    /// Nothing was published: store() must still say "before startup", and a RETRY must succeed.
    EXPECT_ANY_THROW(storage->store());
    storage->startup_fault_injection_for_test = {};
    EXPECT_NO_THROW(storage->startup());
    EXPECT_NO_THROW(storage->store());
}
```

- [ ] **Step 2: Run to verify FAIL** (today `cas_store` is published early, so after the injected throw `store()` succeeds and the retry short-circuits on the half-built state — the second `EXPECT_ANY_THROW(storage->store())` fails).

- [ ] **Step 3: Implement** — restructure `startup()`'s tail: build `auto pool = Cas::Pool::open(...)`, `auto uuid = ...`, `auto facade = std::make_shared<CachedPartFolderAccess>(pool, ...)`, run the probe/sweep into a local `bool copy_supported`, build `auto scheduler = ...` and `scheduler->start()` — ALL into locals; then, as the last statement:

```cpp
    if (startup_fault_injection_for_test)
        startup_fault_injection_for_test();
    {
        std::lock_guard lock(pointer_mutex);
        cas_store = std::move(pool);
        part_access = std::move(facade);
        gc_scheduler = std::move(scheduler);
    }
    pool_uuid = std::move(uuid);
    conditional_copy_supported = copy_supported;
```

Watch for: the early-return `if (cas_store) return;` head needs the same `pointer_mutex` read it has today; `scheduler->start()` before publish means the scheduler runs against the local pool ref — verify `CasGcScheduler` holds its own `PoolPtr` (it is constructed with `cas_store` today, i.e. yes) so this is safe; if a throw happens AFTER `scheduler->start()` (only the injection hook is after), `scheduler` is a local whose dtor must stop the thread — check `CasGcScheduler`'s dtor stops/joins, and if not, wrap in `SCOPE_EXIT` that calls `scheduler->stop()` on the unwind path.

- [ ] **Step 4: PASS + gate green** (`build/{build,test}_p3*.log`). **Step 5: Commit** (`cas: startup publishes the pool atomically as its last step`).

---

## Task 4: lifecycle state machine + operation gate + coherent snapshot accessor

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.{h,cpp}`
- Test: same gtest file as Task 3

**Interfaces:**
- Produces (consumed by Tasks 5–7):
  - `enum class MountState : uint8_t { Mounted, Unmounting, Dormant };` + member `MountState mount_state TSA_GUARDED_BY(pointer_mutex) = MountState::Dormant;` (startup's publish step sets `Mounted`).
  - `struct PoolAccessSnapshot { Cas::PoolPtr pool; std::shared_ptr<Cas::CachedPartFolderAccess> part_access; };`
  - `PoolAccessSnapshot poolAccess() const;` — under ONE `pointer_mutex` acquisition: if `mount_state != Mounted` or `!cas_store`, throw `ErrorCodes::INVALID_STATE` (exists, code 668): `"content-addressed disk '{}' is not mounted (state: {}) — run SYSTEM CONTENT ADDRESSED MOUNT"`; else return `{cas_store, part_access}`.
  - `void unmountSynchronously(uint64_t drain_timeout_ms = 30000);` and `void mountExplicitly();` (bodies in Task 5).
  - New member `mutable std::mutex lifecycle_mutex;` — outermost; taken ONLY by `mountExplicitly`/`unmountSynchronously`/the FSCK handler; documented order `lifecycle_mutex → gc_scheduler_mutex → pointer_mutex`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CaWiring, OperationsRefuseWhenNotMounted)
{
    auto storage = ...; storage->startup();
    storage->shutdown();          /// today's terminal path resets cas_store
    try { storage->store(); FAIL() << "must refuse"; }
    catch (const DB::Exception & e) { EXPECT_EQ(e.code(), DB::ErrorCodes::INVALID_STATE) << e.message(); }
}
```

(RED: today the post-shutdown `store()` throws `LOGICAL_ERROR "accessed before startup"` — wrong code, and the message misleads.)

- [ ] **Step 2: verify FAIL.**

- [ ] **Step 3: Implement**

- Add the enum/members/struct/accessor as specified. `store()` and `partAccess()` become thin wrappers over `poolAccess()` (single lock, state-checked) so every existing caller inherits the gate; keep their signatures.
- Route the two split-acquisition sites through one snapshot: in `tryGetInManifestBytes` (~line 1260) replace the manual `pointer_mutex` snapshot with `poolAccess()` in a try/catch that preserves its current contract of returning `std::nullopt` when unavailable (this site is deliberately non-throwing — document that with a comment); in `getBlobViewPlan` (~line 1312) take `const auto snap = poolAccess();` ONCE at the top and use `snap.part_access->getView(...)` + `snap.pool->locate(...)` (it currently calls `partAccess()` then `store()` separately). Grep the file for any other `partAccess()` + `store()` pair inside one function and consolidate the same way.
- `startup()` publish step (Task 3) sets `mount_state = MountState::Mounted` inside the same `pointer_mutex` section. `shutdown()` sets `mount_state = MountState::Dormant` where it resets the pointers (server-shutdown terminal semantics otherwise unchanged, `shutdown_called` untouched).
- The initial state before first startup is `Dormant` — which makes the pre-startup `store()` error message uniform with the unmounted one (this intentionally replaces the old "accessed before startup" LOGICAL_ERROR: an access before/without mount is an operational condition, not a programming invariant, and LOGICAL_ERROR would abort debug builds on a mis-sequenced access).

- [ ] **Step 4: PASS + gate green** (`build/{build,test}_p4*.log`). Existing tests that pinned "accessed before startup" `LOGICAL_ERROR` must be updated to `INVALID_STATE` (intended change; note in commit). **Step 5: Commit** (`cas: mount-state gate + single coherent pool/facade snapshot accessor`).

---

## Task 5: `unmountSynchronously()` + `mountExplicitly()` (drain, resume, idempotency)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.{h,cpp}`
- Test: same gtest file

**Interfaces:**
- Consumes: Task 3 atomic startup, Task 4 state machine.
- Produces (consumed by Task 6's SYSTEM handlers):
  - `void unmountSynchronously(uint64_t drain_timeout_ms = 30000);` — idempotent/resumable quiescence barrier.
  - `void mountExplicitly();` — idempotent explicit mount, `Dormant → Mounted` via `startup()`.

- [ ] **Step 1: Write the failing tests (four behaviors)**

```cpp
TEST(CaLifecycle, UnmountDrainsAndIsIdempotent)
{
    auto storage = ...; storage->startup();
    storage->unmountSynchronously();
    EXPECT_ANY_THROW(storage->store());                    /// gated
    EXPECT_NO_THROW(storage->unmountSynchronously());      /// Dormant -> no-op
    EXPECT_NO_THROW(storage->mountExplicitly());           /// Dormant -> Mounted
    EXPECT_NO_THROW(storage->store());
    EXPECT_NO_THROW(storage->mountExplicitly());           /// Mounted -> no-op
}

TEST(CaLifecycle, UnmountTimesOutWhileSnapshotHeldThenResumes)
{
    auto storage = ...; storage->startup();
    auto held = storage->store();                          /// outstanding PoolPtr
    try { storage->unmountSynchronously(/*drain_timeout_ms=*/100); FAIL() << "must time out"; }
    catch (const DB::Exception & e) { EXPECT_EQ(e.code(), DB::ErrorCodes::TIMEOUT_EXCEEDED) << e.message(); }
    EXPECT_ANY_THROW(storage->store());                    /// state stays Unmounting: new ops refused
    EXPECT_ANY_THROW(storage->mountExplicitly());          /// MOUNT from Unmounting refused
    held.reset();
    EXPECT_NO_THROW(storage->unmountSynchronously());      /// resume completes
    EXPECT_NO_THROW(storage->mountExplicitly());
}

TEST(CaLifecycle, RemountAfterBackingWipeMintsFreshPool)
{
    /// UNMOUNT -> (rm -rf equivalent: clear the local test object storage's backing map/dir) ->
    /// MOUNT opens a FRESH pool (new pool_uuid, empty) -- the spec's clean-reuse contract.
    auto storage = ...; storage->startup();
    const String uuid_before = storage->poolUuid();        /// use the real accessor name (grep pool_uuid)
    storage->unmountSynchronously();
    ...wipe the test backend's storage...                  /// per the harness (local dir: remove_all; map backend: clear)
    EXPECT_NO_THROW(storage->mountExplicitly());
    EXPECT_NE(storage->poolUuid(), uuid_before) << "a wiped backing store must remount as a brand-new pool";
}
```

- [ ] **Step 2: verify FAIL** (methods don't exist — compile RED; stub them `{ }` if you want a runtime RED instead; either is acceptable evidence).

- [ ] **Step 3: Implement**

```cpp
void ContentAddressedMetadataStorage::unmountSynchronously(uint64_t drain_timeout_ms)
{
    std::lock_guard lifecycle(lifecycle_mutex);
    {
        std::lock_guard lock(pointer_mutex);
        if (mount_state == MountState::Dormant)
            return;                                        /// idempotent no-op
        mount_state = MountState::Unmounting;              /// gate: new operations now refuse
    }
    /// Stop the GC scheduler first (waits for an in-flight synchronous round, same as shutdown()).
    {
        std::lock_guard round_lock(gc_scheduler_mutex);
        std::shared_ptr<Cas::CasGcScheduler> old_scheduler;
        {
            std::lock_guard lock(pointer_mutex);
            old_scheduler = std::move(gc_scheduler);
            gc_scheduler.reset();
            part_access.reset();
        }
        if (old_scheduler)
            old_scheduler->stop();
    }
    /// Drain: wait for outstanding store() snapshots to die. In-flight part writes pin the pool via
    /// shared_from_this, so this naturally waits them out. Bounded slice-poll of use_count() -- a
    /// deliberate wait for an EXTERNAL condition (snapshot holders finishing), not a race-fix sleep;
    /// under the Unmounting gate the count is monotonically non-increasing.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(drain_timeout_ms);
    for (;;)
    {
        long holders = 0;
        {
            std::lock_guard lock(pointer_mutex);
            if (!cas_store)
                break;                                     /// resumed after a prior successful reset
            holders = cas_store.use_count() - 1;           /// -1: our own member reference
        }
        if (holders <= 0)
            break;
        if (std::chrono::steady_clock::now() >= deadline)
            throw Exception(ErrorCodes::TIMEOUT_EXCEEDED,
                "content-addressed disk unmount: {} pool reference(s) still live after {} ms -- "
                "disk stays in Unmounting (new operations refused); retry UNMOUNT to resume",
                holders, drain_timeout_ms);
        sleepForMilliseconds(50);
    }
    /// Sole owner: this reset runs ~Pool synchronously (lease clean-release -- hardened in part 1b --
    /// write-lane drain, background thread joins, farewell).
    {
        Cas::PoolPtr last;
        {
            std::lock_guard lock(pointer_mutex);
            last = std::move(cas_store);
            cas_store.reset();
        }
        last.reset();                                      /// ~Pool outside pointer_mutex
    }
    {
        std::lock_guard lock(pointer_mutex);
        mount_state = MountState::Dormant;
    }
}

void ContentAddressedMetadataStorage::mountExplicitly()
{
    std::lock_guard lifecycle(lifecycle_mutex);
    {
        std::lock_guard lock(pointer_mutex);
        if (mount_state == MountState::Mounted)
            return;                                        /// idempotent no-op
        if (mount_state == MountState::Unmounting)
            throw Exception(ErrorCodes::INVALID_STATE,
                "content-addressed disk mount: an unmount is in progress/incomplete -- "
                "retry SYSTEM CONTENT ADDRESSED UNMOUNT to finish it first");
    }
    startup();                                             /// atomic publish (Task 3) sets Mounted
}
```

Implementer notes: `use_count()` on a `shared_ptr` read under `pointer_mutex` while all new handouts are gated is a sound monotone drain predicate — add exactly that as a comment; `sleepForMilliseconds` needs `#include <base/sleep.h>`; `startup()` must also be callable when `shutdown_called == true`? NO — `unmountSynchronously` deliberately does NOT touch `shutdown_called` (that latch remains the server-terminal path); verify `startup()` does not consult `shutdown_called` (it does not today — only `shutdown()` sets and `runGarbageCollection*` read it; grep to confirm and leave a comment in `mountExplicitly`). Check `TSA` annotations compile (`TSA_NO_THREAD_SAFETY_ANALYSIS` on the two new methods if needed, mirroring `startup`).

- [ ] **Step 4: PASS + gate green** (`build/{build,test}_p5*.log`). **Step 5: Commit** (`cas: synchronous draining unmount + idempotent explicit mount on the lifecycle state machine`).

---

## Task 6: SYSTEM verbs `CONTENT ADDRESSED UNMOUNT / MOUNT` (+ live-table guard, privileges)

**Files:**
- Modify: `src/Parsers/ASTSystemQuery.h` (Type enum: add `CONTENT_ADDRESSED_UNMOUNT, CONTENT_ADDRESSED_MOUNT` after `CONTENT_ADDRESSED_DROP_POOL_MEMBER`), `src/Parsers/ASTSystemQuery.cpp` (formatting: both take a REQUIRED bare-identifier disk — add both cases next to `CONTENT_ADDRESSED_GC_REBUILD`'s, printing ` <disk>`), `src/Parsers/ParserSystemQuery.cpp` (both cases: `parseQueryWithOnClusterAndTarget(res, pos, expected, SystemQueryTargetType::Disk)` REQUIRED — mirror `CONTENT_ADDRESSED_GC_REBUILD` minus FORCE), `src/Access/Common/AccessType.h` (after line ~353: `M(SYSTEM_CONTENT_ADDRESSED_UNMOUNT, "SYSTEM CONTENT ADDRESSED UNMOUNT", GLOBAL, SYSTEM)` and `..._MOUNT` sibling), `src/Interpreters/InterpreterSystemQuery.cpp` (+`.h`: dispatch cases + `getRequiredAccessForDDLOnCluster` entries + two handlers)
- Modify: `tests/queries/0_stateless/01271_show_privileges.reference` (two new rows — run the test to regenerate/verify placement)
- Test: new stateless test via `./tests/queries/0_stateless/add-test 04xxx_content_addressed_mount_unmount.sh`

**Interfaces:**
- Consumes: Task 5's `unmountSynchronously()`/`mountExplicitly()`; `ContentAddressedMetadataStorage::tryFromDisk(disk)` (existing, used by the GC RUN handler).
- Produces: `SYSTEM CONTENT ADDRESSED UNMOUNT <disk>` / `SYSTEM CONTENT ADDRESSED MOUNT <disk>`.

- [ ] **Step 1: enum + parser + formatter + AccessType** (mechanical; verify `typeToString` derives the keyword text from the enum name — grep how `typeToString` is implemented; the existing family confirms underscores→spaces).

- [ ] **Step 2: Handlers in `InterpreterSystemQuery.cpp`** (next to `runContentAddressedGcRun`):

```cpp
void InterpreterSystemQuery::contentAddressedUnmount(const String & disk_name)
{
    auto disk = getContext()->getDisk(disk_name);                       /// UNKNOWN_DISK on bad name
    auto * ca = ContentAddressedMetadataStorage::tryFromDisk(disk);
    if (!ca)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Disk '{}' is not a content-addressed disk", disk_name);

    /// Live-table guard (spec Part 2): refuse while any loaded table's storage policy references
    /// ANY disk sharing this metadata storage (a CAS cache wrapper shares one storage between the
    /// base and cache disks). Traversal mirrors restartDisk's.
    Strings live_tables;
    for (auto & elem : DatabaseCatalog::instance().getDatabases(GetDatabasesOptions{.with_remote_databases = false}))
        for (auto it = elem.second->getTablesIterator(getContext(), {}, /*skip_not_loaded=*/true); it->isValid(); it->next())
            if (auto * mt = dynamic_cast<MergeTreeData *>(it->table().get()))
                for (const auto & table_disk : mt->getStoragePolicy()->getDisks())
                    if (ContentAddressedMetadataStorage::tryFromDisk(table_disk) == ca)
                        live_tables.push_back(it->table()->getStorageID().getNameForLogs());
    if (!live_tables.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "Cannot unmount content-addressed disk '{}': {} live table(s) reference it ({}); "
            "drop or detach them first", disk_name, live_tables.size(), fmt::join(live_tables, ", "));

    ca->unmountSynchronously();
}

void InterpreterSystemQuery::contentAddressedMount(const String & disk_name)
{
    auto disk = getContext()->getDisk(disk_name);
    auto * ca = ContentAddressedMetadataStorage::tryFromDisk(disk);
    if (!ca)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Disk '{}' is not a content-addressed disk", disk_name);
    ca->mountExplicitly();
}
```

Dispatch: `case Type::CONTENT_ADDRESSED_UNMOUNT: { getContext()->checkAccess(AccessType::SYSTEM_CONTENT_ADDRESSED_UNMOUNT); contentAddressedUnmount(query.disk); break; }` (and MOUNT sibling) + the two `required_access` cases in `getRequiredAccessForDDLOnCluster` mirroring the family at ~line 2990.

- [ ] **Step 3: Update `01271_show_privileges.reference`** — run that stateless test, take the two new expected rows from its diff.

- [ ] **Step 4: Stateless test** (`add-test ...mount_unmount.sh`): inline CAS disk over local storage; drive the state table via SQL: `UNMOUNT` refused with a live table (create table → expect error) → drop table → `UNMOUNT` ok → second `UNMOUNT` ok (idempotent) → `SELECT` through the disk fails (any new table creation on the dormant disk errors with the MOUNT hint) → `MOUNT` ok → `MOUNT` ok again (idempotent) → table creation works again. Expected-output file pins the error fragments (`is not mounted`, `live table(s) reference it`). ALSO add a grant/refusal privilege test analogous to the existing GC-verb access test (grep `SYSTEM_CONTENT_ADDRESSED_GC_RUN` under `tests/queries/0_stateless/` for the sibling to mirror): a limited user is refused `SYSTEM CONTENT ADDRESSED UNMOUNT/MOUNT` without the grant and succeeds with it — covers `SYSTEM_CONTENT_ADDRESSED_FSCK` too once Task 7 lands (add its case there if this test predates it).

- [ ] **Step 5: Build server + run the new stateless test + CA gate**

```bash
ninja -C build clickhouse > build/build_p6.log 2>&1
python3 -m ci.praktika run "stateless" --test 04xxx_content_addressed_mount_unmount > build/test_p6_stateless.log 2>&1   # or the repo's direct clickhouse-test invocation per reference_praktika_local_runs
./build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:RefWriter*:RefLedger*' > build/test_p6_gate.log 2>&1
```

- [ ] **Step 6: Commit** (`cas: SYSTEM CONTENT ADDRESSED UNMOUNT/MOUNT verbs with live-table guard`).

---

## Task 7: `SYSTEM CONTENT ADDRESSED FSCK <disk>` (dormant-only) + GC RUN `pending_*` columns

**Files:**
- Modify: verb-wiring set as Task 6 (enum `CONTENT_ADDRESSED_FSCK`, parser case = GC_REBUILD-style required disk, AccessType `SYSTEM_CONTENT_ADDRESSED_FSCK`, privileges reference)
- Modify: `src/Interpreters/InterpreterSystemQuery.cpp` (FSCK handler; extend `contentAddressedGcRoundColumns()` + `appendContentAddressedGcRoundRow`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h` (+ the .cpp filling `RoundReport`): new fields `size_t pending_candidates = 0; size_t pending_condemned = 0; size_t pending_retired = 0;` — filled at round end from the gc state this round's single `gc/state` CAS published (grep `RoundReport` construction in `CasGc.cpp` / `runOneRound` to locate where the post-CAS state is in hand; the fields are the sizes of the candidate/condemned/retired pipeline sets remaining in that state). On `!acquired_lease` / `deferred` rounds leave them 0 and rely on the flags (already in the result set) to mark the row non-authoritative.
- Modify: `ContentAddressedMetadataStorage.{h,cpp}`: new method `Cas::FsckReport runFsckOnDormant(bool detail) const;` — under `lifecycle_mutex`: require `mount_state == Dormant` (else `INVALID_STATE` "disk is mounted — run SYSTEM CONTENT ADDRESSED UNMOUNT first (FSCK requires a quiesced pool)"); construct a TEMPORARY observe-only backend+pool exactly as `startup()` does but with the read-only/observe path (the `read_only` branch startup already has: no probe, no watermark, no GC — grep `read_only` in `startup()` and factor the backend+`Pool::open` construction into a private helper `openPoolView(bool observe_only)` both callers share); run `Cas::runFsck(*view, detail)`; destroy the view before returning.
- Test: extend Task 6's stateless test or a sibling (`04xxx_content_addressed_fsck.sh`): FSCK refused while mounted (error fragment) → UNMOUNT → FSCK returns a row set with 0 unreachable/0 dangling on a healthy drained pool; GC RUN result now carries `pending_candidates|pending_condemned|pending_retired` columns (assert header/zero values after drain).

**Interfaces:**
- Consumes: `Cas::runFsck(Pool&, bool detail, ...)` (`Tools/CasFsck.h`), `FsckReport`/`FsckClass` fields as used by `programs/disks/CommandFsck.cpp` (mirror its class→string mapping and it stays consistent with the offline applet).
- Produces: FSCK result set columns: `disk String, class String, key String, size UInt64` for detail rows PLUS a summary row form — simplest consistent shape: always return the summary as named UInt64 columns (`reachable, dangling, unreachable, pending_gc, awaiting_gc, unaccounted, physical_bytes, referenced_logical_bytes, distinct_blobs, total_blob_refs`) in ONE row per disk, and gate the per-object listing behind a future `DETAIL` keyword (do NOT implement DETAIL now — YAGNI; the offline applet covers per-object listing).

- [ ] **Step 1: RoundReport fields + fill + columns (RED via stateless assertion on missing columns is awkward — do this test-after with the gtest gate as the guard; the stateless test in Step 4 is the behavioral pin).**
- [ ] **Step 2: FSCK verb wiring + `runFsckOnDormant` + handler returning the one-row summary.**
- [ ] **Step 3: privileges reference update.**
- [ ] **Step 4: stateless test as described; build + run + CA gate** (`build/{build,test}_p7*.log`).
- [ ] **Step 5: Commit** (`cas: dormant-only SYSTEM CONTENT ADDRESSED FSCK + GC RUN pending_* drain columns`).

---

## Task 8: no-leftovers teardown rewrite (04290/04295 family)

**Files:**
- Modify: `tests/queries/0_stateless/04295_content_addressed_mutation_no_leftovers.sh`, `tests/queries/0_stateless/04290_content_addressed_no_leftovers.sh` (+ `.reference` files), and any other family member (grep `rm -rf.*POOL_DIR` under `tests/queries/0_stateless/*content_addressed*`).

**Interfaces:** consumes Tasks 6–7's verbs.

- [ ] **Step 1: Rewrite the teardown** in each: after `DROP TABLE ... SYNC`, replace the poll-dir-empty loop with:

```bash
DISK_NAME='04295_content_addressed_mut'   # the disk() name= value in this test
# Drain GC deterministically: loop rounds until the pending pipeline is empty (bounded).
for _ in $(seq 1 60); do
    PENDING=$($CLICKHOUSE_CLIENT --query "SYSTEM CONTENT ADDRESSED GC RUN '${DISK_NAME}'" --format TSVWithNames \
        | awk 'NR==2 {print $(NF-2) + $(NF-1) + $NF}')   # pending_candidates+condemned+retired = last 3 columns
    [ "${PENDING}" = "0" ] && break
    sleep 0.5
done
$CLICKHOUSE_CLIENT --query "SYSTEM CONTENT ADDRESSED UNMOUNT '${DISK_NAME}'"
# Dormant fsck: zero leftovers proves the no-leftovers oracle stronger than the old dir-poll.
$CLICKHOUSE_CLIENT --query "SYSTEM CONTENT ADDRESSED FSCK '${DISK_NAME}'" --format TSVWithNames \
    | awk 'NR==2 {print "fsck_unreachable", $3; print "fsck_dangling", $2}'   # column positions per Task 7's shape
rm -rf "${POOL_DIR:?}"   # safe now: unmount drained and joined every CAS thread for this disk
```

Adjust column extraction to Task 7's final column order (use `--format JSONEachRow` + `jq`-free python if awk positions get brittle — prefer named extraction via `SELECT ... FORMAT` is not available for SYSTEM, so TSVWithNames + awk by header name lookup). Keep each test's existing pre-teardown assertions untouched. Update `.reference` accordingly (`fsck_unreachable 0`, `fsck_dangling 0`).

Note: the bare-identifier disk target may not accept a leading-digit name unquoted — the parser case uses `SystemQueryTargetType::Disk` which accepts an identifier or quoted string (verify against `CONTENT_ADDRESSED_GC_REBUILD` usage in existing tests; quote defensively as shown).

- [ ] **Step 2: Run both tests against the built server; iterate until green** (`build/test_p8_*.log`; per-test unique names).
- [ ] **Step 3: Commit** (`cas: no-leftovers tests tear down via GC RUN -> UNMOUNT -> FSCK -> rm -rf`).

---

## Task 9: offline applet `ca-fsck` (+ deprecated `fsck` alias) and final sweep

**Files:**
- Modify: `programs/disks/CommandFsck.cpp` (`command_name = "ca-fsck"`; description mentions the alias), `programs/disks/DisksApp.cpp:~344` (register BOTH `"ca-fsck"` and `"fsck"` to `makeCommandFsck()`; the `fsck` registration wraps with a one-line stderr deprecation note — simplest: keep one command object; print the note inside `executeImpl` when invoked argv-name is `fsck` if the framework exposes it, otherwise register a tiny alias command that prints the note and delegates)
- Modify callers to `ca-fsck`: `tests/integration/test_content_addressed_drop_pool_member/test.py:~214`, `tests/integration/test_content_addressed_ref_snaplog/test.py:~108`, `utils/ca-soak/soak/fsck.py:~119`, `docs/superpowers/cas/08-testing-and-soak.md` §1.2 (and grep the whole tree for `clickhouse-disks` + `fsck` for stragglers: `grep -rn "disks.*fsck\|fsck.*disks" tests/ utils/ docs/ --include=*.py --include=*.sh --include=*.md`)

- [ ] **Step 1: rename + alias + callers.**
- [ ] **Step 2: Smoke: `./build/programs/clickhouse disks --help` output lists `ca-fsck`; run one integration caller locally if cheap, else rely on the soak unit tests: `cd utils/ca-soak && python3 -m pytest tests/ -q > ../../build/test_p9_soak.log 2>&1`.**
- [ ] **Step 3: Final full verification: CA gtest gate + the two no-leftovers stateless tests + the mount/unmount + fsck stateless tests, one log each under `build/`.**
- [ ] **Step 4: Commit** (`cas: rename offline disks applet fsck -> ca-fsck, keep deprecated alias`).

---

## Notes for the executor

- Task order matters: 1 → 2 (Part 1 standalone, ship-worthy alone) → 3 → 4 → 5 (state machine core) → 6 → 7 (verbs) → 8 (tests that need the verbs) → 9 (independent; may run any time after 7 for its fsck-column reference).
- Tasks 1–2 alone already fix the CI abort (STID 3982-3b48) — if priorities shift mid-plan, they are a safe stopping point.
- Every gtest sketch above marked "harness as in the file" REQUIRES reading the target test file first and reusing its real fixtures — the sketches fix the assertions and semantics, not the fixture spelling.
- After Task 6+, `SYSTEM` verb changes require rebuilding the full `clickhouse` binary for stateless runs (`ninja -C build clickhouse`), not just `unit_tests_dbms`.
- The debug/ASan e2e validation of the original crash (04295 under a debug build) happens in CI on the next PR#2073 push — flag it in the final report; local debug-build verification is optional (`build_debug` exists) but slow.

---

### Task 8a: Dormant CA disk answers existence/enumeration probes as absent (Part 6)

Ordering: this is a PRODUCT-CODE fix that must land BEFORE Task 8's parallel no-leftovers rewrite
can pass (Task 8 exposed the bug). Insert between Task 7 and Task 8's commit.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.{h,cpp}`
  — add a private non-throwing `bool isMounted() const` (reads `mount_state == Mounted && cas_store`
  under `pointer_mutex`); add a top-of-method early-return guard to the READ-ONLY existence/enumeration
  overrides.
- Test: `src/Disks/tests/gtest_ca_transaction.cpp` (extend the `CaLifecycle` suite).

**Benign-absent set** (early return BEFORE any `store()`/`partAccess()`): `existsFile`→false,
`existsDirectory`→false, `existsFileOrDirectory`→false, `isDirectoryEmpty`→true, `listDirectory`→{},
`iterateDirectory`→empty iterator, `getStorageObjectsIfExist`→nullopt.
**Still-throw set** (unchanged, fail-close): `getFileSize`, `getLastModified`, `getStorageObjects`
(non-IfExist), all mutating/transaction ops.

- [ ] Step 1: gtest (RED): startup → unmountSynchronously → assert `existsDirectory("store")`==false,
  `existsFile(...)`==false, `listDirectory(...)`.empty(), `isDirectoryEmpty(...)`==true,
  `getStorageObjectsIfExist(...)`==nullopt; AND assert `getFileSize(...)` / `getStorageObjects(...)`
  still throw `INVALID_STATE`. (RED: today they all throw.)
- [ ] Step 2: add `isMounted()` + the early-return guards; verify GREEN + full CA gate.
- [ ] Step 3: commit (`cas: a dormant CA disk answers existence/enumeration probes as absent, not INVALID_STATE`).
