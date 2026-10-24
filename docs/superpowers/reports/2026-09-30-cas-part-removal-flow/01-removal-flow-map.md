# CAS part-removal flow map (CAS-83, CAS-321.6)

All citations are at `e2dd0f5d343` (`altinity/antalya-26.6` head).
Path prefixes: `MT/` = `src/Storages/MergeTree/`, `CA/` = `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`, `DOS/` = `src/Disks/DiskObjectStorage/`.

## Headline

- Upstream removes one part in **3 separate disk transactions**: rename, file-unlink batch, rmdir. Each `DiskObjectStorage::*` call builds and commits its own transaction.
- The CAS supersede-clear (T8) elides the marks repoint only if the unlinks and the rmdir share **one** `ContentAddressedTransaction`. Upstream never does that, so T8 never fires in production. The gtest that pins T8 claims to mirror `IMergeTreeDataPart::remove` but models a shape upstream never produces.
- Per removed part (no projections), the code issues: 2 manifest PUTs, 6 `appendRefOps` calls (2 precommit, 2 promote, 2 drop). One `dropRef` would do. Each projection adds 1 more manifest PUT and 2 more appends.

## A. Upstream sequence

### Drivers
- Plain `MergeTree`: `StorageMergeTree::clearOldPartsFromFilesystem` (`src/Storages/StorageMergeTree.cpp:2270`) calls `grabOldParts`, then `clearPartsFromFilesystemAndRollbackIfError` (`:2289`).
- `ReplicatedMergeTree`: `clearOldPartsAndRemoveFromZKImpl` (`src/Storages/StorageReplicatedMergeTree.cpp:9243`). Keeper znodes are removed **first** (`:9291`). Only parts reliably removed from Keeper go on to the filesystem (`:9317`).
- `grabOldParts` takes only parts with unique ownership (`MT/MergeTreeData.cpp:3565`) and sets them to `Deleting` (`:3608`).
- `clearPartsFromFilesystemAndRollbackIfError` (`MT/MergeTreeData.cpp:3981`): on failure, the part goes back to `Outdated` via `rollbackDeletingParts` (`:3999`, `:3620-3630`). On success, `removePartsFinally` (`:4010`, `:3632`).
- `DROP TABLE`: `dropAllData` (`MT/MergeTreeData.cpp:4151`) runs `clearPartsFromFilesystemImpl` over all parts (`:4199`). It then clears temp dirs with the `delete_tmp_` prefix (`:4221`), and finally calls `disk->removeRecursive(relative_data_path)` (`:4303`). On CAS that last call is `dropNamespace` (`CA/ContentAddressedTransaction.cpp:1116-1119`).
- Other callers of `IMergeTreeDataPart::remove`:
  - `loadDataParts` (`:2611`)
  - `loadOutdatedDataParts` (`:3076`)
  - `tryRemovePartImmediately` (`:6105`)
  - attach failure (`:8799`)
  - `removeIfNeeded` for temp / `DeleteOnDestroy` parts (`MT/IMergeTreeDataPart.cpp:1028`)

### Serial vs parallel removal
- `clearPartsFromFilesystemImplMaybeInParallel` (`MT/MergeTreeData.cpp:3734`): if the part count is at most `concurrent_part_removal_threshold[_for_remote_disk]` (`:3759-3767`), it removes serially (`:3745-3750`). Otherwise it uses a thread pool with one task per part (`:3791-3807`), or per independent block range for zero-copy (`:3880-3973`).
- Every variant calls `part->remove()` once per part. **No batching reaches the disk layer.** Parallelism only lets the CAS ref lane group-commit appends from different parts (`CasRefLedger::flushRefBatch`, `CA/Pool/CasRefLedger.cpp:2753`). The steps of one part stay sequential.
- `remove_rolled_back_parts_immediately` only affects which parts `grabOldParts` selects (`:3584-3585`). It does not change how a part is removed.

