---
description: Design spec for B59 — let a content-addressed part-build transaction read its own in-flight staged files, so the projection spill-and-merge (mutation/MATERIALIZE and merge) works on a CA disk.
sidebar_label: 'CAS MergeTree projection read-back'
sidebar_position: 6
slug: /superpowers/specs/cas-mergetree-projection-readback
title: 'Content-Addressed MergeTree — In-Flight Read-Your-Writes (B59)'
doc_type: 'guide'
---

# Content-Addressed MergeTree — In-Flight Read-Your-Writes (B59) {#cas-projection-readback}

**Status:** design spec, awaiting review. **Date:** 2026-06-04. **Backlog:** B59 (the bug), B30 (the
whole-part-commit-contract fragility this is an instance of), B5/B58 (projections on CA, the prior work).

## 1. Goal and scope {#goal}

Make a content-addressed (CA) part-build transaction able to **read files it has staged but not yet
committed**, so the projection **spill-and-merge** flow works on a CA disk. This closes B59 and the
latent same-class bug on the merge path, completing projections on CA.

**In scope:** the read-your-writes overlay (Approach A) + un-gating the 7 re-gated projection tests +
multi-block projection oracles (mutate and merge).
**Out of scope:** any change to how projections are built/split (the spill-and-merge stays); the
broader B30 contract refactor (this is the minimal targeted instance).

## 2. The bug (verified) {#bug}

A mutation, `ALTER … MATERIALIZE PROJECTION`, or `ALTER MODIFY COLUMN` that rebuilds a projection — and
**equally a background merge** — builds each projection in **per-chunk temp blocks**
`<part>/<proj>_<N>.tmp_proj` (`MutateTask::PartMergerWriter::writeTempProjectionPart` /
`MergeTreeDataWriter::writeTempProjectionPart`), then `MergeProjectionPartsTask` spawns a sub-`MergeTask`
(with `NO_TRANSACTION_PTR`) that **reads those temp blocks back** and merges them into the final
`<proj>.proj`.

On CA the temp blocks share the parent part's **in-flight** whole-part `ContentAddressedTransaction`
(`IMergeTreeDataPart::getProjectionPartBuilder` sets `use_parent_transaction=true` for CA, B58): their
blobs are **uploaded** to `blobs/<hash>` and recorded in the transaction's `recorded` map, but **no
ref/manifest is committed yet**. The read-back goes `DataPartStorageOnDiskFull::{getStorageObjects,
readFile,readFileIfExists,prepareRead}` → `volume->getDisk()->…(full_path)` →
`ContentAddressedMetadataStorage::{getStorageObjects,resolveBlobEntry}` → `readRefPartId`
(**committed-only**) → no ref → `FILE_DOESNT_EXIST`.

**Key insight (scope):** this is **not** mutation-specific. The merge path uses the identical
temp-block → `MergeProjectionPartsTask` read-back, so it is latently broken too — B58 only made the
**single-block** case work (one temp block, no read-back, rekeyed straight into the parent manifest).
The passing merge tests used small/single-block projections. So the fix belongs at the **shared read
seam**, fixing multi-block projections for both merge and mutate.

The root cause is an architectural mismatch (B30): the CA model is "write all of a part → commit once →
then readable," but the projection spill-and-merge does **read-your-own-uncommitted-writes** mid-build.

## 3. Design — Approach A: in-flight read overlay {#design}

Give the part-build transaction read-your-writes, and have the part storage (which already **holds** the
transaction) consult it on read before the committed path.

### 3.1 Transaction in-flight resolve {#resolve}
- `IMetadataTransaction` gains `std::optional<StoredObjects> tryGetInFlightStorageObjects(const std::string & path)`,
  default `{}` (nullopt). `ContentAddressedTransaction` overrides it: parse `path` (the existing
  `parsePartFilePath` — the transaction already keys `recorded` by the part-relative file), look up the
  in-flight `recorded` entry, and return the **already-uploaded** blob as a `StoredObject`
  (`blobKey(hash)` projected to the full object key). A mutable per-part file staged in
  `recorded_mutable` resolves to its sidecar bytes path the same way (or is served inline). Miss →
  nullopt.
- `IDiskTransaction` / `DiskObjectStorageTransaction` gains a forwarder of the same shape that delegates
  to its metadata transaction; non-object-storage transactions default to nullopt.

### 3.2 Part storage consults the transaction {#part-storage}
`DataPartStorageOnDiskFull`'s read-ish methods — `getStorageObjects`, `readFile`, `readFileIfExists`,
`prepareRead`, and `existsFile`/`getFileSize` (the merge stats the temp files) — gain a guarded prelude:

