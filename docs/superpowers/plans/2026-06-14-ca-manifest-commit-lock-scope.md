# CA Manifest Commit Lock-Scope Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop CA MergeTree from freezing `system.parts` for tens of seconds by publishing a freshly-written part's final manifest ref at the **lock-free** tmp→final rename, so the under-`data_parts`-lock `commit` does no S3 network write.

**Architecture:** Mechanism B (zero generic-MergeTree change). Four changes, all in `src/Disks/`: (1) make CA `DiskObjectStorageTransaction::moveDirectory` dispatch **eagerly** (so the rename runs at the lock-free `renameParts()`, not as a deferred lambda under the lock); (2) in `ContentAddressedTransaction::moveDirectory`'s staged-re-key branch, **publish the final ref** (the freshly-written part is finalized here); (3) add a `published` flag to `PartStaging`; (4) `commit()` **skips** already-published stagings. Replicated INSERT/merge/mutation + plain merge rename lock-free → their publish leaves the lock. Plain mutation/INSERT/fetch keep today's under-lock publish (graceful, documented follow-up).

**Tech Stack:** C++ (ClickHouse, Allman braces), gtest (`unit_tests_dbms`), CA disk layer (`ContentAddressedTransaction`/`DiskObjectStorageTransaction`), the CA wiring test harness (`gtest_ca_wiring.cpp` style), the Python soak harness (`utils/ca-soak/`).

**Spec:** `docs/superpowers/specs/2026-06-14-ca-manifest-commit-lock-scope-design.md` (B151).

**Build/run conventions (CLAUDE.md):** build from the build dir with `ninja unit_tests_dbms` **without** `-j`/`nproc`, redirect to a log, and have a **subagent** summarize it. Run gtests `build/src/unit_tests_dbms --gtest_filter='...' > build/test_<name>.log 2>&1`, subagent summarizes. Allman braces. Say "exception" not "crash". New commits only (no amend/rebase); never commit to master (work on `cas-mergetree-poc`).

**KNOWN PRE-EXISTING FAILING TESTS** (NOT regressions — ignore / filter out): `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak`, `CasProtocol.DropReattachThroughDetachedNamespace`, `CasTruncateReclaim.PerRefDropOfSharedBlobsReclaimsToZero`.

---

## File Structure

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h` — add `bool published = false;` to `PartStaging` (line ~79-84); declare a private `publishStaging` helper.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` — extract `publishStaging` from `commit()`; `commit()` skips published; `moveDirectory` staged-re-key branch calls `publishStaging`.
- `src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp` — `moveDirectory()` (line ~129-135): CA eager dispatch.
- `src/Disks/tests/gtest_ca_transaction.cpp` — NEW wiring-level test (auto-globbed into `unit_tests_dbms`).
- `utils/ca-soak/` — soak re-validation.

---

## TASK 1 — Extract `publishStaging` helper + `published` flag (pure refactor, behavior unchanged)

Refactor the per-part publish out of `commit()` into a reusable helper, and add the `published` flag (defaulting false, so `commit()` still publishes everything exactly as today). This isolates the publish logic so Task 2 can call it from the rename.

**Files:**
- Modify: `src/Disks/.../ContentAddressed/ContentAddressedTransaction.h`
- Modify: `src/Disks/.../ContentAddressed/ContentAddressedTransaction.cpp`

- [ ] **Step 1: Verify the existing wiring test passes (baseline)**

Run: `build/src/unit_tests_dbms --gtest_filter='CaWiring*' > build/test_t1base.log 2>&1`
Subagent summarizes `build/test_t1base.log`.
Expected: all `CaWiring*` PASS (this is the behavior Task 1 must preserve).

- [ ] **Step 2: Add `published` flag + `publishStaging` declaration to the header**

In `ContentAddressedTransaction.h`, add `bool published = false;` to the `PartStaging` struct (currently lines ~79-84):

```cpp
    struct PartStaging
    {
        Cas::BuildPtr build;                       /// nullptr until the first content upload
        std::vector<Cas::TreeEntry> entries;       /// staged tree entries (uploads + adoptions)
        std::map<std::string, std::string> mutable_files;
        std::set<std::string> mutable_removed;     /// staged deletions for a COMMITTED part's payload
        bool published = false;                    /// the ref is already durably published (at the
                                                   /// lock-free rename); commit() must not re-publish it.
    };
```

In the private section (near the `republishRef`/`dropRefIfPresent` declarations ~line 97-102), declare the helper:

