# CAS part removal: design inputs (CAS-83, second pass)

All citations are at `e2dd0f5d343` (`altinity/antalya-26.6`), read in the `cas-30` worktree.
Path prefixes: `MT/` = `src/Storages/MergeTree/`, `CA/` = `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`, `DOS/` = `src/Disks/DiskObjectStorage/`.
Builds on `tmp/cas83/removal-flow-map.md`; facts established there are not repeated.

## A. The most rebase-portable shape

### A1. The four existing `isContentAddressed` branches in `MT/DataPartStorageOnDiskBase.cpp`

| Site | Function | Shape |
|---|---|---|
| `:424-430` | `backup` | Early `throw` guard, placed before the upstream body. Self-contained. |
| `:544-547` | `freeze` | Two lines that create `owned_transaction`, then upstream code reads `clone_transaction` in several places (`:549`, `:567-609`). Woven into the body. |
| `:688-716` | `freezeRemote` | `if (owned_transaction) { new code } else { upstream code }`. The upstream body is re-indented into `else`, so every upstream edit there conflicts. |
| `:790-808` | `clonePart` | Same `if CA { new } else { upstream }` wrap (`:790`, else at `:810`). |

- Fewest upstream lines touched: the `backup` shape (`:424`). It is an early exit that adds lines and changes none.
- The `if/else` wraps at `:688` and `:790` re-indent upstream code. Avoid that for `remove`.

### A2. How often upstream changes `remove` / `clearDirectory`

`git log -S'delete_tmp_'` and `-S'clearDirectory'` return only `8e3698c91f5` (2023-01-25, "refactoring of code near merge tree parts"). So the rename and the function split have not moved since 2023.

`git log -L` over `remove` + `clearDirectory` (`:908-1161`) shows 7 commits between 2025-09 and 2026-02:

| Commit | Date | Touched |
|---|---|---|
| `d6350c12225` | 2025-09-15 | `clearDirectory` unlink list: `TXN_VERSION_METADATA_FILE_NAME` renamed (`:1135`) |
| `6e4b80a2f1d` | 2026-01-14 | `remove`: stop updating `part_dir` after the rename (`:998-1000`) |
| `95cf7bc28ac`, `fbc2f283da1` | 2026-01-23 | `no_such_file_or_directory` catch around `removeSharedRecursive` in `remove` (`:963-972`) and `clearDirectory` (`:1104-1117`, `:1145-1159`) |
| `4255bea5d4d`, `0d2272c3837` | 2026-01-28 / 2026-02-02 | evaluate `can_remove_callback` before the rename (`:990-994`), remove duplicate evaluations |
| `bb567cd8d20` | 2026-02-09 | `LOG_ERROR` → `LOG_WARNING` in the "doesn't exist" messages (`:983`, `:1007`, `:1017`) |

- All churn sits inside `if (!has_delete_prefix) { … }` (`:933-1024`) and in `clearDirectory`.
- Lines `:908-930` (signature, `from`, the NOTE comments, `has_delete_prefix`, `can_remove_description`, `auto disk`) are unchanged since `8e3698c91f5` / `2a327107b63` (2024-01-23, `LoggerPtr`), per `git blame -L 908,930`.
- So the least-conflicting insertion point is right after `auto disk = volume->getDisk();` (`:930`), before `fs::path to = …` (`:931`). Both context lines are 2023 code.

### A3. What the branch must keep, and what it can skip

