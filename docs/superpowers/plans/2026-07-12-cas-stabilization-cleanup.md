# CAS Stabilization & Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the dangerous/clean-fix defects, dead-code removals, and behavior-preserving simplifications identified by the 2026-07-12 umbrella review + CAS archaeology report, while minimizing edits to files shared with upstream (the feature is fork-maintained).

**Architecture:** Six sequenced phases over the content-addressed storage (CAS) feature. Phase 1 fixes crash/corruption (highest danger); Phase 2 remaining correctness; Phase 3 removes vestigial state; Phase 4 dedups/simplifies; Phase 5 perf/operability; Phase 6 encapsulation + surface polish (the one med-risk Ring-2 refactor lands last, revertible in isolation). Spec: `docs/superpowers/specs/2026-07-12-cas-stabilization-cleanup-design.md`.

**Tech Stack:** C++23, ClickHouse (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`), gtest (`src/Disks/tests/gtest_cas_*.cpp`, `unit_tests_dbms`), protobuf codecs (`Core/Proto/cas_format.proto`), CMake glob-sourced build.

## Global Constraints

- **Three-ring footprint model (governs every task).** Ring 0 = inside `ContentAddressed/` (+ `Core/`): free rein. Ring 1 = CA-specific append-only lines in shared files (enum entries, ProfileEvents, CurrentMetrics, system-table columns, the `registerContentAddressedMetadataStorage` function, CA `SYSTEM`-command case handlers): conflict-free, allowed. Ring 2 = shared hot upstream code (`MergeTask`, `PocoHTTPClient` core, `S3::Client`, `DiskSelector`, generic `IMetadataTransaction`/`DiskObjectStorageTransaction`): touch **only** when the delta shrinks/neutralizes the existing fork diff, fixes a real bug, or makes the code simpler/safer/better-reused — never to add new surface.
- **Source of truth = committed HEAD.** The working tree has unrelated noise; ignore it.
- **C++ style:** Allman braces (opening brace on its own line). `chassert` is compiled out in release — a `chassert` is **not** a release-time fail-close; use `throw Exception(...)` where a real guard is required.
- **No `sleep` in concurrency tests.** Use deterministic fault-injection, the in-memory/instrumented backend, or a controlled second thread gated on a `std::atomic`.
- **Messages/comments:** ASan (not ASAN); say "exception" not "crash" for logical errors; wrap literal SQL/class/function names in inline code.
- **Build/test:** redirect `ninja` output to a build-dir log and have a subagent summarize it; run gtests as `cd <build> && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='<Suite>.<Test>'`. Substitute your build dir (`build`, `build_debug`, `build_asan`) for `<build>`; A4's negative test needs a build with `chassert` active (debug/ASan). Do not pass `-j`/`nproc` to ninja.
- **Commits:** add-only (no amend/rebase); end every commit message with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- **Out-of-C++-scope tests** (stateless privilege/schema/projection) are described with exact intent where they arise (E1, E3, C5, B3); add them under the CA-default praktika job.

---

## Phase 1 — Crash / corruption fixes

### Task A1: `~Store()` teardown vs. self-remount abort

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp` (`Store::~Store()` `:437-462`, `Store::scheduleRemount()` `:681-707`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h` (remount members `:890-898`; add test seams near `bool tryRemountOnce();` `:524`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h` (`MountLeaseKeeper` `:286-336`)
- Test: `src/Disks/tests/gtest_cas_store.cpp` (Modify — add to the existing `CasStoreRemount` suite)

**Interfaces:**
- Consumes: `Store::scheduleRemount()`, `Store::~Store()`, `remount_thread`/`remount_thread_mutex`/`remount_stop`/`remount_running` (`CasStore.h:892-898`), `MountLeaseKeeper::stop()`/`stopBackground()` (base `SingleWriterSlot`), the `on_lost → scheduleRemount()` wiring in `Store::open` (`CasStore.cpp:395-398`), `PoolConfig::background_watermark`.
- Produces: `std::atomic<bool> Store::remount_shutting_down{false}` (new member); `MountLeaseKeeper::~MountLeaseKeeper() override`; test seams `bool Store::scheduleRemountForTest()` and `void Store::beginShutdownForTest()`.

Note (real-code deviation): a fully deterministic, UB-free reproduction of the actual teardown/re-arm abort is not achievable from a unit test (it requires pausing `~Store()` between its join and `mount_keeper->stop()`). The test therefore pins the exact regression mechanism — `scheduleRemount()` must refuse to arm a thread once teardown has latched `remount_shutting_down` — which is the guard that prevents the abort. Two small test seams (added in Step 1 as scaffolding) express it deterministically and sleep-free.

- [ ] **Step 1: Write the failing test**

Add the two seams + member first (test scaffolding — inert until Step 3 enforces the guard). In `CasStore.h`, after `bool tryRemountOnce();` (`:524`):

```cpp
    /// A1 test seam: drive the (private) self-remount arm/refuse path directly — in production the
    /// keeper's on_lost callback calls scheduleRemount, otherwise reachable only via the background
    /// renewer's cadence. Returns true iff a recovery thread is armed after the call.
    bool scheduleRemountForTest();
    /// A1 test seam: latch `remount_shutting_down` exactly as ~Store() does at its top, WITHOUT tearing
    /// the Store down, so a test can assert scheduleRemount refuses to spawn once teardown has begun.
    void beginShutdownForTest();
```

In `CasStore.h`, add the member after `std::atomic<bool> remount_stop{false};` (`:895`):

```cpp
    std::atomic<bool> remount_shutting_down{false};   /// A1: latched at ~Store() top; scheduleRemount refuses to re-arm during teardown
```

In `CasStore.cpp`, add the seam definitions next to `scheduleRemount()`:

```cpp
bool Store::scheduleRemountForTest()
{
    scheduleRemount();
    std::lock_guard g(remount_thread_mutex);
    return remount_thread.joinable();
}

void Store::beginShutdownForTest()
{
    std::lock_guard g(remount_thread_mutex);
    remount_shutting_down.store(true);
}
```

The test, appended to the `CasStoreRemount` suite in `gtest_cas_store.cpp`:

```cpp
TEST(CasStoreRemount, ShutdownGuardRefusesToArmRemount)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// background_watermark = true so scheduleRemount actually arms a recovery thread in production mode
    /// (the same gate every background thread checks).
    auto store = DB::Cas::Store::open(backend,
        DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test", .background_watermark = true});

    /// Teardown has begun: ~Store() latches this at its very top, BEFORE its only remount-thread join.
    store->beginShutdownForTest();

    /// A lease-renewal failure firing DURING teardown re-enters scheduleRemount (the keeper's on_lost
    /// callback). With the guard it must refuse to spawn; without it, it arms remount_thread AFTER
    /// ~Store()'s join — the leftover joinable ThreadFromGlobalPool handle then abort()s the process at
    /// member destruction (std::terminate). Reading joinable() immediately after the synchronous call is
    /// race-free: the armed thread never touches the handle.
    EXPECT_FALSE(store->scheduleRemountForTest())
        << "scheduleRemount must not arm a recovery thread once teardown has begun";
}
```

- [ ] **Step 2: Run test, verify it fails**

Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CasStoreRemount.ShutdownGuardRefusesToArmRemount'`
Expected: FAIL — `scheduleRemount()` does not yet consult `remount_shutting_down`, so with `background_watermark=true` it arms `remount_thread` and `scheduleRemountForTest()` returns `true`; `EXPECT_FALSE` fails. (This is the exact condition that, in a real `~Store()`, re-arms the thread after its only join and abort()s the process.)

- [ ] **Step 3: Implement the minimal fix**

In `CasStore.cpp`, `scheduleRemount()` — check the flag first (double-checked, like `remount_running`):

```cpp
void Store::scheduleRemount()
{
    if (!config.background_watermark)
        return;   /// tests drive tryRemountOnce explicitly (the same gate as every background thread)
    if (remount_shutting_down.load() || remount_running.load())
        return;
    std::lock_guard g(remount_thread_mutex);
    if (remount_shutting_down.load() || remount_running.load())
        return;
    if (remount_thread.joinable())
        remount_thread.join();   /// a PREVIOUS recovery finished; reap it before starting a new one
    remount_running.store(true);
    remount_thread = ThreadFromGlobalPool([this]
    {
        // ... unchanged body ...
    });
}
```

In `CasStore.cpp`, `Store::~Store()` — latch the flag at the top and re-join after `mount_keeper->stop()`:

```cpp
Store::~Store()
{
    /// A1: refuse any further self-remount arming for the rest of teardown. Latched under the thread
    /// mutex (paired with scheduleRemount's checks) BEFORE the join below, so a keeper on_lost firing
    /// during teardown can never re-arm remount_thread after we join it.
    {
        std::lock_guard g(remount_thread_mutex);
        remount_shutting_down.store(true);
    }
    /// Stop the self-remount recovery loop FIRST: it may otherwise re-create the keeper below us.
    remount_stop.store(true);
    remount_cv.notify_all();
    {
        std::lock_guard g(remount_thread_mutex);
        if (remount_thread.joinable())
            remount_thread.join();
    }

    // ... unchanged mount_keeper->stop() block (retire the merged heartbeat) ...
    if (mount_keeper)
    {
        try
        {
            mount_keeper->stop();
        }
        catch (...)
        {
            tryLogCurrentException(getLogger("CasStore"), "CAS mount-lease: release during Store teardown failed");
        }
    }

    /// A1: belt-and-suspenders re-join. The shutting-down flag makes scheduleRemount a no-op above, so
    /// this is normally not joinable; it closes the residual window where a keeper on_lost between the
    /// first join and stop() observed the flag late.
    {
        std::lock_guard g(remount_thread_mutex);
        if (remount_thread.joinable())
            remount_thread.join();
    }
}
```

In `CasServerRoot.h`, give `MountLeaseKeeper` its own destructor (after `void stop() { doTerminate(); }`, `:303`):

```cpp
    /// A1: join the renewal thread BEFORE this object's own `std::function` members (on_renew_ok /
    /// on_lost, which reach back into the Store) are destroyed. The base ~SingleWriterSlot also calls
    /// stopBackground(), but it runs AFTER the derived members are gone — a renewal firing on_lost in
    /// that window would call a destroyed std::function. Stopping here closes that window.
    ~MountLeaseKeeper() override { stopBackground(); }
```

- [ ] **Step 4: Run test, verify it passes**

Run: `./src/unit_tests_dbms --gtest_filter='CasStoreRemount.*'`
Expected: PASS (the new guard test plus the three existing `CasStoreRemount` tests).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h \
        src/Disks/tests/gtest_cas_store.cpp
git commit -m "cas: stop self-remount from abort()ing the process during Store teardown

scheduleRemount guarded remount_running/background_watermark but not a
shutdown flag, so a keeper on_lost firing during ~Store() re-armed
remount_thread after its only join -> ThreadFromGlobalPool dtor abort().
Latch remount_shutting_down at teardown top, refuse to spawn past it,
re-join after mount_keeper->stop(), and join the keeper's renewal thread
in ~MountLeaseKeeper before its std::function members are destroyed.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task A2: `RunFileReader::next()` heap OOB via un-checksummed `record_count`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.cpp` (anon-namespace CRC helper `:39-42`; `RunFileWriter::flushBlock` `:92-118`, CRC at `:109`; `RunFileReader::installBlockFrame` `:360-390`; `RunFileReader::next` `:435-459`)
- Test: `src/Disks/tests/gtest_cas_run_file.cpp` (Modify — add to the existing corruption-test group)

**Interfaces:**
- Consumes: `crc32cOf(std::string_view)` (`:39`), `crc32c::Extend` (`contrib/crc32c/include/crc32c/crc32c.h:50`), `le32of` (`:165`), `putLE32`/`putLenPrefixed`, the block frame `block_len u32, record_count u32, min_key(lp), max_key(lp), crc32c u32, payload`.
- Produces: none (internal fix; adds an overload `uint32_t crc32cOf(std::string_view head, std::string_view payload)` in the anonymous namespace). On-disk framing changes (CRC now covers the block head) — pre-release, writer+reader updated together; run files are attempt-scoped and short-lived.

- [ ] **Step 1: Write the failing test**

Add after `CorruptedBlockCountInFooterFailsClosed` in `gtest_cas_run_file.cpp` (reuses the file's `expectCorruptedData`/`constructAndDrain` helpers and `writeRun`):

```cpp
TEST(CasRunFile, CorruptedBlockRecordCountFailsClosed)
{
    /// A2: the per-block record_count lives in the block HEAD, which the block CRC did NOT cover (only
    /// the record payload was checksummed). Inflating record_count slipped past installBlockFrame, and
    /// next() then raw-indexed the block past its end via an unchecked le32at + substr -> heap OOB read
    /// / std::out_of_range (a hard crash under libc++ hardening), violating CasRunFile.h's fail-closed
    /// contract. With the head in the CRC (and next() bounds-checked) the mutation is CORRUPTED_DATA.
    const String valid = writeRun({{"aa", "payload-bytes"}, {"bb", "more"}, {"cc", "third"}});

    /// File layout: 13-byte header, then the first block frame: block_len u32 @13, record_count u32 @17.
    constexpr size_t header_len = 13;
    constexpr size_t record_count_off = header_len + 4;   /// past the block_len prefix
    String b = valid;
    /// Inflate the block's record_count so the reader would loop past the real records.
    b[record_count_off + 0] = static_cast<char>(0xFF);
    b[record_count_off + 1] = static_cast<char>(0x7F);
    expectCorruptedData("inflated block record_count", [&] { constructAndDrain(b); });
}
```

- [ ] **Step 2: Run test, verify it fails**

Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CasRunFile.CorruptedBlockRecordCountFailsClosed'`
Expected: FAIL — `installBlockFrame`'s CRC covers only the payload, so the inflated `record_count` passes; `next()` reads records beyond the payload and either crashes (heap OOB under hardening) or throws `std::out_of_range` from `substr`, which `expectCorruptedData` reports as an escaping non-`DB::Exception` failure.

- [ ] **Step 3: Implement the minimal fix**

In `CasRunFile.cpp`, add the two-argument CRC helper in the anonymous namespace after `crc32cOf` (`:42`):

```cpp
/// A2: CRC over the block HEAD (record_count, min_key, max_key) followed by the payload — so the
/// record_count that next() trusts is itself checksummed, not just the record payload.
uint32_t crc32cOf(std::string_view head, std::string_view payload)
{
    return crc32c::Extend(crc32cOf(head),
        reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
}
```

In `RunFileWriter::flushBlock` (`:104-109`), cover the head in the CRC:

```cpp
    /// DataBlock: block_len u32, record_count u32, min_key(len-prefixed), max_key(len-prefixed),
    /// crc32c u32, payload. block_len is the byte length of (record_count..payload) inclusive.
    String block_head;
    putLE32(block_head, block_records);
    putLenPrefixed(block_head, block_min_key);
    putLenPrefixed(block_head, block_max_key);
    /// A2: CRC covers the whole block head (incl. record_count) AND the payload, so a corrupted
    /// record_count / key length is caught by installBlockFrame before next() ever trusts it.
    putLE32(block_head, crc32cOf(block_head, block_payload));
```

In `RunFileReader::installBlockFrame` (`:360-381`), verify the CRC over head+payload:

```cpp
void RunFileReader::installBlockFrame(std::string_view frame, size_t block_no)
{
    /// `frame` starts at the block_len u32. Bound-check every field against `frame`, so a
    /// self-inconsistent block fails closed, never over-reads (N4 hardening).
    size_t off = 0;
    const uint32_t block_len = le32of(frame, off); off += 4;
    if (block_len > frame.size() - off)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block field out of bounds");
    const size_t block_end = off + block_len;
    const size_t head_start = off;   /// A2: record_count..max_key are CRC-covered together with the payload
    const uint32_t rec_count = le32of(frame, off); off += 4;
    uint32_t mn = le32of(frame, off); off += 4;
    if (mn > frame.size() - off) throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block field out of bounds");
    off += mn;   /// skip min_key
    uint32_t mx = le32of(frame, off); off += 4;
    if (mx > frame.size() - off) throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block field out of bounds");
    off += mx;   /// skip max_key
    const size_t crc_field_off = off;
    const uint32_t stored_crc = le32of(frame, off); off += 4;
    if (off > block_end)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block header exceeds block");
    const std::string_view head(frame.data() + head_start, crc_field_off - head_start);
    const std::string_view payload(frame.data() + off, block_end - off);
    if (crc32cOf(head, payload) != stored_crc)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block crc mismatch");

    cur_block.assign(payload.data(), payload.size());
    cur_block_pos = 0;
    cur_block_records = rec_count;
    cur_record_no = 0;
    cur_block_idx = block_no;
    block_loaded = true;
}
```

In `RunFileReader::next` (`:435-459`), replace the unchecked `le32at` lambda + `substr` with bounds-checked reads and an exact-consumption backstop:

```cpp
bool RunFileReader::next(String & key, String & payload)
{
    if (exhausted)
        return false;
    if (!block_loaded && !loadBlock(0))
        return false;
    while (cur_record_no >= cur_block_records)
    {
        if (!loadBlock(cur_block_idx + 1))
            return false;
    }
    /// A2: bounds-checked reads over the (now CRC-covered, incl. record_count) block payload — a
    /// self-inconsistent length fails closed with CORRUPTED_DATA, never an unchecked operator[] or a
    /// substr-past-end (std::out_of_range).
    const std::string_view block(cur_block);
    const uint32_t klen = le32of(block, cur_block_pos); cur_block_pos += 4;
    if (klen > cur_block.size() - cur_block_pos)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: record key out of bounds");
    key = cur_block.substr(cur_block_pos, klen); cur_block_pos += klen;
    const uint32_t plen = le32of(block, cur_block_pos); cur_block_pos += 4;
    if (plen > cur_block.size() - cur_block_pos)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: record payload out of bounds");
    payload = cur_block.substr(cur_block_pos, plen); cur_block_pos += plen;
    ++cur_record_no;
    /// A2: after the block's last record the cursor must land EXACTLY on the block end — a record_count
    /// that under- or over-counts the payload is corruption. record_count is now CRC-covered, so this
    /// fires only on a CRC collision or a writer bug, but it is the reader's fail-closed backstop.
    if (cur_record_no == cur_block_records && cur_block_pos != cur_block.size())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block record_count does not match payload");
    return true;
}
```

- [ ] **Step 4: Run test, verify it passes**

Run: `./src/unit_tests_dbms --gtest_filter='CasRunFile.*'`
Expected: PASS (the new corruption case plus all existing round-trip/seek/merge/corruption tests — writer and reader are updated together, so round-trips stay byte-deterministic).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.cpp \
        src/Disks/tests/gtest_cas_run_file.cpp
git commit -m "cas: cover the run-file block head in its CRC and bounds-check next()

record_count sat in the block head outside the CRC (which covered only the
payload), so a corrupted count slipped past installBlockFrame and next()
raw-indexed the block past its end -> heap OOB / std::out_of_range on the GC
fold and manifest-decode paths. Extend the block CRC over the head, read
records via the bounds-checked le32of, and assert exact block consumption.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task A3: `flushRefBatch` wedges the ref-queue leader on an uncaught throw

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp` (`Store::flushRefBatch` — wedge-resolve `resolveByExactGet` site `:1414`; `putIfAbsentControlled` site `:1591`)
- Test: `src/Disks/tests/gtest_cas_ref_writer.cpp` (Modify — add to the `RefWriterAppendLane` suite)

**Interfaces:**
- Consumes: `CasRequestController::resolveByExactGet` (throws `CORRUPTED_DATA` on a different object, `CasRequestControl.cpp:192`), `CasRequestController::putIfAbsentControlled` (its internal resolve can throw `CORRUPTED_DATA`, `CasRequestControl.cpp:237`), the `flushRefBatch`-local lambdas `complete_error`/`carve_all_pending`, `survivors`, `rt->leader_active`/`rt->cv`/`ref_queue_mutex`, `Store::runRefQueueLeader`/`appendRefOps`.
- Produces: none (internal fix; mirrors the existing "durably-committed txn fails to apply" recovery catch, `CasStore.cpp:1605-1641`).

- [ ] **Step 1: Write the failing test**

Add to `gtest_cas_ref_writer.cpp` (uses HEAD helpers: `RefWriterTestBackend` with `fault_key_substr`/`fault_count`/`pending_delayed_write`, `openStore`, `publishEmptyPart`, `refLaneWedgedForTest`, `expectThrowsCode`):

```cpp
/// A3: a CORRUPTED_DATA thrown from the wedge-resolve GET (a foreign writer overwrote the wedged key)
/// must surface to the triggering caller AND restore the queue's leader bookkeeping, so a caller queued
/// behind the leader is not blocked forever. Before the fix the throw unwound through runRefQueueLeader
/// and appendRefOps' post-leader reset with `leader_active` still latched true and `cv` un-notified ->
/// every queued and future caller for that table hung in cv.wait.
TEST(RefWriterAppendLane, WedgeResolveThrowDoesNotWedgeQueueLeader)
{
    CasRequestBudget budget;
    budget.max_attempts = 1;
    budget.attempt_timeout_ms = 100;
    budget.operation_deadline_ms = 100;
    budget.lease_safety_margin_ms = 100;

    auto backend = std::make_shared<RefWriterTestBackend>();
    auto store = openStore(backend, budget);
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/a3_wedge_resolve"};
    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    /// Wedge the lane: the next `_log` PUT returns an ambiguous (Unresolved-classified) result that
    /// never lands. The wedged (key, bytes) are captured in `pending_delayed_write`.
    backend->fault_key_substr = layout.refsNamespacePrefix(ns) + "_log/";
    backend->fault_count = 1;
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { store->dropRef(ns, "x"); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));

    /// A foreign writer lands a DIFFERENT object at the exact wedged key. The next append's wedge-resolve
    /// GET now observes a mismatch and resolveByExactGet raises CORRUPTED_DATA.
    ASSERT_TRUE(backend->pending_delayed_write.has_value());
    const String wedged_key = backend->pending_delayed_write->first;
    ASSERT_EQ(backend->putIfAbsent(wedged_key, "a-foreign-different-object").outcome, PutOutcome::Done);

    /// Two callers race into the SAME table's queue: one becomes the leader (hitting the throwing
    /// wedge-resolve), the other queues behind it. Both must return promptly with the corruption — a
    /// leaked leader_active would hang the SECOND one forever (caught here by a bounded wait, which fails
    /// the test rather than stalling it — the file's established hang-detection style).
    auto c1 = std::async(std::launch::async, [&]
    {
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { store->dropRef(ns, "y"); });
    });
    auto c2 = std::async(std::launch::async, [&]
    {
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { store->dropRef(ns, "y"); });
    });
    ASSERT_EQ(c1.wait_for(std::chrono::seconds(10)), std::future_status::ready)
        << "the leader's wedge-resolve throw hung the queue";
    ASSERT_EQ(c2.wait_for(std::chrono::seconds(10)), std::future_status::ready)
        << "the caller queued behind the leader hung — leader_active was not restored after the throw";
    c1.get();
    c2.get();

    EXPECT_TRUE(store->refLaneWedgedForTest(ns)) << "corruption at the wedged key must keep the lane wedged (fail closed)";
}
```

- [ ] **Step 2: Run test, verify it fails**

Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='RefWriterAppendLane.WedgeResolveThrowDoesNotWedgeQueueLeader'`
Expected: FAIL (hang) — `resolveByExactGet` throws out of the unguarded site; `runRefQueueLeader`/`appendRefOps` unwind with `rt->leader_active == true` and no `cv.notify_all()`, so the second caller blocks in `cv.wait` forever; the 10s bounded wait trips and the test fails.

- [ ] **Step 3: Implement the minimal fix**

In `flushRefBatch`, wrap the wedge-resolve site (`:1414`):

```cpp
        if (wedge_copy)
        {
            CasWriteOutcome resolved;
            try
            {
                resolved = ref_request_controller->resolveByExactGet(wedge_copy->key, wedge_copy->bytes);
            }
            catch (...)
            {
                /// A3: resolveByExactGet throws CORRUPTED_DATA when the durable object at the wedged key
                /// differs from ours (a foreign overwrite). Unguarded, this unwinds through
                /// runRefQueueLeader and appendRefOps' post-leader reset, leaving `leader_active` latched
                /// true and `cv` un-notified — every queued and future caller for this table then hangs
                /// forever. Complete every queued caller with the error, restore the leader bookkeeping
                /// here (this catch bypasses appendRefOps' normal reset, since we rethrow past it), and
                /// keep the wedge (fail closed on corruption). Mirrors the durably-committed-apply catch.
                complete_error(carve_all_pending(), std::current_exception());
                {
                    std::lock_guard<std::mutex> g(ref_queue_mutex);
                    rt->leader_active = false;
                    rt->cv.notify_all();
                }
                throw;
            }
            if (resolved == CasWriteOutcome::Committed)
            {
                // ... unchanged apply-then-unwedge branch ...
```

And the create/commit site (`:1591`) — the carved `survivors` are already popped from `rt->pending`, so complete THEM:

```cpp
    CasWriteOutcome outcome;
    try
    {
        outcome = ref_request_controller->putIfAbsentControlled(key, bytes, fence_ok);
    }
    catch (...)
    {
        /// A3: putIfAbsentControlled's internal resolve-before-reissue can throw CORRUPTED_DATA (a proven
        /// different-object conflict) straight out. The survivors are already popped from rt->pending, so
        /// complete THEM (not just the still-pending waiters), restore the leader bookkeeping, and
        /// rethrow — otherwise this table's whole mutation lane wedges.
        complete_error(survivors, std::current_exception());
        {
            std::lock_guard<std::mutex> g(ref_queue_mutex);
            rt->leader_active = false;
            rt->cv.notify_all();
        }
        throw;
    }
    switch (outcome)
    {
        // ... unchanged Committed / DefiniteFailure / Unresolved cases ...
```

- [ ] **Step 4: Run test, verify it passes**

