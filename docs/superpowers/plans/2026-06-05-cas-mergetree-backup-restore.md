# CAS MergeTree BACKUP/RESTORE (B16/B34) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`). Build to a log (`ninja -C build clickhouse > build/<log> 2>&1`, NO `-j`/`nproc`); summarize via a subagent. Tests FOREGROUND, bounded (`timeout` ≤ 590), non-empty `--test`, never `clickhouse local`, never background a build/test.

**Goal:** Make `RESTORE` work onto a content-addressed (CA) disk by routing the restore part-materialization through one whole-part `ContentAddressedTransaction` instead of per-file autocommit. `BACKUP` (read side) already works. Un-gate the 13 BACKUP/RESTORE stateless tests that share this root cause; re-gate true orthogonals.

**Architecture:** `MergeTreeData::restorePartFromBackup` copies each backup file into a `tmp_restore_<part>-XXXX` dir via `backup->copyFileToDisk(…, WriteMode::Rewrite)` — a per-file autocommit CA rejects (content part files must commit as one manifest). Mirror `DataPartStorageOnDiskBase::freeze` (which self-creates one whole-part CA transaction when none is supplied): when `disk->isContentAddressed()`, create one CA disk transaction, write each backup file through it (`backup->readFile` → `tx->writeFile` → `copyData` → `finalize`), then `tx->commit()` once → one content-addressed part. Non-CA disks keep the unchanged `copyFileToDisk` path. The existing load + `attachRestoredParts` (rename `tmp_restore_<part>-XXXX` → active) then proceeds.

**Spec:** `docs/superpowers/specs/2026-06-05-cas-mergetree-backup-restore-design.md`. **Branch:** `cas-mergetree-poc` (never master). Trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Seams (verified):**
- `MergeTreeData::restorePartFromBackup` (`MergeTreeData.cpp:7311`): the per-file copy loop (`:7340-7362`) calls `backup->copyFileToDisk(part_path_in_backup_fs / filename, disk, temp_part_dir / filename, WriteMode::Rewrite)` (`:7360`); it SKIPS `txn_version.txt`/`metadata_version.txt` (`:7352-7358`). Temp dir = `tmp_restore_<part>-XXXX` (`RestoredPartsHolder::getTemporaryDirectory`, `:7220-7226`, via `TemporaryFileOnDisk`). Then `loadPartRestoredFromBackup` (`:7370`) + `restored_parts_holder->addPart` → later `attachRestoredParts`.
- `IBackup::readFile(const String &) -> std::unique_ptr<ReadBufferFromFileBase>` (`IBackup.h:114`). `copyData` in `<IO/copyData.h>`.
- `IDisk::createTransaction()` → `DiskTransactionPtr`; `DiskObjectStorageTransaction::writeFile(path, buf_size, mode, settings)` returns a write buffer; `tx->commit()` publishes the CA part (the path the writer is parsed for `(table_uuid, part_name)` is `temp_part_dir / filename`). The FREEZE template: `DataPartStorageOnDiskBase.cpp:514-523` (`owned_transaction = disk->createTransaction()` gated on `!params.external_transaction && disk->isContentAddressed()`, then `owned_transaction->commit()`).
- CA part-dir recognition is by SHAPE (`PoolPaths.cpp:422-426` — `tmp_insert_`/`tmp_merge_`/`delete_tmp_` all parse as the part-dir component), so `tmp_restore_<part>-XXXX` parses with NO new prefix. The `tmp_restore → active` rename is the generic committed-part rename (`renameCommittedPartRef`, `ContentAddressedTransaction.cpp`) — likely already covered (verify in Phase 2).

---

## Phase 1 — the restore whole-part transaction wrap

### Task 1: route the restore part-write through one CA whole-part transaction

**Files:** Modify `src/Storages/MergeTree/MergeTreeData.cpp` (`restorePartFromBackup`)

