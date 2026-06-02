---
description: M1 Phase 3 — the content-addressed write path (build-local-then-upload, hash-on-finalize) + transaction commit (footer + ref).
sidebar_label: 'CAS MergeTree M3 plan'
sidebar_position: 3
slug: /superpowers/plans/cas-mergetree-m3
title: 'Content-Addressed MergeTree M1 — Phase 3 Plan (write path)'
doc_type: 'guide'
---

# Content-Addressed MergeTree — Phase 3 Plan (write path) {#cas-mergetree-m3-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development`. Steps use checkbox (`- [ ]`).

**Goal:** Implement the content-addressed **write** path so an INSERT/merge produces a part whose files become `blobs/<file_checksum>`, with a `parts/<part_id>` footer and a published `refs/<part>` — then read it back via the Phase-2 resolution.

**Architecture:** Because the object-storage transaction assigns the object key *before* the content exists and a file's checksum is only known at *finalize* (and S3 has no rename), the content-addressed disk uses a **content-addressed write buffer**: it spills bytes to a local temp file while computing the content hash, and on finalize uploads once to `blobs/<hash>` via `putIfAbsent`. The `ContentAddressedTransaction` accumulates `(logical_file → blob_hash, size)`, and at `commit` computes `part_id` (rollup of the deterministic checksum subset), writes the `parts/<part_id>` footer (Phase-1 `Footer`), and publishes the ref. Mutation carry-forward (`createHardLinkFrom`) maps the new footer entry at the source blob hash with no re-upload.

**Tech Stack:** C++ (`src/Disks/DiskObjectStorage/…`), `IMetadataTransaction`, `IObjectStorage::writeObject`/`putIfAbsent`-equivalent, `HashingWriteBuffer`/`cityHash128`, Phase-1 `Footer`/`PoolPaths`. Reuses Phase-2 `ContentAddressedMetadataStorage`.

**Source spec:** `docs/superpowers/specs/content_addressed_shared_mergetree_design.md` + `…/2026-06-02-cas-mergetree-integration-design.md`. **Deferred (do NOT implement):** backlog B1–B21. Phases 1–2 are committed + pushed on `cas-mergetree-poc`.

---

## The write-path challenge (from Phase-3 discovery — read before planning) {#challenge}

Confirmed by reading the tree:
- `MergedBlockOutputStream::finalizePartOnDisk` writes each file via `IDataPartStorage::writeFile` → `DiskObjectStorage::writeFile` → `DiskObjectStorageTransaction::writeFileImpl`. There, the **object key is generated up front** (`metadata_transaction->generateObjectKeyForPath(path)`, `DiskObjectStorageTransaction.cpp:270`) and the bytes are streamed straight to `object_storage->writeObject` during the write; metadata is recorded in a finalize **callback** (`:332-366`) and persisted at `commit`.
- Each file's `cityHash128` is computed by `HashingWriteBuffer` and is available in `checksums.files[name].file_hash` (`MergedBlockOutputStream.cpp:298-300`); `checksums.txt` is written last; `uuid.txt`/`metadata_version.txt` are written before it (exclude from `part_id`).
- `getTotalChecksumUInt128()` (`MergeTreeDataPartChecksum.cpp:318`) rolls up `(name, file_hash)` over the `files` map (deterministic) — the basis for `part_id`.
- There is **no local-staging step today** — writes go direct to remote.

**Decision:** introduce a content-addressed write buffer; do NOT try to rename/copy on S3 after the fact. **Task 0 confirms the cleanest hook** (a capability flag on the metadata storage that makes `DiskObjectStorageTransaction` use the CA buffer + defer key assignment to finalize, vs. a `content_addressed`-specific write path) before committing to the integration point — this is the one real architectural choice in Phase 3.