### `IMergeTreeDataPart::remove` (`MT/IMergeTreeDataPart.cpp:2422-2462`)
- Builds `can_remove_callback` and the `projection_checksums` list from loaded projection parts (`:2455-2458`).
- Calls `getDataPartStorage().remove(...)` with `is_temp || state == Temporary` (`:2460-2461`).
- Does **not** open a part-storage transaction.

### `DataPartStorageOnDiskBase::remove` (`MT/DataPartStorageOnDiskBase.cpp:908-1084`)
Every call goes to `disk->…`, never to a held transaction.

1. Computes `to = delete_tmp_<basename>` under the same parent (`:945` / `:949`). A `detached/` parent is a `LOGICAL_ERROR` (`:938-943`).
2. `disk->existsDirectory(to)`. If the directory is left over (unclean restart or retry), `disk->removeSharedRecursive(to)` (`:954-979`).
3. `disk->existsDirectory(from)`. If absent, warns and returns success (`:981-988`).
4. **Tx 1:** `disk->moveDirectory(from, to)` (`:997`). The in-memory `part_dir` is deliberately **not** updated (`:998-1000`).
5. For each loaded projection: `clearDirectory(to/<p>.proj, …)` (`:1029-1050`). This is **one Tx 2 + one Tx 3 per projection**.
6. For `*.proj` entries in `checksums` that are not loaded: `existsFile` + `readFile(checksums.txt)` **under `to`**, then `clearDirectory` (`:1056-1081`). This reads the renamed directory.
7. `clearDirectory(to, …)` for the part itself (`:1083`).

### `clearDirectory` (`MT/DataPartStorageOnDiskBase.cpp:1086-1161`)
- **Fallback path**, `removeSharedRecursive(dir)` (one transaction), used when:
  - `checksums` is empty, or
  - the part is temporary and incomplete: `is_temp` plus a missing `checksums.txt` or `columns.txt`, both probed through `existsFile` (`:1100-1118`).
- **Fast path** (`:1120-1140`):
  - Builds a `RemoveBatchRequest`: `checksums.txt`, `columns.txt`, every non-`.proj` `checksums` file, plus if-exists entries for `default_compression_codec.txt`, `delete-on-destroy.txt`, `txn_version.txt`, `metadata_version.txt` and `columns_substreams.txt` (`:1122-1137`).
  - **Tx 2:** `disk->removeSharedFiles(request, …)` (`:1139`).
  - **Tx 3:** `disk->removeDirectory(dir)` (`:1140`).
- Any exception on the fast path logs `Cannot quickly remove directory … fallback to recursive removal` and calls `removeSharedRecursive(dir)` (`:1142-1160`).
- `getActualFileNamesOnDisk` is the identity function on `Full` storage (`MT/DataPartStorageOnDiskFull.h:73`).

### Disk layer: one transaction per call
| Call | Site |
|---|---|
| `DiskObjectStorage::moveDirectory` | `DOS/DiskObjectStorage.cpp:269-274` |
| `removeSharedFiles` | `DOS/DiskObjectStorage.cpp:364-369` |
| `removeDirectory` (first `isDirectoryEmpty`, then its own transaction) | `DOS/DiskObjectStorage.cpp:440-448` |
| `removeSharedRecursive` | `DOS/DiskObjectStorage.cpp:645-651` |

Mapping to metadata-transaction calls (`DOS/DiskObjectStorageTransaction.cpp`):

| Disk call | Metadata call | Site |
|---|---|---|
| `moveDirectory` | `tx->moveDirectory` | `:127-133` |
| `removeSharedFiles` | one `tx->unlinkFile(path, if_exists, …)` per file | `:229-240` |
| `removeDirectory` | `tx->removeDirectory` | `:205-211` |
| `removeSharedRecursive` | `tx->removeRecursive` | `:175-195` |

- CAS is an eager staging overlay: operations apply at call time. `commit` requires an empty queue (`:589-596`), then calls `metadata_transaction->commit` (`:624`).
- The only upstream place that puts unlinks and rmdir in one transaction is `removeSharedRecursiveWithLimit` (`DOS/DiskObjectStorage.cpp:653-680`). It is used only by `removeRecursiveWithLimit` (`DOS/DiskObjectStorage.h:96`), not by part removal.