- [ ] **Step 1:** in `restorePartFromBackup`, after `disk->createDirectories(temp_part_dir)` and before the copy loop, create the CA-conditional transaction:
```cpp
    DiskTransactionPtr restore_tx;
    if (disk->isContentAddressed())
        restore_tx = disk->createTransaction();
```
- [ ] **Step 2:** in the per-file loop, route the copy through the transaction on CA (keep the skip of `txn_version.txt`/`metadata_version.txt` and the subdir `createDirectories` exactly as today):
```cpp
        if (restore_tx)
        {
            /// A content-addressed disk publishes a part as ONE manifest (N files -> one ref) atomically;
            /// the per-file copyFileToDisk autocommit below is rejected for content part files. Route the
            /// restore through one whole-part transaction so all files land in a single content-addressed
            /// part (mirrors DataPartStorageOnDiskBase::freeze's owned_transaction). The part is published
            /// at tmp_restore_<part>-XXXX by tx->commit() and renamed active by attachRestoredParts.
            auto in = backup->readFile(part_path_in_backup_fs / filename);
            auto out = restore_tx->writeFile(temp_part_dir / filename, DBMS_DEFAULT_BUFFER_SIZE, WriteMode::Rewrite, getContext()->getWriteSettings());
            copyData(*in, *out);
            out->finalize();
            /// reservation accounting: subtract the backup file size as the copyFileToDisk path does.
            size_t file_size = backup->getFileSize(part_path_in_backup_fs / filename);
            reservation->update(reservation->getSize() - file_size);
        }
        else
        {
            size_t file_size = backup->copyFileToDisk(part_path_in_backup_fs / filename, disk, temp_part_dir / filename, WriteMode::Rewrite);
            reservation->update(reservation->getSize() - file_size);
        }
```
Add `#include <IO/copyData.h>` if not present. Confirm `getContext()->getWriteSettings()` is the right write-settings source here (mirror what `freeze`/`cloneAndLoadDataPart` pass); if `restorePartFromBackup` is `const` and `getContext()` isn't reachable, use the available context/write-settings the function already has, or `WriteSettings{}`.
- [ ] **Step 3:** after the loop, commit the CA transaction before loading the part:
```cpp
    if (restore_tx)
        restore_tx->commit();
```
- [ ] **Step 4: build** `ninja -C build clickhouse > build/br_t1_build.log 2>&1; echo "exit=$?"; grep -cE "error:|FAILED:" build/br_t1_build.log` → 0 errors. (Summarize via a subagent.)
- [ ] **Step 5: commit** `CAS BACKUP/RESTORE: route restore part-write through one whole-part CA transaction (B16/B34)`.

---

## Phase 2 — un-gate + reproduction-driven fixes + oracle

### Task 2: un-gate the 13 BACKUP/RESTORE tests, run, triage

**Files:** Modify the 13 gated tests under `tests/queries/0_stateless/`.

- [ ] **Step 1:** ensure the binary symlink is fresh (`ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse`). Un-tag (remove `no-content-addressed-storage` + the B16/B34 reason comment; preserve other tags): `02843_backup_use_same_password_for_base_backup.sh`, `02843_backup_use_same_s3_credentials_for_base_backup.sh`, `02864_restore_table_with_broken_part.sh`, `02974_backup_query_format_null.sh`, `03001_backup_matview_after_modify_query.sh`, `03001_restore_from_old_backup_with_matview_inner_table_metadata.sh`, `03032_async_backup_restore.sh`, `03145_non_loaded_projection_backup.sh`, `03214_backup_and_clear_old_temporary_directories.sh`, `03286_backup_to_memory.sql`, `03315_query_log_privileges_backup_restore.sh`, `03760_backup_tar_archive.sh`, `03831_backup_archive_to_plain_rewritable_disk.sh`.
- [ ] **Step 2: run in two batches** on the CA-default job (foreground, `timeout 590`, non-empty `--test`, guard `[ -n … ]`), e.g.:
```bash
sel="02864_restore_table_with_broken_part 02974_backup_query_format_null 03001_backup_matview_after_modify_query 03001_restore_from_old_backup_with_matview_inner_table_metadata 03032_async_backup_restore 03145_non_loaded_projection_backup 03286_backup_to_memory"
timeout 590 python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" --test "$sel" > build/br_t2a.log 2>&1
echo "exit=$?"; grep -hoE "Failed: [0-9]+, Passed: [0-9]+, Skipped: [0-9]+" build/br_t2a.log | tail -1; grep -E "0286|0297|0300|0303|0314|0328" ci/tmp/test_result.txt
```
and a second batch for the rest (`02843`×2, `03214`, `03315`, `03760`, `03831`).
- [ ] **Step 3: reproduction-driven fixes.** The wrap should fix the core (~10-11). For each remaining failure read the diff (`grep -A 30 FAIL ci/tmp/test_result.txt | head`) + server err log (`grep -iE "content.?address|NOT_IMPLEMENTED|Autocommit|FILE_DOESNT_EXIST|LOGICAL_ERROR|CORRUPTED|tmp_restore|moveDirectory|projection|detached|No such file" ci/tmp/var/log/clickhouse-server/clickhouse-server.err.log | tail -50`). Likely touchpoints (fix minimally, CA-side only):
  - **`tmp_restore_` rename** — if `attachRestoredParts`'s rename of `tmp_restore_<part>-XXXX` → active is not handled by the generic committed-part rename, extend the CA `renameCommittedPartRef`/`moveDirectory` (verify first — likely already works).
  - **Projections** (`03145`) — projection `<proj>.proj/` files must fold into the one whole-part manifest (the transaction writes them through the same `tx`); should compose with the existing projection-on-CA mechanism.
  - **Broken-part-as-detached** (`02864`) — a broken restored part routes to `detached/` (the CA `detached` ref); preserve the fallback.
  - **Matview inner tables** (`03001`×2) — the matview's inner table restore goes through the same path; should compose.
  After any CA-source change, rebuild foreground + re-run the 122+ `ContentAddressed*` gtests (must stay green) + re-run the failing test.
