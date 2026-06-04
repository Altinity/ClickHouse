# CAS MergeTree Transactions (MVCC) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`). Build to a log (`ninja -C build clickhouse > build/<log> 2>&1`, NO `-j`/`nproc`); summarize via a subagent. Bounded foreground tests (`timeout` ≤ 900), non-empty `--test`, never `clickhouse local`. This plan is **reproduction-driven**: the post-commit `txn_version.txt` rewrite sequence (autocommit ops on an active part) must be validated against the live Tier-1 run — implement the framework, then debug against real behavior.

**Goal:** Make MergeTree transactions (MVCC) work on a content-addressed (CA) disk so the ~18 gated transaction/isolation stateless tests pass.

**Architecture:** The MVCC engine is storage-agnostic and unchanged. CA must satisfy the per-part `txn_version.txt` contract: (1) decouple the transaction gate from append; (2) make the `txn_version.txt` write sequence (`createFile`/`writeFile`/`replaceFile`/`removeFile`) update the per-ref **sidecar** in place — both inside a part-build transaction (INSERT) and as standalone autocommit ops on an already-committed part (creation-CSN fill-in, removal-TID lock/unlock) — without ever republishing the part's manifest/ref/`part_id`.

**Spec:** `docs/superpowers/specs/2026-06-04-cas-mergetree-transactions-design.md`.
**Branch:** `cas-mergetree-poc` (never master). Trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Seams (verified):**
- Gate: `StorageMergeTree::supportTransaction` (`StorageMergeTree.cpp:167`) loops disks, returns false if any `!supportWritingWithAppend(disk)`. `support_transaction` set at `StorageMergeTree.cpp:201`. Base capability `IMetadataStorage::supportWritingWithAppend` (`IMetadataStorage.h:341`) returns false; `MetadataStorageFromDisk` returns true (so plain S3 has transactions).
- `txn_version.txt` write: `VersionMetadataOnDisk::storeInfoToDataPartStorage` (`VersionMetadataOnDisk.cpp:323`) → `createFile(tmp)`, `writeFile(tmp)` (`tmp = "txn_version.txt.tmp"`), `replaceFile(tmp, "txn_version.txt")`. Cleanup: `removeTmpMetadataFile` (`:296`) `readFile(tmp)` + `removeFile(tmp)`. These go through `DataPartStorageOnDiskFull` (`:207`/`:218`/`:232`/`:241`), which uses the member `transaction` if set else autocommits per op.
- Mutable-file machinery (CA): `kMutablePerPartFiles = {uuid.txt, txn_version.txt, metadata_version.txt}` + `isMutablePerPartFile` (`PartManifest.h:19/37`); `writeFile` mutable branch stages inline into `recorded_mutable` (`ContentAddressedTransaction.cpp:270`); `moveFile` mutable re-key (`:967`); `commit` (`:1018`) builds `manifest.blobs = recorded`, writes the sidecar bundle (`refMetaKey`) + per-file mutable objects (`refMutableFileKey`) + publishes the ref (`refKey`). `replaceFile` is NOT implemented (base `throwNotImplemented`, `IMetadataStorage.h:113`). `readRefPartId`/`readRefSidecarIfExists` read a committed ref/sidecar. `resolveMutableFileBytes` resolves committed mutable bytes.
- Rollback-reload: `getLastModified` part-dir branch throws `FILE_DOESNT_EXIST` when no ref (`ContentAddressedMetadataStorage.cpp:639`).
- gtests: `src/Disks/tests/gtest_content_addressed_metadata.cpp` (run `build/src/unit_tests_dbms --gtest_filter='ContentAddressed*'`).

---

## Phase 1 — the CA-side mechanism (framework, then reproduction-driven against Tier 1)

### Task 1: decouple the transaction gate from append

**Files:** Modify `src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h`, `…/ContentAddressed/ContentAddressedMetadataStorage.h`, `src/Storages/StorageMergeTree.cpp`

