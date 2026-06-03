---
description: 'M9 W2 — enable partition-clone ALTERs (ATTACH/REPLACE/MOVE PARTITION, ATTACH PART, FREEZE) on a content_addressed disk. The whole-part clone transaction already exists in DataPartStorageOnDiskBase::freeze; W2 lifts the now-stale checkAlterPartitionIsPossible gate, empirically verifies each clone path produces one ref to the source part_id, and adds a regression gtest.'
sidebar_label: 'CAS M9 W2 partition clone'
sidebar_position: 13
slug: /superpowers/plans/cas-mergetree-m9w2
title: 'Content-Addressed MergeTree — M9 W2 (enable partition clone / FREEZE / ATTACH/MOVE/REPLACE)'
doc_type: 'guide'
---

# CAS M9 W2 — enable partition clone on a content_addressed disk {#m9w2}

> **For agentic workers:** `superpowers:subagent-driven-development` + the `cas-test-triage` skill for
> the un-tag/run/triage loop. Empirical: the now-runnable clone tests are the oracle.

**Goal:** `ATTACH PART`, `ATTACH PARTITION … FROM`, `REPLACE PARTITION`, `MOVE PARTITION` (same disk),
and `FREEZE` work on a content-addressed disk — a clone publishes ONE ref to the existing `part_id`
(identical content → same blobs/manifest/part_id), zero byte copy.

**Key fact (discovered):** the whole-part clone transaction **already exists** —
`DataPartStorageOnDiskBase::freeze` (`:514-558`) wraps the per-file hardlink `Backup` in one
self-created disk transaction when `disk->isContentAddressed()`, committing once. That is how `DETACH
PARTITION` already works (B36/B46). So the per-file-autocommit B21 hazard is already closed for the
`freeze` path; the clone-class is only **gated** at `MergeTreeData::checkAlterPartitionIsPossible`
(`:6584-6611`) whose "file-by-file, no transaction" rationale is now stale.

**Architecture:** lift the gate for the clone-class; rely on the existing transactional `freeze` +
the CA transaction's `createHardLink` carry-forward + single-ref commit. Empirically verify each clone
path on CA (the gate meant they were never exercised). Cross-disk `MOVE PARTITION TO DISK/VOLUME`
(uses `clonePart` → full byte copy, a different path) stays out of scope; `BACKUP` stays gated (B34).

## Build & test {#build}
`cmake --build build --target clickhouse unit_tests_dbms`; CA gtests `--gtest_filter='ContentAddressed*'`.
Stateless via the `cas-test-triage` skill (CA-default job; **assert non-empty `--test`**; foreground;
bounded; subagent runs+triages). Allman; `DB::Exception`+`ErrorCodes`.

## Tasks {#tasks}

### Task 1 — gtest: a clone publishes ONE ref to the source part_id (TDD) {#t1}
**File:** `src/Disks/tests/gtest_content_addressed_metadata.cpp`.
- [ ] Build a source part via the transaction (multiple files). Clone it to a new `(uuid, part_name)`
  through the transactional `freeze`-style path — i.e. open a transaction, `createHardLinkFrom` every
  source file into the destination part dir, `commit`. Assert: (a) the destination resolves a SINGLE
  ref whose `part_id` EQUALS the source's `part_id` (identical content → same manifest), (b) ALL the
  source's files resolve under the destination (NOT just the last one — the B21 regression), (c) no
  new blob objects were created (pure re-reference). Mirror `MutationCarryForwardReusesUnchangedBlobs`
  + `MoveCommittedPartIntoDetachedRekeysRef`.
- [ ] Build + run `--gtest_filter='*Clone*:ContentAddressed*'` → green.
- [ ] Commit `CAS M9 W2: gtest — whole-part clone publishes one ref to the source part_id`.

### Task 2 — lift the partition-clone gate for CA {#t2}
**File:** `src/Storages/MergeTree/MergeTreeData.cpp` (`checkAlterPartitionIsPossible`, the
`ContentAddressed` case `:6584-6611`).
- [ ] Expand `supported_commands` to add the clone-class now that `freeze` is transactional:
  `ATTACH_PARTITION`, `ATTACH_PART`, `REPLACE_PARTITION` (covers `ATTACH PARTITION … FROM`),
  `MOVE_PARTITION`, `FREEZE_PARTITION`, `FREEZE_ALL`, `UNFREEZE_PARTITION`, `UNFREEZE_ALL`. Keep the
  existing `DROP_PARTITION`/`DROP_DETACHED_PARTITION`. Replace the stale comment with: the clone path
  is now transactional (`DataPartStorageOnDiskBase::freeze` runs the whole clone through one
  CA transaction → one ref to the source `part_id`); cross-disk `MOVE … TO DISK/VOLUME` (byte-copy
  `clonePart`) and `BACKUP` remain out (B34) and fail closed where unsupported. `FETCH_PARTITION`
  stays rejected (replication, B1).
- [ ] Build clean.
- [ ] Commit `CAS M9 W2: lift partition-clone gate on CA (freeze is now transactional)`.

### Task 3 — empirically verify + un-tag the clone tests (cas-test-triage skill) {#t3}
- [ ] Enumerate the `no-content-addressed-storage` tests gated for partition-clone (the ~29
  partition-clone set + the `ATTACH PART`/`REPLACE…CLONE` tests re-tagged in W1: `01015_attach_part`,
  `03918_replace_table_clone_as_verbose_result`, `02960_alter_table_part_query_parameter`,
  `04005_empty_part_name`, `02454_compressed_marks_in_compact_part`, `02346_text_index_detach_attach`,
  `01451_detach_drop_part`). EXCLUDE the cross-disk-MOVE and BACKUP ones.
- [ ] Un-tag them; run under CA-default (subagent, guarded selector, bounded). Triage per the skill:
  pass → keep un-tagged; **real CA bug** (wrong data / not one-ref) → fix at the source (the
  freeze/clone path) + regression; cross-disk-MOVE / genuinely-unsupported → re-tag with reason.
- [ ] Commit the un-tags + any fix; record findings + remaining exceptions in the backlog.

## Verify — HARD GATE {#verify}
- CA gtests green incl. the one-ref clone test. Build clean.
- The un-tagged clone stateless tests pass under CA-default with NO data corruption (the B21 mode —
  a clone reading back only its last file — must NOT recur).
- Partition-clone backlog (B21) updated: closed for same-disk clones via the transactional freeze;
  cross-disk MOVE + BACKUP noted as remaining.

## Self-review {#self-review}
- **Coverage:** gate lift (T2), one-ref invariant (T1), empirical proof + un-tag (T3).
- **Safety:** the B21 corruption is closed by the already-existing transactional `freeze`; T1 pins
  one-ref-per-clone; T3's real-data tests prove no corruption. Cross-disk MOVE (byte-copy path) and
  BACKUP stay gated.
- **No new mechanism:** reuses the existing transactional `freeze` + CA carry-forward; W2 is gate +
  verification, not a new contract.
