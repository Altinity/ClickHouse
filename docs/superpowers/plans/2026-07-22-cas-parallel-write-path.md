# CAS Parallel Write Path Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make CAS-on-S3 INSERT commit parts with bounded concurrency so the ref-ledger's batching engages and blob uploads overlap, cutting the measured ~7.6× slowdown — byte-logically-equivalent and invariant-preserving.

**Architecture:** `ContentAddressedTransaction::commit()` dispatches each part's unchanged `publishStaging` (`stageManifest → precommitAdd → uploadPendingBlobs → promote`) onto a **dedicated** CAS-commit thread pool via a bounded set of worker-loop callbacks (disjoint from `getThreadPoolWriter`, so no nested same-pool wait). Blobs stay serial inside a worker. Correctness machinery is hardened FIRST (ref-lane exception-safety, then exact-manifest rollback), THEN concurrency is switched on.

**Tech Stack:** C++ (ClickHouse tree), gtest (`unit_tests_dbms`), `ThreadPool`/`ThreadPoolCallbackRunner` (`src/Common/`), the CAS `ContentAddressed` subsystem.

**Spec:** `docs/superpowers/specs/2026-07-22-cas-parallel-write-path-design.md`

## Global Constraints

- **Logically equivalent, invariant-preserving.** No change to encoded bytes of any object, to the intra-part order `stageManifest → precommitAdd → uploadPendingBlobs → promote` (EDGE-BEFORE-OBSERVE, TLA+ "Gate A"), or to the ref protocol. Determinism is **semantic** (folded ref→manifest bindings, manifest entries, blob payloads, in-degree, clean meta), NOT byte-identical (batching changes ref-log packing / `RefTxnId`s / timestamps / incarnation tags).
- **Pool discipline (hard):** the commit-worker pool MUST be disjoint from `getThreadPoolWriter()`. No worker ever waits on work queued to its own pool.
- **Exception-safe join ordered before rollback:** preallocate every per-part task handle / outcome slot / exception slot BEFORE scheduling; drain via a non-throwing guard that `wait()`s (never `get()`s) on every registered handle; the drain completes BEFORE any rollback. Worker tasks capture **non-owning** references only.
- Allman braces (opening brace on its own line), CI-enforced. Never use `sleep` to fix a race.
- Branch: work on `cas-gc-rebuild` (current). No rebase/amend — new commits only. No push. Not master.
- Build dir: `build/`. Redirect build output to a log in the build dir; use a subagent to summarize build/test logs.
- Unit gate filter (corrected — `Cas*:CA*` alone under-tests):
  `Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:CaDedupCache*:CaTransaction*:CaPartPathParser*:CaWiring*:CaInlinePlacement*`
- Commit trailer: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- No `-j`/`nproc` with ninja.

## File map

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp` / `.h` — ref-lane exception-safety (Task 1); a test-only pre-carve fault hook.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.cpp` / `.h` — `promoteBuild`/`repointRef` return a `CommitOutcome`; new `dropRefIfMatches` (Task 2).
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` / `.h` — `commit()` rollback rework (Task 3) then parallelization (Task 5); `st.build.reset()` after abandon/promote.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.*` (or the metadata-storage settings surface) — `cas_commit_concurrency` setting + dedicated commit pool (Task 4).
- `src/Disks/tests/gtest_cas_ref_lane_exception_safety.cpp`, `gtest_cas_parallel_commit.cpp` — new gtests.

### Test-harness note (read before Task 1)

The new gtests use fixture helpers (`makeRefLedgerFixture`, `makeCaWiringFixture`, `stageSimplePart`,
`commitSimplePart`, `repointToFreshManifest`, `stageInto`, `beginTxn`, `foldedLogicalState`,
`setCommitConcurrency`, etc.) written in the same style as the EXISTING CA harness — read
`src/Disks/tests/gtest_cas_ref_writer.cpp`, `gtest_ca_wiring.cpp`, and `cas_test_helpers.h` first and
reuse their in-process pool/storage setup and counting/emulated backend. Where a test needs a
**fault seam** the harness lacks (`armPromoteFailure`, `armSlowUpload`, `onFirstDropRefIfMatches`,
`testFaultBeforeCarve`), add a test-only hook following Task 1's `testFaultBeforeCarve` pattern: a
`std::function` member on the production object, always compiled, default-empty, invoked at the exact
point, set only by tests. Do NOT weaken production encapsulation beyond that seam. The first task that
needs a given helper defines it in the shared test fixture header; later tasks reuse it. The exact
assertions and the behavior each test pins are given in full below; the fixture plumbing is mechanical
harness code modelled on the cited existing files.

---

### Task 1: Ref-lane exception-safety (the bundled dependency)

Fix the latent `CasRefLedger` use-after-free that concurrent workers activate: a leader that throws BEFORE carving leaves its own item in `rt->pending`; a follower later carves and runs its dangling `[&]` `build_ops` closure. Also capture builder state by value as defense-in-depth. This lands first because it is independent of the rest and is a prerequisite for any concurrent `appendRefOps` caller.

