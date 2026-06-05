---
description: Design spec for BACKUP/RESTORE on a content-addressed MergeTree disk (B16/B34) — route the RESTORE part-materialization through one whole-part ContentAddressedTransaction instead of per-file autocommit, mirroring the FREEZE clone path.
sidebar_label: 'CAS MergeTree BACKUP/RESTORE'
sidebar_position: 13
slug: /superpowers/specs/cas-mergetree-backup-restore
title: 'Content-Addressed MergeTree — BACKUP / RESTORE Design'
doc_type: 'guide'
---

# Content-Addressed MergeTree — BACKUP / RESTORE Design (B16/B34) {#cas-backup-restore}

**Status:** design spec, awaiting review. **Date:** 2026-06-05. **Backlog:** B16/B34 (BACKUP/RESTORE on CA).
**Branch:** `cas-mergetree-poc`.

## 1. Goal and scope {#goal}

Make `RESTORE` of a MergeTree table/partition work onto a content-addressed (CA) disk. `BACKUP` of a CA table
already works (it reads parts via `getStorageObjects`, which CA supports). The 13 gated BACKUP/RESTORE
stateless tests all fail on **one root cause** on the RESTORE side.

**In scope:** route the RESTORE part-materialization through a single whole-part `ContentAddressedTransaction`
(CA-conditional), so a restored part lands as one content-addressed part; un-gate the BACKUP/RESTORE tests
that share this root cause; an inline-CA `BACKUP → RESTORE → SELECT` oracle.
**Out of scope / re-gate with reasons:** tests that fail for a genuinely orthogonal reason — a backup
*destination* that is itself a special disk (`03831_backup_archive_to_plain_rewritable_disk`), or a test that
needs real S3 credentials (`02843_backup_use_same_s3_credentials_for_base_backup`); the content-addressing
core, `part_id` semantics, and the GC/transaction work already built (preserved).

## 2. Root cause (verified) {#root-cause}

`MergeTreeData::restorePartFromBackup` (`MergeTreeData.cpp:7311`) materializes a restored part by, for each
file in the backup, calling `backup->copyFileToDisk(part_path_in_backup / filename, disk,
temp_part_dir / filename, WriteMode::Rewrite)` — a **per-file autocommit write** into a
`tmp_restore_<part>/` directory; it then `loadPartRestoredFromBackup`s the part and
`attachRestoredParts` renames it active. A content-addressed disk models a part as **one manifest published
atomically** (N files → one manifest → one ref), so a per-file autocommit write of a *content* part file is
rejected:

```
Code: 48. DB::Exception: Autocommit writes are not supported for content part files on a content-addressed disk. (NOT_IMPLEMENTED)
```

(43 occurrences across the 13 tests in the night-triage run.) This is structurally identical to the problem
`FREEZE` already solved: `DataPartStorageOnDiskBase::freeze` (`:514`) self-creates **one** whole-part disk
transaction when none is supplied (`owned_transaction = disk->createTransaction()` gated on
`disk->isContentAddressed()`), so all the clone's files land in a single content-addressed part instead of N
one-file autocommits (the B21 corruption mode).

## 3. Design — restore through one whole-part CA transaction {#design}

### 3.1 The transaction wrap (the core change) {#wrap}
In `restorePartFromBackup`, when `disk->isContentAddressed()`, create one CA disk transaction and copy every
backup file of the part through it, then commit once:

```
DiskTransactionPtr tx = disk->isContentAddressed() ? disk->createTransaction() : nullptr;
for (filename : filenames) {              // skipping txn_version.txt / metadata_version.txt as today
    if (tx) {                             // CA: read backup entry -> write through the whole-part tx
        auto in  = backup->readFile(part_path_in_backup_fs / filename);
        auto out = tx->writeFile(temp_part_dir / filename, /*buf_size*/…, WriteMode::Rewrite, write_settings);
        copyData(*in, *out);
        out->finalize();
    } else {                              // non-CA: unchanged
        backup->copyFileToDisk(part_path_in_backup_fs / filename, disk, temp_part_dir / filename, WriteMode::Rewrite);
    }
}
if (tx) tx->commit();                     // publishes ONE manifest + ref for tmp_restore_<part>
```

The N files (including projection `<proj>.proj/<inner>` files — written through the same transaction) become
one content-addressed part at `tmp_restore_<part>`. Non-CA disks keep the current per-file `copyFileToDisk`
autocommit path **unchanged** (the `tx == nullptr` branch). The chosen copy mechanism is **inline**
(`backup->readFile` → `tx->writeFile` → `copyData` → `finalize`) rather than a new transaction-aware
`IBackup::copyFileToDisk` overload — localized, no `IBackup` API change (YAGNI).

