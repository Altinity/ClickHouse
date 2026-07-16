# CAS One-Pipeline Disk Transaction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give a content-addressed (CA) disk transaction ONE mutable transaction-private overlay that every mutating operation updates at call time and every read through the transaction observes, with the disk `commit` as the only durable publication — removing the eager/deferred split without adding any new transaction phase or part-storage method.

**Architecture:** `DiskObjectStorageTransaction` stays one class. A generic `dispatch()` funnel routes every mutating method either into the existing FIFO `operations_to_execute` queue (ordinary object storage) or straight into the metadata transaction at call time (CA, gated by `IMetadataStorage::transactionIsStagingOverlay()`). A generic `IMetadataTransaction::tryCreateWriteBuffer` hook lets CA own its hash-on-write buffer. All CA overlay/publication behavior stays inside `ContentAddressedTransaction`; its `commit` materializes the overlay. No `precommit` API, no `publishStagedData`, no change to `IDataPartStorage::precommitTransaction` or the Replicated `Keeper` call sequence.

**Tech Stack:** C++ (ClickHouse), gtest (`unit_tests_dbms`), praktika stateless jobs, `utils/ca-soak` phase-3 soak, `Common/FailPoint` fault injection.

## Global Constraints

Copied verbatim from the design (`docs/superpowers/specs/2026-07-15-cas-txn-one-pipeline-design.md`) and the task brief. Every task's requirements implicitly include this section.

