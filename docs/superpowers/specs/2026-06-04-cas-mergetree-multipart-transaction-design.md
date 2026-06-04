---
description: Design spec for B67 layer 2 — generalize ContentAddressedTransaction from single-part to per-part, so a transactional merge/mutation (whose one disk transaction spans the merge-output part plus the covered source parts' txn_version rewrites) works on a content-addressed disk.
sidebar_label: 'CAS MergeTree Multi-Part Transaction'
sidebar_position: 10
slug: /superpowers/specs/cas-mergetree-multipart-transaction
title: 'Content-Addressed MergeTree — Multi-Part Disk Transaction (B67 layer 2)'
doc_type: 'guide'
---

# Content-Addressed MergeTree — Multi-Part Disk Transaction (B67 layer 2) {#cas-multipart-transaction}

**Status:** design spec, awaiting review. **Date:** 2026-06-04. **Backlog:** B67 layer 2 (layer 1 — the
`mutation_<n>.txt` `WriteMode::Append` — is DONE, commit `ad136563cfe`). Builds on the Tier-1 transactions
milestone (B39).

## 1. Goal and scope {#goal}

Let a single `ContentAddressedTransaction` hold and commit writes for **more than one part**, so transactional
merges and mutations work on a content-addressed (CA) disk. This is the remaining blocker for the
mutation/isolation Tier-2 transaction tests.

**In scope:** generalize `ContentAddressedTransaction` from a single `(table_uuid, part_name)` to a per-part
staging map; route every staging op to the right part; handle the deferred `tmp_merge → final` rename re-key;
multi-part `commit`/`rollback`; un-gate the Tier-2 mutation/isolation tests (reproduction-driven); gtests.
**Out of scope:** the MVCC engine (unchanged); non-CA disks; any new cross-part *atomicity* guarantee beyond
what a local disk already provides (see §3.0); the replicated/attach-as-replicated tests that may be
orthogonal (`03752`, `03916` — triage separately).

## 2. The mechanism (verified) {#mechanism}

A transactional `OPTIMIZE`/mutation/`DELETE` runs a background merge committed through
`MergeTreeData::Transaction`. Two things make its **one** `DiskObjectStorageTransaction` span multiple parts:

- **Deferred rename window.** `preparePartForCommit` with `rename_in_transaction=true` (`MergeTreeData.cpp:5354`)
  adds the merge-output part with `need_rename` and DEFERS the `tmp_merge_X → X` rename to
  `Transaction::renameParts()`. In the window between add and rename, the part's logical name is the final
  `X` while the disk transaction is still keyed to `tmp_merge_X`. A `txn_version.txt`/CSN write that lands in
  that window calls `rememberTarget(X)` while the transaction holds `tmp_merge_X` → today's single-part
  assertion (`ContentAddressedTransaction::rememberTarget`, `…/ContentAddressedTransaction.cpp:166`) aborts
  with a `LOGICAL_ERROR`, and on rollback the server aborts.
- **Covered source parts.** On commit, `addNewPartAndRemoveCovered` → `lockRemovalTID` rewrites
  `txn_version.txt` on each covered SOURCE part (so a rollback can un-cover them). These rewrites can land on
  the same transaction as the merge output.

Confirmed on a clean binary (after the layer-1 append fix removed the earlier masking crash): the conflict is
`tmp_merge_0_4_106_8` vs `0_4_106_8`, preceded by a source-part `txn_version.txt.tmp` write. The exact set of
parts that share one transaction per scenario is pinned reproduction-driven in the plan's first step.

## 3. Design — per-part staging {#design}

### 3.0 Atomicity is not a new requirement {#atomicity}
On a local disk a transactional merge is **not** atomic at the filesystem level — it renames the output part
and rewrites each source `txn_version.txt` as individual ops. MVCC visibility is gated by the per-part
**CSN/TID in `txn_version.txt`**, not by disk-op atomicity; crash recovery replays the transaction log and GC
reclaims orphans. So CA's multi-part `commit` may publish parts **one at a time** — it only needs to *hold and
apply* every staged part in its single `commit()`. No cross-part atomic flip is introduced.

### 3.1 The per-part staging map {#map}
Replace the transaction's single `(table_uuid, part_name)` + `recorded` / `recorded_mutable` /
`recorded_mutable_removed` with a map keyed by part:
```
struct PartStaging {
    std::map<std::string, BlobEntry> recorded;            // content blobs (-> manifest)
    std::map<std::string, std::string> recorded_mutable;  // mutable per-part files (-> sidecar)
    std::set<std::string> recorded_mutable_removed;        // mutable files to delete from a committed sidecar
    std::string frozen_backup_name;                        // FREEZE target (preserved per-part)
    std::string frozen_table_dir;
};
std::map<std::pair<std::string/*table_uuid*/, std::string/*part_name*/>, PartStaging> parts;
```
The single-part case is exactly one entry — the overwhelmingly common path (every INSERT, every non-merge
write). `table_uuid` is shared in practice but keyed per-part for safety.

### 3.2 Routing {#routing}
`rememberTarget`/`recordBlob`/`writeFile`/`unlinkFile`/the in-flight read helpers parse the path
(`parsePartFilePath`) and route to `parts[{table_uuid, part_name}]` (creating the entry on first touch).
The "single transaction must write one part" assertion is **removed** — multiple keys are now legal. (The
per-file consistency within one part is still enforced.)

### 3.3 The rename re-key {#rekey}
`moveDirectory(tmp_merge_X → X)` (the deferred merge rename) and the existing detached/active re-key paths
move the staging entry from the source part key to the destination key, **merging** into any existing
destination entry (e.g. a `txn_version.txt` write that already arrived under the final name in the deferred
window). This closes the window cleanly: after the re-key, part `X` carries both the merged content blobs and
the mutable file.

### 3.4 Multi-part commit / rollback {#commit}
`commit()` takes the per-pool `gc_lock` once, then iterates `parts`:
- An entry with content blobs (`recorded` non-empty) → the normal whole-part publish (manifest + sidecar +
  ref), exactly as the current single-part commit, with the B49 blob re-validation under the held lock.
- An entry that is mutable-only on an already-committed ref (`recorded` empty, `recorded_mutable`/`_removed`
  non-empty) → the B39 mutable-only branch (sidecar-in-place, keep manifest/`part_id`/ref).
- Session pins / `persistSession` cover the union of all parts' freshly-written blobs.
`rollback`/abort applies per-part the same way (the precommitted parts' `setAndStoreCreationCSN(RolledBackCSN)`
are mutable-only updates). A crash mid-`commit` leaves some refs published and some not — recovered by the
MVCC transaction log + GC of orphan refs, identical to the local-disk story (§3.0).

### 3.5 What does NOT change {#unchanged}
The single-part fast path (one map entry) is byte-for-byte equivalent to today. The MVCC engine,
`VersionMetadata`, every non-CA disk, the manifest/blob/`computePartId`/sidecar mechanics (reused per-part),
FREEZE (`frozen_*` now per-part), and layer-1 append are unchanged.

## 4. Error handling {#errors}
Fail-closed per part: a mutable-only entry with no existing ref still throws (no fabricated part); a
content entry re-validates its blobs under `gc_lock` before publishing. Partial-commit on crash is not a new
hazard — orphan refs are GC-reachable-then-reclaimed, and MVCC governs visibility by CSN (§3.0). No silent
fallback.

## 5. Testing {#testing}
Reproduction-driven:
- **gtest:** a single transaction that stages TWO content parts + one mutable-only co-part publishes all three
  correctly (each ref/manifest/sidecar resolvable); a `moveDirectory(tmp_merge_X → X)` re-key merges a
  pre-existing `txn_version.txt` write under `X`; the single-part path is unchanged (existing 120 gtests stay
  green).
- **Stateless (un-gate, in a wave):** the B67-gated mutation/isolation tests — `01168_mutations_isolation`,
  `01168_mutations_isolation_2`, `01174_select_insert_isolation`, `01167_isolation_hermitage`,
  `01169_alter_partition_isolation_stress`(+`_old`), `01170_alter_partition_isolation`,
  `01171_mv_select_insert_isolation_long`, `02421_truncate_isolation_no_merges`(+`_with_mutations`),
  `02435_rollback_cancelled_queries`, `03803_transaction_mutation_race`, `04036_backup_partition_transaction_visibility`
  — run on the CA-default job; fix reproduction-driven; re-gate any genuinely orthogonal failure (e.g. the
  `03752`/`03916` attach-as-replicated pair) with a precise reason.
- **Regression:** Tier-1 txn tests + the `05004` oracle still pass on CA; a CA non-transaction smoke set
  (insert/select, projections, non-txn mutations, detach, fetch, freeze) unchanged (the per-part refactor
  touches every CA write path — this is the key regression gate); a couple of plain (non-CA) transaction
  tests unchanged.

## 6. Plan phasing {#phasing}
1. **Pin + refactor.** First, a focused reproduction step: confirm exactly which parts share one transaction
   per scenario (the merge-output tmp/final pair + which source parts). Then the per-part-map refactor
   (§3.1–3.4) + gtests + the single-part regression (the 120 `ContentAddressed*` gtests + a CA non-txn smoke)
   — the refactor must not regress the single-part path before any Tier-2 test is touched.
2. **Tier-2 wave.** Un-gate the mutation/isolation tests; reproduction-driven fixes; re-gate orthogonal ones.
3. **Finalize.** Full regression (Tier-1, CA non-txn smoke, plain non-CA); backlog B67 layer 2 → DONE (or note
   any residual); commit + push.

## 7. Risks {#risks}
- **The refactor touches the transaction core.** `recorded`/`recorded_mutable`/`part_name` are referenced
  throughout `ContentAddressedTransaction` (commit, the FREEZE shadow path, detached re-key, projections,
  in-flight reads, relink). Every reference must move to the per-part entry without changing single-part
  behavior. Mitigation: keep a single-entry fast path semantically identical; the 120 gtests + CA non-txn
  smoke are the regression gate run BEFORE any Tier-2 un-gate.
- **Reproduction-driven tail.** The Tier-2 isolation/stress tests may surface further per-part touchpoints
  (rollback ordering, concurrent snapshot reads). Bounded to the same per-part mechanism; phased.
- **Exact co-part set unconfirmed.** Whether a single transaction ever stages two *new content* parts (vs one
  content + N mutable-only) is pinned in phase-1 step 1; the full per-part map handles either way, but the
  gtest should reflect the real shape.
- **`03752`/`03916`/`04036`** may be orthogonal (attach-as-replicated / backup visibility) rather than pure
  layer 2 — triage on their actual error, re-gate with reason if so.