## Build & test {#build}
`build/` configured; build in BACKGROUND (`cmake --build build --target unit_tests_dbms > build/cas_build.log 2>&1`; then `tail`/`grep error:`). Run `build/src/unit_tests_dbms --gtest_filter='ContentAddressed*'`. New `.cpp` in the registered `…/ContentAddressed/` dir auto-globbed; gtests auto-globbed. No `<...>` in `///` comments.

## File structure {#file-structure}
```
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/
  ContentAddressedWriteBuffer.h / .cpp   # NEW: spill-to-local + hash + putIfAbsent(blobs/<hash>) on finalize
  ContentAddressedTransaction.{h,cpp}    # MODIFY: accumulate file->blob, compute part_id, write footer+ref at commit
  ContentAddressedMetadataStorage.{h,cpp}# MODIFY: writeFile entry + a capability flag (per Task 0)
src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp  # MODIFY (only if Task 0 picks the gated-generic-transaction hook)
src/Storages/MergeTree/ContentAddressedPartId.h / .cpp        # NEW: part_id rollup over the deterministic subset
src/Disks/tests/gtest_content_addressed_metadata.cpp          # MODIFY: write->read round-trip + carry-forward + dedup tests
```

---

## Task 0 (DISCOVERY/decision, no commit): pick the write hook {#task-0}
- [ ] Read `DiskObjectStorageTransaction::writeFileImpl` (`:254-369`) end-to-end. Decide between **(A)** a capability the metadata storage exposes (e.g. `bool blobKeysAreContentAddressed()`) that makes `writeFileImpl` (i) wrap the write in the CA buffer and (ii) set the `StoredObject.remote_path` from the buffer's content-hash in the finalize callback *before* `createMetadataFile`; vs **(B)** routing `content_addressed` writes through a transaction-owned path that bypasses `writeFileImpl`'s up-front keying. Prefer the smaller, less-invasive change to the *generic* transaction; document the choice + the exact lines to touch as a comment block in `ContentAddressedWriteBuffer.h`. No commit.

## Task 1: `ContentAddressedWriteBuffer` (spill + hash + upload-on-finalize) — TDD {#task-1}
- [ ] Test (append to gtest): write bytes through the buffer over a `LocalObjectStorage`; on finalize assert (a) an object exists at `blobKey(hash)` with the exact bytes, (b) the buffer reports that `blob_hash`, (c) writing identical bytes again is idempotent (`putIfAbsent` → no duplicate object, same hash). (Test the buffer in isolation — construct it directly with the object storage + a temp dir.)
- [ ] Implement: a `WriteBufferFromFileBase` subclass that writes to a local temp file via `HashingWriteBuffer` (reuse `cityHash128`); on `finalizeImpl()` compute the hex hash, `putIfAbsent`-upload the temp file to `ContentAddressed::blobKey(hash)` (skip if the object already exists — read-after-write strong consistency), expose `getBlobHash()`/`getSize()`, delete the temp. Spill to a per-write temp under the local scratch volume. Allman; `DB::Exception`.
- [ ] Build → green; commit `"CAS M1 P3: content-addressed write buffer (spill+hash+upload-on-finalize)"`.

## Task 2: `ContentAddressedTransaction` — accumulate, part_id, footer + ref at commit {#task-2}
- [ ] Test: drive the transaction directly — `writeFile` two columns + the small files, then `commit`; assert (a) each distinct content uploaded once (dedup via `putIfAbsent`), (b) a `parts/<part_id>` footer exists listing the files, (c) a `refs/<part>` exists pointing at `part_id`, (d) Phase-2 `getStorageObjects`/`readObject` round-trips the bytes. Add a carry-forward test: a second part that `createHardLinkFrom`s an unchanged column reuses its blob hash with NO new object.
- [ ] Implement: `writeFile(path, …)` returns a `ContentAddressedWriteBuffer`; the transaction records `(logical_file → {blob_hash, size})` as buffers finalize. `createHardLinkFrom(from, to)` copies the source footer entry's blob hash to `to` (carry-forward; no upload). `part_id` = `ContentAddressedPartId::compute(checksums-of-files-written)` — a rollup over the deterministic subset (exclude `uuid.txt`/`txn_version.txt`/`metadata_version.txt`). `commit`: build the `Footer`, `putIfAbsent(partKey(part_id), footer.serialize())`, then publish `refKey(server_id, table_uuid, part_name)` = `part_id` (+ the `ReplicatedMergeTreePartHeader` field reserved for B1). Create `ContentAddressedPartId.{h,cpp}` in `src/Storages/MergeTree/` (it needs `MergeTreeDataPartChecksums`).
- [ ] Build → green; commit `"CAS M1 P3: transaction commit (part_id + footer + ref); carry-forward"`.