- **No new** `IDiskTransaction::precommit`, `IMetadataTransaction::precommit`, or `IDataPartStorage::publishStagedData`.
- **No change** to `IDataPartStorage::precommitTransaction` meaning or its call sites (stays a noop for `DataPartStorageOnDiskFull`).
- **No change** to `MergeTreeData::Transaction::renameParts` or the Replicated `Keeper` call sequence (rename → `tryMultiNoThrow` → `Transaction::commit` → `commitTransaction`/disk `commit`).
- **The only shared-code delta allowed** is: (a) one generic eager-overlay capability `IMetadataStorage::transactionIsStagingOverlay()` (default `false`, `true` for CA); (b) one generic write-buffer hook `IMetadataTransaction::tryCreateWriteBuffer(...)` (default `nullptr`); (c) routing every mutating `DiskObjectStorageTransaction` method through a generic `dispatch()` funnel; (d) a release-build assertion that eager transactions never populate `operations_to_execute`. All CA-specific overlay/publication behavior stays under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`.
- **Branch** `cas-gc-rebuild`. NO rebase/amend/push; add a new commit per task. Do not commit to `master`.
- **C++ style:** Allman braces (opening brace on a new line). Never use `sleep` to fix a race. Say "ASan" not "ASAN"; "exception" not "crash" for logical errors.
- **Wrap literal SQL/class/function/log names in inline code** in comments and commit messages. Write a function itself as `f`, not `f()`.
- **New tests:** for stateless SQL/`.sh` use `./tests/queries/0_stateless/add-test <name>`; add new tests, do not extend existing ones (except where this plan explicitly rewrites a behavior-locking CA gtest whose asserted behavior this refactor inverts). Do not add `no-*` tags unless strictly necessary.
- **Build/test discipline:** redirect `ninja`/test output to a log file under the build dir; use a subagent to analyze the log and return a concise summary. No `-j`/`nproc` with `ninja`.

---

## File Structure

**Shared (upstream) code — the only files that receive a shared-code delta:**

- `src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h` — add `transactionIsStagingOverlay()` (on `IMetadataStorage`, default `false`) and `tryCreateWriteBuffer(...)` (on `IMetadataTransaction`, default `nullptr`).
- `src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.h` — add the `dispatch()` template funnel; declaration only.
- `src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp` — route every mutating method through `dispatch()`; call the write-buffer hook at the top of `writeFileImpl`; add the release-build empty-queue assertion in `commit`/`tryCommit`; DELETE the per-method CA branches (`writeFile` CA block, `createHardLink`, disk-layer `moveDirectory` branch, `isEagerContentAddressedUnlink` + its 6 callers, `moveFile`/`replaceFile` CA comments).

**CA-owned code — where all CA overlay/publication behavior lives:**

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.{h,cpp}` — override `transactionIsStagingOverlay()` → `true`.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.{h,cpp}` — the heart of this change:
  - `moveDirectory` tmp→final becomes a pure re-key (stops early publish);
  - remove `rename_published_refs` + its destructor drop and the `published`-at-rename machinery;
  - add a `pending_ref_ops` overlay for committed-ref drop/move/replace, materialized in `commit` and consulted by reads (DESIGN-TENSION-gated — see below);
  - implement `tryCreateWriteBuffer` (moving the CA write path, append RMW, inline/blob split, autocommit, and the lifetime pin in from the disk layer).

**Tests:**

- `src/Disks/tests/gtest_ca_transaction.cpp` — rewrite the three `CaTransactionLockScope` tests that lock in the OLD "publish at rename" behavior; add overlay read-your-writes / op-order / commit-compensation tests.
- `tests/integration/test_cas_insert_fault_recovery/` — NEW: the fault-injection regression (audit 4).
- Existing stateless gates: `tests/queries/0_stateless/01603_remove_column_ttl.sql`, `tests/queries/0_stateless/02346_text_index_materialization.sql`, and the `02941_*` mutation/alter tests.

---

## Plan-Time Audit Findings

These were performed against the code on branch `cas-gc-rebuild` and are folded into the tasks below.

### Audit 1 — Inventory of mutations applied directly to a live ref / namespace file / mountpoint

Current CA operations that touch DURABLE state at call time (not staged), with their required overlay representation:

| Operation | Current durable-at-call-time site | Required representation |
|---|---|---|
| tmp→final part publish | `ContentAddressedTransaction.cpp:1125` `publishStaging(...)` inside `moveDirectory` + `:1129` `rename_published_refs.emplace_back` | Pure re-key of the overlay; publish moves to `commit` (Task 1.1 / Phase 2) |
| committed-ref drop | `moveDirectory`→ n/a; `removeDirectory` `:818` `dropRefIfPresent`; `removeRecursive` `:843/:848/:857/:865/:876/:882` `dropRefIfPresent`/`dropNamespace` | Overlay `pending_ref_ops` (drop), materialized in `commit` — Task 1.2/1.3 (DESIGN-TENSION-gated) |
| committed-ref move / repoint | `moveDirectory` `:1151` `republishRef`; RENAME TABLE `:1007-1011` `republishRef`+`putNamespaceFile`+`dropNamespace` | Overlay `pending_ref_ops` (move/rename), materialized in `commit` — Task 1.3/1.4 (DESIGN-TENSION-gated) |
| committed-content-file delete | `unlinkFile` `:1319` `content_removed.insert` | ALREADY staged (removal mark, resolved at publish). No change. |
| verbatim table-file write | `writeFile` non-part `:651` `putNamespaceFile` (autocommit, durable on finalize) | Immediate autocommit class — stays immediate (see Design Tension 6) |
| verbatim table-file move | `moveFile` `:1186-1187` `putNamespaceFile`+`removeNamespaceFile` | Immediate autocommit class — stays immediate |
| verbatim table-file / mountpoint delete | `unlinkFile` `:1327/:1331` `removeNamespaceFile`/`removeMountpointObject` | Immediate autocommit class — stays immediate |

The B183 scratch-ref drop (`moveDirectory:1144` `dropRefIfPresent(src)`) drops a ref durably published by a *separate* nested sub-storage; it is not this transaction's ref and stays a call-time drop (Task 1.1).

### Audit 2 — Reads after write / delete / rename / replace / projection re-key / committed-ref move

Transaction-scoped reads are served by `ContentAddressedTransaction::tryGetInFlightStorageObjects` / `tryReadFileInFlight` / `tryGetInFlightFileSize` / `hasInFlightDirectory` / `listInFlightDirectory` (`ContentAddressedTransaction.cpp:447-578`), reached from `DataPartStorageOnDiskFull` (`exists`/`iterate`/`getFileSize`/`prepareRead`, per upstream-patch-inventory §`DataPartStorageOnDiskFull.cpp`). Findings:

- **After write / projection re-key:** covered today — staged `entries` (and the `.tmp_proj`→`.proj` prefix re-key at `moveDirectory:1041-1043`) are visible via `findStagedEntry`. Task 1.3 adds an explicit read-your-writes test after a tmp→final re-key.
- **After committed-content-file delete:** the removal mark (`content_removed`) is NOT consulted by the in-flight read methods today. A read through the transaction of a file removed via a removal mark still resolves it from the committed manifest. This is acceptable today because the mark path is only used by the whole-part fast-removal storm (immediately followed by a ref-drop) and by ATTACH `removeVersionMetadata` (which does not read the removed file back). Task 1.4 adds a guard test; no code change unless the test fails.
- **After committed-ref drop / move (DDL):** NOT consulted by the in-flight read methods today (they only resolve staged part files). If Tasks 1.2-1.4 defer DDL ref ops to commit, reads through the transaction must additionally answer "ref dropped/moved". This is new read-path surface — folded into Tasks 1.2/1.3 and flagged in Design Tension 1.

### Audit 3 — B183 temporary text-index behavior without early publication

`createTemporaryTextIndexStorage` (`src/Storages/MergeTree/TextIndexUtils.cpp:502-510`) writes scratch under `<part>/text_index_tmp/` through a SEPARATE `DataPartStorageOnDiskFull` whose own `commitTransaction` durably publishes a committed `<part>` ref containing only scratch files (call sites: `MergeTask.cpp:2235-2236`/`:2845`, `MutateTask.cpp:1764`/`:1886`). The current guard is `ContentAddressedTransaction::moveDirectory:1132-1146`: on a staged-source tmp→final finalize it calls `dropRefIfPresent(src->refKey())` instead of `republishRef`, so the scratch ref is discarded and the real manifest stands.

**Migration gate:** when the tmp→final publish moves out of `moveDirectory` into `commit` (Task 1.1), the scratch-ref drop must STAY in `moveDirectory` (it targets the *nested sub-storage's* already-committed ref, independent of when THIS transaction publishes). Task 1.1 keeps `dropRefIfPresent(src)` in the `had_staged_source` branch and only removes the `publishStaging`+`rename_published_refs` lines. The gate test is `tests/queries/0_stateless/02346_text_index_materialization.sql` under CA-default plus a transaction-level assertion (Task 1.5).

### Audit 4 — Fault-injection regression: termination after successful `Keeper` multi, before disk `commit`

Commit ordering (`ReplicatedMergeTreeSink.cpp`): `renameTempPartAndAdd(..., rename_in_transaction=true)` under `lockParts()` (`:959`) → `transaction.renameParts()` off-lock (`:975`) → `zookeeper->tryMultiNoThrow` (`:984`) → on `ZOK`, `transaction.commit()` (`:989`) → `MergeTreeData::Transaction::commit` re-takes `lockParts()` (`MergeTreeData.cpp:8775`) → `IDataPartStorage::commitTransaction()` → disk `DiskObjectStorageTransaction::commit()` (`MergeTreeData.cpp:8792`). CA publication (`ContentAddressedTransaction::commit`) happens inside that final disk `commit`.

Existing failpoint `disk_object_storage_fail_commit_metadata_transaction` (`Common/FailPoint.cpp:142`) throws inside `DiskObjectStorageTransaction::commit()` (`:753-758`) BEFORE `metadata_transaction->commit()` — i.e. after the `Keeper` multi already returned `ZOK`, the CA publish never runs. On restart, `checkPartsImpl` (`StorageReplicatedMergeTree.cpp:1936`) finds the part in the replica's `Keeper` part set but absent on disk (`:1966-1968`), enqueues it (`:2146` `setBrokenPartsToEnqueueFetchesOnLoading`); the part-check thread searches other replicas (`ReplicatedMergeTreePartCheckThread.cpp:504`), else `onPartIsLostForever` (`:527`) → `createEmptyPartInsteadOfLost` (`StorageReplicatedMergeTree.cpp:11195`) with `lost_part_count` bumped (`:11334-11345`).

**There is no existing integration/stateless test exercising this window** (only the C++ gtest `gtest_disk_object_storage.cpp:414/574/697`). Task 2.3 adds one: a 2-replica CA cluster where the fault forces the part missing-on-disk on replica A after the multi committed, restart A, assert the part is FETCHED from replica B intact (ordinary recovery, no CA-specific mechanism). This documents that CA rides the existing path; it is a regression test, not a new recovery mechanism.

### Audit 5 — Multi-ref commit failure and existing-ref repoint

`ContentAddressedTransaction::commit` (`:392-409`) tracks only refs it CREATED (`created_refs`, populated when `publishStaging` returns `true`) and, on exception, `dropRefBestEffort` on exactly those. `publishStaging` returns `!ref_existed` for the write path (`:370-373`) and `false` for the repoint path (`:347`). Therefore a failed repoint of an EXISTING ref never enters `created_refs` and is never dropped — the existing invariant already satisfies "dropping an existing ref is not a valid rollback for a failed repoint." Task 2.2 adds a characterization test to lock this in across the Phase-1/2 changes; no code change unless the test fails.

### Audit 6 — `writeFileUsingBlobWritingFunction` and `copyFile`

Both call `metadata_transaction->generateObjectKeyForPath(...)` eagerly at call time (`DiskObjectStorageTransaction.cpp:541` and `:677`), which CA implements as `notYet("generateObjectKeyForPath")` (`ContentAddressedTransaction.cpp:424-427`, throws `NOT_IMPLEMENTED`). So both are ALREADY rejected on CA before any metadata effect. The plan preserves this: their trailing metadata effect is routed through `dispatch()` for uniformity (Task 3.1) but is unreachable on CA because `generateObjectKeyForPath` throws first. NO new fallback behavior is added. Task 3.5's empty-queue assertion is not tripped because CA throws before `push_back`.

### Audit 7 — Empty-covering-part `commitTransaction` workaround

`MergeTreeData.cpp:5724-5746` (`removePartsInRangeFromWorkingSetAndGetPartsToRemoveFromZooKeeper`, introduced by `6a0e506533c1` "CAS M6 B61(b)") calls `getDataPartStorage().commitTransaction()` (CA-only) BETWEEN the empty part's rename-into-transaction and the in-memory `transaction.rollback`. The empty cover is deliberately made Outdated (not Active) via `rollback`, so it never flows through `MergeTreeData::Transaction::commit` (`:8790-8792`) — the only other place a precommitted part's disk transaction is committed. Under THIS design there is NO `precommit`, and publication happens ONLY in the disk `commit`; the rollback path never calls the disk `commit`, so the hand-placed `commitTransaction()` is the ONLY thing that publishes the empty cover's ref.

**Decision: KEEP the workaround unchanged.** It is NOT made redundant by moving publication into `commit` (moving publication into `commit` is exactly why it is still needed on the rollback path). This DIVERGES from `upstream-patch-inventory.md`, which classifies this hunk as class-A "dies-with-one-pipeline" — but that classification assumed a `precommit` that fires before the `data_parts` lock, which this design rejects. See Design Tension 2. Task 4.2 documents the decision; no code change.

---

## Design Tensions

Flagged per the brief. None is treated as a silent deviation.

### Tension 1 (PRIMARY — scope/risk of the committed-ref DDL overlay)

The transaction model (§Transaction Model, §`moveDirectory` Responsibilities) requires committed-ref DROP/MOVE/REPLACE and RENAME TABLE to update the overlay at call time and have "the durable ref operation occur in `commit`." Today these execute DURABLY at call time (`removeDirectory`→`dropRefIfPresent`, `moveDirectory`→`republishRef`, RENAME TABLE→`republishRef`+`dropNamespace`), and the CA transaction has no undo for them. The `dispatch()` funnel (Phase 3) makes them EAGER, but "eager" for CA must mean "update the transaction-private overlay," not "mutate the durable pool" — so honoring the spec requires a new `pending_ref_ops` overlay materialized in `commit`, plus new read-path surface (Audit 2).

The §Motivation bugs (`01603`, B58, B63) are ALL in the part-build write path, NOT in DDL ref ops. Deferring DDL ref-drops to commit also interacts with the empty-covering-part workaround (Audit 7) and the DROP/DETACH/ATTACH flows in ways that are the most likely to regress. **Recommendation (least-surprising, no silent deviation):** land the motivation-aligned core first (Tasks 1.1, 1.5, all of Phase 2/3/4/Final), which delivers the invariant for the part-build path; implement the DDL-ref overlay (Tasks 1.2-1.4) behind an explicit review gate and confirm with the design owner whether DDL ref ops are in-scope for the initial landing or a follow-up. Tasks 1.2-1.4 are written in full so they are ready to execute once confirmed. This is flagged, not skipped.

### Tension 2 (inventory divergence on the empty-cover workaround)

`upstream-patch-inventory.md` marks `MergeTreeData::removePartsInRangeFromWorkingSet`'s `commitTransaction` as class-A (dies with the refactor). Under the no-`precommit` design it must STAY (Audit 7). The plan keeps it and documents why. The inventory's "de-patching order" §1/§5 and the class-A count are stale where they assume `precommit`.

### Tension 3 (whole-part-atomicity sites do NOT convert to `precommit`+`commit`)

The inventory flags `DataPartStorageOnDiskBase::freeze`, `MergeTreeData::restorePartFromBackup`, and the B58 projection commit sites as "may shrink to a `precommit`+`commit` pair." This design adds no `precommit`, so they STAY as single-`commit` whole-part transactions. Phase 4 confirms; no change.

### Tension 4 (write-buffer hook needs the owning disk transaction)

Moving the transaction lifetime pin (`shared_from_this`) and the autocommit `commit()` call into CA (§Write-Buffer Hook) requires the hook to reach the owning `DiskObjectStorageTransaction`. `IMetadataTransaction` does not otherwise know its owner. **Resolution:** the hook takes one extra generic parameter `const std::shared_ptr<IDiskTransaction> & owner` (forward-declared, no new include). This is the minimal coupling that keeps ALL CA write logic in CA while the hook stays generic. Recorded here because the spec's illustrative signature omits `owner`.

### Tension 5 (publication moves under the `data_parts` lock)

With publication only in `commit`, CA manifest build + blob upload + promote (remote I/O) run inside `MergeTreeData::Transaction::commit` under `lockParts()` (`MergeTreeData.cpp:8775/8792`). The spec explicitly accepts this (§Why There Is No Disk Precommit: "moving remote I/O out of `data_parts` is a general commit-positioning/performance problem, not a reason to add a CA-only correctness phase"). No action; recorded so a reviewer does not mistake it for a regression.

### Tension 6 (verbatim files: overlay vs. autocommit)

§Overlay Responsibilities lists "verbatim table-level and mountpoint mutations" as overlay responsibilities, but §Write-Buffer Hook says verbatim writes are "durable on finalize (no commit involvement)." A verbatim file written durably-on-finalize but deleted via a commit-staged mark would re-introduce the two-timeline split this design eliminates. **Resolution:** treat verbatim table-level/mountpoint files as a self-consistent IMMEDIATE (autocommit) class — writes durable-on-finalize, deletes/moves durable-at-call-time — NOT deferred to commit. The transaction still answers reads for them consistently. This matches current behavior and the write-hook section. Flagged as my reading of an internal spec inconsistency.

---

# Phase 1 — Complete the overlay

**Phase gate:** CA gtests (`unit_tests_dbms --gtest_filter='Ca*:Cas*'`); read-your-writes and operation-order tests; B183 text-index regression (`02346_text_index_materialization` under CA-default + the Task 1.5 assertion).

### Task 1.1: tmp→final `moveDirectory` becomes a pure re-key (publish moves to `commit`)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp:1120-1146` (the `had_staged_source` branch of `moveDirectory`)
- Test: `src/Disks/tests/gtest_ca_transaction.cpp:54-104` (rewrite the three lock-scope tests)