**Files:**
- Modify: `.../Pool/CasRefLedger.cpp` (`appendRefOps` catch path ~L982-995; `runRefQueueLeader`/`flushRefBatch` carve+complete paths), `.../Pool/CasRefLedger.h` (a test-only fault hook).
- Test: `src/Disks/tests/gtest_cas_ref_lane_exception_safety.cpp`

**Interfaces:**
- Produces: an invariant — after any `appendRefOps` return OR throw, no `RefMutationItem` is left in `rt->pending` that is neither `done` nor owned by a still-blocked caller. A test-only injection point `CasRefLedger::testFaultBeforeCarve(std::function<void()>)` (compiled always; no-op unless set) that runs inside `flushRefBatch` immediately before the batch is carved.

- [ ] **Step 1: Read the current lane exception structure**

Read `CasRefLedger.cpp` `appendRefOps` (~L942-1010) and `runRefQueueLeader`/`flushRefBatch` fully. Confirm the two windows: (w1) leader throws before carving → its own `item` is still in `rt->pending`, the catch (~L982-995) resets `leader_active`+notifies+rethrows but does NOT complete/remove `item`; (w2) leader throws after carving but before completing some carved items. Note where items are carved out of `pending` and where `it->done = true` + `cv.notify_all()` happen.

- [ ] **Step 2: Write the failing test**

Create `src/Disks/tests/gtest_cas_ref_lane_exception_safety.cpp`. Use the existing CAS ledger test harness (mirror the setup in `gtest_cas_ref_writer.cpp` — grep it for how a `CasRefLedger`/pool is constructed in-process with a counting/emulated backend). The test drives TWO concurrent `appendRefOps` callers on one namespace, injects a throw before carve on the first leader, and asserts the second caller completes without UAF and the pending queue is drained.

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h>
// ... plus the same includes/harness gtest_cas_ref_writer.cpp uses to build an in-process ledger.
#include <thread>
#include <atomic>

using namespace DB::Cas;

TEST(RefLaneExceptionSafety, LeaderThrowBeforeCarveDoesNotStrandItem)
{
    auto fixture = makeRefLedgerFixture();            // reuse the ref-writer harness factory
    CasRefLedger & ledger = fixture.ledger();
    const RootNamespace ns = fixture.namespaceForTest();

    std::atomic<int> fault_armed{1};
    ledger.testFaultBeforeCarve([&]
    {
        // Throw only on the FIRST leader flush, so caller B (or a re-drive) can proceed.
        if (fault_armed.exchange(0) == 1)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected pre-carve fault");
    });

    // Two concurrent callers on the same namespace: the ledger batches them, one leads.
    std::atomic<int> ok{0};
    auto caller = [&](uint64_t seq)
    {
        try
        {
            ledger.appendRefOps(ns, MutationScope::ref("ref_" + std::to_string(seq)),
                [seq](const RefTableState &) -> std::vector<RefOp> { return { makeNamespaceBirthOp() }; },
                RootMutationOrigin::Test, RootMutationKind::Test);
            ok.fetch_add(1);
        }
        catch (const DB::Exception &) { /* the faulted caller may see the injected error; that is fine */ }
    };
    std::thread t1(caller, 1), t2(caller, 2);
    t1.join(); t2.join();

    // The critical assertion: no hang, no UAF, and the runtime's pending queue is empty afterwards.
    EXPECT_TRUE(fixture.pendingIsEmpty(ns)) << "an item was stranded in rt->pending";
    // At least one caller must have completed cleanly (the non-faulted one, or a re-drive).
    EXPECT_GE(ok.load(), 1);
}
```

If the ref-writer harness does not expose a factory/`pendingIsEmpty`, add the minimal test-only accessors to the fixture header used by `gtest_cas_ref_writer.cpp` (do not weaken production encapsulation — a `friend` test or a `#ifdef` test accessor as that file already does). Name the suite `RefLaneExceptionSafety` (matches the `RefWriter*`/`Ref*` gate coverage — verify it is included by the corrected filter; if not, prefix the suite so it is).

- [ ] **Step 3: Run to verify it fails / hangs**