## B. What the CAS layer does with each operation

### Tx 1: rename to `delete_tmp_<part>` produces manifest B, then the drop of `<part>`
- `ContentAddressedTransaction::moveDirectory` (`CA/ContentAddressedTransaction.cpp:1253`). A part-dir to part-dir move with no staged source reaches `republishRef(src, dst)` (`:1432-1436`).
- `CachedPartFolderAccess::republishRef` (`CA/Parts/PartFolderAccess.cpp:477-506`):
  - `resolveRef(src)` + `readManifestShared(A)`, then `resolveRef(dst)`.
  - `publishEntries(dst, A.entries)` (`:504`), which runs `prepareEntries` (`:442-465`):
    - `stageManifest` is the **manifest B PUT** (`CA/Pool/CasPartWriteTxn.cpp:590-700`, event `ManifestPut` at `:689`).
    - `precommitAdd` is **append #1**, `precommit` (`CasPartWriteTxn.cpp:702-805`, `appendRefOps` at `:747`).
  - `promote` is **append #2**, `build_publish` (`CasPartWriteTxn.cpp:815`, `appendRefOps` at `:860`, `BuildPublish` at `:1025`).
  - `dropRef(src)` is **append #3**, `ref_drop` of `<part>`/A (`PartFolderAccess.cpp:505`, `CasRefLedger::dropRef` at `CA/Pool/CasRefLedger.cpp:4843`, event at `:4871`).
- The transaction's `commit` then has an empty `parts` map, so it publishes nothing (`CA/ContentAddressedTransaction.cpp:471`, `:521-522`).

**Why a new manifest instead of renaming the ref**
- The manifest body names no ref and no directory. `PartManifest` = `{ManifestRef ref (epoch, build_seq, ordinal), root_namespace_id, payload_digest, entries}` (`CA/Formats/CasPartManifestFormat.h:82-89`). Nothing in A depends on the part name.
- The ledger forbids renaming. The only legal `owner_transition` shapes are AddPrecommit, RemovePrecommit, RemoveCommitted and Promote. Promote requires the **same ref_name and manifest_ref** on both sides; anything else is `CORRUPTED_DATA` (`CA/Pool/CasRefProtocol.cpp:117-156`).
- AddPrecommit refuses a manifest that is already owned under another name: "manifest already has a conflicting owner" (`CasRefProtocol.cpp:252-260`; `owned_manifests` at `:223-231`).
- A `MutationScope::ref` record may name only its own ref (`refNamedOutsideScope`, `CA/Pool/CasRefLedger.cpp:2737-2750`).
- The manifest id is minted per build to provide NoManifestIdReuse (`CasPartWriteTxn.cpp:615-620`). `prepareEntries` states the rule: "each part owns its manifest ID, so dst receives a distinct manifest" (`PartFolderAccess.cpp:453-455`).
- Conclusion: a rename that keeps A needs a new ref-log transition shape, plus GC fold/edge semantics (`manifestEdgesOfTxn` shares the classifier, `CasRefProtocol.cpp:112-116`). That is an on-S3 format change, and the format is frozen since 26.6.4.

### Tx 2 (per projection, then the part): unlink batch produces manifest C, a repoint of `delete_tmp_<part>`
- `unlinkFile` (`CA/ContentAddressedTransaction.cpp:1583-1644`), for a committed file not staged here:
  - `getView(delete_tmp_<part>)`: ForceFresh on the first file, then memoized (`:1623-1631`). This reads B.
  - Records `content_removed.insert(file)` (`:1641`).
  - A missing file with `if_exists=false` throws `FILE_DOESNT_EXIST` (`:1632-1639`). A missing file with `if_exists=true` is a no-op, so no mark for `delete-on-destroy.txt` and the other optional names.