**Interfaces:**
- Consumes: `openTxStorage`, `writeFileTx`, `storage->existsDirectory/existsFile`, `tx->moveDirectory`, `tx->commit(NoCommitOptions{})` (existing helpers in `gtest_ca_transaction.cpp:22-37`).
- Produces: `moveDirectory` no longer publishes the final ref; the existing `commit`→`publishStaging` loop (`ContentAddressedTransaction.cpp:395-400`) publishes it. `rename_published_refs` is left populated-nowhere (removed in Phase 2).

- [ ] **Step 1: Rewrite the behavior-locking test to assert publish-at-commit**

Replace `CaTransactionLockScope.PublishHappensAtRenameNotCommit` (`gtest_ca_transaction.cpp:54-75`) with:

```cpp
/// [TXN-ONE-PIPELINE] A freshly-written part is published by commit(), NOT at the tmp->final rename.
/// moveDirectory only re-keys the transaction overlay; the durable ref appears at commit().
TEST(CaTransactionLockScope, PublishHappensAtCommitNotRename)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    writeFileTx(*tx, "uui/uuid-1/tmp_insert_all_1_1_0/data.bin", "content-A");

    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/tmp_insert_all_1_1_0"));
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));

    tx->moveDirectory("uui/uuid-1/tmp_insert_all_1_1_0", "uui/uuid-1/all_1_1_0");

    /// Re-key only: the final ref is NOT durable yet.
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));

    tx->commit(DB::NoCommitOptions{});

    /// Published by commit().
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_1_1_0/data.bin"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-1/all_1_1_0/data.bin"), 9u);
}
```

- [ ] **Step 2: Run it to confirm it fails against current code**

Run: `<build>/src/unit_tests_dbms --gtest_filter='CaTransactionLockScope.PublishHappensAtCommitNotRename'`
Expected: FAIL at the `EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_1_1_0"))` after `moveDirectory` (current code publishes early).

- [ ] **Step 3: Make `moveDirectory` re-key only**

In `ContentAddressedTransaction.cpp`, inside the `if (had_staged_source)` block (`:1120-1146`), delete the early-publish lines and keep the B183 scratch drop. The block that currently reads (after the entry-merge loop):

```cpp
            /// B151: this is a freshly-written part being finalized tmp->final ...
            publishStaging(dst->ns, dst->ref, parts[dst_key]);
            /// B151 rollback safety: ...
            rename_published_refs.emplace_back(dst->ns, dst->ref);
        }

        if (had_staged_source)
        {
            /// ... B183 scratch-ref clobber prevention ...
            metadata_storage.partAccess().dropRefIfPresent(src->refKey());
            return;
        }
```

becomes (drop the two early-publish lines; the re-keyed `parts[dst_key]` is now published by `commit`):

```cpp
            /// [TXN-ONE-PIPELINE]: a freshly-written part finalized tmp->final is re-keyed in the
            /// overlay above (entries/marks/pending blobs/build moved src->dst). The durable publish
            /// happens only in commit() (the existing publishStaging loop), NOT here — renameParts()
            /// no longer publishes off the data_parts lock. No early-published ref to compensate on
            /// abort (see ~ContentAddressedTransaction).
        }

        if (had_staged_source)
        {
            /// B183: a nested text-index sub-storage (MergeTask/MutateTask createTemporaryTextIndexStorage)
            /// may have DURABLY published a committed scratch ref at THIS part's own path holding only
            /// `<part>/text_index_tmp/` files. That ref is not ours and is not staged; drop it now so the
            /// overlay we publish in commit() is the authoritative manifest. Independent of our publish
            /// timing (it targets an already-committed foreign ref), so it stays a call-time drop.
            metadata_storage.partAccess().dropRefIfPresent(src->refKey());
            return;
        }
```

- [ ] **Step 4: Rewrite the two remaining lock-scope tests**

Replace `RenamePublishedRefDroppedOnAbandon` (`:79-91`) and `RenamePublishedRefSurvivesCommit` (`:94-104`) with:

```cpp
/// [TXN-ONE-PIPELINE] An abandoned transaction (destructed without commit) never published, so the
/// final ref is simply absent — no early-published ref to drop.
TEST(CaTransactionLockScope, AbandonedPartLeavesNoRef)
{
    auto storage = openTxStorage();
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-1/tmp_insert_all_3_3_0/data.bin", "abandoned");
        tx->moveDirectory("uui/uuid-1/tmp_insert_all_3_3_0", "uui/uuid-1/all_3_3_0");
        EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_3_3_0"));   /// not published at the rename
        /// tx goes out of scope WITHOUT commit().
    }
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_3_3_0"));
}

/// [TXN-ONE-PIPELINE] commit() publishes the re-keyed part.
TEST(CaTransactionLockScope, RefPublishedByCommit)
{
    auto storage = openTxStorage();
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-1/tmp_insert_all_4_4_0/data.bin", "kept");
        tx->moveDirectory("uui/uuid-1/tmp_insert_all_4_4_0", "uui/uuid-1/all_4_4_0");
        tx->commit(DB::NoCommitOptions{});
    }
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/all_4_4_0"));
}
```

Leave `CommittedRefMoveDoesNotSpuriouslyPublish` (`:107-124`) unchanged — it asserts a committed-ref rename (`delete_tmp_`) goes via `republishRef` and remains valid.

- [ ] **Step 5: Run the lock-scope tests**

Run: `<build>/src/unit_tests_dbms --gtest_filter='CaTransactionLockScope.*'`
Expected: PASS (4 tests).

- [ ] **Step 6: Run the full CA battery for no regressions**

Run (redirect to a log; use a subagent to summarize): `<build>/src/unit_tests_dbms --gtest_filter='Ca*:Cas*' > <build>/test_ca_battery_t1_1.log 2>&1`
Expected: all CA suites pass.