Run: `./src/unit_tests_dbms --gtest_filter='RefWriterAppendLane.*:RefWriter*'`
Expected: PASS (the new test plus all existing wedge/append-lane tests — the wedge is kept on corruption and the lane stays usable).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/tests/gtest_cas_ref_writer.cpp
git commit -m "cas: don't wedge the ref-queue leader on an uncaught CORRUPTED_DATA

resolveByExactGet (wedge resolve) and putIfAbsentControlled can throw
CORRUPTED_DATA on a proven different-object conflict; both flushRefBatch
sites were unguarded, so the throw skipped leader_active=false / cv.notify_all
and hung the namespace's whole mutation path until restart. Complete the
affected items with the exception, restore the leader bookkeeping, and
rethrow — the same recovery shape as the durably-committed-apply catch.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task A4: EDGE-BEFORE-OBSERVE invariant guarded only by `chassert`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp` (`Build::observeAndAdmit` 3-arg `chassert(precommitted)` `:247`; 4-arg `chassert(precommitted)` `:270`)
- Test: `src/Disks/tests/gtest_cas_build.cpp` (Modify — add to the `CasBuild` suite)

**Interfaces:**
- Consumes: `Build::observeAndAdmit(ObjectKind, const BlobRef&, const String&)` and its 4-arg overload, `Build::precommitted` (set true in `precommitAdd`, `CasBuild.cpp:989`), `ErrorCodes::LOGICAL_ERROR` (already `extern` in the file), `Build::putBlob` → `uploadFromSource` → `reviveObserve` adopt path.
- Produces: none (internal fix — promotes both `chassert(precommitted)` to a real `throw Exception(ErrorCodes::LOGICAL_ERROR, ...)`; `chassert` is compiled out in release, so a throw is required).

Note: both overloads carry the identical `chassert`; the 3-arg delegates to the 4-arg. Promote BOTH so the invariant is a real throw on every adopt entry (the 3-arg short-circuits before its HEAD; the 4-arg is also reached directly from `putBlob`'s HEAD-first dedup path `:179`).

- [ ] **Step 1: Write the failing test**

Add to `gtest_cas_build.cpp` (reuses `openStore`, `startBuildFor`, `writeRawBlobBody`, `writeMetaClean`, `idOf`, `u128Of`, `expectThrowsCode` — exactly the setup of `PutBlobAdoptsWhenMetaCleanNoRetireView`, minus the precommit):

```cpp
/// A4 (negative): observeAndAdmit's EDGE-BEFORE-OBSERVE invariant — adopting an EXISTING incarnation is
/// safe ONLY under this build's durable precommit closure — was guarded only by chassert(precommitted),
/// which is compiled out in release. A putBlob that reaches the adopt path with NO precommit (the wiring
/// order stageManifest -> precommitAdd -> putBlob violated) must fail closed with a real LOGICAL_ERROR,
/// not silently adopt a blob the newborn-debris watermark does not cover.
TEST(CasBuild, AdoptBeforePrecommitFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);

    const String payload = "adopt-before-precommit-payload";
    const UInt128 hash = u128Of(payload);
    const BlobRef id = idOf(payload);

    /// Pre-seed a present body (padded past the pool header so the logical-size guard does not
    /// underflow) + an independent Clean meta, so putBlob's upload conflicts on the present object and
    /// takes the ADOPT branch of observeAndAdmit — mirroring PutBlobAdoptsWhenMetaCleanNoRetireView.
    const uint64_t header_len = s->poolMeta().blob_header_len;
    String raw_body(header_len, '\0');
    raw_body += payload;
    writeRawBlobBody(*b, s->layout(), hash, raw_body);
    writeMetaClean(*b, s->layout(), hash, payload.size());

    /// Start a build but DO NOT call precommitAdd: the adopt runs with `precommitted == false`.
    const RootNamespace ns{"srv/tbl"};
    auto build = startBuildFor(s, ns, "ref_adopt");

    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { build->putBlob(id, BlobSource::fromString(payload)); });
}
```

- [ ] **Step 2: Run test, verify it fails**

Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CasBuild.AdoptBeforePrecommitFailsClosed'`
Expected: FAIL. In a build with `chassert` active (debug/ASan) the process aborts on `chassert(precommitted)`. In a build where `chassert` is compiled out (release), `putBlob` silently adopts and returns, so `expectThrowsCode(LOGICAL_ERROR, ...)` fails with "no exception thrown". Either way the test fails. (If the executor's `build/` compiles out `chassert`, this is a clean gtest failure; otherwise run against `build_debug`/`build_asan`.)

- [ ] **Step 3: Implement the minimal fix**

Replace `chassert(precommitted);` at the 3-arg overload (`:247`) and the 4-arg overload (`:270`) with the same real throw:

```cpp
    /// EDGE-BEFORE-OBSERVE (spec 2026-07-09-cas-writer-gc-simplification): adopting an EXISTING
    /// incarnation is safe ONLY under this build's durable precommit closure — an adopted blob carries the
    /// ORIGINAL writer's build_id, so the newborn-debris watermark does not cover it. Fresh uploads
    /// pre-precommit stay legal (watermark-protected). See the TLA+ order sabotage (Gate A). A4: a real
    /// throw, not chassert — chassert is compiled out in release, and a wiring/retry bug that reached
    /// adopt without a durable precommit would silently drop watermark protection (later dangling ref /
    /// data loss with no production signal).
    if (!precommitted)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "Build::observeAndAdmit: EDGE-BEFORE-OBSERVE invariant violated — adopting an existing "
            "incarnation before this build's precommit is durable would admit {} ({}) under the original "
            "writer's build_id with no newborn-debris watermark protection",
            key, kind == ObjectKind::Blob ? "blob" : "manifest");
```

- [ ] **Step 4: Run test, verify it passes**

Run: `./src/unit_tests_dbms --gtest_filter='CasBuild.*:CasBuildReuseBlob.*'`
Expected: PASS. The new negative test throws `LOGICAL_ERROR`; the existing adopt tests (`PutBlobDedupSecondWriterAdopts`, `PutBlobAdoptsWhenMetaCleanNoRetireView`) precommit before the adopt so `precommitted == true` and stay green; fresh-upload tests never reach `observeAndAdmit`.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/tests/gtest_cas_build.cpp
git commit -m "cas: fail closed (not chassert) when adopting before precommit

observeAndAdmit's EDGE-BEFORE-OBSERVE invariant (adopt only under a durable
precommit closure) was guarded by chassert(precommitted), compiled out in
release; a future wiring/retry bug could silently adopt an existing
incarnation with no watermark protection. Promote both overloads' checks to
a real LOGICAL_ERROR throw.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Phase 2 — Remaining correctness fixes

### Task A5: Fold Ordinary-engine detached parts into the table namespace

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.cpp:117-125` (`findPartDirComponent`, non-Atomic fallback)
- Test: `src/Disks/tests/gtest_ca_wiring.cpp` (Modify — add a `CaPartPathParser` test next to the Atomic-form `DetachedPathsReportTheSharedDetachedComponent` at line 127)

**Interfaces:**
- Consumes: `parsePartFilePath`, `kDetachedDirName`, `PartFilePath` (`table_uuid`/`part_name`/`file`)
- Produces: none (internal fix — the parser output shape that `ContentAddressedMetadataStorage::route` already keys `detached` folding on at `.cpp:591`)

Note: the real fix anchors on `detached` by an explicit **left-to-right first-match** scan placed *before* the existing right-to-left part-dir scan (not "special-case while scanning right-to-left" as the spec sketches) — right-to-left would hit the inner `attaching_all_0_0_0` component first. Anchoring on `detached` first also makes the bare non-Atomic container dir parse symmetrically with the Atomic layout (empty `file` ⇒ `route`'s empty-ref filtered-listing path).

- [ ] **Step 1: Write the failing test**
```cpp
TEST(CaPartPathParser, DetachedPathsNonAtomicFoldIntoTheTableNamespace)
{
    // U#6: the Ordinary/non-Atomic detached form data/<db>/<table>/detached/<part>/<file> must fold
    // into the table's OWN namespace with part_name == "detached" (mirroring the Atomic form), so
    // route() keys the detached/<part> ref off it. The right-to-left part-dir scan would otherwise
    // anchor on the INNER part dir and fold `detached` into a spurious table_uuid
    // ("data/<db>/<table>/detached") that DROP TABLE never cleans — a permanently orphaned live ref.
    auto d = parsePartFilePath("data/db/tbl/detached/attaching_all_0_0_0/metadata_version.txt");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->table_uuid, "data/db/tbl");
    EXPECT_EQ(d->part_name, std::string(kDetachedDirName));
    EXPECT_EQ(d->file, "attaching_all_0_0_0/metadata_version.txt");

    // The bare non-Atomic detached CONTAINER dir folds to part_name == "detached" with an empty file,
    // exactly like the Atomic container, so route()'s empty-ref branch is reached for both layouts.
    auto c = parsePartFilePath("data/db/tbl/detached");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->table_uuid, "data/db/tbl");
    EXPECT_EQ(c->part_name, std::string(kDetachedDirName));
    EXPECT_TRUE(c->file.empty());
}
```

- [ ] **Step 2: Run test, verify it fails**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CaPartPathParser.DetachedPathsNonAtomicFoldIntoTheTableNamespace'`
Expected: FAIL — on HEAD the part form yields `table_uuid == "data/db/tbl/detached"` and `part_name == "attaching_all_0_0_0"` (first `EXPECT_EQ` fails); the container form returns `nullopt` (the `ASSERT_TRUE(c.has_value())` fails).

- [ ] **Step 3: Implement the minimal fix**
In `findPartDirComponent`, replace the non-Atomic fallback (lines 118-124):
```cpp
    // No uuid anchor: a non-Atomic table path. The MergeTree `detached` dir is a reserved table-level
    // subdir (data/<db>/<table>/detached/<part>/...), exactly like the Atomic layout where the uuid
    // anchor makes `detached` the part_name. Anchor on it FIRST (leftmost, index >= 1): the right-to-
    // left part-dir scan below would otherwise anchor on the INNER <part>-shaped component and fold
    // `detached` into a spurious table id (data/<db>/<table>/detached) that DROP TABLE never cleans,
    // orphaning a permanently-live ref (U#6). Mirrors route()'s part_name == kDetachedDirName folding.
    for (size_t i = 1; i < p.size(); ++i)
        if (p[i] == kDetachedDirName)
            return PartDirAnchor{0, i}; // table id = the whole path before `detached`

    // The table identifier must be at least one component (a real table dir, never the bare disk
    // root), so the part dir is at index >= 1. Scan right to left so a part-dir-shaped
    // table/partition name earlier in the path cannot steal the anchor.
    for (size_t i = p.size(); i-- > 1;)
        if (looksLikePartDir(p[i]))
            return PartDirAnchor{0, i}; // table id = the whole path before the part dir
    return std::nullopt;
```

- [ ] **Step 4: Run test, verify it passes**
Run: `./src/unit_tests_dbms --gtest_filter='CaPartPathParser.*'`
Expected: PASS (the new test and every existing `CaPartPathParser`/`CaWiring*` case — the Atomic detached path still hits the uuid anchor and never reaches this fallback).

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.cpp src/Disks/tests/gtest_ca_wiring.cpp
git commit -m "cas: fold Ordinary-engine detached parts into the table namespace

The non-Atomic part-path fallback anchored on the rightmost part-dir-shaped
component, so data/<db>/<table>/detached/<part>/... parsed to a spurious
table_uuid ending in /detached — a namespace DROP TABLE never cleans, leaving
a permanently orphaned live ref. Anchor on the reserved detached component
first, mirroring the Atomic layout and route()'s detached-ref folding.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task A6: Skip the CAS capability probe under `skip_access_check`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h:211` (`PoolConfig`), `.../Core/CasStore.cpp:228` (`Store::open` probe gate)
- Modify (wiring): `.../ContentAddressed/ContentAddressedMetadataStorage.h:100,304` (ctor trailing param + member), `.cpp:180,408` (init list + `startup()` `pool_config`), `.../MetadataStorages/MetadataStorageFactory.cpp:308-314` (`registerContentAddressedMetadataStorage`)
- Test: `src/Disks/tests/gtest_cas_store.cpp` (Modify — add next to `CasStore.ReadOnlyOpenSkipsProbe` at line 131, reusing that file's `WriteCountingBackend` shape)

**Interfaces:**
- Consumes: `Cas::Store::open`, `Cas::PoolConfig`, `runCapabilityProbe`, `config.getBool(config_prefix + ".skip_access_check", ...)`
- Produces: new field `bool PoolConfig::skip_access_check = false;`

Note (deviation): threading `skip_access_check` all the way from `IDisk::startup(bool)` is out of scope — `IDisk::startupImpl()` (Ring 2, shared) drops the flag before `metadata_storage->startup()` runs. The Ring-0 fix reads the per-disk `<skip_access_check>` config directive directly in the CA factory (the operator "start now, fix later" path the spec motivates). The process-global `global_skip_access_check` still requires the deferred Group-G `startupImpl` plumbing — noted, not fixed here.

- [ ] **Step 1: Write the failing test**
```cpp
namespace
{
/// Records whether any MUTATING op touched a `_probe/` key, so a test can assert an open ran (or
/// skipped) the capability probe. Mirrors WriteCountingBackend above but keys on the probe subtree.
class ProbeWatchingBackend final : public DB::Cas::Backend
{
public:
    explicit ProbeWatchingBackend(std::shared_ptr<DB::Cas::Backend> inner_) : inner(std::move(inner_)) {}
    bool probe_touched = false;

    std::optional<DB::Cas::GetResult> get(const String & k, DB::Cas::Range r = {}) override { return inner->get(k, r); }
    std::optional<DB::Cas::GetStreamResult> getStream(const String & k, DB::Cas::Range r = {}) override { return inner->getStream(k, r); }
    DB::Cas::HeadResult head(const String & k) override { return inner->head(k); }
    DB::Cas::ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
    DB::Cas::PutResult putIfAbsent(const String & k, const String & b, const DB::Cas::ObjectMeta & m = {}) override { note(k); return inner->putIfAbsent(k, b, m); }
    DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & m = {}) override { note(k); return inner->putIfAbsentStream(k, m); }
    DB::Cas::PutResult putOverwrite(const String & k, const String & b, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & m = {}) override { note(k); return inner->putOverwrite(k, b, e, m); }
    DB::Cas::CasResult casPut(const String & k, const String & b, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & m = {}) override { note(k); return inner->casPut(k, b, e, m); }
    DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & t) override { note(k); return inner->deleteExact(k, t); }
    bool supportsListTokens() const override { return inner->supportsListTokens(); }
private:
    void note(const String & k) { if (k.find("/_probe/") != String::npos) probe_touched = true; }
    std::shared_ptr<DB::Cas::Backend> inner;
};
}

TEST(CasStore, SkipAccessCheckOpenSkipsProbeButStaysWritable)
{
    auto shared = std::make_shared<DB::Cas::InMemoryBackend>();

    DB::Cas::PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(1);
    cfg.server_root_id = "srv-1";

    /// Baseline: a normal writable open runs the capability probe (PUT+delete of `_probe/` keys).
    {
        auto watch = std::make_shared<ProbeWatchingBackend>(shared);
        auto s = DB::Cas::Store::open(watch, cfg);
        ASSERT_NE(s, nullptr);
        EXPECT_TRUE(watch->probe_touched) << "the probe must run by default";
    }

    /// skip_access_check open ("start now, fix later"): NO probe I/O, yet still a WRITABLE mount
    /// (owner/epoch/mount/watermark bootstrap writes still happen — unlike a read_only open, which is
    /// a total no-op). Distinct root over the same (now-created) pool.
    {
        auto watch = std::make_shared<ProbeWatchingBackend>(shared);
        DB::Cas::PoolConfig sac = cfg;
        sac.server_id = DB::UInt128(2);
        sac.server_root_id = "srv-2";
        sac.skip_access_check = true;
        auto s = DB::Cas::Store::open(watch, sac);
        ASSERT_NE(s, nullptr);
        EXPECT_FALSE(watch->probe_touched) << "skip_access_check must perform no probe I/O";
    }
}
```

- [ ] **Step 2: Run test, verify it fails**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CasStore.SkipAccessCheckOpenSkipsProbeButStaysWritable'`
Expected: FAIL — compile error `no member named 'skip_access_check' in 'DB::Cas::PoolConfig'`, confirming the knob does not exist. (After Step 3 adds the field but before wiring the gate, it would instead fail at runtime: `probe_touched` is `true`.)

- [ ] **Step 3: Implement the minimal fix**
In `CasStore.h`, add to `PoolConfig` after `read_only` (line 211):
```cpp
    /// Boot-time "start now, fix later": skip the mutating capability probe (mirrors `checkAccess`'s
    /// `skip_access_check` gate) while STILL opening writable — a mistyped bucket / transient DNS blip
    /// at mount should not hard-fail the disk when the operator asked to defer the access check (U#5).
    bool skip_access_check = false;
```
In `CasStore.cpp`, change the probe gate at line 228:
```cpp
    if (!config.read_only && !config.skip_access_check)
```
Wiring — `ContentAddressedMetadataStorage.h`: add a trailing ctor param after `bool blob_hash_allow_new_ = false)` (line 100 → `... = false,\n        bool skip_access_check_ = false);`) and a member after `const bool blob_hash_allow_new;` (line 304):
```cpp
    const bool skip_access_check;
```
`ContentAddressedMetadataStorage.cpp`: add to the ctor init list after `, blob_hash_allow_new(blob_hash_allow_new_)` (line 180): `, skip_access_check(skip_access_check_)`; and in `startup()` after `pool_config.read_only = read_only;` (line 408):
```cpp
    pool_config.skip_access_check = skip_access_check;
```
`MetadataStorageFactory.cpp`: read the directive near the other `config.getBool` reads (~line 266) and pass it as the new trailing constructor argument (after `blob_hash_allow_new`, line 314):
```cpp
        const bool skip_access_check = config.getBool(config_prefix + ".skip_access_check", false);
```
```cpp
            manifest_decode_cache_bytes, gc_meta_pool_size, staging_backend, blob_hash_algo, blob_hash_allow_new,
            skip_access_check);
```

- [ ] **Step 4: Run test, verify it passes**
Run: `./src/unit_tests_dbms --gtest_filter='CasStore.*'`
Expected: PASS (the new test plus `ReadOnlyOpenSkipsProbe` and the rest of the `CasStore`/`CasPoolMeta` suites — the added `PoolConfig` field defaults to `false`, so all designated-initializer call sites are unchanged).

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp src/Disks/tests/gtest_cas_store.cpp
git commit -m "cas: honor skip_access_check for the startup capability probe

The write/delete capability probe ran unconditionally in Store::open, before
the skip_access_check-gated checkAccess, so a mistyped bucket or transient DNS
blip at boot hard-failed the mount even when the operator configured the
standard start-now-fix-later path. Thread the per-disk skip_access_check
directive into PoolConfig and skip the probe when set, keeping the mount
writable (owner/epoch/mount/watermark still run), mirroring checkAccess.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task A7: Manual `SYSTEM … GC` reuses a stable `Gc` and serializes against the loop

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.h:53-67` (add `Cas::Gc gc;` + `std::mutex gc_round_mutex;`), `.cpp:170-174` (`runOneRoundNow`) and `.cpp:176-244` (`loop`)
- Modify: `.../ContentAddressed/ContentAddressedMetadataStorage.h:316` (add `std::mutex gc_scheduler_mutex;`), `.cpp:203,317` (guard the lazy `gc_scheduler` creation)
- Test: `src/Disks/tests/gtest_cas_gc_log.cpp` (Modify — already includes `CasGcScheduler.h`, `InMemoryBackend`, `Store`)

**Interfaces:**
- Consumes: `Cas::Gc(store, gc_id)`, `Gc::runRegularRound` (observation members `has_observation`/`last_seen_owner`/`last_seen_seq`), `CasGcScheduler::runOneRoundNow`, `RoundReport::acquired_lease`
- Produces: none (internal fix — one persistent `Gc` shared by `loop()` and `runOneRoundNow`, serialized under a new `gc_round_mutex`; a new `gc_scheduler_mutex` guards lazy creation)

Note (deviation): the spec says "serialize under the scheduler `mutex`". The real `loop()` deliberately *releases* `mutex` before the round precisely so `stop()` and `heartbeatLoop()` (both take `mutex` briefly) are not blocked; holding it across a long fold would freeze the B160 heartbeat and stall shutdown. So the fix adds a dedicated `gc_round_mutex` that serializes only the two round callers. The manual-vs-loop *serialization* race needs a real second thread (untestable without `sleep`); the deterministic **dead-incumbent steal across calls** is the primary regression test.

- [ ] **Step 1: Write the failing test**
```cpp
/// A7/U#7: the manual `SYSTEM ... GC` path (runOneRoundNow) must reuse ONE stable Gc instance across
/// calls — the lease's observation-window steal protocol compares consecutive observations of the
/// SAME observer. A throwaway Gc per call resets the observation every time, so a dead incumbent's
/// lease could never be recovered. Deterministic: "time" is the order of runRegularRound calls; no
/// sleep, no clock, no threads.
TEST(CasGcSchedulerSteal, ManualRoundReusesAStableObserverAndStealsDeadIncumbent)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    /// A foreign incumbent takes the lease and then DIES (never renews, never heartbeats).
    const UInt128 kIncumbent = hexToU128("00000000000000000000000000000abc");
    Gc incumbent(store, kIncumbent);
    ASSERT_TRUE(incumbent.runRegularRound().acquired_lease);

    DB::ContentAddressed::CasGcScheduler sched(store, std::chrono::seconds(1), "test::gc", "ca");

    /// obs #1: records the incumbent's (owner, seq, hb=absent); not yet steal-eligible.
    EXPECT_FALSE(sched.runOneRoundNow(Rec::Trigger::Manual).acquired_lease);
    /// obs #2: the same frozen (owner, seq, hb) observed twice => steal-eligible => the manual round
    /// recovers the lease. With a throwaway Gc per call this stays false forever.
    EXPECT_TRUE(sched.runOneRoundNow(Rec::Trigger::Manual).acquired_lease);
}
```

- [ ] **Step 2: Run test, verify it fails**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CasGcSchedulerSteal.ManualRoundReusesAStableObserverAndStealsDeadIncumbent'`
Expected: FAIL — the second `runOneRoundNow` returns `acquired_lease == false` (each call constructs a fresh `Cas::Gc gc(store, gc_id)` that has `has_observation == false`, so every call is "obs #1" and never reaches the steal condition).

- [ ] **Step 3: Implement the minimal fix**
`CasGcScheduler.h` — add members (declare `gc` after `gc_id` so its init sees `store` and `gc_id`; `gc_round_mutex` beside `mutex`):
```cpp
    const GcRoundLogger logger;

    /// One persistent Gc for BOTH loop() and runOneRoundNow: the lease's observation-window steal
    /// protocol REQUIRES a stable observer (it compares the lease across consecutive runRegularRound
    /// calls of the same instance). A throwaway per call could never recover a dead-incumbent lease.
    Cas::Gc gc;
    /// Serializes the manual round against the background round so the two never touch the single
    /// (not-thread-safe) `gc` concurrently. Distinct from `mutex`: the loop releases `mutex` before
    /// the round so stop()/heartbeatLoop are not blocked, so the round cannot hold `mutex`.
    std::mutex gc_round_mutex;
```
`CasGcScheduler.cpp` — construct `gc` in the ctor init list after `, logger(std::move(logger_))`:
```cpp
    , logger(std::move(logger_))
    , gc(store, gc_id)
```
Replace `runOneRoundNow` (lines 170-174):
```cpp
Cas::RoundReport CasGcScheduler::runOneRoundNow(GcRoundLogRecord::Trigger trigger)
{
    std::lock_guard round_lock(gc_round_mutex);
    return runRoundLogged(gc, trigger);
}
```
In `loop()` delete the local `Cas::Gc gc(store, gc_id);` and its comment (lines 178-181), and take `gc_round_mutex` for the round attempt — add it as the first statement inside the `try` (line 191):
```cpp
        try
        {
            std::lock_guard round_lock(gc_round_mutex);
```
(the body already refers to `gc`, which now resolves to the member).
`ContentAddressedMetadataStorage.h` — add beside `gc_scheduler` (line 316):
```cpp
    std::mutex gc_scheduler_mutex;
```
`ContentAddressedMetadataStorage.cpp` — guard both lazy-creation sites (`runOneGcRoundForTest` ~203, `runGarbageCollectionRoundNow` ~316); the round itself runs OUTSIDE the lock so a full round never serializes command dispatch or blocks shutdown:
```cpp
    ContentAddressed::CasGcScheduler * sched;
    {
        std::lock_guard lock(gc_scheduler_mutex);
        if (!gc_scheduler)
            gc_scheduler = std::make_unique<ContentAddressed::CasGcScheduler>(
                store(), gc_interval, fmt::format("{}::ContentAddressedGC", storage_path_full),
                disk_name, makeGcRoundLogger());
        sched = gc_scheduler.get();
    }
    return sched->runOneRoundNow(ContentAddressed::GcRoundLogRecord::Trigger::Manual);
```
(the `runOneGcRoundForTest` variant returns `void` and passes the default `Trigger::Manual`.)

- [ ] **Step 4: Run test, verify it passes**
Run: `./src/unit_tests_dbms --gtest_filter='CasGcSchedulerSteal.*:CasGcLog.*:CasGcLease.*'`
Expected: PASS (the new steal test plus the existing `CasGcLog` scheduler tests and `CasGcLease` protocol tests — no deadlock; `gc_round_mutex` is uncontended in the single-threaded suites).

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/tests/gtest_cas_gc_log.cpp
git commit -m "cas: manual SYSTEM ... GC reuses a stable Gc and serializes with the loop