| Upstream step | Site | CAS branch | Why |
|---|---|---|---|
| `detached/` `LOGICAL_ERROR` guard | `:938-943` | **Keep**, by excluding `detached/` from the branch condition so that the upstream guard still fires | On CAS, `removeRecursive` of `detached/<p>` routes to `dropRefIfPresent(detached/<p>)` (`CA/ContentAddressedMetadataStorage.cpp:1461-1472`, `CA/ContentAddressedTransaction.cpp:1141-1145`). A caller bug would then silently drop a detached part. |
| `can_remove_callback()` evaluation | `:990-994`, `:1027-1028` | **Keep** (one call), and pass its result to `removeSharedRecursive` | On CAS it has no side effects: `unlockSharedData` returns `{true, {}}` early. Either `allow_remote_fs_zero_copy_replication` is off, or `supportZeroCopyReplication()` is false for CAS (`DOS/DiskObjectStorage.h:54-57`, `src/Storages/StorageReplicatedMergeTree.cpp:11281-11288`). `force_keep_shared_data` is set only for zero-copy disks (`MT/MergeTreeData.cpp:6441-6449`). CAS ignores the predicate (`CA/ContentAddressedTransaction.cpp:1060-1064`). Keeping the call preserves the upstream contract that the callback always runs once (the point of `4255bea5d4d`/`0d2272c3837`). It costs one line. |
| `existsDirectory(to)` + `removeSharedRecursive(to)` for a leftover `delete_tmp_<p>` | `:954-979` | **Skip** | The new code never creates `delete_tmp_*`. Pre-upgrade leftovers are removed at startup by `clearOldTemporaryDirectories(0, {"tmp_", "delete_tmp_", "tmp-fetch_"})` (`src/Storages/StorageMergeTree.cpp:299`, `MT/ReplicatedMergeTreeAttachThread.cpp:199`) and on DROP (`MT/MergeTreeData.cpp:4221`). The in-process-retry case needs a failed rename in the same process, which cannot happen after the upgrade. |
| `existsDirectory(from)` early return + `WARNING` | `:981-988` | **Skip** | `dropRefIfPresent` already resolves the ref and treats absent as success (`CA/Parts/PartFolderAccess.cpp:572-596`). The probe is one more resolve with no effect on the outcome. Only the diagnostic `WARNING` is lost. To keep it, add `if (!disk->existsDirectory(from)) { LOG_WARNING…; return; }` (2 lines + 1 resolve). |
| `moveDirectory(from, to)` | `:997` | **Skip** | This is the saving: manifest B, `precommit`, `build_publish`, `ref_drop <p>`. The upstream reason, a name clash between `all_1_2_1` and `tmp_merge_all_1_2_1` (`:915-917`), does not apply: CAS refs are keyed by the full directory name, and the drop is one atomic ledger op. |
| Loaded projections: `clearDirectory(to/<p>.proj)` | `:1029-1050` | **Skip** | Projections are entries in the parent manifest. Dropping the parent ref removes them. `removeRecursive` of a projection subdir is already a no-op (`CA/ContentAddressedTransaction.cpp:1146-1149`). |
| Unloaded projections: `readFile(checksums.txt)` under `to` + `clearDirectory` | `:1056-1081` | **Skip** | Same reason. It also removes a manifest read of the renamed ref. |
| `clearDirectory(to)`: unlink batch + `removeDirectory` | `:1083`, `:1120-1140` | **Skip** | This is the saving: manifest C, `precommit`, `build_publish`+`ref_repoint`, `ref_drop delete_tmp_<p>`. |
| `is_temp` / incomplete temporary part | `:1100-1118` | **Nothing needed** | See A4. |

### A4. What `removeSharedRecursive(from)` does on CAS for each kind of part

One `DiskObjectStorage` transaction (`DOS/DiskObjectStorage.cpp:645-651`) → `tx->removeRecursive` (`DOS/DiskObjectStorageTransaction.cpp:175-195`) → `ContentAddressedTransaction::removeRecursive` (`CA/ContentAddressedTransaction.cpp:1061-1171`):