- [ ] **Step 7: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp src/Disks/tests/gtest_ca_transaction.cpp
git commit -m "cas txn-one-pipeline: moveDirectory tmp->final is a pure re-key; publish moves to commit"
```

### Task 1.5: B183 text-index migration gate (transaction-level assertion)

**Files:**
- Test: `src/Disks/tests/gtest_ca_transaction.cpp` (add a test near the other lock-scope tests)

**Interfaces:**
- Consumes: `openTxStorage`, `writeFileTx`, `storage->store()->resolveRef`, `storage->store()->readManifest`, `metadata_storage.partAccess()` via the storage.
- Produces: proof that a staged-source finalize drops a foreign scratch ref at the part path and commit publishes the authoritative manifest.

- [ ] **Step 1: Write the failing test**

```cpp
/// [TXN-ONE-PIPELINE] B183 migration gate: a scratch ref durably published at the part's own path by a
/// nested sub-storage must be dropped on the staged-source tmp->final finalize, and commit() must
/// publish the AUTHORITATIVE staged manifest (not the scratch content).
TEST(CaTransactionLockScope, StagedFinalizeDropsForeignScratchRef)
{
    auto storage = openTxStorage();

    /// Simulate the nested text-index sub-storage: a SEPARATE transaction durably publishes a committed
    /// ref at the final part path holding only a scratch file.
    {
        auto scratch_tx = storage->createTransaction();
        writeFileTx(*scratch_tx, "uui/uuid-7/all_1_1_0/text_index_tmp/scratch.bin", "scratch");
        scratch_tx->moveDirectory("uui/uuid-7/all_1_1_0", "uui/uuid-7/all_1_1_0"); // no-op self-move; publish via commit
        scratch_tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsDirectory("uui/uuid-7/all_1_1_0"));

    /// The real part build: stage the authoritative data.bin under tmp, then finalize tmp->final.
    auto tx = storage->createTransaction();
    writeFileTx(*tx, "uui/uuid-7/tmp_merge_all_1_1_0/data.bin", std::string(50000, 'D'));
    tx->moveDirectory("uui/uuid-7/tmp_merge_all_1_1_0", "uui/uuid-7/all_1_1_0");
    tx->commit(DB::NoCommitOptions{});

    /// The published manifest is the authoritative one (has data.bin), not the scratch ref.
    const auto ns = storage->liveNamespace("uuid-7");
    const auto resolved = storage->store()->resolveRef(ns, "all_1_1_0");
    ASSERT_TRUE(resolved.has_value());
    const auto manifest = storage->store()->readManifest(resolved->manifest_id);
    EXPECT_TRUE(findByName(manifest.entries, "data.bin"));
    EXPECT_FALSE(findByName(manifest.entries, "scratch.bin"));
}
```

- [ ] **Step 2: Run it**

Run: `<build>/src/unit_tests_dbms --gtest_filter='CaTransactionLockScope.StagedFinalizeDropsForeignScratchRef'`
Expected: PASS with the Task 1.1 code (the `dropRefIfPresent(src)` in `had_staged_source` is preserved). If the self-move publish shape does not durably create the scratch ref, adjust the scratch setup to use two distinct part names and a committed→final rename so a committed ref exists at `all_1_1_0` before the real build; the assertion (authoritative manifest wins) is the invariant.

- [ ] **Step 3: Commit**

```bash
git add src/Disks/tests/gtest_ca_transaction.cpp
git commit -m "cas txn-one-pipeline: B183 migration-gate test — staged finalize drops foreign scratch ref"
```

### Task 1.3: Read-your-writes after a tmp→final re-key

**Files:**
- Test: `src/Disks/tests/gtest_ca_transaction.cpp`

- [ ] **Step 1: Write the test**

```cpp
/// [TXN-ONE-PIPELINE] After a tmp->final re-key, a read THROUGH the open transaction resolves the
/// staged content under the FINAL path (read-your-writes), before commit().
TEST(CaTransactionLockScope, ReadYourWritesAfterReKey)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);

    writeFileTx(*tx, "uui/uuid-3/tmp_insert_all_1_1_0/checksums.txt", "the-checksums");
    tx->moveDirectory("uui/uuid-3/tmp_insert_all_1_1_0", "uui/uuid-3/all_1_1_0");

    /// The overlay answers the final path before commit.
    auto buf = ca_tx.tryReadFileInFlight("uui/uuid-3/all_1_1_0/checksums.txt", DB::ReadSettings{}, std::nullopt);
    ASSERT_NE(buf, nullptr);
    std::string got;
    DB::readStringUntilEOF(got, *buf);
    EXPECT_EQ(got, "the-checksums");
    EXPECT_TRUE(ca_tx.hasInFlightDirectory("uui/uuid-3/all_1_1_0"));
}
```

Add `#include <IO/readReadableWritable.h>` if `readStringUntilEOF` is not already visible; it is declared in `<IO/ReadHelpers.h>` — prefer that.

- [ ] **Step 2: Run it**

Run: `<build>/src/unit_tests_dbms --gtest_filter='CaTransactionLockScope.ReadYourWritesAfterReKey'`
Expected: PASS (re-key moves the entry to the `dst` staging key, which `findStagedEntry` resolves).

- [ ] **Step 3: Commit**

```bash
git add src/Disks/tests/gtest_ca_transaction.cpp
git commit -m "cas txn-one-pipeline: read-your-writes test after tmp->final re-key"
```

### Task 1.4: Operation-order guard (create → delete → create; delete-then-read)

**Files:**
- Test: `src/Disks/tests/gtest_ca_transaction.cpp`

- [ ] **Step 1: Write the test**

```cpp
/// [TXN-ONE-PIPELINE] Program order in the overlay: create -> delete -> create leaves the file PRESENT
/// (no delayed delete fires after the later create); delete of a staged file makes it absent to reads.
TEST(CaTransactionLockScope, OverlayProgramOrder)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);

    writeFileTx(*tx, "uui/uuid-5/tmp_insert_all_1_1_0/a.txt", "v1");
    ca_tx.unlinkFile("uui/uuid-5/tmp_insert_all_1_1_0/a.txt", /*if_exists=*/false, /*should_remove_objects=*/true);
    EXPECT_EQ(ca_tx.tryReadFileInFlight("uui/uuid-5/tmp_insert_all_1_1_0/a.txt", DB::ReadSettings{}, std::nullopt), nullptr);

    writeFileTx(*tx, "uui/uuid-5/tmp_insert_all_1_1_0/a.txt", "v2");
    tx->moveDirectory("uui/uuid-5/tmp_insert_all_1_1_0", "uui/uuid-5/all_1_1_0");
    tx->commit(DB::NoCommitOptions{});

    ASSERT_TRUE(storage->existsFile("uui/uuid-5/all_1_1_0/a.txt"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-5/all_1_1_0/a.txt"), 2u);
}
```

- [ ] **Step 2: Run it**

Run: `<build>/src/unit_tests_dbms --gtest_filter='CaTransactionLockScope.OverlayProgramOrder'`
Expected: PASS. (`unlinkFile` erases the staged entry; the second write re-stages it; no residual removal mark for a staged-then-rewritten file.)

- [ ] **Step 3: Commit**

```bash
git add src/Disks/tests/gtest_ca_transaction.cpp
git commit -m "cas txn-one-pipeline: overlay program-order test (create/delete/create)"
```

### Tasks 1.2 (DDL ref-drop overlay), 1.3-DDL (DDL ref-move overlay), 1.4-DDL (RENAME TABLE overlay) — DESIGN-TENSION-GATED