runOneRoundNow constructed a fresh Cas::Gc per call, discarding the stable
observer state the lease-steal protocol needs, so the command could never
recover a dead-incumbent lease; it also took no lock against the live loop()
thread. Share one persistent Gc between loop() and runOneRoundNow under a
dedicated gc_round_mutex, and guard the lazy gc_scheduler creation with a mutex.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task A8: Range-check enums decoded in `CasGenerationSeal`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.cpp:119-141` (`decodeFoldSeal` — the `TokenType` and `RefNsCleanupState` casts)
- Test: `src/Disks/tests/gtest_cas_generation_seal.cpp` (Modify)

**Interfaces:**
- Consumes: `decodeFoldSeal`/`encodeFoldSeal`, `TokenType` (`ETag=1`,`Generation=2`,`Emulated=3`), `RefNsCleanupState` (`Pending=1`,`Completed=2`)
- Produces: none (internal fix — mirrors the `RefOwnerKind` range-check in `CasRefLogCodec.cpp:77-80`)

Note (Phase 3 coordination): Task D1d removes `ShardCoverage::folded_cursor`/`::incarnation`. If D1d has already landed, drop those two initializers from the `ShardCoverage{...}` below; if A8 lands first, keep them and D1d will remove them.

- [ ] **Step 1: Write the failing test**
```cpp
namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
}

TEST(CasFoldSeal, RejectsOutOfRangeFoldedTokenType)
{
    CasFoldSeal in = sampleFoldSeal();
    /// Wire an out-of-range folded token type (valid enumerators are 1..3). encodeFoldSeal only
    /// static_casts the enum to a uint32, so the bad value reaches the wire; decode must reject it.
    in.per_ns_shard["ns1/0"].folded_token = Token{"tok-a", static_cast<TokenType>(99)};
    try
    {
        decodeFoldSeal(encodeFoldSeal(in));
        FAIL() << "expected CORRUPTED_DATA for an out-of-range folded token type";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}

TEST(CasFoldSeal, RejectsOutOfRangeNsCleanupState)
{
    CasFoldSeal in = sampleFoldSeal();
    RefNsCleanupItem item;
    item.ns = RootNamespace{"ns9"};
    item.remove_txn_id = RefTxnId{4, 5};
    item.state = static_cast<RefNsCleanupState>(7);   /// valid: 1 (Pending), 2 (Completed)
    in.ns_cleanup_items["k"] = item;
    try
    {
        decodeFoldSeal(encodeFoldSeal(in));
        FAIL() << "expected CORRUPTED_DATA for an out-of-range ns-cleanup state";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}
```

- [ ] **Step 2: Run test, verify it fails**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CasFoldSeal.RejectsOutOfRange*'`
Expected: FAIL — on HEAD `decodeFoldSeal` `static_cast`s the raw wire value straight into the enum with no validation, so no exception is thrown and both `FAIL()` fire.

- [ ] **Step 3: Implement the minimal fix**
In `decodeFoldSeal`, convert the `per_ns_shard` loop body to a block that range-checks the token type before the cast (lines 119-125):
```cpp
    for (const auto & e : msg.per_ns_shard())
    {
        const uint32_t token_type_raw = e.folded_token_type();
        if (token_type_raw != static_cast<uint32_t>(TokenType::ETag)
            && token_type_raw != static_cast<uint32_t>(TokenType::Generation)
            && token_type_raw != static_cast<uint32_t>(TokenType::Emulated))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS fold seal: unknown folded token type {}", token_type_raw);
        seal.per_ns_shard[e.key()] = ShardCoverage{
            .classification = static_cast<uint8_t>(e.classification()),
            .folded_token = Token{e.folded_token_value(), static_cast<TokenType>(token_type_raw)},
            .folded_cursor = e.folded_cursor(),
            .incarnation = ShardIncarnation{e.incarnation_writer_epoch(), e.incarnation_build_sequence()},
            .last_folded_ref_id = RefTxnId{e.last_folded_ref_epoch(), e.last_folded_ref_sequence()}};
    }
```
And in the `ns_cleanup_items` loop, range-check the state before constructing the item (lines 133-141):
```cpp
    for (const auto & e : msg.ns_cleanup_items())
    {
        const uint32_t state_raw = e.state();
        if (state_raw != static_cast<uint32_t>(RefNsCleanupState::Pending)
            && state_raw != static_cast<uint32_t>(RefNsCleanupState::Completed))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS fold seal: unknown ref-namespace cleanup state {}", state_raw);
        const RefTxnId remove_txn_id{e.remove_txn_epoch(), e.remove_txn_sequence()};
        const String key = e.ns() + "\n" + renderRefTxnId(remove_txn_id);
        seal.ns_cleanup_items[key] = RefNsCleanupItem{
            .ns = RootNamespace{e.ns()},
            .remove_txn_id = remove_txn_id,
            .state = static_cast<RefNsCleanupState>(state_raw)};
    }
```

- [ ] **Step 4: Run test, verify it passes**
Run: `./src/unit_tests_dbms --gtest_filter='CasFoldSeal.*:CasGenerationSeal.*'`
Expected: PASS (the two corruption tests plus the existing round-trip/determinism tests — valid enumerators still decode unchanged).

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.cpp src/Disks/tests/gtest_cas_generation_seal.cpp
git commit -m "cas: range-check enums decoded in the fold seal

decodeFoldSeal static_cast RefNsCleanupState and TokenType straight from the
wire with no validation, unlike every sibling codec (RefOwnerKind range-checks
before the cast). RefNsCleanupState feeds GC decision logic. Add the explicit
range check, throwing CORRUPTED_DATA on an out-of-range value.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task A9: `dropRefBestEffort` logs and counts a swallowed rollback failure

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.cpp:283-297` (`dropRefBestEffort`)
- Modify: `src/Common/ProfileEvents.cpp` (CA block, ~line 762 — append one counter, Ring 1)
- Test: `src/Disks/tests/gtest_cas_part_folder_access.cpp` (Modify)

**Interfaces:**
- Consumes: `Store::dropRef`, `tryLogCurrentException` (pattern: `tryLogCurrentException(getLogger("…"), msg)` as in `CasGc.cpp:185`), `ProfileEvents::increment`, `PartRefKey` (`.ns.string()`, `.ref`)
- Produces: new `ProfileEvents::CasRefRollbackBestEffortDropFailed` (append-only, Ring 1)

- [ ] **Step 1: Write the failing test**
```cpp
#include <Common/ProfileEvents.h>

namespace ProfileEvents
{
extern const Event CasRefRollbackBestEffortDropFailed;
}

namespace
{
/// Every mutating backend op throws once armed — models a correlated backend outage during the
/// transaction's compensating rollback (dropRef must append a removal, which mutates the backend).
class RollbackFaultBackend final : public DB::Cas::InMemoryBackend
{
public:
    std::atomic<bool> armed{false};
    DB::Cas::PutResult putIfAbsent(const String & k, const String & b, const DB::Cas::ObjectMeta & m = {}) override { failIfArmed(); return InMemoryBackend::putIfAbsent(k, b, m); }
    DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & m = {}) override { failIfArmed(); return InMemoryBackend::putIfAbsentStream(k, m); }
    DB::Cas::PutResult putOverwrite(const String & k, const String & b, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & m = {}) override { failIfArmed(); return InMemoryBackend::putOverwrite(k, b, e, m); }
    DB::Cas::CasResult casPut(const String & k, const String & b, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & m = {}) override { failIfArmed(); return InMemoryBackend::casPut(k, b, e, m); }
    DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & t) override { failIfArmed(); return InMemoryBackend::deleteExact(k, t); }
private:
    void failIfArmed()
    {
        if (armed.load())
            throw DB::Exception(DB::ErrorCodes::ABORTED, "injected backend outage");
    }
};
}

TEST(CasPartFolderAccess, BestEffortRollbackDropCountsAndSurvivesABackendOutage)
{
    auto backend = std::make_shared<RollbackFaultBackend>();
    auto store = openStoreForTest(backend);
    ContentAddressed::CachedPartFolderAccess access(store, cacheOn());

    const Cas::RootNamespace ns_a{"srv/ta"};
    const Cas::RootNamespace ns_b{"srv/tb"};
    publishPart(store, ns_a, "part_a", {inlineEntry("checksums.txt", "cs")});
    publishPart(store, ns_b, "part_b", {inlineEntry("checksums.txt", "cs")});

    backend->armed = true;
    /// Sanity: with the backend armed, a real dropRef propagates (so the fault reaches the catch).
    EXPECT_ANY_THROW(store->dropRef(ns_a, "part_a"));

    using ProfileEvents::global_counters;
    const auto before = global_counters[ProfileEvents::CasRefRollbackBestEffortDropFailed].load();
    /// The compensating-rollback path must NOT throw (noexcept) and MUST record the swallowed failure.
    access.dropRefBestEffort(ContentAddressed::PartRefKey{ns_b, "part_b"});
    const auto after = global_counters[ProfileEvents::CasRefRollbackBestEffortDropFailed].load();
    EXPECT_EQ(after, before + 1);

    backend->armed = false;   /// let store teardown release its lease cleanly
}
```

- [ ] **Step 2: Run test, verify it fails**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CasPartFolderAccess.BestEffortRollbackDropCountsAndSurvivesABackendOutage'`
Expected: FAIL — compile error `no member named 'CasRefRollbackBestEffortDropFailed'` (the event does not exist). After Step 3 adds the event but before wiring the increment, it fails at runtime (`after == before`).

- [ ] **Step 3: Implement the minimal fix**
`ProfileEvents.cpp` — append to the CA block after `CasRefSweepDeferred` (line 762):
```cpp
    M(CasRefRollbackBestEffortDropFailed, "CA transaction-rollback best-effort dropRef swallowed a backend failure: the ref may stay durably live (GC reclaims only unreferenced objects). Previously silent (A9)", ValueType::Number) \
```
`CachedPartFolderAccess.cpp` — add `#include <Common/logger_useful.h>`, declare `namespace ProfileEvents { extern const Event CasRefRollbackBestEffortDropFailed; }`, and replace the empty catch in `dropRefBestEffort` (lines 289-292):
```cpp
    catch (...)
    {
        /// Best-effort destructor/rollback cleanup: debris is GC-reclaimed, never a masked throw. But
        /// NEVER silent — unlike every other swallow in the feature this had no log trail, so a
        /// correlated backend outage during rollback could leave a permanently-live phantom ref with
        /// no diagnostic (A9). Log + count it as a countable anomaly.
        ProfileEvents::increment(ProfileEvents::CasRefRollbackBestEffortDropFailed);
        tryLogCurrentException(getLogger("CachedPartFolderAccess"),
            fmt::format("CA best-effort rollback dropRef failed (ns={} ref={}); the ref may remain live",
                        key.ns.string(), key.ref));
    }
```

- [ ] **Step 4: Run test, verify it passes**
Run: `./src/unit_tests_dbms --gtest_filter='CasPartFolderAccess.*'`
Expected: PASS (the new test plus the existing part-folder-access suite — the added catch preserves `noexcept` and still `eraseView`s).

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.cpp src/Common/ProfileEvents.cpp src/Disks/tests/gtest_cas_part_folder_access.cpp
git commit -m "cas: log and count a swallowed best-effort rollback dropRef failure

dropRefBestEffort swallowed every exception with a bare empty catch and no log
trail, so a correlated backend outage during a partial-commit rollback could
leave a permanently-live phantom ref (GC reclaims only unreferenced objects)
with no diagnostic. Add tryLogCurrentException on the swallowed path and a
countable CasRefRollbackBestEffortDropFailed ProfileEvent.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task A10: Compute `suppress_destructive` once and thread it through `FoldResult`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h:198-209` (`FoldResult`)
- Modify: `.../Core/CasGc.cpp:1225` (compute once inside `fold`, store on the result) and `.cpp:636` (the round reads the threaded value)
- Test: `src/Disks/tests/gtest_cas_gc_fold.cpp` (Modify)

**Interfaces:**
- Consumes: `Gc::fold`, `Gc::FoldResult`, `RoundReport::anomalies`, `RoundReport::hasAnomaly`
- Produces: new field `bool FoldResult::suppress_destructive = false;`

Note (deviation from failing-first): A10 is a **behavior-preserving dedup**, so there is no red-first state — verified today that `report.anomalies` is not mutated between `fold`'s computation (`.cpp:1225`, after all `recordAnomaly` calls at 772/960/973/1032) and the round's recompute (`.cpp:636`), so the two are already equal. Per the spec ("existing `gtest_cas_gc_*` stay green") the gate is the existing GC suites plus one guard test pinning the cross-cutting invariant *one anomaly ⇒ every destructive action in the round is suppressed* — the property a future edit could silently break by desyncing the two recomputes. Steps 2/4 assert GREEN, not RED.

- [ ] **Step 1: Write the guard test**
```cpp
/// A10: a single clamp anomaly must suppress ALL destructive actions in the round — the merge-side
/// deletes AND the post-CAS ref/namespace cleanup — from ONE decision, not two independent recomputes
/// of !report.anomalies.empty() that a future edit could desync (over-delete class). This pins that a
/// clamped round reclaims nothing.
TEST(CasGcFold, SingleAnomalySuppressesEveryDestructiveActionInTheRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef a = ref("srv-a:1", 1, 0xAA);
    const ManifestRef b = ref("srv-a:2", 2, 0xBB);

    /// Round 0: commit A (references blob 1); its body folds a +1.
    writeManifestRaw(*backend, store->layout(), ns, a, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "r1", std::nullopt, a);
    Gc gc(store, kGc);
    gc.runRegularRound();
    ASSERT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);

    /// One log: drop committed A (`-1`, body present) then add precommit B whose body is absent -> the
    /// missing B body clamps the log AFTER A's `-1` folded.
    writeManifestRaw(*backend, store->layout(), ns, b, {blobEntryFor("b", DB::UInt128(2))});
    deleteManifestBody(*backend, store->layout(), ManifestId{ns, b});
    appendRefLogSeed(*backend, store->layout(), ns,
        {ownerTransitionOp(RefOwnerBinding{RefOwnerKind::Committed, "r1", a}, std::nullopt),
         ownerTransitionOp(std::nullopt, RefOwnerBinding{RefOwnerKind::Precommit, "r2", b})});

    const RoundReport rep = gc.runRegularRound();
    ASSERT_TRUE(rep.hasAnomaly(ns, /*shard*/0)) << "the missing B body must clamp this round";
    /// The clamp suppresses the WHOLE destructive pipeline this round: no deletes, no redeletes, and
    /// A's `-1` stays unadopted (its body must survive, else the re-fold clamps on it forever).
    EXPECT_EQ(rep.deleted, 0u);
    EXPECT_EQ(rep.redeleted, 0u);
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, a})).exists);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
}
```

- [ ] **Step 2: Run test, verify baseline GREEN**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CasGcFold.SingleAnomalySuppressesEveryDestructiveActionInTheRound'`
Expected: PASS on HEAD — the invariant already holds (the two recomputes agree today). This establishes the guard baseline the refactor must preserve.

- [ ] **Step 3: Implement the dedup**
`CasGc.h` — add to `FoldResult` (after `ref_tables`):
```cpp
        /// The single suppress-deletes decision for this round (any clamp / ref-folding abort). Computed
        /// once in fold() and threaded here so the merge-side reducers and the post-CAS ref/namespace
        /// cleanup share ONE value — they can never desync (A10/A§9#7).
        bool suppress_destructive = false;
```
`CasGc.cpp` — in `fold`, at line 1225, store the decision on the result and keep the local alias for the existing reducer calls:
```cpp
    result.suppress_destructive = !report.anomalies.empty();
    const bool suppress_destructive = result.suppress_destructive;
```
`CasGc.cpp` — in the round, at line 636, read the threaded value instead of recomputing:
```cpp
    const bool suppress_destructive = folded.suppress_destructive;
```

- [ ] **Step 4: Run test, verify still GREEN (behavior preserved)**
Run: `./src/unit_tests_dbms --gtest_filter='CasGcFold.*:CasGcAckFloor.*:CasGcLease.*:CasGcResume.*'`
Expected: PASS — the guard test and the full GC suites are unchanged (the value threaded from `fold` equals what the round recomputed).

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp src/Disks/tests/gtest_cas_gc_fold.cpp
git commit -m "cas: compute suppress_destructive once and thread it through FoldResult

The clamp-suppression decision (!report.anomalies.empty()) was recomputed
independently in fold() and in the round's post-CAS cleanup. They agree today,
but a future edit could make fold suppress deletes while the round does not (an
over-delete class). Compute it once in fold(), store it on FoldResult, and have
the round read the threaded value — one source of truth.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Phase 3 — Remove all old stuff