```cpp
    /// Publish one staged part durably (putTree + publish, or updateRefPayload for a mutable-only
    /// staging) and mark it `published`. Idempotent: a no-op if already published. Returns true iff
    /// this call newly CREATED a ref that did not exist before (for commit()'s rollback tracking).
    bool publishStaging(const Cas::RootNamespace & ns, const std::string & ref, PartStaging & st);
```

- [ ] **Step 3: Implement `publishStaging` and refactor `commit()` to use it**

In `ContentAddressedTransaction.cpp`, add the helper (place it just above `commit()`):

```cpp
bool ContentAddressedTransaction::publishStaging(const Cas::RootNamespace & ns, const std::string & ref, PartStaging & st)
{
    if (st.published)
        return false;   /// already durable (published at the lock-free rename) — never re-publish

    if (!st.build && st.entries.empty())
    {
        /// Mutable-only staging: MVCC autocommit one-shots (txn_version.txt) / metadata_version bumps
        /// on a COMMITTED part.
        if (!st.mutable_files.empty() || !st.mutable_removed.empty())
        {
            metadata_storage.store()->updateRefPayload(ns, ref, [&](Cas::RefPayload & payload)
            {
                for (const auto & [name, bytes] : st.mutable_files)
                    payload.mutable_files[name] = bytes;
                for (const auto & name : st.mutable_removed)
                    payload.mutable_files.erase(name);
            });
        }
        st.published = true;
        return false;   /// updateRefPayload mutates an existing ref — never a new ref to roll back
    }

    if (!st.build)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "ContentAddressedTransaction: staged entries for {}/{} without a Build", ns.string(), ref);

    auto tree = st.build->putTree(st.entries);
    Cas::RefPayload payload;
    payload.mutable_files = st.mutable_files;
    /// The publish wall-clock stamp backing getLastModified (reserved name, filtered from listings).
    payload.mutable_files[".ca_mtime"] = std::to_string(static_cast<uint64_t>(::time(nullptr)));

    /// Force-fresh (Pillar B): publish-gate rollback tracking — a stale result mis-tracks rollback.
    const bool ref_existed = metadata_storage.store()->resolveRef(ns, ref).has_value();
    st.build->publish(ns, ref, tree, std::move(payload));
    st.published = true;
    return !ref_existed;
}
```

Replace the body of `commit()` so its per-part loop delegates to `publishStaging` (the helper does exactly what the inline code did; `published` is false for every staging at this point, so behavior is identical):

```cpp
void ContentAddressedTransaction::commit(const TransactionCommitOptionsVariant &)
{
    /// Publish each staged part not already published at the lock-free rename (B151). Commit
    /// atomicity (B122): track the refs THIS commit newly creates and, on any exception, best-effort
    /// unpublish them before rethrowing (a partial commit is GC-reclaimable debris, not a violation).
    /// Fail-closed: only refs ABSENT before we published are rolled back.
    std::vector<std::pair<Cas::RootNamespace, std::string>> created_refs;
    try
    {
        for (auto & [key, st] : parts)
        {
            const Cas::RootNamespace ns{key.first};
            if (publishStaging(ns, key.second, st))
                created_refs.emplace_back(ns, key.second);
        }
    }
    catch (...)
    {
        for (const auto & [ns, ref] : created_refs)
        {
            try
            {
                metadata_storage.store()->dropRef(ns, ref);
            }
            catch (...) // NOLINT(bugprone-empty-catch)
            {
            }
        }
        throw;
    }
    committed = true;
}
```

(This preserves the exact publish logic + the `created_refs`/`dropRef` compensating rollback; only the per-part body moved into `publishStaging`.)

- [ ] **Step 4: Build**

Run: `cd build && ninja unit_tests_dbms > build_t1.log 2>&1`; subagent summarizes `build/build_t1.log`.
Expected: clean build.

- [ ] **Step 5: Run the CA suite to confirm no behavior change**