- `commit` calls `publishStaging` (`:338-458`). The marks-only branch (`:363-418`):
  - `getView` ForceFresh.
  - `merged` = B's entries minus the marks (`:387-394`), usually empty.
  - `repointRef` (`:406`), in `PartFolderAccess.cpp:508-566`: ForceFresh resolve + `readManifestShared(B)`, a byte-equal check, then `publishEntries(…, allow_repoint=true)`:
    - **Manifest C PUT**
    - **Append #4** (`precommit`)
    - **Append #5**: `promote` with `repoint_old = B`, one record that removes the B binding and promotes C (`CasPartWriteTxn.cpp:966-1000`; events `BuildPublish` + `RefRepoint`, `:1046-1059`). The reason text matches the stand trace.
  - `CASRefRepoint` is incremented at `PartFolderAccess.cpp:545`.
- `delete-on-destroy.txt` is only one of the if-exists unlink entries. It is not a marker that causes the repoint. **The repoint comes from the marks of the whole fast-path unlink batch** (`MT/DataPartStorageOnDiskBase.cpp:1139`).
- Projections:
  - Each loaded projection's `clearDirectory` is its own Tx 2 with marks on the same ref `delete_tmp_<part>`, giving one more repoint and manifest per projection.
  - Its Tx 3 is a no-op: the path routes with a non-empty `file`, so the check at `CA/ContentAddressedTransaction.cpp:1039` fails.
  - `isDirectoryEmpty` still answers true for a projection dir (`CA/ContentAddressedMetadataStorage.cpp:1981-1985`).

### Tx 3: `removeDirectory(delete_tmp_<part>)` drops the ref
- `isDirectoryEmpty` answers true for a part dir without I/O (`CA/ContentAddressedMetadataStorage.cpp:1972-1987`).
- `removeDirectory` (`CA/ContentAddressedTransaction.cpp:1020-1059`) calls `dropRefIfPresent` (`:1041`). That is `resolveRef(allow_stale)` + `dropRef`, **append #6**, `ref_drop` of C (`PartFolderAccess.cpp:572-596`).

### The T8 supersede-clear
- Site: `CA/ContentAddressedTransaction.cpp:1042-1056`. After the drop, `findStaging(*r)` clears `content_removed`/`entries` and abandons the build. `publishStaging` then no-ops (`:344-351`). Documented at `:1589-1605`.
- **All four conditions must hold:**
  - the marks were staged in the **same** `ContentAddressedTransaction` object;
  - the rmdir path routes to a part dir (`!r->ref.empty() && r->file.empty()`);
  - the ref key matches;
  - it happens before `commit`.
- Upstream fails the first condition: Tx 2 and Tx 3 come from separate `DiskObjectStorage` calls (`DOS/DiskObjectStorage.cpp:364-369`, `:440-448`).
- The comments at `:1044-1046` ("the dominant removal path pays zero repoints") and `:1596-1601` ("The MergeTree fast-removal path … unlinks EVERY part file one-by-one and THEN calls `removeDirectory`" in the same transaction) are false for production.

### Totals per removal (no projections)
| | Manifest PUTs | Ref-log appends | GC owner edges |
|---|---|---|---|
| Today | 2 (B, C) | 6 | A, B, C each gain and lose an owner |
| Minimum | 0 | 1 (`dropRef <part>`) | 1 removal |

Reads per removal:
- rename: 2 resolves, 1 manifest read (cacheable);
- Tx 2: 3 ForceFresh resolves (`unlinkFile`, `publishStaging`, `repointRef`), manifest-B reads;
- Tx 3: 1 resolve;
- `existsDirectory` probes (`CachedForLoad`).

## C. Crash semantics and who reads `delete_tmp_*`