- [ ] **Step 1: add the capability.** In `IMetadataStorage.h`, near `supportWritingWithAppend` (`:341`):
```cpp
    /// True iff this metadata storage can persist the per-part mutable transaction file (txn_version.txt)
    /// under MVCC. Distinct from supportWritingWithAppend: transactions rewrite txn_version.txt (tmp +
    /// replaceFile), they never WriteMode::Append, so append-capability is the wrong proxy. A
    /// content-addressed disk supports the mutable txn file via its per-ref sidecar.
    virtual bool supportsTransactionalMutableFiles() const { return false; }
```
- [ ] **Step 2: override for CA.** In `ContentAddressedMetadataStorage.h` (near the other `override` predicates, ~`:41`): `bool supportsTransactionalMutableFiles() const override { return true; }`.
- [ ] **Step 3: consult it in the gate.** In `StorageMergeTree.cpp` `supportTransaction` (`:167`), change the per-disk check so a disk passes if it supports append **or** its metadata storage supports transactional mutable files. Concretely, replace the `if (!supportWritingWithAppend(disk))` rejection with a check that also accepts a CA disk:
```cpp
    for (const auto & disk : disks)
    {
        if (supportWritingWithAppend(disk))
            continue;
        /// A content-addressed disk does not support append, but persists the per-part mutable
        /// transaction file (txn_version.txt) via its per-ref sidecar, which is all MVCC needs.
        if (auto * obj = dynamic_cast<DiskObjectStorage *>(disk.get());
            obj && obj->getMetadataStorage()->supportsTransactionalMutableFiles())
            continue;
        LOG_DEBUG(log, "Disk {} does not support transactions", disk->getName());
        return false;
    }
    return true;
```
Add the needed include for `DiskObjectStorage` if absent. (Mirror the `dynamic_cast<DiskObjectStorage*>` + `getMetadataStorage()` shape already used in `Disks/supportWritingWithAppend.cpp`.)
- [ ] **Step 4: build** `ninja -C build clickhouse > build/txn_t1_build.log 2>&1; echo "exit=$?"; grep -cE "error:|FAILED:" build/txn_t1_build.log` → 0 errors. (Subagent-summarize.)
- [ ] **Step 5: commit** `CAS txn: decouple the transaction gate from append (supportsTransactionalMutableFiles)`.

### Task 2: recognize the mutable-file `.tmp` staging + implement `replaceFile` + the mutable-only commit branch

**Files:** Modify `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h`, `…/ContentAddressedTransaction.h`, `…/ContentAddressedTransaction.cpp`

This is the load-bearing task. The `txn_version.txt` write sequence must update only the per-ref sidecar — for BOTH the in-part-transaction INSERT case and the standalone autocommit case on an active part — never republishing the manifest/ref. Three coordinated pieces:

- [ ] **Step 1: treat the `.tmp` of a mutable per-part file as itself mutable-inline.** The sequence writes `txn_version.txt.tmp` (a content-shaped name) then renames it. If CA content-addresses the tmp, an autocommit `writeFile(tmp)` on an active part republishes the part with a one-file manifest (clobber). Make `isMutablePerPartFile` (`PartManifest.h:37`) also match a `<mutable>.tmp` basename so the tmp stages inline (excluded from the manifest, routed to the sidecar). Extend the predicate:
```cpp
constexpr bool isMutablePerPartFile(std::string_view file)
{
    const auto slash = file.rfind('/');
    std::string_view basename = (slash == std::string_view::npos) ? file : file.substr(slash + 1);
    /// The atomic write of a mutable per-part file goes via a sibling tmp (e.g. txn_version.txt.tmp,
    /// VersionMetadataOnDisk). Treat that tmp as mutable too, so it stages inline in the per-ref
    /// sidecar instead of content-addressing into the manifest — otherwise a standalone autocommit
    /// write of the tmp on an already-committed part would republish the part with a one-file manifest.
    std::string_view stem = basename;
    if (stem.ends_with(".tmp"))
        stem = stem.substr(0, stem.size() - 4);
    for (const auto & name : kMutablePerPartFiles)
        if (basename == name || stem == name)
            return true;
    return false;
}
```
(Verify this is `constexpr`-clean — `ends_with`/`substr` on `string_view` are constexpr in C++20. If the toolchain rejects it in a constexpr context, drop `constexpr` to a normal inline function — the predicate is not used in a constexpr context that matters.)