Run: `build/src/unit_tests_dbms --gtest_filter='CaWiring*:Cas*' > build/test_t1.log 2>&1`; subagent summarizes.
Expected: `CaWiring*` all green; the CA suite green except the 3 known-failing baseline tests. (Behavior is unchanged — `published` is always false so far.)

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
git commit -m "CA: extract publishStaging helper + PartStaging.published flag (no behavior change)"
```

---

## TASK 2 — Publish the final ref at the lock-free rename

Make CA `moveDirectory` eager, and have its staged-re-key branch publish the final ref. Then `commit()` skips it (via the `published` flag).

**Files:**
- Modify: `src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp` (`moveDirectory`, ~line 129-135)
- Modify: `src/Disks/.../ContentAddressed/ContentAddressedTransaction.cpp` (`moveDirectory` staged-re-key branch)
- Test: `src/Disks/tests/gtest_ca_transaction.cpp` (new)

- [ ] **Step 1: Write the failing test**

Create `src/Disks/tests/gtest_ca_transaction.cpp`. Model the setup on `gtest_ca_wiring.cpp` (`openWiringStorage()` + `writeThroughTransaction()`). The test drives write → rename → commit and asserts the publish happened at the rename.

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include "cas_test_helpers.h"
#include <filesystem>

namespace
{
std::shared_ptr<DB::ContentAddressedMetadataStorage> openTxStorage()
{
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), "pool", "srv1",
        std::filesystem::temp_directory_path() / "ca_tx_scratch", nullptr);
    storage->startup();
    return storage;
}

void writeFileTx(DB::IMetadataTransaction & tx, const DB::String & path, const DB::String & bytes)
{
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(tx);
    auto buf = ca_tx.writeFile(path, 65536, DB::WriteMode::Rewrite, {});
    buf->write(bytes.data(), bytes.size());
    buf->finalize();
}
}

/// B151: a freshly-written part is PUBLISHED at the (lock-free) tmp->final rename, so commit() does
/// no manifest write under the data_parts lock. Assert the final ref resolves AFTER the rename and
/// BEFORE commit.
TEST(CaTransactionLockScope, PublishHappensAtRenameNotCommit)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();

    /// Write under a tmp part name (as the MergeTree writer does).
    writeFileTx(*tx, "uui/uuid-1/tmp_insert_all_1_1_0/data.bin", "content-A");

    /// Nothing visible yet — neither the tmp nor the final ref.
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/tmp_insert_all_1_1_0"));
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));

    /// The rename (tmp -> final). With the fix this is where the FINAL ref is published.
    tx->moveDirectory("uui/uuid-1/tmp_insert_all_1_1_0", "uui/uuid-1/all_1_1_0");

    /// The final ref is durably published RIGHT NOW — before commit().
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_1_1_0/data.bin"));
    /// The tmp ref was never durably published.
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/tmp_insert_all_1_1_0"));

    /// commit() finalizes; the ref stays published and correct.
    tx->commit(DB::NoCommitOptions{});
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-1/all_1_1_0/data.bin"), 9u);
}

/// A committed-ref rename (no staged source — e.g. DETACH-shaped move of an already-published part)
/// must NOT be treated as a fresh-write publish: it goes through republishRef, moving the existing
/// ref, not a spurious staged publish.
TEST(CaTransactionLockScope, CommittedRefMoveDoesNotSpuriouslyPublish)
{
    auto storage = openTxStorage();
    /// Publish a part normally.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-1/tmp_insert_all_2_2_0/data.bin", "payload");
        tx->moveDirectory("uui/uuid-1/tmp_insert_all_2_2_0", "uui/uuid-1/all_2_2_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsDirectory("uui/uuid-1/all_2_2_0"));

    /// Now rename the COMMITTED part dir (no staged source in this new transaction). It must move via
    /// republishRef: destination present, source gone.
    {
        auto tx = storage->createTransaction();
        tx->moveDirectory("uui/uuid-1/all_2_2_0", "uui/uuid-1/all_2_2_0_moved");
        tx->commit(DB::NoCommitOptions{});
    }
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/all_2_2_0_moved"));
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_2_2_0"));
}
```

(Verify `makeLocalObjectStorageForTest`, `WriteMode`, `NoCommitOptions`, and the route path shape (`uui/<uuid>/<part>/<file>`) against `gtest_ca_wiring.cpp`; adapt the path prefix to whatever that file uses so routing succeeds.)

- [ ] **Step 2: Run the test to verify it fails**

Run: `build/src/unit_tests_dbms --gtest_filter='CaTransactionLockScope.*' > build/test_t2.log 2>&1`; subagent summarizes.
Expected: `PublishHappensAtRenameNotCommit` FAILS at `EXPECT_TRUE(existsDirectory("...all_1_1_0"))` after `moveDirectory` — today the publish is deferred to `commit()`, so the ref does not resolve until after commit.

- [ ] **Step 3: Eager CA dispatch in `DiskObjectStorageTransaction::moveDirectory`**

In `src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp`, replace `moveDirectory` (currently ~line 129-135):