```bash
ninja -C build unit_tests_dbms > build/build_task1a.log 2>&1; echo EXIT=$?
timeout 120 build/src/unit_tests_dbms --gtest_filter='RefLaneExceptionSafety.*' > build/test_task1.log 2>&1; echo EXIT=$?; tail -20 build/test_task1.log
```
Expected: FAIL or TIMEOUT (stranded item → the second caller carves the faulted item's dangling closure → UAF/ASan abort, or a hang, or a non-empty pending queue).

- [ ] **Step 4: Add the fault hook, then make the lane exception-safe**

In `CasRefLedger.h`: add `void testFaultBeforeCarve(std::function<void()> hook);` and a `std::function<void()> test_fault_before_carve;` member (default empty). In `flushRefBatch`, immediately before the carve, add `if (test_fault_before_carve) test_fault_before_carve();`.

Then harden the lane. In `flushRefBatch`/`runRefQueueLeader`, install a guard that runs on EVERY exit (normal or exceptional) and, under `ref_queue_mutex`, completes every item this flush is responsible for that is not yet `done`:

```cpp
/// Every item this leader owns (its own `item` plus everything it carves) must leave the flush
/// either `done` or validly owned by a still-blocked caller — never stranded in `pending` for a
/// future leader to carve after the owning caller's stack (the [&] build_ops closure) is gone.
SCOPE_EXIT({
    std::lock_guard<std::mutex> g(ref_queue_mutex);
    for (const auto & it : owned_items)          // the leader's own item + carved batch
    {
        if (!it->done)
        {
            it->error = std::current_exception()  // may be null on the normal path; see below
                ? std::current_exception()
                : std::make_exception_ptr(Exception(ErrorCodes::LOGICAL_ERROR,
                    "ref-lane flush exited without completing item"));
            it->done = true;
        }
        // Ensure it is not left in pending for a future leader.
        std::erase(rt->pending, it);
    }
    rt->cv.notify_all();
});
```

Adjust to the actual carve structure: the leader must record `owned_items` (its own `item` and each item it removes from `pending` to form the batch) as it goes, so the guard covers exactly them. On the happy path items are already `done` before the guard runs, so it is a no-op; on any throw it completes+de-pends them. Remove the ad-hoc `leader_active`-only reset from the old catch and fold `leader_active = false; cv.notify_all()` into the same guarded exit.

Additionally change the `build_ops` closures at the CALL sites in `CasPartWriteTxn.cpp` (`precommitAdd`, `promote`) from capture-by-reference `[&]` to capturing the needed state **by value** (copy the small scalars / `ManifestId` / ref name the closure reads), so that even a stranded closure never dereferences a dead stack. (Belt-and-suspenders; the lane guard is the real fix.)

- [ ] **Step 5: Run to verify pass + full gate**

```bash
ninja -C build unit_tests_dbms > build/build_task1b.log 2>&1; echo EXIT=$?
timeout 120 build/src/unit_tests_dbms --gtest_filter='RefLaneExceptionSafety.*' > build/test_task1.log 2>&1; echo EXIT=$?; tail -15 build/test_task1.log
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:CaDedupCache*:CaTransaction*:CaPartPathParser*:CaWiring*:CaInlinePlacement*' > build/test_task1_full.log 2>&1; tail -6 build/test_task1_full.log
```
Expected: `RefLaneExceptionSafety.*` PASS; full gate green (subagent-summarize the full log). No hang.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp \
        src/Disks/tests/gtest_cas_ref_lane_exception_safety.cpp
git commit -m "cas: make the ref-lane exception-safe (no stranded pending item on leader throw)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: Exact-manifest commit outcome + `dropRefIfMatches`

Give the commit an exact, in-lane-derived record of what it committed so rollback can be conditional. Still sequential commit — no concurrency yet.

**Files:**
- Modify: `.../Parts/PartFolderAccess.h` (a `CommitOutcome` struct; `promoteBuild` returns it; `repointRef` returns it; add `dropRefIfMatches`), `.../Parts/PartFolderAccess.cpp` (derive `created` inside the `appendRefOps` builder; implement `dropRefIfMatches` as one builder).
- Modify: `.../Pool/CasPartWriteTxn.*` — `PartWriteTxn::promote` propagates the committed `ManifestRef`/`created` out (the `repoint_old` pattern proves the outcome can be read from inside the builder).
- Test: `src/Disks/tests/gtest_cas_parallel_commit.cpp` (created here; grows across tasks).

**Interfaces:**
- Produces:
```cpp
namespace DB::Cas { struct CommitOutcome { RootNamespace ns; String ref; ManifestRef manifest_ref; bool created = false; }; }
```
  - `CommitOutcome CachedPartFolderAccess::promoteBuild(Cas::PartWriteTxn & build, const PartRefKey & key, UInt128 build_id, ManifestId id, Cas::ProvenanceOp op, bool allow_repoint = false);` (was `void`)
  - `CommitOutcome CachedPartFolderAccess::repointRef(const PartRefKey & key, std::vector<Cas::ManifestEntry> entries, Cas::ProvenanceOp op);` (was `bool`)
  - `bool CachedPartFolderAccess::dropRefIfMatches(const PartRefKey & key, const ManifestRef & expected) noexcept;` — returns true iff it removed the ref because the current committed binding equalled `expected`; false (and leaves the ref) otherwise.

- [ ] **Step 1: Write the failing tests**

Create `src/Disks/tests/gtest_cas_parallel_commit.cpp` (reuse the CA wiring/transaction harness — grep `gtest_ca_wiring.cpp` for how a `ContentAddressedMetadataStorage` + pool is stood up in-process). Tests:

```cpp
TEST(CasCommitOutcome, PromoteReportsCreatedAndManifest)
{
    auto fx = makeCaWiringFixture();
    const PartRefKey key{fx.ns(), "20260101_1_1_0"};
    auto build = fx.stageSimplePart(key, /*blobs=*/1);         // stages manifest + one blob, precommits, uploads
    const Cas::CommitOutcome oc = fx.partAccess().promoteBuild(*build, key, build->buildId(), build->manifestId(),
                                                               Cas::ProvenanceOp::Other);
    EXPECT_TRUE(oc.created);
    EXPECT_EQ(oc.ref, key.ref);
    EXPECT_EQ(oc.manifest_ref, build->manifestId().ref);
}

TEST(CasCommitOutcome, DropRefIfMatchesRemovesOnlyExact)
{
    auto fx = makeCaWiringFixture();
    const PartRefKey key{fx.ns(), "20260101_2_2_0"};
    const Cas::CommitOutcome oc1 = fx.commitSimplePart(key, /*blobs=*/1);   // R -> M1
    // Rebind R -> M2 (a legitimate repoint by "another writer").
    const Cas::CommitOutcome oc2 = fx.repointToFreshManifest(key);          // R -> M2 != M1
    ASSERT_NE(oc1.manifest_ref, oc2.manifest_ref);
    // Conditional drop keyed on the STALE M1 must NOT remove M2.
    EXPECT_FALSE(fx.partAccess().dropRefIfMatches(key, oc1.manifest_ref));
    EXPECT_TRUE(fx.partAccess().existsRef(key, Cas::Freshness::ForceFresh));
    // Conditional drop keyed on the CURRENT M2 removes it.
    EXPECT_TRUE(fx.partAccess().dropRefIfMatches(key, oc2.manifest_ref));
    EXPECT_FALSE(fx.partAccess().existsRef(key, Cas::Freshness::ForceFresh));
}
```

Add whatever minimal fixture helpers (`stageSimplePart`, `commitSimplePart`, `repointToFreshManifest`) the harness lacks, modelled on the existing wiring tests. Suite prefix `CasCommitOutcome` (covered by `Cas*`).

- [ ] **Step 2: Run to verify failure**

```bash
ninja -C build unit_tests_dbms > build/build_task2a.log 2>&1; echo EXIT=$?
```
Expected: compile FAILURE (`CommitOutcome` undefined, `promoteBuild` returns void, no `dropRefIfMatches`).

- [ ] **Step 3: Implement**

- Add `CommitOutcome` to `PartFolderAccess.h`.
- `promoteBuild`: change return type to `CommitOutcome`. Inside, `PartWriteTxn::promote`'s `appendRefOps` builder already evaluates the committed state on the leader thread (the `repoint_old` local at `CasPartWriteTxn.cpp:937` proves the pattern). Add an out-parameter (or return-struct) from `PartWriteTxn::promote` carrying `{manifest_ref = id.ref, created = (state.committed.find(final_ref_name) == end evaluated inside the builder)}`. Publish it to a caller-visible slot **inside the builder / immediately upon `appendRefOps` return, before `eraseView`/logging** (so a later throw cannot lose it). `promoteBuild` returns `CommitOutcome{key.ns, key.ref, manifest_ref, created}`.
- `repointRef`: same — derive `created` inside its `appendRefOps` builder (repoint that finds the ref absent creates it → `created = true`), return `CommitOutcome`. Update its two call sites in `publishStaging` (they currently use the `bool`).
- `dropRefIfMatches(key, expected)`: implement as ONE `appendRefOps` builder that reads the current committed binding for `key.ref` and emits the owner-removal op only if it equals `expected`; return whether it removed. Mirror `dropRef` (`PartFolderAccess.cpp:452`) but with the equality guard inside the closure. `noexcept`: swallow+log like `dropRefBestEffort`.
- Update the two `promoteBuild` call sites in `publishStaging` and the `repointRef` sites to consume `CommitOutcome` (Task 3 wires it to rollback; here just make them compile, discarding the value or logging it).

- [ ] **Step 4: Run to verify pass + full gate**

```bash
ninja -C build unit_tests_dbms > build/build_task2b.log 2>&1; echo EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CasCommitOutcome.*' > build/test_task2.log 2>&1; tail -15 build/test_task2.log
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:CaDedupCache*:CaTransaction*:CaPartPathParser*:CaWiring*:CaInlinePlacement*' > build/test_task2_full.log 2>&1; tail -6 build/test_task2_full.log
```
Expected: `CasCommitOutcome.*` PASS; full gate green.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h \
        src/Disks/tests/gtest_cas_parallel_commit.cpp
git commit -m "cas: promoteBuild/repointRef return an exact CommitOutcome; add dropRefIfMatches

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: Sequential rollback rework + `st.build` reset

Rewire `commit()` (still single-threaded) to record each part's `CommitOutcome` at confirm and roll back with `dropRefIfMatches`, and reset `st.build` after abandon/promote. This makes rollback correct BEFORE concurrency is introduced, so Task 5 only adds scheduling.

**Files:**
- Modify: `.../ContentAddressedTransaction.cpp` (`commit()` ~L370-410, `publishStaging` return handling, dtor ~L100-124), `.../ContentAddressedTransaction.h` (replace `created_refs` vector-of-pair with a `std::vector<std::optional<Cas::CommitOutcome>>` sized per-part, or an outcome list).
- Test: `gtest_cas_parallel_commit.cpp` (extend).

**Interfaces:**
- Consumes: `CommitOutcome`, `dropRefIfMatches` (Task 2).
- Produces: `publishStaging` gains a trailing parameter `std::optional<Cas::CommitOutcome> & out_slot` (replacing its `bool` return, or in addition to it) which it writes AT confirm — this exact signature is what Task 5 dispatches. `commit()` records the slot and rolls back with `dropRefIfMatches`.

- [ ] **Step 1: Write the failing tests** (extend `gtest_cas_parallel_commit.cpp`)

```cpp
TEST(CasCommitRollback, AbsentBeforeDroppedPreExistingUntouched)
{
    auto fx = makeCaWiringFixture();
    const PartRefKey pre{fx.ns(), "pre_existing_1_1_0"};
    fx.commitSimplePart(pre, 1);                                  // a pre-existing ref, must survive
    // A transaction that commits one NEW part then fails on a second part's promote.
    auto txn = fx.beginTxn();
    fx.stageInto(txn, {fx.ns(), "new_a_1_1_0"}, 1);
    fx.stageInto(txn, {fx.ns(), "new_b_1_1_0"}, 1);
    fx.armPromoteFailure("new_b_1_1_0");                          // fault injection in publishStaging's promote
    EXPECT_ANY_THROW(txn->commit({}));
    EXPECT_FALSE(fx.partAccess().existsRef({fx.ns(), "new_a_1_1_0"}, Cas::Freshness::ForceFresh)); // rolled back
    EXPECT_TRUE (fx.partAccess().existsRef(pre, Cas::Freshness::ForceFresh));                       // untouched
}

TEST(CasCommitRollback, RepointByOtherWriterSurvivesRollback)
{
    auto fx = makeCaWiringFixture();
    const PartRefKey key{fx.ns(), "shared_1_1_0"};
    auto txn = fx.beginTxn();
    fx.stageInto(txn, key, 1);                                   // T1 will create R -> M1
    fx.armAfterPromoteHook(key, [&]{ fx.repointToFreshManifest(key); }); // T2 repoints R -> M2 right after T1's promote
    fx.stageInto(txn, {fx.ns(), "poison_1_1_0"}, 1);
    fx.armPromoteFailure("poison_1_1_0");
    EXPECT_ANY_THROW(txn->commit({}));
    // T1's rollback used dropRefIfMatches(M1); M2 != M1 so it must survive.
    EXPECT_TRUE(fx.partAccess().existsRef(key, Cas::Freshness::ForceFresh));
    EXPECT_EQ(fx.currentManifest(key), fx.lastRepointManifest());
}
```

- [ ] **Step 2: Run to verify failure**

```bash
ninja -C build unit_tests_dbms > build/build_task3a.log 2>&1; echo EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CasCommitRollback.*' > build/test_task3.log 2>&1; tail -15 build/test_task3.log
```
Expected: FAIL (today's `dropRef` clobbers the repointed M2; and/or the created-ref bookkeeping is by `(ns,ref)` only).

- [ ] **Step 3: Implement (still single-threaded)**

- In `ContentAddressedTransaction.h`, replace `std::vector<std::pair<Cas::RootNamespace, std::string>> created_refs;` with `std::vector<std::optional<Cas::CommitOutcome>> part_outcomes;` (indexed by part position) — a preallocated per-part slot vector (Task 5 relies on this being index-addressed and no-throw to write).
- In `commit()`: snapshot the parts into an indexed vector first; `part_outcomes.assign(parts.size(), std::nullopt);` (single allocation up front). For each part `i`, call `publishStaging(...)` which now writes `part_outcomes[i] = outcome` the instant its `promoteBuild`/`repointRef` confirms (before any post-commit throwable work). On exception: iterate `part_outcomes`, and for each engaged slot with `created == true`, call `partAccess()->dropRefIfMatches({oc.ns, oc.ref}, oc.manifest_ref)`; then rethrow.
- Reset `st.build` after abandon and after promote in `publishStaging` (`st.build.reset()` at `ContentAddressedTransaction.cpp:344` and after `promoteBuild`), so the dtor never double-abandons a repoint scratch build (which throws `LOGICAL_ERROR` from `~`).
- Keep the loop single-threaded in this task.

- [ ] **Step 4: Run to verify pass + full gate**

```bash
ninja -C build unit_tests_dbms > build/build_task3b.log 2>&1; echo EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CasCommitRollback.*:CasCommitOutcome.*' > build/test_task3.log 2>&1; tail -15 build/test_task3.log
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:CaDedupCache*:CaTransaction*:CaPartPathParser*:CaWiring*:CaInlinePlacement*' > build/test_task3_full.log 2>&1; tail -6 build/test_task3_full.log
```
Expected: PASS; full gate green (the CA soak-style rollback invariants unchanged).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h \
        src/Disks/tests/gtest_cas_parallel_commit.cpp
git commit -m "cas: exact-manifest rollback via dropRefIfMatches + per-part outcome slots; reset st.build after abandon

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: `cas_commit_concurrency` setting + dedicated CAS commit pool

Add the tunable and a dedicated pool disjoint from `getThreadPoolWriter`. No behavior change yet (commit still sequential).

**Files:**
- Modify: the CAS pool config surface (`.../Pool/CasPool.h` + `MetadataStorageFactory.cpp:269` area where `dedup_head_first_min_bytes` is read) to add `cas_commit_concurrency` (default 16); a dedicated `ThreadPool` accessor.
- Test: `gtest_cas_parallel_commit.cpp` (extend).

**Interfaces:**
- Produces: `PoolConfig::cas_commit_concurrency` (uint64, default 16); a process-wide `DB::Cas::getCasCommitThreadPool()` returning a `ThreadPool &` (a static pool sized by a server setting `cas_commit_pool_size`, default 100), disjoint from `Context::getThreadPoolWriter()`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(CasCommitPool, DistinctFromWriterPoolAndBounded)
{
    // The dedicated pool object is not the writer pool (disjointness is the hard anti-deadlock rule).
    EXPECT_NE(static_cast<void*>(&DB::Cas::getCasCommitThreadPool()),
              static_cast<void*>(&DB::Context::getThreadPoolWriter()));
    // Default concurrency setting is present.
    EXPECT_EQ(DB::Cas::PoolConfig{}.cas_commit_concurrency, 16u);
}
```
(If `getThreadPoolWriter` is only reachable via a `Context`, assert disjointness structurally instead — e.g. that `getCasCommitThreadPool` is a distinct static from `IOThreadPool`/writer — and cite the pool identities in the test comment.)

- [ ] **Step 2: Run to verify failure** — compile FAILURE (symbols undefined).

```bash
ninja -C build unit_tests_dbms > build/build_task4a.log 2>&1; echo EXIT=$?
```

- [ ] **Step 3: Implement**

- Add `uint64_t cas_commit_concurrency = 16;` to `PoolConfig` (`CasPool.h`) and read it in `MetadataStorageFactory.cpp` next to `dedup_head_first_min_bytes` (`config.getUInt64(config_prefix + ".cas_commit_concurrency", 16)`), threading it through the `CasPool` ctor like `dedup_head_first_min_bytes`.
- Add `DB::Cas::getCasCommitThreadPool()` — a function-local static `ThreadPool` sized by a server setting `cas_commit_pool_size` (default 100), modelled on `getIOThreadPool` in `src/IO/SharedThreadPools.cpp`. Register `cas_commit_pool_size` in `ServerSettings` next to `max_io_thread_pool_size`.

- [ ] **Step 4: Run to verify pass + full gate**

```bash
ninja -C build unit_tests_dbms clickhouse > build/build_task4b.log 2>&1; echo EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CasCommitPool.*' > build/test_task4.log 2>&1; tail -10 build/test_task4.log
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:CaDedupCache*:CaTransaction*:CaPartPathParser*:CaWiring*:CaInlinePlacement*' > build/test_task4_full.log 2>&1; tail -6 build/test_task4_full.log
```
Expected: PASS; server build EXIT=0; full gate green.

- [ ] **Step 5: Commit**

```bash
git add -A src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed src/IO/SharedThreadPools.* src/Core/ServerSettings.* src/Disks/tests/gtest_cas_parallel_commit.cpp
git commit -m "cas: cas_commit_concurrency setting + dedicated CAS commit thread pool (disjoint from writer pool)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: Parallelize `commit()` (bounded worker-loops, exception-safe drain)

Switch the commit loop to `cas_commit_concurrency` worker-loops on the dedicated pool. This is the payoff task; everything it relies on (exception-safe lane, exact rollback, the pool) is already in place.

**Files:**
- Modify: `.../ContentAddressedTransaction.cpp` (`commit()`).
- Test: `gtest_cas_parallel_commit.cpp` (extend: saturation, join-ordering, hardlink, determinism); benchmark note.

**Interfaces:**
- Consumes: `getCasCommitThreadPool()`, `cas_commit_concurrency`, `part_outcomes` slots, `dropRefIfMatches` (Tasks 2-4).

- [ ] **Step 1: Write the failing tests** (extend)

```cpp
TEST(CasParallelCommit, SaturationBoundedCompletion)
{
    // A commit pool of N with >N parts must complete (no worker starves waiting on its own pool).
    auto fx = makeCaWiringFixture();
    fx.setCommitConcurrency(2);                     // N=2 worker-loops
    auto txn = fx.beginTxn();
    for (int i = 0; i < 5; ++i)                     // N+3 parts, >=1 blob each
        fx.stageInto(txn, {fx.ns(), "p_" + std::to_string(i) + "_1_1_0"}, 1);
    // Must return in bounded time; a self-wait deadlock would hang.
    ASSERT_NO_THROW(txn->commit({}));
    for (int i = 0; i < 5; ++i)
        EXPECT_TRUE(fx.partAccess().existsRef({fx.ns(), "p_" + std::to_string(i) + "_1_1_0"}, Cas::Freshness::ForceFresh));
}

TEST(CasParallelCommit, JoinPrecedesRollbackUnderSlowWorker)
{
    auto fx = makeCaWiringFixture();
    fx.setCommitConcurrency(4);
    auto txn = fx.beginTxn();
    fx.stageInto(txn, {fx.ns(), "slow_1_1_0"}, 1);
    fx.armSlowUpload("slow_1_1_0", /*ms=*/50);      // one worker held mid-publishStaging
    fx.stageInto(txn, {fx.ns(), "poison_1_1_0"}, 1);
    fx.armPromoteFailure("poison_1_1_0");
    std::atomic<bool> slow_done{false}, rollback_ran{false};
    fx.onSlowUploadFinish([&]{ slow_done = true; });
    fx.onFirstDropRefIfMatches([&]{ rollback_ran = true; EXPECT_TRUE(slow_done.load()) << "rollback ran before the slow worker joined"; });
    EXPECT_ANY_THROW(txn->commit({}));
    EXPECT_TRUE(rollback_ran.load());
}

TEST(CasParallelCommit, HardlinkSharedPendingBlobParallel)
{
    // Two parts referencing one staged blob, committed in parallel: same-key putIfAbsent/412/adopt.
    auto fx = makeCaWiringFixture();
    fx.setCommitConcurrency(4);
    auto txn = fx.beginTxn();
    const auto shared = fx.stageBlobOnce(txn);                       // one staged blob
    fx.stageReferencing(txn, {fx.ns(), "h_a_1_1_0"}, shared);
    fx.stageReferencing(txn, {fx.ns(), "h_b_1_1_0"}, shared);        // hardlink-copied pending record
    ASSERT_NO_THROW(txn->commit({}));
    EXPECT_TRUE(fx.partAccess().existsRef({fx.ns(), "h_a_1_1_0"}, Cas::Freshness::ForceFresh));
    EXPECT_TRUE(fx.partAccess().existsRef({fx.ns(), "h_b_1_1_0"}, Cas::Freshness::ForceFresh));
}

TEST(CasParallelCommit, ParallelMatchesSequentialLogicalState)
{
    // Semantic determinism: same folded logical state as a forced-sequential commit.
    auto seq = makeCaWiringFixture(); seq.setCommitConcurrency(1);
    auto par = makeCaWiringFixture(); par.setCommitConcurrency(8);
    const auto plan = makeMultiPartInsertPlan(/*parts=*/20, /*blobs_each=*/3);   // deterministic content
    seq.applyAndCommit(plan);
    par.applyAndCommit(plan);
    EXPECT_EQ(seq.foldedLogicalState(), par.foldedLogicalState());   // bindings+manifest entries+payloads+in-degree; excludes IDs/timestamps/encoding
}
```

- [ ] **Step 2: Run to verify failure**

```bash
ninja -C build unit_tests_dbms > build/build_task5a.log 2>&1; echo EXIT=$?
timeout 120 build/src/unit_tests_dbms --gtest_filter='CasParallelCommit.*' > build/test_task5.log 2>&1; echo EXIT=$?; tail -20 build/test_task5.log
```
Expected: FAIL (harness hooks like `setCommitConcurrency`/`onFirstDropRefIfMatches` don't exist yet; commit is still sequential so saturation "passes" trivially but the join-ordering hook is absent).

- [ ] **Step 3: Implement the parallel commit**

Rewrite `commit()`'s publish loop (keep the exact per-part `publishStaging` unchanged):

```cpp
void ContentAddressedTransaction::commit(const TransactionCommitOptionsVariant &)
{
    if (failed)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "retrying a failed content-addressed transaction is not supported");

    /// Snapshot the parts into an index-addressable vector; `parts` is never mutated during commit.
    std::vector<std::pair<Cas::RootNamespace, std::string> *> keys;     /// or copy the (ns, ref) keys
    std::vector<PartStaging *> staged;
    keys.reserve(parts.size()); staged.reserve(parts.size());
    for (auto & [key, st] : parts) { /* fill keys (ns=key.first, ref=key.second) and staged */ }

    part_outcomes.assign(parts.size(), std::nullopt);                  /// preallocated, no-throw per-part slots
    std::vector<std::exception_ptr> errors(parts.size());              /// preallocated exception slots

    const size_t workers = std::min<size_t>(
        std::max<uint64_t>(1, metadata_storage.store()->poolConfig().cas_commit_concurrency), parts.size());

    std::atomic<size_t> next{0};
    ThreadGroupPtr thread_group = CurrentThread::getGroup();           /// propagate attribution; non-owning

    {
        /// Inner scope: the runner's destructor waits for running tasks; declared INSIDE the try so
        /// unwinding drains BEFORE the catch body runs. No worker is ever live during rollback.
        ThreadPoolCallbackRunnerLocal<void> runner(Cas::getCasCommitThreadPool(), "CasCommit");
        for (size_t w = 0; w < workers; ++w)
            runner([this, &next, &staged, &keys, &errors, thread_group]
            {
                ThreadGroupSwitcher switcher(thread_group, "CasCommit");   /// only if a group exists
                for (size_t i = next.fetch_add(1); i < staged.size(); i = next.fetch_add(1))
                {
                    try
                    {
                        const Cas::RootNamespace ns{keys[i]->first};
                        publishStaging(ns, keys[i]->second, *staged[i], part_outcomes[i]);   /// writes the slot at confirm
                    }
                    catch (...) { errors[i] = std::current_exception(); }
                }
            });
        runner.waitForAllToFinish();          /// non-throwing drain; NEVER get()
    }   /// runner dtor also joins on every path

    /// After the drain: first-error-wins rollback.
    std::exception_ptr first;
    for (auto & e : errors) if (e) { first = e; break; }
    if (first)
    {
        failed = true;
        for (const auto & oc : part_outcomes)
            if (oc && oc->created)
                metadata_storage.partAccess()->dropRefIfMatches({oc->ns, oc->ref}, oc->manifest_ref);
        std::rethrow_exception(first);
    }
    committed = true;
    cleanupPendingTempFiles();
    force_fresh_validated_refs.clear();
}
```

Change `publishStaging`'s signature to take `std::optional<Cas::CommitOutcome> & out_slot` and write it at confirm (before any throwable post-commit work). Use the real `ThreadPoolCallbackRunnerLocal` API (grep `src/Common/threadPoolCallbackRunner.h` for the exact ctor/`waitForAllToFinish`/`ThreadGroupSwitcher` spelling; the sketch above matches its shape). Capture only non-owning `this`/pointers into the stack-scoped snapshot vectors and the `thread_group` `shared_ptr` (attribution only — released when `commit` returns). Add the test hooks (`setCommitConcurrency` overrides the pool config for the fixture; `onFirstDropRefIfMatches`/`armSlowUpload` via the existing fault-injection surface).

- [ ] **Step 4: Run to verify pass + full gate + TSan**

```bash
ninja -C build unit_tests_dbms > build/build_task5b.log 2>&1; echo EXIT=$?
timeout 180 build/src/unit_tests_dbms --gtest_filter='CasParallelCommit.*:CasCommitRollback.*:CasCommitOutcome.*' > build/test_task5.log 2>&1; echo EXIT=$?; tail -20 build/test_task5.log
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:CaDedupCache*:CaTransaction*:CaPartPathParser*:CaWiring*:CaInlinePlacement*' > build/test_task5_full.log 2>&1; tail -6 build/test_task5_full.log
```
Then TSan (build a TSan `unit_tests_dbms` in `build_tsan/` if present, else note it): run `CasParallelCommit.*:CasCommitRollback.*` under TSan; expect zero data-race reports. Subagent-summarize logs.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h \
        src/Disks/tests/gtest_cas_parallel_commit.cpp
git commit -m "cas: parallel per-part commit on a dedicated bounded pool (engages ref-ledger batching)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: Benchmark validation + BACKLOG close-out

Confirm the win against the measured baseline and record it.

**Files:**
- Modify: `utils/ca-soak/scenarios/BACKLOG.md` (the write-path-latency entry), the spec status line.

- [ ] **Step 1: Re-run the comparison** — the 500-partition CAS-vs-standard-S3 insert on a real S3/RustFS stand (per `utils/ca-soak/README.md`), or the closest local equivalent. Pull the INSERT's `system.query_log` ProfileEvents: `CasRefBatchFlushes`, `CasRefBatchedMutations`, `CasRefQueueWaitMicroseconds`, `S3HeadObject`, `S3GetObject`, wall time.

- [ ] **Step 2: Assert the success metric** — `CasRefBatchFlushes` ≪ `CasRefBatchedMutations` (batching now engaged; was 1026 == 1026), `CasRefQueueWaitMicroseconds` down sharply, wall-clock materially closer to the standard-S3 22.4 s. Record before/after numbers.

- [ ] **Step 3: Update BACKLOG + spec** — flip the write-path entry's #1/#2 to RESOLVED with the measured before/after and a pointer to spec+plan; set the spec `Status` to `implemented`. Note #3/#4 remain open.

- [ ] **Step 4: Commit**

```bash
git add utils/ca-soak/scenarios/BACKLOG.md docs/superpowers/specs/2026-07-22-cas-parallel-write-path-design.md
git commit -m "cas: mark write-path serialization #1/#2 RESOLVED (bounded parallel commit, measured numbers)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```