- [ ] **Step 2: implement `ContentAddressedTransaction::replaceFile`.** Declare in `.h` (next to `moveFile`, `:255`): `void replaceFile(const std::string & path_from, const std::string & path_to) override;`. Define in `.cpp` (after `moveFile`). Semantics = `moveFile` (re-key source→dest) but **overwrite** the destination, with mutable-aware source resolution:
```cpp
void ContentAddressedTransaction::replaceFile(const std::string & from, const std::string & to)
{
    /// replaceFile = moveFile that overwrites the destination. For a content-addressed disk a rename of
    /// an in-part file re-keys the staged entry (no object moves). The destination of the txn_version.txt
    /// write is a mutable per-part file, so the result must land in recorded_mutable (the per-ref
    /// sidecar), never the content manifest.
    auto src = ContentAddressed::parsePartFilePath(from);
    auto dst = ContentAddressed::parsePartFilePath(to);
    if (!src || src->file.empty() || !dst || dst->file.empty())
    {
        /// Fall back to the table-level / generic verbatim move (replace overwrites): reuse moveFile's
        /// non-part branch by delegating.
        moveFile(from, to);
        return;
    }
    rememberTarget(to);
    /// Drop any existing staged destination (overwrite semantics).
    recorded.erase(dst->file);
    recorded_mutable.erase(dst->file);
    /// Source staged as a mutable inline file (the common case: txn_version.txt.tmp): move the bytes.
    if (auto mit = recorded_mutable.find(src->file); mit != recorded_mutable.end())
    {
        recorded_mutable[dst->file] = std::move(mit->second);
        recorded_mutable.erase(mit);
        return;
    }
    /// Source staged as a content blob but destination is a mutable file: read the just-written bytes
    /// back and inline them (the tmp blob becomes orphaned and is GC-reclaimed). Should be rare once
    /// Step 1 recognizes mutable .tmp, but kept for safety.
    if (auto it = recorded.find(src->file); it != recorded.end())
    {
        if (ContentAddressed::isMutablePerPartFile(dst->file))
        {
            recorded_mutable[dst->file] = resolveInFlightOrCommittedBytes(from); // see Step 2a
            recorded.erase(it);
        }
        else
        {
            recorded[dst->file] = std::move(it->second);
            recorded.erase(it);
        }
        return;
    }
    /// Source not staged in THIS transaction (the standalone autocommit case across ops): resolve the
    /// committed source bytes (sidecar) and re-stage under the destination, marking the source removed.
    recorded_mutable[dst->file] = metadata_storage.resolveMutableFileBytes(from);
    recorded_mutable_removed.insert(src->file); // see Step 3 for how commit applies removals
}
```
  - [ ] **Step 2a:** add a small helper `resolveInFlightOrCommittedBytes(path)` (or reuse the B59 in-flight read `tryReadFileInFlight`/`tryGetInFlightStorageObjects` + a committed fallback) that returns the bytes of a just-recorded content blob (read the uploaded blob) or the committed file. If a suitable helper exists from B59, use it; else add a private one. NOTE: this path should be rare after Step 1 — confirm in Phase 1 debugging which branch actually fires.
  - [ ] **Step 2b:** add `std::set<std::string> recorded_mutable_removed;` to the `.h` (the set of mutable files to delete from an EXISTING sidecar at commit — for `removeFile(tmp)` and the rename's source). Wire `unlinkFile`/`removeFile` of a mutable per-part file on a committed part to add to it (see Step 3).

- [ ] **Step 3: the mutable-only commit branch.** In `commit` (`:1018`), AFTER the `if (recorded.empty() && recorded_mutable.empty()) return;` early-out and the `(table_uuid, part_name)` check, BEFORE `manifest.blobs = recorded`:
```cpp
    /// Mutable-only update of an ALREADY-COMMITTED part: the transaction staged no content blobs, only
    /// mutable per-part files (txn_version.txt / its .tmp / removals) — the MVCC creation-CSN fill-in and
    /// removal-TID lock/unlock rewrite txn_version.txt on a live part. Update only the per-ref sidecar +
    /// the per-file mutable objects in place; keep the existing manifest, part_id and ref. Republishing
    /// (the normal path below) would compute a part_id over an empty manifest and clobber the part.
    if (recorded.empty() && (!recorded_mutable.empty() || !recorded_mutable_removed.empty()))
    {
        auto existing_pid = metadata_storage.readRefPartId(table_uuid, part_name);
        if (!existing_pid)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "ContentAddressed: mutable-only commit for {}/{} with no existing ref", table_uuid, part_name);

        std::lock_guard<std::mutex> gc_guard(*metadata_storage.gc_lock);

        ContentAddressed::RefSidecar sidecar;
        if (auto existing = metadata_storage.readRefSidecarIfExists(table_uuid, part_name))
            sidecar = *existing;
        /// Apply removals first, then upserts.
        for (const auto & f : recorded_mutable_removed)
        {
            sidecar.files.erase(f);
            metadata_storage.object_storage->removeObjectIfExists(StoredObject(
                ContentAddressed::refMutableFileKey(key_prefix, metadata_storage.server_id, table_uuid, part_name, f).string()));
        }
        for (const auto & [file, bytes] : recorded_mutable)
        {
            sidecar.files[file] = bytes;
            const std::string fk = ContentAddressed::refMutableFileKey(key_prefix, metadata_storage.server_id, table_uuid, part_name, file).string();
            auto out = metadata_storage.object_storage->writeObject(StoredObject(fk), WriteMode::Rewrite);
            out->write(bytes.data(), bytes.size()); out->finalize();
        }
        /// Rewrite the sidecar bundle; the ref + manifest are untouched.
        const std::string meta_key = ContentAddressed::refMetaKey(key_prefix, metadata_storage.server_id, table_uuid, part_name).string();
        const std::string meta_bytes = sidecar.serialize();
        auto mo = metadata_storage.object_storage->writeObject(StoredObject(meta_key), WriteMode::Rewrite);
        mo->write(meta_bytes.data(), meta_bytes.size()); mo->finalize();
        if (session_open) releaseSession();
        return;
    }
```
  Also: in `unlinkFile`/`removeFile`, when removing a mutable per-part file whose part already has a committed ref (no in-flight `recorded`/`recorded_mutable` entry), add its name to `recorded_mutable_removed` so the mutable-only branch deletes it from the committed sidecar (handles `removeTmpMetadataFile`'s `removeFile(tmp)`).

- [ ] **Step 4: build** `ninja -C build clickhouse > build/txn_t2_build.log 2>&1; echo "exit=$?"; grep -cE "error:|FAILED:" build/txn_t2_build.log` → 0 errors.

- [ ] **Step 5: commit** `CAS txn: mutable .tmp staging + replaceFile + mutable-only sidecar commit branch`.

### Task 3: gtests for the mechanism

**Files:** Modify `src/Disks/tests/gtest_content_addressed_metadata.cpp`

- [ ] **Step 1:** write a part live + commit (capture `part_id`). Then a NEW transaction that `writeFile`s `txn_version.txt.tmp` + `replaceFile(tmp, txn_version.txt)` + commit. Assert: the ref still resolves to the SAME `part_id` (no clobber); the sidecar now contains `txn_version.txt` with the written bytes; the manifest is unchanged (no `txn_version.txt`/`.tmp` key in it). A second mutable-only commit overwriting `txn_version.txt` updates the bytes. A mutable-only commit on a part with NO ref throws.
- [ ] **Step 2:** build `ninja -C build unit_tests_dbms > build/txn_t3_build.log 2>&1; echo $?` and run `build/src/unit_tests_dbms --gtest_filter='ContentAddressed*' > build/txn_t3_run.log 2>&1; tail -20 build/txn_t3_run.log` → all pass.
- [ ] **Step 3: commit** `CAS txn: gtests for replaceFile + mutable-only sidecar update`.

---

## Phase 2 — Wave 1 (Tier 1, transactional INSERT): reproduction-driven

### Task 4: un-gate Tier 1 + debug to green

**Files:** Modify `tests/queries/0_stateless/01172_transaction_counters.sql`, `01173_transaction_control_queries.sql`, `02345_implicit_transaction.sql`, `01133_begin_commit_race.sh`

- [ ] **Step 1:** un-tag the four Tier-1 tests (remove `no-content-addressed-storage`, preserve other tags). Refresh symlink `ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse`.
- [ ] **Step 2: run**
```bash
cd /home/mfilimonov/workspace/ClickHouse/master
sel="01172_transaction_counters 01173_transaction_control_queries 02345_implicit_transaction 01133_begin_commit_race"
[ -n "$(echo "$sel"|tr -d ' ')" ] || { echo ABORT; exit 1; }
timeout 900 python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" --test "$sel" > build/txn_t4_run.log 2>&1
echo "exit=$?"; grep -hoE "Failed: [0-9]+, Passed: [0-9]+, Skipped: [0-9]+" build/txn_t4_run.log | tail -1
grep -E "01172|01173|02345|01133" ci/tmp/test_result.txt
```
- [ ] **Step 3: DEBUG the actual `txn_version.txt` autocommit behavior.** This is the reproduction-driven core. For each failure read the diff + the server err log (`grep -iE "txn_version|replaceFile|NOT_IMPLEMENTED|FILE_DOESNT_EXIST|CORRUPTED|clobber|no ref|part_id|sidecar" ci/tmp/var/log/clickhouse-server/clickhouse-server.err.log | tail -80`). Confirm/correct the Task-2 mechanism against reality:
  - Does the INSERT-in-txn part build set a member transaction (so the txn_version write folds into the whole-part commit)? If commit shows the mutable-only branch firing for the INSERT, the part content would be lost — verify the INSERT goes through the NORMAL commit (content in `recorded`) and the mutable file is in the sidecar.
  - Does the standalone creation-CSN fill-in (on COMMIT) reach the mutable-only branch with the existing ref intact?
  - Fix `replaceFile`/the mutable-only branch/`isMutablePerPartFile` per what the run shows. Re-run until green. Run gtests again after any CA-transaction code change.
- [ ] **Step 4: gap #2 (rollback-reload).** If a failure shows `getLastModified` (or a mutable-file stat/read) throwing `FILE_DOESNT_EXIST` on an in-flight part with no ref, harden it: route the part-dir/mutable-file stat through the in-flight transaction (B59 overlay) or answer gracefully. Only implement if it actually fires (it was collateral to gap #1 in the probe). `ContentAddressedMetadataStorage.cpp:~639`.
- [ ] **Step 5: commit** the Tier-1 un-gate + any fixes. `CAS txn: Tier-1 transactional INSERT works on CA (un-gate 01172/01173/02345/01133)`.

### Task 5: inline-CA oracle

**Files:** Create `tests/queries/0_stateless/<NNNNN>_content_addressed_transactions.sh` (via `add-test <name>.sh`)

- [ ] **Step 1:** an inline-CA-disk table (copy the disk block from `05003_content_addressed_freeze.sh`). Prove on CA: `BEGIN TRANSACTION`; INSERT; a SELECT in the SAME txn sees it; (optionally a second connection's snapshot does not — if expressible deterministically); `COMMIT`; visible; then a transaction that `DELETE`/lightweight-deletes or mutates, `ROLLBACK`, data intact. Keep output deterministic; hand-verify the reference. Needs `SET allow_experimental_transactions=...` as the existing txn tests do — copy their preamble.
- [ ] **Step 2: run on CA-default + plain** → `Passed: 1` on both (commands per the FREEZE plan's oracle step).
- [ ] **Step 3: commit** `CAS txn: inline-CA oracle (begin/insert/commit/rollback on a content-addressed disk)`.

---

## Phase 3 — Wave 2 (Tier 2, mutations/DELETE-in-txn + isolation): reproduction-driven

### Task 6: un-gate Tier 2 + iterate

**Files:** the Tier-2 tests (below).

- [ ] **Step 1:** un-tag the Tier-2 tests: `01167_isolation_hermitage.sh`, `01168_mutations_isolation.sh`, `01168_mutations_isolation_2.sh`, `01169_alter_partition_isolation_stress.sh`, `01169_old_alter_partition_isolation_stress.sh`, `01170_alter_partition_isolation.sh`, `01171_mv_select_insert_isolation_long.sh`, `01174_select_insert_isolation.sh`, `02421_truncate_isolation_no_merges.sh`, `02421_truncate_isolation_with_mutations.sh`, `02435_rollback_cancelled_queries.sh`, `03803_transaction_mutation_race.sh`, `03657_merge_tree_disk_support_transaction.sql`, `04036_backup_partition_transaction_visibility.sh`, `03752_attach_as_replicated_transaction_metadata.sh`, `03916_attach_as_replicated_implicit_transaction.sh`.
- [ ] **Step 2: run in small batches** (to keep logs readable; non-empty `--test`, `timeout 900` each) on the CA-default job, e.g. the isolation core first (`01167 01168 01169 01170 01171 01174 02421`), then the rest. Record pass/fail.
- [ ] **Step 3: reproduction-driven fixes.** Each new failure is expected to be another `txn_version.txt`-mutable touchpoint surfaced by mutations/DELETE-in-txn (removal-TID lock/unlock on existing parts) or rollback-of-removal. Read the server err log, identify the touchpoint, fix within the Task-2 mechanism family (the mutable-only branch should already cover removal-TID rewrites; new issues are likely in the source-resolution / removal application). Re-run after each fix; re-run gtests after any CA code change.
- [ ] **Step 4: re-gate honestly.** Any Tier-2 test that fails for a genuinely orthogonal reason (a topology the stateless server lacks, a replicated-only requirement, `enable_experimental_transactions`/config the stateless server doesn't set, etc.) — re-gate it with a PRECISE reason rather than a CA-transaction bug, consistent with the FETCH `03350` / FREEZE handling. Do not leave a test failing un-gated.
- [ ] **Step 5: commit** the Tier-2 results (un-gates + fixes + any documented re-gates). `CAS txn: Tier-2 mutations/isolation on CA (un-gate + reproduction-driven fixes)`.

---

## Phase 4 — regression, finalize, push

### Task 7: non-CA regression + backlog + push

- [ ] **Step 1: non-CA regression** — run a couple of transaction tests on the PLAIN job to confirm the gate-decoupling + CA branches didn't regress plain/S3 transactions:
```bash
timeout 900 python3 -m ci.praktika run "Stateless tests (arm_binary, parallel)" --test "01173_transaction_control_queries 01172_transaction_counters" > build/txn_t7_plain.log 2>&1
echo "exit=$?"; grep -hoE "Failed: [0-9]+, Passed: [0-9]+" build/txn_t7_plain.log | tail -1
```
Expect pass (every change is gated by `supportsTransactionalMutableFiles`/`isContentAddressed`/the mutable-only-branch predicate).
- [ ] **Step 2: CA non-transaction smoke** — re-run a handful of existing CA oracles (insert/select, projections, mutations, detach, fetch, freeze) on the CA-default job to confirm the `isMutablePerPartFile` `.tmp` change + the commit-branch addition didn't regress non-transaction CA paths. (The `.tmp` recognition and the `recorded.empty()` branch could affect ordinary writes — verify.)
- [ ] **Step 3: finalize the un-gate** — confirm every un-gated test passes on CA or is re-gated with a precise reason. Tally how many of the ~18 are now green.
- [ ] **Step 4: backlog** — `docs/superpowers/deferred_backlog/cas-mergetree-integration.md`: B39 → DONE/partial. Note: transactions (MVCC) work on CA via the per-ref-sidecar `txn_version.txt` mechanism (gate decoupled from append; `replaceFile`; mutable `.tmp` staging; mutable-only sidecar commit branch). List the tests un-gated and any re-gated with reasons. Note any deferred edge (e.g. replicated-pool transaction interactions, if surfaced).
- [ ] **Step 5: commit + push** `git push filimonov cas-mergetree-poc`.

---

## Done criteria
- The ~18 gated transaction/isolation tests pass on the CA-default job, or are individually re-gated for a documented orthogonal reason (not a CA-transaction bug).
- A CA transaction writes `txn_version.txt` to the per-ref sidecar in place — both inside a part-build transaction and as standalone autocommit ops on a committed part — never republishing the part (gtest-pinned: ref/manifest/`part_id` intact; sidecar updated).
- No non-CA regression (gate-decoupling + CA branches are capability/`isContentAddressed`-gated) and no non-transaction CA regression (the `.tmp` + commit-branch changes).
- Backlog B39 updated; the inline-CA oracle proves begin/insert/commit/rollback on CA.