```
if (transaction)
    if (auto inflight = transaction->tryGetInFlightStorageObjects(<full part path of name>))
        return <serve from *inflight> ;   // read the already-uploaded blob object(s)
// else: unchanged — volume->getDisk()->…(full_path)
```

- The gate is `transaction != nullptr`. A **committed** part being read has no open transaction, so for
  ~all reads this is a single null-check — no behavior change off the build path.
- Serving the blob from `StoredObjects`: read via the disk's object storage (`readObjects`/the existing
  object-read used after `getStorageObjects`). If no public helper exists at this layer, the plan adds a
  small one (`DataPartStorageOnDiskBase`-level "read these StoredObjects").
- `getStorageObjects` returning the in-flight objects is the primary override; `readFile`/`prepareRead`
  either route through `getStorageObjects` or get the same guard; `existsFile`/`getFileSize` answer
  true/size from the in-flight entry.

### 3.3 What does NOT change {#unchanged}
The projection spill-and-merge, `getProjectionPartBuilder`'s `use_parent_transaction` choice, the
`.tmp_proj`→`.proj` rekey into the parent manifest at the single parent commit (B58), and every non-CA
disk's behavior. No new namespace, no early sub-commit, no extra copies, no ref-key collisions.

## 4. Error handling {#errors}

Fail-closed is preserved. A miss falls through to the committed read path, which throws
`FILE_DOESNT_EXIST`/`CORRUPTED_DATA` exactly as today — the overlay never substitutes an empty result.
The in-flight blob is addressed by its recorded hash; the blob is uploaded **before** it is recorded, so
a hit always points at a present object — if the object read nonetheless fails, it fails loudly (no
silent fallback). The overlay only ever *adds* visibility to files this very transaction staged; it
cannot expose another part's data (the `recorded` map is per-transaction, per-part).

## 5. Testing {#testing}

- **Unit (gtest):** a CA transaction records a part file (uploads the blob, no commit) → `tryGet…InFlight`
  resolves it and the bytes read back equal what was written, BEFORE any commit; a second file not
  recorded → nullopt; a non-CA transaction → nullopt. (Metadata-layer proof of read-your-writes.)
- **Stateless (inline CA disk, deterministic multi-block):** force >1 temp projection block (enough rows
  + small `min_insert_block_size`/`max_block_size`) and: (a) a **mutation** that rebuilds the projection
  (`ALTER MODIFY COLUMN`/`MATERIALIZE PROJECTION`), (b) a **merge** (`OPTIMIZE FINAL`) of multi-block
  projection parts — each followed by a projection-served query (`force_optimize_projection=1`) matching
  a base-table oracle, plus `DETACH`/`ATTACH` to prove durability. This covers the merge case the
  existing tests missed.
- **Regression:** un-gate the 7 B59 tests (`02371_select_projection_normal_agg`,
  `01710_projection_vertical_merges`, `02920_alter_column_of_projections`,
  `02941_projections_external_aggregation`, `03401_normal_projection_with_part_offset`,
  `03401_normal_projection_with_part_offset_no_sorting`, `03464_projections_with_subcolumns`) on the
  CA-default job → expect pass. Run a couple of normal projection merge/mutation tests on the default
  (non-CA) job → no regression.

## 6. Risks {#risks}

1. **Touching `DataPartStorageOnDiskFull`'s generic read methods** — mitigated by the `transaction !=
   nullptr` gate; verify (the regression run) that committed-part reads are byte-for-byte unchanged.
2. **Reading a `StoredObject` into a `ReadBuffer` from the part-storage layer** — may need a small
   disk/part-storage helper if none is public; a plan detail.
3. **Path-key alignment** — the transaction keys `recorded` by the parent-part-relative path while the
   temp-block storage reads via its own full path; the CA transaction's existing `parsePartFilePath`
   handles the mapping, and the gtest pins it.
4. **Mutable per-part files in-flight** (`metadata_version.txt` etc., in `recorded_mutable`) — the
   overlay must resolve these from the inline staged bytes, not the manifest; covered by §3.1.

## 7. Plan phasing {#phasing}

1. Transaction in-flight resolve (`tryGetInFlightStorageObjects` on `IMetadataTransaction` +
   `ContentAddressedTransaction` + the `DiskObjectStorageTransaction` forwarder) + a metadata-layer gtest.
2. `DataPartStorageOnDiskFull` read-overlay (guarded prelude on the read methods) + the StoredObject-read
   helper if needed.
3. Multi-block oracles (inline CA disk: mutation + merge) + un-gate the 7 tests; CA-default run.
4. Non-CA regression check; backlog B59 → DONE; commit + push.