```cpp
void DiskObjectStorageTransaction::moveDirectory(const std::string & from_path, const std::string & to_path)
{
    /// CA: dispatch the rename EAGERLY (mirroring the createHardLink CA early-dispatch above) rather
    /// than queuing a deferred lambda that fires inside commit() under the data_parts lock. For a
    /// content-addressed disk this is where a freshly-written part is published to its FINAL manifest
    /// ref; renameParts() runs LOCK-FREE in the replicated paths, so the publish happens off the
    /// data_parts lock (B151).
    if (metadata_storage->isContentAddressed())
    {
        metadata_transaction->moveDirectory(from_path, to_path);
        return;
    }

    operations_to_execute.push_back([from_path, to_path](MetadataTransactionPtr tx)
    {
        tx->moveDirectory(from_path, to_path);
    });
}
```

- [ ] **Step 4: Publish the final ref in `ContentAddressedTransaction::moveDirectory`'s staged-re-key branch**

In `ContentAddressedTransaction.cpp`, in the PART-DIR branch of `moveDirectory` (the `if (!src->ref.empty() && src->file.empty() && !dst->ref.empty() && dst->file.empty())` block), inside the staged-re-key sub-branch `if (auto src_it = parts.find(src_key); src_it != parts.end())`, AFTER `parts.erase(src_it);` (the end of the re-key) and BEFORE the trailing `republishRef(...)`, publish the destination staging:

```cpp
            parts.erase(src_it);

            /// B151: this is a freshly-written part being finalized tmp->final (the only shape that
            /// has a STAGED source here — DETACH/ATTACH/delete_tmp renames have a COMMITTED source and
            /// miss this branch). renameParts() runs lock-free, so publish the FINAL ref NOW (off the
            /// data_parts lock) and mark it published; commit() will skip it. Single publish — the
            /// tmp ref was never durably published, so the republishRef below is a no-op.
            publishStaging(dst->ns, dst->ref, parts[dst_key]);
        }

        /// Move any COMMITTED source ref (DETACH/ATTACH/delete_tmp/merge-result committed rename).
        /// Absent (the staged-publish case above, or a pure staged/tmp move) = nothing durable to move.
        republishRef(src->ns, src->ref, dst->ns, dst->ref);
        return;
```

(The exact surrounding lines are in `ContentAddressedTransaction.cpp` ~686-724; insert the `publishStaging` call as the last statement inside the `if (src_it found)` block, after `parts.erase(src_it);`. `dst->ns` is a `Cas::RootNamespace`; `dst_key` is `{dst->ns.string(), dst->ref}`.)

- [ ] **Step 5: Build**

Run: `cd build && ninja unit_tests_dbms > build_t2.log 2>&1`; subagent summarizes.
Expected: clean build.

- [ ] **Step 6: Run the new test + the CA suite**

Run: `build/src/unit_tests_dbms --gtest_filter='CaTransactionLockScope.*:CaWiring*:Cas*' > build/test_t2.log 2>&1`; subagent summarizes.
Expected: both `CaTransactionLockScope` tests PASS; `CaWiring*` all green (the standard write→commit path still works — when a part is written and committed WITHOUT a separate `moveDirectory`, `commit()` still publishes it via the unpublished fallback); CA suite green except the 3 known-failing baseline tests. If a `CaWiring` test that does write→commit-without-rename breaks, that's the fallback path — confirm `commit()` still publishes unpublished stagings.

- [ ] **Step 7: Commit**

```bash
git add src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp \
        src/Disks/tests/gtest_ca_transaction.cpp
git commit -m "CA B151: publish the final manifest ref at the lock-free rename (eager moveDirectory); commit() skips published"
```

---

## TASK 3 — Adversarial review + full CA suite

**Files:** none (review + test).

- [ ] **Step 1: Run the full CA + disk suite**

Run: `build/src/unit_tests_dbms --gtest_filter='Cas*:CaWiring*:CaTransaction*:DiskObjectStorage*' > build/test_t3.log 2>&1`; subagent summarizes.
Expected: green except the 3 known-failing baseline tests. Pay attention to any DETACH/ATTACH/projection/move-partition CA test — these exercise the moveDirectory branches that must NOT spuriously publish.

- [ ] **Step 2: Adversarial review (subagent)**

Dispatch a code-review subagent (or the `ubrella-clickhose-review` skill) on the Task 1+2 diff against the spec's §3 correctness obligations. Specifically verify: (a) the staged-re-key publish fires ONLY for fresh-write finalization (not DETACH/ATTACH/`delete_tmp`/projection); (b) the `published` flag is set on publish and carried through any further re-key, so `commit()` never double-publishes nor skips an unpublished part; (c) `publishStaging` is idempotent; (d) the eager `moveDirectory` dispatch doesn't break any path that relied on the rename being a deferred `operations_to_execute` lambda (e.g. ordering vs other queued ops, undo()).