### What upstream relies on
- **Crash after the rename, before or during the removal:**
  - At load, `delete_tmp_*` is skipped because it does not parse as a part name (`MT/MergeTreeData.cpp:2521-2531`).
  - At startup, `clearOldTemporaryDirectories(0, {"tmp_", "delete_tmp_", "tmp-fetch_"})` removes it:
    - `src/Storages/StorageMergeTree.cpp:299`
    - `MT/ReplicatedMergeTreeAttachThread.cpp:199`
    - on DROP, `MT/MergeTreeData.cpp:4221`
  - The removal itself (`MT/MergeTreeData.cpp:3379-3470`) is `iterateDirectory` → `isOldPartDirectory` (`getLastModified` on the dir and each child, `:3351-3362`) → `removeSharedRecursive` (`:3454`).
  - The periodic cleanup uses the default prefixes `{"tmp_", "tmp-fetch_"}` (`MT/MergeTreeData.h:971`, `MT/MergeTreeCleanupThread.cpp:46`, `MT/ReplicatedMergeTreeCleanupThread.cpp:54`). So **a leftover `delete_tmp_` is cleaned only at startup, attach or DROP.**
  - Pinned for non-CAS disks by `tests/integration/test_cleanup_after_start/test.py:48-55`.
  - Net effect: after the rename, upstream already treats the part as gone ("no part").
- **Crash before the rename:** `<part>` reloads. It is either covered and outdated again, or (Replicated, already removed from Keeper, `StorageReplicatedMergeTree.cpp:9291-9317`) handled as unexpected. Upstream already accepts "old part".
- **Partial file removal:** the name is still `delete_tmp_*`, so the same startup cleanup applies. On CAS the state is ref `delete_tmp_<part>` → B or C. Either way it is dropped by `removeRecursive` → `dropRefIfPresent` (`CA/ContentAddressedTransaction.cpp:1141-1145`).
- **In-process failure after the rename:**
  - The part goes back to `Outdated` with a stale `part_dir`.
  - On retry, `remove()` finds `to` present, calls `removeSharedRecursive(to)` (`:954-962`), then finds `from` absent and returns with a warning (`:981-987`).
- The upstream comment admits the gap: "TODO directory delete_tmp_<name> is never removed if server crashes before returning from this function" (`:919`).
- Reason for the rename, per upstream (`:915-917`): it avoids a name collision between, for example, `all_1_2_1` (`Deleting`) and `tmp_merge_all_1_2_1`. On a local disk it also makes "gone" atomic. On CAS a single `dropRef` is already atomic.

### Readers of a `delete_tmp_<part>` ref
- **Data reads, `system.parts`, BACKUP, DETACH: none.**
  - The part is `Deleting` and uniquely owned (`MT/MergeTreeData.cpp:3565`, `:3608`).
  - Its in-memory path still names `<part>` (`MT/DataPartStorageOnDiskBase.cpp:998-1000`).
  - DETACH and BACKUP work on `Active` parts.
- **Fetch / relink confirm:** keyed on the name `<part>` in the ledger. Gate 0 says explicitly that a `delete_tmp_*` rename makes the part object authorize nothing (`MT/DataPartsExchange.cpp:252-260`).
- **CAS internals:** no special handling. The only mention is the grammar note in `CA/Parts/PartPathParser.cpp:130-135`. GC folds it like any other ref.
- **The only readers are the removal itself:**
  - `existsDirectory(to)` (`:954`)
  - `unlinkFile`'s existence proof (`CA/ContentAddressedTransaction.cpp:1628-1640`)
  - the unloaded-projection checksum read (`MT/DataPartStorageOnDiskBase.cpp:1065-1071`)
  - `repointRef`/`publishStaging` re-reads
  - crash-recovery enumeration: `iterateDirectory` + `getLastModified` (`CA/ContentAddressedMetadataStorage.cpp:1814-1843`)
- Verdict: **write-only garbage between the rename and the drop.** The contents are never needed. Only three answers matter to upstream control flow:
  - `existsDirectory(to)` at a retry;
  - `unlinkFile(if_exists=false)` must not throw, or it produces the ERROR fallback log;
  - `isDirectoryEmpty` / `removeDirectory` must succeed.

## D. Other renames of a committed part directory on a CAS disk (must stay real)

Most route through `moveDirectory` → `republishRef` (`CA/ContentAddressedTransaction.cpp:1432-1436`); exceptions are noted.