### 3.2 Attach (the rename) {#attach}
After `tx->commit()`, the existing `loadPartRestoredFromBackup` loads the part from `tmp_restore_<part>` and
`attachRestoredParts` renames it to active. On CA the rename is a `moveDirectory` re-key
(`tmp_restore_<part>` → `<part>`). CA's `moveDirectory` already re-keys the analogous staging prefixes
(`tmp_merge_`, `attaching_`, `tmp_fetch_`); if `tmp_restore_` is not already covered by the generic part-dir
re-key, add it. (Verify reproduction-driven — the generic part-dir → part-dir re-key may already handle it.)

### 3.3 Compose-with-existing (verify in tests, likely no new design) {#compose}
- **Version / mutable files.** `restorePartFromBackup` already SKIPS `txn_version.txt` and
  `metadata_version.txt` (`:7352-7358`); `setAndStoreCreationTID` writes `txn_version.txt` afterward. On CA
  these are mutable per-part sidecar files; the mutable-file handling from the transaction work composes.
- **Broken parts.** `loadPartRestoredFromBackup` can mark a part broken and restore it to `detached/`. On CA a
  detached part routes through the shared `detached` ref (already supported); preserve this fallback.
- **Projections.** A restored part's `<proj>.proj/` files go through the same whole-part transaction, landing
  nested in the part's manifest (Approach A, the existing projection-on-CA mechanism).

### 3.4 What does NOT change {#unchanged}
`BACKUP` (read side); every non-CA disk (the `tx == nullptr` branch is byte-identical to today); `part_id`
identity (content-only); the GC/transaction/FREEZE/FETCH features. RESTORE preserves the exact part (name,
partition, checksums) — it does **not** re-INSERT the data (which would change part identity / block numbers
and break replicated coordination); it writes the same part atomically.

## 4. Error handling {#errors}
Fail-closed: the whole-part `commit` is atomic, so a failed/aborted restore publishes **no partial part** (no
one-file ref, no dangling manifest). A broken part still routes to `detached/` via the preserved fallback. An
unreadable backup entry throws the normal restore exception before any ref is published.

## 5. Testing {#testing}
Reproduction-driven (the night-triage pattern):
- **Un-gate + run the 13** (`02843_backup_use_same_password_for_base_backup`,
  `02843_backup_use_same_s3_credentials_for_base_backup`, `02864_restore_table_with_broken_part`,
  `02974_backup_query_format_null`, `03001_backup_matview_after_modify_query`,
  `03001_restore_from_old_backup_with_matview_inner_table_metadata`, `03032_async_backup_restore`,
  `03145_non_loaded_projection_backup`, `03214_backup_and_clear_old_temporary_directories`,
  `03286_backup_to_memory`, `03315_query_log_privileges_backup_restore`, `03760_backup_tar_archive`,
  `03831_backup_archive_to_plain_rewritable_disk`) on the CA-default job; fix until the core (~10-11) passes.
- **Re-gate true orthogonals** with precise reasons (`03831` → plain_rewritable *destination*; any test that
  genuinely needs real S3 credentials), not the CA-restore-write gap.
- **Inline-CA oracle:** a CA table; INSERT deterministic rows; `BACKUP TABLE … TO Disk/Memory`; `RESTORE …
  AS <copy>`; `SELECT` from the restored copy equals the source (round-trip equality) — proving RESTORE
  materializes a usable content-addressed part. Cover a part with a projection.
- **Non-CA regression:** a couple of plain BACKUP/RESTORE tests on the default job → unchanged (the CA wrap is
  `isContentAddressed`-gated; the `tx == nullptr` path is the old behavior).
- **Unit/gtest (if a CA seam changes):** if `moveDirectory` gains `tmp_restore_` recognition, a gtest pins the
  re-key; otherwise the stateless oracle suffices.

## 6. Plan phasing {#phasing}
1. The `restorePartFromBackup` CA-conditional whole-part transaction wrap (§3.1) + the `tmp_restore_` re-key
   if needed (§3.2); build.
2. Un-gate the 13, run on CA-default, reproduction-driven fixes (projections, broken-part, mutable files);
   re-gate orthogonals with reasons; the inline-CA oracle.
3. Non-CA regression + backlog (B16/B34 done; list un-gated + re-gated) + push.

## 7. Risks {#risks}
- **Restore staging path shape.** The `tmp_restore_<part>` dir + the per-file write through the transaction
  must parse to the CA part namespace exactly like an INSERT's `tmp_insert_<part>`. If the restore temp path
  shape differs (e.g. an extra suffix), the parser / re-key may need a small extension — reproduction-driven.
- **Projections / broken parts / matview inner tables.** These add file-shape variety to the restored part;
  the whole-part transaction should absorb them, but the matview/broken/projection tests are where a residual
  gap (if any) surfaces — bounded, caught by the un-gate run.
- **Backup-destination disks.** `03831` (plain_rewritable destination) and the s3-credentials test may be
  orthogonal to the CA *source* restore; re-gate with a precise reason rather than forcing.