- [ ] **Step 4: re-gate true orthogonals** with precise reasons (do NOT force): `03831_backup_archive_to_plain_rewritable_disk` likely fails because the backup *destination* is a plain_rewritable disk (orthogonal to the CA *source* restore); `02843_backup_use_same_s3_credentials_for_base_backup` may need real S3 credentials. Re-gate only if the failure is genuinely not the CA-restore path. Note each.
- [ ] **Step 5: commit** the un-gates + any CA fixes + documented re-gates. `CAS BACKUP/RESTORE: un-gate the cluster on CA (<N> pass, <M> re-gated orthogonal)`.

### Task 3: inline-CA BACKUP→RESTORE→SELECT oracle

**Files:** Create `tests/queries/0_stateless/<NNNNN>_content_addressed_backup_restore.sh` (via `add-test <name>.sh`)

- [ ] **Step 1:** a CA table on an inline content-addressed disk (copy the disk block from `05003_content_addressed_freeze.sh`/`05004_content_addressed_transactions.sh`); INSERT deterministic rows (include a part with a projection); `BACKUP TABLE … TO Disk('…','…')` (or `Memory`); `RESTORE TABLE … AS <copy> FROM …`; `SELECT` from the restored copy and assert it equals the source (row count + a checksum/ordered sample) — proving RESTORE materialized a usable content-addressed part. Hand-verify the reference.
- [ ] **Step 2: run on CA-default + plain** → `Passed: 1` on both (the inline CA disk works on both jobs).
- [ ] **Step 3: commit** `CAS BACKUP/RESTORE: inline-CA oracle (backup → restore → select round-trip)`.

---

## Phase 3 — non-CA regression, finalize, push

### Task 4: non-CA regression + backlog + push

- [ ] **Step 1: non-CA regression** — run 2-3 plain BACKUP/RESTORE tests on the PLAIN job (e.g. `02974_backup_query_format_null 03286_backup_to_memory`) to confirm the `isContentAddressed`-gated wrap didn't change the non-CA path:
```bash
timeout 590 python3 -m ci.praktika run "Stateless tests (arm_binary, parallel)" --test "02974_backup_query_format_null 03286_backup_to_memory" > build/br_t4_plain.log 2>&1
echo "exit=$?"; grep -hoE "Failed: [0-9]+, Passed: [0-9]+" build/br_t4_plain.log | tail -1
```
Expect pass (the `restore_tx == nullptr` branch is byte-identical to today).
- [ ] **Step 2:** all `ContentAddressed*` gtests green.
- [ ] **Step 3: backlog** — `docs/superpowers/deferred_backlog/cas-mergetree-integration.md`: B16/B34 → DONE (RESTORE materializes parts through a whole-part `ContentAddressedTransaction`; BACKUP-read already worked). List the un-gated tests + any re-gated with reasons. Note the oracle `<NNNNN>`.
- [ ] **Step 4: commit + push** `git push filimonov cas-mergetree-poc`.

---

## Done criteria
- `RESTORE` onto a CA disk materializes each restored part as one content-addressed part (one manifest/ref) via a whole-part transaction; non-CA restore is byte-identical to today.
- The BACKUP/RESTORE tests pass on the CA-default job (or are individually re-gated for a documented orthogonal reason — destination disk / real-S3, not the CA-restore-write gap).
- Inline-CA `BACKUP → RESTORE → SELECT` oracle proves round-trip equality (incl. a projection).
- No non-CA regression; `ContentAddressed*` gtests green. Backlog B16/B34 done.