| Caller | Site | Rename |
|---|---|---|
| Commit of a new part (`renameParts` / `preparePartForCommit`) | `MT/MergeTreeData.cpp:9407`, `:5587` | tmp→final. Staged source: overlay re-key, no republish (`:1357-1430`) |
| Projection merge of a parent | `MT/MergeProjectionPartsTask.cpp:63` | `.tmp_proj` → `.proj` (re-key, `:1325-1338`) |
| Parts mover | `MT/MergeTreePartsMover.cpp:313` | `moving/<p>` → `<p>` |
| Detach, or broken on load | `MT/IMergeTreeDataPart.cpp:2497-2503`, `MT/MergeTreeData.cpp:2665`, `:2980`, `:3073`, `:6058`, `:7997`; `StorageReplicatedMergeTree.cpp:2292`, `:2631`, `:3659`, `:5946` | `<p>` → `detached/<prefix>_<p>` |
| Detached renames (`attaching_`, `deleting_`, `ignored_`) | `PartsTemporaryRename::tryRenameAll` (`MT/MergeTreeData.cpp:5373-5385`); `dropDetached` (`:8644`, `:8656`); ATTACH (`:8822`, `:8897`); `:8863`; `StorageReplicatedMergeTree.cpp:2589` | inside `detached/` |
| Replicated sink async-dedup renames | `MT/ReplicatedMergeTreeSink.cpp:633`, `:685`, `:1127`, `:1144` | tmp ↔ final |
| Fetch replace | `StorageReplicatedMergeTree.cpp:6063` | replaced path |
| RENAME TABLE | `MT/MergeTreeData.cpp:4121` | table dir → table dir: `republishRef` per ref + `dropNamespace` (`CA/ContentAddressedTransaction.cpp:1278-1316`) |
| Clone / freeze / `makeCloneInDetached` | `MT/DataPartStorageOnDiskBase.cpp:544`, `:688`, `:790` | copies, not renames |

- **The only `delete_tmp_` producer is `DataPartStorageOnDiskBase::remove`** (`:945`, `:949`). Nothing reads the destination afterwards except the removal steps in C.
- `dropDetached`'s `deleting_` rename + removal has the same waste pattern: republish, then drop.

## E. Candidate cut points

### (1) CAS-side: treat a rename to a `delete_tmp_*` basename as a drop
- **Site:** `CA/ContentAddressedTransaction.cpp:1432-1436`. Branch on `dst->ref` whose basename starts with `delete_tmp_` and use `dropRefIfPresent(src)` instead of `republishRef`.
- **Required:**
  - Tx 2's `unlinkFile(if_exists=false)` on a ref-less `delete_tmp_*` route must be a no-op, not a throw. Otherwise every removal logs `Cannot quickly remove directory` and falls back to `removeSharedRecursive`, which ends up a harmless `dropRefIfPresent` miss.
  - Tx 3 is already fine: `isDirectoryEmpty` → true, `dropRefIfPresent` → miss.
  - Choose between an in-memory tombstone keyed by `(ns, delete_tmp_<p>)` and a stateless "ref-less `delete_tmp_*` answers empty/absent" rule. A retry's `existsDirectory(to)` = false already leads to `existsDirectory(from)` = false → warning + success, which is the same message as today's retry.
  - Must handle a `moving/` parent (match the basename, not the full ref).
  - Temp parts (`delete_tmp_tmp_*`) have no committed ref and are unaffected.
  - Crash: an atomic drop yields "no part", as upstream does post-rename. That satisfies CAS-83 AC#2.
- **Cost:** zero upstream change, but it sniffs an upstream string literal. Breaks tests that assert the renamed ref exists (see F).
- **Saves:** both manifests and 5 of 6 appends. Projections produce no marks on an absent ref.

### (2) CAS-side: defer the rename's publication and collapse it with the later unlinks/rmdir
- The rename, unlinks and rmdir are **three transactions**, so the deferral must live in the metadata storage, not the transaction. Needed:
  - an alias `delete_tmp_<p>` → `<p>` for reads;
  - marks accumulated against the alias;
  - a `dropRef(<p>)` at rmdir.