- **Committed part** `<table>/<p>`: `parsePartFilePath` → `route` (live namespace, `ref = <p>`, `file = ""`, `CA/ContentAddressedMetadataStorage.cpp:1490-1493`) → `dropRefIfPresent` (`:1141-1145`). One `ref_resolve` + one `ref_drop`, 0 manifest PUTs.
- **`moving/<p>`** (failed move clone, `isMovingPart`, `MT/IMergeTreeDataPart.cpp:1011-1022`): routes to the `moving/<p>` ref (`CA/ContentAddressedMetadataStorage.cpp:1474-1489`) → dropped. Today the same removal renames `moving/<p>` → `moving/delete_tmp_<p>`, a republish.
- **Temporary part, never committed** (`tmp_insert_*`, `tmp_merge_*`, `tmp_mut_*`): stays in the writer transaction's staging until `renameParts` commits it (`MT/MergeTreeData.cpp:9411-9413`; staged re-key in `CA/ContentAddressedTransaction.cpp:1357-1430`). No ref exists, so `dropRefIfPresent` → `resolveRef` miss → no-op (`CA/Parts/PartFolderAccess.cpp:578-582`). `remove()` never uses the part's held transaction, today or with the branch. Staging is discarded with that transaction.
- **Temporary part with an early-committed scratch ref** (text-index sub-storage at the part's own path, `CA/ContentAddressedTransaction.cpp:1414-1422`): the scratch ref is dropped. Today the path is rename (a republish of the scratch ref) → `clearDirectory` fallback → drop.
- **Temporary projection sub-part** `<parent>/<p>_N.tmp_proj`: routes to the parent ref with a non-empty `file` → "projection subdir: no-op" (`:1146-1149`). Today `existsDirectory(from)` on committed metadata answers false for a parent that is still staged, so `remove` logs the "doesn't exist" `WARNING` and returns (`:981-988`). The outcome is the same and the `WARNING` disappears. See "Surprising" #3.
- **`resolveRef` staleness:** `allow_stale` is ignored. The mounted writer's ref table is authoritative (`CA/Pool/CasRefLedger.cpp:285-292`), so a cached "absent" cannot hide a live ref and leak it.
- **Retries and crashes:** a thrown drop (668 on a transient disk, `checkOpAdmitted(Remove)`, `:1068-1069`) leaves `<p>` in place. `rollbackDeletingParts` returns the part to `Outdated` (`MT/MergeTreeData.cpp:3999`) and the next round drops it. A crash gives "old part or no part", which upstream already accepts on either side of its rename.

### A5. Proposed hunk (not applied)

```diff
--- a/src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp
+++ b/src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp
@@ -928,6 +928,17 @@ void DataPartStorageOnDiskBase::remove(
     bool has_delete_prefix = part_dir_without_slash.filename().string().starts_with("delete_tmp_");
     std::optional<CanRemoveDescription> can_remove_description;
     auto disk = volume->getDisk();
+
+    /// A content-addressed part is one ref, and dropping it is atomic. The `delete_tmp_` rename and
+    /// the per-file unlinks below would only republish the part twice before the same drop.
+    if (disk->isContentAddressed() && getParentDirectory() != MergeTreeData::DETACHED_DIR_NAME)
+    {
+        auto description = can_remove_callback();
+        disk->removeSharedRecursive(
+            fs::path(from) / "", !description.can_remove_anything, description.files_not_to_remove);
+        return;
+    }
+
     fs::path to = fs::path(root_path) / part_dir_without_slash;
 
     if (!has_delete_prefix)
```

- +11 lines, 0 upstream lines changed. Context lines date from 2023.
- No new includes or error codes. `MergeTreeData::DETACHED_DIR_NAME` is already used at `:938`.
- A `detached/` path falls through to the upstream guard, which throws `LOGICAL_ERROR` as it does today.
- Saves per removed part: 2 manifest PUTs and 5 of 6 ref-log appends. One `ref_resolve` + one `ref_drop` remain. Each projection saves 1 more PUT and 2 more appends.
- Expected `system.cas_log` per removed part after the change: `ref_resolve <p>`, `ref_drop <p>`. Nothing on `delete_tmp_<p>`.

### A6. Would `IDataPartStorage`/`IDisk` be a better home?

No. The existing `IDisk::isContentAddressed` (`src/Disks/IDisk.h:477`) and `IDataPartStorage::isContentAddressed` (`MT/IDataPartStorage.h:196`, implemented at `MT/DataPartStorageOnDiskBase.cpp:278-281`) already name the capability.
- A new `removesPartAtomically()` would add a virtual in `IDisk.h`, an override in `DOS/DiskObjectStorage.h`, and the same branch in `remove`. That is 3 upstream files instead of 1, for the same behavior.
- Overriding `remove` in a CAS-specific `IDataPartStorage` subclass is not possible: CAS parts use the upstream `DataPartStorageOnDiskFull`.

### A7. Prose that becomes false (fix in the same change)
- `MT/DataPartsExchange.cpp:254-257`: "the in-memory part path is deliberately not updated by a `delete_tmp_*` rename". The gate logic stays right (gate 1 is authoritative), but on CAS that rename no longer happens.
- `CA/ContentAddressedTransaction.cpp:1341`, `:1433` (lists "delete_tmp_ rename" among committed renames), `:1044-1046` and `:1596-1601` (already false today: they describe a single-transaction removal).
- `CA/ContentAddressedMetadataStorage.cpp:1484` ("merge-result/delete_tmp renames").
- `src/Disks/tests/gtest_ca_wiring.cpp:1088` ("MergeTree renames a part to delete_tmp_<part> before removing it").
- Correction to `removal-flow-map.md` §F: `CommittedRefMoveDoesNotSpuriouslyPublish` (`gtest_ca_transaction.cpp:165`), `CommittedPartRenameMovesTheRef` (`gtest_ca_wiring.cpp:1081`) and `RepublishRefNoTreeHeadBeforePrecommit` (`gtest_ca_wiring.cpp:2637`) do **not** break under this cut. They call `ContentAddressedTransaction::moveDirectory` directly, and a committed-ref rename still republishes. They only stop mirroring production. They break only under cut (1), the CAS-side string sniff.
- `tests/integration/test_cleanup_after_start/test.py` creates `delete_tmp_*` by hand to test startup cleanup. It is unaffected.

## B. Other "rename a committed directory, then remove it" (and "commit, then rename") sites

Cost notation per part: **P** = manifest PUT, **A** = ref-log append. A committed-ref rename is `republishRef` = 1 P + 3 A (`precommit`, `build_publish`, `ref_drop` src) (`CA/Parts/PartFolderAccess.cpp:477-506`). A drop is 1 A. A staged re-key is free.

### B1. Ranked by expected frequency in normal operation

| # | Site | Sequence and transactions | CAS cost today | Frequency | Recommendation |
|---|---|---|---|---|---|
| 1 | **Outdated-part removal** (merge sources, mutation sources, `DROP PARTITION`-covered parts, TTL-dropped parts). Drivers in the map §A. | `remove()`: rename, unlink batch, rmdir = 3 tx | 2 P + 6 A | Every part that becomes `Outdated`. The dominant flow. | **Yes**: the Part A hunk. |
| 2 | **Replicated fetch**: `downloadPartToDisk` / relink commits `tmp-fetch_<p>` (`MT/DataPartsExchange.cpp:1307`, relink notes `:1404`, `:1515-1523`). `renameTempPartAndReplace` → `renameParts` → `renameTo` (`MT/MergeTreeData.cpp:9407`) → `moveDirectory(tmp-fetch_<p> → <p>)`. | Commit, then rename = 2 tx | 1 P + 2 A (publish) + 1 P + 3 A (rename). 1 P + 3 A of that is avoidable. | Every part a replica fetches: inserts on other replicas, merges assigned as fetch. High on replicated clusters. | **Different cut; worth doing, separately.** Keep the fetch's part-storage transaction open, as inserts and merges do (`precommitTransaction`, `MT/MergeTreeDataWriter.cpp:435`, `MT/MergeTask.cpp:2195`), and let `renameParts` commit at the final name (`MT/MergeTreeData.cpp:9411-9413`). Not a removal and not covered by Part A. Verify first with `cas_log` on a fetching replica: expect `manifest_put` on `tmp-fetch_<p>` and then on `<p>`. |
| 3 | **Mutation of an unaffected part**: `MutateTask` clones with prefix `tmp_clone_` (`MT/MutateTask.cpp:3411-3422`) → `cloneAndLoadDataPart` → `freeze`, whose owned transaction **commits** `tmp_clone_<p>` (`MT/DataPartStorageOnDiskBase.cpp:544-547`, `:607-608`). Then rename to `<p>_<mutation>`. | Commit, then rename = 2 tx (+1 autocommit write, see C) | 1 P + 2 A + 1 P + 3 A | Every part a mutation skips as "doesn't change up to mutation version" (`MT/MutateTask.cpp:3409`). Per mutation × parts not touched. Medium. | **Different cut; worth doing.** Same idea as #2: return the clone's transaction to the caller instead of committing it in `freeze`, so `renameParts` publishes once at the final name. |
| 4 | **REPLACE/MOVE/ATTACH … FROM PARTITION** (`tmp_replace_from_`, `tmp_move_from_`: `src/Storages/StorageMergeTree.cpp:3091`, `:3277`; Replicated `:3495`, `:10046`; `executeReplaceRange` `tmp_clone_` `src/Storages/StorageReplicatedMergeTree.cpp:5833`) | Same as #3 | Same as #3, plus the Replicated `metadata_version.txt` double write (C, row 5) | Per cloned part. Admin/ETL; frequent for REPLACE-PARTITION pipelines. | Same cut as #3. They share `cloneAndLoadDataPart` → `freeze`, so one change covers both. |
| 5 | **Parts mover to CAS**: `clonePart` commits `moving/<p>` in one transaction (`MT/DataPartStorageOnDiskBase.cpp:790-803`). `renameClonedPart` → `renameTo(<p>)` (`MT/MergeTreePartsMover.cpp:307-315`) = republish `moving/<p>` → `<p>`. | Commit, then rename = 2 tx | 1 P + 2 A + 1 P + 3 A | Per part moved by TTL/policy onto CAS. Medium where tiering is used. | **No.** `moving/` is kept deliberately as a crash-atomicity staging ref (`CA/ContentAddressedMetadataStorage.cpp:1476-1485`). A direct publish at `<p>` before the swap would make a pre-swap clone live. |
| 6 | **Parts mover from CAS**: `swapActivePart` sets the old part `DeleteOnDestroy` (`MT/MergeTreeData.cpp:6452`) → `~DataPart` → `removeIfNeeded` → `remove()` (`MT/IMergeTreeDataPart.cpp:1028`) | = #1 | 2 P + 6 A | Per part moved off CAS | **Yes, covered by Part A.** No extra work. |
| 7 | **Empty covering part** for `DROP PARTITION`/`TRUNCATE` on Replicated: committed on CAS on purpose (`MT/MergeTreeData.cpp:5965-5980`), rolled back to `Outdated`, removed later by `remove()` | Commit, then #1 | 1 P + 2 A + (2 P + 6 A) | Per `DROP PARTITION` range | **Yes, covered by Part A** (the removal half). The commit is load-bearing; keep it. |
| 8 | **DROP TABLE**: `dropAllData` → per-part `remove()` (`MT/MergeTreeData.cpp:4199`), then `removeRecursive(relative_data_path)` → `dropNamespace` (`:4303`; `CA/ContentAddressedTransaction.cpp:1116-1119`) | N×3 tx + 1 | N × (2 P + 6 A) + namespace drop | Per DROP. Rare, but N can be large. | **Yes, covered by Part A** (N × 1 A). A further cut (skip per-part removal on CAS, since `dropNamespace` covers it) is CAS-321.6's scope, not this change. |
| 9 | **DROP DETACHED PART/PARTITION**: `PartsTemporaryRename::tryRenameAll` renames `detached/<p>` → `detached/deleting_<p>` (`MT/MergeTreeData.cpp:5373-5392`, `:8644`, `:8656`, `:8662`), then `removeDetachedPart` → `removeRecursive` (`:8665`, `:10465-10470`; Replicated `src/Storages/StorageReplicatedMergeTree.cpp:12249-12261`) | 2 tx | 1 P + 3 A + 1 A | Admin command | **No.** Rare. The `deleting_` rename is also a claim: `validateDetachedPartName` refuses `deleting_`/`attaching_` names (`:8630-8632`), which excludes a concurrent ATTACH. A CAS skip needs a fork patch in a second upstream function for little gain. |
| 10 | **ATTACH PART/PARTITION**: `detached/<p>` → `detached/attaching_<p>` (`:8822`, `:8897`), load, `loadPartAndFixMetadataImpl` (C, row 6), then commit renames to a live name. Failure: `rollBackAll` moves back (`:5395-5427`). | 2 renames + optional unlinks | 2 × (1 P + 3 A) | Admin | **No.** Not a removal. Rare. |
| 11 | **DETACH / broken-on-load** `<p>` → `detached/<prefix>_<p>` (map §D) | 1 rename | 1 P + 3 A | Admin; broken parts | **No.** One rename; nothing to cut. |
| 12 | **OPTIMIZE DRY RUN**: commits the temp part (`MT/MergeTreeData.cpp:8777-8778`), then `new_part->remove()` (`:8799`) | Commit + #1 | 1 P + 2 A + (2 P + 6 A) | Testing aid | **Yes, covered by Part A.** |
| 13 | **Fetch-replace a part on a given disk**: fetch to `tmp-fetch_`, then `getDataPartStorage().rename(replaced_path)` (`src/Storages/StorageReplicatedMergeTree.cpp:6061-6063`) | = #2 | = #2 | Rare repair | Covered by #2 if done. |
| 14 | **`clearOldTemporaryDirectories`** (periodic `tmp_`, `tmp-fetch_`; startup/DROP also `delete_tmp_`; `moving/`): `iterateDirectory` + `isOldPartDirectory` + `removeSharedRecursive(it->path())` (`MT/MergeTreeData.cpp:3379-3470`, `:3373`) | 1 tx per dir, no rename | 1 A if a committed ref exists, else a resolve miss | Periodic; usually finds nothing | **No.** Already one drop. |
| 15 | **FREEZE / UNFREEZE**: `freeze` copies into `shadow/` in one owned transaction; `removeRecursive(shadow/…)` → `dropRef` / `dropNamespace` (`CA/ContentAddressedTransaction.cpp:1072-1109`) | No rename | 1 P per frozen part; drops on unfreeze | Admin | **No.** |
| 16 | **Database drop**: `DatabaseAtomic` renames only metadata files on `db_disk` (`src/Databases/DatabaseAtomic.cpp:370`, `:460`, `:793`). Table data is not renamed. `DatabaseOnDisk` removes it directly (`src/Databases/DatabaseOnDisk.cpp:400`, `:606`). | — | — | — | **No** CAS data-path rename exists. |
| 17 | **Inserts, merges, mutated parts, projection `.tmp_proj` → `.proj`**: staged, re-keyed in the open transaction (`CA/ContentAddressedTransaction.cpp:1322-1338`, `:1357-1430`) | 1 tx | 1 P + 2 A (the publish itself) | Every new part | **No.** Already minimal. |

`clearOldBrokenPartsFromDetachedDirectory` does not exist at this commit. Its settings are obsolete (`MT/MergeTreeSettings.cpp:2310-2311`).

### B2. Where the "commit, then rename" rows come from
- Rows 2-5 share one shape: a whole-part CAS transaction is committed under a temporary name, then `renameParts` → `renameTo` → `moveDirectory` republishes it.
- Inserts and merges avoid this: they `precommitTransaction` and let `MergeTreeData::Transaction::renameParts` commit after the rename (`MT/MergeTreeData.cpp:9407-9413`).
- A per-site fix in `DataPartsExchange` (fetch) and in `freeze`'s owned transaction (clones) would halve those PUTs. These are separate tasks, not part of the removal hunk.

## C. Writes into an already committed part

On CAS, a write or unlink into a committed part is a repoint: 1 P + 2 A (`precommit`, `build_publish`+`ref_repoint`). A byte-equal result is a no-op (`CA/Parts/PartFolderAccess.cpp:508-566`). An if-exists unlink of a missing file is a no-op (`CA/ContentAddressedTransaction.cpp:1632-1634`).

| # | File | Writer / call site | When | CAS effect | Class |
|---|---|---|---|---|---|
| 1 | `delete-on-destroy.txt` | **Never written.** Deprecated by `c1c69553741` ("Deprecate delete-on-destroy.txt, do not create it any more"). `writeDeleteOnDestroyMarker` does not exist. The mover marks `DeleteOnDestroy` in memory only (`MT/MergeTreePartsMover.cpp:318-321`). Only removed: if-exists entry of the removal batch (`MT/DataPartStorageOnDiskBase.cpp:1134`); in the clone transaction (`:570`, `:598`); at ATTACH `removeDeleteOnDestroyMarker` (`MT/IMergeTreeDataPart.cpp:1773-1776`, `MT/MergeTreeData.cpp:6616`) | Removal, clone, ATTACH | Missing file ⇒ no mark, no repoint. **It is not the source of the removal repoint.** | 1 |
| 2 | Removal unlink batch (`checksums.txt`, `columns.txt`, every data file, plus the if-exists names) | `clearDirectory` fast path `removeSharedFiles` (`MT/DataPartStorageOnDiskBase.cpp:1120-1139`) on `delete_tmp_<p>` | Every removal, right after the rename | Marks → repoint of `delete_tmp_<p>` to manifest C (the stand trace's "standalone write/remove on a committed part"). One more per projection. | 2 (Part A removes it) |
| 3 | `txn_version.txt`, non-transactional | `VersionMetadataOnDisk::storeInfoUnlocked` (`src/Interpreters/MergeTreeTransaction/VersionMetadataOnDisk.cpp:186-245`). Non-transactional updates are **deferred in memory** when the file does not exist (`:205-210`; `is_persist_deferrable` = file absent, `:43`). ReplicatedMergeTree has `can_write_metadata = false` (`:40`; `supportsTransactions` defaults to false, `src/Storages/IStorage.h:191`) and returns `UNSTORED_VERSION` (`:191-202`). Covers `setAndStoreNonTransactionalRemovalTID` at load (`MT/MergeTreeData.cpp:2097`), `setAndStoreCreationTID(NonTransactionalTID)` (`MT/DataPartsExchange.cpp:1312`, `src/Storages/StorageReplicatedMergeTree.cpp:2713`, …), `loadAndUpdateMetadata` (`MT/MergeTreePartsMover.cpp:300`, `MT/MergeTreeData.cpp:2237`; writes only when `updateCSNIfNeeded` changes something, `VersionMetadata.cpp:561-585`) | Load, fetch, move, commit, removal | Nothing is written. That matches the stand, which has no transactions. | 1 |
| 4 | `txn_version.txt`, transactional | `setAndStoreCreationCSN`/`RemovalTID`/`RemovalCSN` (`src/Interpreters/MergeTreeTransaction.cpp:116`, `:141`, `:168`, `:292`, `:297`, `:351`, `:370`; rollback `MT/MergeTreeData.cpp:9311`). Once the file exists, even a later non-transactional removal of a transaction-created part writes (`MergeTreeTransaction.cpp:116`, `:141`; `is_persist_deferrable = false`). On CAS a single atomic write (`supportsAtomicFileWrites` = true, `CA/ContentAddressedMetadataStorage.h:270`; `VersionMetadataOnDisk.cpp:330-338`), so no `.tmp` + replace. | Commit of a transaction; removal of a transaction-created part (at the `Outdated` transition, not inside `remove`) | 1 repoint per write | 4 (MVCC visibility after restart depends on it) |
| 5 | `metadata_version.txt` | `writeMetadataVersion` has **no callers** (definition only, `MT/IMergeTreeDataPart.cpp:1756`). ALTERs go through mutations, which write new parts. Written into **clones**: inside the clone transaction (`MT/DataPartStorageOnDiskBase.cpp:585-593`), then **again** by `cloneAndLoadDataPart` as an autocommit write into the already committed `tmp_*` clone (`MT/MergeTreeData.cpp:10121-10128`). The comment at `MT/DataPartStorageOnDiskBase.cpp:580-582` says so. | Replicated `executeReplaceRange` / REPLACE / MOVE PARTITION (`src/Storages/StorageReplicatedMergeTree.cpp:3491`, `:9794`, `:10085`) | Values match (both `getMetadataVersion()` of the destination snapshot) ⇒ byte-equal repoint no-op: ForceFresh resolve + manifest compare, no PUT. | 3 (redundant on CAS; cheap; could be skipped when `isContentAddressed`) |
| 6 | `txn_version.txt` / `delete-on-destroy.txt` removal at ATTACH | `loadPartAndFixMetadataImpl` → `removeDeleteOnDestroyMarker`, `removeVersionMetadata` (`MT/MergeTreeData.cpp:6604-6618`; callers `:8926`, `src/Storages/StorageReplicatedMergeTree.cpp:2616`) on `detached/attaching_<p>` | ATTACH | No-op if absent. 1 repoint if the detached part came from a transaction table. | 1 (absent) / 4 (present) |
| 7 | `checksums.txt` | `loadChecksums` recompute when missing (`MT/IMergeTreeDataPart.cpp:1900-1906`); `CHECK TABLE` (`src/Storages/StorageMergeTree.cpp:3431-3437`) | Legacy or damaged parts only. Every CAS-written part has the file. | Repoint if it fires | 4 (repair) |
| 8 | `columns.txt` | Write-on-load when missing (`MT/IMergeTreeDataPart.cpp:2195-2206`) | Legacy Wide parts only | Repoint if it fires | 4 (repair) |
| 9 | `columns_substreams.txt`, `default_compression_codec.txt`, `uuid.txt`, `ttl.txt`, `serialization.json` | Only into **new** parts (`MT/MutateTask.cpp:1424-1505`, `MT/MergedBlockOutputStream.cpp:320-332`) | Part creation | Staged in the part's own transaction | 1 |
| 10 | mtime | `setLastModified` before every `rename` (`MT/DataPartStorageOnDiskBase.cpp:883`) | Every rename | Accepted and ignored (`CA/ContentAddressedTransaction.cpp:1233-1239`) | 1 |
| 11 | `remove_time` | In memory only (`remove_time.store`, e.g. `MT/MergeTreeData.cpp:5983`). Not persisted. | — | — | 1 |
| 12 | Broken markers | None. Broken parts are renamed to `detached/broken*` (B1 row 11), not marked with a file. | — | — | — |
| 13 | Replicated cleanup thread | Writes Keeper only. The filesystem side is the `remove()` of B1 row 1 (`src/Storages/StorageReplicatedMergeTree.cpp:9291-9317`). | — | — | — |

What is written right before `remove()` in the removal flow, without transactions: **nothing**. `grabOldParts` changes in-memory state (`MT/MergeTreeData.cpp:3608`). `IMergeTreeDataPart::remove` writes nothing (`MT/IMergeTreeDataPart.cpp:2422-2462`). The stand trace is fully explained by the rename (B → `ref_drop <p>`) and the unlink batch (C, repoint) plus the rmdir drop. With transactions there is one earlier `txn_version.txt` repoint at the `Outdated` transition (row 4).

## Surprising

1. **The remembered "marker written before removal" no longer exists.** `delete-on-destroy.txt` has not been written since `c1c69553741`. The repoint in the trace is the removal's own unlink batch.
2. **Fetch and "clone" paths publish every part twice** (commit under a `tmp*` name, then republish on rename): B1 rows 2-4. That is the same waste class as removal, on the write side, and on replicated clusters it is likely as frequent. Part A does not touch it.
3. **Temporary projection sub-parts `<parent>/<p>_N.tmp_proj` are never removed on CAS.** `removeIfNeeded` sees them via the held transaction (`MT/DataPartStorageOnDiskFull.cpp:63-72`), but `remove()` probes committed metadata. It logs "doesn't exist" and returns (`MT/DataPartStorageOnDiskBase.cpp:981-988`). If `remove()` got further, `moveDirectory(<parent>/<p>_N.tmp_proj → <parent>/delete_tmp_<p>_N.tmp_proj)` would hit the `LOGICAL_ERROR` "unsupported shape" (`CA/ContentAddressedTransaction.cpp:1438-1440`). **Not verified:** whether the intermediate levels' staged entries then leak into the published parent manifest (`MT/MergeProjectionPartsTask.cpp:130-138`). Part A does not change this outcome; it only removes the `WARNING`.
4. **`writeMetadataVersion` is dead code** (no callers), and `clearOldBrokenPartsFromDetachedDirectory` is gone upstream. Both were in the brief's list.
5. **The Part A cut breaks no existing gtest.** The three gtests the map lists call `moveDirectory` on the transaction directly, which keeps its semantics.