## Task 3: wire the write path (per Task 0's decision) {#task-3}
- [ ] Implement the chosen hook so `MergedBlockOutputStream`'s `IDataPartStorage::writeFile` ends up using the CA buffer for a `content_addressed` disk and the part's footer/ref get published at `commitTransaction()`. Replace the Phase-2 `throwNotImplemented` transaction-write stubs with the real path. Keep the change to the generic `DiskObjectStorageTransaction` minimal and gated on the capability flag (no behavior change for other metadata types).
- [ ] Test: a higher-level write through `DiskObjectStorage::writeFile` (not just the transaction) for a `content_addressed` disk produces the blob/footer/ref and reads back.
- [ ] Build → green; commit `"CAS M1 P3: route content_addressed writes through the CA buffer"`.

## Task 4: end-to-end write→read round-trip (the Phase-3 oracle) {#task-4}
- [ ] Test: simulate a part as a set of `{logical_file → bytes}`; write it through the metadata storage + transaction (commit); then, using ONLY the Phase-2 read API (`iterateDirectory`/`getStorageObjects`/read), reconstruct the files and assert byte-equality. Add: two parts sharing an identical column → one blob; a mutation (carry-forward) → only the changed column is a new blob. (This is the integration oracle mirroring the PoC's write+read+dedup scenarios.)
- [ ] Build → green; full `--gtest_filter='ContentAddressed*'` all pass; commit `"CAS M1 P3: write->read round-trip + dedup/carry-forward oracle"`.

---

## Self-review {#self-review}
- **Spec coverage:** §5 INSERT (build-local-then-upload, hash-on-finalize, footer+ref at commit), §1/§4 mutation carry-forward by reference, §2 `part_id` from existing checksums excluding the non-deterministic subset. Removal/GC stay Phase 4; the write buffer's local-spill is the §5 "build local then upload."
- **Placeholder scan:** Task 0 is an explicit, no-commit design decision feeding the hook (the one genuine architectural choice, with the exact lines identified before code). Every other task has a test + concrete implementation steps; the buffer/transaction bodies depend on Task 0's hook choice and are written against the live `DiskObjectStorageTransaction` (this is integration glue — the implementer writes the body against the confirmed interface, as in Phase 2).
- **Type consistency:** reuses `Footer`/`BlobEntry`, `PoolPaths` (`blobKey`/`partKey`/`refKey`), `ContentAddressedMetadataStorage`/`ContentAddressedTransaction`; new `ContentAddressedWriteBuffer` (`getBlobHash`/`getSize`) and `ContentAddressedPartId::compute` are used consistently across Tasks 1–4.

## New deferrals likely to surface {#deferrals}
- Empty-blob files (size 0): decide whether to store a zero-byte blob or special-case (note for B-list).
- Local scratch sizing / spill location config (relates to the spec's "build-local-then-upload" note) — append to backlog if it needs a setting.
- The B1 `ReplicatedMergeTreePartHeader` field in the ref is written but unused until replication.

## Execution {#execution}
Task 0 → Task 4 via `superpowers:subagent-driven-development`. After Phase 3 (a part can be written and read content-addressed), run `writing-plans` for Phase 4 (deferred GC wiring: feed `BlobRefIndex` deltas from commit/remove, the background sweeper, `remove` = drop ref). Append new deferrals to the backlog with plug-in points.