- **Crash:** `<p>` survives. Upstream tolerates this ("crash before the rename"), but for Replicated the part is already out of Keeper and loads as unexpected.
- Most state and the most failure windows: marks without an rmdir, and a process-lifetime alias table.

### (3) Upstream: fewer disk transactions
- **(3a)** In `clearDirectory`, wrap `removeSharedFiles` + `removeDirectory` in one `disk->createTransaction()` (`MT/DataPartStorageOnDiskBase.cpp:1130-1140`, about 5 lines).
  - T8 then fires: step 3 disappears, the rename's B and 3 appends remain.
  - Projections still repoint once each, since the rmdir of a projection is a CAS no-op.
  - Local disks use the fake/immediate transaction, so behavior is unchanged there.
  - Object-storage disks lose the `isDirectoryEmpty` guard of `DiskObjectStorage::removeDirectory` (`DOS/DiskObjectStorage.cpp:442-443`), because the transaction path has none (`DOS/DiskObjectStorageTransaction.cpp:205-211`).
- **(3b)** One transaction for the whole `remove()` (rename + all `clearDirectory`) would let CAS stage the rename and collapse it into one drop. But for queued-metadata disks it breaks the unloaded-projection read under `to` (`:1065-1071`), which happens before commit. So it must be gated on a capability. More invasive: it restructures `remove()`.

### (4) Upstream: a disk-type branch in `DataPartStorageOnDiskBase::remove`
- **No `removePart`-style hook exists.**
  - `IDiskTransaction` (`src/Disks/IDiskTransaction.h:36-157`) and `IDataPartStorage` (`MT/IDataPartStorage.h:172-178`) offer only `remove(...)`.
  - No `supportsAtomicPartRemoval` exists.
- **Capability flags that do exist:**
  - `IDisk::isContentAddressed` (`src/Disks/IDisk.h:477`), `isPlain` (`:473`), `isWriteOnce` (`:482`), `supportZeroCopyReplication` (`:464`)
  - `IDataPartStorage::isContentAddressed` (`MT/IDataPartStorage.h:196`)
  - `IMetadataStorage::isContentAddressed` (`DOS/MetadataStorages/IMetadataStorage.h:324`), `transactionIsStagingOverlay` (`:331`)
- **Precedent:** the same file already branches on `isContentAddressed` at `MT/DataPartStorageOnDiskBase.cpp:424`, `:544`, `:688` and `:790`.
- **Option:** if `isContentAddressed()`, skip the rename and `clearDirectory`, and call `disk->removeSharedRecursive(from/)` once.
  - That becomes one transaction → `removeRecursive` → part-dir `dropRefIfPresent(<p>)` (`CA/ContentAddressedTransaction.cpp:1141-1145`) → 1 append, 0 manifests.
  - Keep the `can_remove_callback` evaluation and the `existsDirectory(from)` early return.
  - Projections are virtual, so dropping the ref removes them.
  - Crash: atomic, "old part or no part".
- **Cost:** about 8 lines of fork patch in upstream code, no CAS-layer state. Also covers `DROP TABLE`'s per-part loop (CAS-321.6), leaving only the ref-lane batching across parts.

## F. Tests and counters that pin today's behavior

### gtests that break under (1) or (4)
They assert the renamed ref exists or use the `delete_tmp_` rename as the republish trigger:
- `CASTransactionLockScope.CommittedRefMoveDoesNotSpuriouslyPublish`: `src/Disks/tests/gtest_ca_transaction.cpp:165` (asserts at `:180`)
- `CASWiringOps.CommittedPartRenameMovesTheRef`: `src/Disks/tests/gtest_ca_wiring.cpp:1081` (asserts at `:1094-1095`)
- `CASWiringPrecommitOrder.RepublishRefNoTreeHeadBeforePrecommit`: `gtest_ca_wiring.cpp:2637` (rename at `:2671`, asserts at `:2702-2703`). Could re-target to another rename, such as detach.