- [ ] **Step 3: Address any review findings, re-run, commit if changed.**

---

## TASK 4 — Soak re-validation

**Files:** `utils/ca-soak/`.

- [ ] **Step 1: Rebuild the server**

Run: `cd build && ninja clickhouse > build_server.log 2>&1`; subagent summarizes.
Expected: clean build of `build/programs/clickhouse`.

- [ ] **Step 2: Run the aggressive soak + the responsiveness poller**

From `utils/ca-soak/`, launch the 2h re-validation (same config as the B151/T9 baseline) and the poller:
```bash
cd utils/ca-soak
SEED=20260614 DURATION=2h WORKERS=6 MAX_POOL_GB=25 METRICS="logs/b151_$(date +%Y%m%dT%H%M%S).db" bash scripts/run_24h.sh > logs/b151_run_$(date +%Y%m%dT%H%M%S).log 2>&1 &
bash scripts/revalidate_poll.sh > logs/b151_poll_driver.log 2>&1 &
```

- [ ] **Step 3: Measure vs the B151/T9 baseline**

From the poller TSV (`logs/revalidate_poll_*.tsv`): the slow-poll fraction (was ~12%, max 220s) should drop sharply — ideally no 60s `system.parts` timeouts. While running, sample `system.trace_log` on a node under load and confirm **no** `WriteBufferFromS3::finalizeImpl` appears under `MergeTreeData::Transaction::commit(DataPartsLock&)` (the publish is no longer under the lock):
```sql
SELECT count() FROM system.trace_log
WHERE trace_type='Real' AND event_time > now() - INTERVAL 30 MINUTE
  AND arrayExists(x -> demangle(addressToSymbol(x)) LIKE '%Transaction::commit(%DataPartsLock%', trace)
  AND arrayExists(x -> demangle(addressToSymbol(x)) LIKE '%WriteBufferFromS3::finalizeImpl%', trace);
```
(Expected: 0 or near-0, vs the B151 baseline where this was the dominant exclusive-lock holder.) Also note whether the B152 post-fault settling improved.

- [ ] **Step 4: Record results + commit**

Append the measured deltas (slow-poll %, max `system.parts` latency, the trace-log under-lock-publish count, post-fault settling) to `docs/superpowers/deferred_backlog/cas-mergetree-integration.md` (mark B151 resolved/validated) and the progress report `docs/superpowers/reports/2026-06-13-unattended-progress.md`. Commit.

- [ ] **Step 5: Tear down**

```bash
cd utils/ca-soak && docker compose down -v
```

---

## Self-Review

**Spec coverage:**
- §2.2(1) eager `moveDirectory` dispatch → Task 2 Step 3. ✓
- §2.2(2) publish in staged-re-key branch → Task 2 Step 4. ✓
- §2.2(3) `PartStaging.published` → Task 1 Step 2. ✓
- §2.2(4) `commit()` skips published → Task 1 Step 3 (via `publishStaging` early-return) + Task 2. ✓
- §2.3 single publish (no double-write) → publish lands on final ref directly; tmp never published (Task 2 Step 4 comment + test `PublishHappensAtRenameNotCommit` asserts tmp not resolvable). ✓
- §3 correctness obligations → Task 3 Step 2 adversarial review + the `CommittedRefMoveDoesNotSpuriouslyPublish` test. ✓
- §5 testing (wiring-level publish-at-rename, commit-no-op, tmp-not-published, DETACH-no-spurious) → Task 2 tests. ✓
- §5 soak re-validation → Task 4. ✓

**Placeholder scan:** Task 2 Step 1 notes "verify `makeLocalObjectStorageForTest`/route prefix against `gtest_ca_wiring.cpp`" — a real verification step, not a TODO (the helper + pattern are confirmed to exist). No "TBD"/"implement later".

**Type consistency:** `publishStaging(const Cas::RootNamespace &, const std::string &, PartStaging &) -> bool` is declared (Task 1 Step 2) and used identically in `commit()` (Task 1 Step 3) and `moveDirectory` (Task 2 Step 4). `PartStaging.published` is set only inside `publishStaging` and read by its own early-return guard. `dst->ns` / `dst_key`/`parts[dst_key]` match the existing `moveDirectory` branch variables.