> **REVIEW GATE (Design Tension 1):** Do NOT execute these three tasks until the design owner confirms DDL ref ops are in-scope for the initial landing. They defer committed-ref DROP/MOVE and RENAME TABLE from durable-at-call-time to commit-materialized, add a `pending_ref_ops` overlay and new read-path surface, and interact with the empty-covering-part workaround (Audit 7) and DROP/DETACH/ATTACH flows. The motivation bugs do not involve these ops. If confirmed deferred, the invariant still holds for the part-build path (the design's core) via Tasks 1.1/1.5/1.3/1.4 and all of Phases 2-4.

The full task bodies (overlay struct `pending_ref_ops` with `{Drop, Move, Replace}` variants keyed by `(ns, ref)`; `removeDirectory`/`removeRecursive`/`moveDirectory`-committed/RENAME-TABLE staging into it; `commit` materialization AFTER `publishStaging`; read methods answering "ref dropped/moved") are specified in Appendix A so they are ready to execute on confirmation, but are intentionally kept out of the default execution path.

---

# Phase 2 — Publish only in commit

**Phase gate:** commit-failure compensation tests; the fault-injection regression that termination after the `Keeper` multi enters the ordinary missing-part recovery path.

### Task 2.1: Delete the B151 early-publication machinery

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h:129-134` (remove `rename_published_refs` field), `:112` (the `published`-at-rename comment stays factual for the commit skip-guard — see below)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp:98-103` (remove the destructor drop loop over `rename_published_refs`)

**Interfaces:**
- Consumes: after Task 1.1, `rename_published_refs` is populated nowhere.
- Produces: no early-publish state remains; `PartStaging::published` stays only as the commit-loop idempotency guard (never set true before commit now).

- [ ] **Step 1: Confirm `rename_published_refs` has no remaining writer**

Run: `grep -n rename_published_refs src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.{h,cpp}`
Expected: only the field declaration (`.h:134`) and the destructor read loop (`.cpp:102`) remain (the writer at `.cpp:1129` was removed in Task 1.1).

- [ ] **Step 2: Remove the destructor drop loop**

In `~ContentAddressedTransaction` (`.cpp:98-103`) delete:

```cpp
    /// Drop refs we published early at the rename (B151) ...
    for (const auto & [ns, ref] : rename_published_refs)
        metadata_storage.partAccess().dropRefBestEffort({ns, ref});
```

- [ ] **Step 3: Remove the field and its comment**

In `.h`, delete the `rename_published_refs` member and its doc comment (`:129-134`).

- [ ] **Step 4: Re-evaluate `PartStaging::published`**

`published` (`.h:112`) is now only ever set inside `publishStaging`/`commit`. Keep the field (it is the commit-loop idempotency guard and the "nothing staged" benign no-op guard at `.cpp:287/295`). Update its comment to drop the "published at the lock-free rename" phrasing:

```cpp
        bool published = false;                    /// set by publishStaging during commit(); the
                                                   /// commit loop is idempotent (never re-publishes).
```

- [ ] **Step 5: Build and run the CA battery**

Run (redirect + subagent-summarize): `ninja unit_tests_dbms` then `<build>/src/unit_tests_dbms --gtest_filter='Ca*:Cas*' > <build>/test_ca_battery_t2_1.log 2>&1`
Expected: build clean; all CA suites pass (the Phase-1 tests already assert publish-at-commit/abandon-leaves-no-ref).

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
git commit -m "cas txn-one-pipeline: remove B151 early-publication machinery (rename_published_refs + destructor drop)"
```

### Task 2.2: Commit-failure compensation — existing-ref repoint is never dropped (Audit 5)

**Files:**
- Test: `src/Disks/tests/gtest_ca_transaction.cpp`

- [ ] **Step 1: Write the characterization test**

```cpp
/// [TXN-ONE-PIPELINE] Audit 5: on a multi-part commit where a later publish fails, only refs this
/// commit CREATED are rolled back; a repoint of an already-existing ref is NEVER dropped as rollback.
TEST(CaTransactionLockScope, CommitRollbackSparesPreexistingRef)
{
    auto storage = openTxStorage();

    /// Pre-existing committed part.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-8/tmp_insert_all_1_1_0/data.bin", "orig");
        tx->moveDirectory("uui/uuid-8/tmp_insert_all_1_1_0", "uui/uuid-8/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsDirectory("uui/uuid-8/all_1_1_0"));

    /// A transaction that repoints the existing ref (standalone write on the committed part). Even if a
    /// LATER part in the same commit were to fail, the existing ref must survive. Assert the invariant
    /// directly: publishStaging on a committed ref returns false (not a created ref), so it is never in
    /// created_refs and never dropped.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-8/all_1_1_0/metadata_version.txt", "1");
        tx->commit(DB::NoCommitOptions{});
    }
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-8/all_1_1_0"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-8/all_1_1_0/data.bin"));   /// original content carried forward
}
```

- [ ] **Step 2: Run it**

Run: `<build>/src/unit_tests_dbms --gtest_filter='CaTransactionLockScope.CommitRollbackSparesPreexistingRef'`
Expected: PASS (repoint path returns `false`; original `data.bin` carried forward by `publishStaging`'s merge). If it fails, the compensation logic regressed — fix `commit`/`publishStaging`, do not weaken the test.

- [ ] **Step 3: Commit**

```bash
git add src/Disks/tests/gtest_ca_transaction.cpp
git commit -m "cas txn-one-pipeline: commit-rollback spares pre-existing ref (Audit 5 characterization)"
```

### Task 2.3: Fault-injection regression — post-multi termination enters ordinary missing-part recovery (Audit 4)

**Files:**
- Create: `tests/integration/test_cas_insert_fault_recovery/test.py`
- Create: `tests/integration/test_cas_insert_fault_recovery/configs/content_addressed.xml` (CA storage policy default), plus a 2-replica `Keeper`/replicated setup mirroring an existing CA integration test if one exists; otherwise model on `tests/integration/test_content_addressed_*`.

**Interfaces:**
- Consumes: failpoint `disk_object_storage_fail_commit_metadata_transaction` (registered `Common/FailPoint.cpp:142`, `ONCE`), enabled via `SYSTEM ENABLE FAILPOINT`.
- Produces: proof CA rides the existing fetch/lost-part path; no CA-specific recovery.

- [ ] **Step 1: Scaffold the test with the failure and recovery assertion**

Design (write the actual `test.py` following the repo's integration-test conventions — `cluster.add_instance(..., main_configs=[...], with_zookeeper=True)`, two replicas `r1`/`r2` of one `ReplicatedMergeTree` on a CA disk):

```python
def test_post_multi_termination_recovers_via_fetch(started_cluster):
    r1, r2 = node1, node2
    for n in (r1, r2):
        n.query("CREATE TABLE t (a UInt64) ENGINE=ReplicatedMergeTree('/clickhouse/t','{replica}') ORDER BY a "
                "SETTINGS storage_policy='content_addressed'")
    # Force the disk commit to throw AFTER the Keeper multi succeeds on r1.
    r1.query("SYSTEM ENABLE FAILPOINT disk_object_storage_fail_commit_metadata_transaction")
    # The INSERT's Keeper multi commits the part to ZK, then the disk commit throws -> part in ZK, absent on disk.
    r1.query_and_get_error("INSERT INTO t VALUES (1)")
    # r2 fetched/received the part normally (it is in ZK).
    r2.query("SYSTEM SYNC REPLICA t")
    assert r2.query("SELECT count() FROM t") == "1\n"
    # Restart r1: checkPartsImpl sees the part in its ZK part-set but absent on disk -> enqueue -> fetch from r2.
    r1.restart_clickhouse()
    r1.query("SYSTEM SYNC REPLICA t")
    assert r1.query("SELECT count() FROM t") == "1\n"            # recovered by ordinary fetch, no data loss
    assert r1.query("SELECT value FROM system.events WHERE event='ReplicatedDataLoss'") in ("", "0\n")