### T8 and repoint semantics (keep)
- `CASTransactionRemove.UnlinkStormThenDirDropIsOneRefDrop`, `gtest_ca_transaction.cpp:586`: single-transaction shape. Its comment claims to mirror `IMergeTreeDataPart::remove`; it does not.
- `UnlinkStormMemoizesOneForceFreshResolve`, `:626`
- `SurgicalUnlinkRepoints`, `:545`: a lone unlink produces one repoint
- `CASTransactionRepoint.*`, `:383`, `:425`
- `CASRepoint.ByteEqualIsNoOp` / `AddFileRepoints`: `src/Disks/tests/gtest_cas_repoint.cpp:54`, `:79`
- `CASPartFolderAccess.RepublishRef*`: `gtest_cas_part_folder_access.cpp:313`, `:331`
- `CASPromoteRepublish.*`: `gtest_cas_promote_republish.cpp:93-238`
- `CASPartWriteTxnRepoint.PromoteRepointsCommittedRef`: `gtest_cas_part_write.cpp:1938`

### Other tests
- No stateless test under `tests/queries` counts `CASRefRepoint` or reads `ref_repoint`/`build_publish`. A grep over `tests/` finds only `tests/integration/test_gcs_live/test.py:549`, which is a column-meaning comment.
- Upstream `delete_tmp_` startup cleanup: `tests/integration/test_cleanup_after_start/test.py:48-55`. `test_zero_copy_lock_leak/test.py:16`, `:116` depends on the rename existing on zero-copy disks, not CAS.

### ProfileEvents
All in `src/Common/ProfileEvents.cpp`.

| Counter | Where it is incremented | Per removal today → after the cut |
|---|---|---|
| `CASRefRepoint` (`:848`) | `CA/Parts/PartFolderAccess.cpp:545` | 1 + projections → 0 |
| `CASManifestPut` (`:849`) | the backend instrumentation's manifest class (`CA/Backend/CasInstrumentedBackend.cpp:97`) | 2 → 0 |
| `CASRefBatchFlushes` / `CASRefBatchedMutations` (`:801-802`) | `CA/Pool/CasRefLedger.cpp:4110-4111` | 6 mutations → 1 |
| `CASRefQueueWaitMicroseconds` (`:804`) | | ref-lane wait |
| `CASRefCheckpointPublished` (`:839`) | `CasRefLedger.cpp:4419` | snapshot-driven, not per append |

`system.cas_log` event types: `manifest_put` (`CasPartWriteTxn.cpp:689`), `precommit` (`:805`), `build_publish` (`:1025`), `ref_repoint` (`:1051`), `ref_drop` (`CasRefLedger.cpp:4871`, `PartFolderAccess.cpp:654`).

## Summary

The most promising cut point is (4): one branch in `DataPartStorageOnDiskBase::remove` (`MT/DataPartStorageOnDiskBase.cpp:908`). If `isContentAddressed()`, skip the `delete_tmp_` rename and `clearDirectory` and issue a single `disk->removeSharedRecursive(from)`, which CAS turns into one `dropRefIfPresent(<part>)`. The file already has four `isContentAddressed` branches. It needs no CAS-layer state and no string sniffing. Crash semantics stay "old part or no part", which is what upstream already accepts on either side of the rename.

The zero-upstream alternative is (1), in `ContentAddressedTransaction::moveDirectory` (`:1432-1436`). It drops at the rename and requires `unlinkFile` to be a no-op on a ref-less `delete_tmp_*` route.

The one fact a designer must not get wrong: in production the unlink batch and the rmdir are **separate disk transactions** (`DOS/DiskObjectStorage.cpp:364-369` and `:440-448`, called at `MT/DataPartStorageOnDiskBase.cpp:1139-1140`). So the T8 supersede-clear never fires, and `UnlinkStormThenDirDropIsOneRefDrop` pins a shape upstream never produces. Any fix must be verified against the real `IMergeTreeDataPart::remove` call sequence: count `CASManifestPut` and `CASRefRepoint` per removed part on a real table, not in the single-transaction gtest.
