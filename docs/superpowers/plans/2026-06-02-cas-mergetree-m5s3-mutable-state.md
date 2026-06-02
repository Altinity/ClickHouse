---
description: M5 Step 3 — split mutable per-part state (txn_version/metadata_version/uuid) out of the shared content-addressed manifest into per-ref state, overlaid on read.
sidebar_label: 'CAS MergeTree M5.3 plan'
sidebar_position: 6
slug: /superpowers/plans/cas-mergetree-m5s3
title: 'Content-Addressed MergeTree — M5 Step 3 Plan (split mutable per-ref state, B23)'
doc_type: 'guide'
---

# CAS MergeTree — M5 Step 3: split mutable per-ref state (B23) {#m5s3}

> **For agentic workers:** REQUIRED SUB-SKILL: `superpowers:subagent-driven-development`. Steps use `- [ ]`.

**Goal:** stop the content-addressed manifest from embedding per-part *mutable* files. Today `ContentAddressedTransaction::commit` writes ONE manifest (keyed by `part_id`) containing every recorded file, but `part_id` deliberately EXCLUDES `uuid.txt`, `txn_version.txt`, `metadata_version.txt`. So two logically-different parts with identical content (e.g. two identical INSERTs, or `assign_part_uuids=1`) map to the SAME `part_id` → the SAME manifest (`putIfAbsent` keeps the first writer's copy) → the second part reads the **first part's** `txn_version`/`uuid`. That is silent per-part-metadata corruption. After this step those files live **per-ref** and are overlaid on read; the manifest holds only shared, content-identical state.

**Architecture:** Define the fixed set of *mutable per-part files* = {`uuid.txt`, `txn_version.txt`, `metadata_version.txt`} (the exact set `PartManifest`/`PartId` already exclude — single source of truth). At `commit`, partition the recorded files: content-identical files → the manifest (as today); mutable files → a small **per-ref sidecar** object `store/<server>/<uuid>/refs/<part>.meta` (a versioned blob holding `{filename → bytes}`, written next to the ref). On read, `getStorageObjects`/`existsFile`/`getFileSize`/`listDirectory` resolve a mutable-file path from the sidecar (not the manifest→blob path). `removeRecursive` deletes the sidecar with the ref. These files are tiny, so storing their bytes inline in the sidecar (not content-addressed) is correct and keeps each part's copy private.

**Tech stack:** the consolidated `ContentAddressed/` units (`PartManifest`, `PoolPaths`, `ContentAddressedTransaction`, `ContentAddressedMetadataStorage`), `Identifiers.h`. No object-layout change to `blobs/`/`parts/`; adds one tiny per-ref `.meta` object.

**Source:** backlog B23 (review P1); design spec §3 ("mutable per-part state lives in the ref, never in the manifest").

## Build & test {#build}
`cmake --build build --target clickhouse unit_tests_dbms > build/cas_m5s3_build.log 2>&1`; `--gtest_filter='ContentAddressed*'` (41) + `'*PlainRewritable*:*DiskObjectStorage*'` (66). Stateless via the `clickhouse-praktika-tests` skill. Allman; `DB::Exception`; no `<...>` in `///`.

## File structure {#files}
- `PartManifest.h` — expose the canonical `isMutablePerPartFile(name)` / the excluded-set helper (single source shared with `PartId`).
- `PoolPaths.{h,cpp}` — add `refMetaKey(prefix, server, uuid, part) -> RefMetaObjectKey` (or reuse a typed key) = `store/<server>/<uuid>/refs/<part>.meta`.
- New `RefSidecar.{h,cpp}` (or fold into `PartManifest`) — versioned `{filename → bytes}` (de)serializer for the sidecar.
- `ContentAddressedTransaction.cpp` — `commit`: partition recorded files; write the sidecar; manifest excludes mutable files. `removeRecursive`: delete the sidecar.
- `ContentAddressedMetadataStorage.cpp` — read path: mutable-file branch resolves from the sidecar.

## Tasks {#tasks}

### Task 1 — canonical mutable-file predicate + sidecar format (TDD)
- [ ] Test: `isMutablePerPartFile("txn_version.txt")` true; `"a.bin"` false; the set EXACTLY matches what `computePartId` excludes (assert they're derived from one constant). Sidecar round-trips `{name→bytes}` with a version + bad-magic rejection.
- [ ] Implement the predicate (one shared constant in `PartManifest.h`) + `RefSidecar` (de)serialize (versioned, LE-friendly — coordinate with Step 4).
- [ ] Build → green; commit `CAS M5.3: mutable-per-part-file predicate + ref sidecar format`.

### Task 2 — commit writes mutable files to the sidecar, not the manifest (TDD)
- [ ] Test (real write path): build TWO parts via the transaction with **identical column content** but DIFFERENT `txn_version.txt` (and `uuid.txt`). Assert: (a) both refs resolve to the SAME `part_id`/manifest (dedup preserved); (b) each part has its OWN `.meta` sidecar; (c) `getStorageObjects(part_i/txn_version.txt)` returns part_i's distinct bytes. Before the change this FAILS (both read the first part's bytes) — confirm.
- [ ] Implement: in `commit`, split `recorded` into content files (→ manifest) and mutable files (→ sidecar at `refMetaKey`); publish the sidecar before/with the ref.
- [ ] Build → green; commit `CAS M5.3: commit splits mutable per-part files into a per-ref sidecar`.

### Task 3 — read path overlays the sidecar (TDD)
- [ ] Test: `existsFile`/`getFileSize`/`getStorageObjects`/`listDirectory` for a mutable-file path resolve from the sidecar (correct per-part bytes), and a missing sidecar entry behaves like a missing file (fail-close). Content files still resolve via manifest→blob.
- [ ] Implement the mutable-file branch in those read methods.
- [ ] Build → green; commit `CAS M5.3: read path overlays per-ref mutable files`.

### Task 4 — removal + stateless oracle
- [ ] `removeRecursive` deletes the `.meta` sidecar(s) with the ref(s); a GC unit test asserts the sidecar is gone after drop and is NOT a content-addressed blob (so it doesn't leak/confuse the sweep — sidecars are ref-scoped, removed synchronously).
- [ ] Stateless test (via `add-test`): `SETTINGS … assign_part_uuids=1`, two INSERTs of identical data into a CA-disk table and a normal table; assert per-part `uuid`/`_part`-level metadata differ correctly (oracle), `count`/`sum` match, DROP clean. Run in praktika; `[ OK ]`/`Failed: 0`.
- [ ] Commit `CAS M5.3: remove sidecars on drop + stateless mutable-state oracle`.

## Self-review {#self-review}
- **Coverage:** B23 — mutable files out of the manifest (Task 2), overlaid on read (Task 3), removed with the ref (Task 4); the collision test (Task 2) is the regression that pins it.
- **No layout regression:** `blobs/`/`parts/` unchanged; only a new tiny per-ref `.meta`. Dedup of content preserved (same `part_id`/manifest for identical content).
- **Consistency with later steps:** the sidecar format is versioned now so Step 4 (formal LE formats) just formalizes it; the ref payload itself is untouched here (sidecar is a separate object) — simpler than inlining into the ref, and leaves room for B1's `ReplicatedMergeTreePartHeader` in the ref later.

## Deferrals likely to surface {#deferrals}
- If a metadata-only `ALTER` (rewrites `metadata_version.txt`) is ever ungated (currently blocked by `supportsHardLinks=false`), it becomes a sidecar rewrite — note for when mutations land (B30).
- Sidecar GC: sidecars are ref-scoped and removed in `removeRecursive`; confirm they're never left orphaned (a crashed write leaves a sidecar with no ref → it's not content-addressed, so the reachability sweep won't catch it; add it to the sweep's ref-scoped cleanup or document).
