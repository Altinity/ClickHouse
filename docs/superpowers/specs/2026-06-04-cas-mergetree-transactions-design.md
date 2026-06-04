---
description: Design spec for MergeTree transactions (MVCC) on a content-addressed disk — satisfy the per-part txn_version.txt storage contract (gate decoupling, replaceFile, a mutable-only commit branch, rollback-reload hardening); the MVCC engine itself is storage-agnostic and unchanged.
sidebar_label: 'CAS MergeTree Transactions'
sidebar_position: 9
slug: /superpowers/specs/cas-mergetree-transactions
title: 'Content-Addressed MergeTree — Transactions (MVCC) Design'
doc_type: 'guide'
---

# Content-Addressed MergeTree — Transactions (MVCC) Design {#cas-transactions}

**Status:** design spec, awaiting review. **Date:** 2026-06-04. **Backlog:** B39 (the transaction gate).
**Target:** full parity — all ~18 gated transaction/isolation stateless tests pass on a content-addressed
(CA) disk.

## 1. Goal and scope {#goal}

Make MergeTree transactions (`BEGIN TRANSACTION` / `COMMIT` / `ROLLBACK`, implicit transactions, snapshot
isolation, mutations/`DELETE`-in-transaction) work on a CA disk. They are blocked today by a capability
gate (`StorageMergeTree::support_transaction = false` for CA), which throws `NOT_IMPLEMENTED` at the first
DML inside a transaction.

**Key framing (verified by two probes):** the MVCC *engine* — snapshot/CSN assignment, the
`TransactionLog`, visibility, rollback bookkeeping, the in-memory `DataPartsLock` serialization — is
**storage-agnostic** and works unchanged. The CA-side job is solely to satisfy the per-part
`txn_version.txt` storage contract. Ordinary S3-backed MergeTree already supports transactions
(`MetadataStorageFromDisk::supportWritingWithAppend` → `true`); CA is excluded *only* because
`ContentAddressedMetadataStorage` inherits the base `false`. So this is a CA-specific, bounded gap — not an
object-storage-wide limitation, and not a from-scratch MVCC build.

**In scope:** the four CA-side pieces in §3; un-gating the ~18 transaction tests (reproduction-driven, in
waves); gtests + an inline-CA oracle.
**Out of scope:** any change to the MVCC engine, the `TransactionLog`, or non-CA disks; making transactions
work across the *replicated/shared-pool* dimension beyond what the single-server tests exercise (the gated
set is single-server stateless).

## 2. The disk-side contract (verified) {#contract}

Transactions touch the disk only through the per-part `txn_version.txt` (`VersionMetadata` /
`VersionMetadataOnDisk`) and the part lifecycle. `txn_version.txt` is already one of CA's
`kMutablePerPartFiles` (with `uuid.txt`, `metadata_version.txt`): excluded from the content `part_id`
(`computePartId`) and stored in the per-ref **sidecar**, not the immutable manifest. The three interactions:

- **(A) Write/rewrite `txn_version.txt` on a part.** Two situations:
  - *In-transaction* (INSERT): the new part's whole-part CA commit stamps the creation TID. `VersionMetadataOnDisk`
    writes `txn_version.txt.tmp` (Rewrite) then `replaceFile(tmp, txn_version.txt)` **inside the part-commit
    transaction**. CA does not implement `replaceFile` on its metadata transaction → the base
    `throwNotImplemented` fires (**Probe gap #1**, `NOT_IMPLEMENTED` at `MergeTreeSink::commitPart`).
  - *Post-commit, standalone*: filling in the **creation CSN** on `COMMIT`, and **locking/unlocking the
    removal TID** (`VersionMetadataOnDisk::tryLockRemovalTID`/`unlockRemovalTID`) when a
    `DELETE`/mutation/`DROP`/`TRUNCATE`-in-transaction marks an **already-committed** part for removal.
    These rewrite `txn_version.txt` on a part whose ref is already published, via a fresh autocommit
    transaction (`DataPartStorageOnDiskFull::executeWriteOperation`). On CA that fresh transaction's `commit`
    would build `manifest.blobs = recorded` (≈empty) and republish the ref to an empty manifest —
    **clobbering the part** (the central problem this spec solves).
- **(B) Read `txn_version.txt` for visibility.** Every snapshot SELECT reads it to decide part visibility
  (creation CSN ≤ snapshot; removal CSN absent or > snapshot). **Already works** — CA serves mutable
  per-part files from the sidecar (the read-your-writes overlay from B59 also covers in-flight reads).
- **(C) Part lifecycle.** Create (works: whole-part CA commit publishes the ref). Rollback-remove (works:
  `removeRecursive` unlinks the ref; blobs become GC-eligible). The rollback **reload** path
  (`VersionMetadataOnDisk::removeTmpMetadataFile` → `getLastModified`) stats an in-flight part whose ref is
  not yet published → CA throws `FILE_DOESNT_EXIST` (**Probe gap #2**).

## 3. Design — four CA-side pieces {#design}

### 3.1 Decouple the transaction gate from append {#gate}
Add a narrow capability to `IMetadataStorage` — name it distinctly from the existing
`IStorage::supportsTransactions` to avoid confusion, e.g. `virtual bool supportsTransactionalMutableFiles()
const { return false; }`; `ContentAddressedMetadataStorage` overrides it to `true`. Change
`StorageMergeTree::supportTransaction` (`StorageMergeTree.cpp:167`) to also accept a disk whose metadata
storage `supportsTransactionalMutableFiles()`, rather than gating purely on `supportWritingWithAppend`. Rationale:
transactions provably do **not** need append (`txn_version.txt` is rewritten via tmp+`replaceFile`, never
`WriteMode::Append`; the only append user, `MergeTreeDeduplicationLog`, already has a no-append rewrite
fallback that CA's dedup window relies on). This leaves CA's append semantics — the dedup-log fallback and
the `DiskObjectStorageTransaction` `WriteMode::Append` guard — **untouched**. (Do **not** make CA's
`supportWritingWithAppend` return `true`: that would defeat the dedup-log fallback and disarm the append
guard, since CA's content-addressed write branch cannot append.)

### 3.2 Implement `ContentAddressedTransaction::replaceFile` {#replacefile}
Closes gap #1. `replaceFile(from, to)` for a mutable per-part-file destination: route it through the
existing mutable-file machinery (mirror `moveFile`, which already moves a recorded blob's bytes — or the
source's sidecar bytes — into `recorded_mutable[to]` and drops the source), plus overwrite-destination
semantics (a `replaceFile` may overwrite an existing `to`). A content-file destination keeps the existing
`moveFile`/blob behavior. The in-transaction `txn_version.txt.tmp → txn_version.txt` then folds into the
normal whole-part commit (the part's content blobs are in `recorded`, the mutable file in `recorded_mutable`).

### 3.3 The mutable-only commit branch (load-bearing) {#mutable-only}
Closes the post-commit standalone rewrite in (A). In `ContentAddressedTransaction::commit`, detect a
**mutable-only update of an already-committed part**: `recorded` (content blobs) is empty,
`recorded_mutable` is non-empty, and a ref already exists for `(table_uuid, part_name)`. In that case:
- Read the existing ref's `part_id` and manifest; **keep them** (do not recompute `part_id`, do not
  republish the ref to a new manifest).
- Merge the new mutable bytes into the existing sidecar; rewrite the sidecar bundle + the per-file mutable
  objects (`refMutableFileKey`) for the changed files only.
- Leave the ref pointing at the existing `part_id`.

This makes creation-CSN fill-in and removal-TID lock/unlock update only the part's sidecar, in place,
without touching content identity. (The existing whole-part INSERT path is unchanged: it has content in
`recorded`, so the mutable-only branch does not trigger.) Fail closed: if `recorded` is empty,
`recorded_mutable` is non-empty, but **no** ref exists for the part, that is a real error (a mutable rewrite
of a non-existent part) — throw, never publish an empty/standalone sidecar.

### 3.4 Harden the rollback-reload path {#rollback-reload}
Closes gap #2. `ContentAddressedMetadataStorage::getLastModified` (and any mutable-file stat/reload the
rollback path uses) on an in-flight part whose ref is not yet published must resolve via the open
transaction's in-flight state (the B59 read-your-writes overlay already exposes in-flight staged files) or
answer gracefully, rather than throwing `FILE_DOESNT_EXIST`. The rollback then completes and the part's
in-flight ref/blobs are removed as today.

## 4. What does NOT change {#unchanged}
The MVCC engine, `TransactionLog`, CSN/snapshot logic, and `VersionMetadata` (all storage-agnostic); every
non-CA disk; CA's content blobs / manifest / `computePartId` / GC reachability; and the visibility **reads**
(the sidecar already serves `txn_version.txt`). **Concurrency:** the engine serializes per-part
`txn_version.txt` writes under `DataPartsLock` before calling the disk, and CA's per-part sidecar objects do
not contend across parts — so no new CA-level locking is introduced. (The existing `gc_lock` in `commit`
still guards the blob re-validation; the mutable-only branch performs no blob re-validation since it adds no
blobs.)

## 5. Error handling {#errors}
Fail-closed throughout. The mutable-only branch (§3.3) requires a pre-existing ref or it throws — it never
fabricates a part. A rollback-reload miss (§3.4) resolves in-flight or surfaces the real error; it never
substitutes empty data. An uncommitted INSERT publishes a CA ref (so its blobs stay reachable, not
GC-reclaimed) and visibility is governed by `txn_version.txt`; `ROLLBACK` removes the ref. This matches the
existing precommitted-part model (a part is physically present but logically invisible until commit).

## 6. Testing {#testing}
Reproduction-driven, in **waves** (the post-commit removal-TID/CSN surface only appears once §3.2/§3.1 let
execution reach it):
- **Wave 1 (Tier 1 — transactional INSERT):** with §3.1–§3.4 in place, un-gate and run
  `01172_transaction_counters`, `01173_transaction_control_queries`, `02345_implicit_transaction`,
  `01133_begin_commit_race`. Fix until green. gtests: a CA transaction `replaceFile` of a mutable file lands
  in the sidecar (§3.2); a mutable-only commit on an existing ref rewrites the sidecar and leaves the
  manifest/`part_id`/ref intact (§3.3); a mutable-only commit with **no** existing ref throws (§3.3 fail-close).
- **Wave 2 (Tier 2 — mutations/DELETE-in-txn + isolation):** un-gate and run `01168_mutations_isolation`(+`_2`),
  `01169_alter_partition_isolation_stress`(+`_old`), `01170_alter_partition_isolation`,
  `01171_mv_select_insert_isolation_long`, `01174_select_insert_isolation`, `01167_isolation_hermitage`,
  `02421_truncate_isolation_no_merges`(+`_with_mutations`), `02435_rollback_cancelled_queries`,
  `03803_transaction_mutation_race`, `01172`/`03657_merge_tree_disk_support_transaction`,
  `04036_backup_partition_transaction_visibility`, `03752`/`03916` (attach-as-replicated txn metadata). Each
  new failure is a touchpoint in the same `txn_version.txt`-mutable family; fix and re-run. Re-gate with a
  precise reason anything that fails for an orthogonal cause (e.g. a topology the stateless server lacks, or
  a genuinely-replicated-only requirement) rather than a CA-transaction bug.
- **Inline-CA oracle:** a CA table; `BEGIN`; INSERT; a concurrent-snapshot SELECT does not see it; `COMMIT`;
  now visible; a second transaction `DELETE`/mutation then `ROLLBACK` leaves the data intact — proving
  creation-CSN, removal-TID lock, and rollback all work on CA.
- **Non-CA regression:** a couple of plain transaction tests on the default job → unchanged (every change is
  gated by `isContentAddressed`/`supportsTransactions`/the mutable-only-branch predicate).

## 7. Plan phasing {#phasing}
1. Gate decoupling (§3.1) + `replaceFile` (§3.2) + the mutable-only commit branch (§3.3) + rollback-reload
   hardening (§3.4); gtests; build.
2. Wave 1 (Tier 1) un-gate + fix until green; the inline-CA oracle.
3. Wave 2 (Tier 2) un-gate + reproduction-driven fixes for the deeper mutable-file touchpoints
   (removal-TID lock/unlock under mutations/DELETE, isolation stress); re-gate orthogonal failures with a
   reason.
4. Non-CA regression; backlog B39 → DONE (note any test re-gated and why); commit + push.

## 8. Risks {#risks}
- **The iterative tail.** Tier 2 will surface additional `txn_version.txt`-family touchpoints not seen by
  the probes (the probes aborted at gap #1). They are expected to be in the same mutable-sidecar family
  (§3.3 covers the mechanism), but the exact count is unknown until Wave 2 runs — hence the
  reproduction-driven phasing. No deeper (manifest/blob/`computePartId`/GC) problems were observed.
- **In-flight part visibility / rollback edges.** An uncommitted INSERT publishes a CA ref while the
  transaction is open; the rollback-reload and removal paths must treat such a part correctly (§3.4). The
  isolation stress tests (`01167`, `01169`) are the sharpest exercise of this.
- **Mutation-in-transaction.** A mutation creates new parts (whole-part commit) and removal-TID-locks the
  old ones (§3.3 post-commit rewrite); the mutation command file is a table-level file (already handled).
  Expected to compose, validated in Wave 2.
- **Re-gating honesty.** If a Tier 2 test fails for a genuinely-out-of-scope reason (replicated-only,
  topology), re-gate it with a precise reason rather than forcing it — consistent with the FETCH `03350`
  and FREEZE handling.