> Do Phase 3 after Phase 2 (removals before simplification, so refactors don't preserve dead paths). **Cross-task coordination:** (1) `CasInstrumentedBackend.cpp:125-133` classifier + `gtest_cas_backend.cpp:304-305` are edited once, in **D3** (owns both the `_watermark` and `_precommits` branches); D1b is scoped to `CasLayout.h` + `CasInstrumentedBackend.h` only. (2) `gtest_cas_dangling_precommit.cpp:27` (`cov.folded_cursor = 7;`) is touched by both D1d (field removal) and D5 (file rename) — run **D1d before D5** or fold the line removal into the D5 rename. Spec items intentionally **not executed**: `ObjectKind` removal (D1f — deferred, not cleanly removable: ~100 consumers) and `CasEvent::round` removal (D2 — premise refuted; it is a live serialized `system.content_addressed_log.round` column).

### Task D1: Remove vestigial format constants and state fields

Owner-approved removals of dead pool-meta / gc-state / fold-seal fields. Pre-release, no-persisted-data formats, so touching the on-disk codecs is safe with **zero compat scaffolding** — delete the proto fields and add `reserved` markers (the convention already in `cas_format.proto`, e.g. `reserved 11; reserved "retired_refs";`). Each sub-removal is its own commit.

> ⚠️ **Naming-collision guard.** `git grep root_shards` returns a *second, live* field `FoldResult::root_shards` (`CasGc.h:201`, written `CasGc.cpp:778`, read `CasGc.cpp:337`) — a `std::vector<std::pair<RootNamespace,uint64_t>>`. **Do NOT touch it.** Only the `PoolConfig`/`PoolMeta` scalar `root_shards` is removed. Likewise `n_precommits` (`CasRefSnapshotCodec.cpp`) and `live_precommits` (GC report) are unrelated — do not touch.

**Format gtests guarding the touched codecs:** `gtest_cas_store.cpp` (PoolMeta round-trip), `gtest_cas_gc_formats.cpp` (GcState round-trip), `gtest_cas_generation_seal.cpp` (FoldSeal round-trip).

**Sub-item D1a — `root_shards` (PoolConfig / PoolMeta / factory / proto)**

- [ ] **Step 1: Confirm zero live consumers**
```
git grep -n 'root_shards' HEAD -- src programs
```
Expected: every hit is store/validate/echo/plumb, never a placement/branch decision. Production: `CasStore.h:120`, `CasPoolMeta.h:25,49` + `.cpp:34-37,92,123,129,233,237,258`, `CasStore.cpp:239`, `ContentAddressedMetadataStorage.cpp:132,162,410-412` + `.h:68,273`, `MetadataStorageFactory.cpp:248,251,310`, `cas_format.proto:54`. The only `% root_shards` arithmetic is the test helper `cas_test_helpers.h:561 shardOfForTest`; the production `Store::shardOf` that used it is already deleted (D3).

- [ ] **Step 2: Remove the field and its plumbing**
  - `CasStore.h:120`: delete `uint64_t root_shards = 32;` (+ the tuning comment).
  - `CasPoolMeta.h`: delete the field (`:25`); drop `root_shards` from `createOrValidate` (`:49`) and the JSON-shape/authority comments.
  - `CasPoolMeta.cpp`: `validatePoolConstants` → drop the `root_shards` param + check (`:34-37`); delete `msg.set_root_shards(...)` (`:92`), `pm.root_shards = msg.root_shards()` (`:123`), the args at `:129,237`, the `createOrValidate` param (`:233`), `pm.root_shards = root_shards` (`:258`). After this `validatePoolConstants` validates only `blob_header_len` — rename to `validateBlobHeaderLen`.
  - `CasStore.cpp:239`: drop the `config.root_shards,` argument.
  - `ContentAddressedMetadataStorage.cpp`: delete ctor param (`:132`), init (`:162`), the creation-time-only `pool_config.root_shards = root_shards;` block (`:410-412`).
  - `ContentAddressedMetadataStorage.h`: delete ctor default (`:68`) + member (`:273`).
  - `MetadataStorageFactory.cpp`: delete the config read + comment (`:248-251`) and the ctor argument (`:310`).
  - `cas_format.proto`: delete `uint64 root_shards = 3;` (`:54`); add `reserved 3; reserved "root_shards";` in `PoolMetaProto`.
  - Tests: delete `gtest_ca_wiring.cpp:1337-1352` (root_shards plumbing test); the validation cases in `gtest_cas_gc_formats.cpp:415-446` and `gtest_cas_store.cpp:191-195,288-295`; `shardOfForTest` (`cas_test_helpers.h:558-563`, `gtest_cas_store.cpp:35`) and its call sites; and every `.root_shards = N`/`/*root_shards*/ N` initializer across `cas_test_helpers.h`, `gtest_ca_wiring.cpp`, `gtest_cas_b140_dangle.cpp`, `gtest_cas_blob_digest.cpp`, `gtest_cas_build.cpp`, `gtest_cas_build_root_dangle.cpp`, `gtest_cas_gc_formats.cpp`, `gtest_cas_gc_leak.cpp`, `gtest_cas_gc_round.cpp`, `gtest_cas_gc_round_defer.cpp`, `gtest_cas_gc_shard_plan.cpp`, `gtest_cas_mount.cpp`, `gtest_cas_pluggable_hash.cpp`, `gtest_cas_ref_gc.cpp`, `gtest_cas_s3_staging.cpp`, `gtest_cas_store.cpp` (~40 sites — the largest churn in D1).

- [ ] **Step 3: Build + run the guarding tests**
```
cd build && ninja unit_tests_dbms 2>&1 | tee build/ninja_d1a.log
./src/unit_tests_dbms --gtest_filter='CasStore*:CasGcFormats*:CaWiring*:*PoolMeta*' 2>&1 | tee build/test_d1a.log
```
Expected: PASS. Analyze both logs via a subagent per the build/test-logging rule.

- [ ] **Step 4: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages src/Disks/tests
git commit -m "cas: remove vestigial PoolMeta.root_shards (dead since Store::shardOf removal)

Pre-release format: PoolMetaProto field 3 reserved, no persisted data, no compat shim.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

**Sub-item D1b — `_precommits` modeling + `isPrecommitNamespace`**

- [ ] **Step 1: Confirm zero live consumers**
```
git grep -n 'isPrecommitNamespace\|_precommits' HEAD -- src programs
```
Expected: `isPrecommitNamespace` defined `CasLayout.h:468`, called only self-referentially `:507`. No production code builds a `/_precommits/` key (`CasBuild.h:96`: "there is no `_precommits` namespace"). The `CasInstrumentedBackend.cpp:131` branch + `gtest_cas_backend.cpp:305` are handled by **D3**; `n_precommits`/`live_precommits` are unrelated.

- [ ] **Step 2: Remove the modeling**
  - `CasLayout.h`: delete `isPrecommitNamespace` + doc (`:462-470`), and the reserved-segment block in `checkNamespace` (`:504-509`).
  - `CasInstrumentedBackend.h:22`: delete the `_precommits` doc line.

- [ ] **Step 3: Build + run**
```
cd build && ninja unit_tests_dbms 2>&1 | tee build/ninja_d1b.log
./src/unit_tests_dbms --gtest_filter='CasLayout*:CaWiring*' 2>&1 | tee build/test_d1b.log
```
Expected: PASS (namespace validation still rejects `_files`/`_manifests`).

- [ ] **Step 4: Commit** — `cas: remove dead _precommits namespace modeling + isPrecommitNamespace`

**Sub-item D1c — `fence_seq` (GcState)** *(format-touch: GcStateProto codec)*

> At HEAD `fence_seq` is write-only vestigial state (serialized, incremented on lease-steal, inspected, asserted in tests — but no production code compares/branches on it; outcome/retire keys moved to `(snap_generation, snap_attempt)`).

- [ ] **Step 1: Confirm no production consumer branches on it**
```
git grep -n 'fence_seq' HEAD -- src programs
```
Expected non-branching production sites: `CasGcFormats.h:44`, `CasGcFormats.cpp:37,73`, `CasGc.cpp:2393` (`++next.fence_seq;`), `CasInspect.cpp:294`, `cas_format.proto:122`.

- [ ] **Step 2: Remove the field**
  - `CasGcFormats.h:25-27`: delete `uint64_t fence_seq = 0;` from `struct GcState`.
  - `CasGcFormats.cpp`: delete serialize (`:37`) + deserialize (`:73`).
  - `CasGc.cpp:2393`: delete `++next.fence_seq;` (the `next.lease.owner = gc_id; ++next.lease.seq;` steal is unaffected).
  - `CasInspect.cpp:294`: delete the `fence_seq` JSON line.
  - `cas_format.proto:122`: delete `uint64 fence_seq = 3;`; add `reserved 3; reserved "fence_seq";` in `GcStateProto`.
  - Tests: drop round-trip lines `gtest_cas_gc_formats.cpp:29,36,377`; the assertions `gtest_cas_gc_round.cpp:188,221,860` (the lease `owner`/`seq` assertions stay; consider renaming `StealAfterObservedNonRenewalBumpsEpoch`); drop `fence_seq` params from `encodeMinimalGcState`/`injectRetire` (`cas_test_helpers.h:323-349`) and their `/*fence_seq*/ 0` call-site args in `gtest_cas_build.cpp`, `gtest_cas_gc_shard_incarnation.cpp`, `gtest_cas_protocol_scenarios.cpp`.

- [ ] **Step 3: Build + run the guarding format gtest**
```
cd build && ninja unit_tests_dbms 2>&1 | tee build/ninja_d1c.log
./src/unit_tests_dbms --gtest_filter='CasGcFormats*:CasGcLease*:CasGcRound*' 2>&1 | tee build/test_d1c.log
```
Expected: PASS.

- [ ] **Step 4: Commit** — `cas: remove write-only vestigial GcState.fence_seq (GcStateProto field 3 reserved)`

**Sub-item D1d — `ShardCoverage::folded_cursor` + `ShardCoverage::incarnation`** *(format-touch: FoldSealProto codec; coordinate with A8 + D5)*

- [ ] **Step 1: Confirm zero production readers**
```
git grep -n 'folded_cursor' HEAD -- src programs
git grep -n 'incarnation' HEAD -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.cpp
```
Expected: `folded_cursor` production = field (`CasGenerationSeal.h:47`), serialize (`.cpp:72`), deserialize (`.cpp:123`), inspect (`CasInspect.cpp:349`) only. `ShardCoverage::incarnation` production = serialize (`CasGenerationSeal.cpp:73-74`), deserialize (`.cpp:124`), decl (`.h:48`). Keep the `ShardIncarnation` **type** — only the `ShardCoverage` field is removed.

- [ ] **Step 2: Remove both fields**
  - `CasGenerationSeal.h`: delete `folded_cursor` (`:47`) + `incarnation` (`:48`); update the struct doc.
  - `CasGenerationSeal.cpp`: delete `e->set_folded_cursor(...)` (`:72`), the two `set_incarnation_*` (`:73-74`), and the `.folded_cursor`/`.incarnation` initializers in `decodeFoldSeal` (`:123-124`). **If A8 has already landed**, these initializers are inside the range-checked block A8 added — remove them there.
  - `CasInspect.cpp:349`: delete the `folded_cursor` line.
  - `cas_format.proto`: in `FoldShardCoverageProto` delete `folded_cursor = 5`, `incarnation_writer_epoch = 6`, `incarnation_build_sequence = 7` (`:173-175`); add `reserved 5, 6, 7; reserved "folded_cursor", "incarnation_writer_epoch", "incarnation_build_sequence";`.
  - Tests: drop `.folded_cursor`/`.incarnation` from `gtest_cas_generation_seal.cpp:15,16,33,55,70`, `gtest_cas_gc_formats.cpp:175`, and `gtest_cas_dangling_precommit.cpp:27` (coordinate with D5's rename).

- [ ] **Step 3: Build + run the guarding format gtest**
```
cd build && ninja unit_tests_dbms 2>&1 | tee build/ninja_d1d.log
./src/unit_tests_dbms --gtest_filter='CasGenerationSeal*:CasGcFormats*:CasDanglingPrecommit*' 2>&1 | tee build/test_d1d.log
```
Expected: PASS.

- [ ] **Step 4: Commit** — `cas: remove vestigial ShardCoverage.folded_cursor/incarnation (FoldSealProto fields 5/6/7 reserved)`

**Sub-item D1e — seal `classification=3` (Minted) dead value** *(doc-only)*

- [ ] **Step 1: Confirm no producer ever sets 3**
```
git grep -n 'classification = \|classification == 3\|set_classification' HEAD -- src programs
```
Expected: production sets only 0/1/2/4, never 3, and nothing compares `== 3`.

- [ ] **Step 2: Remove the dead value from the two docs**
  - `CasGenerationSeal.h:36-37`: drop the `3 = Minted (fence-only, retired concept)` clause.
  - `cas_format.proto:171`: `0=Absent 1=Unchanged 2=Folded 3=Minted` → `0=Absent 1=Unchanged 2=Folded` (keep `4=Clamped`).

- [ ] **Step 3: Build** (`cd build && ninja unit_tests_dbms 2>&1 | tee build/ninja_d1e.log`)

- [ ] **Step 4: Commit** — `cas: drop dead classification=3 (Minted) from fold-seal docs`

**Sub-item D1f — single-value `ObjectKind` enum → DEFER (not cleanly removable)**

- [ ] **Step 1: Confirm it is NOT cleanly removable**
```
git grep -cn 'ObjectKind' HEAD -- src   # ~100 hits
```
Expected: threaded as the parameter/field type of `observeAndAdmit`, `uploadFromSource`, `decodeEnvelopeHeader`, `magicFor`, `retiredLogicalSize`, `EnvelopeHeader::kind`, `RetiredEntry::kind`, `CasGcOutcomes` codec, `Fsck`/`GcObserved`. `CasEnvelope.h:21-24` documents it as "kept as a switch-friendly type". Removing it is a refactor, not a subtraction — **do not remove.** No code change, no commit.

**Sub-item D1g — `PartFolderView::fileSize` unreachable mutable branch**

- [ ] **Step 1: Confirm the branch is unreachable via the sole caller**
```
git grep -n 'fileSize' HEAD -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed
```
Expected: sole caller `ContentAddressedMetadataStorage.cpp:775`, which the NOTE (`PartFolderView.cpp:63-67`) confirms short-circuits every mutable-named path first.

- [ ] **Step 2: Remove the unreachable branch + NOTE**
In `PartFolderView.cpp:62-73`, drop the NOTE and the mutable branch, leaving:
```cpp
std::optional<uint64_t> PartFolderView::fileSize(const String & path) const
{
    if (const auto * e = findFile(path))
        return e->placement == Cas::EntryPlacement::Inline ? e->inline_bytes.size() : e->blob_size;
    return std::nullopt;
}
```
Update `PartFolderView.h:51` `/// mutable / inline / blob` → `/// inline / blob`.

- [ ] **Step 3: Build + run**
```
cd build && ninja unit_tests_dbms 2>&1 | tee build/ninja_d1g.log
./src/unit_tests_dbms --gtest_filter='*PartFolderView*:CaWiring*' 2>&1 | tee build/test_d1g.log
```
Expected: PASS.

- [ ] **Step 4: Commit** — `cas: drop unreachable mutable branch from PartFolderView::fileSize`

### Task D2: Remove dead counters/events

**[Ring 1]** Zero-increment `ProfileEvents` + one never-emitted `CasEventType`. Append-only lines, upstream-conflict-free. **Excludes `CasEvent::round`** (spec premise refuted — see note below).

- [ ] **Step 1: Confirm zero increment sites (each ProfileEvent)**
```
for n in CasShardBatchedMutations CasShardBatchFlushes CasShardBatchScopeCuts \
  CasShardQueueWaitMicroseconds CasManifestBackpressureCount CasManifestBackpressureMicroseconds \
  CasManifestHardLimitExceeded CasPartFolderViewEvictions \
  ContentAddressedGenerationResurrectionsTotal ContentAddressedDuplicateGenerationBytes \
  ContentAddressedTombstonesTotal ContentAddressedGenerationsObserved \
  ContentAddressedHashesObserved ContentAddressedOrphanBytesEstimate; do
  echo "== $n =="; git grep -n "$n" HEAD -- src programs; done
```
Expected: each name appears ONLY at its `M(...)` declaration in `src/Common/ProfileEvents.cpp` (lines `750-753`, `835-837`, `848`, `879-884`) — zero `increment` sites.

- [ ] **Step 2: Delete the dead code**
  - `src/Common/ProfileEvents.cpp`: delete the 14 `M(...)` lines above.
  - `CasEvent.h:23`: remove `GcSnapPersist,` (confirm `git grep -n 'GcSnapPersist' HEAD` = exactly the enum decl + its `toString` case, no assignment).
  - `CasEvent.cpp:42`: remove `case CasEventType::GcSnapPersist: return "gc_snap_persist";` (the switch is exhaustive — remove with the enumerator).

- [ ] **Step 3: Build + run the event gtest**
```
cd build && ninja unit_tests_dbms clickhouse 2>&1 | tee build/ninja_d2.log
./src/unit_tests_dbms --gtest_filter='CasEventLog*:CasObservability*' 2>&1 | tee build/test_d2.log
```
Expected: PASS.

- [ ] **Step 4: Commit**
```bash
git add src/Common/ProfileEvents.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.cpp
git commit -m "cas: remove 14 zero-increment ProfileEvents + GcSnapPersist event husk (vestigial after shard-queue/pre-rebuild-GC removal)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

> **DO NOT remove `CasEvent::round`.** The spec's "always-0, 5 write sites" premise is refuted at HEAD: `git grep -n 'e.round =' HEAD` shows 20 write sites (`CasGc.cpp:254 e.round = new_round;`, `:817 e.round = condemn_round;`, …); it is copied at `ContentAddressedMetadataStorage.cpp:296` into `ContentAddressedLogElement::round` and surfaced as `system.content_addressed_log.round` (`ContentAddressedLog.cpp:31,57`, `.h:27`). Deleting it is a user-visible schema change, out of scope.

### Task D3: Remove stale comments / anchors / debris

Comment/dead-branch cleanup. One **[Ring 1]** removal (`checkContentAddressedDiskRestrictions`) net-reduces the CA footprint in a shared file.

- [ ] **Step 1: Confirm each target is dead/stale**
```
git grep -n 'shardOf' HEAD -- src programs                     # no live Store::shardOf; only comments + CasBlobDigest::shardOf codec + shardOfForTest
git grep -nE '"_watermark"|/_watermark' HEAD -- src programs   # only the classifier + one test; no key-builder
git grep -n '/_precommits/' HEAD -- src programs               # only the classifier + one test
git grep -n 'candidates += 0' HEAD -- src programs             # single hit CasGc.cpp:563
git grep -n 'src_st.build' HEAD -- src                         # empty if at ContentAddressedTransaction.cpp:1088
git grep -n 'notYet' HEAD -- src                               # 3 call sites, shared stale message
git grep -n 'checkContentAddressedDiskRestrictions' HEAD -- src # decl + def + 1 call site only
git grep -n 'exists()' HEAD -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed  # spec "exists() debris" -> already clean, no hits, no action
```
Confirmed dead/stale at HEAD: `Store::shardOf` comment refs (`CasGcShardPlan.h:23-24`, `CasBuild.h:98,103`, `cas_test_helpers.h:559-560` — NOT the live `CasBlobDigest::shardOf`); `_watermark` reader (`CasInstrumentedBackend.cpp:129`, control slot is now `gc/hb`); gc/outcomes key comment (`CasGcOutcomes.h:14-16,33-34` + `proto:145` — real builder is `gc/gen/<generation>/attempt/<attempt>/outcomes/<round>/<shard>`); dead anchors (`CasGcCursorKey.h:18-21,29-34`); `report.candidates += 0;` (`CasGc.cpp:563`); empty `if (!src_st.build) {}` (`ContentAddressedTransaction.cpp:1088-1090`); stale `notYet()` cache-over-CA clause (`ContentAddressedTransaction.cpp:70-71`); stale `ContentAddressedLog` docs (`ContentAddressedLog.cpp:24,28` + `.h:24`, `SystemLog.h:21` — `manifest_expand`/`strip`/`pack`/`tree` never emitted); `checkContentAddressedDiskRestrictions` (`MergeTreeData.cpp:1251-1259` `(void)metadata;`, decl `.h:1659-1662`, call `.cpp:771`).

- [ ] **Step 2: Remove/repoint each**
  - Reword the `Store::shardOf` comments (`CasGcShardPlan.h:23-24`, `CasBuild.h:98,103`, `cas_test_helpers.h:559-560`) to state root-shard placement plainly.
  - `CasInstrumentedBackend.cpp:125-133`: delete both the `if (key.ends_with("/_watermark"))` and `if (key.find("/_precommits/") != String::npos)` branches and collapse the `:125-128` comment (absorbs D1b's `_precommits` classifier line — single owner). Update `gtest_cas_backend.cpp:304-305` (the two `_watermark`/`_precommits` classification assertions) to the new `Root` classification, or delete them.
  - `CasStore.cpp:263-265`: reword the `_watermark` slot comment to the heartbeat model.
  - `CasGcOutcomes.h:14-16,33-34` + `proto:145`: repoint the key-layout text to `gc/gen/<generation>/attempt/<attempt>/outcomes/<round>/<shard>`.
  - `CasGcCursorKey.h:18-21,29-34`: delete the "legacy inline expression" + dead line-anchor comments (keep the `rfind('/')` rule).
  - `CasGc.cpp:563`: delete `report.candidates += 0;`.
  - `ContentAddressedTransaction.cpp:1088-1090`: delete the empty `if`, promote the `else if` to `if`.
  - `ContentAddressedTransaction.cpp:70-71`: drop the cache-over-CA "not supported yet" clause from `notYet`.
  - `ContentAddressedLog.cpp:24,28` + `.h:24`, `SystemLog.h:21`: remove the never-emitted `manifest_expand`/`manifest_strip`/`pack`/`tree`/`strip` names from the docs.
  - `MergeTreeData.cpp`: delete `checkContentAddressedDiskRestrictions` def (`:1251-1259`) + call + comment (`:768-771`); `MergeTreeData.h`: delete the decl + comment (`:1659-1662`). **[Ring 1 — net-shrinks the shared-file diff.]**

- [ ] **Step 3: Build + run**
```
cd build && ninja unit_tests_dbms clickhouse 2>&1 | tee build/ninja_d3.log
./src/unit_tests_dbms --gtest_filter='CasBackend*:CasGc*:CaWiring*' 2>&1 | tee build/test_d3.log
```
Expected: PASS (the only behavioral edit is the dead classifier branches, whose keys no production path emits; `gtest_cas_backend.cpp` updated to match).

- [ ] **Step 4: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages src/Interpreters/ContentAddressedLog.h src/Interpreters/ContentAddressedLog.cpp src/Interpreters/SystemLog.h src/Storages/MergeTree/MergeTreeData.h src/Storages/MergeTree/MergeTreeData.cpp src/Disks/tests/gtest_cas_backend.cpp
git commit -m "cas: remove stale comments, dead classifier branches, and the no-op checkContentAddressedDiskRestrictions

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task D4: Document the relink trust model (comment-only)

**[Ring 0 + one Ring-1 line]** No code change. Records that CAS fetch-by-relink shares the trust model of ordinary `ReplicatedMergeTree` interserver part fetch (the retracted umbrella "RBAC bypass").

- [ ] **Step 1: Write the comment** at the top of `ContentAddressedMetadataStorage::adoptPartFromManifest` (`:1142`):
```cpp
/// TRUST MODEL: adopting a part from a peer-supplied manifest is exactly as trusted as an ordinary
/// ReplicatedMergeTree interserver part fetch. The interserver HTTP channel — not a per-blob ACL — is
/// the trust boundary: a malicious or MITM peer on that channel can already serve arbitrary part bytes
/// that the receiver adopts, in both the byte-streaming and the relink path. Table-level RBAC never
/// defended against a hostile peer, so relink-by-manifest adds no new trust surface. (See the retracted
/// umbrella "RBAC bypass" finding; feedback_cas_relink_trust_model.)
```
Optionally add a one-line pointer at `DataPartsExchange.cpp:1104` (just above the `adoptPartFromManifest` call): `/// Trust boundary is the interserver channel, as for a normal part fetch — see adoptPartFromManifest.`

- [ ] **Step 2: Build** — `cd build && ninja clickhouse 2>&1 | tee build/ninja_d4.log` (comment-only; no test)

- [ ] **Step 3: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Storages/MergeTree/DataPartsExchange.cpp
git commit -m "cas: document relink trust model == ReplicatedMergeTree interserver fetch (no per-blob ACL)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task D5: Test-side migration cleanup

**[Ring 0, tests]** Drop the legacy shard-key branch from the wiring-test predicates; rename the mis-named `gtest_cas_dangling_precommit.cpp`. **Keep** the two `DISABLED_` tree-model tests (`gtest_cas_protocol_scenarios.cpp:605,614`).

- [ ] **Step 1: Confirm the legacy shard-key shape is dead in production**
```
git grep -n 'refsNamespacePrefix(ns) +' HEAD -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core
git grep -n 'parseRefObjectKey' HEAD -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h
```
Expected: production ref-object keys are only `_log/`, `_snap/`, `_cleanup/` (`CasLayout.h:119-135`); `renderRefTxnId` is `<16hex>-<16hex>` (never all-numeric); `parseRefObjectKey` (`:138-143`) explicitly excludes the bare numeric-shard shape. No writer emits a numeric ref-shard key.

- [ ] **Step 2: Repoint the two predicates**
  - `gtest_ca_wiring.cpp:1467-1478` (`isRefWriteKey`): drop the bare-numeric acceptance + the "held Phase-E legacy shard lane" comment, leaving:
    ```cpp
    bool isRefWriteKey(const std::string & key)
    {
        if (key.find("/cas/refs/") == std::string::npos)
            return false;
        return key.find("/_log/") != std::string::npos || key.find("/_snap/") != std::string::npos;
    }
    ```
  - `gtest_ca_wiring.cpp:1228-1237` (`isShardManifestPath`): repoint to the current per-part durable publish shape (`_log/` under `/cas/refs/`):
    ```cpp
    bool isShardManifestPath(const std::string & path)
    {
        return path.find("/cas/refs/") != std::string::npos && path.find("/_log/") != std::string::npos;
    }
    ```
    **VERIFY empirically before committing:** run `CaWiringWrite.PartialCommitRollsBackPublishedParts` (`:1270`) and confirm the injected fault still fires on the *second* part's durable publish (it asserts all-or-nothing rollback). If the observed publish key differs from `_log/`, match the actual per-part durable write — the B122 test is the gate.
  - `gtest_cas_dangling_precommit.cpp`: now a 40-line `ShardCoverage`/fold-seal codec round-trip. `git mv` to `gtest_cas_fold_seal_codec.cpp`, rename suite `CasDanglingPrecommit` → `CasFoldSealCodec`; drop the obsolete `cov.folded_cursor = 7;` line (coordinate with **D1d**). Confirm glob-registration: `git grep -n 'gtest_cas_dangling_precommit' HEAD -- src/Disks/tests/CMakeLists.txt` returns nothing.
  - Leave `gtest_cas_protocol_scenarios.cpp:605,614` untouched.

- [ ] **Step 3: Build + run the wiring tests**
```
cd build && ninja unit_tests_dbms 2>&1 | tee build/ninja_d5.log
./src/unit_tests_dbms --gtest_filter='CaWiring*:CaWiringWrite*:CasFoldSealCodec*' 2>&1 | tee build/test_d5.log
```
Expected: PASS — `PartialCommitRollsBackPublishedParts` still throws on the 2nd publish; B188 `firstPrecommitWriteIdx` tests still anchor on the first `_log/` write; the renamed codec round-trip passes.

- [ ] **Step 4: Commit**
```bash
git add src/Disks/tests/gtest_ca_wiring.cpp
git mv src/Disks/tests/gtest_cas_dangling_precommit.cpp src/Disks/tests/gtest_cas_fold_seal_codec.cpp
git add src/Disks/tests/gtest_cas_fold_seal_codec.cpp
git commit -m "cas: drop legacy numeric ref-shard branch from wiring-test predicates; rename dangling-precommit gtest to its fold-seal-codec content

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Phase 4 — Simplification / dedup (incl. two Ring-2-shrink items)

### Task C1: Consolidate the token policy into `tokenForHead`/`tokenForList`/`tokenMatches`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.h:106-155` (add three helpers next to `nativeTokenType`/`supportsListTokens`)
- Modify: `.../Core/CasObjectStorageBackend.cpp` (mint sites: `nativeHead` :177, `nativeConditionalPut` :272-273, `NativeStreamingSink::finalize` :315-316, `list` native :959-960, `list` emulated :918-919; compare sites: `putOverwrite` :744, `casPut` :783, `deleteExact` :824)
- Test: `src/Disks/tests/gtest_cas_backend_generation.cpp` (Modify — it already builds a Native-mode `ObjectStorageBackend` over `LocalObjectStorage` and drives `setNativeTokenTypeForTest`)

**Interfaces:**
- Consumes: `native_token_type` (`TokenType`), `supportsListTokens() const`, `Token{value, TokenType}`, `Token::operator==` (defaulted), `ObjectMetadata::etag`.
- Produces (public const methods so the anon-namespace `NativeStreamingSink` can call them):
  - `Token tokenForHead(const String & etag) const;` → `Token{etag, native_token_type}`
  - `std::optional<Token> tokenForList(const String & etag) const;` → `nullopt` when `!supportsListTokens() || etag.empty()`, else `Token{etag, native_token_type}`
  - `static bool tokenMatches(const Token & observed, const Token & expected);` → `observed == expected`

- [ ] **Step 1: Write the characterization test**
```cpp
/// C1: the three token-policy helpers are the single source of truth for how a Native-mode backend
/// mints a HEAD/PUT token, gates a LIST token, and compares tokens. Characterizes the behavior the
/// scattered call sites have today so the consolidation stays byte-for-byte behavior-preserving.
TEST(CasBackendGeneration, TokenPolicyHelpersAreConsistentWithDialect)
{
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native);

    /// ETag dialect: head/put tokens carry ETag; list surfaces the same-typed token for a non-empty etag.
    ASSERT_EQ(b->nativeTokenType(), TokenType::ETag);
    EXPECT_EQ(b->tokenForHead("abc").type, TokenType::ETag);
    EXPECT_EQ(b->tokenForHead("abc"), (Token{"abc", TokenType::ETag}));
    ASSERT_TRUE(b->tokenForList("abc").has_value());
    EXPECT_EQ(*b->tokenForList("abc"), b->tokenForHead("abc"));   /// list token == head token (same etag)
    EXPECT_FALSE(b->tokenForList("").has_value());                /// empty etag => no list token

    /// Generation dialect (GCS): head token flips to Generation; list tokens are disabled wholesale
    /// (poisoned If-Match), so tokenForList is always nullopt regardless of the etag.
    b->setNativeTokenTypeForTest(TokenType::Generation);
    EXPECT_EQ(b->tokenForHead("g1").type, TokenType::Generation);
    EXPECT_FALSE(b->tokenForList("g1").has_value());

    /// tokenMatches is exact identity (value AND type) — a same-value/different-type token never matches.
    EXPECT_TRUE(ObjectStorageBackend::tokenMatches(Token{"x", TokenType::ETag}, Token{"x", TokenType::ETag}));
    EXPECT_FALSE(ObjectStorageBackend::tokenMatches(Token{"x", TokenType::ETag}, Token{"x", TokenType::Emulated}));
}
```

- [ ] **Step 2: Run test, verify current state**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CasBackendGeneration.TokenPolicyHelpersAreConsistentWithDialect'`
Expected: FAIL to compile first (helpers not declared) — the failing-first gate; PASS once Step 3 adds the helpers, and stays PASS after the call sites migrate (characterization of unchanged behavior).

- [ ] **Step 3: Implement** — add the three helpers, then migrate every mint/compare site.

Header (`CasObjectStorageBackend.h`, public section near `nativeTokenType`):
```cpp
    /// ---- Token policy (single source of truth; see the .cpp) ----
    /// Mint the incarnation token for a key we just HEAD'd or wrote: the object ETag/generation
    /// string carried under this backend's native dialect (native_token_type).
    Token tokenForHead(const String & etag) const
    {
        return Token{etag, native_token_type};
    }

    /// The token to surface for a LISTED key: present iff this backend surfaces per-key list tokens
    /// (supportsListTokens — FALSE on a generation store, where a list-derived token is a poisoned
    /// If-Match) AND the listing carried a non-empty etag. Matches what tokenForHead would return.
    std::optional<Token> tokenForList(const String & etag) const
    {
        if (!supportsListTokens() || etag.empty())
            return std::nullopt;
        return Token{etag, native_token_type};
    }

    /// Whether an observed incarnation token satisfies an expected one: exact identity (value AND
    /// type). Every conditional compare in this backend goes through here.
    static bool tokenMatches(const Token & observed, const Token & expected)
    {
        return observed == expected;
    }
```

Representative migrations in `CasObjectStorageBackend.cpp`:
```cpp
// nativeHead (:177)
    hr.token = tokenForHead(metadata->etag);

// nativeConditionalPut (:272-273) and NativeStreamingSink::finalize (:315-316)
    if (auto etag = buf->getResultObjectETag(); etag && !etag->empty())
        token = tokenForHead(*etag);                    // sink: backend.tokenForHead(*etag)

// list, native page (:959-960) — the supportsListTokens()+empty-etag gate now lives in tokenForList
        if (child->metadata)
            lk.token = tokenForList(child->metadata->etag);

// list, emulated page (:918-919) — same helper (Emulated mode: native_token_type==ETag,
// supportsListTokens()==true, so byte-identical to today's Token{etag, native_token_type})
        if (child->metadata)
            lk.token = tokenForList(child->metadata->etag);

// putOverwrite (:744) / casPut (:783) / deleteExact (:824), emulated compares
    if (!tokenMatches(emuObserveToken(key), expected))     // casPut: *expected ; deleteExact: token
        return {PutOutcome::PreconditionFailed, {}};
```

- [ ] **Step 4: Run tests, verify green**
Run: `./src/unit_tests_dbms --gtest_filter='CasBackendGeneration.*:CasBackendContract/*'`
Expected: PASS (the parameterized contract suite over InMemory + Local exercises head/put/list/delete/overwrite/cas end to end; the generation suite pins the dialect gating).

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.cpp \
        src/Disks/tests/gtest_cas_backend_generation.cpp
git commit -m "cas: consolidate backend token policy into tokenForHead/tokenForList/tokenMatches

Native-ETag / emulated / GCS-generation token minting, the list-token gate, and
token comparison were scattered across head/list/casPut/deleteExact. Route them all
through three helpers; behavior-preserving, pinned by a characterization test.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task C2: Shared `forEachListedKey` iterator + shared delete-outcome classifier

**Files:**
- Create: `.../Core/CasBackendListing.h` (header-only inline helpers — no new TU, no CMake edit)
- Modify: `.../Core/CasGc.cpp` (migrate `Gc::discoverUniverse` :1710-1736; the ~9 other paginated loops + 2 delete-classification clusters follow identically)
- Modify: `.../Core/CasFsck.cpp` (migrate `listAll` :41-61)
- Modify: `.../Core/CasOrphanManifestSweep.cpp` (migrate the delete-classification at `sweepManifestCursorPage` :312-329)
- Test: `src/Disks/tests/gtest_cas_backend_listing.cpp` (Create — direct helper tests) + existing GC/fsck/sweep gtests stay green

**Interfaces:**
- Consumes: `Backend::list(prefix, cursor, limit) -> ListPage`, `ListPage::{keys, next_cursor}`, `ListedKey`, `DeleteOutcome::Kind::{Deleted, NotFound, TokenMismatch}`.
- Produces (in `namespace DB::Cas`, header-only): `void forEachListedKey(Backend &, const String & prefix, const std::function<void(const ListedKey &)> &, size_t page_limit = 1000)`; `enum class DeleteClass : uint8_t { Deleted, Absent, Replaced }`; `DeleteClass classifyDeleteOutcome(const DeleteOutcome &)`; `std::string_view deleteClassName(DeleteClass)`.

Scope note: `forEachListedKey` is over the `Backend` seam. `CasStagingSweeper.cpp` walks `IObjectStorage::listObjects` directly (different seam) — NOT a call site. The blob/manifest delete in `CasGc.cpp:353-408` keeps its blob-specific `created_delete_marker` throw + 412-on-absent HEAD-recheck and calls only `classifyDeleteOutcome` for the three-way mapping.

- [ ] **Step 1: Write the characterization test**
```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackendListing.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>

TEST(CasBackendListing, ForEachWalksEveryPageOnce)
{
    DB::Cas::InMemoryBackend b;
    for (int i = 0; i < 2500; ++i)
        b.putIfAbsent("p/" + std::to_string(1000000 + i), "v");
    b.putIfAbsent("q/other", "v");   /// out of prefix — must not be visited

    std::vector<DB::String> seen;
    DB::Cas::forEachListedKey(b, "p/", [&](const DB::Cas::ListedKey & k) { seen.push_back(k.key); }, /*page_limit=*/1000);
    EXPECT_EQ(seen.size(), 2500u);                                  // paged (3 pages), no key dropped/duplicated
    EXPECT_TRUE(std::is_sorted(seen.begin(), seen.end()));
}

TEST(CasBackendListing, ClassifyMapsEveryDeleteKind)
{
    using DB::Cas::classifyDeleteOutcome; using DB::Cas::DeleteClass; using DB::Cas::DeleteOutcome;
    EXPECT_EQ(classifyDeleteOutcome({DeleteOutcome::Kind::Deleted, false}),      DeleteClass::Deleted);
    EXPECT_EQ(classifyDeleteOutcome({DeleteOutcome::Kind::NotFound, false}),     DeleteClass::Absent);
    EXPECT_EQ(classifyDeleteOutcome({DeleteOutcome::Kind::TokenMismatch, false}),DeleteClass::Replaced);
    EXPECT_EQ(DB::Cas::deleteClassName(DeleteClass::Replaced), "replaced");
}
```

- [ ] **Step 2: Run test, verify current state**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CasBackendListing.*'`
Expected: FAIL to compile (header not created) → PASS after Step 3.

- [ ] **Step 3: Implement** — new header, then migrate the representative sites.

`CasBackendListing.h`:
```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <functional>
#include <string_view>

namespace DB::Cas
{

/// Walk every key under `prefix` exactly once, resuming by the backend's explicit last-returned-key
/// cursor (ListPage::next_cursor, empty => done). The one paginated LIST/cursor loop that GC, fsck,
/// and the sweeps all re-implemented (~10 sites).
inline void forEachListedKey(Backend & backend, const String & prefix,
                             const std::function<void(const ListedKey &)> & cb, size_t page_limit = 1000)
{
    String cursor;
    for (;;)
    {
        const ListPage page = backend.list(prefix, cursor, page_limit);
        for (const ListedKey & k : page.keys)
            cb(k);
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
}

/// The normalized verdict of a token-exact delete, unifying the DeleteOutcome::Kind three-way that GC
/// (blob + manifest delete) and the orphan-manifest sweep each mapped by hand.
enum class DeleteClass : uint8_t { Deleted, Absent, Replaced };

inline DeleteClass classifyDeleteOutcome(const DeleteOutcome & d)
{
    switch (d.kind)
    {
        case DeleteOutcome::Kind::Deleted:       return DeleteClass::Deleted;
        case DeleteOutcome::Kind::NotFound:      return DeleteClass::Absent;
        case DeleteOutcome::Kind::TokenMismatch: return DeleteClass::Replaced;
    }
    return DeleteClass::Replaced;   /// unreachable; fail-safe toward "leave it" (never a false Deleted)
}

inline std::string_view deleteClassName(DeleteClass c)
{
    switch (c)
    {
        case DeleteClass::Deleted:  return "deleted";
        case DeleteClass::Absent:   return "absent";
        case DeleteClass::Replaced: return "replaced";
    }
    return "replaced";
}

}
```

Migration 1 — `CasGc.cpp` `discoverUniverse` (collapse the cursor loop):
```cpp
    std::set<String> namespaces;
    forEachListedKey(backend, prefix, [&](const ListedKey & lk)
    {
        if (const auto parsed = layout.parseRefObjectKey(lk.key))
            namespaces.insert(parsed->ns.string());
    });
```

Migration 2 — `CasFsck.cpp` `listAll` (the per-page deadline/progress cadence is preserved by counting inside the callback; the walk mechanics come from `forEachListedKey`):
```cpp
    forEachListedKey(backend, prefix, [&](const ListedKey & k)
    {
        out[k.key] = k.size;
    });
    /// (page-granular deadline check + PROGRESS_PAGES logging retained via a small stateful callback)
```

Migration 3 — classifier at `CasOrphanManifestSweep.cpp` `sweepManifestCursorPage`:
```cpp
        const DeleteClass verdict = classifyDeleteOutcome(backend.deleteExact(parsed->key, token));
        if (verdict == DeleteClass::Deleted)
            ++result.deleted;
        else
            ++result.skipped;
        // ...
            e.outcome = deleteClassName(verdict);   /// "deleted"/"absent"/"replaced"
```

- [ ] **Step 4: Run tests, verify green**
Run: `./src/unit_tests_dbms --gtest_filter='CasBackendListing.*:CasGc*.*:CasFsck*.*:*OrphanManifest*'`
Expected: PASS (full GC/fsck/sweep suites unchanged).

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackendListing.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.cpp \
        src/Disks/tests/gtest_cas_backend_listing.cpp
git commit -m "cas: shared forEachListedKey iterator + delete-outcome classifier

Factor the paginated LIST/cursor walk (re-implemented ~10x) and the DeleteOutcome
three-way mapping (~4x) into CasBackendListing.h; migrate the GC discovery, fsck
listAll, and orphan-manifest sweep sites. Behavior-preserving.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task C3: Define `Layout::blobKey`/`blobMetaKey`/`parseBlobKey` in a new `CasLayout.cpp`

**Files:**
- Create: `.../Core/CasLayout.cpp` (move the definitions from `CasBuild.cpp:860-926`)
- Modify: `.../Core/CasBuild.cpp:860-926` (delete the three moved definitions + the include-cycle comment)
- Test: existing layout gtests (`gtest_cas_layout*.cpp` / blob-key parse coverage) stay green — pure relocation

**Interfaces:**
- Consumes (reachable from a `.cpp` TU without cycling): `Layout::shardedKey`/`blobsPrefix` (`CasLayout.h`); `BlobRef`, `blobHexOf` (`CasBlobRef.h`); `BlobHashAlgo`, `blobHashAlgoName`, `codecFor` (`CasBlobHasher.h`).
- Produces: no new symbols — `Layout::blobKey`/`blobMetaKey`/`parseBlobKey` (declared `CasLayout.h:86,88,100`) co-locate with their declarations' subtree.

The cycle is header-only: `CasLayout.h` cannot `#include "CasBlobRef.h"` (`CasBlobRef.h → CasBlobDigest.h → CasPoolMeta.h → CasLayout.h`). A `.cpp` has no such constraint.

- [ ] **Step 1: Write the characterization test** (add to the nearest layout suite if `gtest_cas_layout.cpp` doesn't exist)
```cpp
/// C3: blobKey/parseBlobKey are inverses; pins the grammar before relocating the definitions
/// from CasBuild.cpp to CasLayout.cpp (relocation must not change a single byte of output).
TEST(CasLayout, BlobKeyRoundTripsThroughParse)
{
    DB::Cas::Layout layout("pool0");
    const DB::Cas::BlobRef ref{DB::Cas::BlobHashAlgo::XXH3_128,
                               DB::Cas::codecFor(DB::Cas::BlobHashAlgo::XXH3_128).fromHex(std::string(32, 'a'))};
    const DB::String body = layout.blobKey(ref);
    const DB::String meta = layout.blobMetaKey(ref);
    EXPECT_EQ(meta, body + ".meta");

    auto parsed_body = layout.parseBlobKey(body);
    auto parsed_meta = layout.parseBlobKey(meta);   /// body and .meta parse to the SAME BlobRef
    ASSERT_TRUE(parsed_body.has_value());
    ASSERT_TRUE(parsed_meta.has_value());
    EXPECT_EQ(*parsed_body, ref);
    EXPECT_EQ(*parsed_meta, ref);
    EXPECT_FALSE(layout.parseBlobKey("pool0/blobs/unknown-algo/aa/aa00").has_value());  /// foreign => nullopt
}
```

- [ ] **Step 2: Run test, verify current state**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CasLayout.BlobKeyRoundTripsThroughParse'`
Expected: PASS as characterization (the definitions still live in `CasBuild.cpp`; the test pins existing behavior).

- [ ] **Step 3: Implement** — create `CasLayout.cpp` with the three definitions verbatim; delete them from `CasBuild.cpp`.

`CasLayout.cpp`:
```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobRef.h>
#include <Common/Exception.h>

namespace DB::Cas
{

/// Defined out-of-line from CasLayout.h: the header cannot include CasBlobRef.h (that would cycle
/// back through CasBlobDigest.h -> CasPoolMeta.h -> CasLayout.h). A .cpp has the complete BlobRef
/// type and the hash-algo helpers with no cycle, so declaration and definition co-locate here.
String Layout::blobKey(const BlobRef & ref) const
{
    return shardedKey("blobs/" + String(blobHashAlgoName(ref.algo)), blobHexOf(ref));
}

String Layout::blobMetaKey(const BlobRef & ref) const
{
    return blobKey(ref) + ".meta";
}

std::optional<BlobRef> Layout::parseBlobKey(std::string_view key) const
{
    // ... moved verbatim from CasBuild.cpp:876-926 (unchanged) ...
}

}
```
Then in `CasBuild.cpp`: delete lines 860-926 (the three definitions and the include-cycle comment). `CasBuild.cpp` keeps its existing `CasBlobRef`/`CasBlobMeta` includes.

**CMake:** none required. `src/CMakeLists.txt:134` is `add_headers_and_sources(dbms Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core)`, which globs `*.cpp` with `CONFIGURE_DEPENDS`. The new `CasLayout.cpp` is picked up automatically. If a stale cache misses it: `cmake --build build --target rebuild_cache` (or `cd build && cmake .`).

- [ ] **Step 4: Run tests, verify green**
Run: `ninja unit_tests_dbms 2>&1 | tee build/ninja_c3.log && ./src/unit_tests_dbms --gtest_filter='CasLayout.*:CasBuild*.*'`
Expected: links (the moved TU compiles into `dbms`) and PASS.

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/tests/gtest_cas_layout.cpp
git commit -m "cas: define Layout blob-key grammar in CasLayout.cpp, not CasBuild.cpp

Move blobKey/blobMetaKey/parseBlobKey out of CasBuild.cpp (an include-cycle
workaround) into a new CasLayout.cpp so a reader of CasLayout.h can find them.
Glob-picked-up by the existing Core add_headers_and_sources; no CMake edit.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task C4: Unify the `existsDirectory`/`listDirectory` shape dispatch over one routing table

**Files:**
- Modify: `.../ContentAddressedMetadataStorage.h:143-256` (add private `DirShape` enum + `DirRoute` struct + `classifyDirectory` decl + a `classifyDirectoryForTest` accessor)
- Modify: `.../ContentAddressedMetadataStorage.cpp` (`existsDirectory` :654-725, `listDirectory` :810-907 → both switch on `classifyDirectory`)
- Test: `src/Disks/tests/gtest_ca_wiring.cpp` (Modify). **Med risk:** the fixed order (`shadow → atomic-shard → table-uuid → part → subdir → generic`) and the part-branch fall-through must be reproduced exactly.

**Interfaces:**
- Consumes: `isShadowPath`, `isAtomicShardDir`, `parseTableUuid`, `parsePartFilePath`, `parseTableFilePath`, `endsWithTableUuidPair`, `kDetachedDirName`; `route(PartFilePath) -> optional<Route>`, `Route::{ns, ref, file, refKey()}`, `PartFolderView::projectionDirPrefix`, `liveTreeDirHasChildren`, `listLiveTreeChildren`, `detachedRefNames`, `shadowNamespace`, `liveNamespace`.
- Produces (private): `enum class DirShape { ShadowPart, ShadowTable, ShadowIntermediate, AtomicShard, TableDir, DetachedContainer, PartDir, ProjectionDir, TableSubdir, GenericIntermediate }`; `struct DirRoute { DirShape shape; std::optional<PartFilePath> p; std::optional<Route> r; std::optional<std::string> uuid; std::optional<TableFilePath> tf; std::optional<std::string> projection_prefix; }`; `DirRoute classifyDirectory(const std::string & path) const`.

- [ ] **Step 1: Write the characterization test**
```cpp
/// C4: the fixed dispatch order is the invariant. Pins the two ambiguous early guards that make the
/// order load-bearing: store/<u3> (AtomicShard, ambiguous with the non-Atomic table fallback) and a
/// shadow table dir (which also satisfies parseTableUuid). existsDirectory/listDirectory must agree.
TEST(CaWiringRoute, DirShapeDispatchOrderIsStable)
{
    auto storage = makeWiredStorageWithPublishedPart();   // same fixture the CaWiringRead cases use

    using DS = ContentAddressedMetadataStorage::DirShape;
    EXPECT_EQ(storage->classifyDirectoryForTest("store/uui").shape,            DS::AtomicShard);
    EXPECT_EQ(storage->classifyDirectoryForTest("uui/uuid-1").shape,          DS::TableDir);
    EXPECT_EQ(storage->classifyDirectoryForTest("uui/uuid-1/all_1_1_0").shape,DS::PartDir);
    EXPECT_EQ(storage->classifyDirectoryForTest("uui/uuid-1/detached").shape, DS::DetachedContainer);
    EXPECT_EQ(storage->classifyDirectoryForTest("shadow/bk1/store/uui/uuid-1").shape, DS::ShadowTable);
    EXPECT_EQ(storage->classifyDirectoryForTest("shadow/bk1").shape,          DS::ShadowIntermediate);
    EXPECT_EQ(storage->classifyDirectoryForTest("uui/uuid-1/deduplication_logs").shape, DS::TableSubdir);
    EXPECT_EQ(storage->classifyDirectoryForTest("store").shape,              DS::GenericIntermediate);
}
```
(Add a thin `DirRoute classifyDirectoryForTest(const std::string & path) const { return classifyDirectory(path); }`, mirroring the existing `conditionalWriteSettingsForTest` convention.)

- [ ] **Step 2: Run test, verify current state**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CaWiringRead.*:CaWiringRoute.*'`
Expected: existing `CaWiringRead.*` PASS (baseline); the new `DirShapeDispatchOrderIsStable` fails to compile until Step 3 adds `classifyDirectory`.

- [ ] **Step 3: Implement** — the single routing table, consumed by both functions.

`classifyDirectory` (runs the exact fixed order once, reproducing the part-branch fall-through):
```cpp
ContentAddressedMetadataStorage::DirRoute
ContentAddressedMetadataStorage::classifyDirectory(const std::string & path) const
{
    DirRoute dr;
    /// 1. FREEZE shadow — BEFORE the live branches (a shadow table dir also satisfies parseTableUuid).
    if (ContentAddressed::isShadowPath(path))
    {
        if (auto p = ContentAddressed::parsePartFilePath(path); p && !p->backup_name.empty() && p->file.empty())
        {
            dr.shape = DirShape::ShadowPart; dr.p = std::move(p); return dr;
        }
        if (ContentAddressed::endsWithTableUuidPair(path)) { dr.shape = DirShape::ShadowTable; return dr; }
        dr.shape = DirShape::ShadowIntermediate; return dr;
    }
    /// 2. Atomic store/<u3> shard dir — before parseTableUuid misclaims it as a non-Atomic table.
    if (ContentAddressed::isAtomicShardDir(path)) { dr.shape = DirShape::AtomicShard; return dr; }
    /// 3. Table dir.
    if (auto uuid = ContentAddressed::parseTableUuid(path)) { dr.shape = DirShape::TableDir; dr.uuid = std::move(uuid); return dr; }
    /// 4. Part branch — with fall-through if no sub-shape matches.
    if (auto p = ContentAddressed::parsePartFilePath(path))
    {
        if (auto r = route(*p))
        {
            if (r->ref.empty() && p->part_name == ContentAddressed::kDetachedDirName)
            { dr.shape = DirShape::DetachedContainer; dr.p = std::move(p); dr.r = std::move(r); return dr; }
            if (!r->ref.empty() && r->file.empty())
            { dr.shape = DirShape::PartDir; dr.p = std::move(p); dr.r = std::move(r); return dr; }
            if (!r->ref.empty())
                if (auto prefix = ContentAddressed::PartFolderView::projectionDirPrefix(r->file))
                { dr.shape = DirShape::ProjectionDir; dr.p = std::move(p); dr.r = std::move(r); dr.projection_prefix = std::move(prefix); return dr; }
        }
        /// no sub-shape matched: fall through (identical to today's post-`if (p)` continuation).
    }
    /// 5. Table-level subdirectory.
    if (auto tf = ContentAddressed::parseTableFilePath(path)) { dr.shape = DirShape::TableSubdir; dr.tf = std::move(tf); return dr; }
    /// 6. Generic intermediate live-tree dir.
    dr.shape = DirShape::GenericIntermediate; return dr;
}
```

`existsDirectory` rewritten as a switch (each arm is the existing per-shape body, verbatim):
```cpp
bool ContentAddressedMetadataStorage::existsDirectory(const std::string & path) const
{
    const DirRoute dr = classifyDirectory(path);
    switch (dr.shape)
    {
        case DirShape::ShadowPart:
            return partAccess().existsRef(Route{shadowNamespace(dr.p->shadow_table_dir), dr.p->part_name, ""}.refKey(),
                                          ContentAddressed::Freshness::CachedForLoad);
        case DirShape::ShadowTable:
            return !store()->listRefs(shadowNamespace(path)).empty();
        case DirShape::ShadowIntermediate:
        {
            const std::string canonical = canonicalDiskPath(path);
            const std::string scope = canonical.empty() ? "shadow/" : canonical + "/";
            for (const auto & ns : store()->listNamespaces(scope))
                if (!store()->listRefs(Cas::RootNamespace{ns}).empty())
                    return true;
            return false;
        }
        case DirShape::AtomicShard:        return liveTreeDirHasChildren(path);
        case DirShape::TableDir:           return !store()->listRefs(liveNamespace(*dr.uuid)).empty();
        case DirShape::DetachedContainer:  return !detachedRefNames(dr.r->ns).empty();
        case DirShape::PartDir:            return partAccess().existsRef(dr.r->refKey(), ContentAddressed::Freshness::CachedForLoad);
        case DirShape::ProjectionDir:
        {
            auto view = partAccess().getView(dr.r->refKey(), ContentAddressed::Freshness::CachedForLoad);
            return view && view->hasDirectory(*dr.projection_prefix);
        }
        case DirShape::TableSubdir:
        {
            const std::string prefix = dr.tf->tail + "/";
            for (const auto & name : store()->listNamespaceFiles(liveNamespace(dr.tf->table_uuid)))
                if (name.starts_with(prefix))
                    return true;
            return false;
        }
        case DirShape::GenericIntermediate: return liveTreeDirHasChildren(path);
    }
    return liveTreeDirHasChildren(path);   /// unreachable
}
```
`listDirectory` follows identically — same `switch (dr.shape)`, each arm the existing list body (`view->listChildren`, `addFirstComponent` collapse, `listLiveTreeChildren`, etc.). Both functions now consume the ONE order defined in `classifyDirectory`.

- [ ] **Step 4: Run tests, verify green**
Run: `./src/unit_tests_dbms --gtest_filter='CaWiring*.*:CaPartPathParser.*'`
Expected: PASS (the full read/route/shadow/projection/detached/dedup wiring suite — the behavioral oracle for this refactor).

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/tests/gtest_ca_wiring.cpp
git commit -m "cas: unify existsDirectory/listDirectory shape dispatch over one routing table

The load-bearing fixed order (shadow -> atomic-shard -> table-uuid -> part ->
subdir -> generic) was implemented twice and kept in sync by hand. Route both
through classifyDirectory; behavior-preserving, dispatch order pinned by gtest_ca_wiring.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task R412: one `S3::isPreconditionFailedError` / `S3Exception::isPreconditionFailed` policy  [Ring 2 — shrinks the diff]

**Why this SHRINKS the fork diff:** the branch open-coded the 412 check three ways — `RetryStrategy` uses the HTTP response code, while the two CA-added conditional ops (`removeObjectIfTokenMatches`, `copyObjectConditional`) use only `getExceptionName()=="PreconditionFailed" || message.find(...)`, which *misses* the RustFS "unparsed ExceptionName" case the RetryStrategy already handles. Collapsing all three onto one policy deletes the duplicated string checks in the shared S3 files and closes that misclassification gap — fewer modified lines, one correct rule.

**Files:**
- Modify: `src/IO/S3Common.h:64-70` (declare `bool isPreconditionFailed() const;` on `S3Exception`; declare free `S3::isPreconditionFailedError` template in the `DB::S3` block ~:98-104)
- Modify: `src/IO/S3Common.cpp` (define `S3Exception::isPreconditionFailed`)
- Modify: `src/IO/S3/Client.cpp:112` (RetryStrategy) → `S3::isPreconditionFailedError(error)`
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:497-498` (`removeObjectIfTokenMatches`) and `:802-803` (`copyObjectConditional`)
- Test: `src/IO/S3/tests/gtest_aws_s3_client.cpp:201` (extend `DoesNotRetryPreconditionFailed`)

**Interfaces:**
- Consumes: `Aws::Client::AWSError<E>::{GetResponseCode, GetExceptionName, GetMessage}`, `Aws::Http::HttpResponseCode::PRECONDITION_FAILED`, `S3Exception::{exception_name, message()}`.
- Produces: `template <typename ErrorType> bool DB::S3::isPreconditionFailedError(const Aws::Client::AWSError<ErrorType> & error)` (header-inline; works for both `CoreErrors` and `S3Errors`); `bool DB::S3Exception::isPreconditionFailed() const` (typed-exception surface; name+message only).

- [ ] **Step 1: Write the failing test** (extend the existing B166 test)
```cpp
TEST(IOTestAwsS3Client, DoesNotRetryPreconditionFailed)
{
    DB::S3::Client::RetryStrategy strategy(DB::S3::PocoHTTPClientConfiguration::RetryStrategy{.max_retries = 10});

    Aws::Client::AWSError<Aws::Client::CoreErrors> precondition(Aws::Client::CoreErrors::UNKNOWN, /*isRetryable=*/true);
    precondition.SetResponseCode(Aws::Http::HttpResponseCode::PRECONDITION_FAILED);
    EXPECT_FALSE(strategy.ShouldRetry(precondition, /*attemptedRetries=*/0));
    EXPECT_TRUE(DB::S3::isPreconditionFailedError(precondition));       // helper agrees via response code

    Aws::Client::AWSError<Aws::Client::CoreErrors> unavailable(Aws::Client::CoreErrors::SLOW_DOWN, /*isRetryable=*/true);
    unavailable.SetResponseCode(Aws::Http::HttpResponseCode::SERVICE_UNAVAILABLE);
    EXPECT_TRUE(strategy.ShouldRetry(unavailable, /*attemptedRetries=*/0));
    EXPECT_FALSE(DB::S3::isPreconditionFailedError(unavailable));

    /// RustFS-style: unparsed ExceptionName, but the <Code> string is present. A store that surfaces
    /// ONLY the 412 response code (no name/message) is now caught too.
    Aws::Client::AWSError<Aws::S3::S3Errors> named(Aws::S3::S3Errors::UNKNOWN, "PreconditionFailed", "precondition failed", false);
    EXPECT_TRUE(DB::S3::isPreconditionFailedError(named));

    /// Typed-exception surface (copyObjectConditional catches S3Exception).
    EXPECT_TRUE(DB::S3Exception("boom", Aws::S3::S3Errors::UNKNOWN, "PreconditionFailed").isPreconditionFailed());
    EXPECT_FALSE(DB::S3Exception("boom", Aws::S3::S3Errors::NO_SUCH_KEY, "NoSuchKey").isPreconditionFailed());
}
```

- [ ] **Step 2: Run test, verify current state**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='IOTestAwsS3Client.DoesNotRetryPreconditionFailed'`
Expected: FAIL to compile (`isPreconditionFailedError`/`isPreconditionFailed` undeclared) → PASS after Step 3.

- [ ] **Step 3: Implement**

`S3Common.h` — member decl next to `isRetryableError`:
```cpp
    bool isRetryableError() const;
    bool isAccessTokenExpiredError() const;
    /// True iff this is a 412 Precondition Failed (a lost conditional PUT/DELETE). The typed
    /// exception does not carry the HTTP status, so it matches on the canonical <Code> name and message.
    bool isPreconditionFailed() const;
```
`S3Common.h` — free template in the `namespace DB { namespace S3 {` block:
```cpp
/// A 412 Precondition Failed is the deterministic result of a conditional request (If-Match /
/// If-None-Match). Detect it robustly: the HTTP response code is authoritative (an S3-compatible
/// server returning a non-AWS body — RustFS — leaves ExceptionName unparsed), with the canonical
/// <Code> name / message as the fallback. One policy for the RetryStrategy, conditional delete, and
/// conditional copy paths.
template <typename ErrorType>
inline bool isPreconditionFailedError(const Aws::Client::AWSError<ErrorType> & error)
{
    return error.GetResponseCode() == Aws::Http::HttpResponseCode::PRECONDITION_FAILED
        || error.GetExceptionName() == "PreconditionFailed"
        || error.GetMessage().find("PreconditionFailed") != std::string::npos;
}
```
`S3Common.cpp`:
```cpp
bool S3Exception::isPreconditionFailed() const
{
    return exception_name == "PreconditionFailed"
        || message().find("PreconditionFailed") != std::string::npos;
}
```
Call sites:
```cpp
// Client.cpp:105-113 -> the response-code check is the first disjunct, so this is a strict superset.
    if (S3::isPreconditionFailedError(error))
        return false;

// S3ObjectStorage.cpp:497-498 (removeObjectIfTokenMatches)
    if (S3::isPreconditionFailedError(err))
        return {ConditionalRemoveOutcome::TokenMismatch, false};

// S3ObjectStorage.cpp:802-803 (copyObjectConditional) — catches S3Exception, uses the member
    if (exc.isPreconditionFailed())
        return {.created = false, .dest_etag = {}};
```

- [ ] **Step 4: Run tests, verify green**
Run: `./src/unit_tests_dbms --gtest_filter='IOTestAwsS3Client.*'`
Expected: PASS (the retry-storm guard and both conditional-op surfaces now share one rule).

- [ ] **Step 5: Commit**
```bash
git add src/IO/S3Common.h src/IO/S3Common.cpp src/IO/S3/Client.cpp \
        src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp \
        src/IO/S3/tests/gtest_aws_s3_client.cpp
git commit -m "cas: one 412 PreconditionFailed policy (S3::isPreconditionFailedError)

Collapse the three open-coded 412 checks (RetryStrategy response-code vs the two
CA conditional-op name/message checks) onto one helper that checks response code
AND name/message. Shrinks the shared-file diff and fixes the 'unparsed ExceptionName'
misclassification on non-AWS stores.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task RExpect: scope `Expect: 100-continue` to CAS-owned conditional writes  [Ring 2 — bug fix, makes safer]

**Why this SHRINKS/neutralizes the fork diff:** the branch added an `Expect: 100-continue` negotiation to shared `PocoHTTPClient` that fires for *any* conditional PUT ≥ `expect_continue_min_bytes` (default 1 MiB) — including upstream's non-CAS conditional-commit path (Iceberg `If-None-Match`). Gating it on a CAS-owned `WriteSettings` flag delivered via a thread-local scope makes the shared-file delta stop altering wire behavior for anyone but CAS, so the carried diff becomes behaviorally inert for upstream users.

**Files:**
- Modify: `src/IO/WriteSettings.h:56-58` (add the CAS-owned flag next to `s3_force_single_part_upload`)
- Create: `src/IO/S3/S3ExpectContinueScope.h` + `.cpp` (thread-local RAII, mirroring `src/IO/Expect404ResponseScope.{h,cpp}`)
- Modify: `src/IO/WriteBufferFromS3.cpp:772-777` (open the scope around `PutObject` when the flag is set) and `completeMultipartUpload` (:664-666 region, same guard)
- Modify: `src/IO/S3/PocoHTTPClient.cpp:658-664` (add the scope predicate to the `conditional_write` gate)
- Modify: `.../Core/CasObjectStorageBackend.cpp:671-692` (`conditionalWriteSettings` sets the flag — the single CAS conditional-write settings site)
- Test: `src/IO/tests/gtest_writebuffer_s3.cpp` (the `MockS3::Client::PutObject` override runs on the request thread, so it can assert `S3ExpectContinueScope::isEnabled()` at PutObject time)

**Interfaces:**
- Consumes: `WriteBufferFromS3::write_settings`, the `CurrentThread::IOSchedulingScope` RAII precedent at `WriteBufferFromS3.cpp:773`, `poco_request.setExpectContinue`.
- Produces: `WriteSettings::s3_conditional_write_use_expect_continue = false` (CAS-owned bool); `class DB::S3ExpectContinueScope { S3ExpectContinueScope(); ~S3ExpectContinueScope(); static bool isEnabled(); };` (thread-local counter, mirror of `Expect404ResponseScope`).

- [ ] **Step 1: Write the failing test**
```cpp
/// RExpect: the Expect:100-continue scope is active during a CAS conditional write (flag set) and
/// absent for a plain write (flag clear) — so non-CAS conditional PUTs (Iceberg) never negotiate it.
struct RecordExpectScopeInjection : MockS3::InjectionModel
{
    bool * enabled_at_put;
    std::optional<Aws::S3::Model::PutObjectOutcome> call(const Aws::S3::Model::PutObjectRequest &) override
    {
        *enabled_at_put = DB::S3ExpectContinueScope::isEnabled();
        return std::nullopt;   /// fall through to the mock's real PutObject
    }
};

TEST_F(WBS3Test, ExpectContinueScopeIsCasOnly)
{
    bool enabled = false;
    auto injection = std::make_shared<RecordExpectScopeInjection>(); injection->enabled_at_put = &enabled;
    setInjectionModel(injection);

    DB::WriteSettings ws;                                    // plain write: flag clear
    ws.object_storage_write_if_none_match = "*";
    writeOneObject(/*settings=*/ws, /*bytes=*/2 * 1024 * 1024);
    EXPECT_FALSE(enabled);                                   // Iceberg-style conditional PUT: no scope

    ws.s3_conditional_write_use_expect_continue = true;      // CAS write: flag set
    writeOneObject(/*settings=*/ws, /*bytes=*/2 * 1024 * 1024);
    EXPECT_TRUE(enabled);
}
```

- [ ] **Step 2: Run test, verify current state**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='WBS3Test.ExpectContinueScopeIsCasOnly'`
Expected: FAIL to compile (flag + scope undeclared) → after Step 3, PASS (scope enabled only under the CAS flag).

- [ ] **Step 3: Implement**

`WriteSettings.h` (next to `s3_force_single_part_upload`):
```cpp
    /// CAS-owned: opt this conditional write into the Expect:100-continue negotiation in PocoHTTPClient
    /// (reject-before-body on a lost precondition — B118). Default OFF so a non-CAS conditional PUT
    /// (e.g. Iceberg's If-None-Match metadata commit) keeps its historical wire behavior. Delivered to
    /// the HTTP layer via S3ExpectContinueScope, since WriteSettings does not reach PocoHTTPClient.
    bool s3_conditional_write_use_expect_continue = false;
```

`S3ExpectContinueScope.{h,cpp}` — verbatim structure of `Expect404ResponseScope` (thread-local `size_t` counter, `chassert`-guarded same-thread dtor, `static bool isEnabled()`).

`WriteBufferFromS3.cpp:772-777` (single-part PUT; and the same two-line guard around `client_ptr->CompleteMultipartUpload`):
```cpp
            CurrentThread::IOSchedulingScope io_scope(write_settings.io_scheduling);
            CurrentThread::WriteThrottlingScope write_throttling_scope(write_settings.remote_throttler);
            std::optional<S3ExpectContinueScope> expect_scope;
            if (write_settings.s3_conditional_write_use_expect_continue)
                expect_scope.emplace();

            Stopwatch watch;
            auto outcome = client_ptr->PutObject(request);
```

`PocoHTTPClient.cpp:658-664` — add the scope predicate (last conjunct; the body-size + header + method gate is unchanged):
```cpp
            const bool conditional_write
                = S3ExpectContinueScope::isEnabled()
                && method == Poco::Net::HTTPRequest::HTTP_PUT
                && content_body_size >= min_body_size_for_expect_continue
                && (poco_request.has("if-none-match") || poco_request.has("if-match")
                    || poco_request.has("x-goog-if-generation-match"));
```

`CasObjectStorageBackend.cpp` `conditionalWriteSettings` (alongside `s3_skip_check_objects_after_upload`):
```cpp
    ws.s3_skip_check_objects_after_upload = true;
    ws.s3_conditional_write_use_expect_continue = true;   /// scope Expect:100-continue to CAS writes
```

- [ ] **Step 4: Run tests, verify green**
Run: `./src/unit_tests_dbms --gtest_filter='WBS3Test.*:SyncAsync/*'`
Expected: PASS (single/multipart upload paths unaffected; the scope is inert unless the CAS flag is set).

- [ ] **Step 5: Commit**
```bash
git add src/IO/WriteSettings.h src/IO/S3/S3ExpectContinueScope.h src/IO/S3/S3ExpectContinueScope.cpp \
        src/IO/WriteBufferFromS3.cpp src/IO/S3/PocoHTTPClient.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.cpp \
        src/IO/tests/gtest_writebuffer_s3.cpp
git commit -m "cas: scope Expect:100-continue to CAS-owned conditional writes

The negotiation the branch added to PocoHTTPClient fired for any conditional PUT
>= threshold, changing wire behavior for non-CAS conditional commits (Iceberg).
Gate it on a CAS-owned WriteSettings flag delivered via S3ExpectContinueScope
(mirroring Expect404ResponseScope), neutralizing the shared-file behavioral delta.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Phase 5 — Performance / operability

### Task B1: Memoize the raw-path split so the CA read path parses each path once per file-open

**Files:**
- Modify: `.../ContentAddressed/PartPathParser.cpp:6-26` (`splitNonEmpty` — `std::move`), `:143,172,195,201,210,217` (route every classifier through the memo)
- Modify: `.../ContentAddressed/PartPathParser.h` (append two test/observability accessors)
- Test: `src/Disks/tests/gtest_ca_wiring.cpp` (Modify — add one `CaPartPathParser` case)

**Interfaces:**
- Consumes: `DB::ContentAddressed::splitNonEmpty` (file-static), `parsePartFilePath`/`isPartFilePath`/`parseTableFilePath`/`parseTableUuid`/`isAtomicShardDir`/`endsWithTableUuidPair`
- Produces: file-static `splitCached(const std::string &) -> const std::vector<std::string> &` (thread-local MRU); public `size_t splitCacheMissesForTest()`, `void resetSplitCacheForTest()`

Scope note: the split (`splitNonEmpty`) is the ~10-15-allocation cost the finding calls out and is a **pure function of the path only** (no disk config), so a thread-local recency cache keyed on the raw path is always correct and disk-agnostic. Every CA metadata entry point reaches the parser only through these free functions and each runs `isPartFilePath(path)` then `parsePartFilePath(path)` on the *same* raw path — so memoizing at the parser transitively fixes all the entry points without touching `ContentAddressedMetadataStorage.cpp`. `route` does not split; it is left as the cheap remainder.

> Coordination with C4: C4 (Task in Phase 4) makes `existsDirectory`/`listDirectory` call the classifiers via `classifyDirectory`; both still go through these free functions, so B1's memo covers them too. No ordering constraint.

- [ ] **Step 1: Write the failing test**
```cpp
TEST(CaPartPathParser, RawPathSplitMemoizedAcrossClassifiers)
{
    // The CA read path runs isPartFilePath then parsePartFilePath on the SAME raw path several times
    // per logical file-open (existsFile -> getFileSize -> getStorageObjects). The split is a pure
    // function of the path, so all of those must split the path exactly ONCE (B1).
    resetSplitCacheForTest();
    const std::string path = "store/uui/uuid-1/all_1_1_0/columns.txt";
    EXPECT_TRUE(isPartFilePath(path));
    ASSERT_TRUE(parsePartFilePath(path).has_value());
    ASSERT_TRUE(parsePartFilePath(path).has_value());
    EXPECT_EQ(splitCacheMissesForTest(), 1u) << "the same raw path must be split only once";

    // A distinct raw path is a fresh split (miss #2); repeats of it reuse the memo.
    const std::string other = "store/uui/uuid-2/all_1_1_0/data.bin";
    EXPECT_TRUE(isPartFilePath(other));
    EXPECT_TRUE(isPartFilePath(other));
    EXPECT_EQ(splitCacheMissesForTest(), 2u);

    // Correctness is unchanged: the memoized parse yields the same fields the direct parse would.
    const auto parsed = parsePartFilePath(path);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->table_uuid, "uuid-1");
    EXPECT_EQ(parsed->part_name, "all_1_1_0");
    EXPECT_EQ(parsed->file, "columns.txt");
}
```

- [ ] **Step 2: Run test, verify it fails**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CaPartPathParser.RawPathSplitMemoizedAcrossClassifiers'`
Expected: FAIL — compile error, `splitCacheMissesForTest`/`resetSplitCacheForTest` are undeclared.

- [ ] **Step 3: Implement**

In `PartPathParser.cpp`, add `#include <utility>`, fix `splitNonEmpty` to move each component, and add the memo above `findTableUuidComponent`:

```cpp
static std::vector<std::string> splitNonEmpty(const std::string & path)
{
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path)
    {
        if (c == '/')
        {
            if (!cur.empty())
                parts.push_back(std::move(cur));
            cur.clear();
        }
        else
        {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        parts.push_back(std::move(cur));
    return parts;
}

namespace
{

/// The split of a disk-relative path into non-empty components is the dominant allocation of every
/// path classifier, and the CA metadata read path runs SEVERAL of them on the SAME raw path per
/// logical file-open (each of existsFile / getFileSize / getStorageObjects first calls isPartFilePath,
/// then parsePartFilePath). The split is a PURE function of the path, so a small thread-local
/// most-recently-used cache keyed on the raw path is always correct, disk-agnostic and lock-free.
/// The returned reference stays valid until the next splitCached call on the SAME thread; every
/// classifier consumes its split before splitting again (none splits while holding another's split).
struct SplitCache
{
    static constexpr size_t kCapacity = 8;
    std::array<std::pair<std::string, std::vector<std::string>>, kCapacity> slots;
    size_t count = 0;   /// populated slots (<= kCapacity)
    size_t next = 0;    /// round-robin insertion cursor
    size_t misses = 0;  /// underlying splitNonEmpty invocations (observability / test oracle)

    const std::vector<std::string> & get(const std::string & path)
    {
        for (size_t i = 0; i < count; ++i)
            if (slots[i].first == path)
                return slots[i].second;
        ++misses;
        auto & slot = slots[next];
        slot.first = path;
        slot.second = splitNonEmpty(path);
        next = (next + 1) % kCapacity;
        if (count < kCapacity)
            ++count;
        return slot.second;
    }
};

thread_local SplitCache tls_split_cache;

const std::vector<std::string> & splitCached(const std::string & path)
{
    return tls_split_cache.get(path);
}

}
```

Change the six classifier split sites from `auto p = splitNonEmpty(path);` to `const auto & p = splitCached(path);` — in `parsePartFilePath` (`:143`), `parseTableUuid` (`:172`), `isAtomicShardDir` (`:195`), `endsWithTableUuidPair` (`:201`), `isPartFilePath` (`:210`), `parseTableFilePath` (`:217`). (`isShadowPath` does its own manual first-component scan and is left untouched.) These classifiers copy their result strings out of `p`, so returning `p` by reference is safe.

Add the accessors before the definition of `parsePartFilePath` (they read the anonymous-namespace `tls_split_cache` in the same TU):
```cpp
size_t splitCacheMissesForTest()
{
    return tls_split_cache.misses;
}

void resetSplitCacheForTest()
{
    tls_split_cache = SplitCache{};
}
```
And declare them at the end of `PartPathParser.h`, before the closing `}` of `namespace DB::ContentAddressed`:
```cpp
/// Test/observability (B1): underlying splitNonEmpty invocations on THIS thread.
size_t splitCacheMissesForTest();
void resetSplitCacheForTest();
```

- [ ] **Step 4: Run test, verify it passes**
Run: `./src/unit_tests_dbms --gtest_filter='CaPartPathParser.*'`
Expected: PASS — the new case plus every pre-existing `CaPartPathParser.Parse*` characterization case (which pin the exact parse outputs the `std::move` and the memo must preserve).

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h \
        src/Disks/tests/gtest_ca_wiring.cpp
git commit -m "cas: memoize the raw-path split so the CA read path parses each path once (B1)

splitNonEmpty is the dominant allocation of every path classifier and the CA
read path runs isPartFilePath+parsePartFilePath on the same raw path several
times per logical file-open. Route the classifiers through a thread-local MRU
split cache (pure function of the path -> always correct, lock-free) and move
each component out of splitNonEmpty instead of copying.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task B2: Make the getView explain journal opt-in and compute the cache key once

**Files:**
- Modify: `.../ContentAddressed/CachedPartFolderAccess.h:30-38` (`CacheParams`), diagnostics section (add `explainJournalSizeForTest`), `recordDecision` decl
- Modify: `.../ContentAddressed/CachedPartFolderAccess.cpp:46-116` (`getView`), `:160` (`eraseView`), `:310-324` (`recordDecision`)
- Test: `src/Disks/tests/gtest_cas_part_folder_access.cpp` (Modify — enable the journal in the tests that assert on it; add one hit-path oracle)

**Interfaces:**
- Consumes: `CacheParams`, `PartRefKey::cacheKey`, `recordDecision`, `explain_mutex`/`explain_map`
- Produces: `CacheParams::explain_enabled` (default `false`); `recordDecision(const String & cache_key, …)`; `size_t explainJournalSizeForTest() const`

- [ ] **Step 1: Write the failing test**
```cpp
TEST(CasPartFolderAccess, HitPathJournalEmptyAndCheapWhenExplainDisabled)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    /// Retention ON, explain journal OFF (the production default): the hit path must take neither the
    /// per-disk explain mutex nor write a journal entry (B2).
    ContentAddressed::CachedPartFolderAccess access(store,
        {.cache_bytes = 64ULL << 20, .max_entries = 10000, .max_entry_bytes = 16ULL << 20,
         .explain_enabled = false});
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});
    const ContentAddressed::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    backend->resetCounts();
    for (int i = 0; i < 5; ++i)
        ASSERT_NE(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);

    /// Same request oracle as RetainedHitSkipsManifestHead — one cold build, then validated hits.
    EXPECT_EQ(backend->getCount(manifest_key), 1u);
    EXPECT_EQ(backend->headCount(manifest_key), 1u);
    /// The journal is never written when disabled.
    EXPECT_EQ(access.explainJournalSizeForTest(), 0u);
    /// explain() still reports live retention truthfully, but the decision defaults to Miss (unwritten).
    EXPECT_TRUE(access.explain(key).retained);
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::Miss);
}
```

- [ ] **Step 2: Run test, verify it fails**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CasPartFolderAccess.HitPathJournalEmptyAndCheapWhenExplainDisabled'`
Expected: FAIL — compile error, `CacheParams::explain_enabled` and `explainJournalSizeForTest()` do not exist; once added, the assertion `explainJournalSizeForTest() == 0` fails because `recordDecision` records unconditionally.

- [ ] **Step 3: Implement**

In `CachedPartFolderAccess.h`, add the opt-in flag to `CacheParams`:
```cpp
    struct CacheParams
    {
        uint64_t cache_bytes = 0;            /// 0 = retention disabled (unit-test default;
                                             /// the DISK default is 64 MiB, set in the factory)
        uint64_t max_entries = 10000;
        uint64_t max_entry_bytes = 16ULL << 20;
        /// The explain decision journal (spec §Observability) is test/log-only and its recordDecision
        /// path takes a per-disk global mutex and allocates on EVERY read. Off by default so the read
        /// hit path never pays for it; the disk factory / tests turn it on when they consult explain().
        bool explain_enabled = false;
    };
```
Add the test accessor to the diagnostics section (next to `explain`/`clearForTest`):
```cpp
    /// Test-only: number of entries in the decision journal (0 whenever explain is disabled).
    size_t explainJournalSizeForTest() const;
```
Change the private `recordDecision` declaration to take the precomputed key:
```cpp
    void recordDecision(const String & cache_key, LastDecision decision,
                        const PartFolderView * view, bool retained) const;
```

In `CachedPartFolderAccess.cpp`, rewrite `getView` to compute the key once (after the absent-ref early return, so absent refs never allocate it) and thread it through:
```cpp
std::shared_ptr<const PartFolderView>
CachedPartFolderAccess::getView(const PartRefKey & key, Freshness freshness) const
{
    /// Step 1 (spec §Validate-On-Hit): the SAME resolve every read already pays today. Absence is
    /// never retained.
    auto resolved = resolve(key, freshness);
    if (!resolved)
        return nullptr;

    /// One cache-key materialization per getView (B2): reused by the retained get/set and the journal.
    const String cache_key = key.cacheKey();

    /// Step 2: retained views serve ONLY CachedForLoad. ForceFresh must re-prove the manifest BODY
    /// (a fresh ref resolve proves ref currency, not body existence — review 2026-07-08);
    /// StrictValidate bypasses retention entirely.
    if (freshness == Freshness::CachedForLoad && view_cache)
    {
        if (auto cached = view_cache->get(cache_key))
        {
            if (cached->manifestId() == resolved->manifest_id)
            {
                if (cached->mutableFiles() == resolved->mutable_files)
                {
                    ProfileEvents::increment(ProfileEvents::CasPartFolderViewHits);
                    recordDecision(cache_key, LastDecision::Hit, cached.get(), /*retained=*/true);
                    return cached;
                }
                /// 2b: manifest unchanged, mutable-only drift (txn_version bumps) — clone around
                /// the SAME shared decode; no manifest operation at all.
                auto refreshed = std::make_shared<PartFolderView>(
                    key, resolved->manifest_id, resolved->manifest_size,
                    resolved->published_at_ms, resolved->mutable_files, cached->manifest());
                if (refreshed->estimatedBytes() <= params.max_entry_bytes)
                    view_cache->set(cache_key, refreshed);
                ProfileEvents::increment(ProfileEvents::CasPartFolderViewMutableRefreshes);
                recordDecision(cache_key, LastDecision::MutableRefresh, refreshed.get(), /*retained=*/true);
                return refreshed;
            }
            ProfileEvents::increment(ProfileEvents::CasPartFolderViewValidationMismatches);
            /// fall through to rebuild — the stale entry is superseded by the insert below
        }
    }

    auto view = buildView(key, *resolved, freshness);

    /// Step 4: retain (StrictValidate never populates; oversized views are served, not retained).
    bool retained = false;
    bool oversized = false;
    if (freshness != Freshness::StrictValidate && view_cache)
    {
        if (view->estimatedBytes() <= params.max_entry_bytes)
        {
            /// CacheBase stores mutable pointers; views are logically const (never mutated).
            view_cache->set(cache_key, std::const_pointer_cast<PartFolderView>(view));
            retained = true;
        }
        else
        {
            oversized = true;
            ProfileEvents::increment(ProfileEvents::CasPartFolderViewOversizedBypasses);
        }
    }
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewMisses);
    recordDecision(cache_key,
        freshness == Freshness::CachedForLoad ? (oversized ? LastDecision::OversizedBypass : LastDecision::Miss)
        : freshness == Freshness::ForceFresh  ? LastDecision::ForceFreshRead
                                              : LastDecision::StrictBypass,
        view.get(), retained);
    return view;
}
```
Update `eraseView` to reuse its one key:
```cpp
void CachedPartFolderAccess::eraseView(const PartRefKey & key)
{
    const String cache_key = key.cacheKey();
    if (view_cache)
        view_cache->remove(cache_key);
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewInvalidations);
    recordDecision(cache_key, LastDecision::Invalidated, nullptr, /*retained=*/false);
}
```
Gate `recordDecision` on the flag and take the key directly:
```cpp
void CachedPartFolderAccess::recordDecision(const String & cache_key, LastDecision decision,
                                            const PartFolderView * view, bool retained) const
{
    if (!params.explain_enabled)
        return;   /// B2: the hit path pays neither the per-disk mutex nor a journal allocation.
    std::lock_guard lock(explain_mutex);
    if (explain_map.size() >= EXPLAIN_MAX_ENTRIES)
        explain_map.clear();
    auto & e = explain_map[cache_key];
    e.last_decision = decision;
    e.retained = retained;
    if (view)
    {
        e.manifest_ref = Cas::manifestRefDebugString(view->manifestId().ref);
        e.estimated_bytes = view->estimatedBytes();
    }
}

size_t CachedPartFolderAccess::explainJournalSizeForTest() const
{
    std::lock_guard lock(explain_mutex);
    return explain_map.size();
}
```
Because the journal is now off by default, update the tests that assert on `explain(key).last_decision` to enable it (behavior-preserving for those tests): in the `cacheOn()` helper add `.explain_enabled = true`; in `ExplainRecordsDecisions` construct `CachedPartFolderAccess access(store, {.explain_enabled = true});`; in `OversizedViewServedNotRetained` add `.explain_enabled = true`. (`DisabledModeKeepsBaseline` asserts only `explain(key).retained == false`, still correct with the journal off — no change.)

- [ ] **Step 4: Run test, verify it passes**
Run: `./src/unit_tests_dbms --gtest_filter='CasPartFolderAccess.*'`
Expected: PASS — the new hit-path oracle plus the whole retention battery (which pins the byte-identical request counts the `cacheKey()`-once change must preserve).

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.cpp \
        src/Disks/tests/gtest_cas_part_folder_access.cpp
git commit -m "cas: make the getView explain journal opt-in and key once (B2)

The cache-hit path unconditionally took the per-disk explain_mutex and
allocated cacheKey() up to four times purely to maintain a test/log-only
decision journal. Gate recordDecision on a CacheParams.explain_enabled flag
(off by default) and compute the cache key once per getView.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task B3: Per-disk GC health on `system.content_addressed_mounts`; retire the process-global gauges

**Files:**
- Modify: `.../ContentAddressed/CasGcScheduler.h` / `.cpp` (add `GcHealth` + `gcHealth()`; replace the two global gauge writes with per-scheduler atomics)
- Modify: `.../ContentAddressed/Core/CasStore.h` / `.cpp` (`wedgedRefLaneCount`)
- Modify: `.../ContentAddressed/ContentAddressedMetadataStorage.h` / `.cpp` (`gcHealth()` passthrough)
- Modify: `src/Storages/System/StorageSystemContentAddressedMounts.cpp` (4 new columns) **[Ring 1]**
- Modify: `src/Common/CurrentMetrics.cpp:231-232` (retire `CasGcIsLeader`/`CasGcPendingReclaimEntries`) **[Ring 1]**
- Test: `src/Disks/tests/gtest_cas_gc_log.cpp` (Modify — scheduler-level `gcHealth()` oracle); **plus a stateless SQL test — see note**

**Interfaces:**
- Consumes: `CasGcScheduler::i_am_leader`, `Cas::RoundReport{acquired_lease, condemned, redeleted}`, `Store::ref_tables`/`RefTableRuntime::wedge`, `ContentAddressedMetadataStorage::store`/`gc_scheduler`
- Produces: `CasGcScheduler::GcHealth`, `CasGcScheduler::gcHealth()`, `Store::wedgedRefLaneCount()`, `ContentAddressedMetadataStorage::gcHealth()`, columns `is_leader`/`pending_reclaim`/`last_success_age_seconds`/`wedged_namespace_count`

**Testability + dependency note:** the column assembly in `StorageSystemContentAddressedMounts::read` is not cleanly extractable (iterates `getDisksMap()`, `dynamic_cast`s, opens the store, calls `Cas::listMounts`). The smallest unit-testable unit is `CasGcScheduler::gcHealth()` (gtest below). The 4-column table wiring needs a **stateless SQL test under the CA-default job** (`tests/queries/0_stateless/03XXX_content_addressed_mounts_gc_health.sql`: `SELECT is_leader, pending_reclaim, last_success_age_seconds, wedged_namespace_count FROM system.content_addressed_mounts` asserting the columns/types and `wedged_namespace_count = 0` / `is_leader ∈ (0,1)` on a healthy single-disk fixture). **Depends on A7** (guarding lazy `gc_scheduler`): read `gc_scheduler` under A7's `gc_scheduler_mutex` in `ContentAddressedMetadataStorage::gcHealth()`.

- [ ] **Step 1: Write the failing test**
```cpp
/// B3: the scheduler exposes per-disk GC health for system.content_addressed_mounts (the process-
/// global CurrentMetrics gauges were clobbered with >= 2 CAS disks). Drive one leader round and
/// assert the health snapshot reflects leadership, the pending-reclaim backlog and a fresh success.
TEST(CasGcHealth, ReflectsLeadershipAndPendingReclaim)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0});
    const RootNamespace ns{"srv1/tbl"};
    publishPart(store, ns.string(), "all_0_0_0", "hello-cas-gc-health");
    store->dropRef(ns, "all_0_0_0");
    store->renewWatermarkOnce();

    DB::ContentAddressed::CasGcScheduler sched(store, std::chrono::seconds(1), "test::gc", "ca", {});

    const auto h0 = sched.gcHealth();
    EXPECT_FALSE(h0.is_leader);
    EXPECT_FALSE(h0.ever_succeeded);
    EXPECT_EQ(h0.pending_reclaim, 0);
    EXPECT_EQ(h0.wedged_namespace_count, 0u);

    const RoundReport rep = sched.runOneRoundNow(Rec::Trigger::Manual);
    ASSERT_TRUE(rep.acquired_lease);

    const auto h1 = sched.gcHealth();
    EXPECT_TRUE(h1.is_leader);
    EXPECT_TRUE(h1.ever_succeeded);
    EXPECT_EQ(h1.pending_reclaim,
              static_cast<DB::Int64>(rep.condemned) - static_cast<DB::Int64>(rep.redeleted));
    EXPECT_EQ(h1.wedged_namespace_count, 0u);
    EXPECT_LT(h1.last_success_age_seconds, 60u);
}
```

- [ ] **Step 2: Run test, verify it fails**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CasGcHealth.ReflectsLeadershipAndPendingReclaim'`
Expected: FAIL — compile error, `CasGcScheduler::gcHealth()` / `GcHealth` do not exist yet.

- [ ] **Step 3: Implement**

In `CasGcScheduler.h`, add the health type + accessor and the backing atomics (reusing `i_am_leader`):
```cpp
    /// Per-disk GC health for system.content_addressed_mounts (B3): the process-global CurrentMetrics
    /// gauges were clobbered with >= 2 CAS disks. All fields snapshot THIS scheduler's own state;
    /// wedged_namespace_count is read live from the store's ref lanes.
    struct GcHealth
    {
        bool is_leader = false;
        bool ever_succeeded = false;
        Int64 pending_reclaim = 0;             /// cumulative condemned - executed deletes (this process)
        UInt64 last_success_age_seconds = 0;   /// seconds since the last led round (0 if never)
        UInt64 wedged_namespace_count = 0;
    };
    GcHealth gcHealth() const;
```
and in the private members:
```cpp
    std::atomic<Int64> pending_reclaim{0};     /// B3: cumulative condemned - redeleted while leading
    std::atomic<UInt64> last_success_ms{0};    /// B3: steady-clock ms of the last led round; 0 = never
```

In `CasGcScheduler.cpp`, drop the `CurrentMetrics` include + extern block (lines 14-18) and replace the two global gauge writes in `runRoundLogged` (currently `:135-137`) with per-scheduler updates:
```cpp
        const Cas::RoundReport rep = gc.runRegularRound(std::move(on_lease_acquired));
        i_am_leader.store(rep.acquired_lease, std::memory_order_relaxed);   /// B3: feeds gcHealth on both paths
        if (rep.acquired_lease)
        {
            pending_reclaim.fetch_add(
                static_cast<Int64>(rep.condemned) - static_cast<Int64>(rep.redeleted),
                std::memory_order_relaxed);
            last_success_ms.store(
                static_cast<UInt64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()),
                std::memory_order_relaxed);
        }
```
and define the accessor:
```cpp
CasGcScheduler::GcHealth CasGcScheduler::gcHealth() const
{
    GcHealth h;
    h.is_leader = i_am_leader.load(std::memory_order_relaxed);
    h.pending_reclaim = pending_reclaim.load(std::memory_order_relaxed);
    const UInt64 last_ms = last_success_ms.load(std::memory_order_relaxed);
    h.ever_succeeded = last_ms != 0;
    if (last_ms != 0)
    {
        const UInt64 now_ms = static_cast<UInt64>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        h.last_success_age_seconds = now_ms > last_ms ? (now_ms - last_ms) / 1000 : 0;
    }
    h.wedged_namespace_count = store->wedgedRefLaneCount();
    return h;
}
```

In `CasStore.h`, add a real (non-`ForTest`) accessor next to `refLaneWedgedForTest`:
```cpp
    /// Number of ref-append lanes currently wedged (an uncertain PUT exhausted its retry budget and
    /// the lane blocks until the same key resolves durable or is conclusively rejected). Per-disk GC
    /// health for system.content_addressed_mounts (B3). O(live tables); takes each runtime state lock.
    size_t wedgedRefLaneCount();
```
In `CasStore.cpp`:
```cpp
size_t Store::wedgedRefLaneCount()
{
    std::vector<std::shared_ptr<RefTableRuntime>> runtimes;
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        runtimes.reserve(ref_tables.size());
        for (const auto & [_, rt] : ref_tables)
            runtimes.push_back(rt);
    }
    size_t wedged = 0;
    for (const auto & rt : runtimes)
    {
        std::lock_guard lock(rt->state_mutex);
        if (rt->wedge.has_value())
            ++wedged;
    }
    return wedged;
}
```

In `ContentAddressedMetadataStorage.h`, add:
```cpp
    /// Per-disk GC health for system.content_addressed_mounts (B3). nullopt when no GC scheduler runs
    /// on this disk (GC disabled, read-only, or not started). Reads gc_scheduler under A7's mutex.
    std::optional<ContentAddressed::CasGcScheduler::GcHealth> gcHealth() const;
```
In `ContentAddressedMetadataStorage.cpp`:
```cpp
std::optional<ContentAddressed::CasGcScheduler::GcHealth> ContentAddressedMetadataStorage::gcHealth() const
{
    std::lock_guard lock(gc_scheduler_mutex);   /// A7
    if (!gc_scheduler)
        return std::nullopt;
    return gc_scheduler->gcHealth();
}
```

In `StorageSystemContentAddressedMounts.cpp`, append the 4 columns to the schema (after `"state"`):
```cpp
        {"state", std::make_shared<DataTypeString>(), "live | expired | terminated | fenced | corrupt."},
        {"is_leader", std::make_shared<DataTypeUInt8>(), "1 if THIS server's GC scheduler currently holds this disk's leadership lease (per-disk; supersedes the retired process-global CasGcIsLeader metric)."},
        {"pending_reclaim", std::make_shared<DataTypeInt64>(), "Cumulative two-phase deletion backlog observed by this process's GC on this disk (condemned entries minus executed exact-token deletes)."},
        {"last_success_age_seconds", std::make_shared<DataTypeUInt64>(), "Seconds since this disk's GC last led a round (0 if it has never led or GC is not running here)."},
        {"wedged_namespace_count", std::make_shared<DataTypeUInt64>(), "Ref-append lanes currently wedged on this disk (an uncertain PUT exhausted its retry budget)."},
```
Add the four builders (`ColumnUInt8`/`ColumnInt64`/`ColumnUInt64`/`ColumnUInt64`), fetch the health once per disk before the mount loop, and insert one value per mount row (per-disk property denormalized across that disk's slots):
```cpp
        const auto health = ca->gcHealth();
        for (const auto & m : mounts)
        {
            // ... existing inserts ...
            col_state->insert(m.state);
            col_is_leader->insert(health ? static_cast<UInt8>(health->is_leader) : UInt8(0));
            col_pending->insert(health ? health->pending_reclaim : Int64(0));
            col_last_success->insert(health ? health->last_success_age_seconds : UInt64(0));
            col_wedged->insert(health ? health->wedged_namespace_count : UInt64(0));
        }
```
and `emplace_back` the four new columns into `res_columns` in the same (after-`state`) order.

Retire the process-global gauges in `CurrentMetrics.cpp` — delete lines 231-232 (`CasGcIsLeader`, `CasGcPendingReclaimEntries`); they have no other references.

- [ ] **Step 4: Run test, verify it passes**
Run: `./src/unit_tests_dbms --gtest_filter='CasGcHealth.*:CasGcLog.*'`
Expected: PASS (the retired-gauge externs are gone; `CasGcLog.*` still green). Then add and run the stateless SQL test above under the CA-default job.

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Storages/System/StorageSystemContentAddressedMounts.cpp \
        src/Common/CurrentMetrics.cpp \
        src/Disks/tests/gtest_cas_gc_log.cpp \
        tests/queries/0_stateless/03XXX_content_addressed_mounts_gc_health.sql \
        tests/queries/0_stateless/03XXX_content_addressed_mounts_gc_health.reference
git commit -m "cas: per-disk GC health on system.content_addressed_mounts (B3)

Expose is_leader / pending_reclaim / last_success_age_seconds /
wedged_namespace_count per CAS disk via CasGcScheduler::gcHealth and
Store::wedgedRefLaneCount, and retire the two process-global CurrentMetrics
gauges (CasGcIsLeader / CasGcPendingReclaimEntries) that were clobbered with
>= 2 CAS disks.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task B4: `CasRefLatePredecessorObserved` counter for the ref-loss grace window

**Files:**
- Modify: `.../ContentAddressed/Core/CasStore.cpp:36-51` (extern block), `:1780` (the `snapshot_min_log_age_ms` grace check in `trySnapshotPublishOnce`)
- Modify: `src/Common/ProfileEvents.cpp:772` (add the event to the CA block) **[Ring 1]**
- Test: `src/Disks/tests/gtest_cas_ref_writer.cpp` (Modify — fault-inject a young tail via the fake clock, assert the counter)

**Interfaces:**
- Consumes: `Store::trySnapshotPublishOnce`, `PoolConfig::snapshot_min_log_age_ms`, `PoolConfig::boot_ms_fn`
- Produces: `ProfileEvents::CasRefLatePredecessorObserved`

Semantics: the residual Late-Predecessor-PUT ref-loss race is mitigated only by the min-log-age grace window (a candidate snapshot never covers a tail region younger than `snapshot_min_log_age_ms`). Increment once per `trySnapshotPublishOnce` call in which that grace window forces the early `break` — i.e. each time the window is *engaged* — so the residual race is measurable instead of only mitigated. Deterministically testable via the fake-clock (`boot_ms_fn`) fixture.

- [ ] **Step 1: Write the failing test**

In `gtest_cas_ref_writer.cpp`, add `extern const Event CasRefLatePredecessorObserved;` to the `namespace ProfileEvents { … }` block (near `CasRefSnapshotTailLogs`), then add:
```cpp
/// B4: the Late-Predecessor-PUT observability counter. The min-log-age grace window holds a young
/// tail region out of a candidate snapshot so a late predecessor append from a fenced epoch can
/// still land before a snapshot covers it; each engaged window must be counted exactly once. Uses
/// the fake-clock fixture (same as GraceAgeRespectedYoungLogNotCovered) for a deterministic age.
TEST(RefWriterSnapshotPublish, LatePredecessorCounterCountsGraceWindowHoldback)
{
    using ProfileEvents::global_counters;
    auto backend = std::make_shared<RefWriterTestBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/late_pred_counter"};
    uint64_t fake_now = 1000;

    PoolConfig config;
    config.snapshot_min_log_age_ms = 60000;
    config.boot_ms_fn = [&fake_now] { return fake_now; };
    config.mount_lease_ttl_ms = std::chrono::milliseconds(10'000'000);   /// keep the write fence open
    auto store = openStoreWithConfig(backend, config);

    publishEmptyPart(store, ns, "a");   /// tail stamped observed_at_ms = 1000 (younger than 60s)

    const auto before = global_counters[ProfileEvents::CasRefLatePredecessorObserved].load();
    EXPECT_FALSE(store->trySnapshotPublishOnce(ns));   /// young tail => grace window engages
    EXPECT_EQ(global_counters[ProfileEvents::CasRefLatePredecessorObserved].load(), before + 1)
        << "an engaged grace window must be counted exactly once per attempt";

    fake_now += 61000;   /// past the grace window: nothing is held back
    EXPECT_TRUE(store->trySnapshotPublishOnce(ns));
    EXPECT_EQ(global_counters[ProfileEvents::CasRefLatePredecessorObserved].load(), before + 1)
        << "no young entry => no holdback => counter unchanged";
}
```

- [ ] **Step 2: Run test, verify it fails**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='RefWriterSnapshotPublish.LatePredecessorCounterCountsGraceWindowHoldback'`
Expected: FAIL — compile error, `ProfileEvents::CasRefLatePredecessorObserved` is undeclared.

- [ ] **Step 3: Implement**

In `ProfileEvents.cpp`, add to the CA block right after `CasRefSnapshotPublishBackoff` (`:772`):
```cpp
    M(CasRefLatePredecessorObserved, "CA writer: snapshot-publish attempts where the min-log-age grace window held back a young tail region — the residual Late-Predecessor-PUT ref-loss race window is engaged (spec §Late Predecessor PUT / B4)", ValueType::Number) \
```
In `CasStore.cpp`, add the extern to the `namespace ProfileEvents` block (after `CasRefSnapshotTailLogs`):
```cpp
    extern const Event CasRefLatePredecessorObserved;
```
and increment at the grace-window `break` inside `trySnapshotPublishOnce` (`:1780`):
```cpp
            const uint64_t age = now >= entry.observed_at_ms ? now - entry.observed_at_ms : 0;
            if (age < config.snapshot_min_log_age_ms)
            {
                /// spec §Late Predecessor PUT: the grace window is holding this (and every younger)
                /// tail entry out of the candidate snapshot so a late-arriving predecessor append from
                /// a fenced epoch can still land before a snapshot covers its region. Count each engaged
                /// window (B4) so the residual race is measurable, not only mitigated. observed_at_ms is
                /// non-decreasing, so exactly one break — hence one increment — per attempt.
                ProfileEvents::increment(ProfileEvents::CasRefLatePredecessorObserved);
                break;
            }
```

- [ ] **Step 4: Run test, verify it passes**
Run: `./src/unit_tests_dbms --gtest_filter='RefWriterSnapshotPublish.*'`
Expected: PASS — the new counter case plus the pre-existing `GraceAgeRespectedYoungLogNotCovered` and the other snapshot-publish cases (the `break` behavior is unchanged; only the counter is added).

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Common/ProfileEvents.cpp \
        src/Disks/tests/gtest_cas_ref_writer.cpp
git commit -m "cas: CasRefLatePredecessorObserved counter for the grace-window ref-loss race (B4)

The residual Late-Predecessor-PUT ref-loss race is mitigated only by the
min-log-age grace window; the spec's required diagnostic counter was
unimplemented, so the risk was unmeasurable. Increment
ProfileEvents::CasRefLatePredecessorObserved each time trySnapshotPublishOnce's
grace window holds back a young tail region.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Phase 6 — Encapsulation + surface polish

> Land **E1/E2/E3/F1 first** (independent, low-risk Ring-1/hygiene), then **C5 LAST** with focused merge/mutate review. C5 is the one med-risk Ring-2 behavioral touch; it is a pure refactor and revertible in isolation.

### Task E1: Split the GC REBUILD access right and require an explicit disk

**Files:**
- Modify: `src/Access/Common/AccessType.h:351` (append the new right after `SYSTEM_CONTENT_ADDRESSED_GARBAGE_COLLECTION`)
- Modify: `src/Interpreters/InterpreterSystemQuery.cpp:1010` (check the new right), `:2826-2830` (split `getRequiredAccessForDDLOnCluster`), `:2252-2272` (require explicit disk in `runContentAddressedGcRebuild`)
- Modify: `src/Parsers/ParserSystemQuery.cpp:477-493` (make the disk mandatory for REBUILD)
- Modify (optional, comment-only): `src/Parsers/ASTSystemQuery.cpp:274-285` (the `[<disk>]` comment)
- Test: `tests/queries/0_stateless/01271_show_privileges.reference` (Modify — add the new access-type row) and a NEW `0_stateless` privilege + grammar test. **No parser gtest exists**, so the grammar change is covered by the stateless test.

**Interfaces:**
- Consumes: `AccessType::SYSTEM_CONTENT_ADDRESSED_GARBAGE_COLLECTION`, `ASTSystemQuery::Type::CONTENT_ADDRESSED_GC_REBUILD`, `parseQueryWithOnClusterAndTarget(..., SystemQueryTargetType::Disk)`.
- Produces/Removes: adds `AccessType::SYSTEM_CONTENT_ADDRESSED_GC_REBUILD`; REBUILD now requires that privilege + an explicit disk; removes the all-disks REBUILD broadcast loop.

- [ ] **Step 1: Write the failing/characterization test**

Add via `./tests/queries/0_stateless/add-test <name>.sh` a shell test with this exact intent:
```sh
#!/usr/bin/env bash
# Intent (E1):
#  1) A role granted only "SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION" is REFUSED (ACCESS_DENIED)
#     when it runs "SYSTEM CONTENT ADDRESSED GC REBUILD <disk>", but ALLOWED to run the per-round
#     "SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION". Granting the new
#     "SYSTEM CONTENT ADDRESSED GC REBUILD" right then permits REBUILD.
#  2) "SYSTEM CONTENT ADDRESSED GC REBUILD" with NO disk is a SYNTAX_ERROR (required disk);
#     naming a non-content-addressed disk yields BAD_ARGUMENTS (not a silent all-disks fan-out).
# (No CA disk needs to exist: the privilege check and the grammar/required-disk check both fire
#  before any disk I/O; assert on the specific error codes.)
```
Also add one row to `tests/queries/0_stateless/01271_show_privileges.reference` immediately after the existing `SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION` line (161):
```
SYSTEM CONTENT ADDRESSED GC REBUILD	['SYSTEM CONTENT ADDRESSED GC REBUILD']	GLOBAL	SYSTEM
```

- [ ] **Step 2: Run, verify state**
Run the new stateless test against a local build: it FAILS on `HEAD` (REBUILD accepts the per-round grant and a bare `… GC REBUILD`); `01271_show_privileges` FAILS until the reference row is added.

- [ ] **Step 3: Implement**

`AccessType.h:351` — append the new right (sibling of GC under `SYSTEM`):
```cpp
    M(SYSTEM_CONTENT_ADDRESSED_GARBAGE_COLLECTION, "SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION", GLOBAL, SYSTEM) \
    M(SYSTEM_CONTENT_ADDRESSED_GC_REBUILD, "SYSTEM CONTENT ADDRESSED GC REBUILD", GLOBAL, SYSTEM) \
```
`InterpreterSystemQuery.cpp:1010` — check the new right in the `CONTENT_ADDRESSED_GC_REBUILD` case:
```cpp
        case Type::CONTENT_ADDRESSED_GC_REBUILD:
        {
            getContext()->checkAccess(AccessType::SYSTEM_CONTENT_ADDRESSED_GC_REBUILD);
            runContentAddressedGcRebuild(query.disk, query.content_addressed_gc_rebuild_force);
            break;
        }
```
`InterpreterSystemQuery.cpp:2826-2830` — split the required-access cases:
```cpp
        case Type::CONTENT_ADDRESSED_GARBAGE_COLLECTION:
        {
            required_access.emplace_back(AccessType::SYSTEM_CONTENT_ADDRESSED_GARBAGE_COLLECTION);
            break;
        }
        case Type::CONTENT_ADDRESSED_GC_REBUILD:
        {
            required_access.emplace_back(AccessType::SYSTEM_CONTENT_ADDRESSED_GC_REBUILD);
            break;
        }
```
`InterpreterSystemQuery.cpp:2252-2272` — require an explicit disk; delete the all-disks loop:
```cpp
    /// REBUILD requires an EXPLICIT disk (E1): the destructive baseline rebuild must never fan out
    /// across every content-addressed disk on the node. The parser enforces this syntactically; this is
    /// the fail-closed backstop for a directly-constructed AST.
    if (disk_name.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "SYSTEM CONTENT ADDRESSED GC REBUILD requires an explicit disk name");

    auto disk = getContext()->getDisk(disk_name);
    auto * ca = content_addressed_storage_of(disk);
    if (!ca)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Disk '{}' is not a content-addressed disk", disk_name);
    log_and_check(disk_name, ca->runGcRebuildNow(force));   /// synchronous, one rebuild
```
`ParserSystemQuery.cpp:477-493` — drop the empty-disk fallback so the disk is mandatory:
```cpp
        case Type::CONTENT_ADDRESSED_GC_REBUILD:
        {
            /// SYSTEM CONTENT ADDRESSED GC REBUILD [FORCE] <disk> [ON CLUSTER cluster]. Unlike the
            /// per-round GC-collection command, REBUILD requires an EXPLICIT disk: the destructive
            /// baseline rebuild must never fan out across every content-addressed disk from a bare
            /// command. parseQueryWithOnClusterAndTarget requires the target, so omitting the disk is a
            /// syntax error.
            res->content_addressed_gc_rebuild_force = ParserKeyword{Keyword::FORCE}.ignore(pos, expected);
            if (!parseQueryWithOnClusterAndTarget(res, pos, expected, SystemQueryTargetType::Disk))
                return false;
            break;
        }
```
(Optional) `ASTSystemQuery.cpp:274-285` — update the `[<disk>]` comment to say the disk is required; the `if (!disk.empty())` print stays correct.

- [ ] **Step 4: Run, verify green**
Run: build, then the new stateless privilege/grammar test + `01271_show_privileges`.
Expected: PASS — the GC-only role is refused REBUILD and allowed GC; REBUILD without a disk is `SYNTAX_ERROR`; `01271_show_privileges` matches. (No `unit_tests_dbms` target applies; state this in the report.)

- [ ] **Step 5: Commit**
```bash
git add src/Access/Common/AccessType.h \
        src/Interpreters/InterpreterSystemQuery.cpp \
        src/Parsers/ParserSystemQuery.cpp \
        src/Parsers/ASTSystemQuery.cpp \
        tests/queries/0_stateless/01271_show_privileges.reference \
        tests/queries/0_stateless/<new_test>.sh tests/queries/0_stateless/<new_test>.reference
git commit -m "cas: split SYSTEM CONTENT ADDRESSED GC REBUILD access right and require an explicit disk

The destructive baseline rebuild now needs its own SYSTEM_CONTENT_ADDRESSED_GC_REBUILD
privilege, distinct from the benign per-round garbage-collection right, and requires an
explicit disk (no all-disks broadcast). Removes the all-disks REBUILD loop.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task E2: Config-key and naming fixes in `registerContentAddressedMetadataStorage`

**Files:**
- Modify: `.../ContentAddressed/ContentAddressedMetadataStorage.h:79,286` and `.cpp:143,172,350` (rename member/param `gc_max_conditional_put_bytes_` → `gcs_max_conditional_put_bytes_`; the config-key string at `MetadataStorageFactory.cpp:291` is already `gcs_max_conditional_put_bytes`)
- Modify: `.../MetadataStorages/MetadataStorageFactory.cpp:282-284` (missing-`server_root_id` → `DB::Exception(NO_ELEMENTS_IN_CONFIG, …)`), `:295-297` (drop `cas_` prefix on the 3 part-folder-cache keys)
- Modify: `.../ContentAddressed/ContentAddressedMetadataStorage.cpp:187,193,459` (rename config key `cas_staging_backend` → `staging_backend` + its two message/comment mentions)
- Test: `src/Disks/tests/gtest_cas_s3_staging.cpp:239,246,254` (Modify — the `parseStagingBackend` cases)

**Interfaces:**
- Consumes: `ErrorCodes::NO_ELEMENTS_IN_CONFIG` (already declared `MetadataStorageFactory.cpp:24`), the `metadata_type` fail-closed idiom (`:94-96`), `ContentAddressedMetadataStorage::parseStagingBackend`.
- Produces/Removes: renames one class member + 4 user-facing config keys; converts a raw `Poco::NotFoundException` into a typed `DB::Exception`. **Prefix rule (document in the factory comment):** the `content_addressed` block already scopes every key to the disk, so NO key carries a redundant `cas_`/`ca_` prefix. `gcs_` in `gcs_max_conditional_put_bytes` is a *semantic* descriptor (GCS generation stores), not a namespace prefix, so it stays. `root_shards` default alignment is moot (removed by D1). No legacy-key aliases (pre-release).

- [ ] **Step 1: Write the failing/characterization test**

Retarget the existing `parseStagingBackend` gtests to the new key (`gtest_cas_s3_staging.cpp`):
```cpp
TEST(CasS3Staging, ParsesS3BackendFromConfig)
{
    auto config = configWithDiskSection("<staging_backend>s3</staging_backend>");
    EXPECT_EQ(DB::ContentAddressedMetadataStorage::parseStagingBackend(*config, "disk"), DB::StagingBackend::S3);
}

TEST(CasS3Staging, DefaultConfigParsesToLocalBackend)
{
    /// No `staging_backend` key at all — the OFF BY DEFAULT arm.
    auto config = configWithDiskSection("<scratch_path>/tmp/whatever</scratch_path>");
    EXPECT_EQ(DB::ContentAddressedMetadataStorage::parseStagingBackend(*config, "disk"), DB::StagingBackend::Local);
}

TEST(CasS3Staging, UnknownBackendValueThrows)
{
    auto config = configWithDiskSection("<staging_backend>nfs</staging_backend>");
    EXPECT_THROW(DB::ContentAddressedMetadataStorage::parseStagingBackend(*config, "disk"), DB::Exception);
}
```

- [ ] **Step 2: Run, verify state**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='CasS3Staging.ParsesS3BackendFromConfig:CasS3Staging.UnknownBackendValueThrows'`
Expected: `ParsesS3BackendFromConfig` FAILS on `HEAD` — `parseStagingBackend` still reads `cas_staging_backend`, so `<staging_backend>s3</>` parses to the default `Local`. (The member rename and the `server_root_id` throw are compile/build-gated.)

- [ ] **Step 3: Implement**

Member rename (5 spots): `ContentAddressedMetadataStorage.h:79` field, `:286` const member; `.cpp:143` ctor param, `:172` init-list, `:350` use — all `gc_max_conditional_put_bytes_`/`gc_max_conditional_put_bytes` → `gcs_…`. (The factory passes positionally; no change there.)

`MetadataStorageFactory.cpp:282-284` — typed missing-key error mirroring the `metadata_type` check:
```cpp
        if (!config.has(config_prefix + ".server_root_id"))
            throw Exception(ErrorCodes::NO_ELEMENTS_IN_CONFIG,
                "Expected `server_root_id` in config for a content-addressed disk");
        const std::string server_root_id = global_context->getMacros()->expand(
            config.getString(config_prefix + ".server_root_id"));
        Cas::validateServerRootId(server_root_id);
```
`MetadataStorageFactory.cpp:295-297` — drop the `cas_` prefix + document the rule:
```cpp
        /// Config-key convention: the `content_addressed` block already scopes every key to this disk,
        /// so no key carries a redundant `cas_`/`ca_` prefix.
        const uint64_t part_folder_cache_bytes = config.getUInt64(config_prefix + ".part_folder_cache_bytes", 64ULL << 20);
        const uint64_t part_folder_cache_max_entries = config.getUInt64(config_prefix + ".part_folder_cache_max_entries", 10000);
        const uint64_t part_folder_cache_max_entry_bytes = config.getUInt64(config_prefix + ".part_folder_cache_max_entry_bytes", 16ULL << 20);
```
(Update the three local var names threaded into the ctor call at `:313`. Internal class member names `cas_part_folder_cache_*` may stay; only the user-facing key strings + factory locals change.)

`ContentAddressedMetadataStorage.cpp:187` + messages `:193,:459` — rename the staging key:
```cpp
    const std::string value = config.getString(config_prefix + ".staging_backend", "local");
```
and update the two diagnostic strings (`:193`, `:459`) + the `cas_staging_backend` comment mentions.

- [ ] **Step 4: Run, verify green**
Run: `./src/unit_tests_dbms --gtest_filter='CasS3Staging.*'`
Expected: PASS. Full `ninja unit_tests_dbms` clean (confirms the member rename compiles everywhere).

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/tests/gtest_cas_s3_staging.cpp
git commit -m "cas: config-key and naming fixes for the content-addressed metadata storage

Rename member gc_max_conditional_put_bytes_ -> gcs_..., turn a missing server_root_id
into a typed NO_ELEMENTS_IN_CONFIG exception, and drop the redundant cas_ prefix from the
staging_backend / part_folder_cache_* keys (the config block already scopes them).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task E3: System-surface polish (typed mounts columns + system-log comment)

**Files:**
- Modify: `src/Storages/System/StorageSystemContentAddressedMounts.cpp:36,41,42` (column types), `:70,77,78` (construction), `:137,142,143` (insertion), plus includes
- Modify: `src/Interpreters/SystemLog.h:21` (the `ContentAddressedLog` description — reconcile with the config)
- Test: no gtest exists for this table; verify via build + a `SELECT ... FROM system.columns` stateless check. The `SystemLog.h`/`config.xml` reconciliation is comment-only.

> Coordination with B3: B3 also edits `StorageSystemContentAddressedMounts.cpp` (adds 4 GC-health columns). If B3 lands first, apply E3's type changes to the existing `server_uuid`/`started_at_ms`/`expires_at_ms` columns without disturbing B3's new ones.

**Interfaces:**
- Consumes: `Cas::MountInfo::lease.server_uuid` (`DB::UInt128`), `.started_at_ms`/`.expires_at_ms` (unix ms `uint64`), `ColumnUUID`, `ColumnDateTime64`, `DataTypeUUID`, `DataTypeDateTime64`.
- Produces/Removes: types `server_uuid` as `UUID`, `started_at_ms`/`expires_at_ms` as `DateTime64(3)`; drops the now-unused `getHexUIntLowercase`/`base/hex.h`. **Reconciliation: fix the COMMENT, not the config.** `config.xml:1162-1172` ships `<content_addressed_log>` enabled by default (experimental feature); `SystemLog.h:21` wrongly says "Optional, off by default" — correct the description.

- [ ] **Step 1: Write the failing/characterization test**

Add (via `add-test`) a `0_stateless` `.sql` test pinning the types:
```sql
-- Intent (E3): the mounts table exposes typed columns.
SELECT type FROM system.columns
WHERE database='system' AND table='content_addressed_mounts'
  AND name IN ('server_uuid','started_at_ms','expires_at_ms')
ORDER BY name;
-- Expected reference:
-- DateTime64(3)   -- expires_at_ms
-- UUID            -- server_uuid
-- DateTime64(3)   -- started_at_ms
```

- [ ] **Step 2: Run, verify state**
Run the stateless `.sql` against a local build.
Expected: FAILS on `HEAD` — the columns report `String`, `UInt64`, `UInt64`.

- [ ] **Step 3: Implement**

`StorageSystemContentAddressedMounts.cpp` — add includes:
```cpp
#include <DataTypes/DataTypeUUID.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <Columns/ColumnsDateTime.h>
```
Column type declarations (`:36,41,42`):
```cpp
        {"server_uuid", std::make_shared<DataTypeUUID>(), "UUID of the server incarnation holding the lease."},
        // ...
        {"started_at_ms", std::make_shared<DataTypeDateTime64>(3), "Lease start."},
        {"expires_at_ms", std::make_shared<DataTypeDateTime64>(3), "Lease expiry."},
```
Column construction (`:70-81` region):
```cpp
    auto col_uuid = ColumnUUID::create();
    // ...
    auto col_started = ColumnDateTime64::create(0, 3);
    auto col_expires = ColumnDateTime64::create(0, 3);
```
Value insertion (`:137,142,143`), mirroring `StorageSystemBackups`' DateTime64 idiom:
```cpp
            col_uuid->insertValue(UUID(m.lease.server_uuid));
            // ...
            col_started->insertValue(static_cast<Decimal64>(m.lease.started_at_ms));
            col_expires->insertValue(static_cast<Decimal64>(m.lease.expires_at_ms));
```
Drop the now-unused `#include <base/hex.h>`.

`SystemLog.h:21` — reconcile the description with the enabled-by-default config:
```cpp
    M(ContentAddressedLog, content_addressed_log, "Per-event content-addressed (CA) MergeTree audit log: one row per blob/tree/ref/GC decision (put, reuse, retire, delete, root add/remove, in-degree-zero, fence, lease, ...) plus errors (dangling access, fail-closed). Enabled by default while the CA disk feature is experimental (see config.xml); it is the primary forensic instrument for a CA issue and costs nothing when no CA disk is configured.") \
```
(Optional verb-first alias for the GC SYSTEM command: **deferred** — adding an alias grows the grammar surface for no consumer, conflicting with the "no pre-emptive generality" non-goal.)

- [ ] **Step 4: Run, verify green**
Run: the stateless `system.columns` test + `ninja` build.
Expected: PASS — the three columns report `UUID`, `DateTime64(3)`, `DateTime64(3)`.

- [ ] **Step 5: Commit**
```bash
git add src/Storages/System/StorageSystemContentAddressedMounts.cpp \
        src/Interpreters/SystemLog.h \
        tests/queries/0_stateless/<new_test>.sql tests/queries/0_stateless/<new_test>.reference
git commit -m "cas: type system.content_addressed_mounts columns and fix the CA log doc comment

server_uuid -> UUID, started_at_ms/expires_at_ms -> DateTime64(3). Reconcile the
SystemLog.h ContentAddressedLog description with the enabled-by-default config.xml section.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task F1: Delete the orphaned `poc/cas_mergetree/` PoC

**Files:**
- Remove: `poc/cas_mergetree/` (git-tracked: `.gitignore`, `CMakeLists.txt`, `README.md`, `cas.cpp`, `cas.h`, `tests.cpp` — plus untracked `build/`, `cas_poc_scratch/`)
- Test: none (pure deletion; superseded by `.../ContentAddressed/Core/`)

**Interfaces:**
- Removes: 1463 lines of a standalone PoC with generic class names (`GC`/`Engine`/`Catalog`) that pollute symbol search. Confirmed by grep: no file outside the directory references `cas_mergetree`; the top-level `CMakeLists.txt` does not add `poc/`.

- [ ] **Step 1: Confirm zero references**
```bash
grep -rn "cas_mergetree" . --include=*.cpp --include=*.h --include=*.txt --include=*.cmake --include=*.md --include=*.py --include=*.sh | grep -v "poc/cas_mergetree/"
# -> (no output)
grep -rn "poc" CMakeLists.txt   # -> (no output)
```

- [ ] **Step 2: Verify what will be removed**
Run: `git ls-files poc/cas_mergetree/`
Expected: lists exactly the 6 tracked files; nothing else depends on them.

- [ ] **Step 3: Implement**
```bash
git rm -r poc/cas_mergetree
rm -rf poc/cas_mergetree   # clear untracked build/ and cas_poc_scratch/ if the dir lingers
```

- [ ] **Step 4: Verify green**
```bash
git status --porcelain poc/          # -> the 6 files staged as deleted, nothing else
grep -rn "cas_mergetree" . --include=*.cpp --include=*.h --include=*.cmake --include=CMakeLists.txt | grep -v "poc/cas_mergetree/"   # -> (no output)
```
Expected: no dangling references; no build target touched (the PoC had its own standalone `CMakeLists.txt`, never included by the main build).

- [ ] **Step 5: Commit**
```bash
git add -A poc/
git commit -m "cas: delete the orphaned poc/cas_mergetree standalone PoC

Superseded by the real Core/ implementation; referenced by nothing. Its generic class
names (GC/Engine/Catalog) polluted symbol search.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task C5: Encapsulate the whole-part-transaction rule on borrowed projection storage  [Ring 2 — LAND LAST]

**Files:**
- Modify: `src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp:347-366` (the single encapsulation point — `beginTransaction`/`commitTransaction`)
- Modify: `src/Storages/MergeTree/MergeTask.cpp:590-591` (remove begin guard), `:1324-1325` (remove commit guard), `:1368-1369` (remove commit guard)
- Modify: `src/Storages/MergeTree/MutateTask.cpp:1820-1821` (remove commit guard)
- Modify: `src/Storages/MergeTree/MergeProjectionPartsTask.cpp:136-137` (remove commit guard)
- Modify: `src/Storages/MergeTree/MergeTreeDataWriter.cpp:1036-1037` (drop the `isContentAddressed()` conjunct, keep `is_temp`)
- Test: `src/Storages/MergeTree/tests/gtest_projection_borrowed_transaction.cpp` (Create)

**Interfaces:**
- Consumes: `DataPartStorageOnDiskBase::has_shared_transaction` (set in the 4-arg ctor `DataPartStorageOnDiskBase.cpp:54-60` as `transaction != nullptr`; the 4-arg ctor is reached ONLY via `DataPartStorageOnDiskFull::getProjection(name, use_parent_transaction=true)` at `DataPartStorageOnDiskFull.cpp:41-44`), `DataPartStorageOnDiskBase::hasActiveTransaction` (`.cpp:1048-1050`, `return transaction != nullptr`).
- Produces/Removes: removes the 6 `if (!isContentAddressed()) begin/commitTransaction()` conditionals; moves the decision into `beginTransaction`/`commitTransaction` (no-op when `has_shared_transaction`). No signature changes. **The two getProjection-decision sites — `MergeTask.cpp:566` (`projection_uses_parent_transaction = …isContentAddressed()`) and `IMergeTreeDataPart.cpp:1350` (`use_parent_transaction = !is_temp_projection || …isContentAddressed()`) — STAY unchanged: they correctly produce the borrowed storage; they are not begin/commit guards.**

> **LAND LAST, revertible independently.** The only Ring-2 behavioral touch to the merge/mutate hot path. Pure refactor: on a CA disk the projection storage already rode the parent transaction (the guards skipped begin/commit); on a non-CA disk the temp projection already owned its sub-transaction (`has_shared_transaction == false`). The no-op only fires for borrowed projection storage (`has_shared_transaction == true` iff the storage came from `getProjection(..., true)`), so all other begin/commit callers (whole parts, fetched parts, text-index temp storage) are untouched. If it destabilizes, revert this one commit.

- [ ] **Step 1: Write the failing/characterization test**

Create `src/Storages/MergeTree/tests/gtest_projection_borrowed_transaction.cpp`:
```cpp
#include <gtest/gtest.h>

#include <Disks/DiskLocal.h>
#include <Disks/SingleDiskVolume.h>
#include <Storages/MergeTree/DataPartStorageOnDiskFull.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

using namespace DB;

namespace
{
    /// A DiskLocal-backed parent part storage. `DiskLocal::createTransaction` yields a real
    /// transaction object, which is all `beginTransaction` needs to hand a NON-NULL transaction to a
    /// borrowed projection sub-part (the `has_shared_transaction == true` case).
    struct ParentStorageFixture
    {
        std::filesystem::path base_path;
        DiskPtr disk;
        VolumePtr volume;
        MutableDataPartStoragePtr parent;

        ParentStorageFixture()
        {
            const auto unique = std::to_string(::getpid()) + "_"
                + std::to_string(reinterpret_cast<uintptr_t>(this));
            base_path = std::filesystem::temp_directory_path() / ("proj_txn_gtest_" + unique);
            std::filesystem::create_directories(base_path / "all_1_1_0");
            disk = std::make_shared<DiskLocal>("test_disk_" + unique, base_path.string());
            volume = std::make_shared<SingleDiskVolume>("test_volume", disk);
            parent = std::make_shared<DataPartStorageOnDiskFull>(volume, /*root_path=*/"", "all_1_1_0");
        }

        ~ParentStorageFixture()
        {
            std::error_code ec;
            std::filesystem::remove_all(base_path, ec);
        }
    };
}

/// A projection sub-part that BORROWS the parent's whole-part transaction (the CA-disk shape:
/// getProjection(..., use_parent_transaction = true)) must let begin/commit be NO-OPS — it rides the
/// parent's single commit. Before the encapsulation this threw "Uncommitted shared transaction already
/// exists" / "Cannot commit shared transaction", forcing every caller to branch on isContentAddressed().
TEST(ProjectionBorrowedTransaction, BorrowedStorageBeginCommitAreNoOps)
{
    ParentStorageFixture fx;

    /// Parent opens the whole-part transaction (as MergeTask/writer do for a CA part).
    fx.parent->beginTransaction();
    ASSERT_TRUE(fx.parent->hasActiveTransaction());

    /// Borrowed projection sub-part: shares the parent transaction (has_shared_transaction == true).
    auto proj = fx.parent->getProjection("p.proj", /*use_parent_transaction=*/true);
    EXPECT_TRUE(proj->hasActiveTransaction());

    /// The encapsulated rule: begin/commit on the borrowed storage are silent no-ops (they must NOT
    /// open a second transaction, nor commit the parent's).
    EXPECT_NO_THROW(proj->beginTransaction());
    EXPECT_NO_THROW(proj->commitTransaction());

    /// The parent's transaction is untouched by the projection's no-ops and still commits cleanly.
    EXPECT_TRUE(fx.parent->hasActiveTransaction());
    EXPECT_NO_THROW(fx.parent->commitTransaction());
    EXPECT_FALSE(fx.parent->hasActiveTransaction());
}

/// The non-CA temp-projection shape (use_parent_transaction = false) is unchanged: the sub-part OWNS
/// its transaction, so begin creates it and commit commits it (has_shared_transaction == false, so the
/// no-op path never triggers).
TEST(ProjectionBorrowedTransaction, OwnedProjectionStorageStillBeginsAndCommits)
{
    ParentStorageFixture fx;

    auto proj = fx.parent->getProjection("q.proj", /*use_parent_transaction=*/false);
    EXPECT_FALSE(proj->hasActiveTransaction());

    EXPECT_NO_THROW(proj->beginTransaction());
    EXPECT_TRUE(proj->hasActiveTransaction());
    EXPECT_NO_THROW(proj->commitTransaction());
    EXPECT_FALSE(proj->hasActiveTransaction());
}
```

- [ ] **Step 2: Run, verify state**
Run: `cd build && ninja unit_tests_dbms && ./src/unit_tests_dbms --gtest_filter='ProjectionBorrowedTransaction.*'`
Expected: `BorrowedStorageBeginCommitAreNoOps` FAILS on current `HEAD` — `proj->beginTransaction()` throws `LOGICAL_ERROR` "Uncommitted shared transaction already exists" and `proj->commitTransaction()` throws "Cannot commit shared transaction". `OwnedProjectionStorageStillBeginsAndCommits` passes.

- [ ] **Step 3: Implement**

Encapsulation point — `DataPartStorageOnDiskFull.cpp:347-366`:
```cpp
void DataPartStorageOnDiskFull::beginTransaction()
{
    /// A borrowed projection sub-part shares the PARENT part's whole-part transaction (on a
    /// content-addressed disk a part is one atomic unit: one manifest + one ref). It must not open its
    /// own — riding the parent transaction is the point (B58) — so begin is a no-op here. This
    /// centralizes the rule the 6 merge/mutate call sites used to duplicate as
    /// `if (!isContentAddressed()) beginTransaction()`.
    if (has_shared_transaction)
        return;

    if (transaction)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Uncommitted transaction already exists");

    transaction = volume->getDisk()->createTransaction();
}

void DataPartStorageOnDiskFull::commitTransaction()
{
    /// The mirror of beginTransaction: a borrowed projection sub-part rides the parent's transaction and
    /// is published by the parent's single commit. Committing here would be committing someone else's
    /// transaction, so it is a no-op.
    if (has_shared_transaction)
        return;

    if (!transaction)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "There is no uncommitted transaction");

    transaction->commit();
    transaction.reset();
}
```
The 6 removed guard sites (callers now call begin/commit unconditionally; the storage decides):
1. `MergeTask.cpp:590-591` (begin) → `data_part_storage->beginTransaction();`
2. `MergeTask.cpp:1324-1325` (commit) → `tmp_part->part->getDataPartStorage().commitTransaction();`
3. `MergeTask.cpp:1368-1369` (commit) → `temp_part->part->getDataPartStorage().commitTransaction();`
4. `MutateTask.cpp:1820-1821` (commit) → `tmp_part->part->getDataPartStorage().commitTransaction();`
5. `MergeProjectionPartsTask.cpp:136-137` (commit) → `next_level_parts.back()->getDataPartStorage().commitTransaction();`
6. `MergeTreeDataWriter.cpp:1036-1037` (begin — keep `is_temp`, drop the `isContentAddressed()` conjunct):
```cpp
    /// A temp projection sub-part opens a transaction only if it owns one; a borrowed (CA) projection
    /// storage makes beginTransaction a no-op, so the `isContentAddressed()` branch is no longer needed.
    if (is_temp)
        projection_part_storage->beginTransaction();
```
(Delete each now-stale `if (!…->getDataPartStorage().isContentAddressed())` line above sites 2-5 and the surrounding comment fragments that only explained the removed branch. Leave the `MergeTask.cpp:566` and `IMergeTreeDataPart.cpp:1350` getProjection-decision lines untouched.)

- [ ] **Step 4: Run, verify green**
Run: `./src/unit_tests_dbms --gtest_filter='ProjectionBorrowedTransaction.*'`
Expected: PASS (both tests). Also `ninja unit_tests_dbms` clean. **Careful manual review of the merge/mutate diff required** (spec §10 flags C5 as the one med-risk item). The end-to-end gate is the CA-default stateless projection suites (`0_stateless/*projection*`, out of C++ scope) — note them for the CA-default stateless run.

- [ ] **Step 5: Commit**
```bash
git add src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp \
        src/Storages/MergeTree/MergeTask.cpp \
        src/Storages/MergeTree/MutateTask.cpp \
        src/Storages/MergeTree/MergeProjectionPartsTask.cpp \
        src/Storages/MergeTree/MergeTreeDataWriter.cpp \
        src/Storages/MergeTree/tests/gtest_projection_borrowed_transaction.cpp
git commit -m "cas: encapsulate whole-part-transaction rule on borrowed projection storage

Make DataPartStorageOnDiskFull begin/commitTransaction no-ops when the storage
shares a parent transaction (has_shared_transaction), removing the 6 duplicated
'if (!isContentAddressed()) begin/commitTransaction()' guards across MergeTask,
MutateTask, MergeProjectionPartsTask and MergeTreeDataWriter. Pure refactor; lands
last and is revertible in isolation.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Self-review

**Spec coverage** (spec §4 item → plan task): A1→A1, A2→A2, A3→A3, A4→A4, A5→A5, A6→A6, A7→A7, A8→A8, A9→A9, A10→A10, B1→B1, B2→B2, B3→B3, B4→B4, C1→C1, C2→C2, C3→C3, C4→C4, C5→C5, D1→D1(a–g), D2→D2, D3→D3, D4→D4, D5→D5, E1→E1, E2→E2, E3→E3, F1→F1; spec §5 Ring-2-shrink items → R412, RExpect (copyS3File `message_format_string` folded into Group G / not a separate task — see below). All spec §4/§5 items have a task.

**Deviations from the spec, discovered by reading HEAD (all documented in-task):**
- `CasEvent::round` is **not** removed (D2) — it is a live serialized `system.content_addressed_log.round` column (spec premise refuted).
- `ObjectKind` is **not** removed (D1f) — ~100 consumers; a refactor, not a subtraction.
- A5 anchors `detached` left-to-right (not the spec's right-to-left sketch) — right-to-left would hit the inner part dir.
- A6 fixes only the Ring-0 `Store::open` side; the generic `DiskSelector` per-disk isolation stays deferred (Group G).
- A7 uses a dedicated `gc_round_mutex` (not the scheduler `mutex`) so the heartbeat/shutdown aren't blocked.
- **`copyS3File` `message_format_string`** (spec §7 Group G / umbrella-minor): a one-line-per-site revert to `PreformattedMessage::create(...)` at `copyS3File.cpp:68-74,126-138`. Not authored as a standalone task; fold it into the R412 commit (same file family, Ring-2-shrink) or land it as a trivial follow-up. It is listed here so it is not lost.

**Placeholder scan:** no `TBD`/`TODO`/"add error handling"/"similar to Task N"; every code step has real code. The `<new_test>` / `03XXX` placeholders in E1/E3/B3 are literal filename slots for `add-test`-assigned numbers (the test *intent* is fully specified), not missing content.

**Type consistency:** cross-checked the shared symbols that multiple tasks touch — `StorageSystemContentAddressedMounts.cpp` (B3 adds columns, E3 retypes columns: coordination noted), `ContentAddressedMetadataStorage` `gc_scheduler_mutex` (A7 introduces, B3 consumes: noted), `PartPathParser` free functions (A5/B1/C4 all route through them: no signature conflict), `ShardCoverage` fields (A8 initializes, D1d removes: coordination noted), `gtest_cas_dangling_precommit.cpp` (D1d + D5: coordination noted).

## Cross-task coordination summary

- **Phase order matters:** run phases 1→6 in order. Within Phase 3, D1d before D5. A7 (Phase 2) must precede B3's `gcHealth()` mutex use (Phase 5) — it does, by phase order.
- **Shared-file touch coordination:** B3 + E3 both edit `StorageSystemContentAddressedMounts.cpp`; A8 + D1d both touch the `ShardCoverage{...}` decode; D1b + D3 both touch `CasInstrumentedBackend.cpp` (D3 owns the classifier edit). Each is called out in the relevant task.
- **`copyS3File` message-format fix** rides R412's commit (or a trivial follow-up).

## Not in this plan (deferred / parallel track)

- **Deferred refactors:** `CasGc.cpp` split, `Cas::Store` de-god-classing, `DiskSelector` per-disk isolation, `DiskObjectStorageTransaction` part-path virtualization, `removeFileIfExists`+`writeFile` ordering (dead code), `MultipleDisks` `shared_from_this` (latent). (Spec §6.)
- **Parallel track G (non-blocking, shrinks the fork long-term):** upstream the generic Ring-2 fixes separately — `ThreadStatus` B90, `ReadBufferFromFileView` B115, `ReadBufferFromS3` B117, `LocalObjectStorage` TOCTOU, `MergeTreeDeduplicationLog` null-writer, `copyS3File` message-format, `Expect: 100-continue` (as a generic opt-in), `S3Exception::isPreconditionFailed`, GCS dialect/signer. (Spec §7.)