```

- [ ] **Step 2: Run it**

Run (from repo root, redirect + subagent-summarize): `python -m ci.praktika run "integration" --test test_cas_insert_fault_recovery > tmp/test_fault_recovery.log 2>&1`
Expected: PASS. If the single INSERT of one value does not durably commit to `Keeper` before the disk commit (e.g. quorum/async settings), adjust so the multi is confirmed (the sink's `transaction.commit()` at `ReplicatedMergeTreeSink.cpp:989` runs only after `multi_code==ZOK`). The invariant under test: after termination, r1 recovers the part through the ordinary replication-queue fetch, not a CA-specific path.

- [ ] **Step 3: Commit**

```bash
git add tests/integration/test_cas_insert_fault_recovery
git commit -m "cas txn-one-pipeline: fault-injection regression — post-multi termination uses ordinary missing-part recovery"
```

---

# Phase 3 — Dispatch funnel and write-buffer hook

**Phase gate:** build; CA battery; CA-default stateless; targeted `01603_remove_column_ttl` and `02941_*`.

### Task 3.1: Add the eager-overlay capability and the `dispatch()` funnel; route the simple queue-only methods

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h:308-316` (add `transactionIsStagingOverlay()`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h` (override → `true`)
- Modify: `src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.h:130-137` (add the `dispatch` template)
- Modify: `src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp` — convert `createDirectory`, `createDirectories`, `removeDirectory`, `removeRecursive`, `removeSharedRecursive`, `setReadOnly`, `setLastModified`, `chmod`, `truncateFile`, `moveFile`, `replaceFile` to `dispatch(...)`.

**Interfaces:**
- Produces: `dispatch(op)` runs `op(metadata_transaction)` immediately when `metadata_storage->transactionIsStagingOverlay()`, else `operations_to_execute.emplace_back(std::move(op))`.

- [ ] **Step 1: Add the capability (default false)**

In `IMetadataStorage.h`, next to `isContentAddressed()` (`:312`):

```cpp
    /// True when a transaction from this storage stages every mutation into a transaction-private
    /// overlay at call time (eager) rather than queuing effects for FIFO replay in commit. When true,
    /// DiskObjectStorageTransaction routes every mutating method straight to the metadata transaction
    /// and keeps its own operations_to_execute queue empty. Default false (ordinary object storage).
    virtual bool transactionIsStagingOverlay() const { return false; }
```

In `ContentAddressedMetadataStorage.h`, add the override near `isContentAddressed`:

```cpp
    bool transactionIsStagingOverlay() const override { return true; }
```

- [ ] **Step 2: Add the `dispatch` template**

In `DiskObjectStorageTransaction.h`, in the `private:`/`protected:` region:

```cpp
    /// [TXN-ONE-PIPELINE] Route one metadata effect either into the FIFO replay queue (ordinary object
    /// storage) or straight to the metadata transaction at call time (eager staging overlay, e.g. CA).
    template <typename Operation>
    void dispatch(Operation && operation)
    {
        if (metadata_storage->transactionIsStagingOverlay())
            operation(metadata_transaction);
        else
            operations_to_execute.emplace_back(std::forward<Operation>(operation));
    }
```

- [ ] **Step 3: Convert the simple methods**

Replace each `operations_to_execute.push_back([...](MetadataTransactionPtr tx){ ... });` with `dispatch([...](MetadataTransactionPtr tx){ ... });` for the methods listed above. Example (`moveFile`, `.cpp:148-159`) — delete the stale B182 comment and route:

```cpp
void DiskObjectStorageTransaction::moveFile(const String & from_path, const String & to_path)
{
    dispatch([from_path, to_path](MetadataTransactionPtr tx)
    {
        tx->moveFile(from_path, to_path);
    });
}
```

Do the same for `replaceFile` (delete its B182 comment), `createDirectory`, `createDirectories`, `removeDirectory`, `removeRecursive`, `removeSharedRecursive`, `setReadOnly`, `setLastModified`, `chmod`, `truncateFile`.

- [ ] **Step 4: Build and run the CA battery + a smoke of ordinary object storage**

Run (redirect + subagent-summarize): `ninja unit_tests_dbms` then `<build>/src/unit_tests_dbms --gtest_filter='Ca*:Cas*:DiskObjectStorage*' > <build>/test_t3_1.log 2>&1`
Expected: build clean; CA and ordinary-disk transaction gtests pass. For CA, `moveFile`/`replaceFile` now run eagerly (matching the verbatim immediate class, Design Tension 6) and the DDL `removeDirectory`/`removeRecursive` run eagerly at call time (unchanged durable behavior since Tasks 1.2-1.4 are gated off; the durable drop happens at call time exactly as before this refactor, just no longer via a queued lambda).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.h src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp
git commit -m "cas txn-one-pipeline: add transactionIsStagingOverlay capability + dispatch funnel; route simple methods"
```

### Task 3.2: Route the `removeFile` family through `dispatch`; delete `isEagerContentAddressedUnlink` and `stagesPartFileUnlink`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp:179-317` (the anonymous-namespace helper + `removeFile`/`removeSharedFile`/`removeSharedFileIfExists`/`removeFileIfExists`/`removeSharedFiles`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.{h,cpp}` (delete `stagesPartFileUnlink`)

**Interfaces:**
- Produces: every `remove*` variant routes the `unlinkFile` effect through `dispatch`. For CA this runs `unlinkFile` eagerly for ALL paths (part-file marks AND verbatim immediate deletes), uniformly — replacing the old part-file-only eager branch. The old classification predicate is gone.

- [ ] **Step 1: Delete the helper**

Remove the anonymous namespace `isEagerContentAddressedUnlink` (`.cpp:179-201`).

- [ ] **Step 2: Route each `remove*` variant**

`removeFile`:

```cpp
void DiskObjectStorageTransaction::removeFile(const std::string & path)
{
    dispatch([path](MetadataTransactionPtr tx)
    {
        tx->unlinkFile(path, /*if_exists=*/false, /*should_remove_objects=*/true);
    });
}
```

`removeSharedFile`:

```cpp
void DiskObjectStorageTransaction::removeSharedFile(const std::string & path, bool keep_shared_data)
{
    dispatch([path, keep_shared_data](MetadataTransactionPtr tx)
    {
        tx->unlinkFile(path, /*if_exists=*/false, /*should_remove_objects=*/!keep_shared_data);
    });
}
```

`removeSharedFileIfExists`:

```cpp
void DiskObjectStorageTransaction::removeSharedFileIfExists(const std::string & path, bool keep_shared_data)
{
    dispatch([path, keep_shared_data](MetadataTransactionPtr tx)
    {
        tx->unlinkFile(path, /*if_exists=*/true, /*should_remove_objects=*/!keep_shared_data);
    });
}
```

`removeFileIfExists`:

```cpp
void DiskObjectStorageTransaction::removeFileIfExists(const std::string & path)
{
    dispatch([path](MetadataTransactionPtr tx)
    {
        tx->unlinkFile(path, /*if_exists=*/true, /*should_remove_objects=*/true);
    });
}
```

`removeSharedFiles` — route each element through `dispatch` (preserving the per-file `should_remove_objects` computation):

```cpp
void DiskObjectStorageTransaction::removeSharedFiles(const RemoveBatchRequest & files, bool keep_all_batch_data, const NameSet & file_names_remove_metadata_only)
{
    for (const auto & [path, if_exists] : files)
    {
        const bool should_remove_objects = !keep_all_batch_data && !file_names_remove_metadata_only.contains(fs::path(path).filename());
        dispatch([path, if_exists, should_remove_objects](MetadataTransactionPtr tx)
        {
            tx->unlinkFile(path, if_exists, should_remove_objects);
        });
    }
}
```

`removeSharedRecursive` was converted in Task 3.1.

- [ ] **Step 3: Delete `stagesPartFileUnlink`**

Remove the declaration (`ContentAddressedTransaction.h:83-89`) and definition (`.cpp:234-240`). Confirm no other caller: `grep -rn stagesPartFileUnlink src/` → expect zero after removal.

- [ ] **Step 4: Run the removal-behavior CA tests + `01603`**

Run: `<build>/src/unit_tests_dbms --gtest_filter='CaTransactionRemove.*:CaTransactionRepoint.*'` (expect PASS — `UnlinkStormThenDirDropIsOneRefDrop`, `SurgicalUnlinkRepoints` still green: the storm runs eagerly, `removeDirectory` clears the marks).
Then build `clickhouse`, symlink at `ci/tmp/clickhouse`, and run (redirect + subagent-summarize): `python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" --test 01603_remove_column_ttl > tmp/test_01603.log 2>&1`
Expected: `01603_remove_column_ttl` PASS (the eager-unlink ordering that this test originally forced is now the uniform funnel behavior).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
git commit -m "cas txn-one-pipeline: route removeFile family through dispatch; drop eager-unlink predicate"
```

### Task 3.3: Add the `tryCreateWriteBuffer` hook; move the CA write path into `ContentAddressedTransaction`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h` (add the hook to `IMetadataTransaction`, default `nullptr`; forward-declare `struct IDiskTransaction;`)
- Modify: `src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp:337-430` (`writeFileImpl` — replace the CA block with a hook call + generic wrap)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.{h,cpp}` (implement `tryCreateWriteBuffer`)

**Interfaces:**
- Produces: `IMetadataTransaction::tryCreateWriteBuffer(const std::shared_ptr<IDiskTransaction> & owner, const std::string & path, size_t buf_size, WriteMode mode, const WriteSettings & settings, bool autocommit)` → `std::unique_ptr<WriteBufferFromFileBase>` (default `nullptr`). CA returns the fully-wrapped buffer (hash-on-write + append RMW + inline/blob + autocommit/pin using `owner`).

- [ ] **Step 1: Add the hook (default nullptr)**

In `IMetadataStorage.h`, above `class IMetadataStorage` add `struct IDiskTransaction;` forward declaration (or at namespace top). In `IMetadataTransaction`, near `generateObjectKeyForPath`:

```cpp
    /// [TXN-ONE-PIPELINE] Optional per-metadata write buffer. Returns a ready-to-use buffer when the
    /// metadata implementation owns its write mechanism (e.g. a content-addressed hash-on-write buffer
    /// whose blob key is known only after the last byte). `owner` is the disk transaction that must be
    /// kept alive for the returned buffer's lifetime and, when `autocommit`, committed from the finalize
    /// callback. Default nullptr: the caller uses the generic streaming write path unchanged.
    virtual std::unique_ptr<WriteBufferFromFileBase> tryCreateWriteBuffer(
        const std::shared_ptr<IDiskTransaction> & /*owner*/,
        const std::string & /*path*/, size_t /*buf_size*/, WriteMode /*mode*/,
        const WriteSettings & /*settings*/, bool /*autocommit*/) { return nullptr; }
```

- [ ] **Step 2: Call the hook at the top of `writeFileImpl` and delete the CA block**

In `DiskObjectStorageTransaction::writeFileImpl` (`.cpp:337`), after the `enriched_settings` line and BEFORE the append-mode check, insert:

```cpp
    if (auto buffer = metadata_transaction->tryCreateWriteBuffer(
            shared_from_this(), path, buf_size, mode, enriched_settings, autocommit))
        return buffer;
```

Then DELETE the entire `if (metadata_storage->isContentAddressed()) { ... }` block (`.cpp:359-430`), including the append-on-part-file rejection, the autocommit-inline branch, and the keep-alive pin — all of it moves into CA (Step 3). The generic append-mode `NOT_IMPLEMENTED` check (`.cpp:355-357`) stays but its `&& !metadata_storage->isContentAddressed()` clause is removed (CA now rejects/handles append inside the hook):

```cpp
    if (mode == WriteMode::Append && !metadata_storage->supportWritingWithAppend())
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Disk does not support WriteMode::Append");
```

Note: CA's `supportWritingWithAppend()` returns false, so a CA append reaches the hook first (which services verbatim append via RMW / rejects part-file append) — verify the hook is called before this check for CA. Because the hook is called before the append check, CA append is fully handled in the hook; the generic check only rejects append on non-CA storages that lack it.

- [ ] **Step 3: Implement CA `tryCreateWriteBuffer`**

Declare in `ContentAddressedTransaction.h` (public, override):

```cpp
    std::unique_ptr<WriteBufferFromFileBase> tryCreateWriteBuffer(
        const std::shared_ptr<IDiskTransaction> & owner,
        const std::string & path, size_t buf_size, WriteMode mode,
        const WriteSettings & settings, bool autocommit) override;
```

Define in `ContentAddressedTransaction.cpp` by moving the logic that currently lives in `DiskObjectStorageTransaction::writeFileImpl`'s CA block (the append rejection, autocommit-inline vs content-blob decision, the pin/commit wrap) and delegating the inner buffer to the existing `ContentAddressedTransaction::writeFile` (`.cpp:631`). The pin and autocommit `commit()` now use `owner`:

```cpp
std::unique_ptr<WriteBufferFromFileBase> ContentAddressedTransaction::tryCreateWriteBuffer(
    const std::shared_ptr<IDiskTransaction> & owner,
    const std::string & path, size_t buf_size, WriteMode mode,
    const WriteSettings & settings, bool autocommit)
{
    /// Append is serviceable (read-modify-rewrite) only for a non-part / table-level verbatim file
    /// (handled inside writeFile). A part file is a content blob or a whole-rewritten inline entry, so
    /// append on a part-file path is unsupported.
    if (mode == WriteMode::Append && ContentAddressed::isPartFilePath(path))
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Disk does not support WriteMode::Append for content part files");

    /// Autocommit cannot work for a CONTENT BLOB part file: a part's blobs publish only when commit()
    /// runs. A small INLINE-eligible part file IS autocommittable (a standalone one-shot write). Verbatim
    /// / table-level files (not part files) are durable on finalize regardless.
    if (autocommit && ContentAddressed::isPartFilePath(path))
    {
        auto p = ContentAddressed::parsePartFilePath(path);
        if (!p || p->file.empty() || ContentAddressed::partFileMustStayBlob(p->file))
            throw Exception(ErrorCodes::NOT_IMPLEMENTED,
                "Autocommit writes are not supported for content part files on a content-addressed disk");

        auto inner = writeFile(path, buf_size, mode, settings);
        auto commit_callback = [owner](size_t) mutable { owner->commit(); };
        return std::make_unique<WriteBufferWithFinalizeCallback>(
            std::move(inner), std::move(commit_callback), path, /*create_blob_if_empty=*/true);
    }

    /// Non-autocommit (or verbatim autocommit): pin the owning disk transaction for the buffer's lifetime
    /// so the CaContentWriteBuffer / CaInlineWriteBuffer finalize callbacks (which capture `this`) never
    /// dangle if the buffer is deferred-finalized after the part storage/transaction would otherwise be
    /// torn down (the B90 CA-S3 lifetime fix, now expressed generically via `owner`).
    auto inner = writeFile(path, buf_size, mode, settings);
    auto keep_alive_callback = [owner](size_t) mutable {};
    return std::make_unique<WriteBufferWithFinalizeCallback>(
        std::move(inner), std::move(keep_alive_callback), path, /*create_blob_if_empty=*/true);
}
```

Add includes to `ContentAddressedTransaction.cpp`: `<Disks/IO/WriteBufferWithFinalizeCallback.h>`, `<Disks/IDiskTransaction.h>`, `<Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h>` (if not already present).

- [ ] **Step 4: Build and run the write-path CA tests**

Run (redirect + subagent-summarize): `ninja unit_tests_dbms` then `<build>/src/unit_tests_dbms --gtest_filter='CaTransaction*:Cas*' > <build>/test_t3_3.log 2>&1`
Expected: build clean; `CaTransactionInlining.EagerFileInlinedDataBinBlobbed` and all lock-scope/repoint tests pass. The `writeFileTx` helper calls `ContentAddressedTransaction::writeFile` directly, so it is unaffected; the disk-layer path now goes through the hook.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
git commit -m "cas txn-one-pipeline: generic tryCreateWriteBuffer hook; move CA write path into ContentAddressedTransaction"
```

### Task 3.4: Delete the disk-layer `moveDirectory` and `createHardLink` CA branches

**Files:**
- Modify: `src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp:129-146` (`moveDirectory`), `:571-592` (`createHardLink`)

**Interfaces:**
- Produces: `moveDirectory` and `createHardLink` route through `dispatch` uniformly; the `isContentAddressed()` branches are gone.

- [ ] **Step 1: Convert `moveDirectory`**

```cpp
void DiskObjectStorageTransaction::moveDirectory(const std::string & from_path, const std::string & to_path)
{
    dispatch([from_path, to_path](MetadataTransactionPtr tx)
    {
        tx->moveDirectory(from_path, to_path);
    });
}
```

- [ ] **Step 2: Convert `createHardLink`**

```cpp
void DiskObjectStorageTransaction::createHardLink(const std::string & src_path, const std::string & dst_path)
{
    dispatch([src_path, dst_path](MetadataTransactionPtr tx)
    {
        tx->createHardLink(src_path, dst_path);
    });
}
```

- [ ] **Step 3: Route `copyFileImpl`'s and `writeFileUsingBlobWritingFunction`'s trailing metadata effect through `dispatch`**

For uniformity (and so the Task 3.5 assertion holds trivially), change the trailing `operations_to_execute.push_back(...)` in `copyFileImpl` (`.cpp:711`) and `writeFileUsingBlobWritingFunction` (`.cpp:555`) to `dispatch(...)`. Per Audit 6 these are unreachable on CA (they throw at `generateObjectKeyForPath` earlier), so behavior is unchanged; on ordinary storage `dispatch` queues exactly as before.

- [ ] **Step 4: Build and run CA battery + confirm `createHardLink` projection carry-forward (B58/B63)**

Run (redirect + subagent-summarize): `ninja unit_tests_dbms` then `<build>/src/unit_tests_dbms --gtest_filter='Ca*:Cas*' > <build>/test_t3_4.log 2>&1`
Expected: build clean; CA battery green (the createHardLink eager staging that made carried-forward projections visible to `loadProjections` is preserved — it now runs via `dispatch` eager for CA).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp
git commit -m "cas txn-one-pipeline: route moveDirectory/createHardLink/copyFile through dispatch; delete per-method CA branches"
```

### Task 3.5: Release-build assertion that eager transactions never populate `operations_to_execute`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp:730-772` (`commit`), `:774-848` (`tryCommit`)

**Interfaces:**
- Produces: a fail-closed check (release-build, not `chassert`) at the start of `commit`/`tryCommit`: an eager (staging-overlay) transaction with a non-empty queue throws `LOGICAL_ERROR` before publication.

- [ ] **Step 1: Add the guard to `commit`**

At the top of `DiskObjectStorageTransaction::commit()`:

```cpp
    if (metadata_storage->transactionIsStagingOverlay() && !operations_to_execute.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "An eager staging-overlay transaction must not queue deferred operations "
            "(a mutating method bypassed dispatch): {} queued", operations_to_execute.size());
```

- [ ] **Step 2: Add the same guard to `tryCommit`**

At the top of `DiskObjectStorageTransaction::tryCommit(...)`, add the identical check.

- [ ] **Step 3: Rationale note**

Use a real `throw` (not `chassert`) per the guidance that `chassert` is a no-op in release builds and does not fail-close (`feedback_review_blindspots_shards_chassert`). This is the invariant that guarantees no CA op recreates the two-timeline split.

- [ ] **Step 4: Build and run the CA battery**

Run (redirect + subagent-summarize): `ninja unit_tests_dbms` then `<build>/src/unit_tests_dbms --gtest_filter='Ca*:Cas*' > <build>/test_t3_5.log 2>&1`
Expected: build clean; no CA test trips the guard (every CA op now dispatches eagerly).

- [ ] **Step 5: Apply the same policy to `MultipleDisksObjectStorageTransaction`**

`MultipleDisksObjectStorageTransaction` inherits `commit`/`tryCommit`, so the guard already applies. Its `copyFile` override delegates to `copyFileImpl` (routed via `dispatch` in Task 3.4). No extra code; confirm by `grep -n "operations_to_execute" src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp` shows only `dispatch`-fed writers remain.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp
git commit -m "cas txn-one-pipeline: release-build assertion that eager transactions never populate the disk queue"
```

### Task 3.6: Phase-3 gate — CA-default stateless + targeted tests

- [ ] **Step 1: Build `clickhouse`, symlink for praktika**

Run (redirect + subagent-summarize): `ninja clickhouse > <build>/build_clickhouse_t3.log 2>&1` then `ln -sf <build>/programs/clickhouse ci/tmp/clickhouse`.

- [ ] **Step 2: Run the targeted stateless tests under CA-default**

Run (redirect + subagent-summarize): `python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" --test "01603_remove_column_ttl 02346_text_index_materialization 02941" > tmp/test_t3_targeted.log 2>&1`
Expected: `01603_remove_column_ttl`, `02346_text_index_materialization` (B183 gate), and the `02941_*` tests PASS. (Confirm which `02941_*` the design intends — candidates: `02941_variant_type_alters.sh`, `02941_projections_external_aggregation.sql`; run the alters/projection ones as the mutation/alter gate. Flag to the design owner if disambiguation is needed.)

- [ ] **Step 3: Commit (gate note only, if any test needed a fix)**

If a fix was required, commit it with a message citing the failing test; otherwise no commit (gate is a checkpoint).

---

# Phase 4 — Tail de-patch

**Phase gate:** build and CA battery.

### Task 4.1: Delete the redundant `StorageReplicatedMergeTree::checkAlterPartitionIsPossible` override (class C)

**Files:**
- Modify: `src/Storages/StorageReplicatedMergeTree.cpp` (delete the pure-delegation override body), `src/Storages/StorageReplicatedMergeTree.h` (delete the declaration)

- [ ] **Step 1: Confirm it is pure delegation**

Run: `grep -n "checkAlterPartitionIsPossible" src/Storages/StorageReplicatedMergeTree.{h,cpp}`
Read the body; confirm it only calls `MergeTreeData::checkAlterPartitionIsPossible(...)` (per `upstream-patch-inventory.md` §Class C surprises 2-3).

- [ ] **Step 2: Delete both**

Remove the override definition and its `.h` declaration. The base `MergeTreeData::checkAlterPartitionIsPossible` (virtual) is reached by dynamic dispatch.

- [ ] **Step 3: Build**

Run (redirect + subagent-summarize): `ninja clickhouse > <build>/build_t4_1.log 2>&1`
Expected: clean.

- [ ] **Step 4: Commit**

```bash
git add src/Storages/StorageReplicatedMergeTree.cpp src/Storages/StorageReplicatedMergeTree.h
git commit -m "cas txn-one-pipeline (tail de-patch): remove redundant checkAlterPartitionIsPossible override"
```

### Task 4.2: Document that the empty-cover workaround and whole-part-atomicity sites STAY (Audits 7, Tensions 2-3)

**Files:**
- Modify: `src/Storages/MergeTree/MergeTreeData.cpp:5724-5738` (comment only — update to reference the one-pipeline design)

- [ ] **Step 1: Update the comment (no behavior change)**

Amend the comment above the CA-only `commitTransaction()` at `:5735-5738` to reference this design and Audit 7 (the call is load-bearing because publication is only in `commit`, and this rollback path never reaches `MergeTreeData::Transaction::commit`). Do NOT remove the call.

- [ ] **Step 2: Confirm freeze/restore/B58 sites unchanged**

Run: `grep -n "restore_tx->commit\|owned_transaction\|commitTransaction" src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp src/Storages/MergeTree/MergeTreeData.cpp src/Storages/MergeTree/MergeTask.cpp src/Storages/MergeTree/MutateTask.cpp src/Storages/MergeTree/MergeProjectionPartsTask.cpp`
Confirm none needs a `precommit`+`commit` conversion (this design has no `precommit`). No code change.

- [ ] **Step 3: Build + CA battery**

Run (redirect + subagent-summarize): `ninja unit_tests_dbms clickhouse > <build>/build_t4_2.log 2>&1` then `<build>/src/unit_tests_dbms --gtest_filter='Ca*:Cas*' > <build>/test_t4_2.log 2>&1`
Expected: clean; CA battery green.

- [ ] **Step 4: Commit**

```bash
git add src/Storages/MergeTree/MergeTreeData.cpp
git commit -m "cas txn-one-pipeline (tail de-patch): document empty-cover commitTransaction stays under the one-pipeline design"
```

---

# Final — Full CA-default stateless + phase-3 ca-soak

**Gate:** all green; no CA operation bypasses the overlay or populates the disk queue (enforced at runtime by the Task 3.5 assertion).

### Task 5.1: Full CA-default stateless run

- [ ] **Step 1: Build + symlink**

Run (redirect + subagent-summarize): `ninja clickhouse > <build>/build_final.log 2>&1` then `ln -sf <build>/programs/clickhouse ci/tmp/clickhouse`.

- [ ] **Step 2: Run the full CA-default stateless job**

Run (redirect + subagent-summarize; use the cas-test-triage skill for classification): `python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" --test "" > tmp/test_final_stateless.log 2>&1` (the full-suite run — triage failures per `reference_ca_s3_lane_ignore_tests` and `cas-test-triage`; only real CA regressions block the gate).
Expected: no CA regression attributable to this refactor. Fix or file as needed.

### Task 5.2: Phase-3 ca-soak (20 min) + metrics review

- [ ] **Step 1: Rebuild the nightly image / remount the binary per `reference_ca_soak_fresh_restart`**

From `utils/ca-soak/`: `docker compose down` / `up -d` to remount the rebuilt binary (archive host logs first if regression-watch matters).

- [ ] **Step 2: Run a time-driven phase-3 soak**

Run (from `utils/ca-soak/`, `PYTHONPATH=.`): `python3 -m soak.run --seed 1 --phase 3 --duration 20m --workers 6 --metrics soak_txn_one_pipeline_20m.db`
Expected: no wedge, no leak, no `LOGICAL_ERROR` (in particular none tripping the Task 3.5 assertion). Review `soak_txn_one_pipeline_20m.db` and the server `trace_log`/`ProfileEvents` per `reference_stateless_test_triage` (use `ProfileEvents`, not `addressToSymbol`).

- [ ] **Step 3: Report the soak result**

Summarize GREEN/findings; if a finding surfaces, open a follow-up per the scenario-results table format (`feedback_scenario_results_table_format`).

---

## Appendix A — DDL ref-op overlay (Tasks 1.2-1.4, execute only on design-owner confirmation)

These bodies implement Design Tension 1's deferred DDL-ref overlay. They are specified here, out of the default execution path, so they are ready if confirmed in scope.

**Overlay structure** (in `ContentAddressedTransaction.h`, private):

```cpp
    /// [TXN-ONE-PIPELINE] Committed-ref DDL effects staged for materialization in commit(): a ref drop,
    /// a ref move (src->dst), a whole-namespace move (RENAME TABLE), or a ref replace. Keyed and ordered
    /// so commit() applies them AFTER part publication, in program order. Reads through the transaction
    /// consult this to answer "ref dropped/moved".
    enum class RefOpKind { DropRef, MoveRef, DropNamespace, MoveNamespace };
    struct PendingRefOp
    {
        RefOpKind kind;
        Cas::RootNamespace ns;
        std::string ref;            /// for DropRef/MoveRef (source)
        Cas::RootNamespace dst_ns;  /// for MoveRef/MoveNamespace
        std::string dst_ref;        /// for MoveRef
    };
    std::vector<PendingRefOp> pending_ref_ops;
```

**Staging** (replace the durable calls in `removeDirectory`/`removeRecursive`/`moveDirectory`-committed/RENAME-TABLE with `pending_ref_ops.push_back({...})`), **materialization** (a loop at the end of `commit()` after the `publishStaging` loop, applying `dropRefIfPresent`/`republishRef`/`dropNamespace` best-effort with the same partial-commit compensation contract as `created_refs`), and **reads** (extend `tryGetInFlightStorageObjects`/`existsFile`-equivalent to return "absent" for a ref with a pending `DropRef`/source of a `MoveRef`) are each a TDD sub-task mirroring Tasks 1.1-1.4. Because RENAME TABLE has no cross-namespace atomicity (`ContentAddressedTransaction.cpp:989-997`), the materialized form remains best-effort idempotent-redrive — moving it into `commit` does NOT add atomicity (consistent with §Commit And Abort: "There is no atomic backend operation spanning several refs").

**Interaction to verify before executing:** DROP PARTITION / TRUNCATE via `removePartsInRangeFromWorkingSet` (Audit 7) and DETACH/ATTACH rollback (`PartsTemporaryRename::rollBackAll` → `moveFile` → `moveDirectory`, `ContentAddressedTransaction.cpp:1216-1221`) must still produce the correct durable outcome when the ref ops are deferred to `commit`. This verification is the primary risk and the reason for the review gate.

---

## Self-Review

- **Spec coverage:** Phase 1 (overlay completion + tmp→final re-key + reads + B183 gate) → Tasks 1.1, 1.3, 1.4, 1.5 (+ gated 1.2-1.4-DDL / Appendix A). Phase 2 (publish-only-in-commit + compensation + fault-injection) → Tasks 2.1, 2.2, 2.3. Phase 3 (funnel + write hook + delete CA branches + assertion) → Tasks 3.1-3.5, gate 3.6. Phase 4 (tail de-patch) → Tasks 4.1, 4.2. Final → Tasks 5.1, 5.2. All 7 plan-time audits are folded into the audit-findings section and referenced by tasks. All 6 design tensions flagged with resolutions.
- **Constraint compliance:** no `precommit`/`publishStagedData`/`precommitTransaction` changes; only the four allowed shared-code deltas (`transactionIsStagingOverlay`, `tryCreateWriteBuffer`, `dispatch`, assertion); CA behavior stays under `ContentAddressed/`.
- **Type consistency:** `transactionIsStagingOverlay()` (storage-level), `dispatch(Operation&&)`, `tryCreateWriteBuffer(owner, path, buf_size, mode, settings, autocommit)` used identically in every task. `PartStaging::published` retained as the commit idempotency guard.
- **Placeholder scan:** exact code and paths given for every code step; the one deliberate deferral (Appendix A / Tasks 1.2-1.4-DDL) is explicitly gated on design-owner confirmation, not left vague.
- **Known soft spots to confirm during execution:** (a) the exact `02941_*` test the design intends (Task 3.6 Step 2); (b) the fault-injection test's `Keeper`-multi timing (Task 2.3 Step 2); (c) the Task 1.5 scratch-ref setup shape. Each carries an inline fallback instruction.
